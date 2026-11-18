#!/bin/bash
set -euo pipefail
if [[ $# != 2 || "$1" != --output ]]; then
    echo "Usage: $0 --output NEW_PACKAGE_DIRECTORY" >&2
    exit 2
fi
root="$(cd "$(dirname "$0")/.." && pwd)"
native="$root/native/macos"
mkdir -p "$(dirname "$2")"
dest="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
[[ ! -e "$dest" ]] || { echo "Output already exists." >&2; exit 2; }
work="$(mktemp -d -t native-wrapper-package)"
stage="$(mktemp -d "$(dirname "$dest")/.native-wrapper-package.XXXXXXXX")"
trap 'rm -rf "$work" "$stage"' EXIT
make -C "$native" -j 5 OUT="$work/build" runtime
kit="$stage/NativeMusicWrapperKit"
fw="$work/NativeMusicWrapperKit.framework"
version="$fw/Versions/A"
mkdir -p "$version/Headers" "$version/Modules" "$version/Resources" "$kit/Runtime" "$kit/Artifacts"
cp "$work/build/libNativeMusicWrapperKit.a" "$version/NativeMusicWrapperKit"
cp "$native/kit/include/NativeMusicWrapperKit.h" "$version/Headers/"
cp "$native/kit/include/module.modulemap" "$version/Modules/"
cp "$native/BinaryPackage/FrameworkInfo.plist" "$version/Resources/Info.plist"
ln -s A "$fw/Versions/Current"
for part in NativeMusicWrapperKit Headers Modules Resources; do ln -s "Versions/Current/$part" "$fw/$part"; done
xcodebuild -create-xcframework -framework "$fw" -output "$kit/Artifacts/NativeMusicWrapperKit.xcframework"
cp "$work/build/wrapper-native-poc" "$kit/Runtime/native-music-worker"
cp "$native/worker.entitlements" "$kit/Runtime/"
cp "$native/assets/README.md" "$kit/Runtime/ASSETS.md"
"$work/build/native-asset-tool" "$native/assets" "$kit/Runtime/assets" --list > "$work/libraries"
cp "$native/BinaryPackage/"{Package.swift,embed-runtime.sh,README.md} "$kit/"
cp "$root/LICENSE" "$kit/"
mkdir -p "$kit/Notices"
cp "$native/third_party/json/LICENSE.MIT" "$kit/Notices/nlohmann-json.txt"
printf '.build/\n.swiftpm/\n' > "$kit/.gitignore"
prefix='$(SRCROOT)/Vendor/NativeMusicWrapperKit/'
resources='$(TARGET_BUILD_DIR)/$(UNLOCALIZED_RESOURCES_FOLDER_PATH)/NativeMusicWrapper/assets'
{
    for file in embed-runtime.sh Runtime/worker.entitlements Runtime/native-music-worker Runtime/assets; do printf '%s%s\n' "$prefix" "$file"; done
    while IFS= read -r lib; do printf '%sRuntime/assets/%s\n' "$prefix" "$lib"; done < "$work/libraries"
    printf '%sRuntime/assets/manifest.json\n' "$prefix"
} > "$kit/inputs.xcfilelist"
{
    printf '%s\n' '$(TARGET_BUILD_DIR)/$(EXECUTABLE_FOLDER_PATH)/native-music-worker' "$resources"
    while IFS= read -r lib; do printf '%s/%s\n' "$resources" "$lib"; done < "$work/libraries"
    printf '%s/manifest.json\n' "$resources"
} > "$kit/outputs.xcfilelist"
mv "$kit" "$dest"
echo "Built $dest"
