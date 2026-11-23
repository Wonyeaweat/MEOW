#include "meow.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <omp.h>

using namespace std;

namespace als::meow {

// =========================== helpers ===========================

constexpr size_t kInvalidWindowSlot =
    std::numeric_limits<size_t>::max();

uint32_t window_rng_seed(uint32_t base_seed, int gate_id, int window_index) {
    return base_seed ^
           (0x9e3779b9u * static_cast<uint32_t>(gate_id)) ^
           (0x85ebca6bu * static_cast<uint32_t>(window_index + 1));
}

struct MeowThreadState {
    std::mt19937 rng;
    OverlapRunContext::SimulationWorkspace workspace;
    OverlapRunContext::WindowBuildScratch window_scratch;
};

int gate_id_from_fanin(int e) {
    return e >> 1;
}

std::pair<int, int> ordered_fanin_gates(const Node& node) {
    if (node.type != NodeType::AND) return {0, 0};
    int f = gate_id_from_fanin(node.fanin0), s = gate_id_from_fanin(node.fanin1);
    if (f == 0) std::swap(f, s);
    if (s != 0 && s < f) std::swap(f, s);
    if (s == f) s = 0;
    return std::make_pair(f, s);
}

static void complete_window_cut_backward(OverlapRunContext& run_ctx,
                                         WindowBlock& block,
                                         int max_cut_inputs);

WindowBlock build_full_tfo_window_from_mask(AigNetlist& net, int gate_id, const BitPattern& tm,
                                            int max_pi, int max_gates) {
    WindowBlock block;
    BitPattern touched_mask = tm;
    if (!net.forward_output_reachability_valid)
        throw std::logic_error("build_full_tfo_window_from_mask: fwd_po_reachability not computed");
    touched_mask &= net.forward_output_masks[gate_id];
    if (touched_mask.is_zero()) return WindowBlock();
    std::vector<int> touched_pos;
    touched_pos.reserve(touched_mask.popcount());
    touched_mask.for_each_bit([&](size_t b) { if (b < net.outputs.size()) touched_pos.push_back(net.outputs[b]); });
    static thread_local std::vector<uint8_t> in_tfi;
    static thread_local std::vector<int> tfi_touched;
    if (in_tfi.size() < net.nodes.size()) in_tfi.assign(net.nodes.size(), 0);
    tfi_touched.clear();
    std::vector<int> tfi_ands, tfi_pis, stack;
    auto tv = [&](int v) -> bool {
        if (v == 0 || in_tfi[v]) return true;
        BitPattern o = net.forward_output_masks[v];
        o &= touched_mask;
        if (o.is_zero()) return true;
        in_tfi[v] = 1;
        tfi_touched.push_back(v);
        const Node& n = net.nodes[v];
        if (n.type == NodeType::INPUT) {
            tfi_pis.push_back(v);
            if ((int) tfi_pis.size() > max_pi) return false;
        } else if (n.type == NodeType::AND) {
            tfi_ands.push_back(v);
            if ((int) tfi_ands.size() > max_gates) return false;
            stack.push_back(v);
        }
        return true;
    };
    auto fail = [&]() { for (int t : tfi_touched) in_tfi[t] = 0; tfi_touched.clear(); return WindowBlock(); };
    for (int po : touched_pos) {
        const Node& n = net.nodes[po];
        if (!tv(gate_id_from_fanin(n.fanin0))) return fail();
        if (n.type == NodeType::AND && !tv(gate_id_from_fanin(n.fanin1))) return fail();
    }
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        const Node& n = net.nodes[cur];
        if (!tv(gate_id_from_fanin(n.fanin0))) return fail();
        if (n.type == NodeType::AND && !tv(gate_id_from_fanin(n.fanin1))) return fail();
    }
    if (!in_tfi[gate_id]) {
        in_tfi[gate_id] = 1;
        tfi_touched.push_back(gate_id);
        tfi_ands.push_back(gate_id);
    }
    for (int t : tfi_touched) in_tfi[t] = 0;
    tfi_touched.clear();
    block.reset_gates(std::move(tfi_ands));
    block.pis.insert(tfi_pis.begin(), tfi_pis.end());
    block.pos.insert(touched_pos.begin(), touched_pos.end());
    if (block.pis.size() > (size_t) max_pi) return WindowBlock();
    return block;
}

SignedWeight reachable_output_weight_sum(const AigNetlist& net, int gid) {
    Weight w;
    net.forward_output_masks[gid].for_each_bit([&](size_t b) { w += Weight::from_bit(b); });
    return SignedWeight::positive(w);
}

ForcedValueIntervals include_zero_contribution(const ForcedValueIntervals& fo, bool inv) {
    SignedWeight z(0);
    auto CL = [&](const SignedWeight& l) -> SignedWeight { return z.cmp(l) < 0 ? z : l; };
    auto CU = [&](const SignedWeight& u) -> SignedWeight { return z.cmp(u) > 0 ? z : u; };
    return ForcedValueIntervals{
        Interval(CL(inv ? fo.sa1.l : fo.sa0.l), CU(inv ? fo.sa1.u : fo.sa0.u)),
        Interval(CL(inv ? fo.sa0.l : fo.sa1.l), CU(inv ? fo.sa0.u : fo.sa1.u)),
    };
}

WindowKey make_window_key(const OverlapGateContext& gc, const WindowBlock& blk) {
    WindowKey k;
    k.gate_id = gc.gate_id;
    k.gates = blk.gates;
    k.pis.assign(blk.pis.begin(), blk.pis.end());
    std::sort(k.pis.begin(), k.pis.end());
    k.pos.assign(blk.pos.begin(), blk.pos.end());
    std::sort(k.pos.begin(), k.pos.end());
    return k;
}

void clamp_interval(Interval& iv, SignedWeight cap) {
    auto n = -cap;
    if (iv.l < n) iv.l = n;
    if (iv.u > cap) iv.u = cap;
}
void clamp_fi(ForcedValueIntervals& fv, SignedWeight cap) {
    clamp_interval(fv.sa0, cap);
    clamp_interval(fv.sa1, cap);
}

ForcedValueIntervals combine_family(const ForcedValueIntervals& cur, const ForcedValueIntervals& upd, bool ev, SignedWeight cap) {
    ForcedValueIntervals c = ev ? intersect_intervals(cur, upd) : upd;
    clamp_fi(c, cap);
    return c;
}
ForcedValueIntervals add_fi_checked(const ForcedValueIntervals& lhs, const ForcedValueIntervals& rhs) {
    return {Interval(lhs.sa0.l + rhs.sa0.l, lhs.sa0.u + rhs.sa0.u), Interval(lhs.sa1.l + rhs.sa1.l, lhs.sa1.u + rhs.sa1.u)};
}

WindowSimResult build_init_interval_result(OverlapRunContext& rctx,
                                           const OverlapGateContext& gctx) {
    const AigNetlist& net = rctx.net;
    WindowSimResult result;
    for (int fid : net.nodes[gctx.gate_id].fanouts) {
        if (fid <= 0 || fid >= (int) net.nodes.size()) throw std::runtime_error("MEOW init: bad fanout " + std::to_string(fid));
        const Node& fn = net.nodes[fid];
        bool inv = false, mapped = false;
        if (fn.type == NodeType::OUTPUT) {
            mapped = lit_to_var(fn.fanin0) == gctx.gate_id;
            inv = mapped && lit_is_inv(fn.fanin0);
        } else if (fn.type == NodeType::AND) {
            if (!rctx.analysis.node_force_finalized[fid]) throw std::runtime_error("MEOW init: unfinished fanout " + std::to_string(fid));
            if (lit_to_var(fn.fanin0) == gctx.gate_id) {
                mapped = true;
                inv = lit_is_inv(fn.fanin0);
            } else if (lit_to_var(fn.fanin1) == gctx.gate_id) {
                mapped = true;
                inv = lit_is_inv(fn.fanin1);
            }
        }
        if (!mapped) throw std::runtime_error("MEOW init: cannot map fanout " + std::to_string(fid));
        result.force = add_fi_checked(
            result.force,
            include_zero_contribution(rctx.analysis.node_force_intervals[fid], inv));
        result.transition = add_fi_checked(
            result.transition,
            include_zero_contribution(rctx.node_transition_intervals[fid], inv));
    }
    return result;
}

void apply_family_result(OverlapRunContext& rctx, OverlapGateContext& gctx, const WindowSimResult& sim) {
    ForcedValueIntervals cap{
        Interval(-gctx.reachable_output_cap, gctx.reachable_output_cap),
        Interval(-gctx.reachable_output_cap, gctx.reachable_output_cap)};
    ForcedValueIntervals res = intersect_intervals(sim.force, cap), trans = intersect_intervals(sim.transition, cap);
    clamp_fi(res, rctx.global_error_cap);
    clamp_fi(trans, rctx.global_error_cap);
    bool ev = gctx.block_evaluated;
    gctx.best_force = combine_family(gctx.best_force, res, ev, rctx.global_error_cap);
    gctx.best_transition = combine_family(gctx.best_transition, trans, ev, rctx.global_error_cap);
    gctx.block_evaluated = true;
}

ForcedValueIntervals simulate_window_block_with_workspace(
    AigNetlist& net, const WindowBlock& blk, int gid,
    const std::vector<ForcedValueIntervals>& nti, ForcedValueIntervals* tr,
    OverlapRunContext::SimulationWorkspace& ws) {
    ForcedValueIntervals res;
    ws.output_list.assign(blk.pos.begin(), blk.pos.end());
    auto& pl = ws.output_list;
    std::sort(pl.begin(), pl.end());
    if (pl.empty())
        throw std::runtime_error("MEOW: empty PO list for gate " + std::to_string(gid));

    ws.input_list.assign(blk.pis.begin(), blk.pis.end());
    auto& pi_list = ws.input_list;
    ws.gate_list = blk.gates;
    auto& gl = ws.gate_list;
    if (!std::is_sorted(gl.begin(), gl.end()))
        std::sort(gl.begin(), gl.end());
    int np = static_cast<int>(pi_list.size());
    if (np >= 30)
        throw std::runtime_error("MEOW: too many PIs (" + std::to_string(np) +
                                 ") for gate " + std::to_string(gid));

    const uint64_t tp = (1ULL << np);
    const size_t nw = (tp + 63) / 64;
    const int ng = static_cast<int>(gl.size());
    const int ns = np + ng;

    if (ws.slot_map.size() < net.nodes.size())
        ws.slot_map.assign(net.nodes.size(), -1);
    ws.touched_nodes.clear();

    const size_t arena_size = static_cast<size_t>(ns) * nw;
    if (ws.arena.size() < arena_size)
        ws.arena.resize(arena_size);
    if (ws.const_zero_buf.size() < nw)
        ws.const_zero_buf.resize(nw, 0ULL);

    auto slot_data = [&](int slot) -> uint64_t* {
        return ws.arena.data() + static_cast<size_t>(slot) * nw;
    };

    for (int j = 0; j < np; ++j) {
        ws.slot_map[pi_list[j]] = j;
        ws.touched_nodes.push_back(pi_list[j]);
    }
    for (int i = 0; i < ng; ++i) {
        ws.slot_map[gl[i]] = np + i;
        ws.touched_nodes.push_back(gl[i]);
    }

    for (int j = 0; j < np; ++j) {
        uint64_t* w = slot_data(j);
        if (j < 6) {
            uint64_t mask = 0, run = 1ULL << j;
            for (uint64_t bit = 0; bit < 64; ++bit)
                if ((bit / run) & 1ULL)
                    mask |= 1ULL << bit;
            for (size_t k = 0; k < nw; ++k)
                w[k] = mask;
        } else {
            uint64_t word_run = 1ULL << (j - 6);
            for (size_t k = 0; k < nw; ++k)
                w[k] = ((k / word_run) & 1ULL) ? ~0ULL : 0ULL;
        }
    }

    uint64_t tail_mask = ~0ULL;
    if (tp < 64)
        tail_mask = (tp == 64) ? ~0ULL : ((1ULL << tp) - 1ULL);

    auto source_for_literal = [&](int lit, const uint64_t*& ptr, uint64_t& mask) {
        int var = lit_to_var(lit);
        mask = lit_is_inv(lit) ? ~0ULL : 0ULL;

        if (var == 0) {
            ptr = ws.const_zero_buf.data();
            return;
        }

        if (var < 0 || var >= static_cast<int>(net.nodes.size()) ||
            (net.nodes[var].type != NodeType::INPUT &&
             net.nodes[var].type != NodeType::AND))
            throw std::runtime_error(
                "MEOW bp sim: live fanin references tombstone " +
                std::to_string(var));

        int slot = ws.slot_map[var];
        if (slot < 0)
            throw std::runtime_error("MEOW bp sim: missing node " + std::to_string(var));
        ptr = slot_data(slot);
    };

    auto compute_gate = [&](int gate_idx) {
        const Node& gate = net.nodes[gl[gate_idx]];
        const uint64_t* src0;
        uint64_t mask0;
        source_for_literal(gate.fanin0, src0, mask0);

        uint64_t* dst = slot_data(np + gate_idx);
        if (gate.type == NodeType::AND) {
            const uint64_t* src1;
            uint64_t mask1;
            source_for_literal(gate.fanin1, src1, mask1);
            for (size_t k = 0; k < nw; ++k)
                dst[k] = (src0[k] ^ mask0) & (src1[k] ^ mask1);
        } else {
            for (size_t k = 0; k < nw; ++k)
                dst[k] = src0[k] ^ mask0;
        }
    };

    for (int i = 0; i < ng; ++i)
        compute_gate(i);

    int gate_slot = ws.slot_map[gid];
    ws.base_gate_values.resize(nw);
    auto& base_gate_values = ws.base_gate_values;
    {
        const uint64_t* g = slot_data(gate_slot);
        for (size_t k = 0; k < nw; ++k)
            base_gate_values[k] = g[k];
    }

    ws.affected.resize(static_cast<std::size_t>(ns));
    std::fill_n(ws.affected.begin(), ns, std::uint8_t{0});
    auto& affected = ws.affected;
    affected[gate_slot] = 1;
    for (int i = 0; i < ng; ++i) {
        const Node& gate = net.nodes[gl[i]];
        int fanin0_var = lit_to_var(gate.fanin0);
        bool a = (fanin0_var != 0 && ws.slot_map[fanin0_var] >= 0 && affected[ws.slot_map[fanin0_var]]);
        if (!a && gate.type == NodeType::AND) {
            int fanin1_var = lit_to_var(gate.fanin1);
            a = (fanin1_var != 0 && ws.slot_map[fanin1_var] >= 0 && affected[ws.slot_map[fanin1_var]]);
        }
        if (a)
            affected[np + i] = 1;
    }

    const size_t output_count = pl.size();
    auto evaluate_output = [&](int po, uint64_t* dst, bool& output_affected) {
        const Node& node = net.nodes[po];

        if (node.type == NodeType::OUTPUT) {
            const uint64_t* src;
            uint64_t mask;
            source_for_literal(node.fanin0, src, mask);
            for (size_t k = 0; k < nw; ++k)
                dst[k] = src[k] ^ mask;
            int var = lit_to_var(node.fanin0);
            output_affected = (var != 0 && ws.slot_map[var] >= 0 && affected[ws.slot_map[var]]);
            return;
        }

        if (node.type == NodeType::AND && !blk.gates_set.count(po)) {
            const uint64_t *src0, *src1;
            uint64_t mask0, mask1;
            source_for_literal(node.fanin0, src0, mask0);
            source_for_literal(node.fanin1, src1, mask1);
            for (size_t k = 0; k < nw; ++k)
                dst[k] = (src0[k] ^ mask0) & (src1[k] ^ mask1);
            int fanin0_var = lit_to_var(node.fanin0);
            int fanin1_var = lit_to_var(node.fanin1);
            output_affected =
                (fanin0_var != 0 && ws.slot_map[fanin0_var] >= 0 && affected[ws.slot_map[fanin0_var]]) ||
                (fanin1_var != 0 && ws.slot_map[fanin1_var] >= 0 && affected[ws.slot_map[fanin1_var]]);
            return;
        }

        int slot = ws.slot_map[po];
        const uint64_t* src = slot_data(slot);
        for (size_t k = 0; k < nw; ++k)
            dst[k] = src[k];
        output_affected = (affected[slot] != 0);
    };

    const std::size_t output_words = output_count * nw;
    ws.base_outputs.resize(output_words);
    ws.forced_outputs.resize(output_words);
    ws.output_is_affected.resize(output_count);
    std::fill(ws.output_is_affected.begin(), ws.output_is_affected.end(),
              std::uint8_t{0});
    auto& base_outputs = ws.base_outputs;
    auto& forced_outputs = ws.forced_outputs;
    auto& output_is_affected = ws.output_is_affected;
    for (size_t i = 0; i < output_count; ++i) {
        bool output_affected = false;
        evaluate_output(pl[i], base_outputs.data() + i * nw, output_affected);
        output_is_affected[i] = output_affected ? 1 : 0;
    }

    bool gate_is_internal = (gate_slot >= np);
    if (!gate_is_internal) {
        uint64_t* gate_word = slot_data(gate_slot);
        for (size_t k = 0; k < nw; ++k)
            gate_word[k] = ~gate_word[k];
    }

    for (int i = 0; i < ng; ++i) {
        if (!affected[np + i])
            continue;
        compute_gate(i);
        if (gate_is_internal && (np + i) == gate_slot) {
            uint64_t* gate_word = slot_data(gate_slot);
            for (size_t k = 0; k < nw; ++k)
                gate_word[k] = ~gate_word[k];
        }
    }

    for (size_t i = 0; i < output_count; ++i) {
        if (!output_is_affected[i])
            continue;
        bool ignored;
        evaluate_output(pl[i], forced_outputs.data() + i * nw, ignored);
    }

    if (nti.empty())
        throw std::invalid_argument("MEOW window simulation requires transition intervals");
    ws.per_output_bounds.resize(output_count);
    auto& per_output_bounds = ws.per_output_bounds;
    for (size_t i = 0; i < output_count; ++i)
        per_output_bounds[i] = nti[pl[i]];

    std::array<std::optional<Interval>, 2> transition_union;
    auto include_transition = [&](bool is_sa0, const Interval& update) {
        auto& target = transition_union[is_sa0 ? 0 : 1];
        if (!target)
            target = update;
        else
            expand_interval(*target, update);
    };

    std::array<SignedWeight, 64> partial_l{};
    std::array<SignedWeight, 64> partial_u{};
    for (size_t word = 0; word < nw; ++word) {
        uint64_t base_gate = base_gate_values[word];
        uint64_t valid = (word == nw - 1) ? tail_mask : ~0ULL;
        uint64_t any_delta = 0;

        for (size_t i = 0; i < output_count; ++i) {
            if (!output_is_affected[i])
                continue;

            const std::size_t output_word = i * nw + word;
            uint64_t delta =
                (base_outputs[output_word] ^ forced_outputs[output_word]) & valid;
            while (delta) {
                int bit = __builtin_ctzll(delta);
                uint64_t bit_mask = 1ULL << bit;
                const Interval& contribution =
                    (forced_outputs[output_word] & bit_mask)
                        ? per_output_bounds[i].sa1
                        : per_output_bounds[i].sa0;
                size_t index = static_cast<size_t>(bit);
                if (any_delta & bit_mask) {
                    partial_l[index] += contribution.l;
                    partial_u[index] += contribution.u;
                } else {
                    partial_l[index] = contribution.l;
                    partial_u[index] = contribution.u;
                    any_delta |= bit_mask;
                }
                delta &= delta - 1;
            }
        }

        if (any_delta == 0) {
            if (base_gate & valid)
                include_transition(true, Interval());
            if ((~base_gate) & valid)
                include_transition(false, Interval());
            continue;
        }

        uint64_t sa0_no_delta = (base_gate & valid) & ~any_delta;
        uint64_t sa1_no_delta = ((~base_gate) & valid) & ~any_delta;
        if (sa0_no_delta)
            include_transition(true, Interval());
        if (sa1_no_delta)
            include_transition(false, Interval());

        while (any_delta) {
            int bit = __builtin_ctzll(any_delta);
            uint64_t bit_mask = 1ULL << bit;
            any_delta &= any_delta - 1;

            Interval delta(partial_l[static_cast<size_t>(bit)],
                           partial_u[static_cast<size_t>(bit)]);
            if (base_gate & bit_mask) {
                expand_interval(res.sa0, delta);
                include_transition(true, delta);
            } else {
                expand_interval(res.sa1, delta);
                include_transition(false, delta);
            }
        }
    }

    if (tr) {
        tr->sa0 = transition_union[0].value_or(Interval());
        tr->sa1 = transition_union[1].value_or(Interval());
    }

    for (int touched : ws.touched_nodes)
        ws.slot_map[touched] = -1;

    return res;
}

static bool find_best_cut_group(
    const AigNetlist& net, const WindowBlock& window, int max_cut_inputs,
    OverlapRunContext::WindowBuildScratch& scratch,
    const std::vector<std::pair<int, int>>& fanin_pairs) {
    auto pair_contains = [](const std::pair<int, int>& fanins, int gate) {
        return gate != 0 && (fanins.first == gate || fanins.second == gate);
    };

    scratch.cut_gates.clear();
    for (int cut_input : window.pis) {
        if (window.has_gate(cut_input) ||
            net.nodes[cut_input].type != NodeType::AND)
            continue;
        scratch.cut_gates.push_back(cut_input);
    }

    scratch.cut_visited.assign(scratch.cut_gates.size(), 0);
    scratch.cut_best_group.clear();
    int best_delta = 0;
    bool best_found = false;

    auto related = [&](int lhs_index, int rhs_index) {
        int lhs_gate = scratch.cut_gates[lhs_index];
        int rhs_gate = scratch.cut_gates[rhs_index];
        const auto& lhs_fanins = fanin_pairs[lhs_gate];
        const auto& rhs_fanins = fanin_pairs[rhs_gate];
        return lhs_fanins == rhs_fanins ||
               pair_contains(lhs_fanins, rhs_gate) ||
               pair_contains(rhs_fanins, lhs_gate);
    };

    for (size_t start = 0; start < scratch.cut_gates.size(); ++start) {
        if (scratch.cut_visited[start])
            continue;

        scratch.cut_component.clear();
        scratch.cut_component.push_back(static_cast<int>(start));
        scratch.cut_visited[start] = 1;
        for (size_t head = 0; head < scratch.cut_component.size(); ++head) {
            int member = scratch.cut_component[head];
            for (size_t other = 0; other < scratch.cut_gates.size(); ++other) {
                if (scratch.cut_visited[other] ||
                    !related(member, static_cast<int>(other)))
                    continue;
                scratch.cut_visited[other] = 1;
                scratch.cut_component.push_back(static_cast<int>(other));
            }
        }

        scratch.cut_group.clear();
        for (int index : scratch.cut_component)
            scratch.cut_group.push_back(scratch.cut_gates[index]);
        std::sort(scratch.cut_group.begin(), scratch.cut_group.end());

        scratch.cut_new_inputs.clear();
        auto add_new_input = [&](int input) {
            if (input == 0 || window.has_gate(input) ||
                window.pis.count(input))
                return;
            if (std::find(scratch.cut_new_inputs.begin(),
                          scratch.cut_new_inputs.end(),
                          input) == scratch.cut_new_inputs.end())
                scratch.cut_new_inputs.push_back(input);
        };
        for (int gate : scratch.cut_group) {
            const auto& fanins = fanin_pairs[gate];
            add_new_input(fanins.first);
            add_new_input(fanins.second);
        }

        int delta = static_cast<int>(scratch.cut_new_inputs.size()) -
                    static_cast<int>(scratch.cut_group.size());
        if (static_cast<int>(window.pis.size()) + delta > max_cut_inputs)
            continue;
        if (!best_found || delta < best_delta ||
            (delta == best_delta &&
             scratch.cut_group.size() > scratch.cut_best_group.size()) ||
            (delta == best_delta &&
             scratch.cut_group.size() == scratch.cut_best_group.size() &&
             scratch.cut_group.back() > scratch.cut_best_group.back())) {
            best_found = true;
            best_delta = delta;
            scratch.cut_best_group = scratch.cut_group;
        }
    }
    return best_found;
}

struct WindowBuildState {
    enum class SetKind {
        CutInput,
        Boundary
    };

    struct MembershipEdit {
        SetKind set;
        int node;
        bool restore_present;
    };

    struct Checkpoint {
        size_t gate_count;
        size_t affected_count;
        size_t edit_count;
        size_t cut_input_count;
        size_t boundary_count;
        bool cut_complete;
    };

    AigNetlist& net;
    AnalysisWorkspace& analysis;
    const std::vector<std::pair<int, int>>& fanin_pairs;
    int target_gate;
    int max_cut_inputs;
    OverlapRunContext::WindowBuildScratch& scratch;
    WindowBlock window;
    std::vector<MembershipEdit> edits;

    WindowBuildState(AigNetlist& netlist, AnalysisWorkspace& analysis_workspace,
                     const std::vector<std::pair<int, int>>& cached_fanins,
                     int target, int max_inputs,
                     OverlapRunContext::WindowBuildScratch& build_scratch)
        : net(netlist),
          analysis(analysis_workspace),
          fanin_pairs(cached_fanins),
          target_gate(target),
          max_cut_inputs(max_inputs),
          scratch(build_scratch) {
        reset_epoch(scratch.affected_tags, scratch.affected_epoch);
        scratch.affected_nodes.clear();
        scratch.pending_nodes.clear();
        scratch.frontier.clear();
        scratch.side_candidates.clear();
    }

    void reset_epoch(std::vector<uint32_t>& tags, uint32_t& epoch) {
        if (tags.size() < net.nodes.size())
            tags.resize(net.nodes.size(), 0);
        if (++epoch == 0) {
            std::fill(tags.begin(), tags.end(), 0);
            epoch = 1;
        }
    }

    bool is_affected(int node) const {
        return node > 0 && node < static_cast<int>(scratch.affected_tags.size()) &&
               scratch.affected_tags[node] == scratch.affected_epoch;
    }

    void change_membership(SetKind kind, int node, bool present) {
        auto& members = kind == SetKind::CutInput ? window.pis : window.pos;
        bool old_present = members.count(node) != 0;
        if (old_present == present)
            return;
        edits.push_back(MembershipEdit{kind, node, old_present});
        if (present)
            members.insert(node);
        else
            members.erase(node);
    }

    void add_cut_input(int node) {
        if (node != 0 && !window.has_gate(node))
            change_membership(SetKind::CutInput, node, true);
    }

    void add_cut_input_from_literal(int literal) {
        add_cut_input(gate_id_from_fanin(literal));
    }

    void remove_cut_input(int node) {
        change_membership(SetKind::CutInput, node, false);
    }

    bool add_boundary_node(int node) {
        if (node <= 0 || node >= static_cast<int>(net.nodes.size()))
            return false;

        const Node& boundary = net.nodes[node];
        if (boundary.type == NodeType::OUTPUT) {
            if (!window.pos.count(node)) {
                change_membership(SetKind::Boundary, node, true);
                add_cut_input_from_literal(boundary.fanin0);
            }
            return true;
        }
        if (boundary.type != NodeType::AND || !analysis.node_force_finalized[node])
            return false;

        if (!window.pos.count(node)) {
            change_membership(SetKind::Boundary, node, true);
            add_cut_input_from_literal(boundary.fanin0);
            add_cut_input_from_literal(boundary.fanin1);
        }
        return true;
    }

    void remove_boundary_node(int node) {
        change_membership(SetKind::Boundary, node, false);
    }

    void mark_affected(int node) {
        if (is_affected(node))
            return;
        scratch.affected_tags[node] = scratch.affected_epoch;
        scratch.affected_nodes.push_back(node);
        scratch.pending_nodes.push_back(node);
    }

    bool grow_affected_region() {
        while (!scratch.pending_nodes.empty()) {
            int current = scratch.pending_nodes.back();
            scratch.pending_nodes.pop_back();
            for (int fanout : net.nodes[current].fanouts) {
                if (fanout <= 0 || fanout >= static_cast<int>(net.nodes.size()))
                    return false;
                if (window.has_gate(fanout)) {
                    mark_affected(fanout);
                } else if (!add_boundary_node(fanout)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool add_internal_gate(int node, bool force_affected = false) {
        if (node <= 0 || node >= static_cast<int>(net.nodes.size()) ||
            net.nodes[node].type != NodeType::AND || window.has_gate(node))
            return false;

        bool was_boundary = window.pos.count(node) != 0;
        remove_boundary_node(node);
        remove_cut_input(node);
        if (!window.add_gate(node))
            return false;

        const Node& gate = net.nodes[node];
        add_cut_input_from_literal(gate.fanin0);
        add_cut_input_from_literal(gate.fanin1);

        int fanin0 = gate_id_from_fanin(gate.fanin0);
        int fanin1 = gate_id_from_fanin(gate.fanin1);
        if (force_affected || was_boundary || is_affected(fanin0) ||
            is_affected(fanin1)) {
            mark_affected(node);
            if (!grow_affected_region())
                return false;
        }
        return true;
    }

    bool complete_window_cut() {
        while (find_best_cut_group(net, window, max_cut_inputs, scratch, fanin_pairs)) {
            for (int gate : scratch.cut_best_group)
                if (!add_internal_gate(gate))
                    return false;
        }
        window.backward_legalized_fixed_point = true;
        return true;
    }

    Checkpoint checkpoint() const {
        return Checkpoint{window.gates.size(), scratch.affected_nodes.size(),
                          edits.size(), window.pis.size(), window.pos.size(),
                          window.backward_legalized_fixed_point};
    }

    void rollback(const Checkpoint& saved) {
        while (window.gates.size() > saved.gate_count) {
            window.gates_set.erase(window.gates.back());
            window.gates.pop_back();
        }
        while (scratch.affected_nodes.size() > saved.affected_count) {
            scratch.affected_tags[scratch.affected_nodes.back()] = 0;
            scratch.affected_nodes.pop_back();
        }
        for (size_t i = edits.size(); i > saved.edit_count; --i) {
            const MembershipEdit& edit = edits[i - 1];
            auto& members =
                edit.set == SetKind::CutInput ? window.pis : window.pos;
            if (edit.restore_present)
                members.insert(edit.node);
            else
                members.erase(edit.node);
        }
        edits.resize(saved.edit_count);
        scratch.pending_nodes.clear();
        window.backward_legalized_fixed_point = saved.cut_complete;
        if (window.pis.size() != saved.cut_input_count ||
            window.pos.size() != saved.boundary_count)
            throw std::logic_error(
                "MEOW: window rollback changed set cardinality for gate " +
                std::to_string(target_gate) + " (cut_inputs=" +
                std::to_string(saved.cut_input_count) + "->" +
                std::to_string(window.pis.size()) + ", boundary=" +
                std::to_string(saved.boundary_count) + "->" +
                std::to_string(window.pos.size()) + ")");
    }

    void commit() {
        edits.clear();
        scratch.pending_nodes.clear();
    }

    bool try_extend_window(int node, bool complete_cut_each_step) {
        Checkpoint saved = checkpoint();
        if (!add_internal_gate(node) ||
            (complete_cut_each_step && !complete_window_cut()) ||
            window.pos.empty() ||
            window.pis.size() > static_cast<size_t>(max_cut_inputs)) {
            rollback(saved);
            return false;
        }
        commit();
        return true;
    }

    const std::vector<int>& gather_frontier(bool complete_cut_each_step,
                                            std::mt19937& rng) {
        reset_epoch(scratch.frontier_tags, scratch.frontier_epoch);
        scratch.frontier.clear();
        scratch.side_candidates.clear();

        auto add_once = [&](std::vector<int>& target, int node) {
            if (node <= 0 || node >= static_cast<int>(net.nodes.size()) ||
                scratch.frontier_tags[node] == scratch.frontier_epoch)
                return;
            scratch.frontier_tags[node] = scratch.frontier_epoch;
            target.push_back(node);
        };

        for (int boundary : window.pos)
            if (net.nodes[boundary].type == NodeType::AND &&
                !window.has_gate(boundary))
                add_once(scratch.frontier, boundary);

        if (!complete_cut_each_step) {
            for (int cut_input : window.pis)
                if (cut_input > 0 &&
                    cut_input < static_cast<int>(net.nodes.size()) &&
                    net.nodes[cut_input].type == NodeType::AND &&
                    !window.has_gate(cut_input))
                    add_once(scratch.side_candidates, cut_input);

            if (scratch.side_candidates.size() > 4) {
                std::shuffle(scratch.side_candidates.begin(),
                             scratch.side_candidates.end(), rng);
                scratch.side_candidates.resize(4);
            }
            scratch.frontier.insert(scratch.frontier.end(),
                                    scratch.side_candidates.begin(),
                                    scratch.side_candidates.end());
        }

        std::shuffle(scratch.frontier.begin(), scratch.frontier.end(), rng);
        return scratch.frontier;
    }

};



static WindowBlock build_random_window(OverlapRunContext& run_ctx,
                                       int target_gate, int max_cut_inputs,
                                       bool complete_cut_each_step) {
    WindowBuildState state(run_ctx.net, run_ctx.analysis, run_ctx.fanin_pairs,
                           target_gate, max_cut_inputs,
                           run_ctx.window_scratch);
    if (!state.add_internal_gate(target_gate, true))
        throw std::logic_error("MEOW: cannot seed random window");
    if (complete_cut_each_step) {
        WindowBuildState::Checkpoint before_completion = state.checkpoint();
        if (!state.complete_window_cut() ||
            state.window.pos.empty() ||
            state.window.pis.size() > static_cast<size_t>(max_cut_inputs))
            state.rollback(before_completion);
    }
    if (state.window.pos.empty() ||
        state.window.pis.size() > static_cast<size_t>(max_cut_inputs))
        return WindowBlock();
    state.commit();

    while (true) {
        const std::vector<int>& frontier =
            state.gather_frontier(complete_cut_each_step, run_ctx.rng);
        bool extended = false;
        for (int node : frontier) {
            if (state.try_extend_window(node, complete_cut_each_step)) {
                extended = true;
                break;
            }
        }
        if (!extended)
            break;
    }

    if (!complete_cut_each_step) {
        WindowBuildState::Checkpoint before_completion = state.checkpoint();
        if (!state.complete_window_cut() ||
            state.window.pos.empty() ||
            state.window.pis.size() > static_cast<size_t>(max_cut_inputs)) {
            state.rollback(before_completion);
        } else {
            state.commit();
        }
    }
    if (state.window.pos.empty() ||
        state.window.pis.size() > static_cast<size_t>(max_cut_inputs))
        throw std::logic_error(
            "MEOW: invalid random window for gate " +
            std::to_string(target_gate) + " (cut_inputs=" +
            std::to_string(state.window.pis.size()) + ", boundary=" +
            std::to_string(state.window.pos.size()) + ", fixed=" +
            std::to_string(state.window.backward_legalized_fixed_point) +
            ", stepwise=" + std::to_string(complete_cut_each_step) + ")");
    state.commit();
    return std::move(state.window);
}

static void complete_window_cut_backward(OverlapRunContext& run_ctx,
                                         WindowBlock& block,
                                         int max_cut_inputs) {
    while (find_best_cut_group(run_ctx.net, block, max_cut_inputs,
                               run_ctx.window_scratch, run_ctx.fanin_pairs)) {
        for (int gate : run_ctx.window_scratch.cut_best_group)
            block.add_gate(gate);
        for (int gate : run_ctx.window_scratch.cut_best_group) {
            const Node& node = run_ctx.net.nodes[gate];
            block.add_pi_from_fanin(node.fanin0);
            block.add_pi_from_fanin(node.fanin1);
        }
    }
    block.backward_legalized_fixed_point = true;
}

WindowBlock build_full_tfo_window(AigNetlist& net, int gid, int mpi, int mg) {
    return build_full_tfo_window_from_mask(net, gid, net.forward_output_masks[gid], mpi, mg);
}

bool run_full_tfo_window_method(OverlapRunContext& rctx, OverlapGateContext& gctx) {
    WindowBlock b = build_full_tfo_window(rctx.net, gctx.gate_id, rctx.config.window_pi_limit,
                                          kFullTfoMaxGates);
    if (b.pos.empty()) return false;
    complete_window_cut_backward(rctx, b, rctx.config.window_pi_limit);
    std::sort(b.gates.begin(), b.gates.end());
    WindowSimResult s;
    s.force = simulate_window_block_with_workspace(
        rctx.net, b, gctx.gate_id, rctx.node_transition_intervals,
        &s.transition, rctx.workspace);
    apply_family_result(rctx, gctx, s);
    return true;
}

void run_init_interval_method(OverlapRunContext& rctx, OverlapGateContext& gctx) {
    apply_family_result(rctx, gctx, build_init_interval_result(rctx, gctx));
}

bool has_threshold_relevant_direction(const OverlapRunContext& rctx,
                                      const OverlapGateContext& gctx) {
    if (!rctx.config.useful_error_threshold)
        return true;
    const Weight& threshold = *rctx.config.useful_error_threshold;
    return interval_abs_bound(gctx.best_force.sa0) <= threshold ||
           interval_abs_bound(gctx.best_force.sa1) <= threshold;
}

void finalize_gate_results(OverlapRunContext& rctx, OverlapGateContext& gctx) {
    ForcedValueIntervals& fv = rctx.analysis.node_force_intervals[gctx.gate_id];
    fv = gctx.best_force;
    ForcedValueIntervals& tv = rctx.node_transition_intervals[gctx.gate_id];
    tv = gctx.best_transition;

}

} // namespace als::meow

namespace als {

AnalysisResult meow::analyze(AigNetlist& net, const AnalysisConfig& config) {
    if (config.threads < 1)
        throw std::invalid_argument("MEOW thread count must be positive");
    if (config.window_pi_limit < 1 || config.window_pi_limit > 16)
        throw std::invalid_argument("MEOW window PI limit must be in [1, 16]");
    constexpr int random_window_count = AnalysisConfig::random_window_count;
    omp_set_dynamic(0);

    auto& nodes = net.nodes;
    const auto& outputs = net.outputs;
    const int max_var = net.max_var;
    AnalysisWorkspace analysis(nodes.size());
    auto& node_force_intervals = analysis.node_force_intervals;
    auto& node_force_finalized = analysis.node_force_finalized;
    std::vector<std::pair<int, int>> fanin_pairs(nodes.size(), {0, 0});

    SignedWeight gcap = signed_error_cap(outputs.size());
    std::vector<ForcedValueIntervals> nti(nodes.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
        int oid = outputs[i];
        SignedWeight w = signed_output_weight(i);
        node_force_intervals[oid] = ForcedValueIntervals{Interval(-w, -w), Interval(w, w)};
        nti[oid] = node_force_intervals[oid];
        node_force_finalized[oid] = true;
    }
    net.compute_forward_output_reachability();
    for (int gate = 1; gate <= max_var; ++gate)
        if (nodes[gate].type == NodeType::AND)
            fanin_pairs[gate] = meow::ordered_fanin_gates(nodes[gate]);

    std::vector<std::atomic<int>> pending_fanouts(nodes.size());
    std::vector<int> ready_gates;
    int total_gates = 0;
    for (int g = 1; g <= max_var; ++g) {
        pending_fanouts[g].store(0, std::memory_order_relaxed);
        if (nodes[g].type != NodeType::AND)
            continue;
        ++total_gates;
        int pending = 0;
        for (int fo : nodes[g].fanouts)
            if (fo > 0 && fo < static_cast<int>(nodes.size()) &&
                nodes[fo].type == NodeType::AND)
                ++pending;
        pending_fanouts[g].store(pending, std::memory_order_relaxed);
        if (pending == 0)
            ready_gates.push_back(g);
    }

    std::sort(ready_gates.begin(), ready_gates.end(), std::greater<int>());
    std::vector<meow::MeowThreadState> thread_states(
        static_cast<size_t>(config.threads));
    std::atomic<int> completed_gates{0};
    std::atomic<bool> task_failed{false};
    std::exception_ptr task_failure;
    std::mutex failure_mutex;

    auto capture_failure = [&](std::exception_ptr failure) {
        std::lock_guard<std::mutex> lock(failure_mutex);
        if (!task_failure)
            task_failure = std::move(failure);
        task_failed.store(true, std::memory_order_release);
    };

    std::function<void(int)> enqueue_gate;
    enqueue_gate = [&](int gate) {
#pragma omp task default(shared) firstprivate(gate)
        {
            if (!task_failed.load(std::memory_order_acquire)) {
                try {
                    meow::OverlapGateContext gate_ctx(
                        gate, meow::reachable_output_weight_sum(net, gate));
                    bool needs_random = false;

                    if (nodes[gate].fanouts.empty()) {
                        node_force_intervals[gate] = ForcedValueIntervals{};
                        nti[gate] = ForcedValueIntervals{};
                    } else {
                        {
                            meow::MeowThreadState& state = thread_states[
                                static_cast<std::size_t>(omp_get_thread_num())];
                            meow::OverlapRunContext run_ctx{
                                net, analysis, config, gcap, nti, fanin_pairs,
                                state.rng, state.workspace, state.window_scratch
                            };
                            if (meow::run_full_tfo_window_method(
                                    run_ctx, gate_ctx)) {
                                meow::finalize_gate_results(run_ctx, gate_ctx);
                            } else {
                                meow::run_init_interval_method(run_ctx, gate_ctx);
                                needs_random = meow::has_threshold_relevant_direction(
                                        run_ctx, gate_ctx);
                                if (!needs_random) {
                                    meow::finalize_gate_results(run_ctx, gate_ctx);
                                }
                            }
                        }

                        if (needs_random) {
                            std::vector<meow::WindowBlock> windows(
                                static_cast<std::size_t>(random_window_count));
                            std::vector<meow::WindowSimResult> results(
                                static_cast<std::size_t>(random_window_count));
                            std::vector<std::size_t> canonical_slot(
                                static_cast<std::size_t>(random_window_count),
                                meow::kInvalidWindowSlot);

#pragma omp taskgroup
                            {
                                for (int window_index = 0;
                                     window_index < random_window_count;
                                     ++window_index) {
#pragma omp task default(shared) firstprivate(gate, window_index)
                                    {
                                        if (!task_failed.load(
                                                std::memory_order_acquire)) {
                                            try {
                                                meow::MeowThreadState& state =
                                                    thread_states[static_cast<std::size_t>(
                                                        omp_get_thread_num())];
                                                state.rng.seed(
                                                    meow::window_rng_seed(
                                                        config.random_seed,
                                                        gate, window_index));
                                                meow::OverlapRunContext run_ctx{
                                                    net, analysis, config, gcap,
                                                    nti, fanin_pairs, state.rng,
                                                    state.workspace,
                                                    state.window_scratch
                                                };
                                                meow::WindowBlock block =
                                                    meow::build_random_window(
                                                        run_ctx, gate,
                                                        config.window_pi_limit,
                                                        (window_index % 2) == 1);
                                                std::sort(block.gates.begin(),
                                                          block.gates.end());
                                                windows[static_cast<std::size_t>(
                                                    window_index)] =
                                                    std::move(block);
                                            } catch (...) {
                                                capture_failure(
                                                    std::current_exception());
                                            }
                                        }
                                    }
                                }
                            }

                            if (!task_failed.load(std::memory_order_acquire)) {
                                std::vector<meow::WindowKey> keys(
                                    static_cast<std::size_t>(
                                        random_window_count));
                                for (int window_index = 0;
                                     window_index < random_window_count;
                                     ++window_index) {
                                    const std::size_t slot =
                                        static_cast<std::size_t>(window_index);
                                    if (windows[slot].pos.empty())
                                        continue;
                                    keys[slot] = meow::make_window_key(
                                        gate_ctx, windows[slot]);
                                    canonical_slot[slot] = slot;
                                    for (int previous = 0;
                                         previous < window_index; ++previous) {
                                        const std::size_t previous_slot =
                                            static_cast<std::size_t>(previous);
                                        if (canonical_slot[previous_slot] ==
                                            meow::kInvalidWindowSlot)
                                            continue;
                                        if (keys[slot] == keys[previous_slot]) {
                                            canonical_slot[slot] =
                                                canonical_slot[previous_slot];
                                            break;
                                        }
                                    }
                                }

#pragma omp taskgroup
                                {
                                    for (int window_index = 0;
                                         window_index < random_window_count;
                                         ++window_index) {
                                        const std::size_t slot =
                                            static_cast<std::size_t>(
                                                window_index);
                                        if (canonical_slot[slot] != slot)
                                            continue;
#pragma omp task default(shared) firstprivate(gate, slot)
                                        {
                                            if (!task_failed.load(
                                                    std::memory_order_acquire)) {
                                                try {
                                                    meow::MeowThreadState& state =
                                                        thread_states[static_cast<std::size_t>(
                                                            omp_get_thread_num())];
                                                    meow::WindowSimResult simulation;
                                                    simulation.force =
                                                        meow::simulate_window_block_with_workspace(
                                                            net, windows[slot],
                                                            gate, nti,
                                                            &simulation.transition,
                                                            state.workspace);
                                                    results[slot] =
                                                        std::move(simulation);
                                                } catch (...) {
                                                    capture_failure(
                                                        std::current_exception());
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (!task_failed.load(std::memory_order_acquire)) {
                                meow::MeowThreadState& state = thread_states[
                                    static_cast<std::size_t>(
                                        omp_get_thread_num())];
                                meow::OverlapRunContext run_ctx{
                                    net, analysis, config, gcap, nti,
                                    fanin_pairs, state.rng, state.workspace,
                                    state.window_scratch
                                };
                                for (int window_index = 0;
                                     window_index < random_window_count;
                                     ++window_index) {
                                    const std::size_t slot =
                                        static_cast<std::size_t>(window_index);
                                    if (canonical_slot[slot] ==
                                        meow::kInvalidWindowSlot)
                                        continue;
                                    meow::apply_family_result(
                                        run_ctx, gate_ctx,
                                        results[canonical_slot[slot]]);
                                }
                                if (!gate_ctx.block_evaluated)
                                    throw std::runtime_error(
                                        "MEOW failed for gate " +
                                        std::to_string(gate));
                                meow::finalize_gate_results(run_ctx, gate_ctx);
                            }
                        }
                    }

                    if (!task_failed.load(std::memory_order_acquire)) {
                        node_force_finalized[gate] = 1;
                        completed_gates.fetch_add(1, std::memory_order_relaxed);

                        auto release_fanin = [&](int literal) {
                            const int fanin = lit_to_var(literal);
                            if (fanin <= 0 ||
                                fanin >= static_cast<int>(nodes.size()) ||
                                nodes[fanin].type != NodeType::AND)
                                return;
                            const int previous = pending_fanouts[fanin].fetch_sub(
                                1, std::memory_order_acq_rel);
                            if (previous <= 0)
                                throw std::runtime_error(
                                    "MEOW schedule underflow gate " +
                                    std::to_string(fanin));
                            if (previous == 1)
                                enqueue_gate(fanin);
                        };
                        release_fanin(nodes[gate].fanin0);
                        release_fanin(nodes[gate].fanin1);
                    }
                } catch (...) {
                    capture_failure(std::current_exception());
                }
            }
        }
    };

#pragma omp parallel num_threads(config.threads)
    {
#pragma omp single
        {
#pragma omp taskgroup
            {
                for (int gate : ready_gates)
                    enqueue_gate(gate);
            }
        }
    }

    if (task_failure)
        std::rethrow_exception(task_failure);

    const int completed = completed_gates.load(std::memory_order_relaxed);
    if (completed != total_gates)
        throw std::runtime_error(
            "MEOW schedule stalled: " +
            std::to_string(completed) + " of " +
            std::to_string(total_gates));
    return collect_interval_results(net, analysis);
}

} // namespace als
