from pathlib import Path

root = Path(__file__).resolve().parent
source = (root / "scheduler.luau").read_text()
for name in ("encoding", "extended", "websocket", "drawing", "signals", "properties", "mobile_ui", "instance_http", "console", "security", "android_compat", "lifecycle", "saveinstance", "input_compat", "actor_compat"):
    source = "local install_" + name + " = (function()\n" + (root / (name + ".luau")).read_text() + "\nend)()\n" + source
(root / "scheduler_bundled.luau").write_text(source)
