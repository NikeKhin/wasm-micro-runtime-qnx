/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "actor_internal.h"

#define DEFAULT_STACK_SIZE (32 * 1024)
#define DEFAULT_HEAP_SIZE (64 * 1024)

uint64_t
wa_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

void
wa_config_default(wa_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->workers = 1;
    config->worker_stack_size = 256 * 1024;
    config->max_actors = 64;
    config->max_timers = 256;
    config->mailbox_capacity = 64;
    config->message_pool = 1024;
    config->max_message_size = 4096;
    config->reductions = 16;
}

static void
set_error(char *error_buf, uint32_t error_buf_size, const char *message)
{
    if (error_buf && error_buf_size)
        snprintf(error_buf, error_buf_size, "%s", message);
}

actor_t *
actor_slot(wa_runtime_t *rt, wa_pid_t pid)
{
    uint32_t index = (pid & 0xffff) - 1;

    if ((pid & 0xffff) == 0 || index >= rt->config.max_actors)
        return NULL;
    return &rt->actors[index];
}

actor_t *
actor_find_locked(wa_runtime_t *rt, wa_pid_t pid)
{
    actor_t *actor = actor_slot(rt, pid);

    if (actor && actor->kind != KIND_FREE && actor_pid(actor) == pid)
        return actor;
    return NULL;
}

message_t *
message_alloc(wa_runtime_t *rt)
{
    message_t *msg;

    pthread_mutex_lock(&rt->pool_lock);
    msg = rt->free_list;
    if (msg)
        rt->free_list = msg->next;
    pthread_mutex_unlock(&rt->pool_lock);
    return msg;
}

void
message_free(wa_runtime_t *rt, message_t *msg)
{
    pthread_mutex_lock(&rt->pool_lock);
    msg->next = rt->free_list;
    rt->free_list = msg;
    pthread_mutex_unlock(&rt->pool_lock);
}

static void
messages_free(wa_runtime_t *rt, message_t *msg)
{
    while (msg) {
        message_t *next = msg->next;
        message_free(rt, msg);
        msg = next;
    }
}

/* Called with actor->lock held and actor->scheduled clear. */
static void
schedule_locked(wa_runtime_t *rt, actor_t *actor)
{
    actor->scheduled = true;
    pthread_mutex_lock(&rt->rq_lock);
    rt->rq[(rt->rq_head + rt->rq_count) % rt->config.max_actors] = actor->index;
    rt->rq_count++;
    rt->active++;
    pthread_cond_signal(&rt->rq_cond);
    pthread_mutex_unlock(&rt->rq_lock);
}

/* Called with actor->lock held by the worker that runs the actor. */
static void
requeue_locked(wa_runtime_t *rt, actor_t *actor)
{
    pthread_mutex_lock(&rt->rq_lock);
    rt->rq[(rt->rq_head + rt->rq_count) % rt->config.max_actors] = actor->index;
    rt->rq_count++;
    pthread_cond_signal(&rt->rq_cond);
    pthread_mutex_unlock(&rt->rq_lock);
}

static void
unschedule_locked(wa_runtime_t *rt, actor_t *actor)
{
    actor->scheduled = false;
    pthread_mutex_lock(&rt->rq_lock);
    if (--rt->active == 0)
        pthread_cond_broadcast(&rt->idle_cond);
    pthread_mutex_unlock(&rt->rq_lock);
}

int
message_deliver(wa_runtime_t *rt, wa_pid_t to, message_t *msg)
{
    actor_t *actor = actor_slot(rt, to);
    int ret = WA_ENOENT;

    if (actor) {
        pthread_mutex_lock(&actor->lock);
        if ((actor->kind == KIND_WASM || actor->kind == KIND_NATIVE)
            && actor->alive && actor_pid(actor) == to) {
            if (actor->depth >= rt->config.mailbox_capacity) {
                actor->stats.dropped++;
                ret = WA_EFULL;
            }
            else {
                msg->next = NULL;
                if (actor->tail)
                    actor->tail->next = msg;
                else
                    actor->head = msg;
                actor->tail = msg;
                if (++actor->depth > actor->stats.mailbox_high_water)
                    actor->stats.mailbox_high_water = actor->depth;
                if (!actor->scheduled)
                    schedule_locked(rt, actor);
                ret = WA_OK;
            }
        }
        pthread_mutex_unlock(&actor->lock);
    }

    if (ret != WA_OK)
        message_free(rt, msg);
    return ret;
}

static message_t *
message_make(wa_runtime_t *rt, wa_pid_t from, uint32_t tag, const void *data,
             uint32_t len, int *ret)
{
    message_t *msg;

    if (len > rt->config.max_message_size) {
        *ret = WA_ETOOBIG;
        return NULL;
    }
    if (len && !data) {
        *ret = WA_EINVAL;
        return NULL;
    }
    if (!(msg = message_alloc(rt))) {
        *ret = WA_EFULL;
        return NULL;
    }
    msg->from = from;
    msg->tag = tag;
    msg->len = len;
    if (len)
        memcpy(msg->data, data, len);
    return msg;
}

int
wa_send(wa_runtime_t *rt, wa_pid_t from, wa_pid_t to, uint32_t tag,
        const void *data, uint32_t len)
{
    int ret = WA_OK;
    message_t *msg = message_make(rt, from, tag, data, len, &ret);

    if (!msg)
        return ret;
    return message_deliver(rt, to, msg);
}

int
wa_send_after(wa_runtime_t *rt, wa_pid_t from, wa_pid_t to, uint32_t tag,
              const void *data, uint32_t len, uint32_t delay_ms)
{
    int ret = WA_OK;
    message_t *msg = message_make(rt, from, tag, data, len, &ret);

    if (!msg)
        return ret;
    if ((ret = timer_add(rt, delay_ms, to, msg)) != WA_OK)
        message_free(rt, msg);
    return ret;
}

void
actor_request(wa_runtime_t *rt, actor_t *actor, actor_ctl_t ctl, int32_t reason)
{
    pthread_mutex_lock(&actor->lock);
    actor->ctl = ctl;
    actor->ctl_reason = reason;
    if (!actor->scheduled)
        schedule_locked(rt, actor);
    pthread_mutex_unlock(&actor->lock);
}

uint32_t
actor_retire_locked(actor_t *actor, wa_pid_t *old_pid, wa_pid_t *watchers,
                    message_t **dropped)
{
    uint32_t n;

    pthread_mutex_lock(&actor->lock);
    *old_pid = actor_pid(actor);
    actor->alive = false;
    *dropped = actor->head;
    actor->head = actor->tail = NULL;
    actor->depth = 0;
    n = actor->n_watchers;
    memcpy(watchers, actor->watchers, n * sizeof(wa_pid_t));
    actor->n_watchers = 0;
    if (++actor->generation == 0)
        actor->generation = 1;
    pthread_mutex_unlock(&actor->lock);
    return n;
}

void
actor_revive(actor_t *actor)
{
    pthread_mutex_lock(&actor->lock);
    actor->alive = true;
    pthread_mutex_unlock(&actor->lock);
}

void
actor_notify_down(wa_runtime_t *rt, wa_pid_t pid, const wa_pid_t *watchers,
                  uint32_t n_watchers, int32_t reason)
{
    wa_down_t down = { pid, reason };
    uint32_t i;

    for (i = 0; i < n_watchers; i++)
        wa_send(rt, pid, watchers[i], WA_TAG_DOWN, &down, sizeof(down));
}

void
actor_free_locked(wa_runtime_t *rt, actor_t *actor)
{
    actor_t *sup =
        actor->supervisor ? actor_find_locked(rt, actor->supervisor) : NULL;
    uint32_t i;

    if (sup) {
        for (i = 0; i < sup->n_children; i++) {
            if (sup->children[i] == actor->index) {
                memmove(&sup->children[i], &sup->children[i + 1],
                        (sup->n_children - i - 1) * sizeof(uint32_t));
                sup->n_children--;
                break;
            }
        }
    }
    pthread_mutex_lock(&actor->lock);
    actor->kind = KIND_FREE;
    actor->alive = false;
    pthread_mutex_unlock(&actor->lock);
    free(actor->init_arg);
    actor->init_arg = NULL;
    actor->init_arg_len = 0;
    actor->name[0] = '\0';
    actor->supervisor = 0;
}

/*
 * The actor has stopped: tell its watchers and its supervisor, which may
 * restart it. Called in the actor's context.
 */
static void
actor_exit(wa_runtime_t *rt, actor_t *actor, int32_t reason)
{
    supervision_result_t result = { 0 };
    wa_pid_t pid, watchers[WA_MAX_WATCHERS];
    message_t *dropped;
    uint32_t n;

    if (actor->kind == KIND_WASM)
        wasm_actor_stop(actor);

    pthread_mutex_lock(&rt->lock);
    n = actor_retire_locked(actor, &pid, watchers, &dropped);
    if (actor->supervisor)
        supervisor_child_exited_locked(rt, actor, reason, &result);
    else
        actor_free_locked(rt, actor);
    pthread_mutex_unlock(&rt->lock);

    messages_free(rt, dropped);
    actor_notify_down(rt, pid, watchers, n, reason);
    if (result.fatal && rt->fatal)
        rt->fatal(rt, rt->fatal_ctx, result.fatal);
}

static void
actor_count_crash(actor_t *actor)
{
    pthread_mutex_lock(&actor->lock);
    actor->stats.crashes++;
    pthread_mutex_unlock(&actor->lock);
}

static void
actor_run_ctl(worker_t *worker, actor_t *actor, actor_ctl_t ctl, int32_t reason)
{
    wa_runtime_t *rt = worker->rt;
    wa_pid_t pid, watchers[WA_MAX_WATCHERS];
    message_t *dropped;
    uint32_t n;

    switch (ctl) {
        case CTL_RESTART:
            if (actor->kind == KIND_WASM && actor->inst) {
                wasm_actor_stop(actor);
                pthread_mutex_lock(&rt->lock);
                n = actor_retire_locked(actor, &pid, watchers, &dropped);
                pthread_mutex_unlock(&rt->lock);
                messages_free(rt, dropped);
                actor_notify_down(rt, pid, watchers, n, WA_EXIT_SHUTDOWN);
            }
            pthread_mutex_lock(&actor->lock);
            actor->alive = true;
            actor->stats.restarts++;
            pthread_mutex_unlock(&actor->lock);
            /* fall through */
        case CTL_START:
            if (actor->kind == KIND_WASM
                && !wasm_actor_start(worker, actor, NULL, 0)) {
                actor_count_crash(actor);
                actor_exit(rt, actor, WA_EXIT_CRASH);
            }
            break;
        case CTL_STOP:
            actor_exit(rt, actor, reason);
            break;
        case CTL_NONE:
            break;
    }
}

static void
actor_handle_message(worker_t *worker, actor_t *actor, const message_t *msg)
{
    wa_runtime_t *rt = worker->rt;
    deliver_result_t result = DELIVER_OK;
    int32_t reason = WA_EXIT_NORMAL;
    uint64_t start, elapsed;

    start = wa_now_us();
    if (actor->kind == KIND_NATIVE) {
        actor->handler(rt, actor->ctx, actor_pid(actor), msg->from, msg->tag,
                       msg->data, msg->len);
    }
    else if (actor->kind == KIND_WASM) {
        if (actor->code_epoch != rt->code_epoch
            && !wasm_actor_upgrade(worker, actor))
            result = DELIVER_CRASH;
        else
            result = wasm_actor_deliver(worker, actor, msg, &reason);
    }
    elapsed = wa_now_us() - start;

    pthread_mutex_lock(&actor->lock);
    actor->stats.messages++;
    actor->stats.busy_us += elapsed;
    if (elapsed > actor->stats.max_handler_us)
        actor->stats.max_handler_us = elapsed;
    pthread_mutex_unlock(&actor->lock);

    if (result == DELIVER_CRASH) {
        actor_count_crash(actor);
        actor_exit(rt, actor, WA_EXIT_CRASH);
    }
    else if (result == DELIVER_EXIT) {
        actor_exit(rt, actor, reason);
    }
}

/* Runs one slice of the actor: lifecycle requests first, then messages. */
static void
actor_run(worker_t *worker, actor_t *actor)
{
    wa_runtime_t *rt = worker->rt;
    uint32_t i;

    for (i = 0; i < rt->config.reductions; i++) {
        actor_ctl_t ctl;
        int32_t reason;
        message_t *msg = NULL;

        pthread_mutex_lock(&actor->lock);
        ctl = actor->ctl;
        reason = actor->ctl_reason;
        actor->ctl = CTL_NONE;
        if (ctl == CTL_NONE) {
            if (!actor->alive || !actor->head) {
                unschedule_locked(rt, actor);
                pthread_mutex_unlock(&actor->lock);
                return;
            }
            msg = actor->head;
            actor->head = msg->next;
            if (!actor->head)
                actor->tail = NULL;
            actor->depth--;
        }
        pthread_mutex_unlock(&actor->lock);

        if (ctl != CTL_NONE) {
            actor_run_ctl(worker, actor, ctl, reason);
        }
        else {
            actor_handle_message(worker, actor, msg);
            message_free(rt, msg);
        }
    }

    pthread_mutex_lock(&actor->lock);
    if (actor->ctl != CTL_NONE || (actor->alive && actor->head))
        requeue_locked(rt, actor);
    else
        unschedule_locked(rt, actor);
    pthread_mutex_unlock(&actor->lock);
}

/* WAMR's per-thread signal setup writes process-wide state without a lock */
static pthread_mutex_t thread_env_lock = PTHREAD_MUTEX_INITIALIZER;

static void *
worker_main(void *arg)
{
    worker_t *worker = arg;
    wa_runtime_t *rt = worker->rt;
    bool thread_env;

    pthread_mutex_lock(&thread_env_lock);
    thread_env = wasm_runtime_init_thread_env();
    pthread_mutex_unlock(&thread_env_lock);
    if (!thread_env)
        wa_log_error("a worker couldn't initialize WAMR's thread environment");

    pthread_mutex_lock(&rt->rq_lock);
    while (!rt->stopping) {
        uint32_t index;

        if (rt->rq_count == 0) {
            pthread_cond_wait(&rt->rq_cond, &rt->rq_lock);
            continue;
        }
        index = rt->rq[rt->rq_head];
        rt->rq_head = (rt->rq_head + 1) % rt->config.max_actors;
        rt->rq_count--;
        pthread_mutex_unlock(&rt->rq_lock);
        actor_run(worker, &rt->actors[index]);
        pthread_mutex_lock(&rt->rq_lock);
    }
    pthread_mutex_unlock(&rt->rq_lock);

    if (thread_env) {
        pthread_mutex_lock(&thread_env_lock);
        wasm_runtime_destroy_thread_env();
        pthread_mutex_unlock(&thread_env_lock);
    }
    return NULL;
}

static bool
name_taken_locked(wa_runtime_t *rt, const char *name)
{
    uint32_t i;

    for (i = 0; i < rt->config.max_actors; i++) {
        if (rt->actors[i].kind != KIND_FREE
            && strcmp(rt->actors[i].name, name) == 0)
            return true;
    }
    return false;
}

static bool
name_valid(const char *name)
{
    return !name || strlen(name) < WA_NAME_LEN;
}

/* Claims a free slot. Called with rt->lock held. */
static actor_t *
actor_alloc_locked(wa_runtime_t *rt, actor_kind_t kind, const char *name,
                   char *error_buf, uint32_t error_buf_size)
{
    uint32_t i;

    if (!name_valid(name)) {
        set_error(error_buf, error_buf_size, "the name is too long");
        return NULL;
    }
    if (name && name[0] && name_taken_locked(rt, name)) {
        set_error(error_buf, error_buf_size, "the name is taken");
        return NULL;
    }
    for (i = 0; i < rt->config.max_actors; i++) {
        actor_t *actor = &rt->actors[i];

        if (actor->kind != KIND_FREE)
            continue;
        pthread_mutex_lock(&actor->lock);
        actor->kind = kind;
        actor->alive = true;
        actor->ctl = CTL_NONE;
        actor->n_watchers = 0;
        memset(&actor->stats, 0, sizeof(actor->stats));
        pthread_mutex_unlock(&actor->lock);
        snprintf(actor->name, sizeof(actor->name), "%s", name ? name : "");
        actor->supervisor = 0;
        actor->restart = WA_PERMANENT;
        actor->module_name[0] = '\0';
        actor->n_children = 0;
        actor->n_restart_times = 0;
        return actor;
    }
    set_error(error_buf, error_buf_size, "the actor table is full");
    return NULL;
}

static bool
adopt_locked(wa_runtime_t *rt, wa_pid_t supervisor, actor_t *child,
             char *error_buf, uint32_t error_buf_size)
{
    actor_t *sup;

    if (!supervisor)
        return true;
    sup = actor_find_locked(rt, supervisor);
    if (!sup || sup->kind != KIND_SUPERVISOR || !sup->alive) {
        set_error(error_buf, error_buf_size, "no such supervisor");
        return false;
    }
    if (sup->n_children >= WA_MAX_CHILDREN) {
        set_error(error_buf, error_buf_size, "the supervisor is full");
        return false;
    }
    sup->children[sup->n_children++] = child->index;
    child->supervisor = supervisor;
    return true;
}

wa_pid_t
wa_spawn(wa_runtime_t *rt, const wa_actor_spec_t *spec, char *error_buf,
         uint32_t error_buf_size)
{
    actor_t *actor;
    uint8_t *arg = NULL;
    wa_pid_t pid;

    if (!spec || !spec->module || (spec->init_arg_len && !spec->init_arg)) {
        set_error(error_buf, error_buf_size, "invalid actor spec");
        return 0;
    }
    if (spec->init_arg_len) {
        if (!(arg = malloc(spec->init_arg_len))) {
            set_error(error_buf, error_buf_size, "out of memory");
            return 0;
        }
        memcpy(arg, spec->init_arg, spec->init_arg_len);
    }

    pthread_mutex_lock(&rt->lock);
    if (!module_current_locked(rt, spec->module)) {
        pthread_mutex_unlock(&rt->lock);
        set_error(error_buf, error_buf_size, "the module isn't loaded");
        free(arg);
        return 0;
    }
    actor = actor_alloc_locked(rt, KIND_WASM, spec->name, error_buf,
                               error_buf_size);
    if (!actor) {
        pthread_mutex_unlock(&rt->lock);
        free(arg);
        return 0;
    }
    snprintf(actor->module_name, sizeof(actor->module_name), "%s",
             spec->module);
    actor->init_arg = arg;
    actor->init_arg_len = spec->init_arg_len;
    actor->stack_size =
        spec->stack_size ? spec->stack_size : DEFAULT_STACK_SIZE;
    actor->heap_size = spec->heap_size ? spec->heap_size : DEFAULT_HEAP_SIZE;
    actor->max_memory_pages = spec->max_memory_pages;
    actor->budget_ms = spec->budget_ms;
    actor->instruction_budget = spec->instruction_budget;
    actor->restart = spec->restart;
    if (!adopt_locked(rt, spec->supervisor, actor, error_buf, error_buf_size)) {
        actor_free_locked(rt, actor);
        pthread_mutex_unlock(&rt->lock);
        return 0;
    }
    pid = actor_pid(actor);
    pthread_mutex_unlock(&rt->lock);

    actor_request(rt, actor, CTL_START, 0);
    return pid;
}

wa_pid_t
wa_spawn_native(wa_runtime_t *rt, const char *name, wa_native_handler_t handler,
                void *ctx)
{
    actor_t *actor;
    wa_pid_t pid = 0;

    if (!handler)
        return 0;
    pthread_mutex_lock(&rt->lock);
    if ((actor = actor_alloc_locked(rt, KIND_NATIVE, name, NULL, 0))) {
        actor->handler = handler;
        actor->ctx = ctx;
        pid = actor_pid(actor);
    }
    pthread_mutex_unlock(&rt->lock);
    return pid;
}

wa_pid_t
wa_supervisor_start(wa_runtime_t *rt, const wa_supervisor_spec_t *spec)
{
    actor_t *sup;
    wa_pid_t pid = 0;

    if (!spec)
        return 0;
    pthread_mutex_lock(&rt->lock);
    if ((sup = actor_alloc_locked(rt, KIND_SUPERVISOR, spec->name, NULL, 0))) {
        sup->strategy = spec->strategy;
        sup->max_restarts = spec->max_restarts < WA_MAX_RESTART_HISTORY
                                ? spec->max_restarts
                                : WA_MAX_RESTART_HISTORY - 1;
        sup->period_ms = spec->period_ms;
        if (adopt_locked(rt, spec->parent, sup, NULL, 0))
            pid = actor_pid(sup);
        else
            actor_free_locked(rt, sup);
    }
    pthread_mutex_unlock(&rt->lock);
    return pid;
}

void
wa_set_fatal_handler(wa_runtime_t *rt, wa_fatal_handler_t handler, void *ctx)
{
    pthread_mutex_lock(&rt->lock);
    rt->fatal = handler;
    rt->fatal_ctx = ctx;
    pthread_mutex_unlock(&rt->lock);
}

wa_pid_t
wa_whereis(wa_runtime_t *rt, const char *name)
{
    wa_pid_t pid = 0;
    uint32_t i;

    if (!name || !name[0])
        return 0;
    pthread_mutex_lock(&rt->lock);
    for (i = 0; i < rt->config.max_actors; i++) {
        actor_t *actor = &rt->actors[i];

        if (actor->kind != KIND_FREE && strcmp(actor->name, name) == 0) {
            pthread_mutex_lock(&actor->lock);
            if (actor->alive)
                pid = actor_pid(actor);
            pthread_mutex_unlock(&actor->lock);
            break;
        }
    }
    pthread_mutex_unlock(&rt->lock);
    return pid;
}

int
wa_monitor(wa_runtime_t *rt, wa_pid_t watcher, wa_pid_t target)
{
    actor_t *actor = actor_slot(rt, target);
    bool found = false;
    int ret = WA_OK;

    if (actor) {
        pthread_mutex_lock(&actor->lock);
        if (actor->kind != KIND_FREE && actor->alive
            && actor_pid(actor) == target) {
            found = true;
            if (actor->n_watchers < WA_MAX_WATCHERS)
                actor->watchers[actor->n_watchers++] = watcher;
            else
                ret = WA_EFULL;
        }
        pthread_mutex_unlock(&actor->lock);
    }
    if (!found)
        actor_notify_down(rt, target, &watcher, 1, WA_EXIT_NOPROC);
    return ret;
}

int
wa_exit(wa_runtime_t *rt, wa_pid_t pid, int32_t reason)
{
    actor_t *actor = actor_slot(rt, pid);
    int ret = WA_ENOENT;

    if (!actor)
        return WA_ENOENT;
    pthread_mutex_lock(&actor->lock);
    if ((actor->kind == KIND_WASM || actor->kind == KIND_NATIVE) && actor->alive
        && actor_pid(actor) == pid) {
        actor->ctl = CTL_STOP;
        actor->ctl_reason = reason;
        if (!actor->scheduled)
            schedule_locked(rt, actor);
        ret = WA_OK;
    }
    pthread_mutex_unlock(&actor->lock);
    return ret;
}

int
wa_stats(wa_runtime_t *rt, const char *name, wa_stats_t *stats)
{
    int ret = WA_ENOENT;
    uint32_t i;

    if (!name || !stats)
        return WA_EINVAL;
    pthread_mutex_lock(&rt->lock);
    for (i = 0; i < rt->config.max_actors; i++) {
        actor_t *actor = &rt->actors[i];

        if (actor->kind != KIND_FREE && strcmp(actor->name, name) == 0) {
            pthread_mutex_lock(&actor->lock);
            *stats = actor->stats;
            stats->alive = actor->alive;
            stats->pid = actor->alive ? actor_pid(actor) : 0;
            stats->mailbox_depth = actor->depth;
            pthread_mutex_unlock(&actor->lock);
            ret = WA_OK;
            break;
        }
    }
    pthread_mutex_unlock(&rt->lock);
    return ret;
}

static bool
cond_init_monotonic(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    bool ok;

    if (pthread_condattr_init(&attr) != 0)
        return false;
    ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0
         && pthread_cond_init(cond, &attr) == 0;
    pthread_condattr_destroy(&attr);
    return ok;
}

wa_runtime_t *
wa_runtime_create(const wa_config_t *config, char *error_buf,
                  uint32_t error_buf_size)
{
    wa_config_t defaults;
    wa_runtime_t *rt;
    size_t stride;
    uint32_t i;

    if (!config) {
        wa_config_default(&defaults);
        config = &defaults;
    }
    if (!config->workers || !config->max_actors || config->max_actors > 0xffff
        || !config->mailbox_capacity || !config->message_pool
        || !config->reductions || !config->max_timers) {
        set_error(error_buf, error_buf_size, "invalid runtime config");
        return NULL;
    }
    if (!wasm_natives_register()) {
        set_error(error_buf, error_buf_size,
                  "can't register the actor imports with WAMR");
        return NULL;
    }
    stride = (sizeof(message_t) + config->max_message_size + 15) & ~(size_t)15;
    if (!(rt = calloc(1, sizeof(*rt)))
        || !(rt->actors = calloc(config->max_actors, sizeof(actor_t)))
        || !(rt->rq = calloc(config->max_actors, sizeof(uint32_t)))
        || !(rt->workers = calloc(config->workers, sizeof(worker_t)))
        || !(rt->pool_mem = malloc(stride * config->message_pool))) {
        set_error(error_buf, error_buf_size, "out of memory");
        goto fail;
    }
    rt->config = *config;
    if (!cond_init_monotonic(&rt->idle_cond)) {
        set_error(error_buf, error_buf_size, "can't create a condition");
        goto fail;
    }
    if (!timers_init(rt)) {
        set_error(error_buf, error_buf_size, "can't create the timers");
        pthread_cond_destroy(&rt->idle_cond);
        goto fail;
    }
    pthread_mutex_init(&rt->lock, NULL);
    pthread_mutex_init(&rt->rq_lock, NULL);
    pthread_mutex_init(&rt->pool_lock, NULL);
    pthread_cond_init(&rt->rq_cond, NULL);

    for (i = 0; i < config->message_pool; i++) {
        message_t *msg = (message_t *)(rt->pool_mem + stride * i);
        msg->next = rt->free_list;
        rt->free_list = msg;
    }
    for (i = 0; i < config->max_actors; i++) {
        rt->actors[i].rt = rt;
        rt->actors[i].index = i;
        rt->actors[i].generation = 1;
        pthread_mutex_init(&rt->actors[i].lock, NULL);
    }
    for (i = 0; i < config->workers; i++)
        rt->workers[i].rt = rt;
    return rt;

fail:
    if (rt) {
        free(rt->pool_mem);
        free(rt->workers);
        free(rt->rq);
        free(rt->actors);
        free(rt);
    }
    return NULL;
}

int
wa_runtime_start(wa_runtime_t *rt)
{
    pthread_attr_t attr;
    uint32_t i;

    if (rt->started)
        return WA_EINVAL;
    if (pthread_attr_init(&attr) != 0)
        return WA_ENOMEM;
    if (rt->config.worker_stack_size)
        pthread_attr_setstacksize(&attr, rt->config.worker_stack_size);
    for (i = 0; i < rt->config.workers; i++) {
        if (pthread_create(&rt->workers[i].thread, &attr, worker_main,
                           &rt->workers[i])
            != 0)
            break;
    }
    pthread_attr_destroy(&attr);
    rt->config.workers = i;
    rt->started = true;
    if (i == 0 || !timers_start(rt))
        return WA_ENOMEM;
    return WA_OK;
}

bool
wa_runtime_wait_idle(wa_runtime_t *rt, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool idle;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&rt->rq_lock);
    while (rt->active > 0) {
        if (pthread_cond_timedwait(&rt->idle_cond, &rt->rq_lock, &deadline)
            == ETIMEDOUT)
            break;
    }
    idle = rt->active == 0;
    pthread_mutex_unlock(&rt->rq_lock);
    return idle;
}

void
wa_runtime_destroy(wa_runtime_t *rt)
{
    uint32_t i;

    if (!rt)
        return;
    if (rt->started) {
        /* Interrupt handlers that are still running, then stop. */
        pthread_mutex_lock(&rt->timer_lock);
        for (i = 0; i < rt->config.workers; i++) {
            if (rt->workers[i].running && rt->workers[i].running->inst)
                wasm_runtime_terminate(rt->workers[i].running->inst);
        }
        pthread_mutex_unlock(&rt->timer_lock);
        pthread_mutex_lock(&rt->rq_lock);
        rt->stopping = true;
        pthread_cond_broadcast(&rt->rq_cond);
        pthread_mutex_unlock(&rt->rq_lock);
        for (i = 0; i < rt->config.workers; i++)
            pthread_join(rt->workers[i].thread, NULL);
        timers_stop(rt);
    }
    if (rt->actors) {
        for (i = 0; i < rt->config.max_actors; i++) {
            actor_t *actor = &rt->actors[i];

            if (actor->kind == KIND_WASM)
                wasm_actor_stop(actor);
            messages_free(rt, actor->head);
            free(actor->init_arg);
            pthread_mutex_destroy(&actor->lock);
        }
    }
    timers_destroy(rt);
    while (rt->modules) {
        module_entry_t *next = rt->modules->next;
        wasm_runtime_unload(rt->modules->module);
        free(rt->modules->buf);
        free(rt->modules);
        rt->modules = next;
    }
    free(rt->pool_mem);
    free(rt->actors);
    free(rt->rq);
    free(rt->workers);
    pthread_cond_destroy(&rt->rq_cond);
    pthread_cond_destroy(&rt->idle_cond);
    pthread_mutex_destroy(&rt->pool_lock);
    pthread_mutex_destroy(&rt->rq_lock);
    pthread_mutex_destroy(&rt->lock);
    free(rt);
}
