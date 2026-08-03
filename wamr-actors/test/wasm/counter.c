/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* Counts T_INC and answers T_GET with T_VALUE(count). Compiled a second time
 * with COUNTER_VERSION=2, which adds 1000 to its answers, to test upgrades. */

#include "wamr_actors_guest.h"
#include "test_tags.h"

#ifndef COUNTER_VERSION
#define COUNTER_VERSION 1
#endif

static int32_t count;

WA_EXPORT("actor_handle")
int32_t
actor_handle(int32_t from, int32_t tag, const uint8_t *data, uint32_t len)
{
    int32_t value;

    (void)data;
    (void)len;
    if (tag == T_INC) {
        count++;
    }
    else if (tag == T_GET) {
        value = COUNTER_VERSION == 1 ? count : count + 1000;
        wa_send(from, T_VALUE, &value, sizeof(value));
    }
    return 0;
}

WA_EXPORT("actor_snapshot")
int32_t
actor_snapshot(uint8_t *buf, uint32_t cap)
{
    if (cap < sizeof(count))
        return -1;
    __builtin_memcpy(buf, &count, sizeof(count));
    return sizeof(count);
}

WA_EXPORT("actor_restore")
int32_t
actor_restore(const uint8_t *buf, uint32_t len)
{
    if (len != sizeof(count))
        return -1;
    __builtin_memcpy(&count, buf, sizeof(count));
    return 0;
}
