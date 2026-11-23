#pragma once
#include "analysis/analysis.h"
#include "pruning/pruning.h"
#include <string>
namespace als {
void write_analysis_outputs(const std::string& prefix, const AnalysisResult&, double seconds);
void write_pruning_outputs(const std::string& prefix, const PruningResult&, double seconds);
} // namespace als
