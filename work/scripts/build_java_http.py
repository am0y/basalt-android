#!/usr/bin/env python3
"""Compile the isolated HTTP helper; dependencies are compile-only, never repackaged."""
import argparse
import hashlib
import os
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ARTIFACTS = {
    "android.jar": ("https://repo.maven.apache.org/maven2/com/google/android/android/4.1.1.4/android-4.1.1.4.jar", "84072541cbb711eff89f7277100ff854929a446dba7ceb1b195c340e0b4fd3cb"),
    "okhttp.jar": ("https://repo.maven.apache.org/maven2/com/squareup/okhttp3/okhttp/4.12.0/okhttp-4.12.0.jar", "b1050081b14bb7a3a7e55a4d3ef01b5dcfabc453b4573a4fc019767191d5f4e0"),
    "kotlin-stdlib.jar": ("https://repo.maven.apache.org/maven2/org/jetbrains/kotlin/kotlin-stdlib/1.8.21/kotlin-stdlib-1.8.21.jar", "042a1cd1ac976cdcfe5eb63f1d8e0b0b892c9248e15a69c8cfba495d546ea52a"),
    "json.jar": ("https://repo.maven.apache.org/maven2/org/json/json/20240303/json-20240303.jar", "3cf6cd6892e32e2b4c1c39e0f52f5248a2f5b37646fdfbb79a66b46b618414ed"),
    "okio.jar": ("https://repo.maven.apache.org/maven2/com/squareup/okio/okio-jvm/3.6.0/okio-jvm-3.6.0.jar", "67543f0736fc422ae927ed0e504b98bc5e269fda0d3500579337cb713da28412"),
    "r8.jar": ("https://dl.google.com/dl/android/maven2/com/android/tools/r8/8.3.37/r8-8.3.37.jar", "59753e70a74f918389cc87f1b7d66b5c0862932559167425708ded159e3de439"),
}


def verify_dependencies(directory):
    directory.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in ARTIFACTS.items():
        target = directory / name
        if target.is_file():
            actual = hashlib.sha256(target.read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError(f"unexpected SHA-256 for existing compile-only dependency: {target}")
            continue
        with urllib.request.urlopen(url, timeout=60) as source:
            content = source.read(32 * 1024 * 1024)
        if hashlib.sha256(content).hexdigest() != expected:
            raise ValueError(f"unexpected SHA-256 from dependency URL: {url}")
        target.write_bytes(content)


def build(destination, dependencies):
    verify_dependencies(dependencies)
    jars = [dependencies / name for name in ("android.jar", "okhttp.jar", "kotlin-stdlib.jar", "json.jar", "okio.jar")]
    for jar in [*jars, dependencies / "r8.jar"]:
        if not jar.is_file():
            raise FileNotFoundError(jar)
    with tempfile.TemporaryDirectory(prefix="basalt-http-java-") as directory:
        stage = Path(directory)
        classes, dex = stage / "classes", stage / "dex"
        classes.mkdir()
        dex.mkdir()
        subprocess.run(["javac", "--release", "8", "-g:none", "-cp", os.pathsep.join(map(str, jars)),
                        "-d", str(classes), str(ROOT / "native/android/BasaltHttp.java")], check=True)
        subprocess.run(["java", "-cp", str(dependencies / "r8.jar"), "com.android.tools.r8.D8", "--min-api", "28",
                        "--lib", str(jars[0]), "--output", str(dex),
                        *map(str, sorted(classes.rglob("*.class")))], check=True)
        subprocess.run(["baksmali", "disassemble", "--api", "28", "--output", str(stage / "smali"),
                        str(dex / "classes.dex")], check=True)
        destination.mkdir(parents=True, exist_ok=True)
        for source in (stage / "smali/com/roblox/client/startup").glob("BasaltHttp*.smali"):
            (destination / source.name).write_bytes(source.read_bytes())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path, default=Path("/tmp/opencode"))
    parser.add_argument("--output", type=Path, default=ROOT / "work/base_decoded/smali_classes2/com/roblox/client/startup")
    args = parser.parse_args()
    build(args.output, args.dependencies)
