#!/usr/bin/env python3
"""HTTP boundary, native-only execution, and localization regressions."""
import contextlib
import io
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import shutil
import threading
import unittest
from unittest.mock import patch
import urllib.request
import xml.etree.ElementTree as ET

from http_server import NativeBackend, NativeHTTPServer
import http_server
from web_api import ApiError, line_lyrics
from package_desktop import stage


class MemoryBackend:
    def __init__(self):
        self.calls = []
        self.timings = []

    def record_timing(self, route, method, code, milliseconds):
        self.timings.append({"endpoint": route, "method": method, "code": code, "server_ms": milliseconds})

    def execute(self, route, query, body):
        self.calls.append((route, query, body))
        return {"regions": ["ph"], "account_present": True}

    def decrypt(self, token, data):
        self.calls.append(("/decrypt", token, data))
        return data


class HttpBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.backend = MemoryBackend()
        self.server = NativeHTTPServer(("127.0.0.1", 0), self.backend, "test-instance")
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:" + str(self.server.server_port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, path, body=None, headers=None):
        request = urllib.request.Request(self.base + path, data=body, headers=headers or {})
        with urllib.request.urlopen(request, timeout=3) as response:
            return response, json.load(response)

    def test_status_identifies_native_backend_and_server_instance(self):
        response, value = self.request("/status")
        self.assertEqual(response.status, 200)
        self.assertEqual(response.headers["X-Wrapper-Backend"], "native-macos-arm64")
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        self.assertEqual(value["code"], 0)
        self.assertEqual(value["data"]["server_instance"], "test-instance")
        self.assertTrue(response.headers["Server-Timing"].startswith("app;dur="))
        self.assertGreaterEqual(self.backend.timings[0]["server_ms"], 0)

    def test_timing_records_route_and_result_without_sensitive_query(self):
        self.request("/key?adamId=1&uri=fixture-secret-key-uri")
        self.assertEqual(self.backend.timings[0]["endpoint"], "/key")
        self.assertNotIn("fixture-secret", json.dumps(self.backend.timings))

    def test_unknown_routes_and_methods_do_not_invoke_native_code(self):
        for path, body, code in [("/tokens", None, 404), ("/status", b"{}", 405), ("/license", None, 405)]:
            with self.subTest(path=path):
                self.assertEqual(self.request(path, body)[1]["code"], code)
        self.assertEqual(self.backend.calls, [])

    def test_foreign_host_and_browser_origin_are_rejected(self):
        for headers in [{"Host": "example.invalid"}, {"Origin": "https://example.invalid"}]:
            self.assertEqual(self.request("/status", headers=headers)[1]["code"], 403)
        self.assertEqual(self.backend.calls, [])

    def test_ambiguous_query_and_invalid_json_are_rejected(self):
        for path, body in [("/m3u8?adamId=1&adamId=2", None), ("/license", b"{"), ("/license", b"[]")]:
            self.assertEqual(self.request(path, body)[1]["code"], 400)
        self.assertEqual(self.backend.calls, [])

    def test_oversize_body_is_rejected_before_reading_or_dispatch(self):
        self.assertEqual(self.request("/license", b"{}", {"Content-Length": str(1024 * 1024 + 1)})[1]["code"], 413)
        self.assertEqual(self.backend.calls, [])

    def test_binary_sample_route_preserves_bytes_and_disables_response_caching(self):
        body = b"\0\0\0\x01\0\0\0\x03abc"
        request = urllib.request.Request(self.base + "/decrypt", data=body,
                                         headers={"Content-Type": "application/octet-stream", "X-Native-Context": "fixture-handle"})
        with urllib.request.urlopen(request, timeout=3) as response:
            self.assertEqual(response.headers["Content-Type"], "application/octet-stream")
            self.assertEqual(response.headers["Cache-Control"], "no-store")
            self.assertEqual(response.read(), body)
        self.assertEqual(self.backend.calls, [("/decrypt", "fixture-handle", body)])
        self.assertEqual(self.backend.timings[0]["endpoint"], "/decrypt")
        self.assertNotIn("fixture-handle", json.dumps(self.backend.timings))

    def test_binary_route_rejects_wrong_method_type_and_oversize_body(self):
        self.assertEqual(self.request("/decrypt")[1]["code"], 405)
        self.assertEqual(self.request("/decrypt", b"{}")[1]["code"], 400)
        self.assertEqual(self.request("/decrypt", b"x", {"Content-Length": str(16*1024*1024+1)})[1]["code"], 413)
        self.assertEqual(self.backend.calls, [])


class ProfileStorageTests(unittest.TestCase):
    def test_default_account_profile_survives_removing_checkout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            checkout, home = root / "checkout", root / "home"
            checkout.mkdir()
            profile = home / "Library/Application Support/Native Music Wrapper"
            def launch():
                with patch.object(Path, "home", return_value=home), patch.object(http_server, "BUILD", checkout / "build/native-poc"), \
                     patch("http_server.sys.argv", ["http_server.py"]), patch("http_server.signal.signal"), \
                     patch("http_server.NativeHTTPServer") as server, contextlib.redirect_stdout(io.StringIO()):
                    server.return_value.serve_forever.side_effect = KeyboardInterrupt
                    self.assertEqual(http_server.main(), 0)
                    self.assertEqual(server.call_args.args[1].profile, profile.resolve())
            launch()
            identity = profile / "native-device-id"
            identity.write_text("fixture-identity")
            shutil.rmtree(checkout)
            launch()
            self.assertEqual(identity.read_text(), "fixture-identity")
            self.assertEqual(profile.stat().st_mode & 0o777, 0o700)
            self.assertFalse(checkout.exists())


class NativeDispatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.backend = NativeBackend(Path(self.directory.name) / "native-worker", self.directory.name, self.directory.name)

    def tearDown(self):
        self.backend.close()
        self.directory.cleanup()

    def test_invalid_track_and_key_inputs_cannot_launch_native_workers(self):
        with patch.object(self.backend, "call", side_effect=AssertionError("unexpected native call")):
            for route, parameters in [("/m3u8", {}), ("/m3u8", {"adamId": "abc"}),
                                      ("/m3u8", {"adamId": "0"}), ("/m3u8", {"adamId": str(2**63)}),
                                      ("/key", {"adamId": "1"}),
                                      ("/key", {"adamId": "1", "uri": "skd://itunes.apple.com/P000000000/s1/e1"})]:
                with self.subTest(route=route, parameters=parameters), self.assertRaises(ApiError) as error:
                    self.backend.execute(route, parameters, {})
                self.assertEqual(error.exception.code, 400)

    def test_license_validation_precedes_token_issuance(self):
        with patch.object(self.backend.web, "license", side_effect=AssertionError("unexpected license request")):
            for body in [{"adamId": "1"}, {"adamId": "1", "uri": "fixture", "challenge": "fixture", "drm-type": "unknown"}]:
                with self.assertRaises(ApiError) as error:
                    self.backend.execute("/license", {}, body)
                self.assertEqual(error.exception.code, 400)

    def test_child_failure_never_leaks_raw_response_or_stderr(self):
        binary = self.backend.binary
        binary.write_text("#!/usr/bin/python3\nimport sys\nprint('fixture-secret-on-stdout')\nprint('fixture-secret-on-stderr',file=sys.stderr)\nsys.exit(1)\n")
        binary.chmod(0o700)
        output = io.StringIO()
        with contextlib.redirect_stderr(output), self.assertRaises(ApiError) as error:
            self.backend.call("tokens")
        self.assertNotIn("fixture-secret", str(error.exception))
        self.assertNotIn("fixture-secret", output.getvalue())
        self.assertNotIn("docker", output.getvalue())

    def test_timeout_kills_and_reaps_native_child(self):
        with patch("http_server.subprocess.Popen") as spawn:
            child = spawn.return_value
            child.communicate.side_effect = [subprocess.TimeoutExpired("fixture", 65), (b"", b"")]
            with self.assertRaises(ApiError) as error:
                self.backend.call("m3u8", "1")
            self.assertEqual(error.exception.code, 504)
            child.kill.assert_called_once()
            self.assertEqual(child.communicate.call_count, 2)
            self.assertIsNone(self.backend.active)

    def test_token_cache_is_memory_only_and_reused(self):
        with patch.object(self.backend, "call", return_value={"music_token": "fixture-secret", "developer_token": "fixture"}) as call:
            self.assertEqual(self.backend.tokens(), self.backend.tokens())
            call.assert_called_once_with("tokens")
        self.assertEqual(list(Path(self.directory.name).iterdir()), [])

    def test_timing_file_is_private_and_contains_metadata_only(self):
        self.backend.record_timing("/m3u8", "GET", 0, 12.3456)
        path = Path(self.directory.name) / "http-timings.jsonl"
        value = json.loads(path.read_text())
        self.assertEqual(value["server_ms"], 12.346)
        self.assertEqual(value["code"], 0)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(set(value), {"time_utc", "endpoint", "method", "code", "server_ms", "backend"})


class LyricsTests(unittest.TestCase):
    def test_line_conversion_preserves_translations_and_romanization(self):
        value = '<tt xmlns="http://www.w3.org/ns/ttml" xmlns:itunes="http://music.apple.com/lyric-ttml-internal" itunes:timing="Word"><body><p itunes:key="L1"><span>one</span> <span>two</span></p><translations><text for="L1"><span>uno</span>  <span>dos</span></text></translations><transliterations><text for="L1"><span>ichi</span>  <span>ni</span></text></transliterations></body></tt>'
        result = line_lyrics(value)
        ET.fromstring(result)
        self.assertIn('itunes:timing="Line"', result)
        self.assertIn('itunes:key="L1">one two</p>', result)
        self.assertIn('<text for="L1">uno dos</text>', result)
        self.assertIn('<text for="L1">ichi ni</text>', result)
        self.assertNotIn("<span", result)


class PackagingTests(unittest.TestCase):
    def test_only_pinned_library_blobs_are_bundled(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            (assets / "fixture.so").write_bytes(b"fixture-library")
            (assets / "MUSIC_TOKEN").write_text("fixture-secret")
            manifest = {"files": [{"library": "fixture.so", "sha256": hashlib.sha256(b"fixture-library").hexdigest()}]}
            (assets / "manifest.json").write_text(json.dumps(manifest))
            binary = root / "worker"
            binary.write_bytes(b"fixture-worker")
            bundle = root / "Fixture.app"
            stage(bundle, binary, assets)
            packaged = bundle / "Contents/Resources/Runtime/assets"
            self.assertEqual({p.name for p in packaged.iterdir()}, {"fixture.so", "manifest.json"})
            self.assertFalse(any(p.name == "MUSIC_TOKEN" for p in bundle.rglob("*")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
