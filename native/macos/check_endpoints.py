#!/usr/bin/env python3
"""Check the native HTTP API and save response metadata/timings only."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:12341")
    parser.add_argument("--adam-id", required=True)
    parser.add_argument("--report", type=Path, default=ROOT / "build/native-poc/endpoint-checks/native-http-report.json")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]{1,19}", args.adam_id):
        parser.error("adam-id must be numeric")
    base = urllib.parse.urlsplit(args.base_url)
    if base.scheme != "http" or base.hostname not in ("127.0.0.1", "localhost") or base.username or base.password or base.query or base.fragment:
        parser.error("base-url must be a local HTTP address")
    records = []
    report = {"time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "base_url": args.base_url,
              "backend": "native-macos-arm64", "adamId": args.adam_id, "checks": records,
              "limitations": ["No sample decryption or playback is tested.",
                              "A real DRM license needs a CDM-generated challenge.",
                              "The native key result does not supply a Temari template."]}

    def emit(record):
        records.append(record)
        print(json.dumps(record), flush=True)
        args.report.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        args.report.parent.chmod(0o700)
        descriptor = os.open(args.report, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as output:
            json.dump(report, output, indent=2)
            output.write("\n")

    def request(name, path, parameters=None, body=None, expected=0):
        address = args.base_url.rstrip("/") + path + ("?" + urllib.parse.urlencode(parameters) if parameters else "")
        started = time.monotonic()
        record = {"case": name, "endpoint": path, "expected_code": expected}
        payload = {}
        try:
            request = urllib.request.Request(address, data=json.dumps(body).encode() if body is not None else None,
                                             headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=100) as response:
                raw = response.read(4 * 1024 * 1024 + 1)
                backend = response.headers.get("X-Wrapper-Backend")
                timing = re.search(r"app;dur=([0-9.]+)", response.headers.get("Server-Timing", ""))
                record.update({"http_status": response.status, "server_ms": float(timing[1]) if timing else None})
            if backend != "native-macos-arm64":
                raise ValueError("the listener is not the native backend")
            if len(raw) > 4 * 1024 * 1024:
                raise ValueError("oversized response")
            payload = json.loads(raw)
            record.update({"code": payload.get("code"), "passed": payload.get("code") == expected,
                           "response_bytes": len(raw)})
            value = payload.get("data") or {}
            if path == "/status" and expected == 0:
                record["regions"] = value.get("regions")
                record["account_present"] = value.get("account_present")
                record["passed"] &= bool(record["account_present"])
            if path == "/lyrics" and expected == 0:
                lyrics = value.get("lyrics", "")
                record["lyrics_bytes"] = len(lyrics.encode())
                record["valid_ttml"] = bool(lyrics) and ET.fromstring(lyrics).tag.rsplit("}", 1)[-1] == "tt"
                record["passed"] &= record["valid_ttml"]
            if path in ("/m3u8", "/webplayback") and expected == 0:
                record["manifest_present"] = bool(value.get("m3u8"))
                record["passed"] &= record["manifest_present"]
            if path == "/key" and expected == 0:
                record["content_key_present"] = bool(value.get("contentKey"))
                record["temari_compatible"] = value.get("temariCompatible", False)
                record["passed"] &= record["content_key_present"]
        except Exception as error:
            record.update({"passed": False, "error_type": type(error).__name__})
        record["client_ms"] = round((time.monotonic() - started) * 1000, 3)
        emit(record)
        return payload

    def playlist(name, address):
        started = time.monotonic()
        try:
            parsed = urllib.parse.urlsplit(address)
            if parsed.scheme != "https" or not any((parsed.hostname or "").endswith("." + domain) for domain in ("apple.com", "mzstatic.com")):
                raise ValueError("not an Apple HTTPS playlist")
            with urllib.request.urlopen(address, timeout=30) as response:
                raw = response.read(2 * 1024 * 1024 + 1)
            if len(raw) > 2 * 1024 * 1024:
                raise ValueError("oversized playlist")
            text = raw.decode("utf-8-sig")
            keys = [key for key in re.findall(r'#EXT-X-(?:SESSION-)?KEY:[^\r\n]*URI="(skd://[^\"]+)"', text)
                    if key != "skd://itunes.apple.com/P000000000/s1/e1"]
            emit({"case": name, "passed": text.startswith("#EXTM3U"), "playlist_bytes": len(raw),
                  "track_key_uris": len(set(keys)), "external_playlist_ms": round((time.monotonic() - started) * 1000, 3)})
            return keys[0] if keys else None
        except Exception as error:
            emit({"case": name, "passed": False, "error_type": type(error).__name__})
            return None

    status = request("status", "/status")
    if status.get("code") != 0 or (status.get("data") or {}).get("backend") != "macos-arm64":
        return 1
    adam = {"adamId": args.adam_id}
    enhanced = request("m3u8", "/m3u8", adam)
    key = playlist("enhanced_playlist", (enhanced.get("data") or {}).get("m3u8", "")) if enhanced.get("code") == 0 else None
    request("lyrics_syllable", "/lyrics", dict(adam, syllable="1"))
    request("lyrics_line", "/lyrics", dict(adam, syllable="0"))
    web = request("webplayback", "/webplayback", adam)
    if web.get("code") == 0:
        playlist("webplayback_playlist", (web.get("data") or {}).get("m3u8", ""))
    if key:
        request("track_key", "/key", dict(adam, uri=key))
    else:
        emit({"case": "track_key", "passed": False, "reason": "no native enhanced-manifest key URI"})
    for path in ("/m3u8", "/key", "/lyrics", "/webplayback"):
        request(path[1:] + "_missing_id", path, expected=400)
    request("key_missing_uri", "/key", adam, expected=400)
    request("license_missing_fields", "/license", body={}, expected=400)
    request("license_invalid_drm", "/license", body={"adamId": args.adam_id, "challenge": "validation-only",
            "uri": "validation-only", "drm-type": "unsupported"}, expected=400)
    emit({"case": "license_acquisition", "tested": False, "reason": "requires a real DRM challenge"})
    print("Private response-time report: " + str(args.report), flush=True)
    return 0 if all(record.get("passed", True) for record in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
