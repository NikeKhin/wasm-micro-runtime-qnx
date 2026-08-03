/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* Sends itself T_TICK every 20 ms with wa_send_after, and reports
 * T_DONE(microseconds taken) to "reporter" after five ticks. */

#include "wamr_actors_guest.h"
#include "test_tags.h"

static int32_t ticks;
static int64_t started;

WA_EXPORT("actor_init")
int32_t
actor_init(const uint8_t *arg, uint32_t len)
{
    (void)arg;
    (void)len;
    started = wa_now_us();
    return wa_send_after(wa_self(), T_TICK, 0, 0, 20);
}

WA_EXPORT("actor_handle")
int32_t
actor_handle(int32_t from, int32_t tag, const uint8_t *data, uint32_t len)
{
    int32_t elapsed;

    (void)from;
    (void)data;
    (void)len;
    if (tag != T_TICK)
        return 0;
    if (++ticks < 5)
        return wa_send_after(wa_self(), T_TICK, 0, 0, 20);
    elapsed = (int32_t)(wa_now_us() - started);
    wa_send(WA_WHEREIS("reporter"), T_DONE, &elapsed, sizeof(elapsed));
    return 0;
}
