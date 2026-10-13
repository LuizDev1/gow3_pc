"""Validate pinned Windows renderer dependencies before CMake configuration."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BASE = Path('platform/windows/gow3-base')


def git(root, *args):
    return subprocess.check_output(
        ['git', *args], cwd=root, text=True, encoding='utf-8').strip()


def check(root, index_only=False):
    manifest = json.loads((root / BASE / 'SOURCE_MANIFEST.json').read_text())
    errors = []
    for name, dependency in manifest['submodules'].items():
        relative = (BASE / 'gpu/third_party' / name).as_posix()
        entry = git(root, 'ls-files', '--stage', '--', relative)
        expected = f"160000 {dependency['commit']} 0\t{relative}"
        if entry != expected:
            errors.append(f'{name}: missing or incorrect Git dependency pin')
        url = git(root, 'config', '-f', '.gitmodules', '--get',
                  f'submodule.{relative}.url')
        if url != dependency['url']:
            errors.append(f'{name}: dependency URL differs from provenance')
        if index_only:
            continue
        directory = root / relative
        if not (directory / '.git').exists():
            errors.append(f'{name}: dependency checkout is not initialized')
            continue
        if git(directory, 'rev-parse', 'HEAD') != dependency['commit']:
            errors.append(f'{name}: checkout is at a different revision')
        files = subprocess.check_output(
            ['git', 'ls-files', '-z'], cwd=directory).decode('utf-8').split('\0')
        missing = [path for path in files if path and not (directory / path).exists()]
        if missing:
            errors.append(f'{name}: {len(missing)} tracked files missing; first: {missing[0]}')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--index-only', action='store_true',
                        help='Check pins and URLs without requiring downloaded dependencies')
    options = parser.parse_args()
    try:
        errors = check(ROOT, options.index_only)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Dependency check failed: {error}', file=sys.stderr)
        return 1
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Windows renderer dependency pins and files verified.'
          if not options.index_only else 'Windows renderer dependency pins and URLs verified.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
