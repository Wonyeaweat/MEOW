#!/usr/bin/env python3
"""Build MEOW, analyze AIGs, or prune and verify circuits."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from src.runtime.aiger import AigerError, read_aiger
from src.runtime.process import ProcessError, resolve_executable, run_process
from src.runtime.verification import verify_wce

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / 'build'
DEPS = ROOT / '.deps'
ABC_REVISION = 'f1b64be84071a431eac9871f8cdc71bc912fd75a'
ABC_URL = 'https://github.com/changmg/abc.git'
MECALS_URL = 'https://github.com/SJTU-ECTL/MECALS.git'
LIBRARY_REVISION = '10f16718efc19108c405dba25c82c9629071e25f'
LIBRARY_SHA256 = '2efd0b32eb580e4e60e72fc0575bb3bc69aac907c91d908442e4ae6d7fe55895'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def fetch_source(path, url, revision, partial=False):
    commands = [['git', 'init', '-q', str(path)],
                ['git', '-C', str(path), 'remote', 'add', 'origin', url]]
    if partial:
        commands += [['git', '-C', str(path), 'config', 'remote.origin.promisor', 'true'],
                     ['git', '-C', str(path), 'config', 'remote.origin.partialclonefilter', 'blob:none']]
    commands.append(['git', '-C', str(path), 'fetch', '--depth', '1',
                     *(['--filter=blob:none'] if partial else []), 'origin', revision])
    for command in commands:
        subprocess.run(command, check=True, timeout=180)


def build(jobs=4):
    DEPS.mkdir(exist_ok=True)
    abc = DEPS / 'abc'
    marker = abc / '.meow-revision'
    if not abc.exists():
        with tempfile.TemporaryDirectory(dir=DEPS, prefix='abc-fetch-') as temporary:
            checkout = Path(temporary) / 'abc'
            fetch_source(checkout, ABC_URL, ABC_REVISION)
            subprocess.run(['git', '-C', str(checkout), 'checkout', '--detach', 'FETCH_HEAD'],
                           check=True, timeout=180)
            (checkout / '.meow-revision').write_text(ABC_REVISION + '\n')
            checkout.rename(abc)
    if not marker.is_file() or marker.read_text().strip() != ABC_REVISION:
        raise ValueError('ABC cache revision differs; use a fresh .deps/abc directory')
    print('Building MEOW and ABC', flush=True)
    run_process(['cmake', '-S', ROOT, '-B', BUILD, '-DCMAKE_BUILD_TYPE=Release'])
    run_process(['cmake', '--build', BUILD, '--target', 'meow', 'meow-wce', 'abc', '-j', str(jobs)])


def library():
    path = DEPS / 'nangate45.lib'
    if not path.exists():
        print('Downloading the pinned Nangate45 library', flush=True)
        with tempfile.TemporaryDirectory(dir=DEPS, prefix='library-fetch-') as temporary:
            checkout = Path(temporary) / 'source'
            fetch_source(checkout, MECALS_URL, LIBRARY_REVISION, partial=True)
            data = subprocess.check_output(
                ['git', '-C', str(checkout), 'show',
                 'FETCH_HEAD:input/standard-cell/nangate_45nm_typ.lib'], timeout=180)
        if hashlib.sha256(data).hexdigest() != LIBRARY_SHA256:
            raise ValueError('Downloaded Nangate45 library checksum differs')
        path.write_bytes(data)
    if sha256(path) != LIBRARY_SHA256:
        raise ValueError('Nangate45 library checksum differs')
    return path


def yosys_path(requested):
    local = DEPS / 'yosys/usr/bin/yosys'
    return resolve_executable(requested or (local if local.is_file() else 'yosys'))


def quote(path):
    return '"' + str(path).replace('\\', '\\\\').replace('"', '\\"') + '"'


def map_qor(netlist, work, yosys, lib):
    normalized = work.with_suffix('.aig')
    run_process([yosys, '-p', f'read_aiger {quote(netlist)}; write_aiger {quote(normalized)}'], timeout=600)
    result = run_process([BUILD / 'abc/abc', '-c',
                         f'read_aiger {quote(normalized)}; read {quote(lib)}; strash; dch; amap; stime'], timeout=600)
    clean = re.sub(r'\x1b\[[0-9;]*m', '', result.stdout + result.stderr)
    values = {}
    for name in ('Area', 'Delay'):
        matches = re.findall(name + r'\s*=\s*([0-9]+(?:\.[0-9]+)?)', clean)
        if not matches:
            raise ValueError(f'Missing mapped {name} in ABC output')
        values[name.lower()] = float(matches[-1])
    values['adp'] = values['area'] * values['delay']
    return values


def analysis_metrics(path, outputs):
    width = gates = feasible16 = feasible8 = 0
    with path.open(newline='') as handle:
        for row in csv.DictReader(handle):
            intervals = tuple((int(row[f'{d}_interval_l']), int(row[f'{d}_interval_u']))
                              for d in ('sa0', 'sa1'))
            width += sum(upper - lower for lower, upper in intervals)
            bound = min(max(abs(lower), abs(upper)) for lower, upper in intervals)
            feasible16 += bound * 16 <= 1 << outputs
            feasible8 += bound * 8 <= 1 << outputs
            gates += 1
    return {'gates': gates, 'mean_width': width / (2 * gates) if gates else 0,
            'feasible_m16': feasible16, 'feasible_m8': feasible8}


def select_inputs(args):
    if args.all == bool(args.cases):
        raise ValueError('Specify circuit names/AIG paths, or --all')
    paths = sorted((ROOT / 'testcases').glob('*.aig')) if args.all else [
        Path(name).resolve() if Path(name).is_file() else ROOT / 'testcases' / f'{name}.aig'
        for name in args.cases]
    if len({path.stem for path in paths}) != len(paths):
        raise ValueError('Circuit names must be unique in one run')
    for path in paths:
        if not path.is_file():
            raise FileNotFoundError(path)
    return paths


def parser():
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest='command', required=True)
    compile_cmd = commands.add_parser('build', help='fetch pinned ABC and compile')
    compile_cmd.add_argument('-j', '--jobs', type=int, default=4)
    for mode in ('analysis', 'prune'):
        cmd = commands.add_parser(mode)
        cmd.add_argument('cases', nargs='*', help='paper circuit names or AIG/AAG paths')
        cmd.add_argument('--all', action='store_true', help='all 17 paper circuits')
        cmd.add_argument('--threads', type=int, default=16)
        cmd.add_argument('--output', type=Path, required=True, help='new result directory')
        if mode == 'analysis':
            cmd.add_argument('--timeout', type=float, default=600)
        else:
            cmd.add_argument('--threshold', type=int, action='append', help='repeatable; default: both paper thresholds')
            cmd.add_argument('--budget', type=float, default=1200)
            cmd.add_argument('--yosys', help='Yosys executable; default: local cache or PATH')
    return root


def run(args):
    paths = select_inputs(args)
    if args.threads < 1:
        raise ValueError('threads must be positive')
    if args.command == 'analysis' and (args.timeout <= 0 or not math.isfinite(args.timeout)):
        raise ValueError('Analysis timeout must be finite and positive')
    if args.command == 'prune' and (args.budget <= 0 or not math.isfinite(args.budget) or any(t < 0 for t in args.threshold or [])):
        raise ValueError('Budget must be positive and thresholds nonnegative')
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError('Output directory is not empty; choose a new directory')
    circuits = [(path, read_aiger(path)) for path in paths]
    build()
    yosys, lib = (yosys_path(args.yosys), library()) if args.command == 'prune' else (None, None)
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment['OMP_NUM_THREADS'] = str(args.threads)
    rows = []
    baseline = {}
    for path, circuit in circuits:
        thresholds = [None]
        if args.command == 'prune':
            thresholds = args.threshold or ([1, 2] if path.stem == 'mac' else
                         [math.floor(2 ** (len(circuit.outputs) / d)) for d in (10, 5)])
            thresholds = list(dict.fromkeys(thresholds))
        for threshold in thresholds:
            work = output / (path.stem if threshold is None else f'{path.stem}-t{threshold}')
            work.mkdir()
            prefix = work / 'result'
            command = [BUILD / 'meow', '--input', path, '--output-prefix', prefix,
                       '--mode', args.command, '--threads', str(args.threads)]
            row = {'case': path.stem}
            try:
                if threshold is None:
                    timeout = args.timeout
                else:
                    row['threshold'] = threshold
                    command += ['--threshold', str(threshold), '--prune-timeout-seconds', str(args.budget)]
                    timeout = args.budget + 20
                run_process(command, timeout=timeout, env=environment)
                result = json.loads(prefix.with_suffix('.summary.json').read_text())
                row.update(status='SUCCESS', runtime_s=result['metrics']['elapsed_seconds'])
                if threshold is None:
                    row.update(analysis_metrics(prefix.with_suffix('.gates.csv'), len(circuit.outputs)))
                else:
                    certification = result.get('certification', {})
                    for key, expected in {'status': 'PASS', 'method': 'abc-bsat-step-local-interval',
                                          'acceptance_reference': 'current-step-interval'}.items():
                        if certification.get(key) != expected:
                            raise ValueError('Missing internal SAT certification')
                    netlist = Path(result['artifacts']['netlist'])
                    verification = verify_wce(path, netlist, threshold, abc=BUILD / 'abc/abc', yosys=yosys,
                                              timeout=3600,
                                              exhaustive_threads=args.threads)
                    row.update(verification=verification.status, verification_detail=verification.detail,
                               final_gates=result['metrics']['final_gates'],
                               search_complete=result['search_complete'], termination_reason=result['termination_reason'])
                    if verification.status != 'PASS':
                        raise ValueError(verification.detail)
                    if path not in baseline:
                        baseline[path] = map_qor(path, work / 'baseline', yosys, lib)
                    approximate = map_qor(netlist, work / 'approximate', yosys, lib)
                    row.update(baseline=baseline[path], approximate=approximate,
                               adp_percent=100 * approximate['adp'] / baseline[path]['adp'])
            except (OSError, ValueError, ProcessError) as error:
                row.update(status='ERROR', detail=str(error))
            rows.append(row)
            save_json(work / 'summary.json', row)
            save_json(output / 'summary.json', rows)
            print(json.dumps(row), flush=True)
    return int(any(row['status'] != 'SUCCESS' for row in rows))


def main():
    args = parser().parse_args()
    try:
        if args.command == 'build':
            if args.jobs < 1:
                raise ValueError('jobs must be positive')
            build(args.jobs)
            return 0
        return run(args)
    except (OSError, ValueError, AigerError, ProcessError, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
