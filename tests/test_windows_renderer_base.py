from pathlib import Path
import json
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'platform/windows/gow3-base'

class WindowsRendererBaseTests(unittest.TestCase):
    def test_submodule_pins_are_registered_in_git(self):
        provenance=json.loads((BASE/'SOURCE_MANIFEST.json').read_text())
        for name, dependency in provenance['submodules'].items():
            path=f'platform/windows/gow3-base/gpu/third_party/{name}'
            with self.subTest(dependency=name):
                entry=subprocess.check_output(
                    ['git','ls-files','--stage','--',path],cwd=ROOT,text=True).strip()
                self.assertEqual(entry,f"160000 {dependency['commit']} 0\t{path}")

    def test_ci_selects_windows_base_and_no_gow_loader_targets(self):
        workflow=(ROOT/'.github/workflows/windows-gpu-build.yml').read_text()
        self.assertIn('-S platform/windows/gow3-base/gpu',workflow)
        self.assertIn('--target gow3gpu',workflow)
        cmake=(BASE/'gpu/CMakeLists.txt').read_text()
        self.assertNotIn('add_executable(gow3-probe',cmake)
        self.assertNotIn('../src/probe.c',cmake)
        self.assertTrue((ROOT/'src/windows_entry_probe.c').is_file())

    def test_patch_interfaces_are_inert_and_provenance_is_present(self):
        for name in ('actor_hook.h','cheat_hook.h'):
            text=(BASE/'src'/name).read_text()
            self.assertNotIn('0x504',text)
            self.assertNotIn('build_stub',text)
            self.assertNotIn('original[',text)
        orbs=(BASE/'gpu/shim/gow3_orbs.cpp').read_text()
        self.assertNotIn('player) + 0x',orbs)
        self.assertIn('cheat_flag',orbs)
        provenance=json.loads((BASE/'SOURCE_MANIFEST.json').read_text())
        self.assertEqual(len(provenance['submodules']),5)
        self.assertTrue((BASE/'LICENSE').is_file())
        self.assertTrue((BASE/'gpu/VENDOR.txt').is_file())
