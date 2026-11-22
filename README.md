# WebAssembly Micro Runtime for QNX

This is an unofficial fork of [WebAssembly Micro Runtime (WAMR)](https://github.com/wasm-micro-runtime/wasm-micro-runtime) that adds support for QNX Neutrino 7.1 and 8.0 on AArch64. It is not affiliated with or endorsed by the WAMR project, the Bytecode Alliance or BlackBerry. For everything else about WAMR, see the [upstream README](https://github.com/wasm-micro-runtime/wasm-micro-runtime/blob/main/README.md) and the [WAMR guide](https://wamr.gitbook.io/).

## Why this fork

**WebAssembly for QNX 7.1 and 8.0 on AArch64, the platform behind many cars, robots, medical devices and industrial machines.**

- **Both current QNX releases.** One runtime and one set of modules for QNX 7.1, which many of today's production systems are built on, and QNX 8.0, the next generation.
- **Native AArch64 performance.** Built for the Arm SoCs used across these industries. Modules are compiled ahead of time to native AArch64 code for near-native speed, or run in WAMR's fast interpreter.
- **Software that outlives its hardware.** Vehicles, medical devices and industrial machines stay in service for 10 to 20 years, while SoCs and OS releases change every few years. Applications compiled to WebAssembly don't depend on the CPU, the OS release or its toolchain: moving to new hardware means porting the runtime once, and existing modules carry over unchanged. WebAssembly is a W3C standard designed to stay backward compatible, so modules built today keep running on future runtimes.
- **Builds anywhere.** The runtime is built with the public Arm GNU Toolchain and [relibc's QNX port](https://github.com/NikeKhin/relibc-qnx), without the QNX SDP, so it fits into any CI on Linux or macOS.
- **Write once, run on the device, in simulation and in the cloud.** Applications are written in C, C++, Rust or other languages and compiled with standard WebAssembly toolchains; no QNX toolchain is needed. The same module behaves the same everywhere, so an issue seen in the field can be reproduced on a desk.
- **Small, independent updates.** Each module is a self-contained file that can be signed, delivered and replaced on its own, so an update touches only what changed instead of the whole system image.
- **Sandboxed by design.** A module sees only its own memory and the functions the host grants it, with no file, network or OS access unless given. Code from suppliers, partners or customers can run alongside critical functions without access to their memory.
- **Self-healing systems.** [wamr-actors](wamr-actors/README.md) adds mailboxes, supervisors, CPU budgets and hot upgrade: a failing module is restarted, and a new version replaces a running one without a reboot.

## What this fork adds

| Path | Contents |
| --- | --- |
| [`core/shared/platform/qnx`](core/shared/platform/qnx) | QNX platform layer |
| [`product-mini/platforms/qnx`](product-mini/platforms/qnx) | `iwasm` build for QNX and the CMake toolchain file for relibc |
| [`wamr-actors`](wamr-actors) | Actor runtime on top of WAMR |
| [`doc/qnx_actors_notes.md`](doc/qnx_actors_notes.md) | Implementation notes and use cases |
| `core/shared/platform/common/posix` | Small changes for QNX, and an `os_ioctl()` fix that applies to every platform |

## Branches

- `qnx/main` (default): the QNX port, kept rebased on upstream.
- `main`: an unmodified mirror of upstream `main`.

## Building

You need:
- an `aarch64-none-elf` GCC, such as the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) 15.2;
- a checkout of [relibc-qnx](https://github.com/NikeKhin/relibc-qnx), which builds with a Rust nightly toolchain and cbindgen.

Build relibc for the QNX release, then iwasm against it:

``` Bash
cd <relibc>
qnx/build-qnx.sh                                                    # QNX 7.1
TARGET_SPEC=targets/aarch64-unknown-nto-qnx800.json qnx/build-qnx.sh  # QNX 8.0

cd <wamr>/product-mini/platforms/qnx
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/relibc_toolchain.cmake \
      -DRELIBC_QNX_DIR=<relibc> -DQNX_VERSION=7.1   # or 8.0
cmake --build build
```

The result is a statically linked `iwasm`. See [product-mini/README.md](product-mini/README.md#qnx) for details, including the AOT compiler options QNX needs and the `PROCMGR_AID_PROT_EXEC` ability required to load AOT modules.

## Status

- iwasm builds and links for QNX 7.1 and 8.0.
- On-target testing is in progress.
- Hardware bound checks cover linear memory only, so AOT modules must be compiled with `wamrc --stack-bounds-checks=1`.

## License

WAMR's license applies to this fork: Apache 2.0 with the LLVM exception, see [LICENSE](LICENSE). relibc is MIT-licensed.

QNX is a trademark of BlackBerry Limited. All other trademarks belong to their respective owners.
