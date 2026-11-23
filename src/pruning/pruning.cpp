#include "pruning.h"
#include "netlist_transform.h"
#include "verification/wce_sat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace als {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
struct PlannerScore { std::uint64_t fanout_adp = 0, output_depth_sum = 0; };
struct Candidate {
    Replacement replacement;
    Interval local, projected;
    std::size_t order;
    PlannerScore score;
};
class PlannerScoreWorkspace {
public:
    explicit PlannerScoreWorkspace(const AigNetlist& net)
        : net_(net),
          base_fanin0_(net.nodes.size(), 0),
          base_fanin1_(net.nodes.size(), 0),
          base_live_and_(net.nodes.size(), 0),
          fanin0_(net.nodes.size(), 0),
          fanin1_(net.nodes.size(), 0),
          live_and_(net.nodes.size(), 0),
          output_position_(net.nodes.size(), -1),
          fanouts_(net.nodes.size(), 0),
          levels_(net.nodes.size(), 0),
          arrivals_(net.nodes.size(), 0) {
        for (std::size_t i = 0; i < net.nodes.size(); ++i) {
            base_fanin0_[i] = net.nodes[i].fanin0;
            base_fanin1_[i] = net.nodes[i].fanin1;
            base_live_and_[i] = net.nodes[i].type == NodeType::AND ? 1 : 0;
        }
        base_outputs_.reserve(net.outputs.size());
        for (std::size_t i = 0; i < net.outputs.size(); ++i) {
            const int output = net.outputs[i];
            base_outputs_.push_back(net.nodes[output].fanin0);
            output_position_[output] = static_cast<int>(i);
        }
        output_literals_.resize(base_outputs_.size());
        base_gate_count_ = count_and_gates(net);

        const std::size_t max_edges =
            static_cast<std::size_t>(2 * base_gate_count_) + net.outputs.size();
        fanout_penalties_.resize(max_edges + 1, 0);
        for (std::size_t fanout = 2; fanout <= max_edges; ++fanout) {
            int penalty = 0;
            for (std::size_t bound = 1; bound < fanout; bound <<= 1)
                ++penalty;
            fanout_penalties_[fanout] = penalty;
        }
    }

    PlannerScore score(const MutationPlan& plan) {
        const Evaluation evaluation = evaluate(&plan);
        return {static_cast<std::uint64_t>(evaluation.gates) *
                    static_cast<std::uint64_t>(evaluation.fanout_depth),
                evaluation.output_depth_sum};
    }

private:
    struct Evaluation {
        int gates = 0;
        int depth = 0;
        int fanout_depth = 0;
        std::uint64_t output_depth_sum = 0;
    };

    int penalty(int fanout) const {
        if (fanout < 0 ||
            static_cast<std::size_t>(fanout) >= fanout_penalties_.size())
            throw std::logic_error("planner fanout exceeds edge bound");
        return fanout_penalties_[static_cast<std::size_t>(fanout)];
    }

    Evaluation evaluate(const MutationPlan* plan) {
        std::copy(base_fanin0_.begin(), base_fanin0_.end(), fanin0_.begin());
        std::copy(base_fanin1_.begin(), base_fanin1_.end(), fanin1_.begin());
        std::copy(base_live_and_.begin(), base_live_and_.end(), live_and_.begin());
        std::copy(base_outputs_.begin(), base_outputs_.end(),
                  output_literals_.begin());
        if (plan) {
            for (const NodeMutation& edit : plan->nodes) {
                fanin0_[edit.node_id] = edit.fanin0;
                fanin1_[edit.node_id] = edit.fanin1;
                live_and_[edit.node_id] = edit.tombstone ? 0 : 1;
            }
            for (const OutputMutation& edit : plan->outputs) {
                const int position = output_position_.at(edit.output_id);
                if (position < 0)
                    throw std::logic_error("planner output mutation is unknown");
                output_literals_[static_cast<std::size_t>(position)] =
                    edit.literal;
            }
        }

        std::fill(fanouts_.begin(), fanouts_.end(), 0);
        std::fill(levels_.begin(), levels_.end(), 0);
        std::fill(arrivals_.begin(), arrivals_.end(), 0);

        for (int gate = 1; gate <= net_.max_var; ++gate) {
            if (!live_and_[gate]) continue;
            ++fanouts_[lit_to_var(fanin0_[gate])];
            ++fanouts_[lit_to_var(fanin1_[gate])];
        }
        for (int literal : output_literals_)
            ++fanouts_[lit_to_var(literal)];

        Evaluation result;
        result.gates = base_gate_count_ -
            (plan ? plan->removal.total() : 0);
        for (int gate = 1; gate <= net_.max_var; ++gate) {
            if (!live_and_[gate]) continue;
            const int input0 = lit_to_var(fanin0_[gate]);
            const int input1 = lit_to_var(fanin1_[gate]);
            levels_[gate] = 1 + std::max(levels_[input0], levels_[input1]);
            arrivals_[gate] = 1 + std::max(
                arrivals_[input0] + penalty(fanouts_[input0]),
                arrivals_[input1] + penalty(fanouts_[input1]));
        }
        for (int literal : output_literals_) {
            const int driver = lit_to_var(literal);
            const int level = levels_[driver];
            result.depth = std::max(result.depth, level);
            result.output_depth_sum += static_cast<std::uint64_t>(level);
            result.fanout_depth = std::max(
                result.fanout_depth,
                arrivals_[driver] + penalty(fanouts_[driver]));
        }
        return result;
    }

    const AigNetlist& net_;
    int base_gate_count_ = 0;
    std::vector<int> base_fanin0_;
    std::vector<int> base_fanin1_;
    std::vector<std::uint8_t> base_live_and_;
    std::vector<int> base_outputs_;
    std::vector<int> fanin0_;
    std::vector<int> fanin1_;
    std::vector<std::uint8_t> live_and_;
    std::vector<int> output_literals_;
    std::vector<int> output_position_;
    std::vector<int> fanouts_;
    std::vector<int> levels_;
    std::vector<int> arrivals_;
    std::vector<int> fanout_penalties_;
};

std::vector<Candidate> plan_candidates(const AigNetlist& net,
                                       const AnalysisResult& analysis,
                                       const Interval& cumulative,
                                       const Weight& threshold) {
    std::vector<Candidate> candidates;
    MutationScratch scratch;
    PlannerScoreWorkspace scores(net);
    std::size_t order = 0;
    for (const auto& gate : analysis.gates) {
        for (bool force_one : {false, true}) {
            const Interval local = force_one ? gate.sa1 : gate.sa0;
            const Interval projected(cumulative.l + local.l, cumulative.u + local.u);
            const auto stable_order = order++;
            if (interval_abs_bound(projected) > threshold) continue;
            Replacement replacement{gate.gate_id, force_one ? 1 : 0};
            const auto mutation = preview_replacement(net, replacement, scratch);
            candidates.push_back({replacement, local, projected, stable_order, scores.score(mutation)});
        }
    }
    // Stable identity is assigned before any filtering or sorting.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return std::make_tuple(a.score.fanout_adp, a.score.output_depth_sum,
                               interval_abs_bound(a.projected), a.order) <
               std::make_tuple(b.score.fanout_adp, b.score.output_depth_sum,
                               interval_abs_bound(b.projected), b.order);
    });
    return candidates;
}
} // namespace

PruningResult run_pruning(AigNetlist net, const PruningConfig& config) {
    const auto started = Clock::now();
    if (net.outputs.empty() || net.outputs.size() > Weight::nwords() * 64)
        throw std::invalid_argument("pruning requires between 1 and 256 outputs");
    if (!(config.timeout_seconds > 0) || !std::isfinite(config.timeout_seconds))
        throw std::invalid_argument("pruning timeout must be finite and positive");
    PruningResult result;
    result.initial_gates = count_and_gates(net);
    Interval cumulative;
    MutationScratch scratch;
    auto expired = [&] {
        if (elapsed(started) < config.timeout_seconds) return false;
        result.search_complete = false;
        result.budget_exhausted = true;
        result.termination_reason = "algorithm-budget-exhausted";
        return true;
    };
    AnalysisConfig analysis_config;
    analysis_config.threads = config.threads;
    analysis_config.window_pi_limit = 16;
    analysis_config.useful_error_threshold = config.threshold;
    while (!expired()) {
        const auto analysis = analyze(net, analysis_config);
        if (expired()) break;
        const auto candidates = plan_candidates(net, analysis, cumulative, config.threshold);
        if (expired() || candidates.empty()) break;
        const Candidate* selected = nullptr;
        bool unknown = false;
        for (const auto& candidate : candidates) {
            if (expired()) break;
            const double remaining = config.timeout_seconds - elapsed(started);
            if (!(remaining > 0)) { expired(); break; }
            const auto decision = check_gate_const_error_interval_sat(
                net, candidate.replacement.target_gate,
                candidate.replacement.replacement_literal == 1, candidate.local, remaining);
            if (decision == SatDecision::Unknown) {
                result.search_complete = false;
                unknown = true;
            }
            if (expired()) break;
            if (decision == SatDecision::Safe) { selected = &candidate; break; }
        }
        if (result.budget_exhausted) break;
        if (!selected) {
            result.termination_reason = unknown ? "sat-incomplete" : "all-candidates-refuted";
            break;
        }
        if (expired()) break;
        AigNetlist trial = net;
        apply_replacement(trial, selected->replacement, scratch);
        if (expired()) break;
        net = std::move(trial);
        cumulative = selected->projected;
    }
    result.final_gates = count_and_gates(net);
    result.estimated_interval = cumulative;
    result.net = std::move(net);
    return result;
}
} // namespace als
