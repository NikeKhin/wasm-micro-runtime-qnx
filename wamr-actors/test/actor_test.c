/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * Runs the test actors in test/wasm on a runtime per scenario. A native
 * "reporter" actor collects what they report. Usage: actor_test WASM_DIR
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_tags.h"
#include "wamr_actors.h"
#include "wasm_export.h"

typedef struct {
    wa_pid_t from;
    uint32_t tag;
    int32_t value;
} report_t;

#define MAX_REPORTS 256

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t report_cond = PTHREAD_COND_INITIALIZER;
static report_t reports[MAX_REPORTS];
static uint32_t n_reports;

static const char *wasm_dir;
static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static void
reporter(wa_runtime_t *rt, void *ctx, wa_pid_t self, wa_pid_t from,
         uint32_t tag, const void *data, uint32_t len)
{
    report_t r = { from, tag, 0 };

    (void)rt;
    (void)ctx;
    (void)self;
    if (tag == WA_TAG_DOWN && len == sizeof(wa_down_t)) {
        wa_down_t down;
        memcpy(&down, data, sizeof(down));
        r.from = down.pid;
        r.value = down.reason;
    }
    else if (len >= sizeof(int32_t)) {
        memcpy(&r.value, data, sizeof(int32_t));
    }
    pthread_mutex_lock(&report_lock);
    if (n_reports < MAX_REPORTS)
        reports[n_reports++] = r;
    pthread_cond_broadcast(&report_cond);
    pthread_mutex_unlock(&report_lock);
}

static uint32_t
count_reports_locked(uint32_t tag, report_t *last)
{
    uint32_t i, n = 0;

    for (i = 0; i < n_reports; i++) {
        if (reports[i].tag == tag) {
            n++;
            if (last)
                *last = reports[i];
        }
    }
    return n;
}

/* Waits until count reports with tag have arrived; returns the last one. */
static bool
wait_reports(uint32_t tag, uint32_t count, uint32_t timeout_ms, report_t *last)
{
    struct timespec deadline;
    bool ok;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&report_lock);
    while (count_reports_locked(tag, NULL) < count) {
        if (pthread_cond_timedwait(&report_cond, &report_lock, &deadline))
            break;
    }
    ok = count_reports_locked(tag, last) >= count;
    pthread_mutex_unlock(&report_lock);
    return ok;
}

static uint8_t *
read_file(const char *name, uint32_t *size)
{
    char path[512];
    uint8_t *buf;
    FILE *file;
    long len;

    snprintf(path, sizeof(path), "%s/%s", wasm_dir, name);
    if (!(file = fopen(path, "rb"))) {
        printf("    can't open %s\n", path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    len = ftell(file);
    fseek(file, 0, SEEK_SET);
    buf = malloc(len > 0 ? (size_t)len : 1);
    if (buf && fread(buf, 1, (size_t)len, file) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(file);
    *size = (uint32_t)len;
    return buf;
}

static bool
load(wa_runtime_t *rt, const char *module, const char *file)
{
    char error[128] = { 0 };
    uint32_t size;
    uint8_t *buf = read_file(file, &size);
    int ret;

    if (!buf)
        return false;
    ret = wa_module_load(rt, module, buf, size, error, sizeof(error));
    free(buf);
    if (ret != WA_OK)
        printf("    can't load %s: %s\n", file, error);
    return ret == WA_OK;
}

static wa_pid_t reporter_pid;

static wa_runtime_t *
setup(uint32_t workers, uint32_t mailbox_capacity, bool start)
{
    char error[128] = { 0 };
    wa_config_t config;
    wa_runtime_t *rt;

    pthread_mutex_lock(&report_lock);
    n_reports = 0;
    pthread_mutex_unlock(&report_lock);
    wa_config_default(&config);
    config.workers = workers;
    if (mailbox_capacity)
        config.mailbox_capacity = mailbox_capacity;
    if (!(rt = wa_runtime_create(&config, error, sizeof(error)))) {
        printf("    can't create the runtime: %s\n", error);
        return NULL;
    }
    reporter_pid = wa_spawn_native(rt, "reporter", reporter, NULL);
    if (start && wa_runtime_start(rt) != WA_OK) {
        wa_runtime_destroy(rt);
        return NULL;
    }
    return rt;
}

static wa_pid_t
spawn(wa_runtime_t *rt, const char *module, const char *name,
      wa_pid_t supervisor, wa_restart_t restart, uint32_t budget_ms)
{
    char error[128] = { 0 };
    wa_actor_spec_t spec = { 0 };
    wa_pid_t pid;

    spec.module = module;
    spec.name = name;
    spec.supervisor = supervisor;
    spec.restart = restart;
    spec.budget_ms = budget_ms;
    if (!(pid = wa_spawn(rt, &spec, error, sizeof(error))))
        printf("    can't spawn %s: %s\n", name, error);
    return pid;
}

static wa_pid_t
supervisor(wa_runtime_t *rt, const char *name, wa_strategy_t strategy,
           uint32_t max_restarts)
{
    wa_supervisor_spec_t spec = { 0 };

    spec.name = name;
    spec.strategy = strategy;
    spec.max_restarts = max_restarts;
    spec.period_ms = 10000;
    return wa_supervisor_start(rt, &spec);
}

static void
test_ping_pong(void)
{
    wa_runtime_t *rt = setup(2, 0, true);
    int32_t rounds = 1000;
    report_t done;
    wa_stats_t stats;

    if (!rt || !load(rt, "ping", "ping.wasm") || !load(rt, "pong", "pong.wasm"))
        goto out;
    CHECK(spawn(rt, "pong", "pong", 0, WA_TEMPORARY, 0));
    CHECK(spawn(rt, "ping", "ping", 0, WA_TEMPORARY, 0));
    CHECK(
        wa_send(rt, 0, wa_whereis(rt, "ping"), T_START, &rounds, sizeof(rounds))
        == WA_OK);
    CHECK(wait_reports(T_DONE, 1, 5000, &done) && done.value == rounds);
    CHECK(wa_stats(rt, "pong", &stats) == WA_OK
          && stats.messages == (uint64_t)rounds);
out:
    wa_runtime_destroy(rt);
}

static void
test_one_for_one(void)
{
    wa_runtime_t *rt = setup(2, 0, true);
    report_t first, second, down, third;
    wa_pid_t sup;
    wa_stats_t stats;

    if (!rt || !load(rt, "crasher", "crasher.wasm"))
        goto out;
    sup = supervisor(rt, "sup", WA_ONE_FOR_ONE, 5);
    CHECK(spawn(rt, "crasher", "permanent", sup, WA_PERMANENT, 0));
    CHECK(spawn(rt, "crasher", "transient", sup, WA_TRANSIENT, 0));
    CHECK(wait_reports(T_STARTED, 2, 2000, NULL));

    /* A crash restarts the child at a new pid, and its monitor fires. */
    first.value = (int32_t)wa_whereis(rt, "permanent");
    CHECK(wa_monitor(rt, reporter_pid, (wa_pid_t)first.value) == WA_OK);
    CHECK(wa_send(rt, 0, (wa_pid_t)first.value, T_CRASH, NULL, 0) == WA_OK);
    CHECK(wait_reports(WA_TAG_DOWN, 1, 2000, &down)
          && down.from == (wa_pid_t)first.value && down.value == WA_EXIT_CRASH);
    CHECK(wait_reports(T_STARTED, 3, 2000, &second)
          && second.value != first.value);
    CHECK(wa_whereis(rt, "permanent") == (wa_pid_t)second.value);
    CHECK(wa_send(rt, 0, (wa_pid_t)first.value, T_ECHO, NULL, 0) == WA_ENOENT);

    /* A normal exit restarts a permanent child but not a transient one. */
    CHECK(wa_send(rt, 0, wa_whereis(rt, "permanent"), T_STOP, NULL, 0)
          == WA_OK);
    CHECK(wait_reports(T_STARTED, 4, 2000, &third)
          && third.value != second.value);
    CHECK(wa_send(rt, 0, wa_whereis(rt, "transient"), T_STOP, NULL, 0)
          == WA_OK);
    CHECK(wa_runtime_wait_idle(rt, 2000));
    CHECK(wa_whereis(rt, "transient") == 0);

    CHECK(wa_stats(rt, "permanent", &stats) == WA_OK && stats.crashes == 1
          && stats.restarts == 2);

    /* Monitoring an actor that is gone reports it at once. */
    CHECK(wa_monitor(rt, reporter_pid, (wa_pid_t)first.value) == WA_OK);
    CHECK(wait_reports(WA_TAG_DOWN, 2, 2000, &down)
          && down.value == WA_EXIT_NOPROC);
out:
    wa_runtime_destroy(rt);
}

static wa_pid_t fatal_supervisor;

static void
on_fatal(wa_runtime_t *rt, void *ctx, wa_pid_t sup)
{
    (void)rt;
    (void)ctx;
    pthread_mutex_lock(&report_lock);
    fatal_supervisor = sup;
    pthread_cond_broadcast(&report_cond);
    pthread_mutex_unlock(&report_lock);
}

static void
test_intensity(void)
{
    wa_runtime_t *rt = setup(1, 0, true);
    wa_pid_t sup, child;
    uint32_t i;

    if (!rt || !load(rt, "crasher", "crasher.wasm"))
        goto out;
    fatal_supervisor = 0;
    wa_set_fatal_handler(rt, on_fatal, NULL);
    sup = supervisor(rt, "sup", WA_ONE_FOR_ONE, 2);
    CHECK(spawn(rt, "crasher", "crasher", sup, WA_PERMANENT, 0));
    /* Two restarts are allowed; the third crash makes the supervisor give
     * up, and as a top-level supervisor it calls the fatal handler. */
    for (i = 1; i <= 3; i++) {
        CHECK(wait_reports(T_STARTED, i, 2000, NULL));
        child = wa_whereis(rt, "crasher");
        CHECK(wa_send(rt, 0, child, T_CRASH, NULL, 0) == WA_OK);
    }
    CHECK(wa_runtime_wait_idle(rt, 2000));
    pthread_mutex_lock(&report_lock);
    CHECK(fatal_supervisor == sup);
    pthread_mutex_unlock(&report_lock);
    CHECK(wa_whereis(rt, "crasher") == 0);
    CHECK(wa_whereis(rt, "sup") == 0);
out:
    wa_runtime_destroy(rt);
}

static void
test_strategies(void)
{
    wa_runtime_t *rt = setup(2, 0, true);
    wa_pid_t all, rest, a, b, c, d, e;

    if (!rt || !load(rt, "crasher", "crasher.wasm"))
        goto out;

    all = supervisor(rt, "all", WA_ONE_FOR_ALL, 5);
    spawn(rt, "crasher", "a", all, WA_PERMANENT, 0);
    spawn(rt, "crasher", "b", all, WA_PERMANENT, 0);
    CHECK(wait_reports(T_STARTED, 2, 2000, NULL));
    a = wa_whereis(rt, "a");
    b = wa_whereis(rt, "b");
    CHECK(wa_send(rt, 0, a, T_CRASH, NULL, 0) == WA_OK);
    CHECK(wait_reports(T_STARTED, 4, 2000, NULL));
    CHECK(wa_runtime_wait_idle(rt, 2000));
    CHECK(wa_whereis(rt, "a") && wa_whereis(rt, "a") != a);
    CHECK(wa_whereis(rt, "b") && wa_whereis(rt, "b") != b);

    rest = supervisor(rt, "rest", WA_REST_FOR_ONE, 5);
    spawn(rt, "crasher", "c", rest, WA_PERMANENT, 0);
    spawn(rt, "crasher", "d", rest, WA_PERMANENT, 0);
    spawn(rt, "crasher", "e", rest, WA_PERMANENT, 0);
    CHECK(wait_reports(T_STARTED, 7, 2000, NULL));
    c = wa_whereis(rt, "c");
    d = wa_whereis(rt, "d");
    e = wa_whereis(rt, "e");
    CHECK(wa_send(rt, 0, d, T_CRASH, NULL, 0) == WA_OK);
    CHECK(wait_reports(T_STARTED, 9, 2000, NULL));
    CHECK(wa_runtime_wait_idle(rt, 2000));
    CHECK(wa_whereis(rt, "c") == c);
    CHECK(wa_whereis(rt, "d") && wa_whereis(rt, "d") != d);
    CHECK(wa_whereis(rt, "e") && wa_whereis(rt, "e") != e);
out:
    wa_runtime_destroy(rt);
}

static void
test_timers(void)
{
    wa_runtime_t *rt = setup(1, 0, true);
    report_t done;

    if (!rt || !load(rt, "ticker", "ticker.wasm"))
        goto out;
    CHECK(spawn(rt, "ticker", "ticker", 0, WA_TEMPORARY, 0));
    /* Five ticks 20 ms apart. */
    CHECK(wait_reports(T_DONE, 1, 2000, &done) && done.value >= 100000
          && done.value < 1000000);
out:
    wa_runtime_destroy(rt);
}

static void
test_budget(void)
{
    wa_runtime_t *rt = setup(1, 0, true);
    wa_stats_t stats;
    wa_pid_t sup;

    if (!rt || !load(rt, "spinner", "spinner.wasm"))
        goto out;
    sup = supervisor(rt, "sup", WA_ONE_FOR_ONE, 5);
    CHECK(spawn(rt, "spinner", "spinner", sup, WA_PERMANENT, 50));
    CHECK(wait_reports(T_STARTED, 1, 2000, NULL));
    CHECK(wa_send(rt, 0, wa_whereis(rt, "spinner"), T_SPIN, NULL, 0) == WA_OK);
    /* The watchdog interrupts the loop and the supervisor restarts it. */
    CHECK(wait_reports(T_STARTED, 2, 3000, NULL));
    CHECK(wa_stats(rt, "spinner", &stats) == WA_OK && stats.crashes == 1
          && stats.max_handler_us >= 50000 && stats.max_handler_us < 1000000);
out:
    wa_runtime_destroy(rt);
}

#if defined(WASM_ENABLE_INSTRUCTION_METERING) \
    && WASM_ENABLE_INSTRUCTION_METERING != 0
static void
test_instruction_budget(void)
{
    wa_runtime_t *rt = setup(1, 0, true);
    wa_actor_spec_t spec = { 0 };
    wa_stats_t stats;
    wa_pid_t sup;

    if (!rt || !load(rt, "spinner", "spinner.wasm"))
        goto out;
    sup = supervisor(rt, "sup", WA_ONE_FOR_ONE, 5);
    spec.module = "spinner";
    spec.name = "spinner";
    spec.supervisor = sup;
    spec.instruction_budget = 1000000;
    CHECK(wa_spawn(rt, &spec, NULL, 0));
    CHECK(wait_reports(T_STARTED, 1, 2000, NULL));
    CHECK(wa_send(rt, 0, wa_whereis(rt, "spinner"), T_SPIN, NULL, 0) == WA_OK);
    CHECK(wait_reports(T_STARTED, 2, 3000, NULL));
    CHECK(wa_stats(rt, "spinner", &stats) == WA_OK && stats.crashes == 1);
out:
    wa_runtime_destroy(rt);
}
#endif

static int32_t
get_count(wa_runtime_t *rt, uint32_t answers)
{
    report_t value = { 0 };

    wa_send(rt, reporter_pid, wa_whereis(rt, "counter"), T_GET, NULL, 0);
    if (!wait_reports(T_VALUE, answers, 2000, &value))
        return -1;
    return value.value;
}

static void
test_upgrade(void)
{
    wa_runtime_t *rt = setup(1, 0, true);
    char error[128] = { 0 };
    wa_stats_t stats;
    uint8_t *buf;
    uint32_t size, i;

    if (!rt || !load(rt, "counter", "counter_v1.wasm"))
        goto out;
    CHECK(spawn(rt, "counter", "counter", 0, WA_TEMPORARY, 0));
    for (i = 0; i < 3; i++)
        wa_send(rt, 0, wa_whereis(rt, "counter"), T_INC, NULL, 0);
    CHECK(get_count(rt, 1) == 3);

    /* The new code keeps the count and answers differently. */
    if (!(buf = read_file("counter_v2.wasm", &size)))
        goto out;
    CHECK(wa_module_upgrade(rt, "counter", buf, size, error, sizeof(error))
          == WA_OK);
    free(buf);
    CHECK(get_count(rt, 2) == 1003);
    wa_send(rt, 0, wa_whereis(rt, "counter"), T_INC, NULL, 0);
    CHECK(get_count(rt, 3) == 1004);
    CHECK(wa_stats(rt, "counter", &stats) == WA_OK && stats.upgrades == 1);
out:
    wa_runtime_destroy(rt);
}

static void
test_backpressure(void)
{
    wa_runtime_t *rt = setup(1, 4, false);
    wa_stats_t stats;
    uint32_t i;

    if (!rt)
        return;
    /* Nothing runs before wa_runtime_start, so the mailbox fills up. */
    for (i = 0; i < 4; i++)
        CHECK(wa_send(rt, 0, reporter_pid, T_ECHO, &i, sizeof(i)) == WA_OK);
    CHECK(wa_send(rt, 0, reporter_pid, T_ECHO, &i, sizeof(i)) == WA_EFULL);
    CHECK(wa_stats(rt, "reporter", &stats) == WA_OK && stats.dropped == 1
          && stats.mailbox_depth == 4);
    CHECK(wa_runtime_start(rt) == WA_OK);
    CHECK(wait_reports(T_ECHO, 4, 2000, NULL));
    wa_runtime_destroy(rt);
}

/* Eight actors work at once; returns the time taken in ms, or -1. */
static long
run_workers(uint32_t workers)
{
    wa_runtime_t *rt = setup(workers, 0, true);
    uint32_t n = 3000000, x = 2463534242u, i;
    struct timespec t0, t1;
    long ms = -1;
    char name[8];

    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    if (!rt || !load(rt, "worker", "worker.wasm"))
        goto out;
    for (i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "w%u", i);
        CHECK(spawn(rt, "worker", name, 0, WA_TEMPORARY, 0));
    }
    CHECK(wa_runtime_wait_idle(rt, 2000));
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "w%u", i);
        CHECK(wa_send(rt, 0, wa_whereis(rt, name), T_WORK, &n, sizeof(n))
              == WA_OK);
    }
    if (wait_reports(T_DONE, 8, 60000, NULL)) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (long)((t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        pthread_mutex_lock(&report_lock);
        for (i = 0; i < n_reports; i++) {
            if (reports[i].tag == T_DONE)
                CHECK((uint32_t)reports[i].value == x);
        }
        pthread_mutex_unlock(&report_lock);
    }
    CHECK(ms >= 0);
out:
    wa_runtime_destroy(rt);
    return ms;
}

static void
test_parallel(void)
{
    long one = run_workers(1), four = run_workers(4);

    printf("    8 actors: %ld ms on 1 worker, %ld ms on 4 workers\n", one,
           four);
    /* Sanitizers distort the timing. */
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    if (sysconf(_SC_NPROCESSORS_ONLN) >= 4)
        CHECK(four > 0 && one > 2 * four);
#endif
}

int
main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } tests[] = {
        { "ping_pong", test_ping_pong },
        { "one_for_one", test_one_for_one },
        { "intensity", test_intensity },
        { "strategies", test_strategies },
        { "timers", test_timers },
        { "budget", test_budget },
#if defined(WASM_ENABLE_INSTRUCTION_METERING) \
    && WASM_ENABLE_INSTRUCTION_METERING != 0
        { "instructions", test_instruction_budget },
#endif
        { "upgrade", test_upgrade },
        { "backpressure", test_backpressure },
        { "parallel", test_parallel },
    };
    uint32_t i;

    if (argc != 2) {
        printf("usage: %s WASM_DIR\n", argv[0]);
        return 2;
    }
    wasm_dir = argv[1];
    if (!wasm_runtime_init()) {
        printf("can't initialize WAMR\n");
        return 2;
    }
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        struct timespec t0, t1;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        tests[i].run();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("%-14s %s (%ld ms)\n", tests[i].name,
               failures == before ? "ok" : "FAILED",
               (long)((t1.tv_sec - t0.tv_sec) * 1000
                      + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    }
    wasm_runtime_destroy();
    printf("%s\n", failures ? "some tests failed" : "all tests passed");
    return failures ? 1 : 0;
}
