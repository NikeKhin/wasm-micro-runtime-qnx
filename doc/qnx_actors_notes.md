# WAMR on QNX (relibc) + wamr-actors — implementation notes and robotics roadmap

## 1. What exists

### 1.1 QNX platform layer (no QNX SDP)
- **Target**: AArch64 QNX Neutrino 7.1 and 8.0.
- **Toolchain**: public Arm GNU Toolchain `aarch64-none-elf-gcc` 15.2 + relibc's QNX port as libc, static link.
  - `product-mini/platforms/qnx/relibc_toolchain.cmake`: `RELIBC_QNX_DIR`, `QNX_VERSION` (7.1 → `aarch64-unknown-nto-qnx710`, 8.0 → `...qnx800`); `-D__QNXNTO__ -nostdinc` with relibc headers before GCC's (newlib never used); `-mno-outline-atomics` (no libgcc LSE helpers); relibc linker script `targets/aarch64-qnx-static.ld` (keeps program headers mapped so relibc finds the TLS segment). Vars forwarded to `try_compile`.
  - relibc built per release by `qnx/build-qnx.sh` (cbindgen writes headers into the sysroot).
- **`core/shared/platform/qnx/`**: POSIX-based platform (`platform_internal.h`, `platform_init.c`, `shared_platform.cmake`) reusing `common/posix`.
- **Patches to common POSIX code** (all guarded, upstreamable):
  - `posix_memmap.c`: I-cache flush via `msync(MS_INVALIDATE_ICACHE)`, as with QNX's own libc (provided by relibc).
  - `posix_thread.c`: `pthread_getattr_np` path enabled for QNX; relibc reports the main thread's stack (kernel `__stackaddr` + start-up sp) and provides `pthread_setname_np`.
  - `posix_file.c`: `os_ioctl` passes `va_arg(args, void*)` instead of the `va_list` (real bug on all platforms).
- **iwasm build** (`product-mini/platforms/qnx`): fast interp + AOT, SIMD, ref-types, libc-builtin + WASI; JIT off; `WAMR_DISABLE_STACK_HW_BOUND_CHECK=1`.
- **Safety caveats** (documented in `product-mini/README.md#qnx`):
  - HW bound check covers linear memory only (no sigaltstack on 7.1 via relibc).
  - Compile AOT with `wamrc --target=aarch64 --stack-bounds-checks=1` (+ `--bounds-checks=1` if HW checks disabled).
  - Native stack overflow detected on all threads (needs updated relibc).
  - AOT load needs `PROCMGR_AID_PROT_EXEC` ability.

### 1.2 wamr-actors (BEAM-style runtime on WAMR embedding API only)
- Actor = wasm instance + bounded mailbox; share-nothing, copied messages, no allocation on send (pool; `WA_EFULL` backpressure).
- Supervisors: one-for-one / one-for-all / rest-for-one; permanent / transient / temporary; restart intensity → escalation → fatal handler (exit, let QNX process manager / HAM restart).
- Monitors (`WA_TAG_DOWN`), named registry (names survive restarts, pids don't).
- N worker threads, per-actor serial execution, `reductions` fairness.
- Per-message deadlines: `budget_ms` (AOT built with `wamrc --enable-multi-thread`) or `instruction_budget` (interpreter + `WAMR_BUILD_INSTRUCTION_METERING`).
- Hot code upgrade: `wa_module_upgrade()`; state carried via `actor_snapshot` / `actor_restore`.
- Native actors (`wa_spawn_native`) = bridge to middleware (ROS 2, AUTOSAR, SOME/IP, DDS, QNX pulses/channels).
- `wa_stats()`: msgs, crashes, restarts, handler time, worst-case handler, mailbox depth.
- Launcher `wamr-actors NAME=FILE ...`; tests (`ping/pong/counter/crasher/spinner/ticker/worker`) — same `.wasm` files run on host and QNX.
- Size: ~2.3 kLOC C, no dependency from WAMR on it.

## 2. Use cases in robot software

### 2.1 Inference abstracted from hardware
- Model pre/post-processing, tracking, planners, safety monitors as wasm actors; the heavy tensor work behind a **host capability** (`wasi-nn`-like import or a native actor: `infer(model_id, tensor) → tensor`) implemented per platform (TensorRT, Qualcomm QNN, TI TIDL, NPU vendor SDK, CPU fallback/XNNPACK).
- Same artifact on QNX/Linux/RTOS; only the accelerator backend differs. Capability negotiation at spawn time ("needs: npu.int8 ≥ 10 TOPS, else cpu.fp32 degraded mode").
- Graceful degradation: supervisor swaps the actor to a lighter model/backend when the accelerator actor goes DOWN.

### 2.2 Dynamically adaptable systems
- Hot upgrade of planner/controller logic without process restart (state via snapshot/restore) — parameter tuning, A/B of policies in shadow mode (both actors receive inputs, only one drives outputs; diff published).
- Mode-dependent actor sets (parking, highway, teleop, degraded): mode manager spawns/stops subtrees.
- Budgets → enforced WCET envelopes per message; stats drive admission control.

### 2.3 Truly distributable nodes
- Because the unit is a portable `.wasm` + serializable state, an actor can migrate: snapshot → ship bytes → restore on another ECU/arch (QNX ARM ↔ Linux x86 ↔ cloud sim).
- Needed next: **location-transparent pids/names** (`node@name`), a transport native actor (DDS/zenoh/SOME-IP/QNX GNS/TCP), remote monitors (link loss = DOWN), placement agent per node advertising resources (CPU, NPU, memory, latency to sensors).
- Scheduler/placement: constraint solver keeps latency-critical actors near sensors/actuators and moves the rest for load or failure (redundant compute fail-over).

### 2.4 Deployment = config with executable inside
- A signed **bundle**: manifest (supervision tree, restart policy, budgets, capabilities required, topic bindings, placement constraints) + embedded `.wasm`/`.aot` per arch + default state/params.
- Format idea: wasm custom sections or OCI artifact; content-addressed modules (hash = identity, dedup, cache).
- Node agent verifies signature, checks capability grants, AOT-compiles or picks prebuilt `.aot`, builds the tree, rolls out with canary/shadow, rolls back on restart-intensity breach.
- "Config is code" also inside: guards/validators as tiny wasm functions in configs (e.g. parameter sanity, geofence rules).

## 3. Other ideas
- **Deterministic replay**: actors are pure message handlers → record inbound messages + `wa_now_us`, replay bit-exact on desktop for debugging field incidents; sim/HIL using identical binaries.
- **Capability security**: actors only see imports granted in the manifest; untrusted/third-party/customer plugins (skills, behaviors) sandboxed on the safety-certified host.
- **Mixed criticality**: separate runtimes per criticality level in separate QNX processes (adaptive partitioning budgets), wamr-actors inside each; QM wasm never shares memory with ASIL host.
- **Safety monitors as wasm**: small verifiable checkers (envelope, plausibility) deployable independently from the main stack.
- **Fleet learning loop**: push new perception post-processing or heuristics OTA as kilobyte-size modules instead of full images.
- **Edge ↔ cloud continuum**: same actor runs in cloud simulation, regression farm, and car; Erlang-style "let it crash" testing (fault injection via `wa_exit`, mailbox flooding).
- **Telemetry**: export `wa_stats` to tracing (Perfetto/LTTng/QNX tracelogger); per-actor latency histograms.
- **Language freedom**: Rust/C/Zig/TinyGo actors on one bus.

## 4. Gaps / next steps
1. Upstream the 3 POSIX fixes and the QNX platform separately from actors.
2. `sigaltstack` on QNX 7.1 in relibc (would enable full HW bound checks); verify the new relibc main-thread stack, `pthread_setname_np` and `MS_INVALIDATE_ICACHE` on target.
3. QNX x86_64 target; QNX-native IPC native actor (channels/pulses) and HAM integration for the fatal handler.
4. Message schema (FlatBuffers/CDR) + zero-copy for large tensors (shared buffers as handles, not copies).
5. Distribution layer (remote pids, transport, placement agent) and bundle format/signing.
6. Real-time: map workers to priorities/cores; priority-aware mailboxes; budget measurement on target.
7. Safety case: what can and cannot be claimed for WAMR interpreter/AOT in an ISO 26262 / IEC 61508 context.
