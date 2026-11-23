#include "netlist_transform.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace als {
namespace {

void make_tombstone(Node& node) {
    node.type = NodeType::CONST0;
    node.fanin0 = 0;
    node.fanin1 = 0;
    node.fanouts.clear();
}

int simplified_and(int lhs, int rhs) {
    if (lhs == 0 || rhs == 0) return 0;
    if (lhs == 1) return rhs;
    if (rhs == 1) return lhs;
    if (lhs == rhs) return lhs;
    if ((lhs ^ 1) == rhs) return 0;
    return -1;
}

void ensure_scratch_size(MutationScratch& scratch, std::size_t size) {
    scratch.alias_stamp.resize(size, 0);
    scratch.alias_literal.resize(size, 0);
    scratch.scheduled_stamp.resize(size, 0);
    scratch.ref_stamp.resize(size, 0);
    scratch.ref_delta.resize(size, 0);
    scratch.edit_stamp.resize(size, 0);
    scratch.edit_index.resize(size, -1);
    scratch.dangling_stamp.resize(size, 0);
}

void begin_plan(MutationScratch& scratch, std::size_t size) {
    ensure_scratch_size(scratch, size);
    if (++scratch.generation == 0) {
        std::fill(scratch.alias_stamp.begin(), scratch.alias_stamp.end(), 0);
        std::fill(scratch.scheduled_stamp.begin(), scratch.scheduled_stamp.end(), 0);
        std::fill(scratch.ref_stamp.begin(), scratch.ref_stamp.end(), 0);
        std::fill(scratch.edit_stamp.begin(), scratch.edit_stamp.end(), 0);
        std::fill(scratch.dangling_stamp.begin(), scratch.dangling_stamp.end(), 0);
        scratch.generation = 1;
    }
    scratch.heap.clear();
    scratch.ref_touched.clear();
    scratch.node_edits.clear();
    scratch.output_edits.clear();
}

void heap_push(MutationScratch& scratch, int node_id) {
    scratch.heap.push_back(node_id);
    std::push_heap(scratch.heap.begin(), scratch.heap.end(), std::greater<int>());
}

int heap_pop(MutationScratch& scratch) {
    std::pop_heap(scratch.heap.begin(), scratch.heap.end(), std::greater<int>());
    const int result = scratch.heap.back();
    scratch.heap.pop_back();
    return result;
}

void schedule(MutationScratch& scratch, int node_id) {
    if (scratch.scheduled_stamp[node_id] == scratch.generation) return;
    scratch.scheduled_stamp[node_id] = scratch.generation;
    heap_push(scratch, node_id);
}

int resolve_literal(const MutationScratch& scratch, int literal) {
    const int var = lit_to_var(literal);
    if (var == 0 || scratch.alias_stamp[var] != scratch.generation) return literal;
    return scratch.alias_literal[var] ^ static_cast<int>(lit_is_inv(literal));
}

NodeMutation& set_node_edit(MutationScratch& scratch, int node_id, int fanin0,
                            int fanin1, bool tombstone) {
    if (scratch.edit_stamp[node_id] == scratch.generation) {
        NodeMutation& edit = scratch.node_edits[scratch.edit_index[node_id]];
        edit.fanin0 = fanin0;
        edit.fanin1 = fanin1;
        edit.tombstone = tombstone;
        return edit;
    }
    scratch.edit_stamp[node_id] = scratch.generation;
    scratch.edit_index[node_id] = static_cast<int>(scratch.node_edits.size());
    scratch.node_edits.push_back({node_id, fanin0, fanin1, tombstone});
    return scratch.node_edits.back();
}

void adjust_reference(MutationScratch& scratch, int literal, int amount) {
    const int node_id = lit_to_var(literal);
    if (node_id == 0) return;
    if (scratch.ref_stamp[node_id] != scratch.generation) {
        scratch.ref_stamp[node_id] = scratch.generation;
        scratch.ref_delta[node_id] = 0;
        scratch.ref_touched.push_back(node_id);
    }
    scratch.ref_delta[node_id] += amount;
}

int effective_references(const AigNetlist& net, const MutationScratch& scratch,
                         int node_id) {
    const int delta = scratch.ref_stamp[node_id] == scratch.generation
                          ? scratch.ref_delta[node_id]
                          : 0;
    return static_cast<int>(net.nodes[node_id].fanouts.size()) + delta;
}

RemovalCounts build_plan(const AigNetlist& net, const Replacement& replacement,
                         MutationScratch& scratch) {
    const int target_id = replacement.target_gate;
    const int replacement_literal = replacement.replacement_literal;
    if (target_id <= 0 || target_id > net.max_var ||
        net.nodes[target_id].type != NodeType::AND)
        throw std::invalid_argument("replacement target is not a live AND gate");
    const int replacement_var = lit_to_var(replacement_literal);
    if (replacement_var < 0 || replacement_var >= target_id ||
        (replacement_var > 0 &&
         net.nodes[replacement_var].type != NodeType::INPUT &&
         net.nodes[replacement_var].type != NodeType::AND))
        throw std::invalid_argument(
            "replacement literal must be a constant or a live topological predecessor");

    begin_plan(scratch, net.nodes.size());
    scratch.alias_stamp[target_id] = scratch.generation;
    scratch.alias_literal[target_id] = replacement_literal;
    set_node_edit(scratch, target_id, 0, 0, true);
    for (int fanout : net.nodes[target_id].fanouts) schedule(scratch, fanout);

    while (!scratch.heap.empty()) {
        const int node_id = heap_pop(scratch);
        const Node& node = net.nodes[node_id];
        if (node.type == NodeType::OUTPUT) {
            const int literal = resolve_literal(scratch, node.fanin0);
            if (literal != node.fanin0)
                scratch.output_edits.push_back({node_id, literal});
            continue;
        }
        if (node.type != NodeType::AND) continue;
        const int fanin0 = resolve_literal(scratch, node.fanin0);
        const int fanin1 = resolve_literal(scratch, node.fanin1);
        if (fanin0 == node.fanin0 && fanin1 == node.fanin1) continue;
        const int simplified = simplified_and(fanin0, fanin1);
        if (simplified < 0) {
            set_node_edit(scratch, node_id, fanin0, fanin1, false);
            continue;
        }
        scratch.alias_stamp[node_id] = scratch.generation;
        scratch.alias_literal[node_id] = simplified;
        set_node_edit(scratch, node_id, 0, 0, true);
        for (int fanout : node.fanouts) schedule(scratch, fanout);
    }

    // Derive effective reference counts from the planned edge rewrites.
    for (const NodeMutation& edit : scratch.node_edits) {
        const Node& original = net.nodes[edit.node_id];
        adjust_reference(scratch, original.fanin0, -1);
        adjust_reference(scratch, original.fanin1, -1);
        if (!edit.tombstone) {
            adjust_reference(scratch, edit.fanin0, 1);
            adjust_reference(scratch, edit.fanin1, 1);
        }
    }
    for (const OutputMutation& edit : scratch.output_edits) {
        adjust_reference(scratch, net.nodes[edit.output_id].fanin0, -1);
        adjust_reference(scratch, edit.literal, 1);
    }

    scratch.heap.clear();
    auto enqueue_dangling = [&](int node_id) {
        if (node_id <= 0 || node_id > net.max_var ||
            net.nodes[node_id].type != NodeType::AND ||
            (scratch.edit_stamp[node_id] == scratch.generation &&
             scratch.node_edits[scratch.edit_index[node_id]].tombstone) ||
            effective_references(net, scratch, node_id) != 0 ||
            scratch.dangling_stamp[node_id] == scratch.generation)
            return;
        scratch.dangling_stamp[node_id] = scratch.generation;
        heap_push(scratch, node_id);
    };
    for (int node_id : scratch.ref_touched) enqueue_dangling(node_id);

    while (!scratch.heap.empty()) {
        const int node_id = heap_pop(scratch);
        const Node& original = net.nodes[node_id];
        if (scratch.edit_stamp[node_id] == scratch.generation) {
            NodeMutation& edit = scratch.node_edits[scratch.edit_index[node_id]];
            if (edit.tombstone) continue;
            // Its old inputs were already detached and its rewritten inputs
            // attached above. Removing it only cancels the new attachments.
            adjust_reference(scratch, edit.fanin0, -1);
            adjust_reference(scratch, edit.fanin1, -1);
            const int input0 = lit_to_var(edit.fanin0);
            const int input1 = lit_to_var(edit.fanin1);
            edit.fanin0 = 0;
            edit.fanin1 = 0;
            edit.tombstone = true;
            enqueue_dangling(input0);
            enqueue_dangling(input1);
        } else {
            set_node_edit(scratch, node_id, 0, 0, true);
            adjust_reference(scratch, original.fanin0, -1);
            adjust_reference(scratch, original.fanin1, -1);
            enqueue_dangling(lit_to_var(original.fanin0));
            enqueue_dangling(lit_to_var(original.fanin1));
        }
    }

    const int removed = static_cast<int>(std::count_if(
        scratch.node_edits.begin(), scratch.node_edits.end(),
        [](const NodeMutation& edit) { return edit.tombstone; }));
    RemovalCounts counts;
    if (replacement_literal == 0 || replacement_literal == 1)
        counts.const_removed = removed;
    else
        counts.buffer_removed = removed;
    return counts;
}

void commit_plan(AigNetlist& net, const MutationScratch& scratch) {
    for (const NodeMutation& edit : scratch.node_edits) {
        Node& node = net.nodes[edit.node_id];
        if (edit.tombstone) {
            make_tombstone(node);
        } else {
            node.fanin0 = edit.fanin0;
            node.fanin1 = edit.fanin1;
        }
    }
    for (const OutputMutation& edit : scratch.output_edits)
        net.nodes[edit.output_id].fanin0 = edit.literal;
}

} // namespace

void apply_replacement(AigNetlist& net, const Replacement& replacement,
                       MutationScratch& scratch) {
    build_plan(net, replacement, scratch);
    commit_plan(net, scratch);
    net.rebuild_fanouts();
    validate_live_aig(net);
}

MutationPlan preview_replacement(const AigNetlist& net,
                                 const Replacement& replacement,
                                 MutationScratch& scratch) {
    MutationPlan plan;
    plan.removal = build_plan(net, replacement, scratch);
    plan.nodes = scratch.node_edits;
    plan.outputs = scratch.output_edits;
    return plan;
}

void validate_live_aig(const AigNetlist& net) {
    if (net.max_var < 0 || static_cast<std::size_t>(net.max_var) >= net.nodes.size())
        throw std::runtime_error("live AIG invariant failed: invalid maximum variable");
    for (int input : net.inputs)
        if (input <= 0 || input > net.max_var || net.nodes[input].type != NodeType::INPUT)
            throw std::runtime_error("live AIG invariant failed: invalid primary input");

    auto validate_literal = [&](int literal, int owner, const char* kind) {
        const int var = lit_to_var(literal);
        if (var == 0) return;
        if (var <= 0 || var > net.max_var ||
            (net.nodes[var].type != NodeType::INPUT &&
             net.nodes[var].type != NodeType::AND))
            throw std::runtime_error(
                std::string("live AIG invariant failed: ") + kind + " " +
                std::to_string(owner) + " references tombstone " + std::to_string(var));
        if (owner <= net.max_var && var >= owner)
            throw std::runtime_error(
                "live AIG invariant failed: non-topological fanin " +
                std::to_string(var) + " for gate " + std::to_string(owner));
    };

    for (int gate = 1; gate <= net.max_var; ++gate) {
        if (net.nodes[gate].type != NodeType::AND) continue;
        validate_literal(net.nodes[gate].fanin0, gate, "gate");
        validate_literal(net.nodes[gate].fanin1, gate, "gate");
    }
    for (int output : net.outputs) {
        if (output <= net.max_var || static_cast<std::size_t>(output) >= net.nodes.size() ||
            net.nodes[output].type != NodeType::OUTPUT)
            throw std::runtime_error("live AIG invariant failed: invalid primary output");
        validate_literal(net.nodes[output].fanin0, output, "output");
    }
}

} // namespace als
