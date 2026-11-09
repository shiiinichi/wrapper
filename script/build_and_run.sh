#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/native-poc"
MODE="${1:-run}"
APP_BINARY="$BUILD_DIR/wrapper-native-poc"

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo "This proof of concept requires native arm64 macOS and Xcode command-line tools." >&2
  exit 2
fi
case "$MODE" in
  run|--app|app|--app-build|--probes|--build|--serve|serve|--verify|verify|--debug|debug|--logs|logs|--telemetry|telemetry|--login|login) ;;
  *) echo "usage: $0 [run|--app-build|--probes|--build|--serve|--verify|--debug|--logs|--telemetry|--login]" >&2; exit 2 ;;
esac

# Stop this project's desktop app and isolated workers before rebuilding.
# The desktop-owned server watches its parent and exits when the app stops.
pkill -x NativeMusicWrapper >/dev/null 2>&1 || true
pkill -x wrapper-native-poc >/dev/null 2>&1 || true
mkdir -p "$BUILD_DIR"
python3 "$ROOT_DIR/native/macos/prepare_assets.py" --destination "$BUILD_DIR/assets"
xcrun clang++ -arch arm64 -std=c++17 -O1 -g -Wall -Wextra -Werror \
  "$ROOT_DIR/native/macos/main.cpp" \
  "$ROOT_DIR/native/macos/elf_loader.cpp" \
  "$ROOT_DIR/native/macos/shims.cpp" \
  "$ROOT_DIR/native/macos/jni_host.cpp" \
  "$ROOT_DIR/native/macos/abi_runtime.cpp" \
  "$ROOT_DIR/native/macos/abi_selftest.cpp" \
  "$ROOT_DIR/native/macos/services.cpp" \
  "$ROOT_DIR/native/macos/authentication.cpp" \
  "$ROOT_DIR/native/macos/auth_input.cpp" \
  "$ROOT_DIR/native/macos/native_api.cpp" \
  "$ROOT_DIR/native/macos/format_bridge.cpp" \
  "$ROOT_DIR/native/macos/posix_bridge.cpp" \
  "$ROOT_DIR/native/macos/network_bridge.cpp" \
  "$ROOT_DIR/native/macos/sqlite_bridge.cpp" \
  "$ROOT_DIR/native/macos/guest_call.S" \
  -lcurl -lsqlite3 -framework CoreFoundation -o "$APP_BINARY"

case "$MODE" in
  --build) exit 0 ;;
  run|--app|app|--app-build)
    APP_BUNDLE="$BUILD_DIR/Native Music Wrapper.app"
    python3 "$ROOT_DIR/native/macos/package_desktop.py" --destination "$APP_BUNDLE" --binary "$APP_BINARY" --assets "$BUILD_DIR/assets"
    DESKTOP_SOURCES=("$ROOT_DIR"/native/macos/Desktop/{App,Views,Models,Services,ViewModels}/*.swift)
    xcrun swiftc -swift-version 5 -target arm64-apple-macos15.0 -module-cache-path "$BUILD_DIR/swift-module-cache" -O -parse-as-library \
      "${DESKTOP_SOURCES[@]}" -o "$APP_BUNDLE/Contents/MacOS/NativeMusicWrapper"
    /usr/bin/codesign --force --sign - "$APP_BUNDLE"
    if [[ "$MODE" != --app-build ]]; then
      /usr/bin/open -n "$APP_BUNDLE" --args --profile "$BUILD_DIR/auth-profile"
    fi
    exit 0
    ;;
  --serve|serve)
    shift
    exec python3 "$ROOT_DIR/native/macos/http_server.py" --binary "$APP_BINARY" --assets "$BUILD_DIR/assets" "$@"
    ;;
  --login|login)
    exec python3 "$ROOT_DIR/native/macos/sign_in.py" --binary "$APP_BINARY" --assets "$BUILD_DIR/assets"
    ;;
  --debug|debug)
    exec xcrun lldb -- "$APP_BINARY" --assets "$BUILD_DIR/assets" --apple-profile --initialize libandroidappmusic.so
    ;;
  --verify|verify)
    python3 "$ROOT_DIR/native/macos/test_loader.py" --binary "$APP_BINARY" --assets "$BUILD_DIR/assets"
    python3 "$ROOT_DIR/native/macos/test_http_server.py"
    ;;
esac
# Logs and telemetry are the bounded probe's JSON report and stderr. There is
# no background daemon to leave running or a GUI window to verify.
exec python3 "$ROOT_DIR/native/macos/run.py" --binary "$APP_BINARY" --assets "$BUILD_DIR/assets"
