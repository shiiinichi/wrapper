# Legacy wrapper-lite reference

These are the original Linux-wrapper instructions. The current macOS
app builds and runs natively; see [the project README](../README.md).
This legacy setup is not required by the macOS app.

A lightweight single-port HTTP wrapper for Apple Music decryption.

It provides five endpoints on one HTTP port: M3U8, Key, Lyrics, License, and WebPlayback.

## Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Debug build:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

Build outputs:

- `rootfs/system/bin/lite` — Android-layer binary
- `wrapper-lite` — privileged host launcher
- `wrapper-lite-rootless` — rootless host launcher (user namespaces)

## Run on Linux

```bash
# Login, cache tokens, then exit
./wrapper-lite-rootless --login user:pass --code-from-file --base-dir /data

# Start the HTTP service
./wrapper-lite-rootless --base-dir /data --host 0.0.0.0 --port 12340
```

2FA code:

- Interactive prompt when a TTY is available.
- With `--code-from-file`, the code is read from `data/2fa.txt`.

## Docker

```bash
docker build -t wrapper-lite:local .
docker run --privileged -p 12340:12340 \
  -v ./rootfs/data:/app/rootfs/data \
  -e USERNAME=... -e PASSWORD=... \
  wrapper-lite:local
```

### Prebuilt image (GHCR)

Every push to `lite` publishes the image to GitHub Container Registry:

```bash
docker pull ghcr.io/worldobservationlog/wrapper-lite:latest
docker run --privileged -p 12340:12340 \
  -v ./rootfs/data:/app/rootfs/data \
  -e USERNAME=... -e PASSWORD=... \
  ghcr.io/worldobservationlog/wrapper-lite:latest
```

Tags: `latest` and `lite` follow the `lite` branch, and `sha-<commit>` pins
one build. The image is `linux/amd64` only, because the bundled Android
libraries are x86_64.

The package is private when first published; set its visibility to public in
the package settings if anonymous pulls should work.

## HTTP API

All responses use:

```json
{"code":0,"msg":"SUCCESS","data":{...}}
```

| Endpoint | Method | Parameters |
|----------|--------|------------|
| `/m3u8` | GET | `adamId` |
| `/key` | GET | `adamId`, `uri` (required; the prefetch `skd://itunes.apple.com/P000000000/s1/e1` is rejected unless `adamId=0`) |
| `/lyrics` | GET | `adamId`, optional `language`, optional `syllable` (`1`=word-timed syllable-lyrics with translations/romanization, `0`=line-timed lyrics converted in-wrapper **and still carrying translations/romanization as whole-line text**), optional `script` (transliteration script, default `en-Latn`) |
| `/webplayback` | GET | `adamId` |
| `/license` | POST | JSON: `adamId`, `challenge`, `uri`, optional `drm-type` (`wv`=Widevine (default), `pr`=PlayReady) |
| `/status` | GET | returns `regions` (list of storefront codes this wrapper can serve) |

## lite arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--login user:pass` | — | login, cache tokens, then exit |
| `--code-from-file` | off | read 2FA code from file |
| `--host` | `127.0.0.1` | listen address |
| `--port` | `12340` | listen port |
| `--base-dir` | `data` | data directory |
| `--dns` | auto | comma-separated resolvers for the Android layer; falls back to `/etc/resolv.conf`, then the built-in `8.8.8.8,8.8.4.4` |
| `--ca-bundle` | `/system/etc/security/cacert.pem` | CA bundle used while TLS verification is on |
| `--tls` | `auto` | TLS version for the API requests and the Apple libraries: `auto` tries TLS 1.3 first and retries with the default handshake if 1.3 is refused, `1.3` forces 1.3, `default` keeps libcurl's choice |
| `--device-info` | auto | override the entire device-info string (see [Device info](#device-info)) |
| `--proxy` | — | proxy URL |
| `--debug` | off | debug logging; disables TLS verification |
| `--log-level` | `info` | `debug`, `info`, `warn`, `error` |
| `--log-file` | — | log file path |
| `--token-refresh-interval` | `1800` | background refresh interval in seconds |
| `--cors` | off | send CORS headers (`Access-Control-Allow-Origin: *`) and answer `OPTIONS` preflights so browser pages on any origin can call the API; the API has no authentication, so only enable it on instances you trust to be reachable |

## Device info

`lite` identifies itself to Apple's servers with a device-info string of the
form:

```
ClientIdentifier/VersionIdentifier/PlatformIdentifier/ProductVersion/DeviceModel/BuildVersion/LocaleIdentifier/LanguageIdentifier/AndroidID
```

Example: `Music/5.0.2/Android/10/Pixel 8/7663314/en-US/en-US/<android-id>`.

The trailing `AndroidID` is derived automatically:

- During `--login`, it is the FNV-1a 64-bit hash of the account username and is
  persisted to `<base-dir>/ANDROID_ID`, so each account keeps a stable device
  identity across runs.
- In service mode (no username), the persisted `ANDROID_ID` is reused; if none
  exists, a built-in default is used.

Pass `--device-info <string>` to override the entire string explicitly.

## Build notes

- Release: TLS verification is on by default and uses the bundled Mozilla CA
  bundle (`rootfs/system/etc/security/cacert.pem`, installed as
  `/system/etc/security/cacert.pem`). Pass `--debug` to disable verification
  temporarily, or `--ca-bundle` to use a different bundle.
- Debug: TLS verification is disabled by default.
- DNS: the Android layer uses `--dns` / `LITE_DNS` when given, otherwise the
  host's `/etc/resolv.conf`; the built-in `8.8.8.8,8.8.4.4` is only a
  last-resort fallback.

## Security

- TLS verification and the resolver defaults are described under
  [Build notes](#build-notes).
- The HTTP API has no authentication. It listens on `127.0.0.1` by default;
  binding to `0.0.0.0` (as the Docker image does) exposes the logged-in
  account to anyone who can reach the port.

## License

This project is licensed under the [MIT License](../LICENSE).
