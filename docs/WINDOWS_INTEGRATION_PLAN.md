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


### FidelityFX array-count declaration (October 13, 2026)

Run 38461930543 passed the earlier wide-formatting failure and stopped in FSR
upscaler/optical-flow sources because `_countof` was undeclared. The dependency
now receives a scoped compatibility header including both `stdlib.h` (array count)
and `wchar.h` (wide formatting). No substitute formatting or array-size macros are
introduced. The GPU workflow first compiles/runs a small C++ regression checking
array count and formatted output under the actual MSYS2 compiler.

Local Windows C++ object compilation confirmed both declarations. Full renderer
validation remains with the next GitHub run; this fixes a confirmed dependency
compile failure without claiming a renderer link or GPU execution.


### FidelityFX Vulkan backend headers (October 13, 2026)

Run 38462747056 passed the CRT regression and earlier FSR host compilation.
The Vulkan backend then failed on missing declarations for std::lock_guard,
placement new and log2. The scoped FSR header now includes mutex/new/math headers
and is applied to both the host and Vulkan-backend dependency targets. The
regression exercises lock_guard, placement construction/destruction and log2 in
addition to CRT formatting. The CPU diagnostic run for the same commit succeeded.
The next full-renderer CI run remains responsible for confirming this fix and
identifying further portability blockers; no renderer success is claimed yet.


### Sirit standard-library include (October 13, 2026)

Renderer run 38464931313 advanced past the previous FidelityFX failures and
stopped compiling Sirit: stream.h calls std::abort without directly including
cstdlib. The header now includes its declaring standard-library header, preserving
the vendored BSD license and behavior. This is a source dependency correction;
full validation awaits the next renderer build. The CPU diagnostic run for the
same commit succeeded. No rendering or runtime graphics integration is claimed.


### Renderer host portability (October 13, 2026)

Run 38465676807 advanced through FSR/Sirit and failed in the GPU sources on
Linux-only sys/resource.h and the Windows `near` macro collision. Unused resource
headers are removed. Command-buffer diagnostic reads now use ReadProcessMemory
on Windows and retain process_vm_readv on Linux. Helper thread availability uses
the Windows process affinity mask (current processor group) with hardware-count
fallback; background work requests THREAD_PRIORITY_LOWEST as a best-effort host
policy, not SCHED_IDLE or PS4 scheduling. Linux thread-CPU-clock instrumentation
is omitted on Windows rather than returning fabricated profiling values.
The PM4 self-test lambda was renamed to avoid the Windows macro.

A dedicated Windows helper check covers successful and invalid-address reads,
nonzero processor count and background-priority setup. Local Windows C++ object
compilation passed; the GPU CI runs the executable before attempting the complete
renderer. Additional platform failures are still possible. No successful renderer
link, compute execution, frame or menu is claimed.


### Signal recovery and diagnostics portability (October 13, 2026)

Run 38466519756 passed host-helper tests, then exposed Linux sigjmp_buf/gettid/
getrusage dependencies and libc++'s rejection of atomic_ref<const u32>. Windows
now uses the native host-thread ID helper; Linux-only per-thread resource counters
are omitted. Atomic flag reads retain acquire ordering through the compiler's
const-aware atomic-load builtin.

Windows has no implemented speculative fault-recovery contract. Speculative
shader specialization therefore reports worker failure and leaves work to the
existing regular path; optional stale resource-table probing is skipped. Linux
signal recovery remains unchanged. No fake setjmp aliases or unsafe replacement
memory probes are introduced. This may reduce optimization/diagnostic coverage;
it does not implement the loader's missing GPU memory fault tracking.

The updated host-helper test compiles locally for Windows and checks native thread
identity and const atomic reads in CI. The complete renderer still requires the
next build result; GPU execution, window presentation and menu remain unverified.


### Buffer-cache platform diagnostics (October 13, 2026)

Run 38467397706 advanced to buffer-cache compilation, then failed on dlfcn/mman
headers. Page-protection helpers now translate to VirtualProtect on Windows and
mprotect on Linux. Linux context inspection and dladdr symbolization are guarded;
Windows diagnostic addresses are reported without a fabricated module offset.
VRAM access traps and mirror write traps remain disabled on Windows until the
loader's exception-dispatch contract is integrated. This does not establish GPU
memory coherency or validate in-place VRAM/mirror modes for TLG.

The host-helper regression now protects a page, verifies that diagnostic reading
fails, restores access and verifies data. Local Windows object compilation passed;
CI executes the check and rebuilds the full renderer. Conditional source blocks
were checked for balance and git diff --check passed. No renderer success or
frame presentation is claimed.


### Cache dependency and page-fault context adaptation (October 13, 2026)

Run 38468449642 passed host regression checks and advanced to cache/page-manager
sources. The pinned miniz 3.0.2 lacked MZ_ZIP_FLAG_READ_ALLOW_WRITING; the Windows
fallback now pins miniz 3.1.2 at commit 77d0dce8627735138c51770d1799a1ef48f2117d,
whose upstream header declares that flag. No flag is aliased to a different mode.

Page-manager diagnostics now read Windows CONTEXT registers on Windows, preserve
Linux context access on Linux and use the safe host-memory reader for stack/frame
inspection. Host symbol names remain unavailable on Windows, reported as absolute
addresses. Diagnostic stack scanning now requires a completely readable sample;
partial readable samples are skipped. Fault handling uses the existing generic
instruction-pointer accessor for GPU memory callbacks. This prepares compilation;
the loader still does not dispatch guest faults into a linked renderer.

Source conditionals and git diff --check were verified. The next CI run must
validate the dependency update and page-manager build; no full renderer success
or game graphics is claimed. CPU CI for the prior commit succeeded.


### Image-watcher host allocation (October 13, 2026)

Run 38469406423 reached the image-watcher table and failed on a remaining Linux
mmap call. Host-owned zero-initialized allocation now uses VirtualAlloc on Windows
and retains anonymous MAP_NORESERVE mappings on Linux. The image-watcher table
uses this helper without substituting guest-memory mappings. Windows commits the
allocation budget up front (resident pages remain demand-paged), so commitment
requirements differ from Linux MAP_NORESERVE. Allocation failure remains explicit.
The existing image-watcher lifetime is unchanged.

The host regression checks zero-size rejection, zero-fill across multiple pages,
writable endpoints and host release. Local Windows C++ object compilation passed;
CI runs the test before the full renderer. No renderer link or GPU execution is
claimed until subsequent validation succeeds.


### Shared DLSS path helpers (October 13, 2026)

Run 38470438867 reached vk_dlss.cpp and failed because BridgeDirectory,
NgxDirectory and UserDlssDirectory were defined only inside the Linux branch,
while both Windows and Linux call them. The common path-selection helpers now
sit after the platform branch; native library names, executable discovery,
loading and symbol lookup remain platform-specific. No DLSS library is bundled,
activated or claimed available by this correction.

Conditional source blocks and git diff --check pass. The complete compile is
validated by the next renderer CI run; the previous CPU diagnostic succeeded.
GPU commands, rendered frames and gameplay remain unavailable.


### Pipeline constructor completeness (October 13, 2026)

Run 38471717789 reached the pipeline-cache compilation stage and failed because
PipelineCache's constructor was defined before PipelineCompiler, while a member
unique_ptr requires the complete type for constructor exception cleanup. The
constructor definition now follows PipelineCompiler and PendingPipeline. Its
body and member initialization are unchanged; destruction order remains unchanged.

Source verification confirmed the constructor text is preserved exactly once and
follows both complete definitions. git diff --check passes. The next full renderer
CI run validates the compiler fix; no successful renderer link or graphics output
is claimed yet.


### Switch to the existing GoW3 Windows renderer base (October 13, 2026)

At the user's request, Windows renderer CI now builds the user-provided GoW3
Windows renderer under `platform/windows/gow3-base/gpu` rather than continuing to
port the root Linux `gpu` tree. The original trees and root TLG diagnostic are
preserved. The imported renderer is compiled as its existing static `gow3gpu`
target; internal upstream names are retained. Original licenses, VENDOR.txt,
pre-integration file hashes and submodule commit pins are recorded with it.
The source checkout had local Windows CMake changes; provenance says so rather
than claiming an exact clean upstream commit.

GoW3 executable loaders, launchers, patch installers and game binaries are not
imported. Two external headers required by the overlay contain only inert data
layouts/UI enum values. Actor/cheat/orb interfaces are disabled and no game-specific
hook signatures or patch bytes are carried in those headers. The FSR CRT header
fix from the TLG integration is retained for the selected Windows compiler.

This completes source selection/build migration, not runtime integration. The
renderer still contains GoW3 graphics heuristics and settings. Next: obtain a
successful build of this selected base, audit game-specific renderer decisions,
then implement the runtime callback/ABI bridge to the preserved TLG loader.
No TLG frame or menu is claimed by this migration.

### Register Windows renderer dependencies (October 13, 2026)

Renderer run 38473995010 failed during CMake configuration because the imported
submodule paths were declared in .gitmodules but their gitlinks were missing
from the commit. Register all five dependencies at the recorded source revisions.
A regression check verifies Git's indexed gitlink modes and commits against the
source manifest, so checkout can retrieve the required dependency trees.
CPU run 38473994772 passed. The corrected renderer configuration still needs CI
validation; this change does not establish renderer runtime integration.

### Windows renderer dependency audit (October 13, 2026)

Renderer run 38475141121 completed successfully; CPU run 38475141137 also passed.
Local initialization exposed another reproducibility issue: the nested FSR paths
exceed Git for Windows' default path limit. Dependency setup now enables long paths
for the checkout command, without changing global Git settings, and CI downloads
only the selected renderer dependencies. The local incomplete FSR checkout was
repaired and all five pinned dependency revisions and tracked files were verified.

The new preflight checks manifest pins against the Git index, .gitmodules URLs,
checked-out revisions and missing tracked files. CMake also fails early for absent
submodule entry points, vendored SPIR-V headers, font or overlay ABI headers.
System library requirements and remaining loader callbacks are documented in the
Windows base README. No additional missing build dependency was observed in the
successful full build. Runtime memory services and Windows fault recovery remain
integration work; the static archive is not a linked TLG executable.

### First renderer memory query callbacks (October 13, 2026)

Renderer run 38477060026 and CPU run 38477060129 passed, including the dependency
preflight. The Windows memory backend now exposes host C ABI callbacks matching
the selected renderer's runtime_memory_region, runtime_memory_clamp and
runtime_memory_is_mapped interfaces. Queries use the existing locked direct and
flexible view table, report gaps up to the next mapping, clamp across adjacent
views, reject overflowing ranges and immediately reflect released views.

These are mapping queries, not readable-memory guarantees or lifetime pins.
Zero-length storage requests are rejected. Original image/stack registration,
backing writes, GPU hooks, page protection and Windows fault recovery are still
required before the selected library can be connected and run with TLG.
The local Windows diagnostic builds with warnings as errors; its memory self-test
covers mapped bounds, gaps, invalid pointers, overflow and release invalidation.

### Combined Windows renderer runtime bridge (October 13, 2026)

Implemented the remaining selected shim services as a coordinated change:
direct/flexible backing writes through temporary section aliases; CPU protection
restricted by guest permission metadata; serialized map/unmap hook installation
with existing-view replay and callbacks outside the view-table lock; external
image region registration; shared guest/renderer clocks and errno; validated file
translation; and idempotent host-worker guest TLS attachment.

Flexible mappings now own pagefile sections rather than VirtualAlloc storage.
Backing writes do not relax protections on the guest view. External image ranges
return false for backing writes so the renderer uses its tracked access path.
Win64 speculative-read recovery preserves nonvolatile registers and resumes via
the exception context without unwinding through guest frames. Renderer access
violation dispatch is tried before speculative recovery in the linked diagnostic.
Unknown/unhandled exceptions retain the existing diagnostic behavior.

The selected CMake project now has a tlg-renderer-probe executable with the whole
renderer archive linked, rather than a static-library-only check. CI tests its
link, memory and protected-read recovery paths without starting a GPU. No GoW3
loader is imported; its overlay restart action is disabled. This stage still does
not enable guest GPU imports, Vulkan startup or claim a frame/menu. Stack region
registration and game-specific renderer assumptions remain to be addressed.

The first whole-archive run (38480358619) compiled the renderer and TLG sources
and reported a single unresolved Windows import, CreateFileMapping2 in IOFile.
That call did not use extended parameters; it now uses CreateFileMappingW with
the same read/write, whole-file mapping behavior. The CPU run also identified a
stored-but-unused invalidation callback; it is no longer stored. CPU write
invalidation is dispatched through the renderer's access-violation handlers.
Recovery tests additionally verify thread isolation and reject execute faults
and unrelated exception types. These fixes require the next CI validation.

Validation of commit 844a82a completed: CPU run 38481626988 and renderer run
38481627074 both passed. The latter linked the whole renderer archive with the
TLG Windows runtime and successfully executed the linked-symbol, memory and
Win64 fault-recovery tests. Local checks also passed all 32 entry tests and four
renderer integration tests. A local USA 1.03 run preserved the existing startup
milestone and stopped at sceGnmMapComputeQueueWithPriority, caller 0x1472d4d.
This proves the service/link checkpoint, not enabled GPU execution: the executable
still does not initialize Vulkan or route guest GPU imports. No frame/menu is claimed.

### Opt-in Vulkan startup and canonical GPU dispatch (October 13, 2026)

The linked diagnostic now has an explicit GPU mode for canonical linked Windows
images. It configures native guest TLS before renderer startup, uses the process
parameter's actual SDK version and validated SFO attributes, registers image and
growable thread-stack ranges, then rebinds imports before native initialization.
The renderer registry retains library version and symbol kind; dispatch checks
NID, library, module, version and function/object identity. GPU event queues use
the renderer's own provider, avoiding mixed handle types. Unprovided services
retain the existing diagnostic traps. The graphical watchdog allows 120 seconds.

The TLG build disables GoW3 camera tracking, temporal upscaling, reduced scene
targets, configuration loading/saving and settings overlay. Readbacks use the
precise mode; GoW3 option overrides cannot enable null GPU or those enhancements.
Generic renderer changes remain inherited and target compatibility is unproven.
Stacks expose committed nonguard regions; reserved pages and Windows growth guards
are not made accessible by GPU protection. Worker stack registrations are removed
before thread exit.

CI adds canonical resolver/profile tests and packages the diagnostic with its
transitive MSYS2 DLL dependencies, licenses and source revision. The package
contains no game files. Full build and a local GPU-enabled run are the next
validation steps; this source change alone does not establish a rendered frame.

Pre-startup review also corrected the renderer's process-counter units: its
frequency service advertises 1 GHz, so the bridge now returns nanoseconds from
the shared QPC origin. Raw RDTSC remains the separate hardware-counter service.
The clock test compares both counters with elapsed process microseconds.

Run 38485382027 passed, including canonical identity/profile tests and diagnostic
packaging. Its checksum-verified artifact was tested locally on the RTX 3060 Ti:
Vulkan/swapchain creation succeeded and 125 relocations connected to 327 symbols.
Initial startup exposed legacy Windows thread-name notifications (0x406d1388),
which the diagnostic had treated as fatal. A narrowly matched continuable event
is now allowed to return normally, with a real RaiseException regression test.

A local debug run filtering only those notifications initialized both native
modules and entered TLG. GNM created compute vqueues 1 and 2 (pipe 1/queue 4 and
pipe 0/queue 3). The next stop was the engine's tessellation allocation assertion
at 0x1483245. Instruction/relocation inspection confirmed a real 128 KiB direct
allocation followed by mapping at the GNM-provided system-reserved address; the
Windows backend incorrectly discarded that hint and scanned occupied user pages.
Direct mapping now accepts hints in the selected AddressSpace's reserved range,
using an actual shared section view. No guest code or driver success is patched.
The regression test verifies the requested system address and shared storage.
The corrected non-debugger run still needs validation; no game frame is claimed.

### Normal GPU run after the tessellation mapping correction

Renderer run 38488037263 passed at commit 4a9cf2f, including the executable
link, identity, profile, memory, recovery and packaging checks. CPU run
38487967508 passed for the same runtime sources (0524c2a); the later commit
only increased renderer build parallelism. The downloaded diagnostic archive's
SHA-256 matched the GitHub artifact digest. Its local link/profile checks passed.

A normal run, without the debug notification filter, initialized Vulkan on the
RTX 3060 Ti, returned from both native initializers, and entered TLG USA 1.03.
The actual 128 KiB tessellation allocation mapped successfully. GNM subsequently
created vqueues 3, 4 and 5 (pipe 2, queues 0 through 2), in addition to the two
earlier queues. The previous engine allocation assertion did not recur.

The next diagnostic stop is import F+yfmduIBB8#U#H, libkernel version 1,
module libkernel version 257, caller 0xc51aee, exit 20. The upstream shadPS4
[kernel registration](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/pthread_attr.cpp)
identifies it as the Orbis wrapper of pthread_attr_setstackaddr.
Local call-site inspection confirms the game supplies its own stack address
after requesting a 32 KiB stack. The current Windows thread provider uses
CreateThread-owned stacks and does not implement caller-owned stack execution.
That requires a real stack/lifecycle bridge, including Windows stack metadata,
TLS and renderer memory registration; silently discarding the address would not
fulfill this contract. No command submission, rendered game frame or menu is
established by this checkpoint. The legacy direct-memory log text saying
"GPU not connected" predates the new hooks and is not a reliable GPU-mode status;
the import connection and GNM logs provide the evidence above.

### Caller-owned thread stacks

The Windows thread provider now stores and resolves scePthreadAttrSetstackaddr
and scePthreadAttrGetstackaddr. A caller-owned stack must have a nonoverflowing
range, registered guest storage and committed writable nonguard pages. Creation
rejects overlapping stacks of live workers without changing the output handle.
The caller retains ownership and must keep storage mapped until join.

Workers initialize guest TLS and register their Windows host stack before a
Win64-to-SysV bridge enters the requested storage. The bridge preserves Win64
nonvolatile registers, floating-point controls and the original host stack; it
temporarily updates NT_TIB stack bounds for Windows exception handling. Normal
return restores the host context before TLS and host-stack cleanup. Existing
guest memory registrations cover the caller's storage without duplicate views
or ownership changes. Attribute snapshots report the active caller stack.
Caller-owned stacks are fixed-size; their guard attribute does not allocate a
Windows growth guard. Foreign unwinding through the assembly bridge and detached
threads remain unsupported.

The synthetic worker test executes on a real 32 KiB caller stack, checks its
address and Windows bounds, reads private guest TLS, recovers a real protected
read through the renderer landing point, and verifies join results. It also
rejects overflowing, unmapped and readonly storage and simultaneous reuse;
sequential reuse preserves the caller's mapping and surrounding sentinels.
Local validation passed all 33 entry tests and four renderer-base tests; the
entry suite also passed after the final diagnostic-label/log cleanup. GPU build
and target-game validation of this bridge are still pending.

An additional live GetCurrentThreadStackLimits check caught a missing Windows
metadata field: updating NT_TIB alone left the API's lower bound on the host
stack. The bridge now also saves, switches and restores the x64 TEB
DeallocationStack field. Its offset is checked against the Windows header's
TlsSlots layout and its value is checked against the live Windows API before
entering caller storage. This private ABI follows the field/query relationship
documented in Wine's [TEB layout](https://github.com/wine-mirror/wine/blob/master/include/winternl.h)
and [stack query implementation](https://github.com/wine-mirror/wine/blob/master/dlls/kernelbase/thread.c).
The committed worker test verifies the actual API's bounds on the caller stack;
worker cleanup verifies restoration of the original bounds. A separate local
API probe and all 33 entry tests passed with this correction.

### GPU run: main-stack commitment mismatch

CPU run 38490607416 and renderer run 38490607460 passed for 62c79d2. The
checksum-verified renderer package's caller-stack selftest passed locally. A
normal GPU run advanced beyond the previous stack-address import and reached
sceGnmInsertWaitFlipDone, then terminated with access violation 0xc0000005.

An external debugger captured the actual failure at guest offset 0xc872fb. The
faulting stack pointer was below the Windows committed stack limit; this was
not the caller-owned stack range. Inspection of the packaged PE header found
32 MiB reserved but only 4 KiB initially committed. Guest code can skip the
Windows growth guard because it has no Windows stack probes. The reference
GoW3 CMake project explicitly sets both reserve and commit, while the imported
TLG target had set only reserve. The selected target now requests both values
as 32 MiB. Packaging rejects partial-commit stacks or stacks below 16 MiB,
using a shared validated PE parser; regression tests reject the observed
32 MiB/4 KiB combination.

Before publishing, a separate copy of the generated diagnostic was configured
with the existing fully committed 16 MiB diagnostic-stack utility. Its normal
GPU run passed the failing instructions and stopped cleanly at
sceUserServiceInitialize (j3YMu1MVNNo#M#N), caller 0xdd3eea, exit 20. This is
evidence for the main-stack diagnosis, not a full validation of the next CI
package's 32 MiB linker settings. A second generated local copy with exactly
32 MiB reserved and committed reached the same clean UserService stop. The
package guard accepts this copy and rejects the original partial-commit image.
No game files or guest instructions changed.
The run also printed missing shader-cache file requests; their impact remains
unresolved. No rendered game frame or menu is established. Local stack-header,
package and renderer-base checks passed (3, 2 and 4 tests respectively).

### Offline local UserService

CPU run 38492327828 and renderer run 38492327723 passed for 0b59681. The
checksum-verified, unmodified CI package also passed the former stack failure
in a normal local GPU run and stopped at sceUserServiceInitialize, caller
0xdd3eea, exit 20. This confirms the actual linker/packaging correction rather
than only a manually configured generated copy.

The Windows provider now implements an explicitly offline, single-user profile
(local user 1, name Player), following the reference GoW3 service contract and
the bundled shadPS4 ABI. Initialization, termination, initial user, four-entry
login list, bounded user name and the initial login event share locked state.
Duplicate initialization and uninitialized queries return the service's errors;
termination clears pending events. The login event is consumed exactly once,
including concurrent queries, and each fresh initialization resets it. No PSN
or Windows account sign-in is performed. Initialization priority is validated
as an ABI parameter; it does not create a PS4 scheduler.

Resolution requires the canonical function identity libSceUserService@1 and
module libSceUserService@257, independent of dump-local suffixes. Synthetic
tests exercise dispatch and reject wrong libraries, modules and versions.
Lifecycle tests check output sentinels, short buffers, missing users, event
consumption and reset. All 35 local Windows entry tests passed. Renderer CI now
also runs the service selftest; target-game execution with this implementation
still requires the new renderer package. Shader-cache lookup messages and
rendered-frame/menu validation remain unresolved.

### UserService target-game validation

CPU run 38493640276 and renderer run 38493640471 passed for bcb027e. The
renderer run included the new service selftest. Its diagnostic archive matched
the published SHA-256 and was used unchanged for a normal local GPU run.
TLG USA 1.03 initialized the offline UserService profile successfully and
advanced to scePadInit (hv1luiJrqQM#F#G), caller 0xdd3ef7, exit 20. The observed
canonical identity is libScePad@1, module libScePad@257. There was no recurrence
of the previous main-stack access violation. No game frame/menu is confirmed.

The reference GoW3 runtime_pad.c is the relevant next implementation source:
it uses SDL3 gamepads and a keyboard fallback, with events pumped by the same
window layer already selected for TLG. Its ABI includes a 120-byte PadData and
28-byte ControllerInfo. Reuse still needs Windows synchronization/clock changes,
canonical identity dispatch, lifecycle and input tests, and an audit of GoW3
mapping, replay/recording and settings-overlay options. The reference is not
linked into the Windows diagnostic yet; controller connection or working input
is not claimed by this checkpoint.

### Windows Pad service and SDL input sampling

The Windows runtime now provides Pad initialization, standard local-user port
open/close, state and single-sample buffered reads, controller information and
vibration dispatch. Canonical resolution requires libScePad@1 and module
libScePad@257. Stale handles cannot acquire a reopened port. Invalid handles,
arguments and counts leave caller storage untouched. Reads and close share a
Windows SRW lock. CPU-only diagnostics have no input backend and return neutral,
disconnected samples; they do not pretend a hardware device exists.

The SDL backend adapts the reference GoW3 runtime_pad.c ABI and default button,
stick, trigger and touch-click sampling. Events remain owned by the selected
window loop. Keyboard buttons are sampled only with an SDL keyboard focus;
the renderer's keyboard fallback is a virtual controller, not a physical DS4
claim. Disconnection and reconnection update the logical connection generation.
Timestamps use the same process-microsecond clock as the rest of the runtime.
Physical gamepad rumble is dispatched to SDL and failures are returned; no
motion, light-bar or calibration success is simulated. Unsupported imports
still stop. GoW3 mapping overrides, injected input files, replay, recording and
settings-overlay behavior are not imported.

Local tests passed all 37 entry cases, four renderer-base tests and two package
tests. A separate SDL-enabled harness built with the official SDL 3.4.18 MinGW
SDK exercised the actual sampling code using an in-process SDL virtual
controller: button press/release, stick endpoints and triggers passed. The
same selftest runs in renderer CI without Vulkan. Mock-backed lifecycle tests
cover bounded 120-byte PadData and 28-byte ControllerInfo outputs, single-sample
buffer semantics, disconnect/reconnect, stale handles and vibration failures.
CPU and renderer build lists and CPU source packaging include the new module.
The next CI build and target-game run still need to validate this integration;
physical input, a rendered frame and menu navigation remain unconfirmed.

### Pad startup validated; SaveData initialization checkpoint

Renderer run 38496246555 passed for 2a818b1, including SDL virtual-controller
and Pad lifecycle tests. The downloaded archive matched the published SHA-256.
An unchanged-package, normal local GPU run initialized UserService and Pad and
then stopped at TywrFKCoLGY#L#M, libSceSaveData@1 / libSceSaveData@257, caller
0xdd3f09, exit 20. NID hashing and the public shadPS4 registration identify it
as sceSaveDataInitialize3. This validates Pad startup in TLG, not physical input
or menu navigation. No rendered frame is established.

The next change exposes only Initialize3(NULL) and termination, using locked
service state and the reference GoW3 runtime_savedata.c lifecycle/error contract.
Repeated initialization is idempotent; unsupported opaque non-NULL parameters
are rejected without changing state. Termination clears state and reports the
not-initialized error when appropriate. No save file, directory mount, read,
write or completed dialog is fabricated: those imports remain unsupported.
Canonical resolution requires the observed library/module/version and function
kind. Tests cover lifecycle and rejection of wrong identities; all 39 local
Windows entry tests passed. Renderer CI also checks this lifecycle without GPU
startup. A new CI package is required to validate the change against the game.

### SaveData startup validated; live thread affinity

Renderer run 38553038167 passed for 0085cf6. Its archive matched the published
SHA-256 and the unchanged package was tested in a normal local GPU run. TLG
passed UserService, Pad and SaveData initialization, then stopped at
scePthreadSetaffinity (bt3CTBKmGyI#U#H), caller 0xe482de, exit 20. The canonical
identity is libkernel@1 / libkernel@257. Local call-site inspection confirms
the game obtains the current thread and requests mask 0xf. No rendered frame
or menu is confirmed.

Renderer run 38593844899 and CPU run 38593844828 passed for 73ac324. With the
private content root, the GPU run passed positional shader reads and asset
loads, no longer reached the previous `## Exit engine` point, and stopped at
`sceSaveDataSetupSaveDataMemory2`, canonical libSceSaveData@1 /
libSceSaveData@257 (caller 0xcfb253, exit 20, 3759 callbacks). The original
instructions build a 64-byte setup struct: option SET_PARAM (1), offline user 1,
memorySize 1,048,576, iconMemorySize 116,736, NULL initial parameter/icon, slot
zero and reserved zeros. The result pointer is NULL. No rendered frame is
established by that progress.

The local memory-only SaveData profile now requires an explicit existing
`TLG_SAVE_ROOT` directory and uses a separate
`CUSA03627-user1-slot0.memory` file. It supports the observed setup, bounded raw
Get/SetMemory2 data for user 1 / slot 0, explicit zero-fill when extending,
preservation of existing file contents and flush-before-success on writes.
Non-NULL parameter/icon payloads, other users/slots, double buffering, mounted
save directories and sync/event APIs remain unsupported. This is a persistent
raw memory blob, not a PS4 save-container export or complete save-system support.
The setup/get/set ABI reference is
[shadPS4 savedata.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/save_data/savedata.cpp)
(GPL-2.0-or-later). A two-process synthetic test preserves seeded bytes, checks
new zero-filled space, performs a bounded write and verifies persisted bytes
after restart. All 44 local entry tests passed; the renderer workflow also
executes that persistence test against its linked binary. Target-game validation
with an isolated writable save directory remains pending.

### Offline NP title configuration boundary

Renderer run 38595399308 and CPU run 38595399209 passed for a905bed. The local
GPU execution created a new 1,048,576-byte memory file in the isolated diagnostic
save directory, passed SetupSaveDataMemory2 and reached `sceNpSetNpTitleId`
(libSceNpManager@1 / libSceNpManager@257, caller 0xcfb3b8, exit 20). It also
requested `game/savetitle/savetitle.dat`, which was not included in the earlier
359-file staged subset. The frame-capture trigger remained unconsumed: there
is no captured guest display frame yet.

The new NP provider only records local title configuration. It validates the
16-byte title-ID and 128-byte secret ABI, a bounded nonempty terminated title ID,
and NULL parameters; it protects replacement with a state lock and never logs
or persists title secrets. No PSN login, request, account binding, callback or
online identity is reported. Other NP functions remain unresolved. Canonical
resolution requires the observed library/module/version and function kind.
The layout/error references are
[shadPS4 np_types.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/np/np_types.h)
and [np_error.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/np/np_error.h),
GPL-2.0-or-later. Synthetic inputs test NULL and malformed parameters,
configuration/replacement, cleanup of synthetic secret state, and rejection of
wrong canonical identities. All 46 local entry tests passed. The renderer
workflow now runs the NP configuration selftest as well; target-game validation
of this provider is pending.

Renderer run 38597004497 passed for 942aa55, but the target run returned to
engine shutdown before logging successful NP configuration. The first provider
had required an unverified 12-character identifier pattern; the public ABI
only supplies a 13-byte string field. That guessed format restriction has been
removed. The provider checks a nonempty terminated string within the field,
and logs only parameter presence or rejected length, never contents, if it
fails again. A synthetic 9-character ID tests the bounded-string contract.
Target-game validation is still required before claiming NP startup passed.

### Read-to-EOF request and the next offline service boundary

Renderer run 38600341690 passed for a9adc72; run 38602139679 passed for c907b3e.
The paired content tests showed that with savetitle.dat staged, shutdown happens
before NP configuration; without that one file, local NP configuration passes
and shutdown happens later. This control is diagnostic only, not a validated
complete content package. All 347 traced positional reads returned their
requested bytes. The expanded sequential trace identified the actual early
failure: `sceKernelRead` requests UINT32_MAX bytes from the 821-byte save-title
file, and the direct Windows ReadFile call returns a guest I/O error.

Disk reads now bound the Windows request by the bytes remaining in the file,
under the descriptor lock. This preserves read-to-EOF and short-read behavior
without asking Windows to probe a nonexistent four-gigabyte destination range.
The same bound applies to positional reads after selecting the requested
offset; their original cursor is still restored. The native fixture test reads
a five-byte file into an eight-byte buffer with a UINT32_MAX request, verifies
EOF and positional partial reads, and checks cursor preservation.

The paired control also logged a rejected module load for 0xad. The public
[shadPS4 sysmodule table](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/sysmodule/sysmodule_table.h)
identifies this as libSceNpTrophy. That local service is not implemented yet;
the runtime does not report it as loaded. NGS2 (module 0xb) is also unsupported.
Save-memory single-descriptor writes now accept dataNum zero as the SDK 4.x
default for one descriptor; multi-descriptor requests remain unsupported.
No guest display frame has been captured.

Renderer run 38603293952 passed for dbd80d0. In the complete staged-content test,
the UINT32_MAX read now returns exactly 821 bytes. The original save-title
header is accepted and local NP configuration passes. The run then logs module
0xad as unsupported and enters engine shutdown. This confirms the read fix
without omitting savetitle.dat or modifying original instructions.

The next local trophy profile provides module reference tracking and four
observed creation/lifetime APIs: CreateContext, CreateHandle, DestroyContext,
DestroyHandle. Context creation binds the existing read-only trophy00.trp for
offline user 1 / service label 0, checks its v3 header, declared size and table
bounds, and retains the package handle until context destruction/unload.
This is header binding, not complete catalog validation or registration.
Opaque IDs are monotonic and stale IDs are rejected; unloading releases local
objects. Registration, catalog queries, achievement unlocks, callbacks and PSN
synchronization remain unresolved. The API/error reference is
[shadPS4 np_trophy.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/np/np_trophy.cpp)
and np_error.h, GPL-2.0-or-later. All 48 local entry tests passed, including
synthetic TRP header binding, malformed headers, stale IDs, cleanup and
canonical library/module/version rejection. The next GPU package must validate
the context/handle boundary against the real game.

Renderer run 38605641614 passed for aeb855e. The full staged-content execution
passed module loading and created a header-bound trophy context and handle,
then requested `game/trophy/trophy00.dat`. It subsequently rejected a
GetSaveDataMemory2 request with a non-NULL parameter record (icons NULL) and
stopped in linked libc strlcpy, guest offset 0x5b7c6b5, with a null/low source
pointer. A private exception capture identifies the game caller at 0xe20424.
The requested trophy data (987 bytes) was staged privately; a repeat still
reproduced the text-copy fault. No guest instruction or exception is skipped.

The memory-only backend now supports the observed 1328-byte parameter record
(title/subtitle/detail, userParam, timestamp and reserved storage). It stores
that record separately in `CUSA03627-user1-slot0.params`, initializes fresh local
storage with empty strings, parameter zero and a real UTC creation timestamp,
validates stored sizes/terminated strings, and flushes updates before success.
Metadata-only requests are supported and user/slot/lifecycle checks also cover
empty requests. This is a local raw record, not a PSF or complete PS4 save
container. Icons, mount operations, save events and multi-descriptor writes
remain unsupported. The two-process test now checks metadata defaults,
write/reload persistence and wrong-user rejection as well as memory bytes.
All 48 local entry tests passed. The next GPU package must validate the real
metadata request and determine whether the text-copy fault is independent.

Renderer run 38609047743 and CPU run 38609047673 passed for ab05511. The local
target execution accepted the metadata request and created the 1328-byte local
parameter file; the previous unsupported-metadata diagnostic no longer appears.
It still stopped at the same libc strlcpy instruction with source address 0x22.
Thus metadata support is validated for the observed request but does not fix
this independent text-pointer fault. The caller identified at 0xe20424 reads a
string pointer from a language-indexed save-title table. The next investigation
must inspect that table's relocation/lifetime and the caller's registers;
do not suppress the fault or change guest instructions to make progress.
The display-frame capture trigger remains unconsumed. No first game image,
menu, trophy registration, playable behavior or complete save support is claimed.

The Windows thread provider now resolves that service and applies the mask with
SetThreadAffinityMask for the current thread or a live registered guest worker.
PS4 low CPU bits map directly to the low Windows logical CPU bits. Empty masks,
out-of-profile CPU bits and unknown/stale workers return errors; Windows rejects
unavailable CPU masks rather than the runtime reporting fake scheduler success.
For registered workers, the requested attribute is updated only after Windows
accepts it. Local tests verify the actual Windows group mask changes and restore
the original mask, plus invalid argument/handle rejection. All 39 Windows entry
tests passed. Renderer CI also runs this thread test. A new GPU package is still
needed to validate this service against the target game.

### Affinity startup validated; thread priority checkpoint

CPU run 38554914464 and renderer run 38554912067 passed for 194bad0. The
checksum-verified renderer package passed the affinity call in a normal local
GPU run, then stopped at scePthreadSetprio (W0Hpm2X0uPE#U#H), caller 0xe482f0,
exit 20, with canonical libkernel@1 / libkernel@257 identity. The observed
initialization call-site requests priority 550 after mask 0xf. No frame/menu
is confirmed by this checkpoint.

The provider now sets a Windows relative priority for the current thread or a
live guest worker, retaining the exact requested guest value for Getprio and
current-thread attribute snapshots. Guest priorities 256 through 767 map to
coarse Windows levels: below 512 is HIGHEST, 512 through 699 ABOVE_NORMAL, 700
NORMAL, and higher values BELOW_NORMAL. This preserves a coarse ordering around
the existing default 700; it does not emulate PS4 FIFO scheduling or its timing.
No realtime process class or TIME_CRITICAL priority is requested. Guest metadata
changes only if Windows accepts the host operation. Invalid arguments, unknown
handles and finished workers return errors without overwriting query outputs.

Tests verify the actual Windows priority for request 550, exact guest queries,
attribute snapshots, restoration of the main thread's original host priority,
live worker updates and stale worker rejection. All 39 local entry tests passed;
renderer CI also runs the scheduling test. The next GPU package still needs a
target-game run to validate this change.

### Continued startup: POSIX synchronization and compiler reuse

CPU run 38558151163 and renderer run 38558151313 passed for b6f92ec. The
checksum-verified package advanced beyond priority setup and requested font
and interface texture files. A normal run stopped with an access violation in
the host acquire_mutex registry traversal (host RVA 0xfea0, read address 0x22).
A separate debug run instead stopped at pthread_mutexattr_init, canonical
libScePosix@1 / libkernel@257. These timing-dependent results do not establish
a rendered frame or a fix for the registry corruption.

POSIX variants for the mutex and thread functions present in the validated
import metadata now reuse the existing kernel providers through correctly
shaped SysV wrappers. Their errors are positive FreeBSD errno values, following
the reference GoW3 runtime_mutex.c and runtime_thread.c. Resolution accepts only
the observed POSIX library/module/version identity. File operations outside this
subset and detached thread execution remain unsupported. Tests cover recursive
mutex ownership, lifecycle, positive errors, named worker TLS/join and rejection
of incorrect canonical identities. All 40 local entry tests and four renderer
integration tests passed. Target-game validation and the registry fault audit
still require the next package.

The renderer workflow also adds ccache and [GitHub compiler-result caching](https://github.com/actions/cache).
Cache compatibility is tied to the complete MSYS2 package-version fingerprint;
ccache additionally checks compiler content and source/header inputs. Storage is
limited to 512 MiB. Every run still links, executes its runtime tests and packages
the result. The first cache must be populated before reuse can be measured;
no speedup is claimed yet.

### Event-flag boundary after POSIX initialization

CPU run 38560992327 and renderer run 38560992273 passed for 6260d07. The
renderer package SHA256 is
`15f4923a8fb4d4d99b75ac0c580158d5cb1670362111eb1f65079a388fca4d45`.
A normal local GPU execution advanced through POSIX initialization and created
22 semaphores, then stopped at `sceKernelCreateEventFlag`, canonical
libkernel@1 / libkernel@257, caller offset `0xcfaa86`, exit 20. The original
call supplies a five-argument ABI with attributes `0x21` (FIFO, multiple waiters),
initial bits zero and NULL options. This run did not reproduce the previous
mutex fault; one execution does not establish that the timing-dependent fault
is fixed. No rendered game image or menu is confirmed.

The Windows synchronization provider now implements numeric event-flag handles,
AND/OR waits, FIFO consumption among matching waiters, clear-all/clear-pattern
wait modes, the clear API's keep-mask semantics, cancellation, polling, timeout
updates and deletion wakeups. Deleted objects stay allocated until every
awakened waiter has finished using them. Priority-ordered flags, non-NULL options
and named open/close remain unsupported. ABI/mode/NID reference:
[shadPS4 event flags](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/event_flag.cpp)
(GPL-2.0-or-later, 2024-2026 contributors); the Windows implementation is
independent and retains attribution. Local tests exercise actual blocking
threads, FIFO bit consumption, cancellation/deletion lifetimes and canonical
library/module/version rejection. The next GPU package must validate this
implementation against the game.

Renderer run 38583442233 passed for 211583d (package SHA256
`52c8d5af705703db4b12d62a712a81da8ec6009d2eea1d8042d80099d64ac17a`).
The normal GPU execution again stopped in the host mutex traversal (host RVA
0x10460, invalid link 0x2/0x3). Other diagnostic executions reached `_sigprocmask`
in the linked libc, caller offset 0x5b7ca1b, exit 20. These results establish
that the fault remains intermittent, not that event-flag support fixes it.
Optional `TLG_TRACE_SYNC` logging now records mutex allocation/unlink operations
and main TLS storage bounds for a targeted lifetime audit.

A read-only inspection of the local boot and update PSARC manifests found
`ps4_shader_cache/clear_vs_clear_vs_5_0_00000000.sb` in both archives. Thus the
loose-file failure for that shader is not evidence of an absent game asset.
Archive routing/reading still requires runtime validation. No game assets were
extracted or added to Git. This renderer CI run restored compiler results and
its build/link step took 16 seconds; MSYS2 setup took 145 seconds. These are
observations for one run, not a fixed future build-time guarantee.

### Identified mutex ABI overlap

Renderer run 38585420085 passed for the tracing build 433f995. A private hardware
watch captured the original linked libc instruction at guest offset 0x5b6d81c:
`or dword ptr [rax + 0x20], 0x100`, immediately after successful mutex creation.
The guest-visible handle points to the Windows Mutex object. Its private `next`
pointer previously occupied offset 0x20, so this valid libc flag write changed
the linked-list pointer. This explains the corrupted traversal without assuming
premature destruction. No guest instruction is changed or suppressed.

The object now reserves a four-byte guest flag word at the observed offset;
the private registry link follows it at offset 0x28. Compile-time assertions
guard those offsets. The mutex selftest reproduces the guest's four-byte OR
on two initialized locks and verifies traversal, ownership and deletion. This
establishes only the observed flag field, not the complete internal pthread
layout or signal semantics. Target-game validation of the corrected package
is still required.

Renderer run 38586653451 passed for f3edd3b. Three consecutive normal local GPU
runs of its checksum-verified package passed the previously failing heap/mutex
initialization and stopped consistently at `_sigprocmask` (guest caller
0x5b7ca1b, exit 20), with 2358 registered exit callbacks. No host registry access
violation or libc `int 0x44` allocator assertion occurred in these runs.
This validates the observed ABI overlap correction across those executions;
it does not prove a full pthread layout or a rendered image.

The next implementation tracks the observed `_sigprocmask` contract using a
16-byte FreeBSD/Orbis signal set, per-thread masks, block/unblock/set/query,
input/output aliasing and positive EINVAL in guest errno with a -1 return.
New guest workers inherit the creator's mask and subsequently change their own
copy. SIGKILL/SIGSTOP bits remain unmaskable. Windows exception handlers and
host signal behavior are unaffected. Actual guest signal delivery remains
unsupported; this is mask bookkeeping, not a signal-handler implementation.
The real guest thread selftest checks inheritance/isolation, operations and
invalid requests; synthetic imports check the canonical identity gate.

### Content-reading diagnostic after signal-mask support

CPU run 38587607580 and renderer run 38587607624 passed for d5aa750. Normal
execution passed `_sigprocmask`, registered 3759 exit callbacks, and reached
`## Exit engine`; it then stopped at `scePthreadGetname`, caller 0xdd40aa.
This is engine shutdown, not successful boot. Numerous requested assets still
failed loose-file lookup, and NGS2 loading remained unsupported.

A private read-only PSARC diagnostic matched manifest names to TOC entries by
their filename MD5 (uppercase when the archive is case-insensitive). The update
archive's TOC order differs from its manifest order; indexing by manifest line
would select the wrong payload. Each selected filename hash, block range,
decompression size and a bounded aggregate output were checked. The diagnostic
staged 359 requested resources (2,213,370 bytes) under ignored `out/tlg/`, using
update entries where present and preserving original game-file content. No
game assets or generated binaries are added to Git. This is an isolated content
test, not an implemented native archive mount or a distributable game package.

Using that private content root, the same d5aa750 binary found the first shader
and stopped at `sceKernelPread` in the linked FIOS2 module (caller 0x5c1b422,
exit 20, 2349 callbacks). The Windows file provider now implements positional
reads with the descriptor lock held across seek/read/cursor restoration.
Sequential reads/seek/close cannot observe the temporary cursor. Tests cover
exact offset bytes, EOF/partial reads, preserved cursor and invalid parameters.
The next renderer package must validate the actual shader read; no game image
or menu is confirmed.

### Save-title size corruption and process-counter frequency (2026-10-14)

Private debugger captures of ab05511 established that the 821-byte save-title
file initially matches its staged source, including the 24-record TSDL header.
However, its file-manager size query returned -1 before allocation. The original
caller truncates that value to UINT32_MAX for allocation and reading. A hardware
write watch subsequently caught linked libc's allocator writing a chunk header
over the title table at guest offset 0x5b69682. The later strlcpy fault at
0x5b7c6b5 is therefore downstream of an invalid size/allocation path; accepting
save metadata does not repair it. No guest instructions were patched.

The size query binds to native sceFiosFHGetSize. A separate capture before file
close observed handle 0x7fff8000 and a null native handle-record lookup, leading
to the -1 result. The reason this handle cannot be looked up is still unresolved.
These captures and game data remain private in ignored out/; they are not part
of the repository or distribution.

Debugging also exposed a concurrent stop at sceKernelGetProcessTimeCounterFrequency.
Commit 39bcda4 supplies that export and the CPU process-counter export with an
explicit matched unit: nanosecond ticks and 1,000,000,000 ticks per second. This
matches runtime_process_time_counter already used by the selected renderer,
while hardware RDTSC retains its independently calibrated frequency. Native
elapsed-time tests and synthetic wrong-library/module/version tests pass.

All 49 local entry tests passed. CPU run 38611947973 and renderer run 38611948017
passed. The checksum-verified renderer package reached a different concurrent
stop at sceSystemServiceGetStatus, caller 0xdd4634, exit 20. The FIOS size defect
remained observable, so this run does not establish that memory corruption is
fixed. The frame-capture trigger remained unconsumed; no image or menu was seen.

### Native FIOS initialization dependency traced (2026-10-14)

A breakpoint after the original sceFiosInitialize call captured return code
0x805a1000. The native library's initialization path calls sceSysmoduleLoadModule
for DiscMap (0xd7) and forwards this loader error. Initialization therefore never
enables the full file-handle registry. Its fallback FHOpenSync encodes a direct
file slot with 0x7fff8000; FHGetSize searches the full registry and returns -1.
This explains why a successful file open/read could coexist with a failed size
query. It is not evidence that the game itself frees its title table incorrectly.

Commit 0b04b29 provides a reference-counted DiscMap module and the observed
sceDiscMapIsRequestOnHDD contract for absent disc bitmap information. It returns
0x81100004 and preserves outputs, following the existing GoW runtime and the
[shadPS4 implementation](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/disc_map/disc_map.cpp).
It does not advertise disc extents, complete installation, or unknown DiscMap
exports. Native module and canonical identity tests cover the provider.

CPU run 38626008716 and renderer run 38626008559 passed; 51 local entry tests
passed. That target run progressed through DiscMap loading and stopped at
sceKernelClockGettime, caller 0x5c488e5, before FIOS initialization completed.
Commit 1290b08 adds the kernel variant of the existing timespec clock, translating
POSIX errno to kernel errors while preserving output buffers on failure. All
52 local tests pass, including null/invalid clock errors and identity gating.
The full initialization result, a valid title-file size, and rendering still
require target validation of the new renderer package.

### Valid size and direct original-archive validation

CPU run 38626748081 and renderer run 38626748059 passed for 1290b08. FIOS then
opened the archive set and reached its six-argument DiscMap extent query,
fJgP+wqifno, caller 0x5c44510. The original caller checks its error before reading
output extents and has a fallback path. Commit 98518a0 supplies the same absent
bitmap error for this query, preserving every output rather than fabricating
physical disc layout. The selftest covers sentinel preservation and null outputs.

CPU run 38627485362 and renderer run 38627485341 passed. All 52 local entry tests
passed. Separate private breakpoint captures of the checksum-verified renderer
confirmed sceFiosInitialize returns zero and the save-title query now resolves
handle 0x11801f to a valid native record with size 821. These captures modify only
temporary debugger breakpoints; no persisted image patches were made. The
previous UINT32_MAX read and low-pointer strlcpy fault did not recur in these runs.

Normal execution was also repeated with TLG_GAME_ROOT pointing directly at the
original CUSA03627 game directory, without the diagnostic loose-file extraction
root. Native FIOS performed positional reads from the original PSARC archives,
reached local save/trophy setup and stopped at sceSystemServiceGetStatus, caller
0xdd4634, exit 20. Saves remained under the isolated diagnostic save root; game
files were opened read-only. This validates the archive path exercised by this
startup run, not every resource, gameplay or package operation. GetStatus is the
next observed blocker. No frame capture was produced and no menu was observed.

### SystemService status prefix (2026-10-14)

Commit 5cb817a supplies sceSystemServiceGetStatus for the existing
single-application foreground diagnostic profile. Its nine observed field bytes
report zero queued PS4 system events, no shell overlay, foreground execution,
normal CPU mode, no broadcast and no VR out-of-area condition. It does not infer
desktop keyboard focus or implement a shell event queue. The field layout and
null-buffer parameter error follow
[shadPS4 SystemService](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/system/systemservice.cpp).
Only these field bytes are written; adjacent and reserved storage is preserved.

All 54 local Windows entry tests pass, including sentinel bounds, null output,
field values and rejection of mismatched canonical library/module versions.
The original caller at 0xdd4634 reads the overlay/background bytes to control
its frame update. Target rendering remains unverified until the new package runs.

CPU run 38629774404 and renderer run 38629774463 passed. The checksum-verified
package, with the original game directory as its read-only content root, passed
GetStatus and stopped at scePadSetMotionSensorState, caller 0xd2e22f, exit 20.
The caller requests enable=1 and ignores the return before calling the existing
pad state reader. No frame was captured during this run.

Commit 0c5388f supplies this observed motion-state request with explicit limits:
the diagnostic has no implemented motion sampling, so enabling returns the Pad
NOT_PERMITTED error; disabling succeeds because sampling is already disabled.
Invalid/stale handles and invalid boolean values return Pad errors. Tests cover
these requests and resolver scope. No motion readings or enabled-device success
are fabricated. All 54 local entry tests continue to pass.

CPU run 38630559344 and renderer run 38630559580 passed for 0c5388f. The target
run passed the motion-state request and stopped at scePthreadYield, caller
0xe4a399, in a loop waiting for work. The original-content root remained in use;
no frame was captured. Commit 8472989 maps this request to Windows SwitchToThread
with a Sleep(0) fallback when no peer is eligible. It returns success after the
actual scheduler request, without promising PS4 FIFO ordering. Native thread
tests check that yielding preserves the guest thread identity and resolver scope;
all 54 local entry tests pass. A target run of this package is still required.

CPU run 38631281724 and renderer run 38631281666 passed for 8472989. Its target
run reached GNM compute/graphics command preparation: Vulkan compiled a compute
pipeline, and the game called DispatchDirect, SetPsShader350, SetEmbeddedVsShader
and DrawIndexAuto. The guest's own frame counter reached 2; that counter is not
evidence of two displayed frames. The capture trigger remained unconsumed.
Execution stopped at sceKernelGetPrtAperture, caller 0x14a770c, exit 20.

Commit c262dba exposes the three initially empty PRT aperture slots, matching
the initial state in
[shadPS4 kernel memory](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp).
The Windows provider has not configured any such sparse-texture range, so valid
queries return address/size zero. The setter and sparse mapping remain unsupported.
Tests cover all three IDs, output sentinels, invalid IDs/null outputs and resolver
scope; all 54 local entry tests pass. Rendering still requires target validation.

CPU run 38632032537 and renderer run 38632032587 passed for c262dba. The target
passed the PRT query and stopped at sceKernelQueryMemoryProtection, caller
0x14a7791, while validating a GPU buffer range. No display frame was captured.
Commit dc16c69 supplies this query from the actual mapping registry: containing
range with an exclusive end, guest CPU/GPU permission bits, optional outputs,
and EACCES with unchanged outputs for unmapped addresses. Renderer page guards
are temporary host protections and do not alter these guest permission bits.
Native tests check two real aliases with different guest flags, page boundaries,
sentinel preservation, optional/null outputs, and queries while a GPU guard has
made Windows storage read-only. All 54 local entry tests pass.

CPU run 38632975963 and renderer run 38632975675 passed for dc16c69. The
checksum-verified target run passed the memory query and reached
sceGnmSubmitAndFlipCommandBuffersForWorkload, buffer 1. GNM draw/dispatch calls
and Vulkan compute pipeline compilation were observed. No subsequent unsupported
import or guest access fault was reported during that run, but presentation did
not complete: the capture trigger remained unconsumed after a 120-second
diagnostic limit. The harness stopped its own process at that limit; this is not
a naturally returned game exit or a captured frame.

A repeat with reduced log verbosity captured two private thread snapshots at
20 and 40 seconds. The main guest thread waited in wait_count. The GPU command
processor stack included RunEventWriteEop / Rasterizer::DrainDrawPipe, while
another GPU worker stack included Runtime::AccessBuffer / Scheduler::WaitHostCopies
/ RunEventWriteEos. These locate waits, not their proven cause. Private stack
captures and addresses remain in ignored out/ and are not distributed.

A paired diagnostic with GOW3_DRAW_PIPE=0, the existing serial preparation mode,
also produced no display frame during 60 seconds and was stopped by the harness.
Therefore disabling parallel preparation alone did not resolve the observed
stall. The serial setting was not made the default or represented as a fix.
The next investigation is GPU/host-copy completion and the waiting submission;
no synchronization bypass, guest patch, fabricated GPU completion or menu claim
was introduced. Image/menu rendering remains unconfirmed.

### First completed presentation capture (2026-10-14)

Private paired runs of dc16c69 isolated the GoW concurrency defaults. Disabling
only draw preparation, only Vulkan recording, or only copy workers did not
produce a frame in the diagnostic windows. Neither draw+record nor draw+copy
serial mode produced a frame. With recording and copies synchronous while draw
preparation remained parallel, presentation completed and execution proceeded to
sceSystemServiceHideSplashScreen, caller 0x27c48a, exit 20. The all-serial control
also presented a frame. These comparisons identify a useful compatible baseline;
they do not prove the individual root causes of the inherited asynchronous stalls.

The capture trigger was consumed and GPU-completed readback wrote both a
1920x1080 display buffer (8,294,400 bytes) and a 1280x720 output (3,686,400 bytes).
Pixel inspection of both showed RGB zero and alpha 255 throughout, and the output
was visually checked: a fully black frame. This is the first completed capture,
not a game logo, scene or menu. Captures remain private under ignored out/.

Commit 7ec3a07 makes synchronous Vulkan recording and CPU copy execution the
defaults only for CUSA03627, before renderer initialization. Explicit nonempty
experimental overrides are preserved and unrelated title defaults are untouched.
Parallel draw preparation is retained. The standalone configuration test checks
title scope, absent/empty options, defaults and explicit overrides and passed
locally. A new CI step runs the same native test before building the renderer.

The observed HideSplashScreen call now idempotently hides the absent PS4 shell
overlay in this diagnostic; it does not report a rendered menu. Status tests
cover repeat calls and field preservation, and a canonical import test checks
the new export's identity gate. The CPU diagnostic compiled locally, but Windows
Application Control blocked its execution with WinError 4551, so its new native
tests require CI validation. No security policy was changed to bypass that block.

CPU run 38636078867 passed all 55 entry tests and renderer run 38636079056
passed the native submission-profile test and full build. The checksum-verified
7ec3a07 renderer reproduced a completed capture with its new defaults and
passed HideSplashScreen without an unsupported import or guest exception.
The diagnostic was stopped at 90 seconds; no game scene or menu was observed.

Further private snapshots located the main wait in semaphore 2, caller
0xe4a399, with task workers waiting on semaphore 3. The rendering producer
(entry 0xc53fe0) waits in a guest condition at caller 0xc4e27f. The submission
thread (entry 0x1494d40) remains alive and waits in the renderer's EOP QUEUE,
caller 0x1495102. Its reply counter remains zero. The command processor appeared
idle in the earlier snapshots. Trace logs include GfxEop interrupts, but do not
yet prove which subscribed event was delivered or consumed. New trace-level
records correlate queue subscription, event matching, wait results and EOP
packet interrupt/data selections without changing synchronization behavior.

These are localized waits, not a confirmed renderer root cause or a completed
fix. No semaphore signal or GPU completion is fabricated to advance execution.
Private diagnostics and original-game disassembly remain under ignored out/.
