#!/usr/bin/env python3
"""Build the self-contained Apple silicon binary Swift package for app distribution."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "native/macos"
MODULE = "NativeMusicWrapperKit"
TARGET = "arm64-apple-macos15.0"


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def build(package, work):
    artifacts = package / "Artifacts"
    runtime = package / "Runtime"
    modules = work / "Modules" / (MODULE + ".swiftmodule")
    for directory in (artifacts, runtime, modules):
        directory.mkdir(parents=True)
    run(sys.executable, NATIVE / "prepare_assets.py", "--destination", runtime / "assets")
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    run("xcrun", "clang++", "-target", TARGET, "-isysroot", sdk, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
        "-ffile-prefix-map=" + str(ROOT) + "=.", *sorted(NATIVE.glob("*.cpp")), NATIVE / "guest_call.S",
        "-lcurl", "-lsqlite3", "-framework", "CoreFoundation", "-o", runtime / "native-music-worker")
    shutil.copy2(NATIVE / "worker.entitlements", runtime)

    # Resources and the executable are installed separately so the host signs its helper.
    # The XCFramework is a static framework and requires no framework embedding phase.
    fw = work / (MODULE + ".framework")
    version = fw / "Versions/A"
    (version / "Resources").mkdir(parents=True)
    run("xcrun", "swiftc", "-swift-version", "5", "-target", TARGET, "-sdk", sdk,
        "-parse-as-library", "-O", "-enable-library-evolution", "-emit-library", "-static",
        "-module-name", MODULE, "-emit-module", "-emit-module-path", modules / "arm64-apple-macos.swiftmodule",
        "-emit-module-interface-path", modules / "arm64-apple-macos.swiftinterface",
        "-module-cache-path", work / "swift-cache", "-file-prefix-map", str(ROOT) + "=.",
        *sorted((NATIVE / MODULE).glob("*.swift")), "-o", version / MODULE)
    # Distribute textual interfaces (including test SPI), never compiler-specific binaries.
    module_dir = version / "Modules" / modules.name
    module_dir.mkdir(parents=True)
    for interface in modules.glob("*.swiftinterface"):
        shutil.copy2(interface, module_dir)
    with (version / "Resources/Info.plist").open("wb") as output:
        plistlib.dump({"CFBundleIdentifier": "io.github.shiiinichi.NativeMusicWrapperKit",
                       "CFBundleExecutable": MODULE, "CFBundleName": MODULE, "CFBundlePackageType": "FMWK",
                       "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
                       "CFBundleSupportedPlatforms": ["MacOSX"], "LSMinimumSystemVersion": "15.0"}, output)
    (fw / "Versions/Current").symlink_to("A")
    for name in (MODULE, "Modules", "Resources"):
        (fw / name).symlink_to("Versions/Current/" + name)
    run("xcodebuild", "-create-xcframework", "-framework", fw, "-output", artifacts / (MODULE + ".xcframework"))
    shutil.copy2(NATIVE / "BinaryPackage/Package.swift", package)
    shutil.copy2(NATIVE / "BinaryPackage/embed-runtime.sh", package)
    shutil.copy2(NATIVE / "BinaryPackage/README.md", package)
    (package / ".gitignore").write_text(".build/\n.swiftpm/\n")
    shutil.copy2(ROOT / "LICENSE", package)
    shutil.copy2(NATIVE / "assets/README.md", runtime / "ASSETS.md")
    assets = [entry["library"] for entry in json.loads((runtime / "assets/manifest.json").read_text())["files"]]
    assets.append("manifest.json")
    prefix = "$(SRCROOT)/Vendor/NativeMusicWrapperKit/"
    inputs = ["embed-runtime.sh", "Runtime/worker.entitlements", "Runtime/native-music-worker", "Runtime/assets"]
    (package / "inputs.xcfilelist").write_text("\n".join(prefix + path for path in inputs + ["Runtime/assets/" + n for n in assets]) + "\n")
    bundle_assets = "$(TARGET_BUILD_DIR)/$(UNLOCALIZED_RESOURCES_FOLDER_PATH)/NativeMusicWrapper/assets"
    outputs = ["$(TARGET_BUILD_DIR)/$(EXECUTABLE_FOLDER_PATH)/native-music-worker", bundle_assets]
    (package / "outputs.xcfilelist").write_text("\n".join(outputs + [bundle_assets + "/" + n for n in assets]) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new binary package directory")
    output = parser.parse_args().output.resolve()
    if output.exists():
        parser.error("output already exists; build into a new directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wrapper-package-", dir=output.parent) as tmp:
        package = Path(tmp) / MODULE
        build(package, Path(tmp) / "compile")
        os.rename(package, output)
    print(output)


if __name__ == "__main__":
    main()
