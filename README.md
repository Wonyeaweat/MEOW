# MEOW

**Multi-View Asymmetric Error Analysis with Overlapping Windows for Approximate Logic Synthesis** — DATE 2027. [Paper](MEOW.pdf)

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
