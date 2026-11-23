#pragma once
#include "core/aig.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace als {
using Interval = WeightedInterval;
inline void expand_interval(Interval& target, const Interval& update) {
    expand_weighted_interval(target, update);
}
inline ForcedValueIntervals intersect_intervals(const ForcedValueIntervals& a,
                                                const ForcedValueIntervals& b) {
    return {intersect_weighted_interval(a.sa0, b.sa0),
            intersect_weighted_interval(a.sa1, b.sa1)};
}
struct AnalysisConfig {
    int threads = 16;
    int window_pi_limit = 12;
    static constexpr int random_window_count = 4;
    static constexpr std::uint32_t random_seed = 42;
    // Skip optional refinement only when neither direction can enter pruning.
    std::optional<Weight> useful_error_threshold;
};
struct GateEstimate {
    int gate_id;
    Interval sa0, sa1;
};
struct AnalysisResult {
    std::vector<GateEstimate> gates;
};
struct AnalysisWorkspace {
    explicit AnalysisWorkspace(std::size_t size)
        : node_force_intervals(size), node_force_finalized(size, 0) {}
    std::vector<ForcedValueIntervals> node_force_intervals;
    // Distinct tasks must not share packed vector<bool> storage.
    std::vector<std::uint8_t> node_force_finalized;
};
AnalysisResult collect_interval_results(const AigNetlist&, const AnalysisWorkspace&);
AnalysisResult analyze(AigNetlist&, const AnalysisConfig&);
} // namespace als
