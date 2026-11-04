"""Compile the one-time storage-access prompt; Android stubs are compile-only."""
import subprocess
import tempfile
from pathlib import Path
from build_java_http import verify_dependencies

ROOT = Path(__file__).resolve().parents[2]
DEPS = Path("/tmp/opencode")
verify_dependencies(DEPS)
with tempfile.TemporaryDirectory(prefix="basalt-workspace-java-") as directory:
    stage = Path(directory)
    classes, dex = stage / "classes", stage / "dex"
    classes.mkdir(); dex.mkdir()
    subprocess.run(["javac", "--release", "8", "-g:none", "-cp", str(DEPS / "android.jar"), "-d", str(classes),
                    str(ROOT / "native/android/BasaltWorkspaceAccess.java"),
                    str(ROOT / "native/android/BasaltInputSimulation.java")], check=True)
    subprocess.run(["java", "-cp", str(DEPS / "r8.jar"), "com.android.tools.r8.D8", "--min-api", "28",
                    "--lib", str(DEPS / "android.jar"), "--output", str(dex),
                    *map(str, sorted((classes / "com/roblox/client/startup").glob("*.class")))], check=True)
    subprocess.run(["baksmali", "disassemble", "--api", "28", "--output", str(stage / "smali"), str(dex / "classes.dex")], check=True)
    for relative in (stage / "smali/com/roblox/client/startup").glob("Basalt*.smali"):
        target = ROOT / "work/base_decoded/smali_classes2/com/roblox/client/startup" / relative.name
        target.write_bytes(relative.read_bytes())
