# Native Music Wrapper for macOS

A native Apple Silicon desktop app and local HTTP API for Apple Music. The app
executes the Android ARM64 libraries directly in a macOS process.

## Requirements

- Apple Silicon Mac running macOS 15 or later.
- Xcode command-line tools, including Swift and Clang.
- Python 3 and Git.
- This repository's full Git history, which supplies the pinned ARM64 libraries.

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
| `/lyrics` | GET | `adamId`; optional `language`, `syllable`, `script`. |
| `/webplayback` | GET | `adamId`. |
| `/license` | POST | JSON `adamId`, `challenge`, `uri`; optional `drm-type` (`wv` or `pr`). |

The native key response does not yet supply a Temari sample-decryption template.
The desktop marks that limitation explicitly. Actual DRM license issuance needs
a player/CDM-generated challenge; audio playback remains unverified.

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
The inherited Linux/emulated-wrapper instructions and workflow are archived under
[`legacy/`](legacy/README.md); they are not part of the macOS build or CI.

## License

This project is licensed under the [MIT License](LICENSE).
