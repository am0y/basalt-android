<div align="center">

<img src="scripts/assets/basalt-logo.png" width="88" alt="Basalt logo">

# Basalt Android

**Open-source Roblox executor and Luau runtime for Android**

<p>
  <a href="https://github.com/am0y/basalt-android/actions/workflows/checks.yml">
    <img src="https://github.com/am0y/basalt-android/actions/workflows/checks.yml/badge.svg?branch=main" alt="Build checks">
  </a>
  <a href="https://github.com/am0y/basalt-android/blob/main/LICENSE">
    <img src="https://img.shields.io/badge/license-MIT-7c3aed.svg" alt="MIT license">
  </a>
  <img src="https://img.shields.io/badge/Android-API%2029%2B-3ddc84.svg" alt="Android API 29 or newer">
  <img src="https://img.shields.io/badge/Roblox-2.744.1081-00a2ff.svg" alt="Roblox 2.744.1081">
</p>

<p>
  <a href="#build">Build</a> ·
  <a href="#features">Features</a> ·
  <a href="STATUS.md">Status</a> ·
  <a href="CONTRIBUTING.md">Contributing</a>
</p>

</div>

Basalt targets Roblox 2.744.1081 on ARM64. This repository contains the
native runtime, Luau integration, Android helpers, editor, and test tools.

## Features

- Luau compilation and execution
- In-game editor and console
- Saved scripts and autoexecute
- Workspace and file APIs
- Android HTTP and WebSocket helpers
- Input, drawing, signal, and prompt helpers
- Version-pinned VM and engine integration
- Release and development build modes

The core editor has four pages: Code, Console, Saved Scripts, and Settings.
The optional Script Hub and AI features are not included in this repository.

## Build

### Requirements

- Linux x86_64
- Python 3
- CMake
- A C/C++ toolchain
- Android NDK r28b
- Git with submodules enabled

### Native library

```sh
git clone --recurse-submodules https://github.com/am0y/basalt-android.git
cd basalt-android
export ANDROID_NDK_HOME=/path/to/android-ndk-r28b
make -C native MODE=release ARM_ENGINE=744 ARM_FULL=1 -j4
```

The output is `native/libbasalt-744-full.so` for `arm64-v8a` and Android API
29 or newer. The build generates the embedded scheduler and editor bytecode.

APK packaging needs matching Roblox client files, local Java/DEX integration,
and a signing key. Those files are intentionally excluded. See
[BUILDING.md](BUILDING.md) for the packaging notes.

## Tests

Run the host checks without an Android device:

```sh
python3 -m pip install -r requirements-dev.txt
python3 -m unittest discover -s tests -p '*_test.py'
cc -std=c11 -Wall -Wextra -Werror native/protocol_smoke.c -o /tmp/basalt-protocol-smoke
/tmp/basalt-protocol-smoke
```

Device tests require a matching client and a development build. Test results
depend on the Roblox version, Android version, and ABI. Current port notes are
in [STATUS.md](STATUS.md).

## Repository layout

| Path | Purpose |
| --- | --- |
| `native/` | Runtime, VM integration, Android code, and Luau helpers |
| `scripts/` | Editor source, assets, and local utilities |
| `tests/` | Host tests and device test scripts |
| `work/scripts/` | Version-specific packaging tools |
| `luau/` | Pinned Luau submodule |

## Contributing

Bug reports, porting work, documentation fixes, and focused pull requests are
welcome. Include the client build, Android version, ABI, and reproduction
steps. Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## License

Basalt source is available under the [MIT License](LICENSE). Luau and the
included icon assets retain their upstream licenses. See [NOTICE.md](NOTICE.md)
for attribution.
