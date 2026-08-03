/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* T_START(rounds) plays that many PING/PONG rounds with "pong", then reports
 * T_DONE(rounds) to "reporter". */

#include "wamr_actors_guest.h"
#include "test_tags.h"

static int32_t rounds;

WA_EXPORT("actor_handle")
int32_t
actor_handle(int32_t from, int32_t tag, const uint8_t *data, uint32_t len)
{
    int32_t n;

    if (len != sizeof(n))
        return 0;
    __builtin_memcpy(&n, data, sizeof(n));
    if (tag == T_START) {
        rounds = n;
        n = 1;
        wa_send(WA_WHEREIS("pong"), T_PING, &n, sizeof(n));
    }
    else if (tag == T_PONG) {
        if (n == rounds) {
            wa_send(WA_WHEREIS("reporter"), T_DONE, &n, sizeof(n));
        }
        else {
            n++;
            wa_send(from, T_PING, &n, sizeof(n));
        }
    }
    return 0;
}
