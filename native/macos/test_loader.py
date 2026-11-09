#!/usr/bin/env python3
"""Exercise real Android code and reject malformed ELF/unsafe ABI calls."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class ElfFixture:
    def __init__(self, source: Path):
        self.data = bytearray(source.read_bytes())
        offset = struct.unpack_from("<Q", self.data, 32)[0]
        size, count = struct.unpack_from("<HH", self.data, 54)
        self.headers = [(offset + i * size, struct.unpack_from("<IIQQQQQQ", self.data, offset + i * size))
                        for i in range(count)]
        self.tags = {}
        for _, header in self.headers:
            if header[0] == 2:
                for entry in range(header[2], header[2] + header[5], 16):
                    tag, value = struct.unpack_from("<qQ", self.data, entry)
                    if not tag:
                        break
                    self.tags.setdefault(tag, []).append((entry, value))

    def offset(self, address: int) -> int:
        for _, header in self.headers:
            if header[0] == 1 and header[3] <= address < header[3] + header[5]:
                return header[2] + address - header[3]
        raise ValueError("virtual address is not file-backed")

    def symbol(self, name: str) -> int:
        strings = self.offset(self.tags[5][0][1])
        symbols = self.offset(self.tags[6][0][1])
        count = struct.unpack_from("<I", self.data, self.offset(self.tags[4][0][1]) + 4)[0]
        for index in range(count):
            offset = symbols + index * 24
            string = strings + struct.unpack_from("<I", self.data, offset)[0]
            end = self.data.index(0, string)
            if self.data[string:end].decode() == name:
                return offset
        raise ValueError("fixture has no symbol " + name)

    def rename(self, name: str, replacement: str):
        symbol = self.symbol(name)
        offset = self.offset(self.tags[5][0][1]) + struct.unpack_from("<I", self.data, symbol)[0]
        self.data[offset:offset + len(name) + 1] = replacement.encode().ljust(len(name) + 1, b"\0")


class NativeLoaderTests(unittest.TestCase):
    binary: Path
    assets: Path

    def call(self, mode: str, name: str, assets=None):
        return subprocess.run([str(self.binary), "--assets", str(assets or self.assets), mode, name],
                              capture_output=True, text=True, timeout=10)

    def result(self, process):
        self.assertTrue(process.stdout, process.stderr)
        return json.loads(process.stdout)

    def mutated(self, change, mode="--inspect", source="libz.so"):
        fixture = ElfFixture(self.assets / source)
        change(fixture)
        with tempfile.TemporaryDirectory(prefix="elf-fixture-") as temp:
            (Path(temp) / source).write_bytes(fixture.data)
            return self.call(mode, source, Path(temp))

    def test_android_zlib_executes_and_round_trips(self):
        process = self.call("--probe", "zlib")
        result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertEqual(result["result"]["crc32"], 0x414FA339)
        self.assertTrue(result["result"]["round_trip"])
        self.assertTrue(result["loader"]["execution_prepared"])
        self.assertTrue(result["loader"]["constructors_initialized"])
        # This real ELF has an inaccessible inter-segment gap. Cache flushing
        # must not touch that gap; doing so produces SIGBUS on Apple Silicon.
        self.assertEqual(result["loader"]["images"][0]["mixed_write_execute_pages"], 0)

    def test_android_blocks_calls_native_callbacks_and_retains(self):
        process = self.call("--probe", "blocks")
        result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertEqual(result["result"], {"invoke_result": 42, "copy_callbacks": 1,
                                          "dispose_callbacks": 1, "retain_release": True})

    def test_apple_4k_layout_is_reported_without_running_code(self):
        result = self.result(self.call("--inspect", "libCoreADI.so"))
        self.assertFalse(result["loader"]["execution_prepared"])
        self.assertFalse(result["loader"]["constructors_initialized"])
        self.assertFalse(result["loader"]["relocated_without_blockers"])
        self.assertTrue(any(issue["kind"] == "page_permissions" for issue in result["loader"]["issues"]))
        self.assertIn("__system_property_get", result["loader"]["images"][0]["guarded_symbols"])

    def test_apple_initialization_stops_before_mixed_page_execution(self):
        process = self.call("--initialize", "libCoreADI.so")
        result = self.result(process)
        self.assertEqual(process.returncode, 1)
        self.assertIn("blockers", result["error"])
        self.assertTrue(all(image["constructors_called"] == 0 for image in result["loader"]["images"]))

    def test_wrong_cpu_is_rejected(self):
        process = self.mutated(lambda f: struct.pack_into("<H", f.data, 18, 62))
        self.assertEqual(process.returncode, 1)
        self.assertIn("AArch64", self.result(process)["error"])

    def test_truncated_header_is_rejected(self):
        process = self.mutated(lambda f: f.data.__delitem__(slice(32, None)))
        self.assertEqual(process.returncode, 1)
        self.assertIn("size", self.result(process)["error"])

    def test_out_of_file_program_headers_are_rejected(self):
        process = self.mutated(lambda f: struct.pack_into("<Q", f.data, 32, len(f.data) + 1))
        self.assertEqual(process.returncode, 1)
        self.assertIn("program-header", self.result(process)["error"])

    def test_huge_virtual_span_is_rejected_before_mapping(self):
        def change(f):
            offset, _ = next(item for item in f.headers if item[1][0] == 1)
            struct.pack_into("<Q", f.data, offset + 40, 1 << 40)
        process = self.mutated(change)
        self.assertEqual(process.returncode, 1)
        self.assertIn("256 MiB", self.result(process)["error"])

    def test_writable_executable_segment_is_rejected(self):
        def change(f):
            offset, _ = next(item for item in f.headers if item[1][0] == 1)
            struct.pack_into("<I", f.data, offset + 4, 7)
        process = self.mutated(change)
        self.assertEqual(process.returncode, 1)
        self.assertIn("writable executable", self.result(process)["error"])

    def test_invalid_string_table_is_rejected(self):
        def change(f):
            struct.pack_into("<Q", f.data, f.tags[5][0][0] + 8, 0xFFFFFFFFFFFFF000)
        process = self.mutated(change)
        self.assertEqual(process.returncode, 1)
        self.assertIn("outside PT_LOAD", self.result(process)["error"])

    def test_dependency_cannot_escape_asset_directory(self):
        def change(f):
            offset = f.offset(f.tags[5][0][1]) + f.tags[1][0][1]
            f.data[offset:offset + 8] = b"../x.so\0"
        process = self.mutated(change)
        self.assertEqual(process.returncode, 1)
        self.assertIn("unsafe DT_NEEDED", self.result(process)["error"])

    def test_relocation_cannot_write_readonly_code(self):
        def change(f):
            struct.pack_into("<Q", f.data, f.offset(f.tags[7][0][1]), 0x100)
        process = self.mutated(change)
        result = self.result(process)
        self.assertFalse(result["loader"]["relocated_without_blockers"])
        self.assertTrue(any("not in a writable" in issue["detail"] for issue in result["loader"]["issues"]))

    def test_unknown_relocation_blocks_execution(self):
        def change(f):
            struct.pack_into("<Q", f.data, f.offset(f.tags[7][0][1]) + 8, 9999)
        result = self.result(self.mutated(change))
        self.assertFalse(result["loader"]["relocated_without_blockers"])
        self.assertTrue(any("9999" in issue["detail"] for issue in result["loader"]["issues"]))

    def test_strong_and_weak_missing_imports_are_distinct(self):
        def rename(f):
            f.rename("malloc", "nohost")
        strong = self.result(self.mutated(rename))
        self.assertIn("nohost", strong["loader"]["images"][0]["missing_symbols"])
        self.assertFalse(strong["loader"]["relocated_without_blockers"])
        def weak(f):
            offset = f.symbol("malloc")
            rename(f)
            f.data[offset + 4] = (2 << 4) | (f.data[offset + 4] & 15)
        result = self.result(self.mutated(weak))
        self.assertNotIn("nohost", result["loader"]["images"][0]["missing_symbols"])
        self.assertGreater(result["loader"]["images"][0]["weak_unresolved"], 0)

    def test_known_function_bridge_is_not_used_for_a_data_import(self):
        def change(f):
            offset = f.symbol("malloc")
            f.data[offset + 4] = (f.data[offset + 4] & 0xF0) | 1
        result = self.result(self.mutated(change))
        self.assertIn("malloc", result["loader"]["images"][0]["missing_symbols"])

    def test_guarded_call_exits_without_fabricating_success(self):
        # Redirect the Blocks runtime's allocator import to a known guard. The
        # initializer can still run, but copying a block must stop the child.
        fixture = ElfFixture(self.assets / "libBlocksRuntime.so")
        source = fixture.symbol("__assert2")
        target = fixture.symbol("malloc")
        fixture.data[target:target + 4] = fixture.data[source:source + 4]
        with tempfile.TemporaryDirectory(prefix="elf-guard-") as temp:
            (Path(temp) / "libBlocksRuntime.so").write_bytes(fixture.data)
            process = self.call("--probe", "blocks", Path(temp))
        self.assertEqual(process.returncode, 86)
        self.assertIn("guarded Android ABI call: __assert2", process.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    NativeLoaderTests.binary = args.binary.resolve()
    NativeLoaderTests.assets = args.assets.resolve()
    unittest.main(argv=[__file__], verbosity=2)
