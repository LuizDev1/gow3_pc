# Windows renderer integration base

Snapshot of the user-provided GoW3 Windows renderer, based on commit
97f1f8d4410ac8ad4fde29d29041f06e3af51b66, including its local Windows CMake
adaptations. SOURCE_MANIFEST.json records hashes before integration edits.
Original GPL/BSD/MIT licenses and gpu/VENDOR.txt remain intact.

This is the selected Windows renderer build base for TLG. It builds the static
`gow3gpu` library only; internal upstream names are preserved. No GoW3 game
executable, launcher, runtime loader, executable patch or hook installer is imported.
The two src headers contain only inert overlay ABI declarations, not hook code.
Actor-hook status is disabled. The root TLG loader/preparation/tests are preserved.

The renderer still contains GoW3-specific graphics assumptions, shader heuristics,
settings and overlay text. They require audit before this library is executed with
TLG. Compilation is not runtime compatibility. This library is not yet linked to
the TLG diagnostic; no game frame, menu or gameplay is claimed.

Imported text whitespace is normalized; SOURCE_MANIFEST.json records the original bytes.

## Source dependency setup

From the repository root in PowerShell, fetch the five pinned dependencies with
long-path support enabled for this command only:

```powershell
git -c core.longpaths=true submodule update --init --depth 1 -- platform/windows/gow3-base/gpu/third_party/fsr-vulkan platform/windows/gow3-base/gpu/third_party/imgui platform/windows/gow3-base/gpu/third_party/magic_enum platform/windows/gow3-base/gpu/third_party/miniz platform/windows/gow3-base/gpu/third_party/xbyak
python tools/check_windows_renderer_dependencies.py
```

The check verifies Git pins, source URLs, checked-out revisions and tracked-file
presence. `--index-only` checks metadata without downloading dependencies.
Sirit, SPIR-V headers, GCN, half and the overlay font are already tracked in this
snapshot. No game files are needed to compile the renderer library.

The [Windows renderer workflow](../../../.github/workflows/windows-gpu-build.yml)
records the MSYS2 CLANG64 compiler and required system packages: Vulkan headers,
loader and allocator, SDL3, fmt, Boost, robin-map, xxhash, Zydis, glslang and FFmpeg,
with CMake, Ninja and pkgconf. These packages are separate from Git submodules.
Run 38475141121 successfully built the static library with these dependencies.

## Remaining runtime dependencies

A static archive does not resolve its loader callbacks. The imported shim still
expects `runtime_memory_*` mapping/backing/protection services, clocks, errno,
host-thread attachment, file translation and restart support. The Windows
speculative-read path also expects `gow3_setjmp` and thread-local fault recovery.
These require a reviewed bridge to the TLG loader before executable linking and
execution. The original GoW3 loader is not a substitute for that bridge.
