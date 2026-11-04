#!/usr/bin/env python3
"""Build a locally signed Roblox 2.744.1081 ARM64 base and matching split.

This packaging command is for a validated 2.744 ARM64 runtime; a library built
with 2.743 engine offsets cannot provide an operational Basalt installation.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from build_743_apk import HELPERS, build, digest, run, signature_metadata, signing_credentials

ROOT = Path(__file__).resolve().parents[2]
BUILD_ID = bytes.fromhex("cd14337ff95a497763bf0f2b08d2359314bc663b")
BASE_SHA256 = "9d88e688bb5aec39c6e8b3a3f5bd457a0e24c34433bc8183123c55c7dcc6d8f9"
SPLIT_SHA256 = "7317350d0ec70839b6572326aad8ca48473c39cabbd52b9e0e8b661a4503147a"


def sign_split(source: Path, output: Path) -> None:
    if digest(source) != SPLIT_SHA256:
        raise ValueError("split does not match pinned Roblox 2.744.1081 ARM64 input")
    with zipfile.ZipFile(source) as split:
        if BUILD_ID not in split.read("lib/arm64-v8a/libroblox.so")[:8192]:
            raise ValueError("split contains an unexpected ARM64 engine")
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="basalt744-arm64-", dir=output.parent) as temp:
            unsigned = Path(temp) / "unsigned.apk"
            aligned = Path(temp) / "aligned.apk"
            with zipfile.ZipFile(unsigned, "w", allowZip64=True) as merged:
                for entry in split.infolist():
                    if entry.filename == "META-INF/MANIFEST.MF" or not signature_metadata(entry.filename):
                        merged.writestr(entry, split.read(entry.filename))
            run("zipalign", "-f", "4", unsigned, aligned)
            keystore, environment = signing_credentials()
            run("apksigner", "sign", "--v1-signing-enabled", "true", "--v2-signing-enabled", "true",
                "--v3-signing-enabled", "true", "--v4-signing-enabled", "false", "--ks", keystore,
                "--ks-key-alias", "basalt743dev", "--ks-pass", "env:BASALT_743_DEV_PASSWORD",
                "--key-pass", "env:BASALT_743_DEV_PASSWORD", "--out", output, aligned, env=environment)
    run("apksigner", "verify", "--verbose", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, default=ROOT / "work/744/base.apk")
    parser.add_argument("--split", type=Path, default=ROOT / "work/744/split_config.arm64_v8a.apk")
    parser.add_argument("--library", type=Path, default=ROOT / "native/libbasalt-744-dev.so")
    parser.add_argument("--output", type=Path, default=ROOT / "work/744/Basalt-2.744.1081-arm64-dev.apk")
    parser.add_argument("--split-output", type=Path, default=ROOT / "work/744/split_config.arm64_v8a.resigned.apk")
    parser.add_argument("--bundle-output", type=Path, default=ROOT / "work/744/Basalt-2.744.1081-arm64-dev.apks")
    parser.add_argument("--bundle-only", action="store_true", help="repackage already signed base and split")
    parser.add_argument("--reuse-split", action="store_true", help="reuse the verified locally signed split on development rebuilds")
    parser.add_argument("--no-startup-dialogs", action="store_true", help="omit Java startup dialogs for UI testing; keep native helpers and artwork")
    args = parser.parse_args()
    if args.bundle_only:
        for path in (args.output, args.split_output):
            run("apksigner", "verify", path)
        with zipfile.ZipFile(args.bundle_output, "w", allowZip64=True) as bundle:
            bundle.write(args.output, "base.apk", compress_type=zipfile.ZIP_STORED)
            bundle.write(args.split_output, "split_config.arm64_v8a.apk", compress_type=zipfile.ZIP_STORED)
        print(f"Built bundle {args.bundle_output} ({digest(args.bundle_output)})")
        sys.exit(0)
    if digest(args.split) != SPLIT_SHA256:
        raise ValueError("split does not match pinned Roblox 2.744.1081 ARM64 input")
    if not args.library.is_file() or b"BASALT_ARM_ENGINE_2.744.1081_cd14337ff95a497763bf0f2b08d2359314bc663b" not in args.library.read_bytes():
        raise ValueError("ARM64 runtime is not pinned to the supplied 2.744.1081 engine; refusing to package an incompatible library")
    HELPERS.mkdir(parents=True, exist_ok=True)
    if len(list(HELPERS.glob("BasaltHttp*.smali"))) < 3 or (ROOT / "native/android/BasaltHttp.java").stat().st_mtime > (HELPERS / "BasaltHttp.smali").stat().st_mtime:
        subprocess.run([sys.executable, str(Path(__file__).with_name("build_java_http.py"))], check=True)
    build(args.base, args.library, args.output, abi="arm64-v8a", engine_split=args.split,
           source_sha256=BASE_SHA256, engine_build_id=BUILD_ID,
            soname="basalt", texture_dir="basalt", startup_prompts=True,
            show_startup_dialogs=not args.no_startup_dialogs, scoped_editor_limit=True)
    if args.reuse_split:
        if not args.split_output.is_file():
            raise FileNotFoundError(args.split_output)
        run("apksigner", "verify", args.split_output)
    else:
        sign_split(args.split, args.split_output)
    with zipfile.ZipFile(args.bundle_output, "w", allowZip64=True) as bundle:
        bundle.write(args.output, "base.apk", compress_type=zipfile.ZIP_STORED)
        bundle.write(args.split_output, "split_config.arm64_v8a.apk", compress_type=zipfile.ZIP_STORED)
    print(f"Built {args.output} ({digest(args.output)}) and {args.split_output} ({digest(args.split_output)})")
