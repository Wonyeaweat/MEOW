#include "aig.h"

#include <algorithm>
#include <stdexcept>

namespace als {

void AigNetlist::compute_forward_output_reachability() {
    if (forward_output_reachability_valid && forward_output_masks.size() == nodes.size())
        return;

    const std::size_t output_count = outputs.size();
    forward_output_masks.assign(nodes.size(), BitPattern(output_count));
    for (std::size_t index = 0; index < outputs.size(); ++index)
        forward_output_masks[outputs[index]] |= BitPattern::from_bit(index, output_count);

    for (int node_id = static_cast<int>(nodes.size()) - 1; node_id >= 1; --node_id) {
        const BitPattern mask = forward_output_masks[node_id];
        if (mask.is_zero()) continue;
        const Node& node = nodes[node_id];
        if (node.type == NodeType::AND) {
            for (int literal : {node.fanin0, node.fanin1}) {
                int fanin = lit_to_var(literal);
                if (fanin) forward_output_masks[fanin] |= mask;
            }
        } else if (node.type == NodeType::OUTPUT) {
            int fanin = lit_to_var(node.fanin0);
            if (fanin) forward_output_masks[fanin] |= mask;
        }
    }

    forward_output_reachability_valid = true;
}

void AigNetlist::clear_topology_caches() {
    forward_output_reachability_valid = false;
    forward_output_masks.clear();
}

void AigNetlist::rebuild_fanouts() {
    for (auto& node : nodes) node.fanouts.clear();
    for (int gate = 1; gate <= max_var; ++gate) {
        if (nodes[gate].type != NodeType::AND) continue;
        nodes[lit_to_var(nodes[gate].fanin0)].fanouts.push_back(gate);
        nodes[lit_to_var(nodes[gate].fanin1)].fanouts.push_back(gate);
    }
    for (int output : outputs)
        nodes[lit_to_var(nodes[output].fanin0)].fanouts.push_back(output);
    clear_topology_caches();
}

int count_and_gates(const AigNetlist& net) {
    return static_cast<int>(std::count_if(net.nodes.begin(), net.nodes.end(), [](const Node& node) {
        return node.type == NodeType::AND;
    }));
}

} // namespace als

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>

using namespace std;

namespace als {

static uint64_t read_aig_unsigned(istream& is) {
    uint64_t x = 0;
    int shift = 0;
    while (true) {
        int c = is.get();
        if (c == EOF)
            throw runtime_error("Unexpected EOF while reading AIGER binary data");
        unsigned char ch = static_cast<unsigned char>(c);
        x |= (uint64_t) (ch & 0x7F) << shift;
        if (!(ch & 0x80))
            break;
        shift += 7;
    }
    return x;
}

AigNetlist load_aiger(const string& path) {
    ifstream ifs(path, ios::binary);
    if (!ifs)
        throw runtime_error("Cannot open AIGER: " + path);
    string format;
    ifs >> format;
    if (format != "aig" && format != "aag")
        throw runtime_error("Not a valid AIGER file");

    AigNetlist net;
    int M, I, L, O, A; // Max Var Index, Inputs, Latches, Outputs, ANDs
    ifs >> M >> I >> L >> O >> A;
    if (L != 0)
        throw runtime_error("Sequential AIGER netlists are not supported (latches=" +
                            std::to_string(L) + ")");

    net.max_var = M;
    /*
    Constant: 0
    Inputs: 1 .. I
    AND gates: I+1 .. M
    Latches: (ignored, =0)
    Outputs: M+1 .. M+O
    */
    net.nodes.resize(M + O + 1);
    net.input_names.resize(static_cast<std::size_t>(I));
    net.output_names.resize(static_cast<std::size_t>(O));

    net.nodes[0].id = 0;
    net.nodes[0].type = NodeType::CONST0;

    auto add_input = [&](int lit) {
        int id = lit_to_var(lit); // Variable ID
        // Note: In AIGER, inputs are variables.
        // We assume inputs are positive literals in definition.
        net.nodes[id].id = id;
        net.nodes[id].type = NodeType::INPUT;
        net.inputs.push_back(id);
    };

    if (format == "aag") {
        // Inputs
        for (int i = 0; i < I; ++i) {
            int lit;
            ifs >> lit;
            add_input(lit);
        }
        // Latches
        for (int i = 0; i < L; ++i) {
            int tmp;
            ifs >> tmp >> tmp;
        }
        // Outputs
        for (int i = 0; i < O; ++i) {
            int lit;
            ifs >> lit;
            int out_node_id = M + 1 + i;
            net.nodes[out_node_id].id = out_node_id;
            net.nodes[out_node_id].type = NodeType::OUTPUT;
            net.nodes[out_node_id].fanin0 = lit;
            net.outputs.push_back(out_node_id);
        }
        // ANDs
        for (int i = 0; i < A; ++i) {
            int lhs, r0, r1;
            ifs >> lhs >> r0 >> r1;
            int id = lit_to_var(lhs);
            net.nodes[id].id = id;
            net.nodes[id].type = NodeType::AND;
            net.nodes[id].fanin0 = r0;
            net.nodes[id].fanin1 = r1;
        }
    } else {
        // Binary AIG uses implied inputs 1..I
        for (int i = 0; i < I; ++i)
            add_input(2 * (i + 1));
        // Latches
        for (int i = 0; i < L; ++i) {
            int tmp;
            ifs >> tmp >> tmp;
        }
        // Outputs
        for (int i = 0; i < O; ++i) {
            int lit;
            ifs >> lit;
            int out_node_id = M + 1 + i;
            net.nodes[out_node_id].id = out_node_id;
            net.nodes[out_node_id].type = NodeType::OUTPUT;
            net.nodes[out_node_id].fanin0 = lit;
            net.outputs.push_back(out_node_id);
        }
        // Consume only the textual section's line ending. The first binary
        // delta is allowed to be 0x0a or 0x0d and must not be skipped.
        if (ifs.peek() == '\r') {
            ifs.get();
            if (ifs.peek() == '\n')
                ifs.get();
        } else if (ifs.peek() == '\n') {
            ifs.get();
        }
        // ANDs
        for (int i = 0; i < A; ++i) {
            int lhs = 2 * (I + L + i + 1);
            uint64_t delta0 = read_aig_unsigned(ifs);
            uint64_t delta1 = read_aig_unsigned(ifs);
            int rhs0 = lhs - (int) delta0;
            int rhs1 = rhs0 - (int) delta1;

            int id = lit_to_var(lhs);
            net.nodes[id].id = id;
            net.nodes[id].type = NodeType::AND;
            net.nodes[id].fanin0 = rhs0;
            net.nodes[id].fanin1 = rhs1;
        }
    }

    // AIGER symbols follow the structural payload. Preserve them so generic
    // arithmetic stimuli can discover operand buses without case names.
    if (format == "aag") {
        ifs.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    } else if (ifs.peek() == '\r') {
        ifs.get();
        if (ifs.peek() == '\n') ifs.get();
    } else if (ifs.peek() == '\n') {
        ifs.get();
    }
    string line;
    while (std::getline(ifs, line)) {
        if (line == "c" || (!line.empty() && line[0] == 'c')) break;
        if (line.size() < 3 || (line[0] != 'i' && line[0] != 'o'))
            continue;
        std::size_t cursor = 1;
        while (cursor < line.size() &&
               std::isdigit(static_cast<unsigned char>(line[cursor])))
            ++cursor;
        if (cursor == 1 || cursor >= line.size() || line[cursor] != ' ')
            continue;
        const std::size_t index = static_cast<std::size_t>(
            std::stoull(line.substr(1, cursor - 1)));
        const string name = line.substr(cursor + 1);
        if (line[0] == 'i' && index < net.input_names.size())
            net.input_names[index] = name;
        else if (line[0] == 'o' && index < net.output_names.size())
            net.output_names[index] = name;
    }

    // Validate references before building fanouts.  Apart from producing a
    // useful diagnostic, doing this first prevents malformed AIGER literals
    // from indexing outside nodes below.  The exhaustive verifier performs
    // stricter shape checks on top of these loader invariants.
    auto validate_literal = [&](int literal, int owner) {
        if (literal < 0 || lit_to_var(literal) > M || lit_to_var(literal) >= owner)
            throw runtime_error("AIGER contains an invalid or non-topological fanin");
    };
    for (int variable = 1; variable <= M; ++variable) {
        const auto& node = net.nodes[variable];
        if (node.type == NodeType::AND) {
            validate_literal(node.fanin0, variable);
            validate_literal(node.fanin1, variable);
        } else if (node.type != NodeType::INPUT) {
            throw runtime_error("AIGER has an undeclared internal node");
        }
    }
    for (int output : net.outputs)
        validate_literal(net.nodes[output].fanin0, output);

    // Build Fanouts. 1..M (Input + And)
    for (int i = 1; i <= M; ++i) {
        if (net.nodes[i].type == NodeType::AND) {
            int f0 = lit_to_var(net.nodes[i].fanin0);
            int f1 = lit_to_var(net.nodes[i].fanin1);
            net.nodes[f0].fanouts.push_back(i);
            net.nodes[f1].fanouts.push_back(i);
        }
    }
    // Iterate Outputs
    for (int out_id : net.outputs) {
        int f0 = lit_to_var(net.nodes[out_id].fanin0);
        net.nodes[f0].fanouts.push_back(out_id);
    }

    for (int i = 1; i <= M; ++i) {
        if (net.nodes[i].type == NodeType::AND && net.nodes[i].fanouts.empty()) {
            throw runtime_error("AIGER contains dangling AND node " + to_string(i) +
                                " with no fanouts after load.");
        }
    }

    return net;
}

void write_aiger(const AigNetlist& net, const std::string& filepath) {
    const auto& nodes = net.nodes;
    const auto& inputs = net.inputs;
    const auto& outputs = net.outputs;
    const int max_var = net.max_var;
    std::ofstream ofs(filepath);
    if (!ofs)
        throw std::runtime_error("Cannot open " + filepath + " for writing");
    std::vector<int> id_map(nodes.size(), -1);
    int current_idx = 0;
    for (int in_id : inputs) {
        id_map[in_id] = ++current_idx;
    }
    std::vector<int> active_ands;
    for (int i = 1; i <= max_var; ++i) {
        if (nodes[i].type == NodeType::AND) {
            active_ands.push_back(i);
            id_map[i] = ++current_idx;
        }
    }
    int M = current_idx;
    int I = (int) inputs.size();
    int L = 0; // Latches not supported yet
    int O = (int) outputs.size();
    int A = (int) active_ands.size();

    ofs << "aag " << M << " " << I << " " << L << " " << O << " " << A << "\n";

    for (int in_id : inputs) {
        ofs << id_map[in_id] * 2 << "\n";
    }

    auto resolve_removed_lit = [&](int lit) {
        int resolved = lit;
        for (size_t guard = 0; guard < nodes.size(); ++guard) {
            int v = lit_to_var(resolved);
            if (v <= 0 || v >= static_cast<int>(nodes.size()))
                break;
            NodeType t = nodes[v].type;
            if (t != NodeType::CONST0 && t != NodeType::CONST1)
                break;
            int repl = nodes[v].fanin0;
            if (t == NodeType::CONST1 && repl == 0)
                repl = 1;
            if (repl == (v << 1) || repl == ((v << 1) | 1))
                break;
            resolved = repl ^ lit_is_inv(resolved);
        }
        return resolved;
    };
    auto translate_lit = [&](int old_lit) -> int {
        old_lit = resolve_removed_lit(old_lit);
        int old_var = lit_to_var(old_lit);
        bool is_inv = lit_is_inv(old_lit);
        if (old_var == 0)
            return is_inv ? 1 : 0;
        if (id_map[old_var] == -1) {
            if (nodes[old_var].type == NodeType::CONST1)
                return is_inv ? 0 : 1;
            return is_inv ? 1 : 0; // Default to 0
        }

        int new_base = id_map[old_var] * 2;
        return is_inv ? (new_base + 1) : new_base;
    };

    for (int out_id : outputs) {
        int driver_lit = nodes[out_id].fanin0;
        ofs << translate_lit(driver_lit) << "\n";
    }

    for (int and_id : active_ands) {
        int lhs = id_map[and_id] * 2;
        int rhs0 = translate_lit(nodes[and_id].fanin0);
        int rhs1 = translate_lit(nodes[and_id].fanin1);
        ofs << lhs << " " << rhs0 << " " << rhs1 << "\n";
    }

    for (std::size_t index = 0; index < net.input_names.size(); ++index)
        if (!net.input_names[index].empty())
            ofs << 'i' << index << ' ' << net.input_names[index] << "\n";
    for (std::size_t index = 0; index < net.output_names.size(); ++index)
        if (!net.output_names[index].empty())
            ofs << 'o' << index << ' ' << net.output_names[index] << "\n";

    ofs << "c\nGenerated by ALS Sim Pruning\n";
}

} // namespace als
