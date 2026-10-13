"""Keep Linux TLS behavior intact while Windows preparation records original sites."""
from paths import ROOT
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import link_modules
import test_link_libc
from test_windows_entry_probe import package


class ModulePreparationTests(unittest.TestCase):
    def check_mode(self, mode):
        main, libc = test_link_libc.LinkTests().fixture()
        main['ph'].insert(0, dict(type=1, vaddr=0, memsz=4096, filesz=128, flags=5))
        raw = package(link_modules.FS_LOAD + b'\xc3', [(32, 1, 0, 0)], ['fixture#q#q'], capabilities=1)
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out / 'boot.bin').write_bytes(raw)
            with patch.object(link_modules, 'module', side_effect=[main, libc]):
                link_modules.link(Path('fixture'), out, ('libc.prx',), tls_mode=mode)
            blob = (out / 'boot-linked.bin').read_bytes()
            size = struct.unpack_from('<Q', blob, 8)[0]
            return blob[:8], blob[-size:], json.loads((out / 'link.json').read_text())

    def test_windows_preserves_fs_and_records_sites_and_identities(self):
        magic, image, report = self.check_mode('windows-prepare')
        self.assertEqual(magic, b'TLGPRP01')
        self.assertEqual(image[:9], link_modules.FS_LOAD)
        self.assertEqual(report['windows_tls_sites'], [0])
        self.assertEqual(report['fs_loads_patched'], 0)
        self.assertEqual(report['canonical_imports'][0]['kind'], 'function')

    def test_linux_keeps_existing_rewrite(self):
        magic, image, report = self.check_mode('linux')
        self.assertEqual(magic, b'BBPROBE5')
        self.assertEqual(image[:9], b'\x65' + link_modules.FS_LOAD[1:])
        self.assertEqual(report['fs_loads_patched'], 1)
