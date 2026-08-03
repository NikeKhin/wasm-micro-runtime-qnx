/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdlib.h>
#include <time.h>

#include "actor_internal.h"

/* Timers are a binary min-heap on due_us. */

static void
heap_swap(timer_entry_t *a, timer_entry_t *b)
{
    timer_entry_t t = *a;
    *a = *b;
    *b = t;
}

static void
heap_push(wa_runtime_t *rt, timer_entry_t entry)
{
    uint32_t i = rt->n_timers++;

    rt->timers[i] = entry;
    while (i > 0 && rt->timers[(i - 1) / 2].due_us > rt->timers[i].due_us) {
        heap_swap(&rt->timers[(i - 1) / 2], &rt->timers[i]);
        i = (i - 1) / 2;
    }
}

static timer_entry_t
heap_pop(wa_runtime_t *rt)
{
    timer_entry_t top = rt->timers[0];
    uint32_t i = 0;

    rt->timers[0] = rt->timers[--rt->n_timers];
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;

        if (l < rt->n_timers && rt->timers[l].due_us < rt->timers[m].due_us)
            m = l;
        if (r < rt->n_timers && rt->timers[r].due_us < rt->timers[m].due_us)
            m = r;
        if (m == i)
            break;
        heap_swap(&rt->timers[i], &rt->timers[m]);
        i = m;
    }
    return top;
}

bool
timers_init(wa_runtime_t *rt)
{
    pthread_condattr_t attr;
    bool ok;

    if (!(rt->timers = calloc(rt->config.max_timers, sizeof(timer_entry_t))))
        return false;
    if (pthread_condattr_init(&attr) != 0) {
        free(rt->timers);
        return false;
    }
    ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0
         && pthread_cond_init(&rt->timer_cond, &attr) == 0;
    pthread_condattr_destroy(&attr);
    if (!ok) {
        free(rt->timers);
        return false;
    }
    pthread_mutex_init(&rt->timer_lock, NULL);
    return true;
}

int
timer_add(wa_runtime_t *rt, uint32_t delay_ms, wa_pid_t to, message_t *msg)
{
    timer_entry_t entry = { wa_now_us() + (uint64_t)delay_ms * 1000, to, msg };

    pthread_mutex_lock(&rt->timer_lock);
    if (rt->n_timers >= rt->config.max_timers) {
        pthread_mutex_unlock(&rt->timer_lock);
        return WA_EFULL;
    }
    heap_push(rt, entry);
    pthread_cond_signal(&rt->timer_cond);
    pthread_mutex_unlock(&rt->timer_lock);
    return WA_OK;
}

void
watchdog_begin(worker_t *worker, actor_t *actor, uint32_t budget_ms)
{
    wa_runtime_t *rt = worker->rt;

    pthread_mutex_lock(&rt->timer_lock);
    worker->running = actor;
    worker->terminated = false;
    worker->deadline_us =
        budget_ms ? wa_now_us() + (uint64_t)budget_ms * 1000 : 0;
    if (budget_ms)
        pthread_cond_signal(&rt->timer_cond);
    pthread_mutex_unlock(&rt->timer_lock);
}

bool
watchdog_end(worker_t *worker)
{
    wa_runtime_t *rt = worker->rt;
    bool terminated;

    pthread_mutex_lock(&rt->timer_lock);
    terminated = worker->terminated;
    worker->running = NULL;
    worker->deadline_us = 0;
    worker->terminated = false;
    pthread_mutex_unlock(&rt->timer_lock);
    return terminated;
}

/*
 * Delivers due timers and interrupts handlers that overran their budget. The
 * instance a worker runs stays alive while timer_lock is held, since the
 * worker clears running under it before stopping the instance.
 */
static void *
timer_main(void *arg)
{
    wa_runtime_t *rt = arg;
    uint32_t i;

    pthread_mutex_lock(&rt->timer_lock);
    while (!rt->timers_stopping) {
        uint64_t now = wa_now_us(), next = UINT64_MAX;
        struct timespec ts;

        for (i = 0; i < rt->config.workers; i++) {
            worker_t *worker = &rt->workers[i];

            if (!worker->running || !worker->deadline_us || worker->terminated)
                continue;
            if (now >= worker->deadline_us) {
                wasm_runtime_terminate(worker->running->inst);
                worker->terminated = true;
            }
            else if (worker->deadline_us < next) {
                next = worker->deadline_us;
            }
        }

        if (rt->n_timers && rt->timers[0].due_us <= now) {
            timer_entry_t entry = heap_pop(rt);

            pthread_mutex_unlock(&rt->timer_lock);
            message_deliver(rt, entry.to, entry.msg);
            pthread_mutex_lock(&rt->timer_lock);
            continue;
        }
        if (rt->n_timers && rt->timers[0].due_us < next)
            next = rt->timers[0].due_us;

        if (next == UINT64_MAX) {
            pthread_cond_wait(&rt->timer_cond, &rt->timer_lock);
        }
        else {
            ts.tv_sec = (time_t)(next / 1000000);
            ts.tv_nsec = (long)(next % 1000000) * 1000;
            pthread_cond_timedwait(&rt->timer_cond, &rt->timer_lock, &ts);
        }
    }
    pthread_mutex_unlock(&rt->timer_lock);
    return NULL;
}

bool
timers_start(wa_runtime_t *rt)
{
    rt->timer_started =
        pthread_create(&rt->timer_thread, NULL, timer_main, rt) == 0;
    return rt->timer_started;
}

void
timers_stop(wa_runtime_t *rt)
{
    if (!rt->timer_started)
        return;
    pthread_mutex_lock(&rt->timer_lock);
    rt->timers_stopping = true;
    pthread_cond_signal(&rt->timer_cond);
    pthread_mutex_unlock(&rt->timer_lock);
    pthread_join(rt->timer_thread, NULL);
}

void
timers_destroy(wa_runtime_t *rt)
{
    while (rt->n_timers)
        message_free(rt, heap_pop(rt).msg);
    free(rt->timers);
    pthread_cond_destroy(&rt->timer_cond);
    pthread_mutex_destroy(&rt->timer_lock);
}
