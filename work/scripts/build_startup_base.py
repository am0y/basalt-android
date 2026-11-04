#!/usr/bin/env python3
"""Patch the startup dialog and scoped code-input limit in the pinned DEX."""
import argparse
import hashlib
import subprocess
import tempfile
import zipfile
from pathlib import Path

from build_phone_apk import signature_metadata

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "work"
REFERENCE = WORK / "base_basalt_invite_required_signed.apk"
CLASSES = [Path("com/roblox/client/startup/BasaltUpdates.smali"), Path("com/roblox/client/game/b.smali"),
           Path("com/roblox/client/RbxKeyboard.smali"), Path("com/roblox/client/game/b$d$a.smali")]
ADDED_CLASSES = [Path("com/roblox/client/startup/BasaltHttp.smali"), Path("com/roblox/client/startup/BasaltEditorInput.smali"),
                 Path("com/roblox/client/startup/BasaltWorkspaceAccess.smali"), Path("com/roblox/client/startup/BasaltInputSimulation.smali"),
                 Path("com/roblox/client/startup/BasaltInputSimulation$Event.smali"), Path("com/roblox/client/startup/BasaltHttp$SocketState.smali"),
                 Path("com/roblox/client/startup/BasaltHttp$1.smali")]


def build(output, reference=REFERENCE, helpers_only=False, editor_code_limit=False):
    if output.exists():
        raise FileExistsError(output)
    with tempfile.TemporaryDirectory(prefix="startup-base-", dir=WORK) as directory:
        stage = Path(directory)
        original, patched = stage / "original.dex", stage / "classes2.dex"
        with zipfile.ZipFile(reference) as base:
            original.write_bytes(base.read("classes2.dex"))
            subprocess.run(["baksmali", "disassemble", "--api", "28", "--jobs", "4", "--output", str(stage / "smali"), str(original)], check=True)
            classes = [] if helpers_only else CLASSES
            if editor_code_limit and helpers_only:
                classes = [Path("com/roblox/client/game/b.smali")]
            for relative in classes:
                target = stage / "smali" / relative
                if not target.is_file():
                    raise ValueError(f"target class is absent from the reference DEX: {relative}")
                source = (WORK / "base_decoded/smali_classes2" / relative).read_bytes()
                if editor_code_limit and relative == Path("com/roblox/client/game/b.smali"):
                    for register in (b"v0", b"v1"):
                        old = b"const " + register + b", 0x100000"
                        new = b"const " + register + b", 0x800000"
                        if source.count(old) == 1: source = source.replace(old, new)
                        elif source.count(new) != 1: raise ValueError("unexpected editor input limit layout")
                target.write_bytes(source)
            for relative in ADDED_CLASSES:
                target = stage / "smali" / relative
                if target.exists() and not helpers_only:
                    raise ValueError(f"new helper already exists in reference DEX: {relative}")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((WORK / "base_decoded/smali_classes2" / relative).read_bytes())
            subprocess.run(["smali", "assemble", "--api", "28", "--jobs", "4", "--output", str(patched), str(stage / "smali")], check=True)
            with zipfile.ZipFile(output, "w", allowZip64=True) as result:
                for item in base.infolist():
                    if signature_metadata(item.filename) or item.filename == "stamp-cert-sha256":
                        continue
                    result.writestr(item, patched.read_bytes() if item.filename == "classes2.dex" else base.read(item.filename))
    print(f"Startup base: {output}")
    print(f"SHA-256: {hashlib.sha256(output.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=WORK / "base_basalt_release_100.apk")
    parser.add_argument("--reference", type=Path, default=REFERENCE)
    parser.add_argument("--helpers-only", action="store_true", help="update helpers in an existing patched base, preserving startup/input classes")
    parser.add_argument("--editor-code-limit", action="store_true", help="apply the 8 MiB editor source input limit")
    args = parser.parse_args()
    build(args.output, args.reference, args.helpers_only, args.editor_code_limit)
