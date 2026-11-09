#!/usr/bin/env python3
"""Local macOS HTTP API backed exclusively by native ARM64 library workers."""
import argparse
import contextlib
import datetime
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid

from web_api import ApiError, AppleWebApi
from native_decryption import MAX_FRAME, NativeDecryption

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/native-poc"
PREFETCH = "skd://itunes.apple.com/P000000000/s1/e1"


class NativeBackend:
    def __init__(self, binary, assets, profile):
        self.binary, self.assets, self.profile = map(lambda value: Path(value).resolve(), (binary, assets, profile))
        self.lock = threading.RLock()
        self.spawn_lock = threading.Lock()
        self.timing_lock = threading.Lock()
        self.active = None
        self.stopped = False
        self.cached_tokens = None
        self.token_time = 0
        self.profile.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.profile.chmod(0o700)
        self.web = AppleWebApi(self.tokens)
        self.decryption = NativeDecryption(self.binary, self.assets, self.profile)

    @contextlib.contextmanager
    def profile_access(self):
        descriptor = os.open(self.profile / "native-api.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "r+") as lock_file:
            deadline = time.monotonic() + 65
            while True:
                try:
                    fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if self.stopped or time.monotonic() >= deadline:
                        raise ApiError(503, "Native account profile is busy")
                    time.sleep(0.05)
            yield

    def decrypt(self, token, data):
        with self.lock, self.profile_access():
            if self.stopped:
                raise ApiError(503, "Native server is stopping")
            return self.decryption.decrypt(token, data)

    def call(self, operation, adam="", uri=""):
        with self.lock:
            if self.stopped:
                raise ApiError(503, "Native server is stopping")
            # Serialize users of this profile across server processes as well.
            with self.profile_access():
                command = [str(self.binary), "--assets", str(self.assets), "--apple-profile", "--api", operation,
                           "--data-dir", str(self.profile)]
                if adam:
                    command.extend(["--adam-id", adam])
                if uri:
                    command.extend(["--key-uri", uri])
                with self.spawn_lock:
                    if self.stopped:
                        raise ApiError(503, "Native server is stopping")
                    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    self.active = process
                try:
                    stdout, stderr = process.communicate(timeout=65)
                    code = process.returncode
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    raise ApiError(504, "Native library request timed out") from None
                finally:
                    with self.spawn_lock:
                        self.active = None
                try:
                    value = json.loads(stdout)
                except (ValueError, UnicodeError):
                    value = {}
                if code or not value.get("ok"):
                    # Only stable diagnostics, never response bodies, reach logs.
                    guard = re.search(rb"(?:unimplemented|guarded) Android[^\r\n]*", stderr)
                    diagnostic = guard[0].decode("utf-8", "replace")[:240] if guard else "native request failed"
                    print("native-api: " + operation + " exit=" + str(code) + " " + diagnostic, file=sys.stderr, flush=True)
                    error = value.get("error", "")
                    if error.startswith("no saved account"):
                        raise ApiError(401, "No saved account; sign in first")
                    raise ApiError(502, "Native " + operation + " request failed")
                result = value.get("result")
                if not isinstance(result, dict):
                    raise ApiError(502, "Native library returned an invalid result")
                return result

    def tokens(self):
        with self.lock:
            if self.cached_tokens is None or time.monotonic() - self.token_time > 1800:
                self.cached_tokens = self.call("tokens")
                self.token_time = time.monotonic()
            return self.cached_tokens

    def execute(self, route, query, body):
        if route == "/status":
            return self.call("status")
        adam = body.get("adamId", "") if route == "/license" else query.get("adamId", "")
        if not isinstance(adam, str) or not adam:
            raise ApiError(400, "missing adamId")
        if not re.fullmatch(r"[0-9]{1,19}", adam) or (adam == "0" and route != "/key") or int(adam) > 2**63 - 1:
            raise ApiError(400, "invalid adamId")
        if route == "/m3u8":
            with self.lock:
                return self.web.enhanced_hls(adam)
        if route == "/key":
            uri = query.get("uri", "")
            if not uri:
                raise ApiError(400, "missing uri")
            if not uri.startswith("skd://") or len(uri) > 8192 or any(ord(c) < 32 for c in uri) or (adam != "0" and uri == PREFETCH):
                raise ApiError(400, "invalid uri for adamId")
            with self.lock, self.profile_access():
                if self.stopped:
                    raise ApiError(503, "Native server is stopping")
                return self.decryption.context(adam, uri)
        if route == "/lyrics":
            syllable = query.get("syllable", "1").lower() not in ("0", "false", "no", "off")
            with self.lock:
                return self.web.lyrics(adam, query.get("language", "en"), syllable, query.get("script", "en-Latn"))
        if route == "/webplayback":
            with self.lock:
                return self.web.webplayback(adam)
        if route == "/license":
            challenge, uri = body.get("challenge"), body.get("uri")
            if not isinstance(challenge, str) or not challenge or not isinstance(uri, str) or not uri:
                raise ApiError(400, "missing adamId, challenge, or uri")
            drm = body.get("drm-type", "wv")
            if drm not in ("wv", "pr"):
                raise ApiError(400, "drm-type must be wv or pr")
            with self.lock:
                return self.web.license(adam, challenge, uri, drm)
        raise ApiError(404, "not found")

    def close(self):
        # Synchronize with spawn, so shutdown cannot miss a child being created.
        with self.spawn_lock:
            self.stopped = True
            self.cached_tokens = None
            if self.active is not None:
                with contextlib.suppress(ProcessLookupError):
                    self.active.kill()
        self.decryption.close()

    def record_timing(self, route, method, code, milliseconds):
        record = {"time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "endpoint": route, "method": method, "code": code,
                  "server_ms": round(milliseconds, 3), "backend": "macos-arm64"}
        try:
            with self.timing_lock:
                descriptor = os.open(self.profile / "http-timings.jsonl", os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
                with os.fdopen(descriptor, "w") as output:
                    output.write(json.dumps(record, separators=(",", ":")) + "\n")
        except OSError:
            print("native-api: response timing could not be recorded", file=sys.stderr, flush=True)


class NativeHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, backend, instance=None):
        self.backend = backend
        self.instance = instance or str(uuid.uuid4())
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    server_version = "NativeMusicWrapper/0.1"
    sys_version = ""

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def log_message(self, *_):
        # The default logger would include signed URLs and key query strings.
        pass

    def do_GET(self):
        self.handle_api("GET")

    def do_POST(self):
        self.handle_api("POST")

    def handle_api(self, method):
        started = time.monotonic()
        route = urllib.parse.urlsplit(self.path).path
        binary = None
        try:
            host = self.headers.get("Host", "").split(":", 1)[0]
            if host not in ("127.0.0.1", "localhost") or self.headers.get("Origin"):
                raise ApiError(403, "Local clients only")
            routes = {"/status", "/m3u8", "/key", "/lyrics", "/webplayback", "/license", "/decrypt"}
            if route not in routes:
                raise ApiError(404, "not found")
            if (route in {"/license", "/decrypt"}) != (method == "POST"):
                raise ApiError(405, "method not allowed")
            if self.headers.get("Transfer-Encoding"):
                raise ApiError(400, "transfer encoding is unsupported")
            parameters = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query, keep_blank_values=True, max_num_fields=20)
            if any(len(values) != 1 for values in parameters.values()):
                raise ApiError(400, "duplicate query parameter")
            query = {key: values[0] for key, values in parameters.items()}
            body = {}
            if method == "POST":
                length = int(self.headers.get("Content-Length", "0"))
                limit = MAX_FRAME if route == "/decrypt" else 1024 * 1024
                if length < 0 or length > limit:
                    raise ApiError(413, "request body is too large")
                raw = self.rfile.read(length)
                if len(raw) != length:
                    raise ApiError(400, "truncated request body")
                if route == "/decrypt":
                    if self.headers.get("Content-Type") != "application/octet-stream":
                        raise ApiError(400, "native sample batches require application/octet-stream")
                    binary = self.server.backend.decrypt(self.headers.get("X-Native-Context", ""), raw)
                else:
                    try:
                        body = json.loads(raw)
                    except (ValueError, UnicodeError):
                        raise ApiError(400, "invalid JSON body") from None
                    if not isinstance(body, dict):
                        raise ApiError(400, "JSON body must be an object")
            result = {} if binary is not None else self.server.backend.execute(route, query, body)
            if route == "/status":
                result.update({"server_instance": self.server.instance, "backend": "macos-arm64"})
            payload = {"code": 0, "msg": "SUCCESS", "data": result}
        except ApiError as error:
            payload = {"code": error.code, "msg": str(error)}
        except (ValueError, UnicodeError):
            payload = {"code": 400, "msg": "invalid request"}
        except Exception:
            payload = {"code": 500, "msg": "native request failed"}
        raw = binary if binary is not None and payload["code"] == 0 else json.dumps(payload, separators=(",", ":")).encode()
        milliseconds = (time.monotonic() - started) * 1000
        # Record a known endpoint name only, never arbitrary query/URI text.
        timing_route = route if route in {"/status", "/m3u8", "/key", "/lyrics", "/webplayback", "/license", "/decrypt"} else "unknown"
        self.server.backend.record_timing(timing_route, method, payload["code"], milliseconds)
        try:
            # Keep the lite API's HTTP-200 / JSON-code contract.
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream" if binary is not None and payload["code"] == 0 else "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Wrapper-Backend", "native-macos-arm64")
            self.send_header("Server-Timing", "app;dur=" + format(milliseconds, ".3f"))
            self.end_headers()
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=BUILD / "wrapper-native-poc")
    parser.add_argument("--assets", type=Path, default=BUILD / "assets")
    parser.add_argument("--data-dir", type=Path, default=Path.home() / "Library/Application Support/Native Music Wrapper")
    parser.add_argument("--host", choices=("127.0.0.1", "localhost"), default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12341)
    parser.add_argument("--instance-id", default=None)
    parser.add_argument("--owner-pid", type=int, default=None)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    backend = NativeBackend(args.binary, args.assets, args.data_dir)
    try:
        server = NativeHTTPServer((args.host, args.port), backend, args.instance_id)
    except OSError:
        print("Native server could not open the requested local port.", file=sys.stderr)
        return 1
    def interrupt(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupt)
    if args.owner_pid:
        def watch_owner():
            while not backend.stopped:
                if os.getppid() != args.owner_pid:
                    os.kill(os.getpid(), signal.SIGTERM)
                    return
                time.sleep(0.25)
        threading.Thread(target=watch_owner, daemon=True).start()
    print("Native Apple Music API listening at http://127.0.0.1:" + str(args.port), flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        backend.close()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
