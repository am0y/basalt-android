#!/usr/bin/env python3
"""Fuse a pinned, ABI-matched Roblox split into a signed standalone APK."""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import tempfile
import zipfile
from dataclasses import dataclass, replace
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "work"
BASE = WORK / "base_basalt_android10_helpers.apk"
SPLIT = WORK / "split_config.arm64_v8a.apk"
LIBRARY = ROOT / "native/libbasalt.so"
SIGNING_DIR = Path(os.environ.get("BASALT_SIGNING_KEY_DIR", ROOT / "keys"))
KEYSTORE = SIGNING_DIR / "basalt.keystore"
LEGACY_KEYSTORE = SIGNING_DIR / "rbxexec.keystore"
SIGNING_LINEAGE = SIGNING_DIR / "basalt-signing-lineage.bin"
MIN_SDK = int((ROOT / "native/android_api.txt").read_text().strip())
OUTPUT = WORK / "Basalt-1.2.1-optional-protection.apk"
LIBRARY_NAME = "lib/arm64-v8a/libbasalt.so"
SPLIT_META = "com.android.vending.splits.required"
SPLIT_ABI = "base__abi"
NDK_STRIP = Path.home() / "android-tools/ndk/android-ndk-r28b/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
EXPECTED_BASE_SHA256 = "114a6f8ed6f0f974cf632a9cbec2f5cd45e1d85d7ed251db4f3920140bc6ead4"
EXPECTED_SPLIT_SHA256 = "975d32ad874fa6b78d9a3efda2ee51c4a28c84fdd45e36b549a850e1569ab363"
EXPECTED_ENGINE_SHA256 = "38ec6854b61898994b325c817e1db943d7d0c0654815814430bf6b97cdc40cf3"


@dataclass(frozen=True)
class Target:
    abi: str
    split: Path
    engine: Path
    split_sha256: str
    engine_sha256: str
    library: Path

    @property
    def library_name(self) -> str:
        return f"lib/{self.abi}/libbasalt.so"

    @property
    def engine_name(self) -> str:
        return f"lib/{self.abi}/libroblox.so"


TARGETS = {
    "arm64-v8a": Target("arm64-v8a", SPLIT, WORK / "apk/lib/arm64-v8a/libroblox.so",
                           EXPECTED_SPLIT_SHA256, EXPECTED_ENGINE_SHA256, LIBRARY),
    "x86_64": Target("x86_64", WORK / "linux-x86/split_config.x86_64.apk",
                     WORK / "linux-x86/lib/libroblox.so",
                     "f42ace3b9428b30b4c06d28330937a2470cc13e7321130a5d27a4e29538b9356",
                     "3e498d649421115bdbebaa69f78e2b818d4351d8990db60e232d2aef5b741335",
                     ROOT / "native/libbasalt-x86_64.so"),
}


def signature_metadata(name: str) -> bool:
    if not name.startswith("META-INF/"):
        return False
    leaf = name[len("META-INF/"):]
    return "/" not in leaf and (leaf.upper() == "MANIFEST.MF" or
                                leaf.upper().endswith((".SF", ".RSA", ".DSA", ".EC")))


def run(*args: object) -> None:
    subprocess.run([str(arg) for arg in args], check=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_inputs(target: Target = TARGETS["arm64-v8a"]) -> None:
    for path, expected in ((WORK / "base.apk", EXPECTED_BASE_SHA256),
                           (target.split, target.split_sha256),
                           (target.engine, target.engine_sha256)):
        if sha256(path) != expected:
            raise ValueError(f"unsupported Roblox binary: {path}")
    with zipfile.ZipFile(target.split) as split:
        if hashlib.sha256(split.read(target.engine_name)).hexdigest() != target.engine_sha256:
            raise ValueError("split APK contains an unexpected libroblox.so")
    with zipfile.ZipFile(BASE) as base:
        with zipfile.ZipFile(WORK / "base.apk") as original:
            if base.read("resources.arsc") != original.read("resources.arsc"):
                raise ValueError("patched base resources do not match the pinned base")
            dex_names = {name for name in base.namelist() if name.endswith(".dex")}
            original_dex_names = {name for name in original.namelist() if name.endswith(".dex")}
            if dex_names != original_dex_names:
                raise ValueError("patched base DEX layout does not match the pinned base")
            shared = set(base.namelist()) & set(original.namelist())
            if any(base.getinfo(name).CRC != original.getinfo(name).CRC
                   for name in shared - dex_names):
                raise ValueError("patched base changes pinned non-DEX entries")
        startup = base.read("classes2.dex")
        if b"basalt" not in startup or b"rbxexec" in startup:
            raise ValueError("patched base does not load the executor library")
        if b"BasaltUpdates" not in startup or b"https://discord.gg/JQgHkZKap" not in startup:
            raise ValueError("patched base does not include the Basalt Discord invite prompt")
        if b"discord_invite_seen_v1" not in startup or b"basalt_startup" not in startup or b"Open Discord" not in startup:
            raise ValueError("patched base is missing the persistent one-time invite prompt")
        if b"releases:\nhttps://discord.gg/" in startup:
            raise ValueError("the startup dialog still displays the invite URL in its body")


def parse_string_pool(chunk: bytes) -> list[str]:
    kind, header, length = struct.unpack_from("<HHI", chunk)
    if kind != 0x0001 or header != 28 or length != len(chunk):
        raise ValueError("unexpected binary XML string pool")
    count, styles, flags, strings_at, _ = struct.unpack_from("<IIIII", chunk, 8)
    if styles or count > 100_000:
        raise ValueError("unsupported binary XML string pool")
    if flags & 0x100:
        def length8(offset: int) -> tuple[int, int]:
            first = chunk[offset]
            return (((first & 0x7f) << 8) | chunk[offset + 1], offset + 2) if first & 0x80 else (first, offset + 1)
        strings = []
        for i in range(count):
            position = strings_at + struct.unpack_from("<I", chunk, header + i * 4)[0]
            _, position = length8(position)
            size, position = length8(position)
            if position + size + 1 > len(chunk):
                raise ValueError("binary XML string exceeds pool bounds")
            strings.append(chunk[position:position + size].decode("utf-8"))
        return strings
    def decode_length(offset: int) -> tuple[int, int]:
        value = struct.unpack_from("<H", chunk, offset)[0]
        if value & 0x8000:
            return ((value & 0x7FFF) << 16) | struct.unpack_from("<H", chunk, offset + 2)[0], offset + 4
        return value, offset + 2
    strings = []
    for i in range(count):
        position = strings_at + struct.unpack_from("<I", chunk, header + i * 4)[0]
        size, position = decode_length(position)
        if position + size * 2 + 2 > len(chunk):
            raise ValueError("binary XML string exceeds pool bounds")
        strings.append(chunk[position:position + size * 2].decode("utf-16le"))
    return strings


def string_pool(strings: list[str]) -> bytes:
    offsets, payload = [], bytearray()
    for text in strings:
        offsets.append(len(payload))
        encoded = text.encode("utf-16le")
        units = len(encoded) // 2
        payload += struct.pack("<H", units) if units < 0x8000 else struct.pack("<HH", 0x8000 | (units >> 16), units & 0xffff)
        payload += encoded + b"\0\0"
    while len(payload) % 4:
        payload.append(0)
    strings_at = 28 + 4 * len(strings)
    return struct.pack("<HHIIIIII", 0x0001, 28, strings_at + len(payload), len(strings), 0, 0, strings_at, 0) + \
        struct.pack("<" + "I" * len(offsets), *offsets) + payload


def http_network_config(data: bytes) -> bytes:
    """Allow explicit http:// requests while retaining the pinned system TLS trust anchors."""
    chunks, strings = [], []
    pool_index = None
    changed = 0
    cursor = 8
    while cursor < len(data):
        kind, header, size = struct.unpack_from("<HHI", data, cursor)
        if header < 8 or size < header or cursor + size > len(data):
            raise ValueError("invalid network config chunk")
        chunk = bytearray(data[cursor:cursor + size])
        if kind == 1:
            pool_index = len(chunks)
            strings = parse_string_pool(bytes(chunk))
        elif kind == 0x102:
            tag, start, stride, count = struct.unpack_from("<IHHH", chunk, 20)
            if strings[tag] == "base-config":
                if stride != 20:
                    raise ValueError("unexpected network attribute layout")
                attribute_name = "cleartextTrafficPermitted"
                existing = None
                for i in range(count):
                    at = 16 + start + i * stride
                    name = struct.unpack_from("<I", chunk, at + 4)[0]
                    if strings[name] == attribute_name:
                        existing = at
                if existing is None:
                    if attribute_name not in strings:
                        strings.append(attribute_name)
                    at = 16 + start + count * stride
                    attribute = struct.pack("<IIIHBBI", 0xffffffff, strings.index(attribute_name), 0xffffffff, 8, 0, 0x12, 0xffffffff)
                    chunk[at:at] = attribute
                    struct.pack_into("<I", chunk, 4, len(chunk))
                    struct.pack_into("<H", chunk, 28, count + 1)
                else:
                    chunk[existing + 15] = 0x12
                    struct.pack_into("<I", chunk, existing + 8, 0xffffffff)
                    struct.pack_into("<I", chunk, existing + 16, 0xffffffff)
                changed += 1
        chunks.append(bytes(chunk))
        cursor += size
    if pool_index is None or changed != 1:
        raise ValueError("expected one base network configuration")
    chunks[pool_index] = string_pool(strings)
    body = b"".join(chunks)
    return struct.pack("<HHI", 3, 8, len(body) + 8) + body


def public_storage_manifest(data: bytes) -> bytes:
    """Declare the Android 11+ access used by Basalt's explicit Storage action."""
    permission = "android.permission.MANAGE_EXTERNAL_STORAGE"
    chunks = []
    strings = []
    pool_index = application_index = None
    cursor = 8
    already_declared = False
    while cursor < len(data):
        kind, header, size = struct.unpack_from("<HHI", data, cursor)
        if header < 8 or size < header or cursor + size > len(data):
            raise ValueError("invalid manifest chunk")
        chunk = data[cursor:cursor + size]
        if kind == 0x0001:
            pool_index = len(chunks)
            strings = parse_string_pool(chunk)
        elif kind == 0x0102:
            tag, start, stride, count = struct.unpack_from("<IHHH", chunk, 20)
            if strings[tag] == "application":
                application_index = len(chunks)
            if strings[tag] == "uses-permission":
                for i in range(count):
                    at = 16 + start + i * stride
                    _, name, raw, _, _, value_type, value = struct.unpack_from("<IIIHBBI", chunk, at)
                    if strings[name] == "name" and value_type == 3 and strings[value] == permission:
                        already_declared = True
        chunks.append(chunk)
        cursor += size
    if already_declared:
        return data
    if pool_index is None or application_index is None:
        raise ValueError("manifest string pool or application is missing")

    permission_index = len(strings)
    strings.append(permission)
    chunks[pool_index] = string_pool(strings)

    no_index = 0xffffffff
    tag = strings.index("uses-permission")
    namespace = strings.index("http://schemas.android.com/apk/res/android")
    name = strings.index("name")
    node = struct.pack("<HHIII", 0x0102, 16, 56, 1, no_index)
    node += struct.pack("<IIHHHHHH", no_index, tag, 20, 20, 1, 0, 0, 0)
    node += struct.pack("<IIIHBBI", namespace, name, permission_index, 8, 0, 3, permission_index)
    end = struct.pack("<HHIIIII", 0x0103, 16, 24, 1, no_index, no_index, tag)
    chunks[application_index:application_index] = [node, end]
    body = b"".join(chunks)
    return struct.pack("<HHI", 0x0003, 8, len(body) + 8) + body


def standalone_manifest(data: bytes) -> bytes:
    document = bytearray(data)
    if struct.unpack_from("<H", document)[0] != 0x0003 or len(document) != struct.unpack_from("<I", document, 4)[0]:
        raise ValueError("unexpected binary AndroidManifest format")
    cursor = 8
    strings = []
    metadata = 0
    required_split_types = 0
    minimum_sdk = 0
    while cursor < len(document):
        chunk_type, header, size = struct.unpack_from("<HHI", document, cursor)
        if not size or header < 8 or cursor + size > len(document):
            raise ValueError("invalid manifest chunk size")
        chunk = document[cursor:cursor + size]
        if chunk_type == 0x0001:
            strings = parse_string_pool(bytes(chunk))
        elif chunk_type == 0x0102:  # START_ELEMENT
            tag_index, attribute_start, attribute_size, attribute_count = struct.unpack_from("<IHHH", chunk, 20)
            tag = strings[tag_index]
            attrs = cursor + 16 + attribute_start
            if attribute_size != 20 or attrs + attribute_count * 20 > cursor + size:
                raise ValueError("unexpected attribute structure")
            attr_by_name = {}
            for i in range(attribute_count):
                at = attrs + i * 20
                _, name, raw, _, _, _, value = struct.unpack_from("<IIIHBBI", document, at)
                attr_by_name[strings[name]] = (at, raw, value)
            if tag == "uses-sdk" and "minSdkVersion" in attr_by_name:
                at, _, _ = attr_by_name["minSdkVersion"]
                if document[at + 15] != 0x10:
                    raise ValueError("unexpected minimum SDK representation")
                struct.pack_into("<I", document, at + 8, 0xffffffff)
                struct.pack_into("<I", document, at + 16, MIN_SDK)
                minimum_sdk += 1
            if tag == "meta-data" and "name" in attr_by_name:
                _, raw, value = attr_by_name["name"]
                name = strings[raw if raw != 0xFFFFFFFF else value]
                if name == SPLIT_META:
                    target = attr_by_name.get("value")
                    if not target:
                        raise ValueError("split-required metadata has no value")
                    document[target[0] + 15] = 0x12  # TYPE_INT_BOOLEAN
                    struct.pack_into("<I", document, target[0] + 16, 0)
                    metadata += 1
            if tag == "manifest" and "requiredSplitTypes" in attr_by_name:
                at, raw, value = attr_by_name["requiredSplitTypes"]
                _, _, _, _, _, kind, _ = struct.unpack_from("<IIIHBBI", document, at)
                if kind != 3 or raw >= len(strings) or value >= len(strings) or \
                        strings[raw] != SPLIT_ABI or strings[value] != SPLIT_ABI:
                    raise ValueError("unexpected requiredSplitTypes in base manifest")
                try:
                    empty = strings.index("")
                except ValueError as exc:
                    raise ValueError("manifest has no empty string for clearing requiredSplitTypes") from exc
                struct.pack_into("<I", document, at + 8, empty)  # raw string index
                struct.pack_into("<I", document, at + 16, empty)  # typed string index
                required_split_types += 1
            if tag == "manifest" and "splitTypes" in attr_by_name:
                at, _, _ = attr_by_name["splitTypes"]
                _, _, _, _, _, kind, value = struct.unpack_from("<IIIHBBI", document, at)
                if kind == 3:
                    text = strings[value]
                    if text:
                        raise ValueError("the base manifest has a non-empty split type")
        cursor += size
    if metadata != 1:
        raise ValueError(f"expected one split-required metadata value; found {metadata}")
    if minimum_sdk != 1:
        raise ValueError(f"expected one minimum SDK attribute; found {minimum_sdk}")
    if required_split_types != 1:
        raise ValueError(f"expected one requiredSplitTypes attribute; found {required_split_types}")
    if cursor != len(document):
        raise ValueError("malformed manifest chunks")
    return public_storage_manifest(bytes(document))


def fuse(output: Path, password_file: Path, legacy_password_file: Path = SIGNING_DIR / "signing-password.txt",
         target: Target = TARGETS["arm64-v8a"], probe: bool = False, development: bool = False) -> None:
    if not all(p.is_file() for p in (BASE, target.split, target.library, KEYSTORE, LEGACY_KEYSTORE, SIGNING_LINEAGE,
                                     password_file, legacy_password_file, NDK_STRIP)):
        raise FileNotFoundError(f"build the {target.abi} libbasalt.so and provide the pinned APKs and signing files")
    password_file = password_file.resolve()
    legacy_password_file = legacy_password_file.resolve()
    for secret in (password_file, legacy_password_file, KEYSTORE, LEGACY_KEYSTORE, SIGNING_LINEAGE):
        if secret.stat().st_mode & 0o077:
            raise PermissionError("signing material must only be readable by its owner (chmod 600)")
    if output.exists():
        raise FileExistsError(f"output already exists: {output}")
    verify_inputs(target)
    native = target.library.read_bytes()
    if probe:
        if (target.abi != "x86_64" or b"BASALT_X86_64_PROBE_ONLY_V1" not in native or
                output.parent.resolve() != (WORK / "linux-x86").resolve() or not output.name.endswith("-probe.apk")):
            raise ValueError("x86 probe APKs are restricted to the local, clearly marked linux-x86 directory")
    elif development:
        if output.parent.resolve() != WORK.resolve() or not output.name.endswith("-dev.apk"):
            raise ValueError("development APKs must be named *-dev.apk in the local work directory")
        if b"BASALT_DEVELOPMENT_BUILD_V1" not in native or b"BASALT_RELEASE_NO_EXTERNAL_COMMANDS_V1" in native:
            raise ValueError("development APK requires a development native library")
    else:
        if b"BASALT_RELEASE_NO_EXTERNAL_COMMANDS_V1" not in native or b"BASALT_DEVELOPMENT_BUILD_V1" in native:
            raise ValueError("refusing to package a development native library as a public release")
        for marker in (b"request-log protocol", b"EXEC_BIN", b"10.0.2.2", b"/out/session.txt", b"_probeMemoryHex"):
            if marker in native:
                raise ValueError(f"release native library still contains development ingress: {marker!r}")
    with tempfile.TemporaryDirectory(prefix="basalt-phone-", dir=WORK) as tmp:
        stage = Path(tmp)
        unsigned = stage / "unsigned.apk"
        aligned = stage / "aligned.apk"
        stripped = stage / "libbasalt.so"
        run(NDK_STRIP, "--strip-all", "-o", stripped, target.library)
        with zipfile.ZipFile(BASE) as base, zipfile.ZipFile(target.split) as split, zipfile.ZipFile(unsigned, "w", allowZip64=True) as merged:
            existing = set()
            expected_services = {entry.filename for entry in base.infolist() if entry.filename.startswith("META-INF/services/")}
            for entry in base.infolist():
                if signature_metadata(entry.filename) or entry.filename == "stamp-cert-sha256":
                    continue
                payload = base.read(entry.filename)
                if entry.filename == "AndroidManifest.xml":
                    payload = standalone_manifest(payload)
                elif entry.filename == "res/xml/network_security_config.xml":
                    payload = http_network_config(payload)
                merged.writestr(entry, payload)
                existing.add(entry.filename)
            for entry in split.infolist():
                if not entry.filename.startswith(f"lib/{target.abi}/") or entry.filename.endswith("/"):
                    continue
                if entry.filename in existing:
                    raise ValueError(f"duplicate native library: {entry.filename}")
                payload = stripped.read_bytes() if entry.filename == target.library_name else split.read(entry.filename)
                merged.writestr(entry, payload)
                existing.add(entry.filename)
            if target.library_name not in existing:
                entry = zipfile.ZipInfo(target.library_name)
                entry.compress_type = zipfile.ZIP_STORED
                merged.writestr(entry, stripped.read_bytes())
            for filename in ("glass-icons.png", "basalt-logo.png"):
                asset = "assets/content/textures/basalt/" + filename
                if asset in existing:
                    raise ValueError(f"duplicate bundled UI asset: {asset}")
                merged.write(ROOT / "scripts/assets" / filename, asset, compress_type=zipfile.ZIP_DEFLATED)
                existing.add(asset)
            if not expected_services.issubset(existing):
                raise ValueError("fused APK dropped Java ServiceLoader providers")
        run("zipalign", "-f", "4", unsigned, aligned)
        run("apksigner", "sign", "--v1-signing-enabled", "false", "--v2-signing-enabled", "true",
            "--v3-signing-enabled", "true", "--v4-signing-enabled", "false", "--debuggable-apk-permitted", "false",
            "--lineage", SIGNING_LINEAGE, "--rotation-min-sdk-version", "28",
            "--ks", LEGACY_KEYSTORE, "--ks-key-alias", "rbxexec", "--ks-pass", f"file:{legacy_password_file}",
            "--next-signer", "--ks", KEYSTORE, "--ks-key-alias", "basalt",
            "--ks-pass", f"file:{password_file}",
            "--out", output, aligned)
        run("zipalign", "-c", "4", output)
        run("apksigner", "verify", "--verbose", output)
    digest = sha256(output)
    print(f"Standalone APK: {output} ({output.stat().st_size / 1024 / 1024:.1f} MiB)")
    if probe:
        print("DIAGNOSTIC ONLY: no editor, script execution or user-facing x86 support in this APK")
    print(f"SHA-256: {digest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--abi", choices=TARGETS, default="arm64-v8a",
                        help="Android ABI to build; each requires its own independently verified native port")
    parser.add_argument("--probe", action="store_true", help="local x86 JNI-loading probe only; never a user build")
    parser.add_argument("--development", action="store_true", help="package a local *-dev.apk with the ADB testing bridge")
    parser.add_argument("--password-file", type=Path,
                        default=Path(os.environ.get("BASALT_KEY_PASSWORD_FILE", SIGNING_DIR / "basalt-signing-password.txt")))
    parser.add_argument("--legacy-password-file", type=Path,
                        default=Path(os.environ.get("RBXEXEC_KEY_PASSWORD_FILE", SIGNING_DIR / "signing-password.txt")))
    args = parser.parse_args()
    target = TARGETS[args.abi]
    if args.probe:
        target = replace(target, library=WORK / "linux-x86/libbasalt-probe.so")
    if args.development:
        if args.probe or args.abi != "arm64-v8a": parser.error("development packaging requires ARM64 without --probe")
        target = replace(target, library=ROOT / "native/libbasalt-dev.so")
    fuse(args.output, args.password_file, args.legacy_password_file, target, args.probe, args.development)
