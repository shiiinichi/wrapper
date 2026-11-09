#!/usr/bin/env python3
"""Extract only immutable, tracked ARM64 libraries; never copy account data."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import struct
import subprocess

ARM64_COMMIT = "c0a456f578212ef77d53aa2b5ad8cfdfe25a63c2"
PREFIX = "rootfs/system/lib64/"
ROOT = Path(__file__).resolve().parents[2]


def prepare(destination: Path) -> dict:
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "cat-file", "-e", ARM64_COMMIT + "^{commit}"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    listing = subprocess.run(["git", "ls-tree", "-r", ARM64_COMMIT, "--", PREFIX], cwd=ROOT,
                             check=True, capture_output=True, text=True).stdout
    files = []
    for entry in listing.splitlines():
        metadata, name = entry.split("\t", 1)
        mode, kind, object_id = metadata.split()
        relative = PurePosixPath(name)
        if (kind != "blob" or mode not in {"100644", "100755"} or not name.startswith(PREFIX)
                or relative.parent.as_posix() != PREFIX.rstrip("/") or relative.suffix != ".so"):
            continue
        target = destination / relative.name
        # git show reads a pinned blob, never the dirty original checkout/rootfs.
        data = subprocess.run(["git", "cat-file", "blob", object_id], cwd=ROOT,
                              check=True, capture_output=True).stdout
        if (len(data) < 64 or data[:6] != b"\x7fELF\x02\x01"
                or struct.unpack_from("<H", data, 18)[0] != 183):
            raise ValueError(f"not a little-endian AArch64 ELF64: {name}")
        digest = hashlib.sha256(data).hexdigest()
        if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            temporary = target.with_suffix(".so.tmp")
            temporary.write_bytes(data)
            os.replace(temporary, target)
        files.append({"library": relative.name, "git_blob": object_id, "sha256": digest, "bytes": len(data)})
    if not files:
        raise RuntimeError("the pinned ARM64 commit contains no libraries")
    manifest = {"source_commit": ARM64_COMMIT, "source_path": PREFIX, "files": files}
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "build/native-poc/assets")
    args = parser.parse_args()
    manifest = prepare(args.destination)
    print(f"ARM64 assets: {len(manifest['files'])} libraries from {ARM64_COMMIT[:12]}")
