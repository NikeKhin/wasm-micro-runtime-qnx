/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* T_WORK(n) runs n rounds of xorshift32 and reports T_DONE(result) to
 * "reporter", to keep several workers busy at once. */

#include "wamr_actors_guest.h"
#include "test_tags.h"

WA_EXPORT("actor_handle")
int32_t
actor_handle(int32_t from, int32_t tag, const uint8_t *data, uint32_t len)
{
    uint32_t n, i, x = 2463534242u;

    (void)from;
    if (tag != T_WORK || len != sizeof(n))
        return 0;
    __builtin_memcpy(&n, data, sizeof(n));
    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    wa_send(WA_WHEREIS("reporter"), T_DONE, &x, sizeof(x));
    return 0;
}
