import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('renderer_package',Path(__file__).resolve().parents[1]/'tools/package_renderer_probe.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RendererPackageTests(unittest.TestCase):
    def test_transitive_dlls_case_cycles_and_system_exclusions(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);dlls=root/'dlls';system=root/'system'
            dlls.mkdir();system.mkdir()
            for name in ('A.dll','b.dll'): (dlls/name).touch()
            (system/'KERNEL32.dll').touch()
            imports={'probe.exe':['a.DLL','kernel32.dll','api-ms-win-core-test.dll'],
                     'A.dll':['b.dll'],'b.dll':['A.dll']}
            result=module.dependencies(root/'probe.exe',dlls,system,lambda p:imports[p.name])
            self.assertEqual({p.name for p in result},{'A.dll','b.dll'})

    def test_missing_import_is_an_error(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            with self.assertRaisesRegex(ValueError,'missing.dll'):
                module.dependencies(root/'probe.exe',root,root,lambda p:['missing.dll'])
