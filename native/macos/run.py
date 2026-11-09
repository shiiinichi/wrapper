#!/usr/bin/env python3
"""Run bounded native probes and save their evidence separately from source."""

import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/native-poc"


def invoke(binary: Path, assets: Path, mode: str, target: str) -> dict:
    command = [str(binary), "--assets", str(assets), mode, target]
    try:
        process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=15)
        try:
            result = json.loads(process.stdout)
        except json.JSONDecodeError:
            result = {"ok": False, "error": "probe exited without a JSON result", "stdout": process.stdout[-4000:]}
        result["exit_code"] = process.returncode
        if process.stderr:
            result["stderr"] = process.stderr[-8000:]
        return result
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "isolated probe exceeded 15 seconds", "timeout": True}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=BUILD / "wrapper-native-poc")
    parser.add_argument("--assets", type=Path, default=BUILD / "assets")
    parser.add_argument("--report", type=Path, default=BUILD / "report.json")
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("run on native arm64 macOS; Rosetta/Linux/QEMU are not this experiment")
    manifest = json.loads((args.assets / "manifest.json").read_text())
    report = {"platform": platform.platform(), "asset_source_commit": manifest["source_commit"],
              "native_probes": {}, "apple_library_inspection": {}}
    for target in ["zlib", "blocks"]:
        result = invoke(args.binary, args.assets, "--probe", target)
        report["native_probes"][target] = result
        print(f"Native {target}: {'PASS' if result.get('ok') else 'FAIL'}")
        if result.get("result"):
            print("  " + json.dumps(result["result"]))
        if not result.get("ok"):
            print("  " + str(result.get("error", result.get("stderr", "unknown failure"))))
    for name in ["libCoreADI.so", "libCoreLSKD.so", "libstoreservicescore.so", "libandroidappmusic.so"]:
        result = invoke(args.binary, args.assets, "--inspect", name)
        report["apple_library_inspection"][name] = result
        loader = result.get("loader") or {}
        images = loader.get("images", [])
        missing = sum(len(image["missing_symbols"]) for image in images)
        guards = sum(len(image["guarded_symbols"]) for image in images)
        mixed = sum(image["mixed_write_execute_pages"] for image in images)
        print(f"Inspect {name}: {len(images)} mapped images; {missing} unresolved, {guards} guarded imports; {mixed} W/X page conflicts")
        if not result.get("ok"):
            print("  " + str(result.get("error", "inspection failed")))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Report: {args.report}")
    results = [*report["native_probes"].values(), *report["apple_library_inspection"].values()]
    return 0 if all(result.get("ok") and result.get("exit_code") == 0 for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
