#!/bin/bash
set -euo pipefail
runtime="$(cd "$(dirname "$0")" && pwd)/Runtime"
helper="$TARGET_BUILD_DIR/$EXECUTABLE_FOLDER_PATH/native-music-worker"
resources="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/NativeMusicWrapper/assets"
stage="$TARGET_TEMP_DIR/NativeMusicWrapperKit"
mkdir -p "$(dirname "$helper")" "$resources" "$stage"
# Xcode's script sandbox allows the declared filenames, including during clean builds.
# In-place copies avoid undeclared temporary sibling paths in the signed bundle.
/usr/bin/rsync -a --inplace --delete "$runtime/assets/" "$resources/"
/bin/cp "$runtime/native-music-worker" "$stage/native-music-worker"
if [[ "${CODE_SIGNING_ALLOWED:-YES}" == YES ]]; then
  : "${EXPANDED_CODE_SIGN_IDENTITY:?A signing identity is required for the embedded worker}"
  /usr/bin/codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" --timestamp=none \
    --identifier "$PRODUCT_BUNDLE_IDENTIFIER.native-worker" --entitlements "$runtime/worker.entitlements" "$stage/native-music-worker"
fi
/bin/cp "$stage/native-music-worker" "$helper"
