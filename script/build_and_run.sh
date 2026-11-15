#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
native="$root/native/macos"
build="$root/build/native-poc"
profile="${WRAPPER_PROFILE_DIR:-$HOME/Library/Application Support/Native Music Wrapper}"
mode="${1:-run}"
[[ $# == 0 ]] || shift
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || { echo "Apple Silicon macOS and Xcode command-line tools are required." >&2; exit 2; }
case "$mode" in
  --build|--probes|--debug|debug|--login|login) target=runtime ;;
  run|app|--app|--app-build|--serve|serve|--verify|verify|--logs|logs|--telemetry|telemetry|--check-endpoints) target=all ;;
  *) echo "Usage: $0 [run|--app-build|--build|--serve|--login|--verify|--probes|--debug|--logs|--check-endpoints]" >&2; exit 2 ;;
esac
make -C "$native" -j 5 "$target"
"$build/native-asset-tool" "$native/assets" "$build/assets"
worker="$build/wrapper-native-poc"
case "$mode" in
  --build) ;;
  run|--app|app|--app-build)
    stage="$(mktemp -d "$build/.desktop.XXXXXXXX")"
    trap 'rm -rf "$stage"' EXIT
    app="$stage/Native Music Wrapper.app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/Runtime"
    cp "$native/standalone/Info.plist" "$app/Contents/Info.plist"
    mkdir -p "$app/Contents/Resources/Notices"
    cp "$native/third_party/json/LICENSE.MIT" "$app/Contents/Resources/Notices/nlohmann-json.txt"
    cp "$native/third_party/http/LICENSE" "$app/Contents/Resources/Notices/cpp-httplib.txt"
    cp "$worker" "$build/native-music-wrapper-server" "$build/NativeMusicWrapper" "$app/Contents/MacOS/"
    "$build/native-asset-tool" "$native/assets" "$app/Contents/Resources/Runtime/assets"
    /usr/bin/codesign --force --sign - "$app"
    /usr/bin/codesign --verify --strict "$app"
    dst="$build/Native Music Wrapper.app"
    if [[ -d "$dst" ]]; then mv "$dst" "$stage/previous.app"; fi
    if ! mv "$app" "$dst"; then [[ ! -d "$stage/previous.app" ]] || mv "$stage/previous.app" "$dst"; exit 1; fi
    if [[ "$mode" != --app-build ]]; then /usr/bin/open "$dst" --args --profile "$profile" "$@"; fi ;;
  --serve|serve) exec "$build/native-music-wrapper-server" --binary "$worker" --assets "$build/assets" --data-dir "$profile" "$@" ;;
  --login|login) exec "$worker" --assets "$build/assets" --apple-profile --probe auth-login --data-dir "$profile" --prompt-script @system-dialog ;;
  --debug|debug) exec xcrun lldb -- "$worker" --assets "$build/assets" --apple-profile --initialize libandroidappmusic.so ;;
  --logs|logs|--telemetry|telemetry) exec tail -f "$profile/http-timings.jsonl" ;;
  --verify|verify|--probes) make -C "$native" -j 4 verify ;;
  --check-endpoints) exec "$build/native-endpoint-check" "$@" ;;
esac
