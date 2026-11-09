# Native macOS ARM64 library loader proof of concept

This is a separate command-line experiment on `lite`. It
loads AArch64 Android ELF shared libraries into an arm64 macOS process. It uses
the Mac CPU directly, anonymous memory mappings, and a small explicit symbol
bridge. It does not launch QEMU, Linux, Android, an APK, or the music service.

The baseline is the original `WorldObservationLog/wrapper` `lite` branch at
`e51e370a6b0660ed9d722fff1add079098aeb935`. The ARM64 library source is the
immutable `arm64` commit `c0a456f578212ef77d53aa2b5ad8cfdfe25a63c2`.

## Run

Requires an Apple Silicon Mac, Xcode command-line tools, Python 3, and the
repository's ARM64 Git history. No package manager or additional loader library
is required.

```sh
./script/build_and_run.sh
./script/build_and_run.sh --verify
./script/build_and_run.sh --debug
```

The build script produces a native arm64 Mach-O executable and extracts the
pinned `.so` blobs into `build/native-poc/assets`. It never copies account data,
credentials, or files from an existing wrapper service. `manifest.json` records
each library's Git blob and SHA-256. All extracted binaries, build products, and
generated reports stay inside the existing ignored `build/` directory.

`--verify` runs malformed-ELF/ABI tests and the normal probes. `--debug` opens
the Blocks probe in LLDB. `--logs` and `--telemetry` run the same bounded probes;
their evidence is JSON and stderr, with no background daemon.

The Run action invokes the same build/run script.

## What executes

The default run launches each probe in a separate process with a 15-second
timeout and core dumps disabled. It loads the **Android versions** of:

* `libz.so`: prepares initialization, checks `zlibVersion`, compresses/restores a
  6,656-byte payload, and checks CRC-32 against a known value.
* `libBlocksRuntime.so`: prepares initialization, copies a stack block to the heap,
  invokes it, retains/releases it, and checks that the Android library calls
  the native macOS copy/dispose callbacks exactly once.

The second probe exercises calls in both directions across the boundary. It
uses the C block layout described by Clang's
[Blocks ABI](https://clang.llvm.org/docs/Block-ABI-Apple.html).
The pinned versions have no non-null constructors to invoke; the report records
zero constructor calls rather than claiming an Apple library was initialized.

On the development Mac (macOS 27.2, arm64, 16,384-byte pages), both probes pass.
Android zlib reports `1.2.8`, compresses the payload to 101 bytes, restores it
exactly, and computes CRC-32 `0x414fa339`. The Blocks invocation returns `42`.

## Inspecting Apple libraries

The default run also maps and attempts relocation of `libCoreADI.so`,
`libCoreLSKD.so`, `libstoreservicescore.so`, and `libandroidappmusic.so`, including
their available dependency graphs. **Inspection does not enable execution or
run their constructors.**

Full evidence is written to `build/native-poc/report.json`. To inspect one:

```sh
build/native-poc/wrapper-native-poc \
  --assets build/native-poc/assets --inspect libCoreLSKD.so
```

The report distinguishes mapped images, applied relocations, unresolved strong
imports, zero-valued weak imports, guarded imports, unsupported relocations/TLS,
and host pages with incompatible permissions. An inspection returning `ok:true`
means the inspection completed; it does not mean that the library can run.

`execution_prepared` and `constructors_initialized` show whether code execution
was actually enabled. `relocated_without_blockers` also excludes known
dependency/relocation/page-layout issues, but does not imply complete Android
OS compatibility: guarded imports remain traps.

Observed first blockers:

* `libCoreADI.so` and `libCoreLSKD.so` each have a code/data boundary that places
  executable and writable bytes in the same 16 KiB macOS page. The loader
  reports the conflict and refuses to initialize these libraries. It never
  requests writable/executable pages or changes host security settings.
* Their imports include Android properties, file flags/stat layouts, pthread
  object layouts, and Linux/Darwin variadic calling-convention differences.
  Guarded calls exit the isolated process with status `86`; they never return
  fake success or blindly forward these calls to Darwin.
* The full Music dependency graph still has unresolved Android/platform and
  C++ symbols. These are listed per library; initialization is blocked.

This establishes native execution of Android libraries and a repeatable way to
measure Apple-library blockers. Authentication, FairPlay context capture, and
audio playback have not been implemented or tested by this experiment.

## Loader boundary

The loader implements little-endian ELF64/AArch64 `PT_LOAD`, `PT_DYNAMIC`,
SysV/GNU symbol hashes, ordinary RELA `ABS64`, `GLOB_DAT`, `JUMP_SLOT`, and
`RELATIVE` relocations, dependency traversal, and init arrays. It validates file
ranges, overflow, allocation limits, strings, relocation destinations, and
dependency basenames before using them. It maps writable memory first, applies
relocations, then gives each host page its final permissions for reviewed
execution probes. Instruction-cache invalidation only touches code segments,
because inaccessible inter-segment gaps fault on Apple Silicon.

Only reviewed memory/string functions and process-local destructor registration
are bridged. The Android `libc.so`, `libm.so`, `libdl.so`, `liblog.so`, and
`libstdc++.so` names are explicit shim modules; their Linux implementations are
not executed. Unknown symbols stay unresolved. This is not a general Bionic,
C++ exception/unwind, Java/JNI, Linux syscall, TLS, or ELF interposition
implementation. Guest destructors are not automatically run at unload; probe
process exit owns cleanup. General ARM64 ABI issues such as reserved registers,
signals, and variadic calls still need a wider compatibility implementation.

The probes use the default local linker/ad-hoc signing behavior. No hardened
runtime, library-validation exception, JIT entitlement, or notarization change
is needed for the tested read/write-to-read/execute transitions.

The next milestone is a deliberate solution for the 4 KiB/16 KiB layout conflict
and typed Android ABI bridges for one small Apple library. The full Music
dependency tree should remain an inspection target until those interfaces are
implemented and checked independently.

Reference formats: [AArch64 ELF ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst).
