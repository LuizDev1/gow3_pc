"""Bundle the renderer diagnostic and its imported MSYS2 DLLs, never game files."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import os
from configure_probe_stack import require_guest_stack

ROOT=Path(__file__).resolve().parents[1]


def dependencies(binary, dll_dir, system_dir, imports):
    available={p.name.lower():p for p in dll_dir.glob('*.dll')}
    system={p.name.lower() for p in system_dir.glob('*.dll')}
    pending=[binary]
    selected={}
    while pending:
        current=pending.pop()
        for name in imports(current):
            key=name.lower()
            if key in selected:
                continue
            if key in available:
                selected[key]=available[key]
                pending.append(available[key])
            elif key not in system and not key.startswith(('api-ms-win-','ext-ms-win-')):
                raise ValueError(f'Unresolved runtime DLL: {name} (imported by {current.name})')
    return list(selected.values())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--dll-dir',type=Path,required=True)
    parser.add_argument('--reader',default='llvm-readobj')
    args=parser.parse_args()
    require_guest_stack(args.binary.read_bytes())
    def imports(path):
        output=subprocess.check_output([args.reader,'--coff-imports',str(path)],text=True)
        return re.findall(r'^\s+Name:\s+(\S+\.dll)\s*$',output,re.M|re.I)
    files=dependencies(args.binary,args.dll_dir,Path(os.environ['SystemRoot'])/'System32',imports)
    dest=ROOT/'out/renderer-package'
    if dest.exists() and any(dest.iterdir()):
        raise ValueError('Renderer package destination must be empty')
    dest.mkdir(parents=True,exist_ok=True)
    shutil.copy2(args.binary,dest/args.binary.name)
    for path in files:
        shutil.copy2(path,dest/path.name)
    shutil.copy2(ROOT/'LICENSE',dest/'LICENSE')
    shutil.copy2(ROOT/'platform/windows/gow3-base/README.md',dest/'INTEGRATION_STATUS.md')
    licenses=args.dll_dir.parent/'share/licenses'
    if not licenses.is_dir():
        raise ValueError('MSYS2 dependency licenses are missing')
    shutil.copytree(licenses,dest/'dependency-licenses')
    base=ROOT/'platform/windows/gow3-base'
    for path in base.rglob('*'):
        if path.is_file() and (path.name.lower().startswith(('license','copying','notice','copyright')) or path.name=='VENDOR.txt'):
            target=dest/'renderer-licenses'/path.relative_to(base)
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(path,target)
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    (dest/'SOURCE_REVISION.txt').write_text(
        f'https://github.com/LuizDev1/gow3_pc/tree/{revision}\n'
        'Experimental diagnostic; no game files included.\n',encoding='utf-8')
    print(f'Renderer package: {len(files)} DLLs plus executable, licenses and provenance')


if __name__=='__main__':
    main()
