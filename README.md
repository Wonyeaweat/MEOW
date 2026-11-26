# MEOW2

MEOW2 is the development workspace for the next iteration of MEOW in
[Wonyeaweat/MEOW](https://github.com/Wonyeaweat/MEOW). The published MEOW baseline
is maintained separately in
[Lin-HKUST-Guangzhou/MEOW](https://github.com/Lin-HKUST-Guangzhou/MEOW).

This revision migrates the existing analyzer, optimized SAT verifier, regression
tests, profiling tools, and circuit viewer. The analyzer still uses the original
per-node overlapping-window algorithm. Window-centric MFFW analysis, simulation
screening, and selective SAT refinement are planned work, not implemented features.
The command names `meow`, `meow-wce`, and `run.py` remain compatible.

Original paper: **Multi-View Asymmetric Error Analysis with Overlapping Windows
for Approximate Logic Synthesis** — DATE 2027. [Paper](MEOW.pdf).

MEOW analyzes gate-replacement errors and simplifies combinational AIG circuits under a worst-case error (WCE) bound.

## Build

Requirements: Linux, C++17, CMake >= 3.16, GNU Make, OpenMP, Python >= 3.10, Git, and Yosys.

```bash
sudo apt-get install build-essential cmake python3 git libreadline-dev libncurses-dev yosys
python3 run.py build -j 8
```

ABC and the Nangate45 library are downloaded automatically when needed and cached in `.deps/`. Executables are placed in `build/`.

## Usage

Analyze a circuit:

```bash
python3 run.py analysis absdiff --output results/absdiff-analysis
```

Prune a circuit with WCE bounded by 3, then verify and report mapped area, delay, and area–delay product (ADP):

```bash
python3 run.py prune absdiff --threshold 3 --output results/absdiff-prune
```

Use circuit names from `testcases/` or paths to `.aig`/`.aag` files. Multiple circuits can be listed together; `--all` selects all bundled circuits.

```bash
python3 run.py analysis mac add8 --output results/analysis-batch
python3 run.py analysis --all --output results/analysis-all
python3 run.py prune path/to/circuit.aig --threshold 3 --output results/custom-prune
```

Circuit outputs represent an unsigned binary word, with the least-significant bit first. Choose a new `--output` directory for each run.

| Option | Meaning | Default |
|---|---|---|
| `--threads N` | Worker threads | 16 |
| `--timeout S` | Analysis timeout in seconds | 600 |
| `--threshold N` | Pruning WCE bound; repeat to run multiple bounds | Two automatic bounds |
| `--budget S` | Pruning time budget in seconds | 1200 |
| `--yosys PATH` | Yosys executable for pruning | Local `.deps/yosys/usr/bin/yosys`, then `PATH` |

Automatic bounds are `floor(2^(PO/10))` and `floor(2^(PO/5))`, where `PO` is the output count. For `mac`, they are 1 and 2.

## Results

The commands above produce:

| File | Contents |
|---|---|
| `results/absdiff-analysis/summary.json` | Analysis status, runtime, mean interval width, and feasible-gate counts |
| `results/absdiff-analysis/absdiff/result.gates.csv` | Each gate's signed error intervals for constant-0 and constant-1 replacement |
| `results/absdiff-prune/summary.json` | Pruning status, runtime, final gate count, WCE verification, and mapped area/delay/ADP |
| `results/absdiff-prune/absdiff-t3/result.pruned.aag` | Simplified circuit |
| `results/absdiff-prune/absdiff-t3/summary.json` | Results for this circuit and threshold |

Start with the top-level `summary.json`. For pruning, `baseline` contains the original circuit's mapped metrics and `approximate` contains the simplified circuit's metrics. `adp_percent` is the simplified ADP as a percentage of the original. `verification` reports the WCE check; `search_complete` and `termination_reason` describe how pruning finished.

For command-line help:

```bash
python3 run.py analysis --help
python3 run.py prune --help
```

To profile core pruning after building:

```bash
python3 scripts/profile_pruning.py --output results/log2-profile
```

This runs `log2` at thresholds 9 and 84 and saves per-round analysis, planning,
SAT, and commit times in `profile.csv`, plus `breakdown.json` and run metadata.
SAT diagnostics include encoding, solver loading, solving, and conflict counts.
They also report changed output counts and CNF sizes before/after cone compaction.
Use `--threshold N` for one bound or `--summarize-only` to aggregate existing traces.
Build, independent final verification, and mapping are excluded. Direct executable
runs can enable tracing with `MEOW_PROFILE_CSV=/path/to/profile.csv`; its parent
directory must already exist.

The pruning verifier cancels structurally identical output terms at their original
bit weights, folds Boolean constants, shares common expressions, and sends only
the required CNF to ABC bsat. It retains upstream constraints for every surviving
signal, including the replacement activation condition. These are exact reductions
of the same signed-interval query; only UNSAT permits a replacement. If constant
folding already proves the query contradictory, the solver receives a minimal
contradictory CNF.

To check the verifier against exhaustive simulation (including reconvergence,
previous replacements, intervals excluding zero, and up to 256 output bits):

```bash
cmake --build build --target meow-sat-test -j 8
cmake -E chdir build ctest --output-on-failure
```

## Circuit and bound viewer

Open `tools/bound-viewer/index.html` in a browser for the bundled absdiff circuit
snapshot, or serve it locally:

```bash
python3 -m http.server 8765 --bind 127.0.0.1 --directory tools/bound-viewer
```

Visit `http://localhost:8765`. For a remote VS Code session, forward port 8765.
Click a gate to inspect its force-0/force-1 intervals and exact exhaustive results.
The default estimate is the experimental paired-signal set analysis; the original
MEOW estimate is shown separately. This snapshot is not a result of the planned
MFFW analyzer. `bounds.html` provides the corresponding table view.

The generator `tools/bound-viewer/build.py` accepts `--csv`, `--summary`, `--aag`,
`--circuit`, and `--output`; run it with `--help` for details. Generating a new view
requires Graphviz (`dot`) and matching analysis data. The bundled HTML is standalone.

## Planned MEOW2 analysis

1. Build a shared window library and reuse baseline truth tables when analyzing
   multiple nodes within each window.
2. Generate MFFWs from selected cut leaf sets; deduplicate input sets and bound
   enumeration, simulation work, and memory usage.
3. Use concrete random-input simulation to reject unsuitable replacement
   directions and retain witness inputs.
4. Spend a bounded SAT budget on selected nodes, then update dependent upstream
   estimates while reusing local window computations.

Local window evaluation and global interval composition need separate interfaces,
including handling of window outputs that are themselves analysis targets.
Simulation gives observed lower bounds, and a certified node interval does not
make the existing upstream interval composition sound. Keep estimates, witnesses,
and certified intervals distinct; retain certification before pruning mutations.
