/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <string.h>

#include "actor_internal.h"

/* Everything else is the actor's decision, or its supervisor's. */
static bool
abnormal(int32_t reason)
{
    return reason < 0 && reason != WA_EXIT_SHUTDOWN;
}

static bool
should_restart(const actor_t *child, int32_t reason)
{
    switch (child->restart) {
        case WA_PERMANENT:
            return true;
        case WA_TRANSIENT:
            return abnormal(reason);
        default:
            return false;
    }
}

/* Returns false if this restart exceeds the supervisor's intensity. */
static bool
record_restart_locked(actor_t *sup)
{
    uint64_t now = wa_now_us(), window = (uint64_t)sup->period_ms * 1000;
    uint32_t i, kept = 0;

    for (i = 0; i < sup->n_restart_times; i++) {
        if (now - sup->restart_times[i] <= window)
            sup->restart_times[kept++] = sup->restart_times[i];
    }
    sup->n_restart_times = kept;
    if (kept >= sup->max_restarts)
        return false;
    sup->restart_times[sup->n_restart_times++] = now;
    return true;
}

static void
supervisor_restart_locked(wa_runtime_t *rt, actor_t *sup);

static void
restart_child_locked(wa_runtime_t *rt, actor_t *child)
{
    if (child->kind == KIND_SUPERVISOR)
        supervisor_restart_locked(rt, child);
    else
        actor_request(rt, child, CTL_RESTART, 0);
}

/*
 * A supervisor that is started again restarts its children, which it stopped
 * when it gave up.
 */
static void
supervisor_restart_locked(wa_runtime_t *rt, actor_t *sup)
{
    uint32_t i;

    pthread_mutex_lock(&sup->lock);
    sup->alive = true;
    sup->stats.restarts++;
    pthread_mutex_unlock(&sup->lock);
    sup->n_restart_times = 0;
    for (i = 0; i < sup->n_children; i++)
        restart_child_locked(rt, &rt->actors[sup->children[i]]);
}

/*
 * Stops the supervisor's subtree. The children keep their slots, so that the
 * supervisor can start them again if its own parent restarts it.
 */
static void
supervisor_stop_tree_locked(wa_runtime_t *rt, actor_t *sup)
{
    wa_pid_t pid, watchers[WA_MAX_WATCHERS];
    message_t *dropped;
    uint32_t i, n;

    for (i = 0; i < sup->n_children; i++) {
        actor_t *child = &rt->actors[sup->children[i]];

        if (child->kind == KIND_SUPERVISOR)
            supervisor_stop_tree_locked(rt, child);
        else
            actor_request(rt, child, CTL_STOP, WA_EXIT_SHUTDOWN);
    }
    n = actor_retire_locked(sup, &pid, watchers, &dropped);
    actor_notify_down(rt, pid, watchers, n, WA_EXIT_SHUTDOWN);
}

void
supervisor_child_exited_locked(wa_runtime_t *rt, actor_t *child, int32_t reason,
                               supervision_result_t *result)
{
    actor_t *sup = actor_find_locked(rt, child->supervisor);
    uint32_t i, pos = 0;

    /*
     * The supervisor stopped the child itself, or gave up: the child waits in
     * its slot until the supervisor is restarted.
     */
    if (!sup || !sup->alive)
        return;

    if (!should_restart(child, reason)) {
        actor_free_locked(rt, child);
        return;
    }

    if (!record_restart_locked(sup)) {
        wa_pid_t sup_pid = actor_pid(sup);

        supervisor_stop_tree_locked(rt, sup);
        if (sup->supervisor)
            supervisor_child_exited_locked(rt, sup, WA_EXIT_SHUTDOWN, result);
        else
            result->fatal = sup_pid;
        return;
    }

    for (i = 0; i < sup->n_children; i++) {
        if (sup->children[i] == child->index) {
            pos = i;
            break;
        }
    }
    for (i = 0; i < sup->n_children; i++) {
        actor_t *sibling = &rt->actors[sup->children[i]];

        if (i == pos || sup->strategy == WA_ONE_FOR_ALL
            || (sup->strategy == WA_REST_FOR_ONE && i > pos))
            restart_child_locked(rt, sibling);
    }
}
