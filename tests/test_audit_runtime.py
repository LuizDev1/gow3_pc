"""Static audit must distinguish scoped evidence, name hints and missing matches."""
from paths import ROOT
import sys
sys.path.insert(0, str(ROOT / 'tools'))
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import audit_runtime


class AuditTests(unittest.TestCase):
    def test_scopes_and_modules_are_not_dropped(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'src').mkdir()
            (root / 'gpu/shadps4/core/libraries').mkdir(parents=True)
            (root / 'src/runtime.c').write_text('if (!strcmp(name, "fixture#libc")) return 1;')
            (root / 'gpu/shadps4/core/libraries/gpu.cpp').write_text(
                'LIB_FUNCTION("gpu", "graphics", 1, "driver", function);')
            entries = [('fixture', 'libc', 'libc'), ('fixture', 'other', 'other'),
                       ('gpu', 'graphics', 'driver'), ('gpu', 'graphics', 'wrong')]
            report = dict(target={'image_sha256': audit_runtime.IMAGE}, bindings=0,
                          host_import_identities=[dict(symbol=f'{key}#A#B',
                              identity=[key, [lib, 1], [mod, 257]]) for key, lib, mod in entries])
            with patch.object(audit_runtime, 'ROOT', root):
                result = audit_runtime.audit(report, root)
            self.assertEqual([r['candidate'] for r in result['details']],
                             ['scoped_runtime', 'unmatched', 'graphics_registration', 'unmatched'])
            self.assertEqual(sum(v['total'] for v in result['libraries'].values()), 4)

    def test_unknown_target_is_rejected(self):
        with self.assertRaises(ValueError):
            audit_runtime.audit({'target': {'image_sha256': 'unknown'}}, ROOT)
