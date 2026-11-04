# Building Basalt Android

## Native build (ARM64 Roblox 2.744.1081)

This is the part you can build directly from a source checkout. You need
Linux x86_64, Git, Python 3, CMake, a host C/C++ toolchain, and Android NDK
r28b. The native Makefile defaults to API 29 and expects the upstream Luau
submodule. In a fresh clone:

```sh
git submodule update --init --recursive
export ANDROID_NDK_HOME=/path/to/android-ndk-r28b
make -C native MODE=release ARM_ENGINE=744 ARM_FULL=1 -j4
```

The output is `native/libbasalt-744-full.so`. For a local diagnostic build use
`MODE=development` with the same `ARM_ENGINE` and `ARM_FULL` flags; its output
is `native/libbasalt-744-full-dev.so`. Do not distribute the development
library as a release.

`ARM_ENGINE=743` selects the older ARM64 port; `ABI=x86_64 ARM_ENGINE=744`
builds a separate headless proof of concept. The Makefile defaults to the old
ARM64 engine when flags are omitted, so set them explicitly.

## APK inputs and limitations

You need the **matching** Roblox 2.744.1081 base and ARM64 split APKs, plus
locally generated Java/DEX startup helpers and a signing identity. These are
not in this repository. The 2.744 packaging script
`work/scripts/build_744_arm64_apk.py` checks known input digests and the
engine build ID; it expects local integration work before it can package an
APK. It outputs a base APK and a split bundle, **not** a working single-file
APK for phone installers. Do not rename a split bundle to `.apk`.

The existing standalone manifest transformer lives in
`work/scripts/build_phone_apk.py` in the earlier, version-pinned builder; it
is not wired into a supported 2.744 standalone build command here. In
particular, a fused base must remove the original `requiredSplitTypes` and
`com.android.vending.splits.required` declarations before signing. Merely
copying the native library into a base APK leaves Android expecting a split.

We will add a tested, source-only 2.744 packaging path after the editor and
phone install are verified. An APK that passes `apksigner verify` can still
fail at install or fail to show the editor. Never claim an official Roblox
signature; locally signed builds use a different certificate and cannot update
an installation signed by another key.

## Offline verification

```sh
python3 -m pip install -r requirements-dev.txt
python3 -m unittest discover -s tests -p '*_test.py'
cc -std=c11 -Wall -Wextra -Werror native/protocol_smoke.c -o /tmp/basalt-protocol-smoke
/tmp/basalt-protocol-smoke
```

Device-level tests require a compatible installed client and a development
runtime. `scripts/execute.py` talks to that local development channel through
ADB; it is not enabled in `MODE=release` builds. Test commands and results
should always include the client build and the actual ABI.
