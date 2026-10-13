#!/usr/bin/env python3
"""Package an explicit allowlist; game files and generated boot images are excluded."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def package():
    dest = ROOT / 'out/entry-package'
    if dest.exists() and any(dest.iterdir()):
        raise ValueError('Package destination must be empty; refusing to include stale files')
    dest.mkdir(parents=True, exist_ok=True)
    files = ['out/tlg-entry-probe.exe', 'LICENSE', 'docs/WINDOWS_ENTRY_PROBE.md',
             'docs/UPSTREAM_README.md',
             'src/windows_entry_probe.c', 'src/import_names.inc',
             'src/windows_equeue.c', 'src/windows_equeue.h',
             'src/windows_mutex.c', 'src/windows_mutex.h',
             'src/windows_tls.c', 'src/windows_tls.h',
             'tools/inspect_target.py', 'tools/package_entry_probe.py',
             'tests/paths.py', 'tests/test_windows_entry_probe.py']
    files.extend(p.relative_to(ROOT).as_posix() for p in sorted((ROOT / 'scripts').glob('*.py')))
    for name in files:
        relative = 'tlg-entry-probe.exe' if name == 'out/tlg-entry-probe.exe' else name
        target = dest / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)
    print(f'Diagnostic package: {dest}')


if __name__ == '__main__':
    package()
