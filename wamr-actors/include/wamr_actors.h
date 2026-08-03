/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WAMR_ACTORS_H
#define WAMR_ACTORS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An actor's address. 0 is never valid. An actor gets a new pid each time it
 * is (re)started, so a pid held after a crash fails instead of reaching the
 * restarted actor; look the actor up by name to find its current pid.
 */
typedef uint32_t wa_pid_t;

typedef struct wa_runtime wa_runtime_t;

/* Tags below WA_TAG_USER are the runtime's own. */
enum {
    /* A monitored actor exited. The payload is a wa_down_t. */
    WA_TAG_DOWN = 1,
    WA_TAG_USER = 256,
};

/* Exit reasons. Zero and positive values are the actor's own. */
enum {
    WA_EXIT_NORMAL = 0,
    WA_EXIT_CRASH = -1,    /* trapped, or exceeded a budget */
    WA_EXIT_KILLED = -2,   /* stopped with wa_exit() */
    WA_EXIT_SHUTDOWN = -3, /* stopped by its supervisor */
    WA_EXIT_NOPROC = -4,   /* monitored after it had already exited */
};

typedef struct {
    wa_pid_t pid;
    int32_t reason;
} wa_down_t;

/* Results. Negative values are errors. */
enum {
    WA_OK = 0,
    WA_ENOENT = -1, /* no such actor, or it has exited */
    WA_EFULL =
        -2, /* the mailbox, the message pool or the timer table is full */
    WA_ETOOBIG = -3,
    WA_EINVAL = -4,
    WA_ENOMEM = -5,
};

typedef struct {
    /* Scheduler threads. */
    uint32_t workers;
    uint32_t worker_stack_size;
    /* Fixed table sizes, allocated when the runtime is created. */
    uint32_t max_actors;
    uint32_t max_timers;
    /* Messages queued per actor before senders get WA_EFULL. */
    uint32_t mailbox_capacity;
    /* Messages in flight across all actors, each up to max_message_size. */
    uint32_t message_pool;
    uint32_t max_message_size;
    /* Messages an actor handles before it yields its worker. */
    uint32_t reductions;
    /* Whether wasm actors may spawn actors themselves. */
    bool guest_spawn;
} wa_config_t;

void
wa_config_default(wa_config_t *config);

/*
 * The embedder initializes WAMR before creating a runtime, and destroys it
 * after the runtime.
 */
wa_runtime_t *
wa_runtime_create(const wa_config_t *config, char *error_buf,
                  uint32_t error_buf_size);

int
wa_runtime_start(wa_runtime_t *rt);

/* Stops the workers and destroys every actor. */
void
wa_runtime_destroy(wa_runtime_t *rt);

/* Waits until no actor has messages or work pending. */
bool
wa_runtime_wait_idle(wa_runtime_t *rt, uint32_t timeout_ms);

/*
 * Loads a .wasm or .aot file under a name actors are spawned from. The
 * runtime keeps its own copy of the buffer.
 */
int
wa_module_load(wa_runtime_t *rt, const char *name, const uint8_t *buf,
               uint32_t size, char *error_buf, uint32_t error_buf_size);

/*
 * Replaces the code of a loaded module. Actors running it switch at their next
 * message: if the old code exports actor_snapshot and the new one
 * actor_restore, the state is carried over, otherwise the actor starts afresh
 * with actor_init.
 */
int
wa_module_upgrade(wa_runtime_t *rt, const char *name, const uint8_t *buf,
                  uint32_t size, char *error_buf, uint32_t error_buf_size);

typedef enum {
    /* Restart only the child that exited. */
    WA_ONE_FOR_ONE,
    /* Restart every child. */
    WA_ONE_FOR_ALL,
    /* Restart the child that exited and those started after it. */
    WA_REST_FOR_ONE,
} wa_strategy_t;

typedef enum {
    WA_PERMANENT, /* always restarted */
    WA_TRANSIENT, /* restarted unless it exited normally */
    WA_TEMPORARY, /* never restarted */
} wa_restart_t;

typedef struct {
    const char *name;
    /* Parent supervisor, 0 for a top-level one. */
    wa_pid_t parent;
    wa_strategy_t strategy;
    /*
     * More than max_restarts restarts within period_ms make the supervisor
     * stop its children and exit. Its parent then handles it like any other
     * child; a top-level supervisor calls the fatal handler.
     */
    uint32_t max_restarts;
    uint32_t period_ms;
} wa_supervisor_spec_t;

wa_pid_t
wa_supervisor_start(wa_runtime_t *rt, const wa_supervisor_spec_t *spec);

typedef void (*wa_fatal_handler_t)(wa_runtime_t *rt, void *ctx,
                                   wa_pid_t supervisor);

void
wa_set_fatal_handler(wa_runtime_t *rt, wa_fatal_handler_t handler, void *ctx);

typedef struct {
    /* Name passed to wa_module_load. */
    const char *module;
    /* Registered name, kept across restarts. Optional. */
    const char *name;
    /* 0 for an unsupervised actor. */
    wa_pid_t supervisor;
    wa_restart_t restart;
    /* Passed to actor_init on every start. */
    const void *init_arg;
    uint32_t init_arg_len;
    uint32_t stack_size;
    /* Heap inside the actor's memory that incoming messages are copied to. */
    uint32_t heap_size;
    /* Upper limit of the actor's linear memory, 0 for the module's own. */
    uint32_t max_memory_pages;
    /*
     * Limits per message, 0 for none. A time budget interrupts interpreted
     * code, and AOT code compiled with wamrc --enable-multi-thread. An
     * instruction budget needs WAMR_BUILD_INSTRUCTION_METERING and the
     * interpreter.
     */
    uint32_t budget_ms;
    uint32_t instruction_budget;
} wa_actor_spec_t;

/*
 * Returns at once: the actor initializes on a worker, and messages sent to it
 * in the meantime are queued.
 */
wa_pid_t
wa_spawn(wa_runtime_t *rt, const wa_actor_spec_t *spec, char *error_buf,
         uint32_t error_buf_size);

/*
 * A native actor runs a host callback for each message, on a worker. It is
 * how middleware such as AUTOSAR or ROS 2 is connected: inbound traffic is
 * sent to wasm actors with wa_send, and messages sent to the native actor go
 * out through the middleware.
 */
typedef void (*wa_native_handler_t)(wa_runtime_t *rt, void *ctx, wa_pid_t self,
                                    wa_pid_t from, uint32_t tag,
                                    const void *data, uint32_t len);

wa_pid_t
wa_spawn_native(wa_runtime_t *rt, const char *name, wa_native_handler_t handler,
                void *ctx);

/* Copies the payload. from may be 0 for messages from the host. */
int
wa_send(wa_runtime_t *rt, wa_pid_t from, wa_pid_t to, uint32_t tag,
        const void *data, uint32_t len);

int
wa_send_after(wa_runtime_t *rt, wa_pid_t from, wa_pid_t to, uint32_t tag,
              const void *data, uint32_t len, uint32_t delay_ms);

wa_pid_t
wa_whereis(wa_runtime_t *rt, const char *name);

/* watcher gets one WA_TAG_DOWN message when target exits. */
int
wa_monitor(wa_runtime_t *rt, wa_pid_t watcher, wa_pid_t target);

/* Stops an actor. Its supervisor sees an abnormal exit unless reason >= 0. */
int
wa_exit(wa_runtime_t *rt, wa_pid_t pid, int32_t reason);

typedef struct {
    bool alive;
    wa_pid_t pid;
    uint64_t messages;
    uint64_t crashes;
    uint64_t restarts;
    uint64_t upgrades;
    uint64_t busy_us;
    uint64_t max_handler_us;
    uint32_t mailbox_depth;
    uint32_t mailbox_high_water;
    /* Messages refused because the mailbox was full. */
    uint32_t dropped;
} wa_stats_t;

/* Statistics of the actor that has, or last had, a registered name. */
int
wa_stats(wa_runtime_t *rt, const char *name, wa_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* end of WAMR_ACTORS_H */
