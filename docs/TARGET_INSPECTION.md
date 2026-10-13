# Initial target inspection

Target observed locally: The Last Guardian, CUSA03627, APP_VER 01.03.
The SFO VERSION field remains 01.00; APP_VER identifies the installed update.
No execution or rendering milestone has been validated.

## Reproduce

Run `python tools/inspect_target.py GAME_DIRECTORY` from the repository.
The tool reads the executable and bundled PRX files and writes metadata to
`out/target-inspection.json`. It does not patch, execute, or copy executable
bytes into its report. Generated reports stay in the ignored `out/` directory.

## Observations

- Executable source SHA-256:
  `b7f61057861b9704cf157a925659ad74a92a49daf33bbd6c06a2f5ee02f9a5cb`.
- Loaded image SHA-256 (loadable segments and zero-filled memory):
  `da302cb58cfe4b3711e81458c416e761c291ffea76d194bbeac9efad08b4afe8`.
- Entry point: `0xa0`; loaded image: 95,468,404 bytes.
- Main executable: 516 undefined named symbols, across 22 library identities.
- Bundled modules parsed: `libc.prx` and `libSceFios2.prx`.
- Assets use `trico_*.psarc`, unlike Bloodborne's `dvdroot_ps4` layout.
- Main imports include 12 Audio3d and 10 Ngs2 symbols. No Audio3d/Ngs2
  implementation was found in the inherited `src/` or `gpu/shim/` sources.
  Their runtime necessity must be established by execution traces.

## First implementation blockers

1. `scripts/game_check.py` accepts Bloodborne identities and image hashes only.
   Add an explicit experimental target path; do not replace its Bloodborne hash
   or bypass validation globally.
2. `src/runtime.c` resolves many symbols using complete NID/library/module strings
   from the Bloodborne dump. This target uses different local suffixes: for example,
   `sceAppContentInitialize` is `R9lA82OraNs#T#U` here. Resolve by verified library
   and module identities, without dropping scope globally. Static string matches
   and NID/name candidates in the inspection report are not compatibility proof.
3. Audit `run.sh`, patches, content profiling, graphics assumptions, and hooks before
   any launch. Do not apply inherited Bloodborne patches to this executable.
4. The upstream build and launcher target Linux. A Windows build requires a
   separate platform adaptation; the existing GoW runtime is a reference, not
   an interchangeable binary for this project.

## Next milestone

Prepare and link a validated experimental TLG image without Bloodborne patches,
then enumerate unresolved imports by full identity. Only after that, attempt a
controlled first boot with appropriate crash diagnostics and a compatible host build.

## Offline preparation implemented

`python scripts/prepare_tlg.py GAME_DIRECTORY` now selects the observed USA 1.03
target explicitly, validating its title, update and loaded executable hash. It also
checks the loaded hashes of both bundled libraries. The legacy skip-check variable
cannot bypass this target validation. The standard Bloodborne preparation path is
unchanged except for accepting an optional validator from this dedicated script.

The generated files are under ignored `out/tlg/`. No Bloodborne patch compiler,
content profile, graphics profile or launcher is invoked. `boot-linked.bin` uses
the inherited Linux TLS rewrite and is **not** an image validated for a Windows
runtime. Do not launch it with the GoW executable or the inherited `run.sh`.

On the observed local files, preparation completed with 30,062 main executable
relocations, 209 native module bindings and 391 host-runtime import entries.
`tlg-link.json` records their full library/module identities and counts by library.
Those 391 entries are not necessarily missing implementations: runtime resolution
must still be audited with a compatible host build. No game code was executed.

## Windows entry diagnostic

A separate Windows-only diagnostic (`src/windows_entry_probe.c`) now accepts the
unlinked `boot.bin`, applies relocations, and traps every imported function/data
access. It never initializes linked modules or applies Linux TLS rewrites.

The locally compiled diagnostic executed this target's original entry at `0xa0`
and stopped at `bzQExy189ZI#V#V` (`_init_env`), caller offset `0xb9`, exit 20.
This validates only this attempt at mapping, relocation and entry execution.
No game initialization, graphics or gameplay is demonstrated. Four synthetic
Windows tests cover import traps, data traps, guest faults and invalid inputs.
The GitHub workflow packages the diagnostic and source without game data.

## Verified `_init_env` return

The Windows diagnostic now resolves only `bzQExy189ZI#V#V` as a function, gated
by the preparation capability bit proving the supplied libc export is exactly
RET. Other scopes and data imports are not resolved by this contract.

The observed USA 1.03 executable returned from `_init_env` once and next stopped
at `8G2LB+A3rzg#V#V` (`atexit`), caller offset `0xc1`, exit 20. This is still
startup code, not successful game initialization. Six synthetic Windows tests
now include the verified return and rejection without evidence or exact scope.

This Windows work is maintained independently by LuizDev1, with bbport as the
original upstream. Spexxl's Linux project is a separate effort. The current
hosting location remains the dedicated `the-last-guardian` branch of `gow3_pc`;
the God of War main branch is unrelated to these Windows diagnostic changes.

## `atexit` and stack canary startup

The Windows diagnostic registers `atexit` callbacks with SysV calling convention,
an SRW lock and an allocation failure path. Normal guest termination drains the
registry in reverse registration order. Callbacks are removed before invocation
and can register additional callbacks; diagnostic stops do not run them.

The exact observed `__stack_chk_guard` data import is backed by a random canary
from Windows BCryptGenRandom, with its low byte cleared. Resolution distinguishes
function and data imports and retains the preparation evidence and scope gates.

The observed executable now registers two exit callbacks and reaches
`D0OdFMjp46I#U#H` (`sceKernelCreateEqueue`), caller offset `0x14944fd`, exit 20.
Eight native Windows tests cover previous behavior, callback order/reentrancy and
readable canary data. Event queues are not implemented by this diagnostic yet;
no graphics or gameplay has been validated.

## Event queue lifecycle

`src/windows_equeue.c` implements creation/deletion using a locked registry of
64-bit guest handles, distinct handles for live queues, bounded names and
guest error codes. Unknown/deleted handles fail without dereferencing pointers.
There is no event registration, signaling or waiting implementation yet.

The observed executable creates `EOP QUEUE` (handle `0x40000`) and stops at
`b0xyllnVY-I#E#F` (`sceGnmAddEqEvent`), caller offset `0x1494510`, exit 20.
This is the next boundary between the CPU startup and GPU event system.
Nine native Windows tests include queue lifecycle and invalid-handle behavior.
