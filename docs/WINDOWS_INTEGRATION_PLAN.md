# Windows runtime audit and integration plan

This is LuizDev1's Windows project, based on bbport. The local GoW Windows
`compatibility-source` tree is a platform reference, not a compatible TLG runtime.
The last diagnostic run reaches `strlen`; this audit does not advance execution.

## Evidence and limits

Target: CUSA03627, APP_VER 01.03, loaded image SHA-256 recorded in
[target inspection](TARGET_INSPECTION.md). The offline linker reports **209 native
module bindings and 391 remaining host import entries**. Those are Linux-prepared
link results. The Windows entry probe does not load or initialize those native
modules; its actual runtime obligations are therefore larger than the 391-entry
residual list. This is why libc functions such as `strlen` still stop the probe.

`tools/audit_runtime.py --reference PATH_TO_GOW_SOURCE` audits the residual list
from `out/tlg/tlg-link.json`. Generated details and source fingerprints stay under
ignored `out/`. It reads source and import metadata, not assets or executable bytes.

| Static candidate category | Import entries | Meaning |
|---|---:|---|
| GoW runtime, scoped literal or its explicit POSIX alias | 57 | Candidate implementation, still needs identity/ABI validation |
| GoW runtime, function name hashed to NID | 125 | Weaker hint; library/module scope is not proven |
| Vendored GPU registration with matching NID/library/module names | 125 | Candidate export; registration/build availability is unproven |
| No match by these methods | 84 | Investigation required; not proof that no implementation exists |

These categories are exclusive by precedence, not percentages of compatibility.
Versions, data/function kinds, calling conventions and actual code paths require
separate validation. Diagnostic label tables are excluded as implementation evidence.

The graphics candidates include all 110 residual GnmDriver imports, 10 VideoOut
imports and 5 kernel event registrations. This identifies a substantial reusable
renderer interface, but proves neither Windows buildability nor TLG rendering.
Audio3d (12) and Ngs2 (10) have no matches in this audit. Other unmatched entries
include 42 kernel, 7 POSIX and 4 save-data imports. Network/PSN and audio results
must be investigated rather than replaced with unconditional success responses.

## Reusable Windows platform work

The GoW source already contains:

- `src/compat_win.c` / `.h`: POSIX adaptations and exception recovery.
- `src/runtime_thread.c`: a Windows TLS slot, guest TCB handling and rewritten
  `gs:[0x1480 + 8*slot]` accesses. Copying Linux `gs:[0]` rewriting is invalid.
- `src/runtime_memory.c`: section-backed direct memory, placeholder reservations,
  `VirtualAlloc2` and `MapViewOfFile3` for aliasing guest mappings.
- `src/probe.c`: Windows exception handling and linked-module initialization.
- `gpu/CMakeLists.txt`: a Windows static GPU target and loader linkage with
  winpthreads, onecore, bcrypt and ws2_32; X11/Linux-only flags are conditional.

The inherited bbport CMake still requires X11 and Linux shared-library/TLS flags.
The GoW graph also adds magic_enum, miniz and xbyak submodules and brings game
profiles, overlays, hooks and graphics fixes that must not be enabled for TLG.
Keep attribution and licenses when porting source. Generic platform code must be
separated from assumptions about GoW addresses, shaders, paths and asset layout.

## Implementation sequence and acceptance gates

1. **Canonical imports.** Produce a validated mapping from dump-local suffixes to
   NID, library/module names, versions and symbol kinds. Keep ambiguous identities
   unresolved. Adapt runtime tables by identity, not NID alone. Keep named-export
   audit hints separate from this executable resolver.
2. **Windows native modules and TLS.** Adapt linking/preparation and the production
   loader to initialize the observed libc/Fios2 modules using Windows TCB/TLS
   handling. Validate per-thread isolation, relocations and startup exports with
   synthetic modules before the target. Never pass `boot-linked.bin` from the
   current Linux linker into the Windows entry probe.
3. **System runtime as a block.** Port the generic memory, threads, synchronization,
   file and clock contracts using the GoW platform implementation as a reference.
   Test mapping aliases, ownership, condition waits and file structures. Record
   unsupported calls honestly. Preserve the entry probe as a regression diagnostic.
4. **Minimal Vulkan integration.** Backport Windows CMake/platform changes to the
   inherited renderer, register GnmDriver/VideoOut/kernel events, and connect real
   GPU completion to event queues. Disable Bloodborne/GoW patches, special shader
   fixes, overlays and temporal upscalers until independently audited for TLG.
   Build and test the renderer without game files on GitHub; validate rendering
   locally with the target and graphics diagnostics.
5. **First frame, then menu.** Trace shader compilation, resource binding, memory
   visibility and presentation. A window or successful build is not a rendered
   frame, and one rendered frame is not a working menu. Check input and transition
   stability before claiming the menu milestone.

Priority now is stages 1 and 2, rather than adding another isolated libc wrapper.
No reliable calendar estimate follows from static import counts. Audio3d/Ngs2 and
TLG-specific GPU behavior remain material unknowns even after platform reuse.

## Canonical identity metadata implemented

`prepare_tlg.py` now writes ignored `out/tlg/imports-canonical.json` alongside
its existing outputs. It contains referenced imports from the executable and
both bundled libraries, grouped by NID, library name/version, module name/version
and function/data kind. Local spellings retain their source module context.
Equal local names cannot silently merge unrelated imports; different local
aliases merge only when every canonical identity field matches.

The observed files produce 600 canonical identities (592 functions, 8 data
symbols). This includes imports eligible for native bindings, not just the
391 residual host entries. Required unscoped imports and unsupported symbol
kinds fail rather than being guessed. Four synthetic tests cover these rules.

The manifest is now consumed via a separate unlinked Windows image, described
below. Native module initialization and Windows TLS still require adaptation.
Linux linked output remains unsuitable for the Windows probe.

## Windows identity-aware diagnostic image

`scripts/windows_boot.py` converts the validated unlinked image and manifest to
`boot-windows.bin` (`TLGWIN01`). Fixed-width import records preserve NID, library
and module names/versions and function/data kind. Original import names remain
diagnostic labels; their dump-local suffixes no longer select contracts.

The Windows loader validates record bounds, NID correspondence and relocation
kinds before execution. Contract dispatch requires an explicitly supported full
identity. Different versions/scopes remain unresolved; inconsistent data/function
types fail before entry. Legacy `BBPROBE2` is retained for regression diagnostics.
Neither format links native modules or rewrites TLS.

Thirteen native Windows tests pass, including remapped local aliases, rejected
scope/version mismatches and a data/function mismatch. The observed USA 1.03
image reaches `strlen` with identity `libc@1 / libc@257 / function`, matching the
previous startup boundary. This is resolver integration, not additional gameplay
progress. Next: Windows linked modules and their TCB/TLS initialization.

## Windows module preparation and static TLS foundation

The module linker now has a separate `windows-prepare` mode. It keeps original
FS-load bytes and records candidate pattern offsets in executable segments,
alongside full import identities and kinds. It never applies Linux GS:[0]
rewrites. Real target preparation still finds 209 native bindings and 391 host
entries; the output is under ignored `out/tlg/windows-native/`.

Its binary magic is deliberately `TLGPRP01`, rejected by both current loaders:
this intermediate artifact must not be mistaken for an executable Windows
linked image. Pattern offsets still need instruction-boundary and expected-byte
validation before a future loader rewrites them with its allocated TLS slot.

`src/windows_tls.c` provides a Windows slot below 64, TEB slot displacement,
per-host-thread static TLS template/zero-fill allocation and TCB self pointer.
A synthetic machine-code test reads the real GS slot, checks template data,
zero-fill, thread isolation and detach. It preserves the Windows GS base.
This foundation is not attached to the target's main thread yet. Guest thread
records, dynamic-module TLS/DTV contracts and module initialization remain pending.

Fourteen native probe tests and two module-preparation tests cover this work.
Next: complete dynamic TLS and the linked Windows image reader, then run module
initializers with controlled diagnostics. The game still stops at `strlen` in
the existing entry diagnostic; no additional execution milestone is claimed.

## Native-module Windows loading connected

`TLGWIN02` now carries canonical identities, module metadata, native bindings,
main/module TLS metadata and pattern patch offsets. The loader validates bounds,
executable initialization/export addresses, function/data kinds and expected TLS
bytes. It allocates a Windows TLS slot, configures static/dynamic module templates,
prefers explicit host contracts over native exports, and calls native initializers.
The intermediate `TLGPRP01` and Linux formats remain rejected.

Synthetic integration tests initialize a module and call its native export, and
reject a mismatched binding kind before execution. Dynamic TLS tests verify
isolated module blocks on separate Windows threads. Fourteen probe tests and
four module-preparation/integration tests pass locally.

On the actual USA 1.03 files, native libc initialization starts at `0x5b10000`,
passes `sceKernelGetProcParam`, and registers the application heap API. It then
stops at `NWtTN10cJzE`, identity `libSceLibcInternalExt@1 / libSceLibcInternal@257`,
caller offset `0x5b6b8e1`, exit 20. This realizes the internal-libc gap previously
identified by the audit; no success stub is installed for that unknown contract.
The initializer has not completed, and Fios2/game entry have not run in this mode.

Next: identify that internal libc contract, then complete library initialization
with the generic Windows runtime. Guest thread records, direct DTV access,
instruction-boundary validation of TLS rewrite candidates and GPU integration
remain unfinished. The unlinked regression path still reaches `strlen`.

## Heap trace contract and base-console profile

`NWtTN10cJzE` was identified as `sceLibcHeapGetTraceInfo`; its NID also matches
the locally computed name hash. The ABI and shared structures follow the
[shadPS4 implementation](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/libc_internal/libc_internal_memory.cpp):
a 32-byte info record exposes a writable mask and 64-entry state table, disables
segment-info queries, and preserves input size/flag fields. This is shared state,
not an unconditional success stub. Attribution remains in the adapted source.

`WslcK1FQcGI` was identified as `sceKernelIsNeoMode` in shadPS4's
[symbol table](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/aerolib/aerolib.inl).
The diagnostic explicitly models a base PS4, so this query reports false; it
does not inspect the host GPU or claim PS4 Pro support. A guest-code test checks
the returned value. Heap tests check ABI preservation and shared writable storage.

The local USA 1.03 libc initializer passes these calls and next stops at
`pO96TwzOm5E#U#H` (`sceKernelGetDirectMemorySize`), caller offset `0x207d00`, exit 20.
Sixteen probe tests pass locally. The initializer has not completed; Fios2 and
game entry have not run in linked mode. Next is generic Windows memory backend
integration (reservations, mappings and aliasing), not a guessed size-only answer.


### Renderer platform groundwork (October 13, 2026)

The conditional startup path now reaches `sceGnmMapComputeQueueWithPriority`.
The CPU diagnostic must not fabricate queue completion to advance past it.
Initial renderer portability changes remove the unconditional X11 package/link
requirement on Windows, select a static `bbgpu` target there, avoid ELF-only TLS
and undefined-shared-symbol flags, and link the Windows Vulkan loader. SDL window
selection now exposes the Win32 HWND to the existing Vulkan Win32 surface path;
monitor-name matching uses SDL's portable case-insensitive substring routine.
Internal bbport names and game profiles remain unchanged.

These are source-level platform preparations, not a validated Windows renderer
build. Local CMake, SDL3/Vulkan development packages and the renderer's full C++
dependency set are not available in the diagnostic toolchain. Existing CPU CI
still builds only the diagnostic. Remaining work includes Windows compatibility
for POSIX calls and runtime callbacks, an isolated full-renderer build, auditing
Bloodborne hooks, connecting the loader memory/event interfaces and dispatching
compute queues through the real backend. No window or GPU execution has been
observed from this branch. The diagnostic remains blocked at compute-queue mapping.


SDL API references: [portable substring matching](https://wiki.libsdl.org/SDL3/SDL_strcasestr)
(requires SDL 3.2.0, now declared by CMake) and
[Win32 window property](https://wiki.libsdl.org/SDL3/SDL_PROP_WINDOW_WIN32_HWND_POINTER).
Verification for this change was source review and `git diff --check`; neither a
full renderer compile nor a native SDL/Vulkan surface test was performed.


### Isolated renderer compile diagnostics (October 13, 2026)

The `Windows renderer build diagnostic` workflow configures the full GPU sources
with MSYS2 CLANG64 and builds only the static `bbgpu` target. It runs for renderer
changes on this branch or by manual dispatch, with a 45-minute limit and two
compile workers. No game files or GPU device are required and no runtime/window
test is performed. Package/compiler versions, configure output and build output
are preserved as `tlg-windows-renderer-build-logs`, including failed attempts.
This is separate from the working CPU entry diagnostic and publishes no release.

Windows dependency setup uses MSYS2 packages and pinned upstream source commits
for magic_enum v0.9.7 and miniz 3.0.2 when system packages are absent. VMA uses
MSYS2's header-only installation when its CMake package is absent. Original
upstream licensing is retained by FetchContent. Linux package requirements remain
unchanged. A successful static-library compile alone does not prove runtime
callbacks link correctly or compute queues execute. The first run must establish
actual configure/compile blockers before more integration changes are claimed.


### First full-renderer CI result and FSR header fix (October 13, 2026)

Run 38460942003 configured successfully, then failed compiling FidelityFX 1.1.4
`ffx_message.cpp`: `swprintf_s` was undeclared. The source includes Windows headers
but omits the CRT wide-character header declaring that function. On MinGW Windows,
CMake now force-includes `wchar.h` for the affected FSR host target only. Upstream
submodule bytes and formatting behavior are unchanged.

A local Windows compile/run of a small wide-formatting program confirmed that
including `wchar.h` declares and links `swprintf_s`. This checks the proposed header
fix, not the full renderer. The next GPU CI run must verify it with MSYS2 CLANG64
and reveal any subsequent portability failures. No GPU execution is claimed.
