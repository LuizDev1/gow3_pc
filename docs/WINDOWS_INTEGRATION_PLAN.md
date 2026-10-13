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
