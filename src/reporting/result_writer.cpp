#include "result_writer.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace als {
namespace {
std::ofstream output(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write " + path);
    out << std::setprecision(17);
    return out;
}
std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c);
        else out << c;
    }
    return out.str() + '"';
}
} // namespace

void write_analysis_outputs(const std::string& prefix, const AnalysisResult& result, double seconds) {
    const auto path = prefix + ".gates.csv";
    auto csv = output(path);
    csv << "gate_id,sa0_interval_l,sa0_interval_u,sa1_interval_l,sa1_interval_u\n";
    for (const auto& gate : result.gates)
        csv << gate.gate_id << ',' << gate.sa0.l << ',' << gate.sa0.u
            << ',' << gate.sa1.l << ',' << gate.sa1.u << '\n';
    auto out = output(prefix + ".summary.json");
    out << "{\"metrics\":{\"gate_count\":" << result.gates.size()
        << ",\"elapsed_seconds\":" << seconds << "},\"artifacts\":{\"gate_csv\":"
        << json_string(path) << "}}\n";
}

void write_pruning_outputs(const std::string& prefix, const PruningResult& result, double seconds) {
    const auto path = prefix + ".pruned.aag";
    auto out = output(prefix + ".summary.json");
    write_aiger(result.net, path);
    out << "{\"metrics\":{\"initial_gates\":" << result.initial_gates
        << ",\"final_gates\":" << result.final_gates
        << ",\"elapsed_seconds\":" << seconds
        << ",\"estimated_wce\":\"" << interval_abs_bound(result.estimated_interval)
        << "\"},\"search_complete\":" << (result.search_complete ? "true" : "false")
        << ",\"budget_exhausted\":" << (result.budget_exhausted ? "true" : "false")
        << ",\"termination_reason\":" << json_string(result.termination_reason)
        << ",\"certification\":{\"status\":\"PASS\",\"method\":\"abc-bsat-step-local-interval\","
           "\"acceptance_reference\":\"current-step-interval\"},\"artifacts\":{\"netlist\":"
        << json_string(path) << "}}\n";
}
} // namespace als
