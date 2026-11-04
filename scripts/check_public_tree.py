"""Sanity-check the files staged for the source repository.

Run after `git add` and before the first public push. This catches common
mistakes, not every possible kind of credential.
"""

from pathlib import Path
import re
import subprocess


root = Path(__file__).resolve().parents[1]
tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).split(b"\0")
if not tracked or not tracked[0]:
    raise SystemExit("No staged files; stage the source before running this check")

bad_path = re.compile(
    r"(^keys/|^work/(?!scripts/)|/\.git/|\.(?:apk|apks|apkm|xapk|aab|so|a|o|dex|smali|cmd|log|keystore|jks|pem|p12|pfx)$)",
    re.IGNORECASE,
)
bad_text = re.compile(
    r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|"
    r"gh[pousr]_[A-Za-z0-9]{20,}|AKIA[0-9A-Z]{16}|"
    r"/home/" + "cer/" + r"|C:\\" + "Users" + r"\\",
)
problems = []
for name in tracked:
    if not name:
        continue
    path = name.decode("utf-8")
    if bad_path.search(path):
        problems.append(f"not source: {path}")
        continue
    source = root / path
    if source.is_dir():  # Git submodule, checked by its own upstream.
        continue
    data = source.read_bytes()
    if len(data) > 2 * 1024 * 1024:
        problems.append(f"unexpected large file: {path}")
    if path.endswith((".png", ".svg")):
        continue
    if b"\0" in data:
        problems.append(f"unexpected binary file: {path}")
    elif bad_text.search(data.decode("utf-8", errors="replace")):
        problems.append(f"local path or credential pattern: {path}")

if problems:
    raise SystemExit("Public tree check failed:\n" + "\n".join(problems))
print(f"Public tree check passed ({len(tracked) - 1} staged paths; review the diff manually as well)")
