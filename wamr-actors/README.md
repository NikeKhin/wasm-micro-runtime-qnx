# wamr-actors

Actors in the style of Erlang's BEAM on top of WAMR. Each actor is a WebAssembly instance with its own linear memory and a mailbox. Actors share nothing and only exchange messages, which are copied. Supervisors restart actors that crash, a pool of worker threads runs actors in parallel, and a module's code can be replaced while its actors run.

wamr-actors only uses WAMR's embedding API, [wasm_export.h](../core/iwasm/include/wasm_export.h), and nothing in WAMR depends on it, so the directory can be carried on top of upstream WAMR without merge conflicts.

## Model

- **Actors** are wasm instances. A trap, or a message that exceeds its time or instruction budget, crashes only that actor, with the exit reason `WA_EXIT_CRASH`.
- **Messages** are copied into the receiver's heap. Mailboxes are bounded and messages come from a pool allocated with the runtime, so sending never allocates: when a mailbox or the pool is full, the sender gets `WA_EFULL`.
- **Monitors**: the watcher gets one `WA_TAG_DOWN` message, carrying a `wa_down_t`, when the watched actor exits.
- **Supervisors** restart their children one-for-one, one-for-all or rest-for-one, as each child's restart type allows: permanent, transient or temporary. A supervisor that needs more than `max_restarts` restarts within `period_ms` stops its children and exits, which its own supervisor handles like any other exit. A top-level supervisor calls the fatal handler instead, which can, for example, exit the process for the platform's process manager to restart it.
- **Scheduling**: `workers` threads run the actors that have messages. An actor never runs on two workers at once, so its code is single-threaded, and it yields its worker after `reductions` messages. Instead of BEAM's preemption, a handler that runs longer than `budget_ms`, or longer than `instruction_budget` instructions, is interrupted.
- **Code upgrades**: `wa_module_upgrade()` replaces a module's code, and each actor running it switches at its next message. The actor keeps its state if the old code exports `actor_snapshot` and the new code `actor_restore`; otherwise it starts afresh.
- **Names** registered with an actor survive its restarts, while pids don't: a pid held across a crash fails with `WA_ENOENT` instead of reaching the restarted actor.

BEAM's garbage collection, distribution and selective receive have no counterpart: an actor's memory is freed when it exits, other processes or ECUs are reached through native actors (below), and each message is handled in order. Actors can only spawn actors if the runtime is created with `guest_spawn`, so normally the host sets up the whole tree.

## Embedding

The host API is [include/wamr_actors.h](include/wamr_actors.h). WAMR is initialized before the runtime is created:

``` C
char error[128];
wa_config_t config;
wa_runtime_t *rt;

wasm_runtime_init();
wa_config_default(&config);
config.workers = 4;
rt = wa_runtime_create(&config, error, sizeof(error));

wa_module_load(rt, "planner", wasm, wasm_size, error, sizeof(error));

wa_supervisor_spec_t sup = { .name = "main", .strategy = WA_ONE_FOR_ONE,
                             .max_restarts = 3, .period_ms = 5000 };
wa_actor_spec_t planner = { .module = "planner", .name = "planner",
                            .restart = WA_PERMANENT, .budget_ms = 10 };
planner.supervisor = wa_supervisor_start(rt, &sup);
wa_spawn(rt, &planner, error, sizeof(error));

wa_runtime_start(rt);
wa_send(rt, 0, wa_whereis(rt, "planner"), WA_TAG_USER, data, data_size);
...
wa_runtime_destroy(rt);
wasm_runtime_destroy();
```

`wa_send()` can be called from any thread. A native actor, started with `wa_spawn_native()`, runs a host callback for each message sent to it, on a worker; it is how middleware such as AUTOSAR or ROS 2 is connected. Its callback publishes what wasm actors send to it, and the middleware's own threads pass inbound traffic on with `wa_send()`. Callbacks shouldn't block, as they hold a worker.

`wa_stats()` reports an actor's message count, crashes, restarts, time spent in handlers, the longest handler and its mailbox depth.

## Writing actors

Actors written in C include [include/wamr_actors_guest.h](include/wamr_actors_guest.h) and export a message handler. The runtime calls it once per message; returning anything other than 0 makes the actor exit with that reason:

``` C
#include "wamr_actors_guest.h"

#define T_PING (WA_TAG_USER + 1)
#define T_PONG (WA_TAG_USER + 2)

WA_EXPORT("actor_handle")
int32_t
actor_handle(int32_t from, int32_t tag, const uint8_t *data, uint32_t len)
{
    if (tag == T_PING)
        wa_send(from, T_PONG, data, len);
    return 0;
}
```

`actor_init`, `actor_snapshot` and `actor_restore` are optional exports, and the header lists what actors can import, such as `wa_send_after()`, `wa_whereis()`, `wa_monitor()` and `wa_now_us()`. The tests compile actors with:

``` Bash
clang --target=wasm32 -mcpu=mvp -mbulk-memory -O2 -nostdlib -Wl,--no-entry \
      -I wamr-actors/include -o pong.wasm pong.c
```

Time budgets only interrupt AOT code compiled with `wamrc --enable-multi-thread`. Instruction budgets need the interpreter and WAMR built with `WAMR_BUILD_INSTRUCTION_METERING=1`.

## Building and testing

``` Bash
cmake -S wamr-actors -B build -DWAMR_ACTORS_WASM_CC=<clang>
cmake --build build
ctest --test-dir build
```

`WAMR_ACTORS_WASM_CC` is a clang that targets wasm32, used to compile the test actors. WAMR's features are chosen with the usual `WAMR_BUILD_*` options. The defaults here are the fast interpreter, AOT and the thread manager, which time budgets need, without WASI or the built-in libc, as actors only use the runtime's imports.

For QNX, use the toolchain file described in [product-mini/README.md](../product-mini/README.md#qnx). The test actors are still compiled with the host's clang, as the same .wasm files run on every target:

``` Bash
cmake -S wamr-actors -B build-qnx \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/product-mini/platforms/qnx/relibc_toolchain.cmake \
      -DRELIBC_QNX_DIR=<relibc> -DQNX_VERSION=8.0 -DWAMR_ACTORS_WASM_CC=<clang>
cmake --build build-qnx
```

Then copy `build-qnx/actor_test` and `build-qnx/test` to the target and run `actor_test test`. Actors run on worker threads, whose stacks relibc reports, so native stack overflow is detected on QNX as well; AOT code needs `wamrc --stack-bounds-checks=1` for it there.

## Launcher

`wamr-actors [options] NAME=FILE ...` loads each file as a module and runs one permanent actor of it, registered under its name, beneath a one-for-one supervisor. It prints each actor's statistics when the actors are idle, or after `--run-ms`, and exits with status 2 if the supervisor gives up. `wamr-actors --help` lists the options.
