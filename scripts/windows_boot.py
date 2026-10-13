"""Attach complete import identities to an unlinked Windows diagnostic image.

TLGWIN01 preserves the BBPROBE2 layout, adding 152-byte identity records after
import names. It performs no TLS rewrite, module linking or game-code patching.
"""
import struct

IDENTITY = struct.Struct('<12s64sI64sII')


def field(value, size):
    raw = value.encode('ascii')
    if not raw or b'\0' in raw or len(raw) >= size:
        raise ValueError('Invalid canonical identity string')
    return raw


def encode_boot(raw, manifest):
    if manifest.get('schema_version') != 1:
        raise ValueError('Unknown import manifest schema')
    magic, size, entry, ns, nr, ni, flags = struct.unpack_from('<8s6Q', raw)
    if magic != b'BBPROBE2' or not (0 < size <= 512 * 1024 * 1024 and
            entry < size and 0 < ns <= 64 and nr <= 1000000 and ni <= 100000 and flags <= 1):
        raise ValueError('Expected bounded unlinked BBPROBE2 image')
    start = 56 + ns * 24
    end = start + ni * 128
    if len(raw) != end + nr * 32 + size:
        raise ValueError('Incorrect boot image length')
    by_name = {}
    for item in manifest['imports']:
        for reference in item['references']:
            if reference['source'] == 'eboot.bin':
                name = reference['symbol']
                if name in by_name and by_name[name] != item:
                    raise ValueError('Ambiguous main import identity')
                by_name[name] = item
    identities = []
    for i in range(ni):
        name = raw[start + i * 128:start + (i + 1) * 128].split(b'\0', 1)[0].decode('ascii')
        item = by_name[name]
        if item['nid'] != name.split('#')[0] or item['kind'] not in ('data', 'function'):
            raise ValueError('Import identity mismatch')
        lv, mv = item['library']['version'], item['module']['version']
        if not (0 <= lv <= 65535 and 0 <= mv <= 65535):
            raise ValueError('Invalid identity version')
        identities.append(IDENTITY.pack(field(item['nid'], 12), field(item['library']['name'], 64), lv,
            field(item['module']['name'], 64), mv, 1 if item['kind'] == 'data' else 2))
    return b'TLGWIN01' + raw[8:end] + b''.join(identities) + raw[end:]


def encode_linked(raw, report):
    """TLGWIN02 carries module/binding metadata and Windows TLS patch sites."""
    magic, size, entry, ns, nr, ni, flags = struct.unpack_from('<8s6Q', raw)
    if magic != b'TLGPRP01' or report.get('tls_mode') != 'windows-prepare':
        raise ValueError('Expected untouched Windows module preparation')
    if not (0 < size <= 512 * 1024 * 1024 and entry < size and 0 < ns <= 64 and
            nr <= 1000000 and ni <= 100000 and flags <= 1):
        raise ValueError('Linked image limits exceeded')
    count = struct.unpack_from('<Q', raw, 96)[0]
    if count > 15: raise ValueError('Too many native modules')
    bindings_at = 104 + count * 56
    bindings = struct.unpack_from('<Q', raw, bindings_at)[0]
    if bindings > ni: raise ValueError('Invalid binding count')
    metadata_end = bindings_at + 8 + bindings * 24
    names_start = metadata_end + ns * 24
    names_end = names_start + ni * 128
    if len(raw) != names_end + nr * 32 + size: raise ValueError('Incorrect linked image length')
    records = {item['index']: item for item in report['canonical_imports']}
    if len(records) != ni: raise ValueError('Incomplete canonical import records')
    identities = []
    for i in range(ni):
        item = records[i]
        nid, (lib, lv), (mod, mv) = item['identity']
        if item['kind'] not in ('data', 'function') or not (0 <= lv <= 65535 and 0 <= mv <= 65535):
            raise ValueError('Invalid linked identity')
        name = raw[names_start+i*128:names_start+(i+1)*128].split(b'\0',1)[0].decode('ascii')
        if nid != name.split('#')[0]: raise ValueError('Linked identity NID mismatch')
        identities.append(IDENTITY.pack(field(nid,12),field(lib,64),lv,field(mod,64),mv,
                                       1 if item['kind']=='data' else 2))
    sites = report['windows_tls_sites']
    if len(sites) > 1000000 or len(set(sites)) != len(sites): raise ValueError('Invalid TLS sites')
    image = raw[-size:]
    for at in sites:
        if not 0 <= at <= size-9 or image[at:at+9] != bytes.fromhex('64488b042500000000'):
            raise ValueError('TLS site does not match original instruction bytes')
    patches = struct.pack('<Q',len(sites)) + b''.join(struct.pack('<Q',at) for at in sites)
    return b'TLGWIN02'+raw[8:metadata_end]+patches+raw[metadata_end:names_end]+b''.join(identities)+raw[names_end:]
