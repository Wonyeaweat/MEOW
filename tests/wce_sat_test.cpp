#include "core/aig.h"
#include "pruning/netlist_transform.h"
#include "verification/wce_sat.h"

#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace als;
int checks = 0;

AigNetlist make_net(int inputs, const std::vector<std::pair<int, int>>& gates,
                    const std::vector<int>& outputs) {
    AigNetlist net;
    net.max_var = inputs + static_cast<int>(gates.size());
    net.nodes.resize(net.max_var + outputs.size() + 1);
    for (int i = 1; i <= inputs; ++i) {
        net.inputs.push_back(i);
        net.nodes[i] = {i, NodeType::INPUT};
    }
    for (std::size_t i = 0; i < gates.size(); ++i) {
        const int id = inputs + 1 + static_cast<int>(i);
        net.nodes[id] = {id, NodeType::AND, gates[i].first, gates[i].second};
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const int id = net.max_var + 1 + static_cast<int>(i);
        net.outputs.push_back(id);
        net.nodes[id] = {id, NodeType::OUTPUT, outputs[i]};
    }
    net.rebuild_fanouts();
    return net;
}

// Independent concrete simulation, including all 256 output weights.
Weight evaluate(const AigNetlist& net, unsigned input, int target, bool force) {
    std::vector<bool> values(net.nodes.size(), false);
    for (std::size_t i = 0; i < net.inputs.size(); ++i)
        values[net.inputs[i]] = (input >> i) & 1;
    auto read = [&](int lit) { return values[lit_to_var(lit)] != lit_is_inv(lit); };
    for (int id = 1; id <= net.max_var; ++id) {
        const auto& node = net.nodes[id];
        if (node.type == NodeType::AND)
            values[id] = id == target ? force : read(node.fanin0) && read(node.fanin1);
    }
    Weight output;
    for (std::size_t bit = 0; bit < net.outputs.size(); ++bit)
        if (read(net.nodes[net.outputs[bit]].fanin0)) output += Weight::from_bit(bit);
    return output;
}

WeightedInterval exact_interval(const AigNetlist& net, int gate, bool force) {
    WeightedInterval result;
    for (unsigned input = 0; input < (1U << net.inputs.size()); ++input) {
        const auto a = evaluate(net, input, gate, force);
        const auto b = evaluate(net, input, -1, false);
        const SignedWeight delta = a >= b ? SignedWeight::positive(a - b)
                                         : SignedWeight::negative(b - a);
        if (input == 0) result = {delta, delta};
        else {
            result.l = std::min(result.l, delta);
            result.u = std::max(result.u, delta);
        }
    }
    return result;
}

void verify(const AigNetlist& net, int gate, bool force,
            const WeightedInterval& interval, const WeightedInterval& exact) {
    const bool safe = interval.l <= exact.l && exact.u <= interval.u;
    SatProfile profile;
    const auto actual = check_gate_const_error_interval_sat(net, gate, force, interval, 5, &profile);
    ++checks;
    if (actual != (safe ? SatDecision::Safe : SatDecision::Unsafe))
        throw std::runtime_error("SAT/concrete mismatch, check " + std::to_string(checks) +
                                 ", gate " + std::to_string(gate) + ", force " + std::to_string(force));
    if (profile.variables > profile.variables_before_compaction ||
        profile.clauses > profile.clauses_before_compaction)
        throw std::runtime_error("CNF compaction increased the formula");
}

void verify_gate(const AigNetlist& net, int gate) {
    const SignedWeight one(1), zero;
    const auto cap = signed_error_cap(net.outputs.size());
    for (bool force : {false, true}) {
        const auto exact = exact_interval(net, gate, force);
        verify(net, gate, force, exact, exact);
        verify(net, gate, force, {-cap, cap}, exact);
        verify(net, gate, force, {zero, zero}, exact);
        verify(net, gate, force, {one, one}, exact);  // Must not assume fault activation.
        verify(net, gate, force, {-one, -one}, exact);
        if (exact.l < exact.u) {
            verify(net, gate, force, {exact.l + one, exact.u}, exact);
            verify(net, gate, force, {exact.l, exact.u - one}, exact);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        // Repeated/reversed gates, constants, complements, reconvergence, and
        // unchanged output terms interspersed with changed terms.
        auto net = make_net(3, {{2,4}, {4,2}, {8,6}, {9,6}, {12,14},
                                {8,9}, {1,8}, {0,8}}, {2,12,4,15,16,19,20,22});
        for (int gate = 4; gate <= net.max_var; ++gate) verify_gate(net, gate);
        // Exercise tombstones and rewired fanins after a preceding prune.
        MutationScratch scratch;
        apply_replacement(net, {6, 1}, scratch);
        for (int gate = 1; gate <= net.max_var; ++gate)
            if (net.nodes[gate].type == NodeType::AND) verify_gate(net, gate);

        // Sparse high-bit changes, signed 257-bit subtraction and full-range bounds.
        for (int width : {1, 8, 64, 65, 128, 255, 256}) {
            std::vector<int> outputs(width, 2);
            outputs.back() = 6;
            verify_gate(make_net(2, {{2,4}}, outputs), 3);
            std::fill(outputs.begin(), outputs.end(), 6);
            verify_gate(make_net(2, {{2,4}}, outputs), 3);
        }

        std::mt19937 rng(42073);
        for (int trial = 0; trial < 40; ++trial) {
            std::vector<std::pair<int, int>> gates;
            for (int id = 5; id < 17; ++id)
                gates.emplace_back(rng() % (2 * id), rng() % (2 * id));
            std::vector<int> outputs;
            for (int bit = 0; bit < 8; ++bit) outputs.push_back(rng() % 34);
            const auto random = make_net(4, gates, outputs);
            for (int gate = 5; gate <= random.max_var; ++gate) verify_gate(random, gate);
        }

        // Real reconvergent circuit: exact and deliberately too-narrow bounds.
        if (argc == 2) {
            const auto absdiff = load_aiger(argv[1]);
            for (int gate : {17, 19, 25, 68, 73, 74, 75, 76, 77, 157})
                verify_gate(absdiff, gate);
        }
        std::cout << "PASS: " << checks << " SAT decisions agree with exhaustive simulation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
