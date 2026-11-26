#include "wce_sat.h"

#include "pruning/netlist_transform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "misc/util/abc_global.h"
#include "sat/bsat/satSolver.h"

namespace als {
namespace {


struct CnfFormula {
    int variables = 0;
    std::vector<std::vector<int>> clauses;
    // Owner 0 denotes a query constraint. Other clauses define a Boolean gate.
    std::vector<int> owners;
    std::vector<std::vector<std::size_t>> definitions{1};

    int new_variable() {
        definitions.emplace_back();
        return ++variables;
    }
    void add_clause(std::initializer_list<int> literals) {
        clauses.emplace_back(literals);
        owners.push_back(0);
    }
    void define(int output, std::initializer_list<int> literals) {
        definitions.at(output).push_back(clauses.size());
        clauses.emplace_back(literals);
        owners.push_back(output);
    }

    // Retain the transitive fanin of ALL query constraints, including fault
    // activation. Removed gates are total Boolean functions, so their clauses
    // can always be satisfied by extending an assignment of the retained cone.
    // Never replace a retained internal signal by a free cut input: doing so
    // would lose the upstream correlations needed for an exact SAT decision.
    void compact() {
        // Constant folding can already make the violation predicate false.
        // Keep a minimal contradictory CNF for the solver instead of loading
        // the (now irrelevant) activation cone alongside that contradiction.
        std::vector<signed char> units(variables + 1, 0);
        for (const auto& clause : clauses) {
            if (clause.size() != 1) continue;
            const int variable = std::abs(clause.front());
            const signed char value = clause.front() > 0 ? 1 : -1;
            if (units[variable] == -value) {
                variables = 1;
                clauses = {{1}, {-1}};
                owners.clear();
                definitions.clear();
                return;
            }
            units[variable] = value;
        }
        std::vector<unsigned char> live(variables + 1, 0);
        std::vector<int> pending;
        auto mark = [&](int literal) {
            const int variable = std::abs(literal);
            if (!live[variable]) {
                live[variable] = 1;
                pending.push_back(variable);
            }
        };
        for (std::size_t i = 0; i < clauses.size(); ++i)
            if (owners[i] == 0)
                for (int literal : clauses[i]) mark(literal);
        while (!pending.empty()) {
            const int variable = pending.back();
            pending.pop_back();
            for (std::size_t index : definitions[variable])
                for (int literal : clauses[index]) mark(literal);
        }
        std::vector<int> remap(variables + 1, 0);
        int count = 0;
        for (int variable = 1; variable <= variables; ++variable)
            if (live[variable]) remap[variable] = ++count;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < clauses.size(); ++i) {
            if (owners[i] != 0 && !live[owners[i]]) continue;
            if (kept != i) clauses[kept] = std::move(clauses[i]);
            for (int& literal : clauses[kept])
                literal = literal < 0 ? -remap[-literal] : remap[literal];
            ++kept;
        }
        clauses.resize(kept);
        variables = count;
        owners.clear();
        definitions.clear();
    }
};

class BooleanCnfBuilder {
  public:
    explicit BooleanCnfBuilder(CnfFormula& formula)
        : formula_(formula) {
        false_literal_ = formula_.new_variable();
        formula_.define(false_literal_, {-false_literal_});
    }

    int false_literal() const { return false_literal_; }
    int true_literal() const { return -false_literal_; }

    int make_and(int lhs, int rhs) {
        if (lhs == false_literal() || rhs == false_literal() || lhs == -rhs)
            return false_literal();
        if (lhs == true_literal() || lhs == rhs) return rhs;
        if (rhs == true_literal()) return lhs;
        const auto key = gate_key(lhs, rhs);
        const auto found = ands_.find(key);
        if (found != ands_.end()) return found->second;
        const int out = formula_.new_variable();
        formula_.define(out, {-lhs, -rhs, out});
        formula_.define(out, {lhs, -out});
        formula_.define(out, {rhs, -out});
        ands_.emplace(key, out);
        return out;
    }

    int make_or(int lhs, int rhs) {
        return -make_and(-lhs, -rhs);
    }

    int make_xor(int lhs, int rhs) {
        if (lhs == rhs) return false_literal();
        if (lhs == -rhs) return true_literal();
        if (lhs == false_literal()) return rhs;
        if (rhs == false_literal()) return lhs;
        if (lhs == true_literal()) return -rhs;
        if (rhs == true_literal()) return -lhs;
        const bool inverted = (lhs < 0) != (rhs < 0);
        lhs = std::abs(lhs);
        rhs = std::abs(rhs);
        const auto key = gate_key(lhs, rhs);
        const auto found = xors_.find(key);
        if (found != xors_.end()) return inverted ? -found->second : found->second;
        const int out = formula_.new_variable();
        formula_.define(out, {-lhs, -rhs, -out});
        formula_.define(out, {lhs, rhs, -out});
        formula_.define(out, {lhs, -rhs, out});
        formula_.define(out, {-lhs, rhs, out});
        xors_.emplace(key, out);
        return inverted ? -out : out;
    }

    std::pair<int, int> make_full_adder(int lhs, int rhs, int carry_in) {
        if (lhs == rhs) return {carry_in, lhs};
        if (lhs == -rhs) return {-carry_in, carry_in};
        if (lhs == carry_in) return {rhs, lhs};
        if (lhs == -carry_in) return {-rhs, rhs};
        if (rhs == carry_in) return {lhs, rhs};
        if (rhs == -carry_in) return {-lhs, lhs};
        // Move any constant into carry_in, then use a half adder.
        if (lhs == false_literal() || lhs == true_literal()) std::swap(lhs, carry_in);
        if (rhs == false_literal() || rhs == true_literal()) std::swap(rhs, carry_in);
        const int partial = make_xor(lhs, rhs);
        if (carry_in == false_literal()) return {partial, make_and(lhs, rhs)};
        if (carry_in == true_literal()) return {-partial, make_or(lhs, rhs)};
        const int sum = make_xor(partial, carry_in);
        const int carry_out = formula_.new_variable();
        formula_.define(carry_out, {-lhs, -rhs, carry_out});
        formula_.define(carry_out, {-lhs, -carry_in, carry_out});
        formula_.define(carry_out, {-rhs, -carry_in, carry_out});
        formula_.define(carry_out, {lhs, rhs, -carry_out});
        formula_.define(carry_out, {lhs, carry_in, -carry_out});
        formula_.define(carry_out, {rhs, carry_in, -carry_out});
        return {sum, carry_out};
    }

    std::vector<int> make_signed_difference(const std::vector<int>& lhs,
                                            const std::vector<int>& rhs) {
        if (lhs.size() != rhs.size())
            throw std::invalid_argument("SAT difference width mismatch");
        std::vector<int> difference;
        difference.reserve(lhs.size() + 1);
        int carry = true_literal();
        for (std::size_t bit = 0; bit <= lhs.size(); ++bit) {
            const int a = bit < lhs.size() ? lhs[bit] : false_literal();
            const int inverted_b = bit < rhs.size() ? -rhs[bit] : true_literal();
            auto [sum, next_carry] = make_full_adder(a, inverted_b, carry);
            difference.push_back(sum);
            carry = next_carry;
        }
        return difference;
    }

    int make_signed_greater(const std::vector<int>& lhs,
                            const std::vector<int>& rhs) {
        if (lhs.size() != rhs.size() || lhs.size() < 2)
            throw std::invalid_argument("SAT signed comparator width mismatch");
        int equal_prefix = true_literal();
        int greater = false_literal();
        for (std::size_t step = 0; step < lhs.size(); ++step) {
            const std::size_t bit = lhs.size() - 1 - step;
            const int a = bit == lhs.size() - 1 ? -lhs[bit] : lhs[bit];
            const int b = bit == rhs.size() - 1 ? -rhs[bit] : rhs[bit];
            const int bit_greater = make_and(a, -b);
            greater = make_or(greater, make_and(equal_prefix, bit_greater));
            equal_prefix = make_and(equal_prefix, -make_xor(a, b));
        }
        return greater;
    }

  private:
    static std::uint64_t gate_key(int lhs, int rhs) {
        if (lhs > rhs) std::swap(lhs, rhs);
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(lhs)) << 32) |
               static_cast<std::uint32_t>(rhs);
    }
    CnfFormula& formula_;
    int false_literal_ = 0;
    std::unordered_map<std::uint64_t, int> ands_, xors_;
};

bool weight_bit(const Weight& value, std::size_t bit) {
    if (bit >= Weight::nwords() * 64) return false;
    return ((value.data()[bit / 64] >> (bit % 64)) & 1U) != 0;
}

std::vector<bool> signed_to_twos_complement(const SignedWeight& value,
                                            std::size_t width) {
    std::vector<bool> bits(width, false);
    if (!value.is_negative()) {
        for (std::size_t bit = 0; bit < width; ++bit)
            bits[bit] = weight_bit(value.abs(), bit);
        return bits;
    }
    bool carry = true;
    for (std::size_t bit = 0; bit < width; ++bit) {
        const bool inverted = !weight_bit(value.abs(), bit);
        bits[bit] = inverted != carry;
        carry = inverted && carry;
    }
    return bits;
}

std::vector<int> constant_signed_bits(BooleanCnfBuilder& builder,
                                      const SignedWeight& value,
                                      std::size_t width) {
    const std::vector<bool> bits = signed_to_twos_complement(value, width);
    std::vector<int> result;
    result.reserve(width);
    for (bool bit : bits)
        result.push_back(bit ? builder.true_literal() : builder.false_literal());
    return result;
}

int aig_literal(int literal, const std::vector<int>& values,
                const BooleanCnfBuilder& builder) {
    const int node = lit_to_var(literal);
    int result = node == 0 ? builder.false_literal() : values.at(node);
    if (result == 0)
        throw std::runtime_error("SAT encoder encountered a non-live AIG literal");
    return lit_is_inv(literal) ? -result : result;
}

std::vector<int> shared_inputs(CnfFormula& formula, std::size_t count) {
    std::vector<int> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        result.push_back(formula.new_variable());
    return result;
}

struct EncodedNet {
    std::vector<int> by_node;
    std::vector<int> outputs;
};

EncodedNet encode_plain_net(const AigNetlist& net,
                            const std::vector<int>& input_variables,
                            BooleanCnfBuilder& builder) {
    EncodedNet encoded;
    encoded.by_node.assign(net.nodes.size(), 0);
    for (std::size_t i = 0; i < net.inputs.size(); ++i)
        encoded.by_node[net.inputs[i]] = input_variables[i];
    for (int gate = 1; gate <= net.max_var; ++gate) {
        if (net.nodes[gate].type != NodeType::AND) continue;
        encoded.by_node[gate] = builder.make_and(
            aig_literal(net.nodes[gate].fanin0, encoded.by_node, builder),
            aig_literal(net.nodes[gate].fanin1, encoded.by_node, builder));
    }
    encoded.outputs.reserve(net.outputs.size());
    for (int output : net.outputs)
        encoded.outputs.push_back(
            aig_literal(net.nodes[output].fanin0, encoded.by_node, builder));
    return encoded;
}

std::vector<int> encode_gate_const_outputs(
    const AigNetlist& current, const EncodedNet& encoded_reference,
    const std::vector<int>& input_variables, BooleanCnfBuilder& builder,
    int target_gate, bool force_one) {
    if (target_gate <= 0 || target_gate > current.max_var ||
        current.nodes[target_gate].type != NodeType::AND)
        throw std::invalid_argument(
            "interval SAT target is not a live AND gate");
    std::vector<int> candidate(current.nodes.size(), 0);
    for (std::size_t index = 0; index < current.inputs.size(); ++index)
        candidate[current.inputs[index]] = input_variables[index];
    for (int gate = 1; gate <= current.max_var; ++gate) {
        if (current.nodes[gate].type != NodeType::AND) continue;
        if (gate == target_gate) {
            candidate[gate] = force_one ? builder.true_literal()
                                        : builder.false_literal();
            continue;
        }
        const int lhs = aig_literal(
            current.nodes[gate].fanin0, candidate, builder);
        const int rhs = aig_literal(
            current.nodes[gate].fanin1, candidate, builder);
        const int reference_lhs = aig_literal(
            current.nodes[gate].fanin0, encoded_reference.by_node, builder);
        const int reference_rhs = aig_literal(
            current.nodes[gate].fanin1, encoded_reference.by_node, builder);
        if ((lhs == reference_lhs && rhs == reference_rhs) ||
            (lhs == reference_rhs && rhs == reference_lhs)) {
            candidate[gate] = encoded_reference.by_node[gate];
        } else {
            candidate[gate] = builder.make_and(lhs, rhs);
        }
    }
    std::vector<int> outputs;
    outputs.reserve(current.outputs.size());
    for (int output : current.outputs)
        outputs.push_back(aig_literal(
            current.nodes[output].fanin0, candidate, builder));
    return outputs;
}

bool interval_contains_zero(const WeightedInterval& interval) {
    const SignedWeight zero;
    return interval.l <= zero && zero <= interval.u;
}

int add_interval_violation(CnfFormula& formula, BooleanCnfBuilder& builder,
                           const std::vector<int>& reference_outputs,
                           const std::vector<int>& candidate_outputs,
                           const WeightedInterval& interval) {
    if (!interval.is_valid())
        throw std::invalid_argument("SAT error interval is invalid");
    // Exact integer identity: equal output terms cancel from sum_i 2^i(C_i-R_i).
    // Keep their positions: packing surviving bits would change their weights.
    auto reduced_candidate = candidate_outputs;
    auto reduced_reference = reference_outputs;
    for (std::size_t bit = 0; bit < reference_outputs.size(); ++bit) {
        if (candidate_outputs[bit] == reference_outputs[bit]) {
            reduced_candidate[bit] = builder.false_literal();
            reduced_reference[bit] = builder.false_literal();
        }
    }
    const std::vector<int> difference =
        builder.make_signed_difference(reduced_candidate, reduced_reference);
    const SignedWeight cap = signed_error_cap(reference_outputs.size());
    if (interval.l < -cap || interval.u > cap)
        throw std::invalid_argument(
            "SAT error interval exceeds the representable output range");
    const auto lower = constant_signed_bits(
        builder, interval.l, difference.size());
    const auto upper = constant_signed_bits(
        builder, interval.u, difference.size());
    const int above = builder.make_signed_greater(difference, upper);
    const int below = builder.make_signed_greater(lower, difference);
    const int violation = builder.make_or(above, below);
    formula.add_clause({violation});
    return violation;
}

enum class SolveStatus { Sat, Unsat, Unknown };

class AbcSatSolver {
  public:
    explicit AbcSatSolver(const CnfFormula& formula) : solver_(sat_solver_new()) {
        if (!solver_)
            throw std::runtime_error("ABC bsat solver allocation failed");
        sat_solver_setnvars(solver_, formula.variables);
        for (const auto& clause : formula.clauses) {
            std::vector<lit> converted;
            converted.reserve(clause.size());
            for (int literal : clause)
                converted.push_back(lit_read(literal));
            if (!sat_solver_addclause(solver_, converted.data(),
                                      converted.data() + converted.size()))
                inconsistent_ = true;
        }
    }

    ~AbcSatSolver() { sat_solver_delete(solver_); }
    AbcSatSolver(const AbcSatSolver&) = delete;
    AbcSatSolver& operator=(const AbcSatSolver&) = delete;

    void profile_stats(SatProfile& profile) const {
        profile.conflicts = solver_->stats.conflicts;
        profile.decisions = solver_->stats.decisions;
        profile.propagations = solver_->stats.propagations;
    }

    SolveStatus solve(double timeout_seconds) {
        if (inconsistent_) return SolveStatus::Unsat;
        if (!(timeout_seconds > 0.0) || !std::isfinite(timeout_seconds))
            throw std::invalid_argument("SAT query timeout must be finite and positive");
        const abctime now = Abc_Clock();
        const abctime maximum = std::numeric_limits<abctime>::max();
        const long double requested_ticks = std::ceil(
            static_cast<long double>(timeout_seconds) * CLOCKS_PER_SEC);
        const abctime ticks =
            requested_ticks >= static_cast<long double>(maximum - now)
                ? maximum - now
                : std::max<abctime>(
                      1, static_cast<abctime>(requested_ticks));
        sat_solver_set_runtime_limit(solver_, now + ticks);
        const int status = sat_solver_solve(
            solver_, nullptr, nullptr, 0, 0, 0, 0);
        sat_solver_set_runtime_limit(solver_, 0);
        if (status == l_True) return SolveStatus::Sat;
        if (status == l_False) return SolveStatus::Unsat;
        return SolveStatus::Unknown;
    }

  private:
    sat_solver* solver_ = nullptr;
    bool inconsistent_ = false;
};

} // namespace

SatDecision check_gate_const_error_interval_sat(
    const AigNetlist& current, int gate_id, bool force_one,
    const WeightedInterval& interval, double timeout_seconds, SatProfile* profile) {
    using Clock = std::chrono::steady_clock;
    const auto started = profile ? Clock::now() : Clock::time_point{};
    validate_live_aig(current);
    if (current.outputs.empty() || current.outputs.size() > Weight::nwords() * 64)
        throw std::invalid_argument("SAT requires between 1 and 256 outputs");
    if (gate_id <= 0 || gate_id > current.max_var ||
        current.nodes[gate_id].type != NodeType::AND)
        throw std::invalid_argument(
            "interval SAT target is not a live AND gate");
    if (!interval.is_valid())
        throw std::invalid_argument("local SAT interval is invalid");
    CnfFormula formula;
    BooleanCnfBuilder builder(formula);
    const auto inputs = shared_inputs(formula, current.inputs.size());
    EncodedNet reference = encode_plain_net(current, inputs, builder);
    std::vector<int> candidate_outputs = encode_gate_const_outputs(
        current, reference, inputs, builder, gate_id, force_one);
    if (interval_contains_zero(interval)) {
        const int reference_gate = reference.by_node.at(gate_id);
        formula.add_clause({force_one ? -reference_gate : reference_gate});
    }
    add_interval_violation(
        formula, builder, reference.outputs, candidate_outputs, interval);
    if (profile) {
        profile->variables_before_compaction = formula.variables;
        profile->clauses_before_compaction = formula.clauses.size();
        profile->outputs = reference.outputs.size();
        profile->changed_outputs = 0;
        for (std::size_t bit = 0; bit < reference.outputs.size(); ++bit)
            profile->changed_outputs += reference.outputs[bit] != candidate_outputs[bit];
    }
    formula.compact();
    const auto encoded = profile ? Clock::now() : Clock::time_point{};
    AbcSatSolver solver(formula);
    const auto loaded = profile ? Clock::now() : Clock::time_point{};
    const SolveStatus status = solver.solve(timeout_seconds);
    if (profile) {
        profile->encode_seconds = std::chrono::duration<double>(encoded - started).count();
        profile->load_seconds = std::chrono::duration<double>(loaded - encoded).count();
        profile->solve_seconds = std::chrono::duration<double>(Clock::now() - loaded).count();
        profile->variables = formula.variables;
        profile->clauses = formula.clauses.size();
        solver.profile_stats(*profile);
    }

    if (status == SolveStatus::Unsat) return SatDecision::Safe;
    if (status == SolveStatus::Sat) return SatDecision::Unsafe;
    return SatDecision::Unknown;
}
} // namespace als
