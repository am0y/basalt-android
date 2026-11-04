"""Exercise migration cleanup and the actual Java transport against local fixtures."""
import http.server
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from websockets.sync.server import serve

ROOT = Path(__file__).resolve().parents[1]
DEPS = Path(os.environ.get("BASALT_TEST_DEPS", "/tmp/opencode"))
JARS = tuple(DEPS / name for name in ("okhttp.jar", "kotlin-stdlib.jar", "okio.jar", "json.jar"))
if not DEPS.is_dir() or any(not path.is_file() for path in JARS):
    raise unittest.SkipTest("local Java transport test dependencies are unavailable")


class HTTP(http.server.BaseHTTPRequestHandler):
    websocket_port = 0
    def do_GET(self):
        assert not self.headers.get("Cookie") and not self.headers.get("Authorization")
        if self.path == "/websocket-redirect":
            self.send_response(301)
            self.send_header("Location", f"ws://127.0.0.1:{self.websocket_port}/")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Set-Cookie", "fixture=not-retained")
        self.end_headers()
        self.wfile.write(b"isolated-http-ok")

    def log_message(self, *args):
        pass


def echo(connection):
    assert not connection.request.headers.get("Cookie")
    for message in connection:
        connection.send(message)


with tempfile.TemporaryDirectory(prefix="basalt-hardening-", dir=DEPS) as directory:
    stage = Path(directory)
    source = stage / "cleanup.c"
    source.write_text('#include "legacy_diagnostics.h"\nint main(int n,char**v){if(n!=2)return 1;cleanup_legacy_diagnostics(v[1]);return 0;}')
    subprocess.run(["cc", "-D_GNU_SOURCE", "-I", str(ROOT / "native"), str(source),
                    str(ROOT / "native/legacy_diagnostics.c"), "-o", str(stage / "cleanup")], check=True)
    root = stage / "data"
    for name in ("in", "out", "workspace"):
        (root / name).mkdir(parents=True)
    (root / "in/queued.cmd").write_bytes(b"not executed")
    (root / "out/log.txt").write_text("private diagnostic output")
    (root / "workspace/draft.luau").write_text("preserve draft")
    outside = stage / "outside.txt"
    outside.write_text("preserve outside")
    (root / "out/link").symlink_to(outside)
    subprocess.run([str(stage / "cleanup"), str(root)], check=True)
    assert not (root / "in").exists() and not (root / "out").exists()
    assert (root / "workspace/draft.luau").read_text() == "preserve draft"
    assert outside.read_text() == "preserve outside"
    (root / "out").symlink_to(stage, target_is_directory=True)
    subprocess.run([str(stage / "cleanup"), str(root)], check=True)
    assert outside.read_text() == "preserve outside" and (root / "out").is_symlink()
    print("LEGACY_CLEANUP_OK: commands/logs removed, workspace and symlink targets preserved", flush=True)

    stub = stage / "android/util/Base64.java"
    stub.parent.mkdir(parents=True)
    stub.write_text('package android.util; public class Base64 { public static final int DEFAULT=0,NO_WRAP=2; '
                    'public static byte[] decode(String s,int f){return java.util.Base64.getDecoder().decode(s);} '
                    'public static String encodeToString(byte[] b,int f){return java.util.Base64.getEncoder().encodeToString(b);} }')
    classpath = os.pathsep.join(map(str, [stage, *JARS]))
    subprocess.run(["javac", "--release", "8", "-cp", classpath, "-d", str(stage), str(stub),
                    str(ROOT / "native/android/BasaltHttp.java"), str(ROOT / "tests/TransportHardeningTest.java")], check=True)
    http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HTTP)
    threading.Thread(target=http.serve_forever, daemon=True).start()
    try:
        with serve(echo, "127.0.0.1", 0) as websocket:
            HTTP.websocket_port = websocket.socket.getsockname()[1]
            threading.Thread(target=websocket.serve_forever, daemon=True).start()
            subprocess.run(["java", "-ea", "-cp", classpath, "TransportHardeningTest",
                            str(http.server_port), str(websocket.socket.getsockname()[1])], check=True, timeout=45)
    finally:
        http.shutdown()
        http.server_close()
