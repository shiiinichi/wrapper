# Native Music Wrapper for macOS

A native Apple Silicon desktop app and local HTTP API for Apple Music. The app
executes the Android ARM64 libraries directly in a macOS process.

## Requirements

- Apple Silicon Mac running macOS 15 or later.
- Xcode command-line tools, including Swift and Clang.
- Python 3. The 12 required ARM64 libraries are included in `native/macos/assets/`.
- Current macOS security updates. zlib, XML and ICU use system libraries instead
  of the obsolete Android copies.

QEMU, Docker, a Linux VM, an Android NDK, and a rooted Android device are not
required. No additional runtime packages need to be installed.

## Build and run

```sh
./script/build_and_run.sh
```

This builds and opens `build/native-poc/Native Music Wrapper.app`. The desktop
app provides sign-in, server start/stop controls, a track-ID endpoint tester,
and response-time measurements. Its local API defaults to
`http://127.0.0.1:12341`.

For a build without launching the app:

```sh
./script/build_and_run.sh --app-build
```

To run only the native HTTP server:

```sh
./script/build_and_run.sh --serve --port 12341
```

The app and standalone server use the same native backend and account profile.
Use one server per port. Account data stays outside the app bundle and Git.

## Verify

```sh
./script/build_and_run.sh --verify
python3 native/macos/check_endpoints.py --adam-id 1552791761
```

The live endpoint checker requires a running native server and a signed-in
account. It saves response metadata and timings under ignored
`build/native-poc/endpoint-checks/`. The desktop also records server timings in
the private account profile.

## HTTP API

Responses preserve the original JSON envelope:

```json
{"code":0,"msg":"SUCCESS","data":{}}
```

| Endpoint | Method | Parameters |
| --- | --- | --- |
| `/status` | GET | Saved-account status, storefront regions, backend identity. |
| `/m3u8` | GET | `adamId`. |
| `/key` | GET | `adamId`, `uri` from the enhanced HLS manifest. |
| `/decrypt` | POST | Framed binary audio samples and the `X-Native-Context` handle from `/key`. |
| `/lyrics` | GET | `adamId`; optional `language`, `syllable`, `script`. |
| `/webplayback` | GET | `adamId`. |
| `/license` | POST | JSON `adamId`, `challenge`, `uri`; optional `drm-type` (`wv` or `pr`). |

The native key response supplies a versioned, opaque sample-decryption handle.
Compatible clients send sample batches to `/decrypt`; a persistent ARM64 worker
decrypts them without x86 snapshots. Legacy Temari templates remain unsupported.
Actual DRM license issuance needs a player/CDM-generated challenge.

See the [native implementation guide](native/macos/README.md) for authentication,
token lifecycle, compatibility boundaries, and detailed validation evidence.

## Optional CMake entrypoint

On macOS, CMake selects the native build and delegates to the same build script:

```sh
cmake -S . -B build/macos
cmake --build build/macos --target wrapper-native
```

CMake is optional. The commands above produce the same desktop app under
`build/native-poc/`.

## Project history

This project derives from the original wrapper-lite project.
This checkout contains the native macOS implementation, its build tools, and
regression checks. The former Linux/Docker/QEMU code, launchers, workflows, and
working-tree root filesystem have been removed; Git history retains them.
The build validates and copies the 12 required ARM64 libraries directly from
`native/macos/assets/` on this branch. No other branch, Git history, or `rootfs/`
directory is needed; an ordinary source archive can build the native app.
The native GitHub workflow is disabled and retained as
`.github/workflows/build-macos.yml.disabled`; local builds and verification remain
available through `script/build_and_run.sh`.

## License

This project is licensed under the [MIT License](LICENSE).
