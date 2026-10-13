"""Canonical import identities, independent of each module's local suffixes.

Keep library/module versions and ELF function/data kinds. Never merge symbols
by NID alone or treat equal local spellings in different modules as equal.
"""
import json


def build_manifest(modules, target):
    records = {}
    for source, module in modules.items():
        for _, relocation, index, _ in module['relocs']:
            if relocation not in (1, 6, 7):
                continue
            symbol = module['symbols'][index]
            if symbol['section']:
                continue
            identity = symbol['identity']
            if identity is None:
                if symbol['binding'] == 2:
                    continue  # unresolved weak module_start/stop are optional
                raise ValueError(f'{source}: unscoped required import {symbol["name"]}')
            nid, (library, lib_version), (owner, mod_version) = identity
            if symbol['type'] not in (1, 2):
                raise ValueError(f'{source}: unsupported import symbol type {symbol["type"]}')
            kind = 'data' if symbol['type'] == 1 else 'function'
            key = json.dumps([nid, library, lib_version, owner, mod_version, kind], separators=(',', ':'))
            record = records.setdefault(key, dict(canonical_id=key, nid=nid,
                library={'name': library, 'version': lib_version},
                module={'name': owner, 'version': mod_version}, kind=kind, references=[]))
            reference = {'source': source, 'symbol': symbol['name']}
            if reference not in record['references']:
                record['references'].append(reference)
    return dict(schema_version=1, target=target,
                status='Identity metadata only; not a runtime resolver or compatibility claim.',
                module_source_sha256={source: item['sha256'] for source, item in modules.items()},
                imports=[records[key] for key in sorted(records)])
