#!/usr/bin/env python3
"""Compare validated import metadata with source candidates; never claims compatibility.

Input contains import identities only, not game executable or asset bytes.
Reference paths are used locally and are not written into the report.
"""
import argparse
import collections
import json
import hashlib
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from prepare import nid
from prepare_tlg import IMAGE


def audit(report, reference):
    if report.get('target', {}).get('image_sha256') != IMAGE:
        raise ValueError('Expected metadata from the validated TLG USA 1.03 image')
    direct, named, graphics = collections.defaultdict(list), collections.defaultdict(list), set()
    for path in sorted((reference / 'src').glob('runtime*.c')):
        source = path.read_text(errors='replace')
        for symbol in re.findall(r'strcmp\(name,\s*"([^"\n]+)"\)', source):
            direct[symbol].append(path.name)
        for name in re.findall(r'\{\s*"([A-Za-z_][A-Za-z_0-9]*)"\s*,', source):
            named[nid(name)].append(path.name)
    for path in (reference / 'gpu/shadps4/core/libraries').rglob('*.cpp'):
        graphics.update(re.findall(r'LIB_FUNCTION\(\s*"([^"]+)",\s*"([^"]+)",\s*[^,]+,\s*"([^"]+)"',
                                  path.read_text(errors='replace')))
    current = set()
    for path in (ROOT / 'src').glob('windows*.c'):
        current.update(re.findall(r'strcmp\(name,\s*"([^"\n]+)"\)', path.read_text()))
    rows, details = collections.defaultdict(collections.Counter), []
    for item in report['host_import_identities']:
        key, library, module = item['identity']
        lib, mod = library[0], module[0]
        canonical = key + '#' + lib
        alias = lib == 'libScePosix' and key + '#libkernel' in direct
        candidate = ('scoped_runtime' if canonical in direct or alias else
                     'named_runtime' if key in named else
                     'graphics_registration' if (key, lib, mod) in graphics else 'unmatched')
        rows[lib]['total'] += 1
        rows[lib][candidate] += 1
        rows[lib]['current_literals'] += item['symbol'] in current
        details.append(dict(symbol=item['symbol'], identity=item['identity'], candidate=candidate,
                            source_candidates=direct.get(canonical, []) or named.get(key, []),
                            posix_alias_candidate=bool(alias)))
    return dict(caveat='Static candidates only: not ABI, build, runtime or gameplay validation. '
                       'Named exports are NID-only hints. GPU registrations may not be enabled by the host.',
                target=report['target'], native_bindings=report['bindings'],
                reference_fingerprints={name: hashlib.sha256((reference / name).read_bytes()).hexdigest()
                                        for name in ('src/runtime.c', 'src/runtime_thread.c',
                                                     'src/runtime_memory.c', 'src/compat_win.c',
                                                     'gpu/CMakeLists.txt') if (reference / name).is_file()},
                host_import_entries=len(details), libraries=dict(rows), details=details)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--input', type=Path, default=ROOT / 'out/tlg/tlg-link.json')
    parser.add_argument('--out', type=Path, default=ROOT / 'out/runtime-audit.json')
    args = parser.parse_args()
    result = audit(json.loads(args.input.read_text()), args.reference)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print('Library | Host entries | Current literals | Scoped runtime | Name-only | Graphics | Unmatched')
    for lib, row in sorted(result['libraries'].items()):
        print(lib + ' | ' + ' | '.join(str(row.get(k, 0)) for k in
              ('total', 'current_literals', 'scoped_runtime', 'named_runtime', 'graphics_registration', 'unmatched')))
    print(result['caveat'])
