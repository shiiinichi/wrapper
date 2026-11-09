# Native macOS ARM64 Apple Music authentication experiment

This experiment executes the actual Android ARM64 Apple Music initializers and
service bootstrap and authentication callbacks in a native arm64 macOS process. It uses no
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
./script/build_and_run.sh --login
```

The Run action builds the Mach-O executable, extracts only pinned library
blobs, and runs isolated execution, initialization, and service probes. Each child
has a 15-second timeout and core dumps disabled. Debug mode opens the actual
`libandroidappmusic.so` initialization path in LLDB.

Assets, SHA-256/Git-blob provenance (`manifest.json`), binaries, and evidence
(`report.json`) stay under ignored `build/native-poc/`. The default service probe
uses a fresh temporary directory there and removes it afterward. No existing
wrapper account data is copied.

## Native sign-in and verification

`--login` opens local macOS dialogs for Apple Account email, password, and (only
when requested by Apple) a six-digit verification code. Password/code fields are
masked. The authentication child reads them through a private pipe; credentials
are never command arguments, environment variables, or report fields. Android
library logging and curl verbose logging are disabled during authentication.

The persistent profile is separate from bootstrap probes and the Linux wrapper:
`build/native-poc/auth-profile/`. Its random 16-character device identifier is
saved once in `native-device-id` with mode `0600`; the directory is `0700`.
Apple's library owns account, cookie, and provisioning storage there. Cancelled
and failed sign-ins return nonzero, with `authenticated:false`. Successful sign-in
requires response type 6, a positive account identifier, and a usable context
account. Diagnostic reports stay in the ignored profile with mode `0600`.

The default Run action performs a fresh, noninteractive authentication preflight:
it runs the real AuthenticateFlow and returns a real cancellation response at
the credential callback. It never asks for or submits account credentials.
The callback implementation uses separate full-length password/code strings,
keeps the original password for each new code, and bounds retries. A wrong
password does not trigger automatic resubmission. Additional protocol dialogs
show Apple's message and numbered button choices on macOS. The selected action
is returned through the actual Android presentation interface; Cancel returns
an empty dialog response. Message text travels through the private stdin pipe,
not command arguments. A manual dialog choice can permit a fresh credential
prompt when Apple requests one.

An explicit network preflight fetches Apple's public URL bag and exercises a
non-account Mescal signing challenge:

```sh
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --apple-profile --probe auth-network --data-dir build/native-poc/network-preflight
```

This requires network access. Curl adapters translate Linux register varargs to
Darwin stack varargs for easy/share/multi options and typed getinfo outputs.
Certificate and hostname verification stay enabled, and Android system CA paths
map to the macOS CA bundle. Unreviewed SSL-context/socket-structure callbacks
remain guards.

To inspect a persisted account without validating credentials or starting login:

```sh
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --apple-profile --probe auth-status --data-dir build/native-poc/auth-profile
```

A stored account is persistence evidence, not a token-validity check.

The live development-Mac test now completes with `authenticated:true`, response
type `6`, a positive account identifier, `account_present:true`, and exit status
`0`. A separate process reloads that profile with `stored_account:true`; all four
databases pass `PRAGMA quick_check`. The final successful attempt had zero
verification-code and protocol-dialog requests. The verification-code callback
was exercised in an earlier live attempt, but a complete fresh 2FA sign-in in a
single process still needs independent confirmation. Protocol-dialog payload
parsing and callback wiring are tested; the successful run did not require a
protocol-dialog interaction. Authenticated manifest/key requests and playback
have not been tested yet.

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

**The default service probe is bootstrap before sign-in.** It does not read
credentials, start authentication, or request a lease. Automatic lease refresh
is disabled; the manager receives empty Android callbacks. Presentation callbacks
stop unexpected credential/dialog interaction. Manifest/key retrieval, FairPlay
decryption, HTTP endpoints, and playback remain future work. Creating
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
  capture guest destructor registration, pthreads, or stdio. CoreFoundation string formatting translates object/scalar/vector/stack
  arguments and positional formats, including Android va_list input. Binary128
  and unreviewed format extensions remain guarded. Typed curl adapters are
  described above. Android logging and `snprintf`/`vsnprintf` have
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
  CoreADI file creation also has a typed process-local umask bridge.
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

## Native database backend and x18

macOS reserves register `x18`; an independent assembly loop reproduced the kernel
clearing it. A stress run exposed Android SQLite using that register for a live
pointer. The Apple profile now interposes public `sqlite3_*` bindings onto macOS
SQLite, including bindings defined inside MediaPlatform. All database handles
and statements stay with the native backend. Reviewed SQLite configuration
varargs have typed adapters; other variadic/extension interfaces remain guards.
Native database opens are limited to the dedicated profile (or `:memory:`).

This removes the observed Android SQLite failure without modifying encrypted
code/data tables in the proprietary libraries. It does not implement general
`x18` virtualization for arbitrary Android machine code. Paths beyond the tested
bootstrap/authentication operations still need independent runtime validation.
The report counts successful native SQLite opens; persistence tests require this
backend and validate the resulting standard SQLite databases.

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
activation, and malformed ELF rejection. Authentication regressions run the real
callback cancellation, verify CoreFoundation formatting via an independent Linux
assembly caller and Android va_list, and verify a 512-character password plus six-digit code through
Android getters, and check stable/private device identity. Curl tests verify exact
callback bytes and rejection of an untrusted local HTTPS certificate even when
the guest requests verification off. `--verify` needs permission to bind localhost;
the cancelled-login test also reaches the public network preflight when permitted.

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
[AOSP sysconf names](https://github.com/aosp-mirror/platform_bionic/blob/android-7.1.2_r39/libc/include/sys/sysconf.h),
[SQLite C API](https://www.sqlite.org/capi3ref.html),
[Apple ARM64 conventions](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms).
