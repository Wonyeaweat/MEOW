#pragma once
#include "core/aig.h"
namespace als {
enum class SatDecision { Safe, Unsafe, Unknown };
// Optional diagnostics; times are wall seconds and exclude caller overhead.
struct SatProfile {
    double encode_seconds = 0, load_seconds = 0, solve_seconds = 0;
    int variables = 0;
    std::size_t clauses = 0;
    long long conflicts = 0, decisions = 0, propagations = 0;
    int variables_before_compaction = 0;
    std::size_t clauses_before_compaction = 0;
    std::size_t outputs = 0, changed_outputs = 0;
};
// Only UNSAT authorizes a mutation; SAT and timeout leave the circuit intact.
SatDecision check_gate_const_error_interval_sat(
    const AigNetlist& current, int gate_id, bool force_one,
    const WeightedInterval& interval, double timeout_seconds,
    SatProfile* profile = nullptr);
} // namespace als
