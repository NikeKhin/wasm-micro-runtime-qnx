/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WAMR_ACTORS_TEST_TAGS_H
#define WAMR_ACTORS_TEST_TAGS_H

/* Message tags shared by the test actors and the test program. */
enum {
    T_START = 256,
    T_PING,
    T_PONG,
    T_DONE,
    T_STARTED,
    T_CRASH,
    T_ECHO,
    T_SPIN,
    T_INC,
    T_GET,
    T_VALUE,
    T_TICK,
    T_WORK,
    T_STOP,
};

#endif /* end of WAMR_ACTORS_TEST_TAGS_H */
