#!/usr/bin/env python3
"""Build offline circuit and bound viewers from paired-bound results.

Generation uses Graphviz (dot); the resulting HTML works without dependencies.
The AAG and CSV must use the same original gate IDs.
"""
import argparse
import csv
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def circuit_graph(path, query_gates):
    """Read the actual AIGER connectivity; Graphviz only determines positions."""
    lines = path.read_text().splitlines()
    header = lines[0].split()
    if header[0] != 'aag' or int(header[3]) != 0:
        raise ValueError('Expected a combinational ASCII AIGER (.aag) file')
    maximum, ni, _, no, na = map(int, header[1:6])
    inputs = [int(x) for x in lines[1:1+ni]]
    outputs = [int(x) for x in lines[1+ni:1+ni+no]]
    gates = [tuple(map(int, x.split())) for x in lines[1+ni+no:1+ni+no+na]]
    if {lhs//2 for lhs, _, _ in gates} != query_gates:
        raise ValueError('AAG AND IDs do not match the query gate IDs')
    names = {}
    for line in lines[1+ni+no+na:]:
        if line == 'c':
            break
        key, _, value = line.partition(' ')
        if key and key[0] in 'io' and key[1:].isdigit():
            names[key] = value
    nodes, edges, levels = [], [], {0: 0}
    used_literals = outputs + [lit for _, a, b in gates for lit in (a, b)]
    if any(lit//2 == 0 for lit in used_literals):
        nodes.append(dict(id='n0', kind='constant', label='0', variable=0, level=0))
    for i, lit in enumerate(inputs):
        if lit & 1:
            raise ValueError('Unexpected inverted primary-input literal')
        v = lit//2
        nodes.append(dict(id=f'n{v}', kind='input', label=names.get(f'i{i}', f'PI{i}'),
                          variable=v, index=i, level=0))
        levels[v] = 0
    for lhs, a, b in gates:
        v = lhs//2
        if lhs & 1 or any(lit//2 >= v for lit in (a, b)):
            raise ValueError('Expected topologically ordered AND gates')
        levels[v] = 1 + max(levels[a//2], levels[b//2])
        nodes.append(dict(id=f'n{v}', kind='and', label=f'g{v}', variable=v, level=levels[v]))
        for pin, lit in enumerate((a, b)):
            edges.append(dict(id=f'e{len(edges)}', source=f'n{lit//2}', target=f'n{v}',
                              inverted=bool(lit & 1), pin=pin, literal=lit))
    for i, lit in enumerate(outputs):
        nodes.append(dict(id=f'p{i}', kind='output', label=names.get(f'o{i}', f'PO{i}'),
                          index=i, level=levels[lit//2]+1, weight=1 << i))
        edges.append(dict(id=f'e{len(edges)}', source=f'n{lit//2}', target=f'p{i}',
                          inverted=bool(lit & 1), pin=0, literal=lit))
    dot = ['digraph circuit {',
           'graph [rankdir=LR, nodesep=0.28, ranksep=0.58, splines=spline, margin=0.2];',
           'node [shape=box, fixedsize=true, label=""];',
           'edge [arrowhead=none, tailport=e, headport=w];']
    for n in nodes:
        w, h = (170, 86) if n['kind'] == 'and' else (108, 44)
        n.update(width=w, height=h)
        dot.append(f'"{n["id"]}" [width={w/72:.6f}, height={h/72:.6f}];')
    dot.append('{rank=same; ' + '; '.join(f'"{n["id"]}"' for n in nodes if n['kind'] == 'input') + ';}')
    dot.append('{rank=same; ' + '; '.join(f'"{n["id"]}"' for n in nodes if n['kind'] == 'output') + ';}')
    for e in edges:
        dot.append(f'"{e["source"]}" -> "{e["target"]}" [id="{e["id"]}"];')
    dot.append('}')
    layout = json.loads(subprocess.run(['dot', '-Tjson'], input='\n'.join(dot),
                                      text=True, capture_output=True, check=True, timeout=60).stdout)
    bb = list(map(float, layout['bb'].split(',')))
    height = bb[3]
    locations = {n['name']: n for n in layout['objects'] if 'pos' in n}
    node_map = {n['id']: n for n in nodes}
    for n in nodes:
        x, y = map(float, locations[n['id']]['pos'].split(','))
        n.update(x=round(x, 3), y=round(height-y, 3))
    routes = {e['id']: e for e in layout['edges']}
    for e in edges:
        curves = [op['points'] for op in routes[e['id']].get('_draw_', []) if op['op'] == 'b']
        if len(curves) != 1 or (len(curves[0])-1) % 3:
            raise ValueError('Expected one cubic spline per edge')
        points = [[round(x, 3), round(height-y, 3)] for x, y in curves[0]]
        target = node_map[e['target']]
        # Two visibly separate AND input ports. Inversion bubbles sit outside the box.
        endpoint_y = target['y'] + ((-18 if e['pin'] == 0 else 18) if target['kind'] == 'and' else 0)
        endpoint_x = target['x'] - target['width']/2 - (10 if e['inverted'] else 0)
        points[-1] = [endpoint_x, endpoint_y]
        points[-2] = [min(points[-2][0], endpoint_x-12), endpoint_y]
        e['points'] = points
    return dict(nodes=nodes, edges=edges, width=bb[2], height=height,
                max_level=max(n['level'] for n in nodes), source_aag=path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--csv', type=Path, default=ROOT / 'results/paired-absdiff/comparison.csv')
    parser.add_argument('--summary', type=Path, default=ROOT / 'results/paired-absdiff/absdiff.json')
    parser.add_argument('--circuit', default='absdiff')
    parser.add_argument('--aag', type=Path, default=ROOT / 'results/paired-absdiff/absdiff.aag')
    parser.add_argument('--output', type=Path, default=HERE / 'index.html')
    args = parser.parse_args()
    required = ('gate', 'force', 'bound_l', 'bound_u', 'exact_l', 'exact_u')
    optional = ('exact_bitwise_l', 'exact_bitwise_u', 'meow_l', 'meow_u',
                'min_witness', 'max_witness', 'signal_violations', 'output_violation_patterns')
    with args.csv.open(newline='') as source:
        reader = csv.DictReader(source)
        if not set(required).issubset(reader.fieldnames or []):
            raise ValueError('CSV must contain: ' + ', '.join(required))
        rows = [{key: int(row[key]) for key in required + optional
                 if row.get(key) not in (None, '')} for row in reader]
    if not rows:
        raise ValueError('No query results found')
    seen = set()
    for row in rows:
        key = row['gate'], row['force']
        if key in seen or row['force'] not in (0, 1):
            raise ValueError(f'Duplicate/invalid query: {key}')
        if row['bound_l'] > row['bound_u'] or row['exact_l'] > row['exact_u']:
            raise ValueError(f'Invalid interval: {key}')
        seen.add(key)
    gates = {row['gate'] for row in rows}
    if len(rows) != len(gates) * 2:
        raise ValueError('Each gate must have force=0 and force=1 results')
    metadata = json.loads(args.summary.read_text())
    if metadata.get('queries', len(rows)) != len(rows):
        raise ValueError('Summary query count does not match CSV')
    dataset = {'circuit': args.circuit, 'metadata': metadata,
               'rows': sorted(rows, key=lambda r: (r['gate'], r['force']))}
    dataset['graph'] = circuit_graph(args.aag, gates)
    payload = json.dumps(dataset, ensure_ascii=False, separators=(',', ':'))
    template = (HERE / 'template.html').read_text()
    if template.count('__BOUND_DATA__') != 1:
        raise ValueError('Expected one data placeholder')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(template.replace('__BOUND_DATA__', payload.replace('<', '\\u003c')))
    table_template = (HERE / 'bounds-template.html').read_text()
    table_payload = json.dumps({k: v for k, v in dataset.items() if k != 'graph'},
                               ensure_ascii=False, separators=(',', ':')).replace('<', '\\u003c')
    (args.output.parent / 'bounds.html').write_text(table_template.replace('__BOUND_DATA__', table_payload))
    print(f'{args.output}: {len(gates)} gates / {len(rows)} queries / {args.output.stat().st_size:,} bytes')


if __name__ == '__main__':
    main()
