#pragma once

#include "analysis.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <utility>
#include <vector>

namespace als::meow {

struct WindowBlock {
    std::vector<int> gates;
    std::unordered_set<int> gates_set;
    std::unordered_set<int> pis;
    std::unordered_set<int> pos;
    bool backward_legalized_fixed_point = false;

    bool has_gate(int gate_id) const { return gates_set.count(gate_id) != 0; }
    bool add_gate(int gate_id) {
        if (gate_id == 0 || has_gate(gate_id)) return false;
        backward_legalized_fixed_point = false;
        gates.push_back(gate_id);
        gates_set.insert(gate_id);
        pis.erase(gate_id);
        return true;
    }
    void reset_gates(std::vector<int> gate_ids) {
        gates.clear();
        gates_set.clear();
        for (int gate_id : gate_ids) add_gate(gate_id);
    }
    bool try_add_pi(int gate_id, int max_pi_size) {
        backward_legalized_fixed_point = false;
        if (gate_id == 0 || has_gate(gate_id) || pis.count(gate_id)) return true;
        if (static_cast<int>(pis.size()) >= max_pi_size) return false;
        pis.insert(gate_id);
        return true;
    }
    void add_pi_from_fanin(int literal) {
        int gate_id = lit_to_var(literal);
        if (gate_id != 0 && !has_gate(gate_id)) pis.insert(gate_id);
    }
    bool try_add_pi_from_fanin(int literal, int max_pi_size) {
        return try_add_pi(lit_to_var(literal), max_pi_size);
    }
};

struct WindowKey {
    int gate_id = 0;
    std::vector<int> gates;
    std::vector<int> pis;
    std::vector<int> pos;
    bool operator==(const WindowKey& other) const {
        return gate_id == other.gate_id && gates == other.gates &&
               pis == other.pis && pos == other.pos;
    }
};

struct WindowSimResult {
    ForcedValueIntervals force;
    ForcedValueIntervals transition;
};

inline constexpr int kFullTfoMaxGates = 2147483647;

struct OverlapGateContext {
    OverlapGateContext(int id, SignedWeight cap)
        : gate_id(id), reachable_output_cap(std::move(cap)) {}
    int gate_id = 0;
    SignedWeight reachable_output_cap{};
    ForcedValueIntervals best_force = ForcedValueIntervals{};
    ForcedValueIntervals best_transition = ForcedValueIntervals{};
    bool block_evaluated = false;
};

struct OverlapRunContext {
    struct SimulationWorkspace {
        std::vector<uint64_t> arena;
        std::vector<int> slot_map;
        std::vector<int> touched_nodes;
        std::vector<uint64_t> const_zero_buf;
        std::vector<int> output_list;
        std::vector<int> input_list;
        std::vector<int> gate_list;
        std::vector<uint64_t> base_gate_values;
        std::vector<std::uint8_t> affected;
        std::vector<std::uint8_t> output_is_affected;
        std::vector<uint64_t> base_outputs;
        std::vector<uint64_t> forced_outputs;
        std::vector<ForcedValueIntervals> per_output_bounds;
    };
    struct WindowBuildScratch {
        std::vector<uint32_t> affected_tags;
        std::vector<uint32_t> frontier_tags;
        std::vector<int> affected_nodes;
        std::vector<int> pending_nodes;
        std::vector<int> frontier;
        std::vector<int> side_candidates;
        std::vector<int> cut_gates;
        std::vector<uint8_t> cut_visited;
        std::vector<int> cut_component;
        std::vector<int> cut_group;
        std::vector<int> cut_new_inputs;
        std::vector<int> cut_best_group;
        uint32_t affected_epoch = 0;
        uint32_t frontier_epoch = 0;
    };

    AigNetlist& net;
    AnalysisWorkspace& analysis;
    const AnalysisConfig& config;
    SignedWeight global_error_cap{};
    std::vector<ForcedValueIntervals>& node_transition_intervals;
    std::vector<std::pair<int, int>>& fanin_pairs;
    std::mt19937& rng;
    SimulationWorkspace& workspace;
    WindowBuildScratch& window_scratch;
};

AnalysisResult analyze(AigNetlist& net, const AnalysisConfig& config);

} // namespace als::meow
