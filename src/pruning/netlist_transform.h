#pragma once

#include "pruning.h"

#include <cstdint>
#include <vector>

namespace als {

struct NodeMutation {
    int node_id = 0;
    int fanin0 = 0;
    int fanin1 = 0;
    bool tombstone = false;
};

struct OutputMutation {
    int output_id = 0;
    int literal = 0;
};

struct MutationPlan {
    std::vector<NodeMutation> nodes;
    std::vector<OutputMutation> outputs;
    RemovalCounts removal;
};

// Reusable candidate-planning storage. Generation stamps make reset proportional
// to the affected cone instead of the size of the complete AIG.
struct MutationScratch {
    std::uint32_t generation = 0;
    std::vector<std::uint32_t> alias_stamp;
    std::vector<int> alias_literal;
    std::vector<std::uint32_t> scheduled_stamp;
    std::vector<std::uint32_t> ref_stamp;
    std::vector<int> ref_delta;
    std::vector<std::uint32_t> edit_stamp;
    std::vector<int> edit_index;
    std::vector<std::uint32_t> dangling_stamp;
    std::vector<int> heap;
    std::vector<int> ref_touched;
    std::vector<NodeMutation> node_edits;
    std::vector<OutputMutation> output_edits;
};

void apply_replacement(AigNetlist& net, const Replacement& replacement,
                       MutationScratch& scratch);
MutationPlan preview_replacement(const AigNetlist& net,
                                 const Replacement& replacement,
                                 MutationScratch& scratch);
void validate_live_aig(const AigNetlist& net);

} // namespace als
