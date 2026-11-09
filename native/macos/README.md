# Native macOS ARM64 Apple Music service bootstrap

This experiment executes the actual Android ARM64 Apple Music initializers and
unauthenticated service bootstrap in a native arm64 macOS process. It uses no
QEMU, Linux VM, APK runtime, or Rosetta. The original HTTP wrapper remains separate.

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

The Run action builds the Mach-O executable, extracts only pinned library
blobs, and runs isolated execution, initialization, and service probes. Each child
has a 15-second timeout and core dumps disabled. Debug mode opens the actual
`libandroidappmusic.so` initialization path in LLDB.

Assets, SHA-256/Git-blob provenance (`manifest.json`), binaries, and evidence
(`report.json`) stay under ignored `build/native-poc/`. The default service probe
uses a fresh temporary directory there and removes it afterward. No existing
wrapper account data is copied.

## Verified bootstrap milestone

On the development Mac (macOS 27.2, arm64, 16 KiB pages):

| Target | Actual executed result |
| --- | --- |
| CoreADI / CoreLSKD | Each real `JNI_OnLoad` returns JNI 1.6 |
| StoreServicesCore | 64 initializers return; 73 including dependencies |
| MediaPlatform | 7 initializers return |
| AndroidAppMusic | 91 initializers return |
| Full Music graph | 373 initializers return, including 209 in MediaLibraryCore |
| Device identity | FootHill/DeviceGUID configuration returns error code 0; `isConfigured()` is true |
| RequestContext | Real factory and `init` return; error code 0; manager returns the same context |
| Local stores | Accounts, cookies, HTTP cache, and key/value SQLite databases are created and reopen successfully |
| Presentation | Actual AndroidPresentationInterface is installed |
| FairPlay services | RequestContext's FairPlay service and FootHill session controller are created |
| Lease manager | Actual constructor creates a PlaybackLeaseSession using the configured context |

The pinned CoreADI/CoreLSKD JNI entry points do not request a JNI environment or
register methods; the report records zero calls/registrations. Unsupported JNI
functions terminate the child. The independent Android zlib round-trip/CRC and
Blocks-to-macOS callback tests also remain.

**This milestone is service bootstrap before sign-in.** No credentials are read,
authentication flow started, or playback lease requested. Automatic lease refresh
is disabled; the manager receives empty Android callbacks. Presentation callbacks
stop unexpected credential/dialog interaction. Sign-in, manifest/key retrieval,
FairPlay decryption, HTTP endpoints, and playback remain future work. Creating
service objects does not prove those later operations work.

To retain a separate probe profile between invocations:

```sh
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --apple-profile --probe services --data-dir build/native-poc/service-data
```

`--data-dir` is required for this direct probe. Use a directory dedicated to the
native experiment. Its permissions are set to `0700`; guest file writes are
limited to that directory. The current disposable profile uses a fixed synthetic
Android identifier, not account/device data from the Linux wrapper.

Service initialization starts real guest worker threads. Their ELF mappings have
process lifetime: after emitting the result, this isolated child exits without
unloading live worker code. Graceful service shutdown and reusable in-process
embedding remain unimplemented. Other initializer probes use ordinary exit.

## Compatibility profile

The explicit `--apple-profile`:

* Executes original ELF code for Music, StoreServicesCore, MediaPlatform,
  MediaLibraryCore, CoreADI/CoreFP/CoreLSKD, ICU, Android libc++, XML, and zlib.
* Supplies macOS CoreFoundation, dispatch, Blocks, and libcurl modules. Binding
  stays within each module's symbol family so dependency libc exports cannot
  capture guest destructor registration, pthreads, or stdio. CF/curl variadic
  interfaces remain guarded. Android logging and `snprintf`/`vsnprintf` have
  explicit register/vector/stack and `va_list` translation. Unsupported formats
  and binary128 long doubles stop the child.
* Adapts inline `MRS TPIDR_EL0` reads using native ARM64 trampolines that preserve
  GPRs, vectors, NZCV, FPCR, FPSR, LR, and SP and supply a per-thread compatibility
  block with the canary slot. The full graph adapts 24,611 reads. Arbitrary ELF
  TLS and all Android thread structures/slots are not implemented.
* Translates mutex/condition/rwlock/once objects, thread attributes, and 32-bit
  pthread keys through owned native objects. Thread creation/join and private
  futex WAIT/WAKE perform real synchronization. Relevant Linux errno values are
  translated; Linux syscall numbers are never sent to Darwin.
* Provides typed Android FILE shells, locale masks, memory/string/math functions,
  and an empty Android-property namespace. POSIX bridges translate open flags,
  stat/flock/time structures, sysconf names, non-executable mmap, and the network
  interface queries used for device configuration. SHA-1 uses CommonCrypto.
  Guest writes require a dedicated data directory; initializer-only probes have
  no writable guest directory.
* Supports guest `dlopen`/`dlsym` only for already loaded ELF images/exports.
  Guest handles are not Darwin handles; `dlclose` never unloads process-owned code.
* Uses AAPCS64 adapters for nontrivial C++ return objects in `x8`. Android shared
  pointers, strings, and callbacks never use Darwin `std::__1` object layouts.
  Returned StoreErrorConditions are checked and destroyed by Android functions.
* Uses OpenSLES/Wilhelm data solely for original `SL_IID_*` constants. Their code
  is not executable; constructors and Android audio services are not started.
* Keeps guest destructor registrations in a guest registry. Full teardown,
  dynamic unload, and guest C++ exception/unwind integration remain unimplemented.

Unimplemented function imports get named fail-stop trampolines; entering one
exits with status `86`. Missing data imports prevent execution. Guards are visible
in reports and are not successful API implementations. Successful service probes
exit 0 without entering a guard and explicitly report their process-owned lifetime.

## 4 KiB ELF / 16 KiB host pages

Older libraries place writable bytes next to code inside one macOS page when
their image bias is host-page aligned. The Apple profile chooses a legal 4 KiB
load-bias offset inside the allocation: `+4096` for CoreADI/Android libc++,
`+12288` for CoreFP/CoreLSKD. Code/data boundaries then fall on separate host
pages. Relative addresses remain intact, code is RX, and writable data stays RW.
All sixteen images have zero mixed W/X pages. CoreLSKD's writes during device
configuration now complete.

Larger ELF alignment gets an aligned allocation within a bounded reservation.
Remaining code/data page conflicts still prevent execution; the earlier
mixed-page read-only fallback is removed. Strict ELF-only inspection preserves
the original layout for comparison.

No page is writable and executable. Adapters/guards are written before becoming
RX. Host security settings, JIT entitlements, library validation, and SIP are
unchanged. The helper uses ordinary local ad-hoc signing.

## Evidence and tests

`build/native-poc/report.json` records constructor/JNI results, service state,
TLS adapter counts/sizes, original/effective mixed-page counts, bias offsets,
host modules, data-only modules, guest destructors, and guarded/missing symbols.

Tests exercise real bootstrap twice in the same directory (including spaces)
and validate all four databases with `PRAGMA quick_check`. Independent AArch64
fixtures check indirect returns, TLS state preservation across three threads,
and variadic integer/vector overflow arguments. Tests also cover real pthread/
futex/rwlock/once synchronization, POSIX layouts/write isolation, SHA-1, guard
activation, and malformed ELF rejection. Run `--verify` for the full suite.

Strict baseline diagnostics remain available:

```sh
python3 native/macos/run.py --inspect-baseline
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --inspect libandroidappmusic.so
```

Inspection `ok:true` means inspection completed, not that code ran. Constructor
counts identify execution. `relocated_without_blockers` is now true for the Apple
profile and false for incompatible strict layouts. Retained report fields
`mixed_pages_readonly` / `mixed_pages_read_execute` are false/zero on new paths.

References: [AArch64 ELF ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst),
[AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst),
[Clang Blocks ABI](https://clang.llvm.org/docs/Block-ABI-Apple.html),
[AOSP pthread ABI](https://android.googlesource.com/platform/bionic/+/android-5.1.1_r1/libc/include/pthread.h),
[AOSP stdio ABI](https://android.googlesource.com/platform/bionic/+/android-5.1.1_r1/libc/include/stdio.h),
[AOSP stat layout](https://github.com/aosp-mirror/platform_bionic/blob/android-7.1.2_r39/libc/include/sys/stat.h),
[AOSP open/fcntl constants](https://github.com/aosp-mirror/platform_bionic/blob/android-7.1.2_r39/libc/kernel/uapi/asm-generic/fcntl.h),
[AOSP sysconf names](https://github.com/aosp-mirror/platform_bionic/blob/android-7.1.2_r39/libc/include/sys/sysconf.h).
