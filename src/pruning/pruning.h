#pragma once
#include "analysis/analysis.h"
#include <string>

namespace als {
struct Replacement { int target_gate; int replacement_literal; };
struct RemovalCounts {
    int const_removed = 0, buffer_removed = 0;
    int total() const { return const_removed + buffer_removed; }
};
struct PruningConfig {
    int threads = 16;
    Weight threshold = Weight(3);
    double timeout_seconds = 1200.0;
};
struct PruningResult {
    AigNetlist net;
    int initial_gates = 0, final_gates = 0;
    Interval estimated_interval;
    bool search_complete = true;
    bool budget_exhausted = false;
    std::string termination_reason = "candidate-pool-empty";
};
PruningResult run_pruning(AigNetlist net, const PruningConfig& config);
} // namespace als
