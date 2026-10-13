from paths import ROOT
import unittest
from import_manifest import build_manifest


def fixture(name='N#A#B', library=('libc', 1), module=('libc', 257), kind=2):
    return dict(sha256='fixture', relocs=[(0, 7, 0, 0), (8, 7, 0, 0)],
                symbols=[dict(name=name, identity=('N', library, module), type=kind,
                              section=0, binding=1)])


class ManifestTests(unittest.TestCase):
    def test_local_aliases_merge_only_when_full_identity_matches(self):
        report = build_manifest({'main': fixture(), 'module': fixture(name='N#X#Y')}, {})
        self.assertEqual(len(report['imports']), 1)
        self.assertEqual(len(report['imports'][0]['references']), 2)

    def test_equal_local_names_cannot_hide_scope_version_or_kind(self):
        modules = {'main': fixture(), 'library': fixture(library=('other', 1)),
                   'version': fixture(library=('libc', 2)), 'module': fixture(module=('other', 257)),
                   'module-version': fixture(module=('libc', 258)), 'data': fixture(kind=1)}
        records = build_manifest(modules, {})['imports']
        self.assertEqual(len(records), 6)
        self.assertEqual(len({r['canonical_id'] for r in records}), 6)

    def test_required_unscoped_import_fails_closed(self):
        module = fixture()
        module['symbols'][0]['identity'] = None
        with self.assertRaises(ValueError): build_manifest({'main': module}, {})
        module['symbols'][0]['binding'] = 2
        self.assertEqual(build_manifest({'main': module}, {})['imports'], [])

    def test_unknown_symbol_kind_is_not_assumed_to_be_function(self):
        with self.assertRaises(ValueError): build_manifest({'main': fixture(kind=6)}, {})
