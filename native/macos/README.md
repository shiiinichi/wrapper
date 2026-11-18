# Native macOS wrapper

The desktop client, local HTTP service, embeddable library, sign-in transport and decryption worker are C++. The loader executes the 12 manifest-pinned Android ARM64 SDK libraries on Apple Silicon. The small assembly trampoline implements the Android calling convention.

The reference for playback behavior is the original `WorldObservationLog/wrapper` lite tree at `e51e370a6b0660ed9d722fff1add079098aeb935`. The ARM64 libraries originate from `c0a456f578212ef77d53aa2b5ad8cfdfe25a63c2`. All build inputs are in the checkout.

## Build and run

Use Xcode command-line tools on arm64 macOS. The default command builds and opens the native desktop app. The source build has a standalone local server and an embedded SDK worker. The SwiftPM distribution contains the C++ library and worker.

```sh
./script/build_and_run.sh --build
./script/build_and_run.sh --app-build
./script/build_and_run.sh
./script/build_and_run.sh --serve --port 12341
./script/build_and_run.sh --login
./script/build_and_run.sh --verify
./script/build_and_run.sh --check-endpoints --adam-id 1552791761
./script/package_library.sh --output build/NativeMusicWrapperKit
```

The desktop starts its own loopback server, checks its instance ID before reporting readiness, and shuts down its child on exit. The server and desktop expose status, sign-in, endpoint checks and response times. Configure account storage through `WRAPPER_PROFILE_DIR` or the desktop’s `--profile` option. The default uses the current user's Application Support location. The HTTP timings file contains route names, result codes and durations.

## Embedding and playback

[NativeMusicWrapperKit](BinaryPackage/README.md) exports the same dispatcher used by the standalone server. The host supplies an app-private account folder. The runtime starts and owns the signed worker over inherited pipes. Native decryption stays inside the worker. A separate admission gate serves public artist lookups. Account operations are serialized, and cancellation invalidates outstanding work.

Every login, saved profile and playback request uses the original Music 5.0.2, Android 10, Pixel 8 device client. Database location is independent of identity. The importer accepts stopped wrapper-lite and native profile folders, validates known files, and atomically replaces the destination only after staging a usable account. A session Apple rejects under this identity can be reauthorized by signing in. Standalone sign-in uses native macOS prompts. Embedders receive bounded prompt frames to display their own UI.

| Route | Result |
| --- | --- |
| GET `/status` | Native account presence and storefront regions. Embedded and HTTP clients also receive transport metadata. |
| GET `/profile` | Active account's cached Apple Music profile; optional `refresh=true` performs a bounded SDK refresh. |
| GET `/m3u8` | The signed-in SDK asset for `adamId`. An offline-eligible account tries its subscription asset first. A playback-only account tries its playback asset first. Only an empty result tries the other SDK method. The raw response preserves HLS or a direct protected file. |
| GET `/key` | Native FairPlay key information and a versioned opaque context for the song and key URI. |
| POST `/decrypt` | Length-prefixed samples decrypted in their original order. The SDK processes each aligned prefix and preserves its clear tail. |
| GET `/lyrics` | Word-timed TTML or line lyrics for the requested language and script. |
| GET `/assets` | Authenticated CBC HLS asset for a song. A playback request for `representation=hls` uses this only when the device selected a direct protected file. |
| GET `/webplayback` | Explicit diagnostic for browser playback assets. |
| POST `/license` | Widevine or PlayReady exchange using a real client-created challenge. |
| GET `/artist` | Public artist metadata, transparent logo and eligible wide artwork. |

The signed-in SDK supplies the manifest. Clients can choose a codec from the playlist and read the sample rate and bit depth of each ALAC rendition from its MP4 audio configuration. Streamed and saved samples use the same native decryption path.

The service bounds metadata, decompressed HTTP data, request admission and sample frames. Redirects stay on the original HTTPS origin. Credentials and signed URLs stay out of service logs. The server listens on loopback and rejects cross-origin browser requests. Signing in or importing discards cached account tokens and decryption handles.

## Apple Music user profile

HTTP `GET /profile` and embedded `nmw_send(runtime, operation, "/profile", "{}", ...)`
use the same dispatcher and authenticated account gate. The result contains only:

```json
{"code":0,"msg":"SUCCESS","data":{"profile_present":true,"name":"Fixture Name","handle":"fixture.handle","image":{"url":"https://is1-ssl.mzstatic.com/fixture/{w}x{h}bb.jpg","width":512,"height":512}}}
```

The embedded success message is `success`, as on the other embedded routes. A
signed-in account without a cached Apple Music social profile returns code `0`
with `profile_present: false` and `name`, `handle`, `image` all `null`. A profile
without a photo has `image: null`; invalid optional metadata also becomes `null`.
An account name is separate from a social profile name. A signed-out request
returns the existing code `401` envelope. `/status` confirms a saved account
record without validating its token with Apple; an expired session needs a fresh
sign-in before authenticated refresh can succeed. Consumers can keep their system
avatar when either the profile or image is absent.

The bridge retains the owning guest profile, reads borrowed getter results only
while that owner is alive, and matches its DSID against the current account
before exposing metadata. It rechecks the account before returning. No account
IDs, biographies, social IDs, credentials, tokens or arbitrary dictionary fields
are included. Profile state is read anew for each request, so account replacement
cannot reuse an old photo. The SDK's null image setter preserves an old image;
this route does not use that setter or maintain an additional photo cache.

Name and handle are valid UTF-8 without control characters, capped at 1024 and
256 bytes. Image metadata is limited to a URL of at most 4096 bytes and optional
integer dimensions in `1...16384`. URLs must use HTTPS on a subdomain of
`mzstatic.com`, with no credentials, query, fragment or port other than 443.
Supported template tokens are `{w}`, `{h}`, `{c}` and `{f}`. The wrapper returns
metadata without resolving the template or downloading image bytes. The complete worker
profile reply is capped at 16 KiB, and the dispatcher independently validates and
projects it onto this schema.

The default route performs no network refresh. Its warm path reads the active
account and the SDK's keyed profile cache and inspects three image keys; it does
not scan the music library, request account tokens or fetch artwork. It reuses
the existing worker, and cold calls pay the normal SDK startup cost. `/status`
continues to use only its existing status opcode.

For an empty or stale cache, callers can opt into `GET /profile?refresh=true`
(embedded query `{"refresh":"true"}`). This constructs the SDK's read-only
`UserProfileRequest` for the active account, requests an 8-second SDK timeout,
and rereads the account-matched cache on success. Both profile opcodes have a
12-second worker-exchange wall deadline, including URL-bag bootstrap and cold
startup; the existing admission queue can add up to 5 seconds. Cancellation or
deadline expiry closes the worker, and later requests restart through the
normal generation recovery path. SDK/network rejection remains a backend error
(HTTP envelope `502`); wall timeout is `504`. Neither is disguised as an absent
profile. `refresh=false` is equivalent to the default. Other query parameters,
request bodies and key contexts are rejected.

Deterministic verification covers real SDK fixture accounts, the native getter
and request-layout ABIs, the C dispatcher and HTTP route, optional/invalid
metadata, account switching, private-pipe cancellation and refresh wall timeout.
Fixtures contain no live credentials, and these checks make no live refresh
request. Separate live-account verification on 2026-11-18 passed for cached reads
and explicit SDK refresh: the endpoint returned the account-matched profile and
validated photo metadata, and a header-only request to the resolved photo URL
returned HTTP `200` with an image content type. The service's account-specific
no-profile response remains unverified; a cached miss alone does not establish
that the account has no Apple Music profile.

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

## Verification

`./script/build_and_run.sh --verify` compiles and runs the C++ contract suite in isolated temporary directories. It exercises the device lookup order, native contexts and sample boundaries, HTTP API, Apple response handling, reusable private processes, TLS and system-library bridges, account files, ELF validation, and checksummed asset staging. Release qualification with a signed-in account should compare the selected audio headers and decoded source frames for known recordings. No test starts music playback.
