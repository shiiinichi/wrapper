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
  run|--verify|verify|--debug|debug|--logs|logs|--telemetry|telemetry) ;;
  *) echo "usage: $0 [run|--verify|--debug|--logs|--telemetry]" >&2; exit 2 ;;
esac

# The executable exists only as isolated probes. Stop an earlier invocation of
# this uniquely named tool, never the music app or the Linux wrapper service.
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
  -o "$APP_BINARY"

case "$MODE" in
  --debug|debug)
    exec xcrun lldb -- "$APP_BINARY" --assets "$BUILD_DIR/assets" --apple-profile --initialize libandroidappmusic.so
    ;;
  --verify|verify)
    python3 "$ROOT_DIR/native/macos/test_loader.py" --binary "$APP_BINARY" --assets "$BUILD_DIR/assets"
    ;;
esac
# Logs and telemetry are the bounded probe's JSON report and stderr. There is
# no background daemon to leave running or a GUI window to verify.
exec python3 "$ROOT_DIR/native/macos/run.py" --binary "$APP_BINARY" --assets "$BUILD_DIR/assets"
