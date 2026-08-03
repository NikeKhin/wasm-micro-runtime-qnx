/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WAMR_ACTORS_INTERNAL_H
#define WAMR_ACTORS_INTERNAL_H

#include <pthread.h>
#include <stdio.h>

#include "wamr_actors.h"
#include "wasm_export.h"

#define WA_NAME_LEN 32
#define WA_MAX_WATCHERS 8
#define WA_MAX_CHILDREN 32
#define WA_MAX_RESTART_HISTORY 32

typedef struct message {
    struct message *next;
    wa_pid_t from;
    uint32_t tag;
    uint32_t len;
    uint8_t data[];
} message_t;

typedef enum {
    KIND_FREE,
    KIND_WASM,
    KIND_NATIVE,
    KIND_SUPERVISOR,
} actor_kind_t;

/* Lifecycle requests, carried out in the actor's own context. */
typedef enum {
    CTL_NONE,
    CTL_START,
    /* Stop the running instance, if any, and start a new one. */
    CTL_RESTART,
    CTL_STOP,
} actor_ctl_t;

typedef struct module_entry {
    struct module_entry *next;
    char name[WA_NAME_LEN];
    uint32_t version;
    uint8_t *buf;
    wasm_module_t module;
    /* Instances running this version. */
    uint32_t users;
    bool current;
} module_entry_t;

typedef struct actor {
    struct wa_runtime *rt;
    uint32_t index;

    /* Guarded by lock. generation and kind also change only under rt->lock. */
    pthread_mutex_t lock;
    uint16_t generation;
    actor_kind_t kind;
    bool alive;
    /* In the run queue, or being run by a worker. */
    bool scheduled;
    actor_ctl_t ctl;
    int32_t ctl_reason;
    message_t *head, *tail;
    uint32_t depth;
    wa_pid_t watchers[WA_MAX_WATCHERS];
    uint32_t n_watchers;
    wa_stats_t stats;

    /* Guarded by rt->lock. */
    char name[WA_NAME_LEN];
    wa_pid_t supervisor;
    wa_restart_t restart;

    /* Used only in the actor's own context. */
    char module_name[WA_NAME_LEN];
    module_entry_t *code;
    uint32_t code_epoch;
    wasm_module_inst_t inst;
    wasm_exec_env_t exec_env;
    wasm_function_inst_t f_init, f_handle, f_snapshot, f_restore;
    uint8_t *init_arg;
    uint32_t init_arg_len;
    uint32_t stack_size, heap_size, max_memory_pages;
    uint32_t budget_ms, instruction_budget;

    wa_native_handler_t handler;
    void *ctx;

    /* Supervisors, guarded by rt->lock. */
    wa_strategy_t strategy;
    uint32_t max_restarts, period_ms;
    uint64_t restart_times[WA_MAX_RESTART_HISTORY];
    uint32_t n_restart_times;
    uint32_t children[WA_MAX_CHILDREN];
    uint32_t n_children;
} actor_t;

typedef struct worker {
    struct wa_runtime *rt;
    pthread_t thread;
    /* Guarded by rt->timer_lock. */
    actor_t *running;
    uint64_t deadline_us;
    bool terminated;
} worker_t;

typedef struct {
    uint64_t due_us;
    wa_pid_t to;
    message_t *msg;
} timer_entry_t;

struct wa_runtime {
    wa_config_t config;

    /* Actor table, registry, supervision and modules. */
    pthread_mutex_t lock;
    actor_t *actors;
    module_entry_t *modules;
    /* Bumped on every upgrade, so actors only check their code then. */
    volatile uint32_t code_epoch;

    pthread_mutex_t rq_lock;
    pthread_cond_t rq_cond, idle_cond;
    uint32_t *rq;
    uint32_t rq_head, rq_count;
    /* Actors with scheduled set. */
    uint32_t active;
    bool stopping;

    pthread_mutex_t pool_lock;
    uint8_t *pool_mem;
    message_t *free_list;

    pthread_mutex_t timer_lock;
    pthread_cond_t timer_cond;
    timer_entry_t *timers;
    uint32_t n_timers;
    bool timers_stopping;
    bool timer_started;
    pthread_t timer_thread;

    worker_t *workers;
    bool started;

    wa_fatal_handler_t fatal;
    void *fatal_ctx;
};

uint64_t
wa_now_us(void);

static inline wa_pid_t
actor_pid(const actor_t *actor)
{
    return ((wa_pid_t)actor->generation << 16) | (actor->index + 1);
}

/* The slot a pid refers to, without checking that the pid is still current. */
actor_t *
actor_slot(wa_runtime_t *rt, wa_pid_t pid);

/* Called with rt->lock held. */
actor_t *
actor_find_locked(wa_runtime_t *rt, wa_pid_t pid);

message_t *
message_alloc(wa_runtime_t *rt);

void
message_free(wa_runtime_t *rt, message_t *msg);

/* Takes ownership of msg. */
int
message_deliver(wa_runtime_t *rt, wa_pid_t to, message_t *msg);

/* Asks the actor to carry out ctl in its own context. */
void
actor_request(wa_runtime_t *rt, actor_t *actor, actor_ctl_t ctl,
              int32_t reason);

/*
 * Marks the actor dead: clears its mailbox, retires its pid and returns its
 * watchers. The mailbox is returned through dropped for the caller to free
 * outside the locks.
 */
uint32_t
actor_retire_locked(actor_t *actor, wa_pid_t *old_pid, wa_pid_t *watchers,
                    message_t **dropped);

/* Makes a retired actor accept messages again, at its new pid. */
void
actor_revive(actor_t *actor);

void
actor_free_locked(wa_runtime_t *rt, actor_t *actor);

void
actor_notify_down(wa_runtime_t *rt, wa_pid_t pid, const wa_pid_t *watchers,
                  uint32_t n_watchers, int32_t reason);

/* actor_wasm.c */
bool
wasm_natives_register(void);

module_entry_t *
module_current_locked(wa_runtime_t *rt, const char *name);

void
module_release_locked(wa_runtime_t *rt, module_entry_t *code);

/* Starts the current code with actor_restore(state), or else actor_init. */
bool
wasm_actor_start(worker_t *worker, actor_t *actor, const uint8_t *state,
                 uint32_t state_len);

void
wasm_actor_stop(actor_t *actor);

typedef enum {
    DELIVER_OK,
    DELIVER_EXIT,
    DELIVER_CRASH,
} deliver_result_t;

deliver_result_t
wasm_actor_deliver(worker_t *worker, actor_t *actor, const message_t *msg,
                   int32_t *reason);

/*
 * Moves the actor to the module's current code, keeping its state if it can.
 * Returns false if the actor was left without code.
 */
bool
wasm_actor_upgrade(worker_t *worker, actor_t *actor);

/* actor_supervisor.c, called with rt->lock held. */
typedef struct {
    /* A top-level supervisor that gave up, for the fatal handler. */
    wa_pid_t fatal;
} supervision_result_t;

void
supervisor_child_exited_locked(wa_runtime_t *rt, actor_t *child, int32_t reason,
                               supervision_result_t *result);

/* actor_timer.c */
bool
timers_init(wa_runtime_t *rt);

bool
timers_start(wa_runtime_t *rt);

void
timers_stop(wa_runtime_t *rt);

void
timers_destroy(wa_runtime_t *rt);

int
timer_add(wa_runtime_t *rt, uint32_t delay_ms, wa_pid_t to, message_t *msg);

void
watchdog_begin(worker_t *worker, actor_t *actor, uint32_t budget_ms);

/* Returns whether the watchdog terminated the handler. */
bool
watchdog_end(worker_t *worker);

#define wa_log_error(...)                 \
    do {                                  \
        fprintf(stderr, "wamr-actors: "); \
        fprintf(stderr, __VA_ARGS__);     \
        fprintf(stderr, "\n");            \
    } while (0)

#endif /* end of WAMR_ACTORS_INTERNAL_H */
