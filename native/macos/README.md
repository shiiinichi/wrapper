# Native macOS ARM64 Apple Music library initialization

This command-line experiment now executes the actual Android ARM64 Apple Music
library initializers in a native arm64 macOS process. It uses no QEMU, Linux VM,
APK runtime, or Rosetta. The original wrapper HTTP service remains separate.

The original `WorldObservationLog/wrapper` `lite` baseline is
`e51e370a6b0660ed9d722fff1add079098aeb935`. The pinned ARM64 binary source is
`c0a456f578212ef77d53aa2b5ad8cfdfe25a63c2`.

## Run and verify

Requires Apple Silicon, Xcode command-line tools, Python 3, and this repository's
ARM64 Git history. No additional packages are required.

```sh
./script/build_and_run.sh
./script/build_and_run.sh --verify
./script/build_and_run.sh --debug
```

The default Run action builds the native Mach-O executable, extracts only
pinned library blobs, and runs isolated execution and initialization probes.
Each child has a 15-second timeout and core dumps disabled. Debug mode opens the
actual `libandroidappmusic.so` initialization path in LLDB.

Assets, SHA-256/Git-blob provenance (`manifest.json`), binaries, and evidence
(`report.json`) stay under the ignored `build/native-poc/` directory. The extractor
does not copy account data or an existing wrapper's data directory.

## Verified initialization milestone

On the development Mac (macOS 27.2, arm64, 16 KiB pages), the default run verifies:

| Target | Actual executed result |
| --- | --- |
| `libCoreADI.so` | Its real `JNI_OnLoad` returns JNI 1.6 |
| `libCoreLSKD.so` | Its real `JNI_OnLoad` returns JNI 1.6 |
| `libstoreservicescore.so` | 64 ELF initializers return; 73 including dependencies |
| `libmediaplatform.so` | 7 ELF initializers return |
| `libandroidappmusic.so` | 91 ELF initializers return |
| Full Music initialization profile | 373 initializers return, including 209 in `libmedialibrarycore.so` |

The pinned CoreADI/CoreLSKD `JNI_OnLoad` functions do not request a JNI environment
or register methods. The report records those counts as zero. The small JNI host
has no Java class definitions or fabricated method registrations; unsupported
JNI calls terminate the child.

The original native zlib round-trip/CRC test and Android Blocks-to-macOS callback
test remain independent checks.

**This milestone is native library initialization.** RequestContext/service
configuration, sign-in, manifest/key retrieval, FairPlay context capture, and
playback are not wired into the helper yet. Passing the initializers does not
establish that every later API or full application lifetime is compatible.

## Compatibility profile

```sh
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --apple-profile --initialize libandroidappmusic.so
```

The explicit Apple profile:

* Executes the original ELF code for Music, StoreServicesCore, MediaPlatform,
  MediaLibraryCore, CoreADI/CoreFP/CoreLSKD, ICU, Android libc++, XML, and zlib.
* Supplies native macOS CoreFoundation, dispatch, Blocks, and libcurl modules.
  Binding is limited to each module's symbol family, so a dependency's libc
  exports cannot capture guest destructor registration, pthreads, or stdio.
  Known variadic/`va_list` interfaces remain guarded.
* Rewrites inline Android `MRS TPIDR_EL0` reads in executable ELF sections to
  native ARM64 adapters. The adapters save/restore GPRs, vector registers,
  NZCV, FPCR, FPSR, LR, and SP and supply a per-thread compatibility block with
  the stack-canary slot. The main profile adapts 24,611 reads. This does not
  implement arbitrary ELF TLS, Linux thread structures, or all Android TLS slots.
* Translates Android mutex/condition objects and 32-bit pthread keys through
  owned native objects; implements real private futex WAIT/WAKE behavior and
  relevant Linux errno values. Linux syscall numbers are never sent to Darwin.
* Provides explicit Android stdio stream shells and typed file operations,
  locale-category mask translation, memory/string/math functions, and an empty
  Android-property namespace. Initializer `fopen` access is read-only.
* Uses the pinned OpenSLES/Wilhelm ELF data solely for `SL_IID_*` interface
  constants. These images have no executable pages, dependency services, or
  constructor calls in this profile. Audio engine functions remain guarded.
* Keeps guest destructor registrations in the guest registry. Full teardown,
  dynamic unload, and guest C++ exception/unwind integration are not implemented;
  the isolated process owns final cleanup.

Unimplemented function imports get named native fail-stop trampolines. Entering
one exits with status `86`; missing data imports still prevent execution. These
guards are visible in the report and are not successful API implementations.
Successful Music initialization children exit normally without entering a guard.

## 4 KiB ELF / 16 KiB host pages

Some older libraries place writable bytes next to code inside one macOS page.
Strict ELF-only loading still rejects that layout. The explicit Apple
initialization profile protects mixed pages **read/execute**, without write
permission. The tested initialization paths do not write those bytes and return
successfully. A later runtime write would fault: this is an initialization policy,
not a complete writable-layout solution for all FairPlay/service operations.

No page is mapped writable and executable. Adapters and import guards are written
before being protected RX. Host security settings, JIT entitlements, library
validation, and SIP are not changed. The helper uses normal local ad-hoc signing.

## Evidence and tests

`build/native-poc/report.json` records initializer counts, actual JNI return
values, TLS rewrite/adapter sizes, mixed-page permissions, native host modules,
data-only modules, skipped data-module relocations, guest destructor registrations,
and guarded/unresolved symbols per image.

Tests exercise real Apple initialization, an independently assembled AArch64 ELF
TLS fixture checking registers/flags/vectors and three concurrent thread contexts,
real pthread/futex synchronization, typed FILE/errno behavior, guard activation,
and malformed ELF rejection. Run `--verify` for the full suite.

Strict unadapted inspection remains available as a diagnostic baseline:

```sh
python3 native/macos/run.py --inspect-baseline
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --inspect libandroidappmusic.so
```

An inspection's `ok:true` means it completed, not that the library ran.
`constructors_initialized` and per-image `constructors_called` identify actual
execution. `relocated_without_blockers` remains false where the original writable
ELF layout is incompatible; `mixed_pages_readonly` records the explicit policy
used by the successful initializer probes.

References: [AArch64 ELF ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst),
[Clang Blocks ABI](https://clang.llvm.org/docs/Block-ABI-Apple.html),
[AOSP pthread ABI](https://android.googlesource.com/platform/bionic/+/android-5.1.1_r1/libc/include/pthread.h),
[AOSP stdio ABI](https://android.googlesource.com/platform/bionic/+/android-5.1.1_r1/libc/include/stdio.h).
