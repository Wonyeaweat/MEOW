#include "analysis.h"
#include "meow.h"

namespace als {
AnalysisResult collect_interval_results(const AigNetlist& net,
                                        const AnalysisWorkspace& workspace) {
    AnalysisResult result;
    for (int gate = 1; gate <= net.max_var; ++gate) {
        if (net.nodes[gate].type != NodeType::AND) continue;
        const auto& force = workspace.node_force_intervals.at(gate);
        result.gates.push_back({gate,
            force.sa0, force.sa1});
    }
    return result;
}
AnalysisResult analyze(AigNetlist& net, const AnalysisConfig& config) {
    return meow::analyze(net, config);
}
} // namespace als
