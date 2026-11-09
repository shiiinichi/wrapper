#!/usr/bin/env python3
"""Stage a self-contained development app, excluding every account profile."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil


def stage(destination, binary, assets):
    contents = destination / "Contents"
    runtime = contents / "Resources/Runtime"
    (contents / "MacOS").mkdir(parents=True, exist_ok=True)
    runtime.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, contents / "MacOS/wrapper-native-poc")
    source = Path(__file__).resolve().parent
    for name in ("http_server.py", "web_api.py", "sign_in.py", "sign_in.applescript"):
        shutil.copy2(source / name, runtime / name)
    # Package only the pinned public blobs, never arbitrary neighboring files.
    manifest = json.loads((assets / "manifest.json").read_text())
    target = runtime / "assets"
    if target.exists():
        shutil.rmtree(target)
    target.mkdir()
    for entry in manifest["files"]:
        name = entry["library"]
        if Path(name).name != name or not name.endswith(".so"):
            raise ValueError("invalid library name in asset manifest")
        source_file = assets / name
        if hashlib.sha256(source_file.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("asset does not match its pinned manifest")
        shutil.copy2(source_file, target / name)
    shutil.copy2(assets / "manifest.json", target / "manifest.json")
    info = {"CFBundleExecutable": "NativeMusicWrapper", "CFBundleIdentifier": "io.github.shiiinichi.native-music-wrapper",
            "CFBundleName": "Native Music Wrapper", "CFBundleDisplayName": "Native Music Wrapper",
            "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "0.1.0", "CFBundleVersion": "1",
            "NSPrincipalClass": "NSApplication", "LSMinimumSystemVersion": "15.0", "NSHighResolutionCapable": True,
            "NSAppTransportSecurity": {"NSAllowsLocalNetworking": True}}
    with (contents / "Info.plist").open("wb") as output:
        plistlib.dump(info, output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    stage(args.destination, args.binary, args.assets)
