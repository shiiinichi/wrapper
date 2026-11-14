# Native macOS ARM64 desktop app and HTTP API

This implementation executes the Android ARM64 Apple Music libraries directly in
native arm64 macOS worker processes for service bootstrap, authentication, API
operations, and sample decryption. A SwiftUI desktop app manages the local server.
It uses no Docker,
QEMU, Linux VM, APK runtime, or Rosetta. The old Linux runtime remains in Git history.

The original `WorldObservationLog/wrapper` `lite` baseline is
`e51e370a6b0660ed9d722fff1add079098aeb935`. The vendored ARM64 libraries originated
from `c0a456f578212ef77d53aa2b5ad8cfdfe25a63c2`; that hash is provenance, not a
build dependency.

## Run and verify

Requires Apple Silicon, Xcode command-line tools, Python 3, and current macOS
security updates. The 12 required
Android ARM64 libraries are included directly in `native/macos/assets/` on `lite`.
A normal checkout or source archive contains everything needed to build; no
other branch, Git history, Android root filesystem, or Android NDK is required.

The repository's default build targets the native macOS app. The GitHub workflow
is disabled; use `script/build_and_run.sh --verify` for local verification. CMake
on macOS also delegates to that script. The old Linux build, launchers, root
filesystem, and documentation have been removed from the checkout.

```sh
./script/build_and_run.sh
./script/build_and_run.sh --app-build
./script/build_and_run.sh --serve
./script/build_and_run.sh --verify
./script/build_and_run.sh --probes
./script/build_and_run.sh --debug
./script/build_and_run.sh --login
```

The default Run action builds and opens
`build/native-poc/Native Music Wrapper.app`. The desktop app starts its own native
API at `http://127.0.0.1:12341`, shows saved-account status, offers masked sign-in,
and checks a supplied track ID. Closing the app stops its server and active
workers. The server watches its owning app process; it also stops if that process
is terminated. Signing in temporarily stops the server to avoid concurrent use
of the account profile.

`--serve --port 12341` runs the same server without the GUI. `--app-build` builds
the app without launching it. Run, package-build, serve, sign-in, and debug modes
stop the existing desktop app and native workers before proceeding. `--verify`,
`--probes`, and `--build` leave the running desktop app in place.

The bundle contains the native executable, Python
server modules, and pinned library blobs; account data stays outside the bundle.
Development launches, standalone servers, and sign-in use
`~/Library/Application Support/Native Music Wrapper` for account data, outside
build folders and Git checkouts. A direct Finder launch remembers the last profile.
Set `WRAPPER_PROFILE_DIR` when using the build script for an isolated development
profile, or pass `--data-dir` when invoking `http_server.py` or `sign_in.py`
directly. If an older profile exists under `build/native-poc/auth-profile`, select
that directory explicitly to reuse it.

`--probes` runs the earlier isolated execution, initialization, and service probes.
Each probe child has a 15-second timeout and core dumps disabled. Debug mode opens the actual
`libandroidappmusic.so` initialization path in LLDB.

Assets, SHA-256/Git-blob provenance (`manifest.json`), binaries, and evidence
(`report.json`) stay under ignored `build/native-poc/`. The default service probe
uses a fresh temporary directory there and removes it afterward. No existing
wrapper account data is copied.

Asset preparation reads the 12 required ARM64 libraries from
`native/macos/assets/`, including the native runtime dependencies and the
independent Blocks check. It validates their architecture, sizes, and SHA-256
checksums against the included manifest. Original commit, path, and blob IDs are
recorded only for provenance; the build never reads or fetches Git objects.
Unused Android platform libraries are omitted. The obsolete Android zlib 1.2.8,
libxml2 2.9.9 and ICU 57.1 copies are removed; their dependency names resolve to
maintained macOS libraries through reviewed C interfaces. The security ABI probe
checks the CVE-2022-37434 gzip-header and CVE-2023-29469 XML-dictionary conditions,
the system ICU interface, and fresh process-specific stack canaries.
New SDK account files use owner-only permissions. Apple web requests reject
cross-origin redirects and bound gzip output during decompression. The public
HTTP routes, request/response formats, default port and desktop workflow remain
unchanged.
Previously generated, unchanged unused libraries are pruned using their old
manifest hashes; caller-owned or modified files are preserved.

## Reusing the native core

`./script/build_and_run.sh --build` produces the standalone native executable
and validated `build/native-poc/assets/`. The native core has no dependency on
the desktop UI or Python HTTP adapter. Phonograph can bundle these artifacts or
vendor the C++/header/assembly sources and the pinned asset manifest; its host
adapter owns signing, sandbox inheritance and an app-private `--data-dir`.
Use one owner for each profile at a time.

The existing native `--api` operations and framed sample worker remain stable.
The original HTTP adapter remains available for clients using the established
HTTP contract. App-specific transport and account UI belong in the integrating
application; this security patch adds no XPC service or app allowlist.

## Local HTTP API

The server binds to loopback. Control endpoints use the original JSON envelope:

```json
{"code":0,"msg":"SUCCESS","data":{}}
```

API errors use a nonzero JSON `code` with HTTP status 200. Responses carry
`X-Wrapper-Backend: native-macos-arm64` so clients can identify this implementation.
The desktop also verifies a per-server instance identifier before reporting ready.

| Endpoint | Method | Parameters and behavior |
| --- | --- | --- |
| `/status` | GET | Native saved-account presence, storefront regions, and backend identity. |
| `/artist` | GET | Artist `adamId` and optional two-letter `storefront` (default `us`). Public Apple Music artist profile and logo. |
| `/m3u8` | GET | `adamId`; enhanced HLS from Apple's catalog using native-issued account tokens. |
| `/key` | GET | `adamId`, `uri`; returns the native persistent key and an opaque sample-decryption context. |
| `/decrypt` | POST | Binary sample batch; `X-Native-Context` from `/key`. Returns the same binary sample framing. |
| `/lyrics` | GET | `adamId`; optional `language`, `syllable`, and `script`; preserves TTML localizations. |
| `/webplayback` | GET | `adamId`; Apple web playback playlist using native-issued account tokens. |
| `/license` | POST | JSON `adamId`, `challenge`, `uri`, optional `drm-type` (`wv` or `pr`). Requires a real player/CDM challenge. |

`/artist` uses the public web developer token and works without a saved account.
`data` contains `adamId`, `storefront`, `name`, `url` (Apple Music page),
`genreNames`, `artwork` (Apple's portrait artwork object), `artistBio`, `origin`,
`bornOrFormed`, `editorialNotes`, and `artistLogo`. Optional metadata fields can
be null, and missing genres produce an empty array. Biography and editorial
text retain Apple's markup. The logo object contains a ready-to-use PNG `url`,
a PNG `urlTemplate` with `{w}` and `{h}` dimensions, and the source `width`
and `height`. An artist without a logo returns `artistLogo: null`. An unknown
artist in the selected storefront returns envelope code 404.

The server uses bounded native child processes and serializes access to each
profile. A child failure or timeout returns a JSON error while the HTTP process
stays available. Server token and context caches stay in process memory; request
logs omit query strings, challenge bodies, tokens, keys, signed URLs, and lyric
content. Browser-origin requests and foreign Host
headers are rejected; this development API is intended for local application clients.

**Native sample decryption uses a separate, versioned API.** `/key` returns the
actual persistent `contentKey`, `nativeDecryptionVersion:1`, and an opaque
`nativeContext` handle. `/status` advertises `native_sample_decryption:true`.
The x86 `ctx`, `state`, and register snapshots cannot be reused on ARM64, so
`temariCompatible:false` / `temari_compatible:false` still describe that legacy
template ABI. Clients must support the native API to process samples.
No Temari library is loaded or packaged by the native wrapper.

`POST /decrypt` uses `Content-Type: application/octet-stream` and the
`X-Native-Context` header. The body is a big-endian `uint32` sample count followed
by each sample's big-endian `uint32` length and bytes. Successful responses use
the same framing and content type; errors retain the JSON `code`/`msg` envelope.
Batches contain 1–8192 nonempty samples, each at most 4 MiB, with a total body
limit of 16 MiB minus 8 bytes. Samples are processed independently: complete
16-byte blocks are decrypted and each unaligned tail is preserved. Sample lengths
and ordering stay unchanged.

The HTTP process retains at most 32 context descriptors. Contexts expire after
30 idle minutes; expiry is checked on key/decryption requests. One isolated ARM64
worker loads the libraries once and serves repeated batches. The idle worker
stays loaded until shutdown or recycling.
It is recycled after 64 key attempts, before the library's internal context limit;
live descriptors are rehydrated lazily after recycling or a worker exit. Before
opening a new context, the worker resets the library's per-asset key cache so a
different key URI for the same song cannot reuse another rendition's key. Existing
contexts retain their owning references. Requests share the existing profile lock,
have bounded pipe deadlines, and stop their worker
on server shutdown. HTTP cache storage is disabled; plaintext samples are neither
persisted nor logged. An unknown/expired handle returns code 409 so a client can request `/key`
again. The service never forwards samples to an external decryption backend.

The desktop shows endpoint summaries without displaying keys, tokens, signed
manifest URLs, or lyric text. Its license row explains why a DRM challenge is
needed instead of inventing a successful license response.

Every response carries an `app;dur=...` `Server-Timing` measurement. Sanitized
timestamp/endpoint/method/result-code/server-milliseconds records append to
`http-timings.jsonl` inside the private profile with mode `0600`. The desktop's Response column
measures the client round trip for each API call, separately from subsequent
playlist validation. Copy Summary includes these measurements; Show Timings
reveals the private file. Timings include cold token acquisition when needed.

## Token lifecycle

The native library reloads account/session state from the private profile. The
first web API request obtains an Android developer token through the native
signed `apiToken` request, then exchanges that assertion for a music user token
through native `createMusicToken`. Both results are cached in the server's RAM
for 30 minutes and fetched again on the next request after that interval.

The AMPWebPlay developer JWT is read from Apple's web application assets and
cached in RAM for 24 hours. Web requests prefer this JWT; HLS, lyrics, and
WebPlayback can fall back to the native developer token. License requests require
the web JWT. A native `/key` request uses the library's FairPlay account state
directly and does not use the server's web-token cache.

These intervals are cache policies, not Apple's token expiration times. There
is currently no JWT-expiry scheduling, background refresh, or invalidate/refresh/
retry after an upstream 401/403. Stopping the server discards the RAM caches but
does not sign out or revoke the saved account. Sign In Again stops the server,
runs native authentication, and restarts it with empty caches. Account presence
in `/status` is not a credential-validity check. The app does not implement
Keychain storage or its own refresh-token protocol; persistent session handling
belongs to the native library.

## Native sign-in and verification

`--login` opens local macOS dialogs for Apple Account email, password, and (only
when requested by Apple) a six-digit verification code. Password/code fields are
masked. The authentication child reads them through a private pipe; credentials
are never command arguments, environment variables, or report fields. Android
library logging and curl verbose logging are disabled during authentication.

The persistent profile is separate from bootstrap probes and the Git checkout:
`~/Library/Application Support/Native Music Wrapper/`. Its random 16-character device identifier is
saved once in `native-device-id` with mode `0600`; the directory is `0700`.
Apple's library owns account, cookie, and provisioning storage there. Cancelled
and failed sign-ins return nonzero, with `authenticated:false`. Successful sign-in
requires response type 6, a positive account identifier, and a usable context
account. Diagnostic reports stay in the ignored profile with mode `0600`.

`--probes` performs a fresh, noninteractive authentication preflight:
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
  --apple-profile --probe auth-status --data-dir "$HOME/Library/Application Support/Native Music Wrapper"
```

A stored account is persistence evidence, not a token-validity check.

Recorded sign-in validation on the development Mac completed with
`authenticated:true`, response
type `6`, a positive account identifier, `account_present:true`, and exit status
`0`. A separate process reloads that profile with `stored_account:true`; all four
databases pass `PRAGMA quick_check`. The final successful attempt had zero
verification-code and protocol-dialog requests. The verification-code callback
was exercised in an earlier live attempt, but a complete fresh 2FA sign-in in a
single process still needs independent confirmation. Protocol-dialog payload
parsing and callback wiring are tested; the successful run did not require a
protocol-dialog interaction. API validation is separate from this sign-in
milestone; saved-account presence alone does not establish token validity or playback.

## Verified bootstrap milestone

On the development Mac (macOS 27.2, arm64, 16 KiB pages):

| Target | Actual executed result |
| --- | --- |
| CoreADI / CoreLSKD | Each real `JNI_OnLoad` returns JNI 1.6 |
| StoreServicesCore | 64 initializers return; 72 including dependencies |
| MediaPlatform | 7 initializers return |
| AndroidAppMusic | 91 initializers return |
| Full Music graph | 372 initializers return, including 209 in MediaLibraryCore |
| Device identity | FootHill/DeviceGUID configuration returns error code 0; `isConfigured()` is true |
| RequestContext | Real factory and `init` return; error code 0; manager returns the same context |
| Local stores | Accounts, cookies, HTTP cache, and key/value SQLite databases are created and reopen successfully |
| Presentation | Actual AndroidPresentationInterface is installed |
| FairPlay services | RequestContext's FairPlay service and FootHill session controller are created |
| Lease manager | Actual constructor creates a PlaybackLeaseSession using the configured context |

The pinned CoreADI/CoreLSKD JNI entry points do not request a JNI environment or
register methods; the report records zero calls/registrations. Unsupported JNI
functions terminate the child. The independent system-zlib round-trip/CRC and
Blocks-to-macOS callback tests also remain.

**The default service probe is bootstrap before sign-in.** It does not read
credentials, start authentication, or request a lease. Automatic lease refresh
is disabled; the manager receives empty Android callbacks. Presentation callbacks
stop unexpected credential/dialog interaction. This probe does not exercise
sample decryption or playback; the native API checks below cover decryption
separately. Creating service objects does not prove those later operations work.

To retain a separate probe profile between invocations:

```sh
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --apple-profile --probe services --data-dir build/native-poc/service-data
```

`--data-dir` is required for this direct probe. Use a directory dedicated to the
native experiment. Its permissions are set to `0700`; guest file writes are
limited to that directory. The disposable service probe uses a fixed synthetic
Android identifier and does not use saved account/device data.

Service initialization starts real guest worker threads. Their ELF mappings have
process lifetime: after emitting the result, this isolated child exits without
unloading live worker code. Graceful service shutdown and reusable in-process
embedding remain unimplemented. Other initializer probes use ordinary exit.

## Compatibility profile

The explicit `--apple-profile`:

* Executes original ELF code for Music, StoreServicesCore, MediaPlatform,
  MediaLibraryCore, CoreADI/CoreFP/CoreLSKD and Android libc++.
* Supplies macOS CoreFoundation, dispatch, Blocks, libcurl, zlib, XML and ICU C interfaces. Binding
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
  CoreFP's verified inline `gettimeofday` syscalls use register-preserving
  adapters at thirteen pinned call sites; expected instruction pairs are checked
  before patching. No Linux syscall number is passed to the macOS kernel.
  Checked reads enforce buffer capacity, and sleep uses the native ABI.
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
`x18` virtualization for arbitrary Android machine code. Coverage is limited to
the bootstrap, authentication, API, and sample-decryption operations listed below.
The report counts successful native SQLite opens; persistence tests require this
backend and validate the resulting standard SQLite databases.

## 4 KiB ELF / 16 KiB host pages

Older libraries place writable bytes next to code inside one macOS page when
their image bias is host-page aligned. The Apple profile chooses a legal 4 KiB
load-bias offset inside the allocation: `+4096` for CoreADI/Android libc++,
`+12288` for CoreFP/CoreLSKD. Code/data boundaries then fall on separate host
pages. Relative addresses remain intact, code is RX, and writable data stays RW.
All sixteen images in the service dependency graph have zero mixed W/X pages.
The seventeenth bundled library is Android's Blocks runtime for its independent
probe; the service graph uses macOS Blocks. CoreLSKD's writes during device
configuration complete.

Larger ELF alignment gets an aligned allocation within a bounded reservation.
Remaining code/data page conflicts still prevent execution; the earlier
mixed-page read-only fallback is removed. Strict ELF-only inspection preserves
the original layout for comparison.

No page is writable and executable. Adapters/guards are written before becoming
RX. Host security settings, JIT entitlements, library validation, and SIP are
unchanged. The helper uses ordinary local ad-hoc signing.

## Evidence and tests

Latest local validation: **2026-11-11**, after removal of the obsolete
zlib/XML/ICU copies, randomized stack canaries, private account-file permissions,
and redirect/decompression hardening. The original HTTP contract and desktop
sources are retained. Asset preparation also works from an ordinary source copy
without `.git` or `rootfs/`.

| Suite | Passing tests |
| --- | ---: |
| Native loader, ABI and security | 36 |
| HTTP API, profile storage, and packaging | 17 |
| Native sample worker | 8 |
| Vendored native asset preparation | 8 |
| Upstream redirect/decompression safeguards | 4 |
| **Total** | **73** |

The original desktop package builds and passes strict signature verification.
The native executable and assets also bootstrap from a temporary directory
outside the checkout, and a real SDK-backed HTTP `/status` request preserves
the original headers and response format using a fresh unsigned-in profile.

Earlier saved-account validation covered ALAC, AAC and EC-3 sample batches
without audio playback. Those account-dependent checks are separate from this
security regression suite. GitHub Actions remains disabled.

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
The HTTP suite checks backend identity, request validation, localhost-only
access, child failures/timeouts, token redaction, and line-lyric localization.
The indirect-return fixture includes a ninth stack argument, as used by the
native FairPlay key operation.
Asset regressions stage libraries from an ordinary source copy without `.git`
or `rootfs/`, and cover checksums, architecture, missing libraries, pruning, and
preservation of caller-owned files.

Earlier development-Mac endpoint validation with `adamId=1552791761` passed native account status,
enhanced HLS playlist validation, word/line TTML, WebPlayback playlist validation,
and native FairPlay persistent-key retrieval. The desktop start/stop/restart and
quit/relaunch flows were exercised; quitting releases the local server port.
The legacy Temari template ABI remains incompatible. Earlier offline decryption
checks on ALAC, AAC, and EC-3 segments decoded 175, 701, and 468 audio frames
respectively; these decoder-frame counts differ from framed sample-batch counts.
An ALAC batch containing 175 samples / 3,326,499 bytes remained
byte-identical across 100 repeated decryptions. Warm worker processing was about
35–43 MB/s in that earlier ALAC fixture check on this Mac; this excludes key
acquisition and network time and is not a current resource benchmark or general
throughput guarantee. These checks produced no
audio output. License input validation is covered; actual license issuance still
needs a real CDM challenge.

Cross-rendition validation also opens ALAC and AAC keys for the same catalog song,
then an EC-3 key, and verifies all three retained contexts again after switching.
Both preshare and song-specific keys are exercised. The account-free worker
fixture reproduces the library's per-asset cache; its format-switch regression
fails when the cache reset is omitted and passes with the reset enabled.

To record a new live endpoint report (including client and server times):

```sh
python3 native/macos/check_endpoints.py --adam-id 1552791761
```

This requires a running native server and a signed-in account. The checker refuses
another backend and writes metadata only to the ignored
`build/native-poc/endpoint-checks/native-http-report.json`. External playlist fetch
times are recorded separately from API response times. The initial enhanced-HLS
request observed in the desktop took 4–5 seconds with token acquisition; subsequent
requests were below one second. These are observations on the development Mac,
not fixed latency guarantees.

Native loader diagnostics remain available:

```sh
build/native-poc/wrapper-native-poc --assets build/native-poc/assets \
  --apple-profile --inspect libandroidappmusic.so
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
