"""Build a canonical EE name table from the gt4-decomp name lists (read-only).

Input is a checkout of gt4-decomp; the only files read are config/symbol_addrs.txt and
config/adhoc_methods.txt. Output is a sorted CSV of address, name, kind, origin, quality.
Nothing here touches generated EE code or any production pin (TASK-020, level 1).
"""

import argparse
import csv
from pathlib import Path
import re
import subprocess
import sys

COLUMNS = ['address', 'name', 'kind', 'origin', 'quality']
SYMBOL = re.compile(r'^([A-Za-z_][\w$.@<>:,*&~]*)\s*=\s*0x([0-9A-Fa-f]{1,8});\s*//\s*type:(func|data)(?:\s+(\w+))?\s*$')
PLACEHOLDER = re.compile(r'(?:^func_[0-9A-F]{8}$|__virtual_\d+$)')


def quality(name):
    return 'placeholder' if PLACEHOLDER.search(name) else 'named'


def parse_symbol_addrs(text):
    """Yield one row per `name = 0xADDR; // type:func|data [tag]` line."""
    rows = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith('//'):
            continue
        match = SYMBOL.match(stripped)
        if not match:
            raise ValueError(f'Unrecognised symbol line: {stripped!r}')
        name, address, kind, tag = match.groups()
        rows.append({'address': f'0x{int(address, 16):08X}', 'name': name, 'kind': kind,
                     'origin': 'gthd' if tag == 'gthd' else 'decomp', 'quality': quality(name)})
    return rows


def parse_adhoc_methods(text):
    """Rows for registered script natives: `Class method 0xADDR` becomes `Class::method`."""
    rows = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        fields = stripped.split()
        if len(fields) != 3 or not re.fullmatch(r'0x[0-9A-Fa-f]{1,8}', fields[2]):
            raise ValueError(f'Unrecognised adhoc line: {stripped!r}')
        rows.append({'address': f'0x{int(fields[2], 16):08X}', 'name': f'{fields[0]}::{fields[1]}',
                     'kind': 'func', 'origin': 'adhoc', 'quality': 'named'})
    return rows


def build_table(symbols, adhoc):
    """Merge and sort; the same (address, name) pair keeps its first origin."""
    seen = {}
    for row in symbols + adhoc:
        seen.setdefault((row['address'], row['name']), row)
    return [seen[key] for key in sorted(seen)]


def write_csv(rows, path):
    with open(path, 'w', encoding='utf-8', newline='') as handle:
        writer = csv.DictWriter(handle, COLUMNS, lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def read_csv(path):
    with open(path, encoding='utf-8', newline='') as handle:
        return list(csv.DictReader(handle))


def summary(rows):
    counts = {}
    for row in rows:
        key = (row['kind'], row['origin'], row['quality'])
        counts[key] = counts.get(key, 0) + 1
    return counts


def decomp_commit(root):
    try:
        out = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True,
                             text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decomp', required=True, type=Path, help='gt4-decomp checkout')
    parser.add_argument('--commit', help='gt4-decomp commit to report when the folder is not a git checkout')
    parser.add_argument('--out', type=Path, default=Path('docs/data/ee-names.csv'))
    args = parser.parse_args(argv)
    config = args.decomp / 'config'
    symbols = parse_symbol_addrs((config / 'symbol_addrs.txt').read_text(encoding='utf-8'))
    adhoc = parse_adhoc_methods((config / 'adhoc_methods.txt').read_text(encoding='utf-8'))
    rows = build_table(symbols, adhoc)
    write_csv(rows, args.out)
    print(f'{len(rows)} rows from gt4-decomp {args.commit or decomp_commit(args.decomp)} -> {args.out}')
    for key, count in sorted(summary(rows).items()):
        print('  ', *key, count)
    return 0


if __name__ == '__main__':
    sys.exit(main())
