/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * The interface between a wasm actor and the runtime, for actors written in C
 * and compiled to wasm32. An actor exports
 *
 *   int32_t actor_handle(int32_t from, int32_t tag, const uint8_t *data,
 *                        uint32_t len);
 *
 * which the runtime calls once per message. Returning 0 keeps the actor
 * running; any other value makes it exit with that reason. Optionally it also
 * exports
 *
 *   int32_t actor_init(const uint8_t *arg, uint32_t len);
 *
 * called on every start with the spawn argument, where non-zero fails the
 * start, and, to keep its state across a code upgrade,
 *
 *   int32_t actor_snapshot(uint8_t *buf, uint32_t cap);   returns the length
 *   int32_t actor_restore(const uint8_t *buf, uint32_t len);
 */

#ifndef WAMR_ACTORS_GUEST_H
#define WAMR_ACTORS_GUEST_H

#include <stdint.h>

#define WA_EXPORT(name) __attribute__((export_name(name)))
#define WA_IMPORT(name) \
    __attribute__((import_module("actor"), import_name(name)))

#ifndef WA_TAG_DOWN
#define WA_TAG_DOWN 1
#define WA_TAG_USER 256
#endif

WA_IMPORT("self") int32_t wa_self(void);

/* Returns 0, or a negative wamr_actors.h error such as WA_EFULL. */
WA_IMPORT("send")
int32_t
wa_send(int32_t to, int32_t tag, const void *data, uint32_t len);

WA_IMPORT("send_after")
int32_t
wa_send_after(int32_t to, int32_t tag, const void *data, uint32_t len,
              uint32_t delay_ms);

/* Returns the pid of the actor registered under name, or 0. */
WA_IMPORT("whereis") int32_t wa_whereis(const char *name, uint32_t len);

WA_IMPORT("register") int32_t wa_register(const char *name, uint32_t len);

WA_IMPORT("monitor") int32_t wa_monitor(int32_t pid);

/* Spawns an unsupervised actor, if the host allows it. Returns its pid. */
WA_IMPORT("spawn")
int32_t
wa_spawn(const char *module, uint32_t module_len, const void *arg,
         uint32_t arg_len);

/* Monotonic time in microseconds. */
WA_IMPORT("now_us") int64_t wa_now_us(void);

WA_IMPORT("log") void wa_log(int32_t level, const char *msg, uint32_t len);

#define WA_WHEREIS(literal) wa_whereis(literal, sizeof(literal) - 1)
#define WA_LOG(level, literal) wa_log(level, literal, sizeof(literal) - 1)

#endif /* end of WAMR_ACTORS_GUEST_H */
