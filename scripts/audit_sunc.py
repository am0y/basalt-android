#!/usr/bin/env python3
"""Audit presence against official metadata; this is never a behavioral score."""
import json
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
URL = "https://docs.sunc.io/api/mini.json"


def main():
    request = urllib.request.Request(URL, headers={"User-Agent": "basalt-api-audit/1.0"})
    with urllib.request.urlopen(request, timeout=20) as response:
        metadata = json.load(response)
    names = sorted(set(metadata) | {"Drawing.new", "WebSocket.connect"})
    source = "local names = {" + ",".join(json.dumps(name) for name in names) + "}\n" + '''
local present, missing = 0, 0
local env = getfenv()
for _, name in names do
    local value = env
    for _, key in string.split(name, ".") do
        value = type(value) == "table" and value[key] or nil
    end
    local available = type(value) == "function"
    print("SUNC_API_PRESENCE", name, available and "present" or "missing")
    if available then present += 1 else missing += 1 end
end
print("SUNC_API_INVENTORY", present, missing, #names)
'''
    with tempfile.TemporaryDirectory(prefix="basalt-audit-") as directory:
        path = Path(directory) / "audit.luau"
        path.write_text(source)
        completed = subprocess.run(
            [sys.executable, str(ROOT / "scripts/execute.py"), str(path), "--timeout", "35"],
            capture_output=True, text=True, timeout=100,
        )
    print(completed.stdout, end="")
    print(completed.stderr, end="", file=sys.stderr)
    print("Presence inventory only. No function is certified by this check.")
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
