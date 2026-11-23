#include "core/aig.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <omp.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using als::AigNetlist;

[[noreturn]] void usage(const std::string& message = {}) {
    if (!message.empty()) std::cerr << "error: " << message << "\n\n";
    std::cerr << "Usage: meow-wce --original FILE --approximate FILE"
                 " --threshold N [--threads N]\n";
    std::exit(message.empty() ? 0 : 2);
}

struct Options {
    std::string original;
    std::string approximate;
    std::uint64_t threshold = 0;
    bool threshold_set = false;
    int threads = 1;
};

std::string require_value(int& index, int argc, char** argv) {
    if (++index >= argc) usage(std::string("missing value for ") + argv[index - 1]);
    return argv[index];
}

std::uint64_t parse_u64(const std::string& text, const std::string& option) {
    if (text.empty() || std::any_of(text.begin(), text.end(), [](char character) {
            return !std::isdigit(static_cast<unsigned char>(character));
        }))
        usage(option + " expects a non-negative unsigned integer");
    std::size_t consumed = 0;
    try {
        const auto value = std::stoull(text, &consumed);
        if (consumed != text.size()) throw std::invalid_argument("trailing");
        return static_cast<std::uint64_t>(value);
    } catch (const std::exception&) {
        usage(option + " expects a non-negative unsigned integer");
    }
}

int parse_threads(const std::string& text) {
    const auto value = parse_u64(text, "--threads");
    if (value == 0 || value > 1000000)
        usage("--threads expects a positive integer");
    return static_cast<int>(value);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") usage();
        if (argument == "--original") {
            options.original = require_value(index, argc, argv);
        } else if (argument == "--approximate") {
            options.approximate = require_value(index, argc, argv);
        } else if (argument == "--threshold") {
            options.threshold = parse_u64(require_value(index, argc, argv), argument);
            options.threshold_set = true;
        } else if (argument == "--threads") {
            options.threads = parse_threads(require_value(index, argc, argv));
        } else {
            usage("unknown option: " + argument);
        }
    }
    if (options.original.empty()) usage("--original is required");
    if (options.approximate.empty()) usage("--approximate is required");
    if (!options.threshold_set) usage("--threshold is required");
    return options;
}

// Validate the parts of AIGER that are easy to accidentally misread when
// evaluating directly.  In particular, reject out-of-range or non-topological
// fanins instead of silently evaluating a malformed graph.  AND fanins are
// deliberately not required to be numerically sorted: AND is commutative and
// valid AIGER emitted by transformations may use either left/right order.
void validate_netlist(const AigNetlist& net, const std::string& label) {
    if (net.inputs.size() > 24)
        throw std::invalid_argument(label + " has more than 24 primary inputs");
    if (net.outputs.empty() || net.outputs.size() > 63)
        throw std::invalid_argument(label + " must have between 1 and 63 outputs");
    if (net.max_var < 0 || net.nodes.size() !=
            static_cast<std::size_t>(net.max_var + net.outputs.size() + 1))
        throw std::invalid_argument(label + " has an invalid AIGER node shape");

    std::vector<bool> seen_inputs(static_cast<std::size_t>(net.max_var + 1), false);
    for (int input : net.inputs) {
        if (input <= 0 || input > net.max_var || seen_inputs[input] ||
            net.nodes[input].type != als::NodeType::INPUT)
            throw std::invalid_argument(label + " has an invalid or duplicated input");
        seen_inputs[input] = true;
    }

    auto validate_literal = [&](int literal, int owner) {
        if (literal < 0) throw std::invalid_argument(label + " has a negative literal");
        const int variable = als::lit_to_var(literal);
        if (variable < 0 || variable > net.max_var || variable >= owner)
            throw std::invalid_argument(label + " has a non-topological fanin");
    };

    for (int variable = 1; variable <= net.max_var; ++variable) {
        const auto& node = net.nodes[variable];
        if (node.type == als::NodeType::INPUT) {
            if (!seen_inputs[variable])
                throw std::invalid_argument(label + " has an undeclared input node");
        } else if (node.type == als::NodeType::AND) {
            validate_literal(node.fanin0, variable);
            validate_literal(node.fanin1, variable);
        } else {
            throw std::invalid_argument(label + " has an unexpected internal node type");
        }
    }
    for (std::size_t index = 0; index < net.outputs.size(); ++index) {
        const int output = net.outputs[index];
        if (output != net.max_var + 1 + static_cast<int>(index) ||
            output <= net.max_var || output >= static_cast<int>(net.nodes.size()))
            throw std::invalid_argument(label + " has an invalid output ordering");
        validate_literal(net.nodes[output].fanin0, output);
    }
}

std::uint64_t literal_word(const std::vector<std::uint64_t>& values, int literal) {
    const std::uint64_t value = values[als::lit_to_var(literal)];
    return als::lit_is_inv(literal) ? ~value : value;
}

std::uint64_t exhaustive_wce(const AigNetlist& original,
                             const AigNetlist& approximate, int threads) {
    const std::uint64_t pattern_count = std::uint64_t{1} << original.inputs.size();
    const std::uint64_t block_count = (pattern_count + 63) / 64;
    std::uint64_t low_input_words[6] = {};
    for (unsigned input = 0; input < 6; ++input) {
        for (unsigned lane = 0; lane < 64; ++lane) {
            if ((lane >> input) & 1U)
                low_input_words[input] |= std::uint64_t{1} << lane;
        }
    }
    std::uint64_t maximum = 0;
#pragma omp parallel for schedule(static) num_threads(threads) reduction(max:maximum)
    for (std::int64_t block = 0; block < static_cast<std::int64_t>(block_count); ++block) {
        std::vector<std::uint64_t> original_values(original.nodes.size());
        std::vector<std::uint64_t> approximate_values(approximate.nodes.size());
        const std::uint64_t base = static_cast<std::uint64_t>(block) * 64;
        for (std::size_t index = 0; index < original.inputs.size(); ++index) {
            // Consecutive blocks vary the six least-significant pattern bits;
            // all higher input bits are constant throughout a 64-lane block.
            const std::uint64_t word = index < 6
                ? low_input_words[index]
                : (((base >> index) & 1ULL) ? UINT64_MAX : 0ULL);
            original_values[original.inputs[index]] = word;
            approximate_values[approximate.inputs[index]] = word;
        }
        for (int variable = 1; variable <= original.max_var; ++variable) {
            const auto& node = original.nodes[variable];
            if (node.type == als::NodeType::AND)
                original_values[variable] = literal_word(original_values, node.fanin0) &
                                             literal_word(original_values, node.fanin1);
        }
        for (int variable = 1; variable <= approximate.max_var; ++variable) {
            const auto& node = approximate.nodes[variable];
            if (node.type == als::NodeType::AND)
                approximate_values[variable] = literal_word(approximate_values, node.fanin0) &
                                               literal_word(approximate_values, node.fanin1);
        }
        std::uint64_t original_outputs[64] = {};
        std::uint64_t approximate_outputs[64] = {};
        for (std::size_t output = 0; output < original.outputs.size(); ++output) {
            const std::uint64_t original_word =
                literal_word(original_values, original.nodes[original.outputs[output]].fanin0);
            const std::uint64_t approximate_word = literal_word(
                approximate_values, approximate.nodes[approximate.outputs[output]].fanin0);
            for (unsigned lane = 0; lane < 64; ++lane) {
                if ((original_word >> lane) & 1ULL) original_outputs[lane] |= 1ULL << output;
                if ((approximate_word >> lane) & 1ULL)
                    approximate_outputs[lane] |= 1ULL << output;
            }
        }
        const unsigned lanes = static_cast<unsigned>(std::min<std::uint64_t>(64, pattern_count - base));
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const std::uint64_t lhs = original_outputs[lane];
            const std::uint64_t rhs = approximate_outputs[lane];
            maximum = std::max(maximum, lhs >= rhs ? lhs - rhs : rhs - lhs);
        }
    }
    return maximum;
}

std::string json_escape(const std::string& text) {
    std::ostringstream escaped;
    for (const char character : text) {
        if (character == '\\' || character == '"') escaped << '\\';
        escaped << character;
    }
    return escaped.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const AigNetlist original = als::load_aiger(options.original);
        const AigNetlist approximate = als::load_aiger(options.approximate);
        validate_netlist(original, "original");
        validate_netlist(approximate, "approximate");
        if (original.inputs.size() != approximate.inputs.size() ||
            original.outputs.size() != approximate.outputs.size())
            throw std::invalid_argument("original and approximate AIGER shapes do not match");

        const auto started = std::chrono::steady_clock::now();
        const std::uint64_t wce = exhaustive_wce(original, approximate, options.threads);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const bool pass = wce <= options.threshold;
        std::cout << "{\"status\":\"" << (pass ? "PASS" : "FAIL")
                  << "\",\"original\":\"" << json_escape(options.original)
                  << "\",\"approximate\":\"" << json_escape(options.approximate)
                  << "\",\"inputs\":" << original.inputs.size()
                  << ",\"outputs\":" << original.outputs.size()
                  << ",\"patterns\":" << (std::uint64_t{1} << original.inputs.size())
                  << ",\"threshold\":" << options.threshold
                  << ",\"wce\":" << wce
                  << ",\"threads\":" << options.threads
                  << ",\"elapsed_seconds\":" << elapsed << "}\n";
        return pass ? 0 : 1;
    } catch (const std::exception& error) {
        std::cout << "{\"status\":\"ERROR\",\"error\":\""
                  << json_escape(error.what()) << "\"}\n";
        return 2;
    }
}
