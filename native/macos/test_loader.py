#!/usr/bin/env python3
"""Exercise real Android code and reject malformed ELF/unsafe ABI calls."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import sqlite3
import ssl
import threading
import plistlib
from http.server import HTTPServer, BaseHTTPRequestHandler
from test_fixtures import tls_fixture


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

    def call(self, mode: str, name: str, assets=None, apple_profile=False, data_directory=None):
        command = [str(self.binary), "--assets", str(assets or self.assets), mode, name]
        if apple_profile:
            command.append("--apple-profile")
        if data_directory:
            command.extend(["--data-dir", str(data_directory)])
        return subprocess.run(command,
                              capture_output=True, text=True, timeout=10)

    def test_real_apple_jni_entry_points_return_jni_16(self):
        for name in ["coreadi-jni", "lskd-jni"]:
            with self.subTest(library=name):
                process = self.call("--probe", name, apple_profile=True)
                result = self.result(process)
                self.assertEqual(process.returncode, 0, result)
                self.assertEqual(result["result"]["jni_version"], 0x10006)
                self.assertEqual(result["result"]["get_env_calls"], 0)
                self.assertFalse(result["loader"]["mixed_pages_readonly"])
                self.assertTrue(result["loader"]["relocated_without_blockers"])

    def test_real_storeservices_constructor_chain_completes(self):
        process = self.call("--initialize", "libstoreservicescore.so", apple_profile=True)
        result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        images = {image["library"]: image for image in result["loader"]["images"]}
        self.assertEqual(images["libstoreservicescore.so"]["constructors_called"], 64)
        self.assertEqual(images["libmediaplatform.so"]["constructors_called"], 7)
        self.assertGreater(images["libc++_shared.so"]["tls_read_rewrites"], 0)
        self.assertGreater(result["loader"]["guest_destructors_registered"], 60)
        self.assertIn("libCoreFoundation.so", result["loader"]["native_host_libraries"])

    def test_real_music_constructor_chain_completes_without_audio(self):
        process = self.call("--initialize", "libandroidappmusic.so", apple_profile=True)
        result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        images = {image["library"]: image for image in result["loader"]["images"]}
        self.assertEqual(images["libandroidappmusic.so"]["constructors_called"], 91)
        self.assertEqual(images["libmedialibrarycore.so"]["constructors_called"], 209)
        self.assertEqual(sum(image["constructors_called"] for image in images.values()), 372)
        self.assertFalse({"libz.so", "libxml2.so", "libicuuc_sv_apple.so", "libicui18n_sv_apple.so", "libicudata_sv_apple.so"} & images.keys())
        self.assertTrue(result["loader"]["constructors_initialized"])
        self.assertTrue(images["libOpenSLES.so"]["data_only"])
        self.assertTrue(images["libwilhelm.so"]["data_only"])
        self.assertEqual(images["libOpenSLES.so"]["constructors_called"], 0)
        self.assertFalse(any(image["missing_symbols"] for image in images.values()))
        guarded = {name for image in images.values() for name in image["guarded_symbols"]}
        self.assertNotIn("CFStringCreateWithFormat", guarded)
        self.assertNotIn("CFStringAppendFormat", guarded)
        self.assertNotIn("curl_easy_setopt", guarded)
        self.assertNotIn("curl_share_setopt", guarded)
        self.assertFalse(images["libCoreADI.so"]["guarded_symbols"])

    def test_tls_adaptation_preserves_machine_state_and_thread_isolation(self):
        with tempfile.TemporaryDirectory(prefix="elf-tls-abi-") as temp:
            tls_fixture(Path(temp))
            process = self.call("--probe", "tls-abi", Path(temp), apple_profile=True)
        result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertEqual(result["result"], {"state_preserved": True, "independent_threads": 3})
        self.assertEqual(result["loader"]["images"][0]["tls_read_rewrites"], 1)

    def test_corefoundation_formats_guest_registers_objects_positions_and_va_lists(self):
        with tempfile.TemporaryDirectory(prefix="native-cf-format-") as temp:
            tls_fixture(Path(temp))
            process = self.call("--probe", "corefoundation-abi", Path(temp), apple_profile=True)
            result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertTrue(all(result["result"].values()))

    def test_guest_pthread_and_futex_synchronize_real_threads(self):
        with tempfile.TemporaryDirectory(prefix="elf-runtime-abi-") as temp:
            (Path(temp) / "abi-read.txt").write_bytes(b"typed-android-file-bridge\n")
            process = self.call("--probe", "runtime-abi", Path(temp), apple_profile=True)
        result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertTrue(all(result["result"].values()))

    def test_service_abi_uses_android_registers_structures_and_write_scope(self):
        with tempfile.TemporaryDirectory(prefix="elf-service-abi-") as temp:
            tls_fixture(Path(temp))
            process = self.call("--probe", "service-abi", Path(temp), apple_profile=True)
            result = self.result(process)
            self.assertEqual(process.returncode, 0, result)
            self.assertTrue(all(result["result"].values()))
            self.assertEqual((Path(temp) / "writes/abi-file").read_bytes(), b"native-bootstrap-file")
            self.assertFalse((Path(temp) / "outside-write").exists())
            self.assertFalse((Path(temp) / "outside/forbidden").exists())

    def test_real_services_bootstrap_persists_valid_databases_and_can_reopen(self):
        with tempfile.TemporaryDirectory(prefix="native service data ") as temp:
            data = Path(temp) / "isolated profile"
            for _ in range(2):
                process = self.call("--probe", "services", apple_profile=True, data_directory=data)
                result = self.result(process)
                self.assertEqual(process.returncode, 0, result)
                for flag in ["device_configured", "request_context_initialized", "context_round_trip", "presentation_interface",
                             "fairplay_service", "foothill_controller", "lease_session", "process_owned_lifetime"]:
                    self.assertTrue(result["result"][flag], flag)
                self.assertFalse(result["result"]["authenticated"])
                self.assertFalse(result["result"]["lease_requested"])
                self.assertFalse(result["loader"]["mixed_pages_readonly"])
                self.assertTrue(result["loader"]["relocated_without_blockers"])
                self.assertFalse(any(image["mixed_write_execute_pages"] for image in result["loader"]["images"]))
                self.assertGreaterEqual(result["loader"]["native_sqlite_opened"], 4)
                shifted = {image["library"] for image in result["loader"]["images"] if image["load_bias_offset"]}
                self.assertEqual(shifted, {"libc++_shared.so", "libCoreFP.so", "libCoreADI.so", "libCoreLSKD.so"})
                self.assertIn("VALID REQUEST CONTEXT", process.stderr)
                self.assertNotIn("unimplemented Android", process.stderr)
            for name in ["accounts", "cookies", "httpcache", "kvs"]:
                with sqlite3.connect((data / (name + ".sqlitedb")).as_uri() + "?mode=ro", uri=True) as database:
                    self.assertEqual(database.execute("PRAGMA quick_check").fetchall(), [("ok",)])
                    self.assertTrue(database.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall())

    def test_services_require_an_explicit_data_directory(self):
        result = self.result(self.call("--probe", "services", apple_profile=True))
        self.assertFalse(result["ok"])
        self.assertIn("requires --data-dir", result["error"])

    def test_excessive_alignment_is_rejected_before_reserving_memory(self):
        def change(f):
            offset, _ = next(item for item in f.headers if item[1][0] == 1)
            struct.pack_into("<Q", f.data, offset + 48, 1 << 40)
        process = self.mutated(change)
        self.assertEqual(process.returncode, 1)
        self.assertIn("alignment exceeds limit", self.result(process)["error"])

    def test_real_authentication_flow_cancels_at_credentials_without_login(self):
        with tempfile.TemporaryDirectory(prefix="native-auth-preflight-") as temp:
            result = self.result(self.call("--probe", "auth-preflight", apple_profile=True, data_directory=temp))
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["result"]["credential_requests"], 1)
        self.assertTrue(result["result"]["cancelled"])
        self.assertFalse(result["result"]["authenticated"])
        self.assertEqual(result["result"]["verification_requests"], 0)

    def test_guest_credentials_keep_long_password_and_code_with_persistent_identity(self):
        with tempfile.TemporaryDirectory(prefix="native-auth-abi-") as temp:
            process = self.call("--probe", "auth-abi", apple_profile=True, data_directory=temp)
            result = self.result(process)
            identity = (Path(temp) / "native-device-id").read_text()
            self.assertRegex(identity, r"^[0-9a-f]{16}$")
            self.assertEqual((Path(temp) / "native-device-id").stat().st_mode & 0o777, 0o600)
        self.assertEqual(process.returncode, 0, result)
        self.assertTrue(result["result"]["long_password_2fa_round_trip"])
        self.assertTrue(result["result"]["profile_identity_persistent"])
        self.assertFalse(result["result"]["credentials_submitted"])

    def test_new_native_profile_files_are_owner_only(self):
        with tempfile.TemporaryDirectory(prefix="native-profile-permissions-") as temp:
            process = self.call("--api", "status", apple_profile=True, data_directory=temp)
            result = self.result(process)
            self.assertEqual(process.returncode, 0, result)
            self.assertTrue(result["ok"], result)
            self.assertTrue(list(Path(temp).glob("*.sqlitedb")))
            self.assertEqual(Path(temp).stat().st_mode & 0o777, 0o700)
            for path in Path(temp).iterdir():
                if path.is_file():
                    self.assertEqual(path.stat().st_mode & 0o777, 0o600, path.name)

    def test_login_prompt_cancel_returns_nonzero_without_success(self):
        with tempfile.TemporaryDirectory(prefix="native-auth-cancel-") as temp:
            script = Path(temp) / "cancel.applescript"
            script.write_text('error number -128\n')
            process = subprocess.run([str(self.binary), "--assets", str(self.assets), "--apple-profile", "--probe", "auth-login",
                                      "--data-dir", temp, "--prompt-script", str(script)], capture_output=True, text=True, timeout=20)
            result = self.result(process)
        self.assertNotEqual(process.returncode, 0, result)
        self.assertFalse(result["ok"])
        # Without network access, preflight fails before the prompt. With it,
        # the deliberately cancelled prompt must remain a failed login.
        if result.get("result"):
            self.assertTrue(result["result"]["cancelled"])
            self.assertFalse(result["result"]["authenticated"])

    def test_protocol_dialog_script_reads_private_unicode_payload_without_ui(self):
        script = Path(__file__).with_name("sign_in.applescript")
        payload = {"title": "Native dialog", "message": "Quotes: \" and Unicode: 音楽", "buttons": ["1. Continue", "2. Cancel"]}
        process = subprocess.run(["/usr/bin/osascript", str(script), "dialog", "parse-only"],
                                 input=plistlib.dumps(payload), capture_output=True, timeout=15)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(process.stdout.strip(), b"2")

    def test_curl_option_callback_and_info_adapters_transfer_exact_bytes(self):
        with tempfile.TemporaryDirectory(prefix="native-curl-abi-") as temp:
            (Path(temp) / "transfer.txt").write_text("native-curl-typed-transfer\n")
            process = self.call("--probe", "network-abi", Path(temp), apple_profile=True)
            result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertTrue(result["result"]["curl_typed_options"])
        self.assertTrue(result["result"]["curl_typed_info"])

    def test_curl_keeps_tls_verification_enabled_against_untrusted_server(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
        with tempfile.TemporaryDirectory(prefix="native-curl-tls-") as temp:
            root = Path(temp)
            subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-subj", "/CN=localhost", "-keyout", str(root / "key.pem"), "-out", str(root / "cert.pem")],
                           check=True, capture_output=True)
            server = HTTPServer(("127.0.0.1", 0), Handler)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(root / "cert.pem", root / "key.pem")
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
            thread.start()
            (root / "network-test-url").write_text(f"https://127.0.0.1:{server.server_port}/")
            try:
                process = self.call("--probe", "network-abi", root, apple_profile=True)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
            result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertTrue(result["result"]["untrusted_tls_rejected"])

    def test_dynamic_function_guards_stop_unimplemented_calls(self):
        fixture = ElfFixture(self.assets / "libBlocksRuntime.so")
        fixture.rename("malloc", "nohost")
        with tempfile.TemporaryDirectory(prefix="elf-import-guard-") as temp:
            (Path(temp) / "libBlocksRuntime.so").write_bytes(fixture.data)
            process = self.call("--probe", "blocks", Path(temp), apple_profile=True)
        self.assertEqual(process.returncode, 86)
        self.assertIn("unimplemented Android function: nohost", process.stderr)

    def test_incompatible_long_double_api_is_not_forwarded_to_darwin(self):
        # Android ARM64 long double is binary128; Darwin ARM64 uses binary64.
        # Redirect the allocator to this API to prove that calling it stops.
        fixture = ElfFixture(self.assets / "libBlocksRuntime.so")
        source, target = fixture.symbol("__assert2"), fixture.symbol("malloc")
        fixture.rename("__assert2", "strtold")
        fixture.data[target:target + 4] = fixture.data[source:source + 4]
        with tempfile.TemporaryDirectory(prefix="elf-long-double-") as temp:
            (Path(temp) / "libBlocksRuntime.so").write_bytes(fixture.data)
            process = self.call("--probe", "blocks", Path(temp), apple_profile=True)
        self.assertEqual(process.returncode, 86)
        self.assertIn("unimplemented Android function: strtold", process.stderr)

    def result(self, process):
        self.assertTrue(process.stdout, process.stderr)
        return json.loads(process.stdout)

    def mutated(self, change, mode="--inspect", source="libBlocksRuntime.so"):
        fixture = ElfFixture(self.assets / source)
        change(fixture)
        with tempfile.TemporaryDirectory(prefix="elf-fixture-") as temp:
            (Path(temp) / source).write_bytes(fixture.data)
            return self.call(mode, source, Path(temp))

    def test_system_zlib_executes_and_round_trips_without_a_bundled_copy(self):
        process = self.call("--probe", "zlib")
        result = self.result(process)
        self.assertEqual(process.returncode, 0, result)
        self.assertEqual(result["result"]["crc32"], 0x414FA339)
        self.assertTrue(result["result"]["round_trip"])
        self.assertTrue(result["loader"]["execution_prepared"])
        self.assertTrue(result["loader"]["constructors_initialized"])
        self.assertIn("libz.so", result["loader"]["native_host_libraries"])
        self.assertEqual(result["loader"]["images"], [])

    def test_security_regressions_use_patched_host_libraries_and_fresh_canaries(self):
        results = [self.result(self.call("--probe", "security-abi")) for _ in range(2)]
        for result in results:
            self.assertTrue(result["ok"], result)
            for flag in ("gzip_header_bounds", "xml_empty_dictionary", "stack_canary_matches_tls"):
                self.assertTrue(result["result"][flag])
            self.assertEqual(result["loader"]["images"], [])
        self.assertNotEqual(results[0]["result"]["stack_canary_fingerprint"], results[1]["result"]["stack_canary_fingerprint"])

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
