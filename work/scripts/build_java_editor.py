"""Compile the code-input bridge against compile-only Android stubs."""
import subprocess
import tempfile
from pathlib import Path
from build_java_http import verify_dependencies

ROOT = Path(__file__).resolve().parents[2]
DEPS = Path("/tmp/opencode")
verify_dependencies(DEPS)
with tempfile.TemporaryDirectory(prefix="basalt-input-java-") as directory:
    stage = Path(directory)
    classes, dex = stage / "classes", stage / "dex"
    classes.mkdir(); dex.mkdir()
    subprocess.run(["javac", "--release", "8", "-g:none", "-cp", str(DEPS / "android.jar"), "-d", str(classes),
                    str(ROOT / "native/android/BasaltEditorInput.java")], check=True)
    subprocess.run(["java", "-cp", str(DEPS / "r8.jar"), "com.android.tools.r8.D8", "--min-api", "28",
                    "--lib", str(DEPS / "android.jar"), "--output", str(dex),
                    str(classes / "com/roblox/client/startup/BasaltEditorInput.class")], check=True)
    subprocess.run(["baksmali", "disassemble", "--api", "28", "--output", str(stage / "smali"), str(dex / "classes.dex")], check=True)
    relative = Path("com/roblox/client/startup/BasaltEditorInput.smali")
    (ROOT / "work/base_decoded/smali_classes2" / relative).write_bytes((stage / "smali" / relative).read_bytes())
