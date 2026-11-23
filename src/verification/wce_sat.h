#pragma once
#include "core/aig.h"
namespace als {
enum class SatDecision { Safe, Unsafe, Unknown };
// Only UNSAT authorizes a mutation; SAT and timeout leave the circuit intact.
SatDecision check_gate_const_error_interval_sat(
    const AigNetlist& current, int gate_id, bool force_one,
    const WeightedInterval& interval, double timeout_seconds);
} // namespace als
