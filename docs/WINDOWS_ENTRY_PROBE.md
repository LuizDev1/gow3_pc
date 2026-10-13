# Windows entry diagnostic

This artifact is a CPU entry diagnostic, **not a playable port**. It maps an
unlinked image, applies its relocations, and attempts the original x86-64 entry.
Every system function and data import is deliberately unresolved. There is no
PS4 runtime, native library initialization, graphics, audio or TLS adaptation.
A failure or an early unsupported import is an expected diagnostic result.

## Build on GitHub

The `Windows entry diagnostic` workflow builds with MSYS2 CLANG64 on Windows,
runs synthetic tests without any game files, and uploads the
`tlg-windows-entry-diagnostic` artifact. Pushes to `the-last-guardian` trigger it.
It can also run manually when the workflow is available on the default branch.
No game files, credentials, boot images, or local inspection outputs are packaged.

## Run locally

Extract the artifact and use Windows Python 3.10 or newer. From its folder:

```powershell
python scripts/prepare_tlg.py 'D:\path\to\CUSA03627'
.\tlg-entry-probe.exe .\out\tlg\boot.bin
```

Use **boot.bin**, never `boot-linked.bin`: the latter contains Linux TLS rewrites.
Preparation accepts only the observed USA 1.03 image and bundled module hashes.
The original game files are read only. The generated files remain local.
Do not run the inherited Bloodborne launcher, patch scripts or `run.sh` on TLG.

The probe prints its result to the terminal. Exit 20 means the first unsupported
function import was reached; 23 means a Windows exception (including a data import
access or unimplemented guest TLS); 24 means the guest exit callback was reached;
124 is the ten-second watchdog; 1 is an input/loader error or unexpected return.
None of these outcomes proves the game boots or plays.

The corresponding diagnostic source and preparation scripts are included with
the GNU GPL license. Full project attribution remains in the repository.
