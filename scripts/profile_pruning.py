#!/usr/bin/env python3
"""Run the existing prune algorithm with optional phase/SAT diagnostics.

Build first: python3 run.py build -j 8
Run: python3 scripts/profile_pruning.py --output results/log2-profile
Summarize an existing run: add --summarize-only
Times exclude build, final independent WCE verification, and technology mapping.
"""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def summarize(work):
    with (work / 'profile.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    phase = defaultdict(float)
    counts = Counter()
    for row in rows:
        phase[row['stage']] += float(row['seconds'])
        counts[row['stage']] += 1
    sat = [r for r in rows if r['stage'] == 'sat']
    result = {
        'phase_seconds': dict(phase), 'phase_calls': dict(counts),
        'sat_decisions': dict(Counter(r['decision'] for r in sat)),
        'sat_seconds': {key: sum(float(r[key]) for r in sat)
                        for key in ('encode_s', 'load_s', 'solve_s')},
        'sat_counts': {key: sum(int(r[key]) for r in sat)
                       for key in ('conflicts', 'decisions', 'propagations')},
        'candidate_counts_by_round': [int(r['candidates']) for r in rows if r['stage'] == 'planning'],
        'slowest_sat_queries': sorted(sat, key=lambda r: float(r['seconds']), reverse=True)[:10],
    }
    if sat and 'variables_before_compaction' in sat[0]:
        result['sat_formula_totals'] = {
            key: sum(int(r[key]) for r in sat)
            for key in ('variables_before_compaction', 'clauses_before_compaction',
                        'variables', 'clauses', 'outputs', 'changed_outputs')
        }
    summary = work / 'result.summary.json'
    if summary.exists():
        result['result'] = json.loads(summary.read_text())
        total = result['result']['metrics']['elapsed_seconds']
        result['unattributed_seconds'] = total - sum(phase.values())
        result['phase_percent'] = {k: 100 * v / total for k, v in phase.items()}
    (work / 'breakdown.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', default='log2')
    parser.add_argument('--threshold', type=int, action='append')
    parser.add_argument('--threads', type=int, default=16)
    parser.add_argument('--budget', type=float, default=1200)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--summarize-only', action='store_true')
    args = parser.parse_args()
    for threshold in args.threshold or [9, 84]:
        work = args.output.resolve() / f'{args.case}-t{threshold}'
        if not args.summarize_only:
            work.mkdir(parents=True, exist_ok=False)
            binary = ROOT / 'build/meow'
            circuit = ROOT / 'testcases' / f'{args.case}.aig'
            command = [str(binary), '--input', str(circuit), '--output-prefix', str(work / 'result'),
                       '--mode', 'prune', '--threshold', str(threshold), '--threads', str(args.threads),
                       '--prune-timeout-seconds', str(args.budget)]
            env = dict(os.environ, MEOW_PROFILE_CSV=str(work / 'profile.csv'),
                       OMP_NUM_THREADS=str(args.threads))
            manifest = {'command': command, 'host': platform.node(), 'platform': platform.platform(),
                        'cpu_model': next((s.split(':', 1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')), ''),
                        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                        'input_sha256': hashlib.sha256(circuit.read_bytes()).hexdigest(),
                        'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                        'omp_environment': {k: v for k, v in env.items() if k.startswith(('OMP_', 'GOMP_'))},
                        'start_loadavg': os.getloadavg(), 'started_epoch': time.time()}
            (work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            started = time.monotonic()
            print(f'Running {args.case} T={threshold}', flush=True)
            with (work / 'stdout.log').open('w') as stdout, (work / 'stderr.log').open('w') as stderr:
                child = subprocess.run(command, env=env, stdout=stdout, stderr=stderr, check=True)
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            manifest.update(wall_seconds=time.monotonic() - started,
                            user_seconds=after.ru_utime - before.ru_utime,
                            system_seconds=after.ru_stime - before.ru_stime,
                            end_loadavg=os.getloadavg(), returncode=child.returncode)
            (work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        result = summarize(work)
        print(json.dumps({'case': args.case, 'threshold': threshold,
                          **{k: v for k, v in result.items() if k != 'slowest_sat_queries'}}, indent=2), flush=True)


if __name__ == '__main__':
    main()
