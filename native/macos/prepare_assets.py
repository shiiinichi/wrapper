#!/usr/bin/env python3
"""Validate and stage the ARM64 libraries included in the native source tree."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).resolve().parent / "assets"
NATIVE_LIBRARIES = frozenset({
    "libBlocksRuntime.so", "libCoreADI.so", "libCoreFP.so", "libCoreLSKD.so",
    "libOpenSLES.so", "libandroidappmusic.so", "libc++_shared.so", "libdaapkit.so",
    "libicudata_sv_apple.so", "libicui18n_sv_apple.so", "libicuuc_sv_apple.so",
    "libmedialibrarycore.so", "libmediaplatform.so", "libstoreservicescore.so",
    "libwilhelm.so", "libxml2.so", "libz.so",
})


def prepare(destination: Path, source: Path = SOURCE) -> dict:
    manifest = json.loads((source / "manifest.json").read_text())
    entries = {}
    for entry in manifest["files"]:
        name = entry["library"]
        if Path(name).name != name or not name.endswith(".so") or name in entries:
            raise ValueError("invalid or duplicate native library name")
        entries[name] = entry
    missing = NATIVE_LIBRARIES - entries.keys()
    if missing:
        raise RuntimeError("missing native libraries in manifest: " + ", ".join(sorted(missing)))
    if entries.keys() - NATIVE_LIBRARIES:
        raise ValueError("the native manifest contains unused libraries")
    missing_files = [name for name in entries if not (source / name).is_file()]
    if missing_files:
        raise RuntimeError("missing native libraries in source tree: " + ", ".join(sorted(missing_files)))
    destination.mkdir(parents=True, exist_ok=True)
    try:
        previous = json.loads((destination / "manifest.json").read_text())
    except (OSError, ValueError):
        previous = {}
    for name in sorted(NATIVE_LIBRARIES):
        entry = entries[name]
        target = destination / name
        data = (source / name).read_bytes()
        if (len(data) < 64 or data[:6] != b"\x7fELF\x02\x01"
                or struct.unpack_from("<H", data, 18)[0] != 183):
            raise ValueError(f"not a little-endian AArch64 ELF64: {name}")
        digest = hashlib.sha256(data).hexdigest()
        if len(data) != entry["bytes"] or digest != entry["sha256"]:
            raise ValueError(f"{name} does not match its vendored manifest")
        if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            temporary = target.with_suffix(".so.tmp")
            temporary.write_bytes(data)
            os.replace(temporary, target)
    # Remove only unchanged files owned by the previous generated manifest.
    # Unrelated or locally modified files in a caller-supplied directory survive.
    for entry in previous.get("files", []):
        name = entry.get("library", "")
        if Path(name).name != name or not name.endswith(".so") or name in NATIVE_LIBRARIES:
            continue
        old = destination / name
        if old.is_file() and not old.is_symlink() and hashlib.sha256(old.read_bytes()).hexdigest() == entry.get("sha256"):
            old.unlink()
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "build/native-poc/assets")
    args = parser.parse_args()
    manifest = prepare(args.destination)
    print(f"Vendored ARM64 assets: {len(manifest['files'])} verified libraries")
