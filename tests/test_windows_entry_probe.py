"""Execute synthetic guest code only; no game files are used by CI."""
from paths import ROOT
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

EXE = Path(os.environ.get('TLG_ENTRY_PROBE', str(ROOT / 'out/tlg-entry-probe.exe')))


def package(code, relocs=(), imports=(), flags=5, capabilities=0):
    image = code.ljust(4096, b'\0')
    return (struct.pack('<8s6Q', b'BBPROBE2', len(image), 0, 1, len(relocs), len(imports), capabilities)
            + struct.pack('<3Q', 0, len(image), flags)
            + b''.join(name.encode().ljust(128, b'\0') for name in imports)
            + b''.join(struct.pack('<4Q', *r) for r in relocs) + image)


@unittest.skipUnless(os.name == 'nt' and EXE.is_file(), 'build the Windows entry probe first')
class WindowsEntryTests(unittest.TestCase):
    def test_mutex_attribute_lifecycle_and_invalid_handles(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-mutex'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('type validation, isolation and stale handles passed', result.stdout)

    def test_event_queue_lifecycle_and_invalid_handles(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-equeue'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('lifecycle, invalid handles, invalid inputs and exact scope checks passed', result.stdout)
        self.assertIn('replacement, isolation, explicit signaling and deletion checks passed', result.stdout)

    def test_atexit_callbacks_finalize_in_reverse_order_exactly_once(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-atexit'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('exactly-once checks passed', result.stdout)

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

    def test_verified_init_env_returns_then_reaches_next_import(self):
        # Align the SysV stack, call the first import, then tail-jump to the second.
        code = (b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
                b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        result = self.run_image(package(code, [(32, 1, 0, 0), (40, 1, 1, 0)],
                                       ['bzQExy189ZI#V#V', 'after-init'], capabilities=1))
        self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
        self.assertIn('Verified _init_env calls: 1', result.stdout)
        self.assertIn('first unsupported PS4 import: after-init', result.stdout)

    def test_init_env_requires_preparation_evidence_and_exact_scope(self):
        for name, capabilities in [('bzQExy189ZI#V#V', 0), ('bzQExy189ZI#q#q', 1)]:
            with self.subTest(name=name, capabilities=capabilities):
                result = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90',
                                                [(8, 1, 0, 0)], [name], capabilities=capabilities))
                self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
                self.assertIn('Verified _init_env calls: 0', result.stdout)
                self.assertNotIn('Runtime: _init_env returned', result.stdout)

    def test_guest_fault_reports_offset(self):
        result = self.run_image(package(b'\x0f\x0b'))
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn('guest offset 0x0', result.stdout)

    def test_verified_stack_canary_data_is_readable(self):
        code = b'\x48\x8b\x05\x09\0\0\0\x48\x8b\x00\x0f\x0b'
        result = self.run_image(package(code, [(16, 2, 0, 0)],
                                       ['f7uOxY9mM1U#U#H'], capabilities=1))
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn('guest offset 0xa', result.stdout)
        self.assertNotIn('Unsupported data import', result.stdout)

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
