#!/usr/bin/env python3
"""Build a local ARM64 Basalt development APK from the pinned Roblox 2.743.1136 APK."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "work"
EXPECTED_SOURCE_SHA256 = "6ee6bc94f503027f670034579fd7cb1f5a710b4016cb65645ba8abe8eef41a69"
EXPECTED_ENGINE_BUILD_ID = bytes.fromhex("4a916f58386d0c329a1202bfc4df0c392e869487")
HELPERS = WORK / "base_decoded/smali_classes2/com/roblox/client/startup"
STARTUP = Path("com/roblox/client/startup/a.smali")
LIBRARY_NAME = "lib/arm64-v8a/libbasalt.so"


def public_storage_manifest(data: bytes) -> bytes:
    spec = importlib.util.spec_from_file_location("basalt_phone_builder", Path(__file__).with_name("build_phone_apk.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.public_storage_manifest(data)


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def run(*args: object, env: dict[str, str] | None = None) -> None:
    subprocess.run([str(arg) for arg in args], check=True, env=env)


def signing_credentials() -> tuple[Path, dict[str, str]]:
    directory = WORK / "743/keys"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    password_file = directory / "password"
    keystore = directory / "development.keystore"
    if not password_file.exists():
        with password_file.open("x") as stream:
            os.chmod(password_file, 0o600)
            stream.write(secrets.token_urlsafe(32))
    if password_file.stat().st_mode & 0o077:
        raise PermissionError(f"signing password is accessible by other users: {password_file}")
    environment = {**os.environ, "BASALT_743_DEV_PASSWORD": password_file.read_text().strip()}
    if not keystore.exists():
        run("keytool", "-genkeypair", "-keystore", keystore, "-alias", "basalt743dev",
            "-keyalg", "RSA", "-keysize", "3072", "-validity", "10000",
            "-storepass:env", "BASALT_743_DEV_PASSWORD", "-keypass:env", "BASALT_743_DEV_PASSWORD",
            "-dname", "CN=Basalt local development, O=Basalt, C=AU", env=environment)
        os.chmod(keystore, 0o600)
    return keystore, environment


def signature_metadata(name: str) -> bool:
    if name == "stamp-cert-sha256":
        return True
    if not name.startswith("META-INF/"):
        return False
    leaf = name[len("META-INF/"):]
    return "/" not in leaf and (leaf.upper() == "MANIFEST.MF" or
                                leaf.upper().endswith((".SF", ".RSA", ".DSA", ".EC")))


def build(source: Path, library: Path, output: Path, *,
          abi: str = "arm64-v8a", engine_split: Path | None = None,
          source_sha256: str = EXPECTED_SOURCE_SHA256,
          engine_build_id: bytes = EXPECTED_ENGINE_BUILD_ID,
          soname: str = "basalt", texture_dir: str = "basalt",
            startup_prompts: bool = True, show_startup_dialogs: bool = True,
            scoped_editor_limit: bool = False) -> None:
    if abi not in ("arm64-v8a", "x86_64"):
        raise ValueError(f"unsupported ABI: {abi}")
    if digest(source) != source_sha256:
        raise ValueError("APK does not match the pinned input")
    if not library.is_file():
        raise FileNotFoundError(library)
    library_bytes = library.read_bytes()[:20]
    machine = 183 if abi == "arm64-v8a" else 62
    if library_bytes[:4] != b"\x7fELF" or library_bytes[4] != 2 or int.from_bytes(library_bytes[18:20], "little") != machine:
        raise ValueError("executor library architecture does not match the APK ABI")
    library_name = f"lib/{abi}/lib{soname}.so"
    with zipfile.ZipFile(source) as apk:
        if engine_split is None:
            engine = apk.read(f"lib/{abi}/libroblox.so")
        else:
            with zipfile.ZipFile(engine_split) as split:
                engine = split.read(f"lib/{abi}/libroblox.so")
        if engine_build_id not in engine[:8192]:
            raise ValueError("APK contains an unexpected engine build ID")
        if library_name in apk.namelist():
            raise ValueError("input APK already contains the executor library")
        with tempfile.TemporaryDirectory(prefix="basalt743-", dir=WORK) as temp:
            stage = Path(temp)
            original = stage / "classes2.dex"
            original.write_bytes(apk.read("classes2.dex"))
            smali = stage / "smali"
            run("baksmali", "disassemble", "--api", "35", "--jobs", "4", "--output", smali, original)
            if scoped_editor_limit:
                keyboard = smali / "com/roblox/client/game/b.smali"
                body = keyboard.read_text()
                method = ".method public b(JZLjava/lang/String;Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)V"
                start = body.index(method)
                end = body.index(".end method", start)
                segment = body[start:end]
                anchor = "    const v2, 0xfa00\n\n    invoke-direct {v1, v2}, Landroid/text/InputFilter$LengthFilter;-><init>(I)V"
                if body.count(method) != 1 or segment.count(anchor) != 1:
                    raise ValueError("2.744 keyboard input limit layout changed")
                segment = segment.replace(anchor,
                    "    invoke-static {p5}, Lcom/roblox/client/game/b;->basaltEditorLimit(Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)I\n\n"
                    "    move-result v2\n\n    invoke-direct {v1, v2}, Landroid/text/InputFilter$LengthFilter;-><init>(I)V")
                helper = """.method private static basaltEditorLimit(Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)I
    .registers 3
    if-eqz p0, :standard_limit
    iget-boolean v0, p0, Lcom/roblox/engine/jni/model/NativeTextBoxInfo;->multiline:Z
    if-eqz v0, :standard_limit
    iget v0, p0, Lcom/roblox/engine/jni/model/NativeTextBoxInfo;->font:I
    const/16 v1, 0xa
    if-ne v0, v1, :standard_limit
    iget v0, p0, Lcom/roblox/engine/jni/model/NativeTextBoxInfo;->textColor:I
    ushr-int/lit8 v0, v0, 0x18
    if-nez v0, :standard_limit
    const v0, 0x800000
    return v0
    :standard_limit
    const v0, 0xfa00
    return v0
.end method

"""
                keyboard.write_text(body[:start] + segment + body[end:] + "\n" + helper)
            startup = smali / STARTUP
            content = startup.read_text()
            anchor = "    invoke-static {v1}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V"
            if content.count(anchor) != 1:
                raise ValueError("startup library load is not unique in the target DEX")
            content = content.replace(anchor, anchor + "\n\n    const-string v1, \"" + soname + "\"\n\n" + anchor, 1)
            startup.write_text(content)
            helper_files = sorted(HELPERS.glob("Basalt*.smali"))
            if not startup_prompts:
                helper_files = [file for file in helper_files
                                if file.name.startswith("BasaltHttp")]
            if len(helper_files) < (9 if startup_prompts else 3):
                raise ValueError("Android helper smali sources are incomplete")
            for helper in helper_files:
                target = smali / STARTUP.parent / helper.name
                if target.exists():
                    raise ValueError(f"helper already exists: {helper.name}")
                shutil.copy2(helper, target)
            if startup_prompts and show_startup_dialogs:
                # Restore the previous release's startup prompts: the shared
                # workspace explanation, then the Discord update notice.
                updates = smali / STARTUP.parent / "BasaltUpdates.smali"
                body = updates.read_text()
                guard = "    sget-boolean v0, Lcom/roblox/client/startup/BasaltUpdates;->shownThisProcess:Z"
                if "BasaltWorkspaceAccess;->showOnce" not in body:
                    if body.count(guard) != 1:
                        raise ValueError("unexpected BasaltUpdates showOnce layout")
                    body = body.replace(guard,
                        "    invoke-static {p0}, Lcom/roblox/client/startup/BasaltWorkspaceAccess;->showOnce(Landroid/app/Activity;)Z\n\n"
                        "    move-result v0\n\n"
                        "    if-nez v0, :done\n\n" + guard, 1)
                    updates.write_text(body)
                call = "    invoke-static {p0}, Lcom/roblox/client/startup/BasaltUpdates;->showOnce(Landroid/app/Activity;)V"
                # The home screen and in-experience view run in ActivityNativeMain on
                # 2.743; the previous release hooked both activities.
                for relative, resume in (
                    ("com/roblox/client/startup/MainGameActivity.smali",
                     "    invoke-super {p0}, Lcom/google/androidgamesdk/GameActivity;->onResume()V"),
                    ("com/roblox/client/ActivityNativeMain.smali",
                     "    invoke-super {p0}, Lcom/roblox/client/a;->onResume()V"),
                ):
                    activity = smali / relative
                    activity_body = activity.read_text()
                    if activity_body.count(resume) != 1:
                        raise ValueError(f"unexpected onResume layout: {relative}")
                    if call not in activity_body:
                        activity.write_text(activity_body.replace(resume, resume + "\n\n" + call, 1))
            patched = stage / "patched.dex"
            run("smali", "assemble", "--api", "35", "--jobs", "4", "--output", patched, smali)
            unsigned = stage / "unsigned.apk"
            manifest = public_storage_manifest(apk.read("AndroidManifest.xml"))
            with zipfile.ZipFile(unsigned, "w", allowZip64=True) as merged:
                for entry in apk.infolist():
                    name = entry.filename
                    if (signature_metadata(name) or
                            (name.startswith("lib/") and not name.startswith(f"lib/{abi}/"))):
                        continue
                    if name == "AndroidManifest.xml":
                        merged.writestr(entry, manifest)
                        continue
                    merged.writestr(entry, patched.read_bytes() if name == "classes2.dex" else apk.read(name))
                merged.write(library, library_name, compress_type=zipfile.ZIP_DEFLATED)
                for asset_name in (("glass-icons.png", "basalt-logo.png") if startup_prompts else ()):
                    merged.write(ROOT / "scripts/assets" / asset_name,
                                 "assets/content/textures/" + texture_dir + "/" + asset_name,
                                 compress_type=zipfile.ZIP_DEFLATED)
            aligned = stage / "aligned.apk"
            run("zipalign", "-f", "4", unsigned, aligned)
            keystore, environment = signing_credentials()
            output.parent.mkdir(parents=True, exist_ok=True)
            run("apksigner", "sign", "--v1-signing-enabled", "false", "--v2-signing-enabled", "true",
                "--v3-signing-enabled", "true", "--v4-signing-enabled", "false", "--ks", keystore,
                "--ks-key-alias", "basalt743dev", "--ks-pass", "env:BASALT_743_DEV_PASSWORD",
                "--key-pass", "env:BASALT_743_DEV_PASSWORD", "--out", output, aligned, env=environment)
    run("zipalign", "-c", "4", output)
    run("apksigner", "verify", "--verbose", output)
    print(f"Built {output} (SHA-256 {digest(output)})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--library", type=Path, default=ROOT / "native/libbasalt.so")
    parser.add_argument("--output", type=Path, default=WORK / "743/Basalt-2.743.1136-r6-dev.apk")
    args = parser.parse_args()
    build(args.source, args.library, args.output)
