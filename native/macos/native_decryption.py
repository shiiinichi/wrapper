"""Bounded, reusable native sample worker; keys and handles never enter logs."""
from collections import OrderedDict
import contextlib
import os
import re
import secrets
import select
import struct
import subprocess
import threading
import time

from web_api import ApiError

MAX_FRAME = 16 * 1024 * 1024 - 8  # Leave room for the worker operation and handle.
MAX_SAMPLES = 8192


def sample_sizes(data):
    if len(data) < 4 or len(data) > MAX_FRAME:
        raise ApiError(400, "Invalid native sample batch")
    count = struct.unpack_from(">I", data)[0]
    if not 1 <= count <= MAX_SAMPLES:
        raise ApiError(400, "Invalid native sample count")
    offset, sizes = 4, []
    for _ in range(count):
        if offset + 4 > len(data):
            raise ApiError(400, "Truncated native sample batch")
        size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if not 1 <= size <= 4 * 1024 * 1024 or size > len(data) - offset:
            raise ApiError(400, "Invalid native sample size")
        sizes.append(size)
        offset += size
    if offset != len(data):
        raise ApiError(400, "Trailing native sample bytes")
    return sizes


class NativeDecryption:
    def __init__(self, binary, assets, profile):
        self.binary, self.assets, self.profile = binary, assets, profile
        self.contexts = OrderedDict()
        self.process = None
        self.state_lock = threading.Lock()
        self.stopped = False
        self.generation = 0
        self.opens = 0

    def _stop_worker(self):
        with self.state_lock:
            process, self.process = self.process, None
            if process is not None and process.poll() is None:
                with contextlib.suppress(ProcessLookupError):
                    process.kill()
        if process is not None:
            with contextlib.suppress(subprocess.TimeoutExpired):
                process.wait(timeout=2)
            for pipe in (process.stdin, process.stdout):
                if pipe is not None:
                    pipe.close()

    def close(self):
        with self.state_lock:
            self.stopped = True
        self._stop_worker()

    def _read(self, process, size, deadline):
        result = bytearray()
        descriptor = process.stdout.fileno()
        while len(result) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([descriptor], [], [], remaining)[0]:
                raise ApiError(504, "Native sample worker timed out")
            chunk = os.read(descriptor, min(size - len(result), 65536))
            if not chunk:
                raise ApiError(502, "Native sample worker stopped")
            result.extend(chunk)
        return bytes(result)

    def _exchange(self, request=None):
        process = self.process
        deadline = time.monotonic() + 65
        try:
            if request is not None:
                data = memoryview(struct.pack(">I", len(request)) + request)
                descriptor = process.stdin.fileno()
                while data:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not select.select([], [descriptor], [], remaining)[1]:
                        raise ApiError(504, "Native sample worker timed out")
                    count = os.write(descriptor, data[:65536])
                    if count <= 0:
                        raise ApiError(502, "Native sample worker stopped")
                    data = data[count:]
            size = struct.unpack(">I", self._read(process, 4, deadline))[0]
            if not 1 <= size <= MAX_FRAME + 1:
                raise ApiError(502, "Invalid native worker response")
            response = self._read(process, size, deadline)
        except (OSError, ValueError, AttributeError):
            self._stop_worker()
            raise ApiError(503 if self.stopped else 502, "Native sample worker unavailable") from None
        except ApiError:
            self._stop_worker()
            raise
        if response[0] != 0:
            raise ApiError(422, "Native sample operation failed")
        return response[1:]

    def _ensure_worker(self):
        if self.process is not None and self.process.poll() is not None:
            self._stop_worker()
        if self.process is not None:
            return
        with self.state_lock:
            if self.stopped:
                raise ApiError(503, "Native server is stopping")
            self.process = subprocess.Popen([str(self.binary), "--assets", str(self.assets), "--apple-profile",
                                             "--decryption-worker", "--data-dir", str(self.profile)],
                                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.DEVNULL, bufsize=0)
            os.set_blocking(self.process.stdin.fileno(), False)
            os.set_blocking(self.process.stdout.fileno(), False)
            self.generation += 1
            self.opens = 0
        if self._exchange() != b"NAD2":
            self._stop_worker()
            raise ApiError(502, "Incompatible native sample worker")

    def _ensure_context(self, entry):
        self._ensure_worker()
        if entry.get("generation") == self.generation:
            return
        # The library retains a context map. Recycle before its 1024-context limit;
        # descriptors survive and are rehydrated lazily in the replacement worker.
        if self.opens >= 64:
            self._stop_worker()
            self._ensure_worker()
        adam, uri = entry["adam"].encode(), entry["uri"].encode()
        # The library's key cache only distinguishes catalog assets, not their
        # rendition/key URIs. Retain live contexts, but reset its lookup cache.
        self._exchange(struct.pack(">I", 3))
        request = struct.pack(">II", 1, len(adam)) + adam + struct.pack(">I", len(uri)) + uri
        self.opens += 1
        response = self._exchange(request)
        if len(response) < 8:
            raise ApiError(502, "Invalid native context response")
        handle, size = struct.unpack_from(">II", response)
        if not 1 <= handle <= 64 or size != len(response) - 8 or not size:
            raise ApiError(502, "Invalid native context response")
        try:
            content = response[8:].decode("utf-8")
        except UnicodeError:
            raise ApiError(502, "Invalid native key response") from None
        entry.update(handle=handle, content=content, generation=self.generation)

    def _prune(self):
        cutoff = time.monotonic() - 1800
        for token in [token for token, value in self.contexts.items() if value["used"] < cutoff]:
            del self.contexts[token]

    def context(self, adam, uri):
        self._prune()
        for token, entry in self.contexts.items():
            if entry["adam"] == adam and entry["uri"] == uri:
                break
        else:
            token = secrets.token_hex(24)
            entry = {"adam": adam, "uri": uri, "used": time.monotonic()}
        self._ensure_context(entry)
        entry["used"] = time.monotonic()
        self.contexts[token] = entry
        self.contexts.move_to_end(token)
        while len(self.contexts) > 32:
            self.contexts.popitem(last=False)
        return {"adamId": adam, "keyUri": uri, "contentKey": entry["content"], "temariCompatible": False,
                "nativeDecryptionVersion": 1, "nativeContext": token}

    def decrypt(self, token, data):
        sizes = sample_sizes(data)
        self._prune()
        if not isinstance(token, str) or not re.fullmatch(r"[a-f0-9]{48}", token) or token not in self.contexts:
            raise ApiError(409, "Native decryption context expired; request the key again")
        entry = self.contexts[token]
        for attempt in range(2):
            try:
                self._ensure_context(entry)
                response = self._exchange(struct.pack(">II", 2, entry["handle"]) + data)
                break
            except ApiError as error:
                if attempt or error.code not in (502, 504) or self.stopped:
                    raise
        try:
            valid = sample_sizes(response) == sizes
        except ApiError:
            valid = False
        if not valid:
            self._stop_worker()
            raise ApiError(502, "Native worker changed the sample layout")
        entry["used"] = time.monotonic()
        self.contexts.move_to_end(token)
        return response
