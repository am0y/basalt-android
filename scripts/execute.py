#!/usr/bin/env python3
"""Compile Luau for this Roblox client and submit it to the live VM bridge."""

import argparse
import math
import os
import re
import shlex
import subprocess
import urllib.request
import sys
import tempfile
import time
from pathlib import Path
from uuid import uuid4


ROOT = Path(__file__).resolve().parent.parent
NATIVE = ROOT / "native"
ADB = os.environ.get("BASALT_ADB", "/mnt/c/Program Files/BlueStacks_nxt/HD-Adb.exe")
DEVICE = os.environ.get("BASALT_DEVICE", os.environ.get("ANDROID_SERIAL", ""))
ROOT_ADB = None
CHANNEL = os.environ.get("DECODE_CHANNEL", "/sdcard/Android/data/com.roblox.client/files/decode")
FINISHED = re.compile(r"^protected execution status=(\d+)$", re.MULTILINE)
LOAD = re.compile(r"^load status=(\d+)\b", re.MULTILINE)
COMPLETE = re.compile(r"^request completed status=(\d+)$", re.MULTILINE)
SCRIPT_COMPLETE = re.compile(r"^script completed status=(\d+)$", re.MULTILINE)


def completion_status(log: str, scheduled: bool) -> int | None:
    request = COMPLETE.findall(log)
    if request and int(request[-1]) != 0:
        return int(request[-1])
    if scheduled:
        finished = SCRIPT_COMPLETE.findall(log)
        return int(finished[-1]) if finished else None
    return int(request[-1]) if request else None


def uses_request_logs(session: str, log: str) -> bool:
    return bool(re.search(r"^protocol=[23]$", session.replace("\r", ""), re.MULTILINE)) or \
        "request-log protocol v2" in log or "request-log protocol v3" in log


def select_device(command: tuple[str, ...]) -> tuple[str, ...]:
    if command[0] == ADB and DEVICE:
        return (ADB, "-s", DEVICE, *command[1:])
    return command


def run(*command: str, timeout: float = 30) -> str:
    if command[0] == ADB and ADB.lower().endswith(".exe") and len(command) > 2 and command[1] == "push":
        source = subprocess.check_output(["wslpath", "-w", command[2]], text=True).strip()
        command = (*command[:2], source, *command[3:])
    command = select_device(command)
    completed = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=timeout)
    if completed.returncode:
        raise RuntimeError(
            f"{' '.join(command)} failed ({completed.returncode}): "
            f"{completed.stderr.strip() or completed.stdout.strip()}"
        )
    return completed.stdout


def ensure_http_bridge():
    try:
        urllib.request.urlopen("http://127.0.0.1:17981/health", timeout=1).close()
        return
    except OSError:
        pass
    subprocess.Popen(
        (sys.executable, str(ROOT / "scripts/http_bridge.py")),
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    for _ in range(20):
        try:
            urllib.request.urlopen("http://127.0.0.1:17981/health", timeout=0.5).close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("Could not start the HTTP bridge")


def parse_logcat(output: str, timestamp: float, pid: str) -> list[str]:
    lines = []
    for line in output.splitlines():
        parts = line.split(maxsplit=5)
        if len(parts) < 6:
            continue
        try:
            recent = float(parts[0]) > timestamp
        except ValueError:
            continue
        if recent and parts[1] == pid:
            for marker in ("[FLog::CreatorOutput]", "[FLog::CreatorError]", "[FLog::CreatorWarning]"):
                if marker in line:
                    lines.append(line.split(marker, 1)[1].strip())
                    break
    return lines


def root_shell(command: str, timeout: float = 10) -> str:
    global ROOT_ADB
    if ROOT_ADB is None:
        ROOT_ADB = run(ADB, "shell", "id -u", timeout=10).strip() == "0"
        if not ROOT_ADB:
            # App-specific external storage is ADB-readable on some unrooted phones.
            probe = subprocess.run(select_device((ADB, "shell", f"test -r {CHANNEL}/out/session.txt")),
                                   capture_output=True, timeout=10)
            ROOT_ADB = probe.returncode == 0
    return run(ADB, "shell", command if ROOT_ADB else "su -c " + shlex.quote(command), timeout=timeout)


def executor_log() -> str:
    return root_shell(f"test ! -f {CHANNEL}/out/log.txt || cat {CHANNEL}/out/log.txt").replace("\r", "")


def request_log(token: str) -> str:
    return root_shell(f"test ! -f {CHANNEL}/out/{token}.cmd.log || cat {CHANNEL}/out/{token}.cmd.log").replace("\r", "")


def logcat_since(timestamp: float, pid: str) -> list[str]:
    output = run(ADB, "logcat", "-d", "-v", "epoch", "-s", "Roblox:I", "*:S", "-t", "500", timeout=20)
    return parse_logcat(output, timestamp, pid)


def wait_for_output(timestamp: float, pid: str, budget: float = 2) -> list[str]:
    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        messages = logcat_since(timestamp, pid)
        if messages:
            return messages
        time.sleep(0.25)
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?", help="Luau source file to execute")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--inspect", action="store_true", help="load bytecode without executing it")
    modes.add_argument("--direct", action="store_true", help="legacy synchronous diagnostic mode")
    modes.add_argument("--schedule", action="store_true", help="run on a new Luau thread (default)")
    parser.add_argument("--status", help="read a previous 32-character request ID; source may be omitted", default=None)
    parser.add_argument("--opt", type=int, choices=(0, 1, 2), default=1, help="Luau optimization level (default: 1)")
    parser.add_argument("--debug", type=int, choices=(0, 1, 2), default=1, help="Luau debug level (default: 1)")
    parser.add_argument("--type", type=int, choices=(0, 1), default=0, help="Luau type information level (default: 0)")
    parser.add_argument("--timeout", type=float, default=20, help="seconds to wait for the VM (default: 20)")
    args = parser.parse_args()

    if args.status:
        if not re.fullmatch(r"[0-9a-f]{32}", args.status):
            parser.error("--status must be a 32-character request ID")
        output = request_log(args.status)
        print(output or "No result log yet")
        status = completion_status(output, "script scheduled" in output)
        return 0 if status == 0 else 1 if status is not None else 3
    if args.source is None or not args.source.is_file():
        parser.error(f"source file does not exist: {args.source}")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive")
    scheduled = not args.direct and not args.inspect

    pid = run(ADB, "shell", "pidof com.roblox.client", timeout=10).strip()
    if not pid or not pid.isdecimal():
        raise RuntimeError("Roblox must be running before submitting bytecode")

    if scheduled:
        ensure_http_bridge()

    run("make", "-C", str(NATIVE), "compile_bytecode", timeout=120)
    session = root_shell(f"test ! -f {CHANNEL}/out/session.txt || cat {CHANNEL}/out/session.txt")
    if session and f"pid={pid}\n" not in session.replace("\r", ""):
        raise RuntimeError("Native session belongs to a different process; wait for Roblox to finish loading")
    baseline = executor_log()
    v2 = uses_request_logs(session, baseline)

    with tempfile.TemporaryDirectory(prefix="basalt-") as folder:
        command = Path(folder) / "command.cmd"
        options = (["inspect"] if args.inspect else ["schedule"] if scheduled else []) + [f"--opt={args.opt}", f"--debug={args.debug}", f"--type={args.type}"]
        if "loader=engine-native" in session:
            options.append("--x86-httpget")
        print(run(str(NATIVE / "compile_bytecode"), str(args.source), str(command), *options, timeout=30).strip())

        token = uuid4().hex
        staging = f"{CHANNEL}/in/.{token}.cmd"
        ready = f"{CHANNEL}/in/{token}.cmd"
        try:
            run(ADB, "push", str(command), staging, timeout=30)
            timestamp = float(run(ADB, "shell", "date +%s", timeout=10).strip()) - 1
            run(ADB, "shell", f"mv {staging} {ready}", timeout=10)
        except Exception:
            subprocess.run(select_device((ADB, "shell", f"rm -f {staging}")), capture_output=True, timeout=10)
            raise

    print(f"Request {token}; Roblox PID {pid}; waiting for {'script completion' if scheduled else 'VM result'}...")
    deadline = time.monotonic() + args.timeout
    last_identity_check = 0.0
    while time.monotonic() < deadline:
        output = request_log(token) if v2 else executor_log()
        new = output if v2 else (output[len(baseline):] if output.startswith(baseline) else output)
        if new:
            errors = LOAD.findall(new)
            result = FINISHED.findall(new)
            complete = COMPLETE.findall(new)
            status = completion_status(new, scheduled)
            done = status is not None if v2 else bool(result or (errors and errors[-1] != "0") or (args.inspect and "inspect LIVE" in new))
            if done:
                print(new.strip())
                if not session:
                    for message in logcat_since(timestamp, pid):
                        print("Roblox:", message)
                if v2:
                    return 0 if status == 0 else 1
                if result:
                    return 0 if result[-1] == "0" else 1
                return 0 if args.inspect and errors and errors[-1] == "0" else 1
        time.sleep(0.35)
        if time.monotonic() - last_identity_check > 3:
            live = subprocess.run(select_device((ADB, "shell", "pidof com.roblox.client")), capture_output=True, text=True, timeout=10).stdout.strip()
            if live != pid:
                raise RuntimeError(f"Roblox PID changed from {pid} to {live or 'stopped'}; request {token} was not retried")
            last_identity_check = time.monotonic()

    raise RuntimeError(f"Request {token} has not completed within {args.timeout:g}s. It may still run; do not automatically retry. Log: {CHANNEL}/out/{token}.cmd.log")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"decode: {exc}", file=sys.stderr)
        sys.exit(1)
