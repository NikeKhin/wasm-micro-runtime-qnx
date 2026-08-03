/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdlib.h>
#include <string.h>

#include "actor_internal.h"

module_entry_t *
module_current_locked(wa_runtime_t *rt, const char *name)
{
    module_entry_t *entry;

    for (entry = rt->modules; entry; entry = entry->next) {
        if (entry->current && strcmp(entry->name, name) == 0)
            return entry;
    }
    return NULL;
}

static void
module_unlink_locked(wa_runtime_t *rt, module_entry_t *code)
{
    module_entry_t **link;

    for (link = &rt->modules; *link; link = &(*link)->next) {
        if (*link == code) {
            *link = code->next;
            break;
        }
    }
    wasm_runtime_unload(code->module);
    free(code->buf);
    free(code);
}

void
module_release_locked(wa_runtime_t *rt, module_entry_t *code)
{
    if (--code->users == 0 && !code->current)
        module_unlink_locked(rt, code);
}

static int
module_add(wa_runtime_t *rt, const char *name, const uint8_t *buf,
           uint32_t size, bool upgrade, char *error_buf,
           uint32_t error_buf_size)
{
    module_entry_t *entry, *old;

    if (!name || !name[0] || strlen(name) >= WA_NAME_LEN || !buf || !size)
        return WA_EINVAL;
    if (!(entry = calloc(1, sizeof(*entry))) || !(entry->buf = malloc(size))) {
        free(entry);
        return WA_ENOMEM;
    }
    memcpy(entry->buf, buf, size);
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->module =
        wasm_runtime_load(entry->buf, size, error_buf, error_buf_size);
    if (!entry->module) {
        free(entry->buf);
        free(entry);
        return WA_EINVAL;
    }

    pthread_mutex_lock(&rt->lock);
    old = module_current_locked(rt, name);
    if ((old != NULL) != upgrade) {
        pthread_mutex_unlock(&rt->lock);
        if (error_buf && error_buf_size)
            snprintf(error_buf, error_buf_size, "module %s is %s", name,
                     old ? "already loaded" : "not loaded");
        wasm_runtime_unload(entry->module);
        free(entry->buf);
        free(entry);
        return WA_EINVAL;
    }
    entry->version = old ? old->version + 1 : 1;
    entry->current = true;
    entry->next = rt->modules;
    rt->modules = entry;
    if (old) {
        old->current = false;
        if (old->users == 0)
            module_unlink_locked(rt, old);
        rt->code_epoch++;
    }
    pthread_mutex_unlock(&rt->lock);
    return WA_OK;
}

int
wa_module_load(wa_runtime_t *rt, const char *name, const uint8_t *buf,
               uint32_t size, char *error_buf, uint32_t error_buf_size)
{
    return module_add(rt, name, buf, size, false, error_buf, error_buf_size);
}

int
wa_module_upgrade(wa_runtime_t *rt, const char *name, const uint8_t *buf,
                  uint32_t size, char *error_buf, uint32_t error_buf_size)
{
    return module_add(rt, name, buf, size, true, error_buf, error_buf_size);
}

static const char *
actor_label(const actor_t *actor)
{
    return actor->name[0] ? actor->name : actor->module_name;
}

/* Calls into the actor under its budgets. Returns false if it trapped. */
static bool
call_guarded(worker_t *worker, actor_t *actor, wasm_function_inst_t func,
             uint32_t argc, uint32_t *argv)
{
    const char *exception;
    bool ok, terminated;

#if defined(WASM_ENABLE_INSTRUCTION_METERING) \
    && WASM_ENABLE_INSTRUCTION_METERING != 0
    wasm_runtime_set_instruction_count_limit(
        actor->exec_env,
        actor->instruction_budget ? (int)actor->instruction_budget : -1);
#endif
    watchdog_begin(worker, actor, actor->budget_ms);
    ok = wasm_runtime_call_wasm(actor->exec_env, func, argc, argv);
    terminated = watchdog_end(worker);
    if (ok && !terminated)
        return true;

    exception = wasm_runtime_get_exception(actor->inst);
    wa_log_error("%s (pid %u): %s", actor_label(actor), actor_pid(actor),
                 terminated ? "exceeded its time budget"
                            : (exception ? exception : "call failed"));
    return false;
}

/*
 * Copies data into the actor's heap and calls func(ptr, len). Returns false
 * if the call failed, otherwise its result in *ret.
 */
static bool
call_with_buffer(worker_t *worker, actor_t *actor, wasm_function_inst_t func,
                 const uint8_t *data, uint32_t len, int32_t *ret)
{
    uint64_t app = 0;
    void *native = NULL;
    uint32_t argv[2];

    if (len) {
        app = wasm_runtime_module_malloc(actor->inst, len, &native);
        if (!app) {
            wa_log_error("%s: %u bytes don't fit in its heap",
                         actor_label(actor), len);
            return false;
        }
        memcpy(native, data, len);
    }
    argv[0] = (uint32_t)app;
    argv[1] = len;
    if (!call_guarded(worker, actor, func, 2, argv))
        return false;
    if (app)
        wasm_runtime_module_free(actor->inst, app);
    *ret = (int32_t)argv[0];
    return true;
}

void
wasm_actor_stop(actor_t *actor)
{
    wa_runtime_t *rt = actor->rt;

    if (actor->exec_env) {
        wasm_runtime_destroy_exec_env(actor->exec_env);
        actor->exec_env = NULL;
    }
    if (actor->inst) {
        wasm_runtime_deinstantiate(actor->inst);
        actor->inst = NULL;
    }
    if (actor->code) {
        pthread_mutex_lock(&rt->lock);
        module_release_locked(rt, actor->code);
        pthread_mutex_unlock(&rt->lock);
        actor->code = NULL;
    }
    actor->f_init = actor->f_handle = NULL;
    actor->f_snapshot = actor->f_restore = NULL;
}

bool
wasm_actor_start(worker_t *worker, actor_t *actor, const uint8_t *state,
                 uint32_t state_len)
{
    wa_runtime_t *rt = actor->rt;
    struct InstantiationArgs2 *args = NULL;
    char error[128] = { 0 };
    uint32_t epoch = rt->code_epoch;
    int32_t ret = 0;

    pthread_mutex_lock(&rt->lock);
    actor->code = module_current_locked(rt, actor->module_name);
    if (actor->code)
        actor->code->users++;
    pthread_mutex_unlock(&rt->lock);
    actor->code_epoch = epoch;
    if (!actor->code) {
        wa_log_error("%s: module %s isn't loaded", actor_label(actor),
                     actor->module_name);
        return false;
    }

    if (!wasm_runtime_instantiation_args_create(&args)) {
        wa_log_error("%s: out of memory", actor_label(actor));
        goto fail;
    }
    wasm_runtime_instantiation_args_set_default_stack_size(args,
                                                           actor->stack_size);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        args, actor->heap_size);
    if (actor->max_memory_pages)
        wasm_runtime_instantiation_args_set_max_memory_pages(
            args, actor->max_memory_pages);
    wasm_runtime_instantiation_args_set_custom_data(args, actor);
    actor->inst = wasm_runtime_instantiate_ex2(actor->code->module, args, error,
                                               sizeof(error));
    wasm_runtime_instantiation_args_destroy(args);
    if (!actor->inst) {
        wa_log_error("%s: %s", actor_label(actor), error);
        goto fail;
    }
    if (!(actor->exec_env =
              wasm_runtime_create_exec_env(actor->inst, actor->stack_size))) {
        wa_log_error("%s: can't create an execution environment",
                     actor_label(actor));
        goto fail;
    }
    actor->f_handle = wasm_runtime_lookup_function(actor->inst, "actor_handle");
    actor->f_init = wasm_runtime_lookup_function(actor->inst, "actor_init");
    actor->f_snapshot =
        wasm_runtime_lookup_function(actor->inst, "actor_snapshot");
    actor->f_restore =
        wasm_runtime_lookup_function(actor->inst, "actor_restore");
    if (!actor->f_handle) {
        wa_log_error("%s: the module doesn't export actor_handle",
                     actor_label(actor));
        goto fail;
    }

    if (state && actor->f_restore) {
        if (!call_with_buffer(worker, actor, actor->f_restore, state, state_len,
                              &ret))
            goto fail;
    }
    else if (actor->f_init) {
        if (!call_with_buffer(worker, actor, actor->f_init, actor->init_arg,
                              actor->init_arg_len, &ret))
            goto fail;
    }
    if (ret != 0) {
        wa_log_error("%s: %s returned %d", actor_label(actor),
                     state && actor->f_restore ? "actor_restore" : "actor_init",
                     ret);
        goto fail;
    }
    return true;

fail:
    wasm_actor_stop(actor);
    return false;
}

deliver_result_t
wasm_actor_deliver(worker_t *worker, actor_t *actor, const message_t *msg,
                   int32_t *reason)
{
    uint64_t app = 0;
    void *native = NULL;
    uint32_t argv[4];

    if (!actor->inst)
        return DELIVER_CRASH;
    if (msg->len) {
        app = wasm_runtime_module_malloc(actor->inst, msg->len, &native);
        if (!app) {
            wa_log_error("%s: a %u byte message doesn't fit in its heap",
                         actor_label(actor), msg->len);
            return DELIVER_CRASH;
        }
        memcpy(native, msg->data, msg->len);
    }
    argv[0] = msg->from;
    argv[1] = msg->tag;
    argv[2] = (uint32_t)app;
    argv[3] = msg->len;
    if (!call_guarded(worker, actor, actor->f_handle, 4, argv))
        return DELIVER_CRASH;
    if (app)
        wasm_runtime_module_free(actor->inst, app);
    if ((int32_t)argv[0] != 0) {
        *reason = (int32_t)argv[0];
        return DELIVER_EXIT;
    }
    return DELIVER_OK;
}

/* Returns the state the actor hands over, or NULL if it doesn't. */
static uint8_t *
actor_snapshot(worker_t *worker, actor_t *actor, uint32_t *len)
{
    uint32_t cap = actor->rt->config.max_message_size;
    uint64_t app;
    void *native = NULL;
    uint8_t *state;
    uint32_t argv[2];

    if (!actor->f_snapshot
        || !(app = wasm_runtime_module_malloc(actor->inst, cap, &native)))
        return NULL;
    argv[0] = (uint32_t)app;
    argv[1] = cap;
    if (!call_guarded(worker, actor, actor->f_snapshot, 2, argv)) {
        return NULL;
    }
    if ((int32_t)argv[0] < 0 || argv[0] > cap
        || !(state = malloc(argv[0] ? argv[0] : 1))) {
        wasm_runtime_module_free(actor->inst, app);
        return NULL;
    }
    memcpy(state, native, argv[0]);
    *len = argv[0];
    wasm_runtime_module_free(actor->inst, app);
    return state;
}

bool
wasm_actor_upgrade(worker_t *worker, actor_t *actor)
{
    wa_runtime_t *rt = actor->rt;
    module_entry_t *current;
    uint8_t *state;
    uint32_t len = 0;
    bool ok;

    pthread_mutex_lock(&rt->lock);
    current = module_current_locked(rt, actor->module_name);
    pthread_mutex_unlock(&rt->lock);
    actor->code_epoch = rt->code_epoch;
    if (!current || current == actor->code)
        return actor->inst != NULL;

    state = actor_snapshot(worker, actor, &len);
    wasm_actor_stop(actor);
    ok = wasm_actor_start(worker, actor, state, len);
    free(state);
    if (ok) {
        pthread_mutex_lock(&actor->lock);
        actor->stats.upgrades++;
        pthread_mutex_unlock(&actor->lock);
    }
    return ok;
}

static actor_t *
guest_actor(wasm_exec_env_t exec_env)
{
    return wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(exec_env));
}

static int32_t
guest_self(wasm_exec_env_t exec_env)
{
    return (int32_t)actor_pid(guest_actor(exec_env));
}

static int32_t
guest_send(wasm_exec_env_t exec_env, int32_t to, int32_t tag,
           const uint8_t *data, uint32_t len)
{
    actor_t *actor = guest_actor(exec_env);

    return wa_send(actor->rt, actor_pid(actor), (wa_pid_t)to, (uint32_t)tag,
                   data, len);
}

static int32_t
guest_send_after(wasm_exec_env_t exec_env, int32_t to, int32_t tag,
                 const uint8_t *data, uint32_t len, int32_t delay_ms)
{
    actor_t *actor = guest_actor(exec_env);

    if (delay_ms < 0)
        return WA_EINVAL;
    return wa_send_after(actor->rt, actor_pid(actor), (wa_pid_t)to,
                         (uint32_t)tag, data, len, (uint32_t)delay_ms);
}

static bool
copy_name(char *buf, const char *name, uint32_t len)
{
    if (len == 0 || len >= WA_NAME_LEN || memchr(name, '\0', len))
        return false;
    memcpy(buf, name, len);
    buf[len] = '\0';
    return true;
}

static int32_t
guest_whereis(wasm_exec_env_t exec_env, const char *name, uint32_t len)
{
    char buf[WA_NAME_LEN];

    if (!copy_name(buf, name, len))
        return 0;
    return (int32_t)wa_whereis(guest_actor(exec_env)->rt, buf);
}

static int32_t
guest_register(wasm_exec_env_t exec_env, const char *name, uint32_t len)
{
    actor_t *actor = guest_actor(exec_env);
    wa_runtime_t *rt = actor->rt;
    char buf[WA_NAME_LEN];
    uint32_t i;

    if (!copy_name(buf, name, len))
        return WA_EINVAL;
    pthread_mutex_lock(&rt->lock);
    for (i = 0; i < rt->config.max_actors; i++) {
        if (&rt->actors[i] != actor && rt->actors[i].kind != KIND_FREE
            && strcmp(rt->actors[i].name, buf) == 0) {
            pthread_mutex_unlock(&rt->lock);
            return WA_EINVAL;
        }
    }
    snprintf(actor->name, sizeof(actor->name), "%s", buf);
    pthread_mutex_unlock(&rt->lock);
    return WA_OK;
}

static int32_t
guest_monitor(wasm_exec_env_t exec_env, int32_t pid)
{
    actor_t *actor = guest_actor(exec_env);

    return wa_monitor(actor->rt, actor_pid(actor), (wa_pid_t)pid);
}

static int32_t
guest_spawn(wasm_exec_env_t exec_env, const char *module, uint32_t module_len,
            const uint8_t *arg, uint32_t arg_len)
{
    actor_t *actor = guest_actor(exec_env);
    wa_actor_spec_t spec = { 0 };
    char name[WA_NAME_LEN];

    if (!actor->rt->config.guest_spawn || !copy_name(name, module, module_len))
        return WA_EINVAL;
    spec.module = name;
    spec.restart = WA_TEMPORARY;
    spec.init_arg = arg;
    spec.init_arg_len = arg_len;
    spec.budget_ms = actor->budget_ms;
    spec.instruction_budget = actor->instruction_budget;
    return (int32_t)wa_spawn(actor->rt, &spec, NULL, 0);
}

static int64_t
guest_now_us(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int64_t)wa_now_us();
}

static void
guest_log(wasm_exec_env_t exec_env, int32_t level, const char *msg,
          uint32_t len)
{
    actor_t *actor = guest_actor(exec_env);

    fprintf(stderr, "[%s %u] %d: %.*s\n", actor_label(actor), actor_pid(actor),
            level, (int)len, msg);
}

/* clang-format off */
static NativeSymbol natives[] = {
    { "self", (void *)guest_self, "()i", NULL },
    { "send", (void *)guest_send, "(ii*~)i", NULL },
    { "send_after", (void *)guest_send_after, "(ii*~i)i", NULL },
    { "whereis", (void *)guest_whereis, "(*~)i", NULL },
    { "register", (void *)guest_register, "(*~)i", NULL },
    { "monitor", (void *)guest_monitor, "(i)i", NULL },
    { "spawn", (void *)guest_spawn, "(*~*~)i", NULL },
    { "now_us", (void *)guest_now_us, "()I", NULL },
    { "log", (void *)guest_log, "(i*~)", NULL },
};
/* clang-format on */

static pthread_once_t natives_once = PTHREAD_ONCE_INIT;
static bool natives_registered;

static void
register_natives(void)
{
    natives_registered = wasm_runtime_register_natives(
        "actor", natives, sizeof(natives) / sizeof(natives[0]));
}

bool
wasm_natives_register(void)
{
    pthread_once(&natives_once, register_natives);
    return natives_registered;
}
