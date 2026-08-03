/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* Reports T_STARTED(pid) to "reporter" on every start, traps on T_CRASH,
 * returns its payload on T_ECHO and exits with reason 7 on T_STOP. */

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
    if (tag == T_CRASH)
        __builtin_trap();
    if (tag == T_ECHO)
        wa_send(from, T_ECHO, data, len);
    if (tag == T_STOP)
        return 7;
    return 0;
}
