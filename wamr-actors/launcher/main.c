/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wamr_actors.h"
#include "wasm_export.h"

#define MAX_ACTORS 32
#define MAX_SENDS 32

static void
usage(const char *prog)
{
    printf(
        "usage: %s [options] NAME=FILE ...\n"
        "Loads each FILE (.wasm or .aot) as module NAME and spawns it under a\n"
        "one-for-one supervisor, as a permanent actor registered as NAME.\n"
        "  --workers=N             scheduler threads (default 1)\n"
        "  --restarts=N:MS         supervisor intensity: N restarts in MS "
        "(default 3:5000)\n"
        "  --budget-ms=N           time budget per message (default none)\n"
        "  --heap-size=BYTES       heap per actor for incoming messages\n"
        "  --send=NAME:TAG[:TEXT]  send TEXT to NAME once the actors run, "
        "each\n"
        "                          after the previous one has been handled\n"
        "  --run-ms=N              run this long, otherwise until idle\n",
        prog);
}

static uint8_t *
read_file(const char *path, uint32_t *size)
{
    uint8_t *buf;
    FILE *file;
    long len;

    if (!(file = fopen(path, "rb")))
        return NULL;
    fseek(file, 0, SEEK_END);
    len = ftell(file);
    fseek(file, 0, SEEK_SET);
    buf = len > 0 ? malloc((size_t)len) : NULL;
    if (buf && fread(buf, 1, (size_t)len, file) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(file);
    *size = (uint32_t)len;
    return buf;
}

static void
on_fatal(wa_runtime_t *rt, void *ctx, wa_pid_t supervisor)
{
    (void)rt;
    (void)ctx;
    fprintf(stderr, "supervisor %u gave up, exiting\n", supervisor);
    exit(2);
}

static void
sleep_ms(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

    nanosleep(&ts, NULL);
}

int
main(int argc, char **argv)
{
    const char *names[MAX_ACTORS], *sends[MAX_SENDS];
    uint32_t n_names = 0, n_sends = 0, run_ms = 0, heap_size = 0;
    uint32_t budget_ms = 0, max_restarts = 3, period_ms = 5000, i;
    wa_supervisor_spec_t sup_spec = { 0 };
    char error[128] = { 0 };
    wa_config_t config;
    wa_runtime_t *rt;
    wa_pid_t sup;
    int ret = 1;

    wa_config_default(&config);
    for (i = 1; i < (uint32_t)argc; i++) {
        const char *arg = argv[i];

        if (!strncmp(arg, "--workers=", 10))
            config.workers = (uint32_t)atoi(arg + 10);
        else if (!strncmp(arg, "--restarts=", 11)
                 && sscanf(arg + 11, "%u:%u", &max_restarts, &period_ms) == 2)
            ;
        else if (!strncmp(arg, "--budget-ms=", 12))
            budget_ms = (uint32_t)atoi(arg + 12);
        else if (!strncmp(arg, "--heap-size=", 12))
            heap_size = (uint32_t)atoi(arg + 12);
        else if (!strncmp(arg, "--send=", 7) && n_sends < MAX_SENDS)
            sends[n_sends++] = arg + 7;
        else if (!strncmp(arg, "--run-ms=", 9))
            run_ms = (uint32_t)atoi(arg + 9);
        else if (arg[0] != '-' && strchr(arg, '=') && n_names < MAX_ACTORS)
            names[n_names++] = arg;
        else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!n_names) {
        usage(argv[0]);
        return 1;
    }

    if (!wasm_runtime_init()) {
        fprintf(stderr, "can't initialize WAMR\n");
        return 1;
    }
    if (!(rt = wa_runtime_create(&config, error, sizeof(error)))) {
        fprintf(stderr, "can't create the runtime: %s\n", error);
        goto destroy_wamr;
    }
    wa_set_fatal_handler(rt, on_fatal, NULL);
    sup_spec.name = "main";
    sup_spec.strategy = WA_ONE_FOR_ONE;
    sup_spec.max_restarts = max_restarts;
    sup_spec.period_ms = period_ms;
    sup = wa_supervisor_start(rt, &sup_spec);

    for (i = 0; i < n_names; i++) {
        const char *eq = strchr(names[i], '=');
        char name[32];
        wa_actor_spec_t spec = { 0 };
        uint32_t size;
        uint8_t *buf;

        snprintf(name, sizeof(name), "%.*s", (int)(eq - names[i]), names[i]);
        if (!(buf = read_file(eq + 1, &size))) {
            fprintf(stderr, "can't read %s\n", eq + 1);
            goto destroy_rt;
        }
        if (wa_module_load(rt, name, buf, size, error, sizeof(error))
            != WA_OK) {
            fprintf(stderr, "can't load %s: %s\n", eq + 1, error);
            free(buf);
            goto destroy_rt;
        }
        free(buf);
        spec.module = name;
        spec.name = name;
        spec.supervisor = sup;
        spec.restart = WA_PERMANENT;
        spec.heap_size = heap_size;
        spec.budget_ms = budget_ms;
        if (!wa_spawn(rt, &spec, error, sizeof(error))) {
            fprintf(stderr, "can't spawn %s: %s\n", name, error);
            goto destroy_rt;
        }
    }
    if (wa_runtime_start(rt) != WA_OK) {
        fprintf(stderr, "can't start the workers\n");
        goto destroy_rt;
    }
    wa_runtime_wait_idle(rt, 1000);

    for (i = 0; i < n_sends; i++) {
        char name[32] = { 0 };
        unsigned tag = 0;
        int consumed = 0;
        const char *text;

        if (sscanf(sends[i], "%31[^:]:%u%n", name, &tag, &consumed) < 2) {
            fprintf(stderr, "bad --send=%s\n", sends[i]);
            continue;
        }
        text = sends[i][consumed] == ':' ? sends[i] + consumed + 1 : "";
        if (wa_send(rt, 0, wa_whereis(rt, name), tag, text,
                    (uint32_t)strlen(text))
            != WA_OK)
            fprintf(stderr, "can't send to %s\n", name);
        /* So that the next message reaches the actor as it is then */
        wa_runtime_wait_idle(rt, 1000);
    }

    if (run_ms)
        sleep_ms(run_ms);
    else
        while (!wa_runtime_wait_idle(rt, 1000))
            ;

    printf("%-16s %6s %10s %8s %8s %10s %10s %8s\n", "actor", "alive",
           "messages", "crashes", "restarts", "busy_us", "max_us", "dropped");
    for (i = 0; i < n_names; i++) {
        const char *eq = strchr(names[i], '=');
        char name[32];
        wa_stats_t stats;

        snprintf(name, sizeof(name), "%.*s", (int)(eq - names[i]), names[i]);
        if (wa_stats(rt, name, &stats) == WA_OK)
            printf("%-16s %6s %10llu %8llu %8llu %10llu %10llu %8u\n", name,
                   stats.alive ? "yes" : "no",
                   (unsigned long long)stats.messages,
                   (unsigned long long)stats.crashes,
                   (unsigned long long)stats.restarts,
                   (unsigned long long)stats.busy_us,
                   (unsigned long long)stats.max_handler_us, stats.dropped);
    }
    ret = 0;

destroy_rt:
    wa_runtime_destroy(rt);
destroy_wamr:
    wasm_runtime_destroy();
    return ret;
}
