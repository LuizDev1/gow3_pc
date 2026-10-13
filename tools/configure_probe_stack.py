"""Configure a fully committed 16 MiB Windows diagnostic stack in a PE32+ image.
Guest code does not perform Windows stack probes before large stack adjustments.
Only generated diagnostic binaries may be passed to this utility.
"""
from pathlib import Path
import struct
import sys


def configure(data):
    result = bytearray(data)
    if len(result) < 64 or result[:2] != b'MZ':
        raise ValueError('Not a PE image')
    pe = struct.unpack_from('<I', result, 60)[0]
    if pe > len(result)-24 or result[pe:pe+4] != b'PE\0\0':
        raise ValueError('Invalid PE header')
    machine = struct.unpack_from('<H', result, pe+4)[0]
    optional_size = struct.unpack_from('<H', result, pe+20)[0]
    opt = pe+24
    if machine != 0x8664 or optional_size < 112 or opt+optional_size > len(result):
        raise ValueError('Expected complete AMD64 optional header')
    if struct.unpack_from('<H', result, opt)[0] != 0x20b:
        raise ValueError('Expected PE32+')
    struct.pack_into('<QQ', result, opt+72, 16*1024*1024, 16*1024*1024)
    return result


if __name__ == '__main__':
    path = Path(sys.argv[1])
    path.write_bytes(configure(path.read_bytes()))
