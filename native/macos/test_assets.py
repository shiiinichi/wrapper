#!/usr/bin/env python3
"""Native assets stage from an ordinary source copy with no Git metadata."""
from pathlib import Path
import hashlib
import json
import struct
import tempfile
import unittest

import prepare_assets


class NativeAssetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.checkout = Path(self.temp.name) / "plain checkout"
        self.source = self.checkout / "native/macos/assets"
        self.source.mkdir(parents=True)
        self.destination = Path(self.temp.name) / "build assets"
        self.images = {}
        self.manifest = {"source_commit": "0" * 40, "source_path": "native/macos/assets", "files": []}
        for name in sorted(prepare_assets.NATIVE_LIBRARIES):
            image = bytearray(64)
            image[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", image, 18, 183)
            image.extend(name.encode())
            self.images[name] = bytes(image)
            (self.source / name).write_bytes(image)
            self.manifest["files"].append(self.entry(name, image))
        self.write_manifest()

    @staticmethod
    def entry(name, data):
        return {"library": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    def write_manifest(self):
        (self.source / "manifest.json").write_text(json.dumps(self.manifest))

    def prepare(self):
        return prepare_assets.prepare(self.destination, self.source)

    def old_unused_library(self, changed=False):
        self.destination.mkdir()
        original = b"previously generated unused library"
        (self.destination / "libEGL.so").write_bytes(b"caller-modified" if changed else original)
        previous = {"files": [self.entry("libEGL.so", original)]}
        (self.destination / "manifest.json").write_text(json.dumps(previous))

    def test_prepares_from_plain_checkout_without_git_or_rootfs(self):
        (self.source / "account.txt").write_text("fixture-private-data")
        self.assertFalse((self.checkout / ".git").exists())
        self.assertFalse((self.checkout / "rootfs").exists())
        manifest = self.prepare()
        self.assertEqual({entry["library"] for entry in manifest["files"]}, prepare_assets.NATIVE_LIBRARIES)
        self.assertEqual({file.name for file in self.destination.iterdir()},
                         prepare_assets.NATIVE_LIBRARIES | {"manifest.json"})
        for entry in manifest["files"]:
            self.assertEqual((self.destination / entry["library"]).read_bytes(), self.images[entry["library"]])

    def test_prunes_only_unchanged_owned_libraries_and_preserves_unrelated_files(self):
        self.old_unused_library()
        unrelated = self.destination / "notes.txt"
        unrelated.write_text("caller-owned")
        self.prepare()
        self.assertFalse((self.destination / "libEGL.so").exists())
        self.assertEqual(unrelated.read_text(), "caller-owned")

    def test_preserves_local_modification_of_a_previously_generated_library(self):
        self.old_unused_library(changed=True)
        self.prepare()
        self.assertEqual((self.destination / "libEGL.so").read_bytes(), b"caller-modified")

    def test_missing_required_library_does_not_replace_existing_manifest(self):
        self.prepare()
        original = (self.destination / "manifest.json").read_bytes()
        (self.source / sorted(self.images)[0]).unlink()
        with self.assertRaisesRegex(RuntimeError, "missing native libraries"):
            self.prepare()
        self.assertEqual((self.destination / "manifest.json").read_bytes(), original)

    def test_corrupt_source_is_rejected_by_checksum(self):
        name = sorted(self.images)[0]
        (self.source / name).write_bytes(self.images[name] + b"changed")
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.prepare()
        self.assertFalse((self.destination / "manifest.json").exists())

    def test_x86_library_is_rejected_even_with_matching_checksum(self):
        name = sorted(self.images)[0]
        image = bytearray(self.images[name])
        struct.pack_into("<H", image, 18, 62)
        (self.source / name).write_bytes(image)
        self.manifest["files"][0] = self.entry(name, image)
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "AArch64 ELF64"):
            self.prepare()

    def test_retired_dependencies_cannot_be_reintroduced_via_the_manifest(self):
        self.manifest["files"].append(self.entry("libz.so", b"obsolete"))
        (self.source / "libz.so").write_bytes(b"obsolete")
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "unused libraries"):
            self.prepare()

    def test_previously_generated_obsolete_dependencies_are_removed(self):
        self.destination.mkdir()
        names = {"libz.so", "libxml2.so", "libicuuc_sv_apple.so", "libicui18n_sv_apple.so", "libicudata_sv_apple.so"}
        previous = {"files": []}
        for name in names:
            data = ("obsolete-" + name).encode()
            (self.destination / name).write_bytes(data)
            previous["files"].append(self.entry(name, data))
        (self.destination / "manifest.json").write_text(json.dumps(previous))
        self.prepare()
        self.assertFalse(names & {path.name for path in self.destination.iterdir()})


if __name__ == "__main__":
    unittest.main(verbosity=2)
