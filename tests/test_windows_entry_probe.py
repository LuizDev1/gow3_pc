"""Execute synthetic guest code only; no game files are used by CI."""
from paths import ROOT
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

EXE = Path(os.environ.get('TLG_ENTRY_PROBE', str(ROOT / 'out/tlg-entry-probe.exe')))


def package(code, relocs=(), imports=(), flags=5):
    image = code.ljust(4096, b'\0')
    return (struct.pack('<8s6Q', b'BBPROBE2', len(image), 0, 1, len(relocs), len(imports), 0)
            + struct.pack('<3Q', 0, len(image), flags)
            + b''.join(name.encode().ljust(128, b'\0') for name in imports)
            + b''.join(struct.pack('<4Q', *r) for r in relocs) + image)


@unittest.skipUnless(os.name == 'nt' and EXE.is_file(), 'build the Windows entry probe first')
class WindowsEntryTests(unittest.TestCase):
    def run_image(self, data):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'fixture.bin'
            image.write_bytes(data)
            return subprocess.run([str(EXE.resolve()), str(image)], capture_output=True,
                                  text=True, timeout=15)

    def test_original_entry_reaches_named_import(self):
        result = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90',
                                        [(8, 1, 0, 0)], ['fixture-import']))
        self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
        self.assertIn('first unsupported PS4 import: fixture-import', result.stdout)

    def test_unsupported_data_import_is_named(self):
        code = b'\x48\x8b\x05\x09\0\0\0\x48\x8b\x00\xc3'
        result = self.run_image(package(code, [(16, 2, 0, 0)], ['fixture-data']))
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn('Unsupported data import: fixture-data', result.stdout)

    def test_guest_fault_reports_offset(self):
        result = self.run_image(package(b'\x0f\x0b'))
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn('guest offset 0x0', result.stdout)

    def test_bad_images_fail_before_execution(self):
        cases = [package(b'\xc3')[:-1], package(b'\xc3') + b'extra',
                 package(b'\xc3', flags=4),
                 package(b'\xc3', [(4090, 0, 0, 0)]),
                 b'BBPROBE5' + package(b'\xc3')[8:]]
        for data in cases:
            with self.subTest(header=data[:8], length=len(data)):
                result = self.run_image(data)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertNotIn('Entering original', result.stdout)


if __name__ == '__main__':
    unittest.main()
