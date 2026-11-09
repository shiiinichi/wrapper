#!/usr/bin/env python3
"""Run native sign-in with private local prompts and a separate persistent profile."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/native-poc"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=BUILD / "wrapper-native-poc")
    parser.add_argument("--assets", type=Path, default=BUILD / "assets")
    parser.add_argument("--data-dir", type=Path, default=BUILD / "auth-profile")
    parser.add_argument("--owner-pid", type=int, default=None)
    args = parser.parse_args()
    def interrupt(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupt)
    if args.owner_pid:
        def watch_owner():
            while True:
                if os.getppid() != args.owner_pid:
                    os.kill(os.getpid(), signal.SIGTERM)
                    return
                time.sleep(0.25)
        threading.Thread(target=watch_owner, daemon=True).start()
    args.data_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    args.data_dir.chmod(0o700)
    print("Opening native wrapper sign-in prompts. Enter your Apple Account details in the macOS dialogs.", flush=True)
    command = [str(args.binary.resolve()), "--assets", str(args.assets.resolve()), "--apple-profile", "--probe", "auth-login",
               "--data-dir", str(args.data_dir.resolve()), "--prompt-script", str(Path(__file__).with_name("sign_in.applescript"))]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=600)
    except (subprocess.TimeoutExpired, KeyboardInterrupt):
        os.killpg(process.pid, signal.SIGTERM)
        process.communicate()
        print("Native sign-in stopped; no successful authentication was reported.")
        return 1
    try:
        report = json.loads(stdout)
    except json.JSONDecodeError:
        report = {"ok": False, "error": "native sign-in exited before producing a result"}
    report["exit_code"] = process.returncode
    # Guest logging is disabled for authentication. This is bounded native stage
    # and compatibility diagnostics; account/password/code are never included.
    report["diagnostics"] = stderr[-8000:]
    path = args.data_dir / "auth-report.json"
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    result = report.get("result") or {}
    if report.get("ok") and result.get("authenticated") and process.returncode == 0:
        print("Native Apple Account sign-in succeeded; a usable account is present.")
    elif result.get("input_error"):
        print("The native prompt could not provide valid input; authentication was cancelled.")
    elif result.get("cancelled"):
        print("Native sign-in was cancelled; authentication did not complete.")
    else:
        print("Native sign-in did not complete: " + str(report.get("error", "response type " + str(result.get("response_type")))))
        # Named guards and memory-fault locations contain no credential values.
        for line in stderr.splitlines():
            if any(marker in line for marker in ["unimplemented Android", "guarded Android", "memory fault", "unsupported curl"]):
                print(line)
    print("Private diagnostic report: " + str(path))
    return 0 if report.get("ok") and result.get("authenticated") and process.returncode == 0 else 1

if __name__ == "__main__":
    raise SystemExit(main())
