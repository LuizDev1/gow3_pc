"""Keep Linux TLS behavior intact while Windows preparation records original sites."""
from paths import ROOT
import json
from pathlib import Path
import struct
import tempfile
import unittest
import os
import subprocess
from unittest.mock import patch
import link_modules
import test_link_libc
from test_windows_entry_probe import package
from test_windows_entry_probe import EXE
from windows_boot import encode_linked


class ModulePreparationTests(unittest.TestCase):
    def linked_fixture(self):
        main, libc = test_link_libc.LinkTests().fixture()
        main['identity'] = lambda name: (name.split('#')[0], ('libc',1), ('libc',1))
        main['ph'].insert(0, dict(type=1,vaddr=0,memsz=4096,filesz=128,flags=5))
        code = (b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
                b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        raw=package(code,[(32,1,0,0),(40,1,1,0)],['fixture#q#q','after-lib#q#q'],capabilities=1)
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder); (out/'boot.bin').write_bytes(raw)
            with patch.object(link_modules,'module',side_effect=[main,libc]):
                link_modules.link(Path('fixture'),out,('libc.prx',),tls_mode='windows-prepare')
            return encode_linked((out/'boot-linked.bin').read_bytes(),json.loads((out/'link.json').read_text()))

    @unittest.skipUnless(os.name=='nt' and EXE.is_file(),'build Windows probe first')
    def test_windows_initializes_native_module_and_calls_native_export(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'native.bin'; path.write_bytes(self.linked_fixture())
            result=subprocess.run([str(EXE.resolve()),str(path)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('Native module 0 returned 0',result.stdout)
            self.assertIn('first unsupported PS4 import: after-lib',result.stdout)

    @unittest.skipUnless(os.name=='nt' and EXE.is_file(),'build Windows probe first')
    def test_native_binding_type_mismatch_fails_before_execution(self):
        data=bytearray(self.linked_fixture())
        struct.pack_into('<Q',data,184,2)  # binding kind after one module record
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'invalid.bin'; path.write_bytes(data)
            result=subprocess.run([str(EXE.resolve()),str(path)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,1,result.stdout+result.stderr)
            self.assertIn('invalid native binding identity',result.stderr)
            self.assertNotIn('Starting native module',result.stdout)

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
