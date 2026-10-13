#!/usr/bin/env python3
"""Inspect a plaintext game dump without preparing, patching, or executing it.

Outputs metadata and import identities only; never copies game executable bytes.
Static runtime string matches are diagnostic hints, not proof of implementation.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from link_modules import module
from prepare import nid, sfo, span


def inspect(game):
    metadata = sfo((game / 'sce_sys/param.sfo').read_bytes())
    labels = {}
    for raw, label in re.findall(r'\{"([^"\n]+)","([^"\n]+)"\}',
                                 (ROOT / 'src/import_names.inc').read_text()):
        labels.setdefault(raw.split('#')[0], label)
    source = '\n'.join(p.read_text(errors='replace') for p in
                       [*ROOT.joinpath('src').glob('*.c'),
                        *ROOT.joinpath('gpu/shim').glob('*.cpp')])
    literal_names = set(re.findall(r'"([^"\n]*#[^"\n]*)"', source))
    named_exports = {nid(name): name for name in
                     re.findall(r'\{\s*"([A-Za-z_][A-Za-z_0-9]*)"\s*,', source)}
    for key, name in named_exports.items():
        labels.setdefault(key, name)
    files = [game / 'eboot.bin', *sorted((game / 'sce_module').glob('*.prx'))]
    results = []
    for path in files:
        m = module(path)
        loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
        size = max(p['vaddr'] + p['memsz'] for p in loads)
        if size > 512 * 1024 * 1024:
            raise ValueError('loaded image exceeds inspection limit')
        image = bytearray(size)
        for p in loads:
            if p['filesz'] > p['memsz']:
                raise ValueError('segment file size exceeds memory size')
            image[p['vaddr']:p['vaddr'] + p['filesz']] = span(m['elf'], p['offset'], p['filesz'])
        imports = []
        for symbol in m['symbols']:
            if symbol['section'] or not symbol['name']:
                continue
            identity = symbol['identity']
            imports.append(dict(symbol=symbol['name'],
                                diagnostic_name=labels.get(symbol['name'].split('#')[0]),
                                identity=identity,
                                named_runtime_export_candidate=named_exports.get(symbol['name'].split('#')[0]),
                                exact_literal_in_runtime_sources=symbol['name'] in literal_names))
        results.append(dict(file=path.relative_to(game).as_posix(),
                            source_sha256=m['sha256'],
                            loaded_image_sha256=hashlib.sha256(image).hexdigest(),
                            entry=hex(m['header'][4]), image_bytes=size,
                            libraries=m['libraries'], modules=m['modules'],
                            relocations=dict(collections.Counter(r[1] for r in m['relocs'])),
                            unavailable_metadata_headers=m['missing'], imports=imports))
    return dict(metadata={k: metadata.get(k) for k in
                          ('TITLE', 'TITLE_ID', 'APP_VER', 'VERSION', 'SYSTEM_VER')},
                status='Static inspection only. No loading or execution performed.',
                caveat='Literal matches do not prove ABI compatibility or runtime coverage; '
                       'missing matches may be served by native module exports or other resolvers.',
                files=results)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'out/target-inspection.json')
    args = parser.parse_args()
    try:
        report = inspect(args.game)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        print(json.dumps(report['metadata'], ensure_ascii=True))
        for item in report['files']:
            hits = sum(i['exact_literal_in_runtime_sources'] for i in item['imports'])
            print(f"{item['file']}: entry={item['entry']}, image={item['image_bytes']}, "
                  f"imports={len(item['imports'])}, exact runtime literals={hits}")
        print(f'Report: {args.out}')
    except (ValueError, OSError, KeyError, IndexError, StopIteration) as error:
        parser.exit(1, f'Inspection failed: {error}\n')
