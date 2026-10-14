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
validates the complete archive against TLG loader callbacks and offers an explicit
experimental `--gpu` mode. Local testing created a Vulkan device/swapchain on an
RTX 3060 Ti and two real guest compute queues. No game frame, menu or gameplay is claimed.

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
recovery, canonical identity and profile checks without initializing a GPU.
The experimental mode initializes Vulkan, registers image/stack memory and routes
GNM, VideoOut and related kernel imports by canonical identity. GoW3 camera,
upscaling, reduced scene targets and configuration/overlay controls are disabled.
Target initialization still has gaps; see the integration plan for observed stops.
The original GoW3 loader is not imported.

## Experimental local diagnostic

Download `tlg-windows-renderer-diagnostic` from a successful Windows renderer CI
run and extract the whole package, including its DLLs. From the repository root,
prepare metadata with `tools/prepare_content_parameters.py` using your game
directory, then run the extracted executable with `--gpu` and your prepared
`TLGWIN02` boot image. GPU parameters default to `out/tlg/gpu-parameters.bin`;
`TLG_GPU_PARAMETERS` overrides that path. Files remain under `TLG_GAME_ROOT`.
The GPU diagnostic has a 120-second watchdog. Omitting `--gpu` keeps CPU mode.
Game files and prepared boot images are never included in the CI package.
