# How the source fits together

The Android client loads Basalt's native library. `native/engine.c` pins VM
entry points to a particular client build; `native/executor.c` creates the
execution environment, runs the scheduler, and starts the editor. The editor
itself lives in `scripts/editor.luau` and its modules; `native/bundle_editor.py`
embeds them during the build. `native/bundle_scheduler.py` does the same for
the Luau runtime helpers under `native/`.

`native/vm_api.c` provides VM-facing APIs, `native/workspace.c` handles files,
and the Android helpers under `native/android/` handle device-specific work
such as networking and editor input. The x86_64 path in `native/x86_executor.c`
has a different feature set and no in-game editor.

This code relies on build-specific layouts and offsets. When Roblox changes,
check the engine version and VM structures before changing an address. A
successful compile says nothing about whether callbacks, thread capabilities,
or UI initialization work in a joined game. The current verified and missing
checks are listed in [STATUS.md](../STATUS.md).
