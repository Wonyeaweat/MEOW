#include "analysis/analysis.h"
#include "pruning/pruning.h"
#include "reporting/result_writer.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
struct Options {
    std::string input, prefix, mode;
    int threads = 16;
    Weight threshold = Weight(3);
    double budget = 1200;
};
Options parse(int argc, char** argv) {
    Options args;
    bool prune_option = false;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") {
            std::cout << "meow --input FILE --output-prefix PREFIX --mode analysis|prune\n"
                         "     [--threads N] [--threshold N] [--prune-timeout-seconds N]\n";
            std::exit(0);
        }
        if (++i == argc) throw std::invalid_argument("Missing value for " + key);
        const std::string value = argv[i];
        std::size_t used = 0;
        if (key == "--input") args.input = value;
        else if (key == "--output-prefix") args.prefix = value;
        else if (key == "--mode") args.mode = value;
        else if (key == "--threads") {
            args.threads = std::stoi(value, &used);
            if (used != value.size() || args.threads < 1)
                throw std::invalid_argument("threads must be positive");
        } else if (key == "--threshold") {
            args.threshold = Weight::from_dec_string(value);
            prune_option = true;
        } else if (key == "--prune-timeout-seconds") {
            args.budget = std::stod(value, &used);
            if (used != value.size() || !(args.budget > 0) || !std::isfinite(args.budget))
                throw std::invalid_argument("budget must be finite and positive");
            prune_option = true;
        } else throw std::invalid_argument("Unknown option: " + key);
    }
    if (args.input.empty() || args.prefix.empty())
        throw std::invalid_argument("--input and --output-prefix are required");
    if (args.mode != "analysis" && args.mode != "prune")
        throw std::invalid_argument("--mode must be analysis or prune");
    if (prune_option && args.mode != "prune")
        throw std::invalid_argument("threshold and pruning budget require prune mode");
    return args;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse(argc, argv);
        auto net = als::load_aiger(args.input);
        const auto start = std::chrono::steady_clock::now();
        auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
        if (args.mode == "analysis") {
            als::AnalysisConfig config;
            config.threads = args.threads;
            const auto result = als::analyze(net, config);
            als::write_analysis_outputs(args.prefix, result, elapsed());
        } else {
            als::PruningConfig config;
            config.threads = args.threads;
            config.threshold = args.threshold;
            config.timeout_seconds = args.budget;
            const auto result = als::run_pruning(std::move(net), config);
            als::write_pruning_outputs(args.prefix, result, elapsed());
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "meow: " << error.what() << '\n';
        return 1;
    }
}
