# Experimental Ivy Bridge / BMI1 compatibility

This change adds a Windows guest-instruction fallback, not general AVX2 emulation.
It requires rebuilding `gow3-probe.exe`. Copying XML or Python files does not install it.

## Evidence

The supplied CUSA01623 v01.02 executable (SHA256
`6e72330e9092e5164049b865e5568f2d450904001a73fe0a2f4b39194c195b3c`)
contains valid BMI1 instructions that Intel Ivy Bridge does not implement:

| Instruction | Static occurrences in the executable code scan |
| --- | ---: |
| ANDN | 139 |
| BEXTR | 846 |
| BLSI | 28 |
| BLSR | 3 |

The linked guest libc and Fios2 modules also contain BMI1. The existing game loader
runs these instructions directly on the host CPU. A missing instruction can therefore
raise STATUS_ILLEGAL_INSTRUCTION even if all host libraries have CPU dispatch.
Static scans are not execution traces and do not establish which instruction caused
a particular user's crash. Optimized AVX2 code inside DLLs may be correctly guarded by
runtime CPU dispatch and must not be removed indiscriminately.

## Behavior

At startup the Windows loader checks CPUID leaf 7 for BMI1. On a CPU without BMI1,
its existing vectored exception handler attempts to emulate ANDN, BEXTR, BLSI,
BLSMSK and BLSR only for illegal-instruction faults in the guest address range.

The decoder supports 32/64-bit operands, extended registers, ModRM/SIB addressing,
signed displacements and RIP-relative memory. It preserves other registers, updates
the defined arithmetic flags and advances RIP only after a successful emulation.
Memory is read through ReadProcessMemory. Unsupported encodings or inaccessible
operands retain the existing fatal-reporting path.

CPUs with BMI1 keep the native execution path. No game files are modified.

## Build and test

Use the repository's existing MSYS2 CLANG64 build environment and dependencies:

```bash
bash build.sh
ninja -C out/gpu bmi1-compat-test
out/gpu/bmi1-compat-test.exe
```

The scalar test can also be built without GPU dependencies:

```bash
cc -std=c11 -O2 -Wall -Wextra -Werror tests/test_bmi1_compat.c -o bmi1-compat-test
./bmi1-compat-test
```

Alternatively, the manual GitHub Actions workflow **Experimental BMI1 compatibility
build** builds the full runtime, runs the scalar test, and uploads
`experimental-bmi1-runtime` with a `bin/` directory and its MSYS2 DLL dependencies.
The workflow has not been run during local development. After a successful run,
test the artifact in a separate copy of the existing port, preserving the FPS and
resolution XML/Python files. The artifact is not a complete game package.

Validation completed during development:

- Compiled the fallback and Windows CONTEXT adapter for x86_64 Ivy Bridge.
- Passed 100,000 register/flag checks across 32/64-bit operations, plus invalid
  encoding, truncated instruction and unreadable memory checks.
- Passed 30,480 additional semantic checks against an independent model, covering
  all 1,016 ANDN/BEXTR/BLSI/BLSR sites detected in the supplied game executable.
- Did not build the complete graphics runtime or test gameplay on an i7-3770.

## Limitations and acceptance criteria

This is an experimental exception-based implementation. Frequently executed BMI1
instructions can cause substantial overhead. A future optimization may translate
verified instructions into native baseline sequences and cache the result.

TZCNT is deliberately not intercepted: without BMI1 its encoding executes as BSF,
so it does not raise the exception handled here. Nonzero-input uses that ignore
the differing flags can be compatible; zero-input and flag-dependent uses require
separate analysis. This implementation does not establish their correctness.

The fallback does not emulate AVX2, BMI2, FMA or unsupported prefixes such as
segment/address-size overrides. It does not change host DLL CPU dispatch or GPU
requirements. Further failures may require additional work.

Before claiming i7-3770 support, rebuild the complete port and test startup,
cinematics, loading, combat, saves and a sustained gameplay session on that CPU.
Compare behavior and performance with a BMI1-capable CPU. Preserve the existing
FPS/resolution fixes when installing the rebuilt runtime.
