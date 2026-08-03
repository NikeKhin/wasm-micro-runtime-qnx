/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* Never returns from a T_SPIN, so only a budget can stop it. */

#include "wamr_actors_guest.h"
#include "test_tags.h"

WA_EXPORT("actor_init")
int32_t
actor_init(const uint8_t *arg, uint32_t len)
{
    int32_t self = wa_self();

    (void)arg;
    (void)len;
    wa_send(WA_WHEREIS("reporter"), T_STARTED, &self, sizeof(self));
    return 0;
}

WA_EXPORT("actor_handle")
int32_t
actor_handle(int32_t from, int32_t tag, const uint8_t *data, uint32_t len)
{
    volatile uint32_t spins = 0;

    (void)from;
    (void)data;
    (void)len;
    if (tag == T_SPIN) {
        for (;;)
            spins++;
    }
    return 0;
}
