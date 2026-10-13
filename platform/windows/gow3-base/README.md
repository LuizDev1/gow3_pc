# Windows renderer integration base

Snapshot of the user-provided GoW3 Windows renderer, based on commit
97f1f8d4410ac8ad4fde29d29041f06e3af51b66, including its local Windows CMake
adaptations. SOURCE_MANIFEST.json records hashes before integration edits.
Original GPL/BSD/MIT licenses and gpu/VENDOR.txt remain intact.

This is the selected Windows renderer build base for TLG. It builds the static
`gow3gpu` library and a TLG runtime link diagnostic; internal upstream names are preserved. No GoW3 game
executable, launcher, runtime loader, executable patch or hook installer is imported.
The two src headers contain only inert overlay ABI declarations, not hook code.
Actor-hook status is disabled. The root TLG loader/preparation/tests are preserved.

The renderer still contains GoW3-specific graphics assumptions, shader heuristics,
settings and overlay text. They require audit before this library is executed with
TLG. Compilation is not runtime compatibility. The `tlg-renderer-probe` target
validates the complete archive against TLG loader callbacks; GPU startup and
guest graphics dispatch are not enabled. No game frame, menu or gameplay is claimed.

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

A static archive does not resolve its loader callbacks. The TLG runtime now
provides mapping queries, backing writes, page protection, map/unmap notifications,
clocks, errno, host-thread guest TLS, file translation and Windows speculative-read
recovery. Flexible mappings use pagefile sections so backing writes preserve GPU
page protection. External image segments are registered but have no backing alias.
The GoW3 restart action is disabled in this integration.

CI links the entire renderer archive with the TLG diagnostic and runs memory,
recovery and link checks without initializing a GPU. This catches dormant paths
with missing callbacks. Remaining work includes auditing game-specific behavior,
registering stack memory, enabling Vulkan startup, routing canonical GPU imports
and verifying the target game. The original GoW3 loader is not imported.
