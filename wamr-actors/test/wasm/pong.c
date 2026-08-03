/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* Answers every T_PING with a T_PONG carrying the same payload. */

#include "wamr_actors_guest.h"
#include "test_tags.h"

WA_EXPORT("actor_handle")
int32_t
actor_handle(int32_t from, int32_t tag, const uint8_t *data, uint32_t len)
{
    if (tag == T_PING)
        wa_send(from, T_PONG, data, len);
    return 0;
}
