import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('configure_probe_stack', Path(__file__).resolve().parents[1]/'tools/configure_probe_stack.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ProbeStackTests(unittest.TestCase):
    def image(self):
        data = bytearray(256)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 64)
        data[64:68] = b'PE\0\0'
        struct.pack_into('<H', data, 68, 0x8664)
        struct.pack_into('<H', data, 84, 112)
        struct.pack_into('<H', data, 88, 0x20b)
        return data

    def test_changes_only_stack_fields_and_is_idempotent(self):
        original = self.image()
        result = module.configure(original)
        self.assertEqual(struct.unpack_from('<QQ', result, 160), (16777216, 16777216))
        self.assertEqual(result[:160], original[:160])
        self.assertEqual(result[176:], original[176:])
        self.assertEqual(module.configure(result), result)

    def test_rejects_invalid_truncated_and_wrong_architecture(self):
        wrong = self.image()
        struct.pack_into('<H', wrong, 68, 0x14c)
        for data in (b'', b'MZ', self.image()[:100], wrong):
            with self.assertRaises(ValueError):
                module.configure(data)

    def test_packaging_rejects_reserved_but_uncommitted_guest_stack(self):
        data = self.image()
        struct.pack_into('<QQ',data,160,33554432,4096)
        with self.assertRaisesRegex(ValueError,'fully committed'):
            module.require_guest_stack(data)
        struct.pack_into('<QQ',data,160,33554432,33554432)
        module.require_guest_stack(data)
        module.require_guest_stack(module.configure(self.image()))
        struct.pack_into('<QQ',data,160,4096,4096)
        with self.assertRaisesRegex(ValueError,'at least 16 MiB'):
            module.require_guest_stack(data)
