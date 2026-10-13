#!/usr/bin/env python3
"""Prepare the observed experimental TLG target without any Bloodborne patches.

Offline preparation only. The result requires the Linux host loader and does not
claim runtime compatibility. Source game files are read, never modified.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path

from prepare import prepare, sfo, span
from link_modules import link, module
from import_manifest import build_manifest
from windows_boot import encode_boot

TITLE = 'CUSA03627'
VERSION = '01.03'
IMAGE = 'da302cb58cfe4b3711e81458c416e761c291ffea76d194bbeac9efad08b4afe8'
MODULE_HASHES = {
    'libc.prx': '8439e7658c4123e9910d4f6062a76389626af73d9e90c24561d7f100d9f345e5',
    'libSceFios2.prx': '98d3861eed4016fe0c856f3f6ab5b14bb91231f53a47b3e4b5c2439a3ee0360c',
}


def validate(game, image_hash):
    """Require observed identity AND image; independent of BB_SKIP_GAME_CHECK."""
    info = sfo((game / 'sce_sys/param.sfo').read_bytes())
    if (info.get('TITLE_ID'), info.get('APP_VER')) != (TITLE, VERSION):
        raise ValueError(f'Experimental target requires {TITLE} {VERSION}')
    if image_hash != IMAGE:
        raise ValueError('Unrecognized TLG executable image; inspect before enabling it')
    return None


def build(game, out):
    # Pin bundled modules too: their code, TLS and initialization have not yet
    # been validated at runtime. A matching filename is not sufficient evidence.
    parsed = {}
    for name, expected in MODULE_HASHES.items():
        item = module(game / 'sce_module' / name)
        loads = [p for p in item['ph'] if p['type'] in (1, 0x61000010)]
        size = max(p['vaddr'] + p['memsz'] for p in loads)
        if size > 512 * 1024 * 1024:
            raise ValueError(f'{name}: loaded image exceeds limit')
        image = bytearray(size)
        for p in loads:
            if p['filesz'] > p['memsz']:
                raise ValueError(f'{name}: invalid segment size')
            image[p['vaddr']:p['vaddr'] + p['filesz']] = span(item['elf'], p['offset'], p['filesz'])
        if hashlib.sha256(image).hexdigest() != expected:
            raise ValueError(f'{name}: unrecognized module image')
        parsed[name] = item
    prepare(game, out, validator=validate)
    link(game, out)
    # Resolve dump-local spellings to full identities for further runtime work.
    main = module(game / 'eboot.bin')
    manifest = build_manifest({'eboot.bin': main,
        **{'sce_module/' + name: item for name, item in parsed.items()}},
        {'title_id': TITLE, 'app_ver': VERSION, 'image_sha256': IMAGE})
    (out / 'imports-canonical.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (out / 'boot-windows.bin').write_bytes(encode_boot((out / 'boot.bin').read_bytes(), manifest))
    identities = {}
    by_identity = {}
    for item in [main, *parsed.values()]:
        for symbol in item['symbols']:
            if not symbol['section'] and symbol['identity']:
                identities.setdefault(symbol['name'], symbol['identity'])
                key, library, mod = symbol['identity']
                by_identity[(key, library, mod)] = symbol['identity']
    report = json.loads((out / 'link.json').read_text())
    report['target'] = {'title_id': TITLE, 'app_ver': VERSION, 'image_sha256': IMAGE}
    report['status'] = 'Prepared and linked only. No host runtime or GPU execution validated.'
    report['host_platform'] = 'Linux x86-64 (inherited FS-to-GS TLS rewrite)'
    report['boot_linked_sha256'] = hashlib.sha256((out / 'boot-linked.bin').read_bytes()).hexdigest()
    report['windows_diagnostic'] = dict(file='boot-windows.bin', format='TLGWIN01',
        linked_modules=False, tls_rewrite=False,
        sha256=hashlib.sha256((out / 'boot-windows.bin').read_bytes()).hexdigest())
    report['bloodborne_patches_applied'] = False
    host_imports = []
    for name in report['unresolved_imports']:
        identity = identities.get(name)
        if identity is None and name.count('#') == 2:
            identity = main['identity'](name)
        if identity is None:
            # link_modules spells imports absent from the main tables as
            # NID#library. Recover only an unambiguous original identity.
            key, library = name.split('#', 1)
            candidates = [value for value in by_identity.values()
                          if value[0] == key and value[1][0] == library]
            if len(candidates) != 1:
                raise ValueError(f'Ambiguous host import identity: {name}')
            identity = candidates[0]
        host_imports.append({'symbol': name, 'identity': identity})
    report['host_import_identities'] = host_imports
    report['host_imports_by_library'] = dict(collections.Counter(
        item['identity'][1][0] for item in host_imports))
    (out / 'tlg-link.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    windows_native = out / 'windows-native'
    windows_native.mkdir(parents=True, exist_ok=True)
    (windows_native / 'boot.bin').write_bytes((out / 'boot.bin').read_bytes())
    link(game, windows_native, tls_mode='windows-prepare')
    print('Experimental TLG image ready for host-runtime development; not ready to play.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parents[1] / 'out/tlg')
    args = parser.parse_args()
    try:
        build(args.game, args.out)
    except (OSError, ValueError, KeyError, IndexError, StopIteration) as error:
        parser.exit(1, f'TLG preparation failed: {error}\n')
