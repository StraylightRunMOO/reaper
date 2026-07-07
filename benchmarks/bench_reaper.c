/* =====================================================================
 * benchmarks/bench_reaper.c — micro-benchmarks for reaper.h
 *
 * Measures allocation throughput, full-collection latency, incremental
 * step latency, and the difference between major and minor cycles.
 *
 * Compile with the CMake target or manually:
 *   cc -std=c11 -O2 -Iinclude -DREAPER_IMPLEMENTATION \
 *      benchmarks/bench_reaper.c -o reaper_bench
 * ===================================================================== */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
#include <time.h>
#define BENCH_HAS_CLOCK_GETTIME 1
#else
#include <time.h>
#define BENCH_HAS_CLOCK_GETTIME 0
#endif

#define REAPER_IMPLEMENTATION
#include "reaper.h"

/* ------------------------------------------------------------------ */
/* Benchmark host: a binary-tree node graph rooted in a small array.   */
/* ------------------------------------------------------------------ */

typedef struct bench_node {
    struct bench_node *left;
    struct bench_node *right;
    uint64_t payload[2];
} bench_node;

#define BENCH_ROOTS 64

typedef struct bench_host {
    reaper_t *r;
    bench_node *roots[BENCH_ROOTS];
    uint64_t allocated;
} bench_host;

static bench_host g_host;

static void bench_trace(reaper_t *r, void *obj, uint8_t tag, void *ud) {
    (void)tag; (void)ud;
    bench_node *n = (bench_node *)obj;
    reaper_mark(r, n->left);
    reaper_mark(r, n->right);
}

static void bench_roots(reaper_t *r, void *ud) {
    bench_host *h = (bench_host *)ud;
    for (int i = 0; i < BENCH_ROOTS; i++)
        reaper_mark(r, h->roots[i]);
}

static reaper_t *bench_create(unsigned minor_per_major) {
    memset(&g_host, 0, sizeof(g_host));
    reaper_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.trace = bench_trace;
    cfg.roots = bench_roots;
    cfg.ud = &g_host;
    cfg.arena_size = 256 * 1024;
    cfg.minor_per_major = minor_per_major;
    g_host.r = reaper_create(&cfg);
    return g_host.r;
}

static bench_node *bench_new(reaper_t *r) {
    bench_node *n = (bench_node *)reaper_alloc(r, sizeof(bench_node), 1,
                                                REAPER_TRAVERSABLE);
    if (n) {
        n->left = n->right = NULL;
        g_host.allocated++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */

static double bench_now(void) {
#if BENCH_HAS_CLOCK_GETTIME
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void bench_report(const char *name, double seconds, uint64_t count,
                         const char *unit) {
    if (count > 0)
        printf("%-40s %10.4f s  %12" PRIu64 " %s  (%12.3f %s/s)\n",
               name, seconds, count, unit, count / seconds, unit);
    else
        printf("%-40s %10.4f s\n", name, seconds);
}

/* ------------------------------------------------------------------ */
/* Benchmarks                                                          */
/* ------------------------------------------------------------------ */

static void bm_allocation_throughput(void) {
    reaper_t *r = bench_create(0);

    const uint64_t n = 1000000;
    double t0 = bench_now();
    for (uint64_t i = 0; i < n; i++)
        (void)bench_new(r);
    double dt = bench_now() - t0;

    bench_report("Allocation throughput (major)", dt, n, "allocs");
    reaper_destroy(r);
}

static void bm_full_collection_latency(void) {
    reaper_t *r = bench_create(0);

    /* Build a rooted binary tree of ~1M nodes. */
    bench_node *root = bench_new(r);
    g_host.roots[0] = root;
    bench_node *tail = root;
    for (int i = 0; i < 999999; i++) {
        tail->left = bench_new(r);
        reaper_write_barrier(r, tail);
        tail = tail->left;
    }

    double t0 = bench_now();
    reaper_collect(r);
    double dt = bench_now() - t0;

    bench_report("Full major collection (1M live)", dt, 0, "");
    reaper_destroy(r);
}

static void bm_incremental_step_latency(void) {
    reaper_t *r = bench_create(0);

    /* 250k garbage + a small rooted chain so there is real mark work. */
    bench_node *root = bench_new(r);
    g_host.roots[0] = root;
    for (int i = 0; i < 99; i++) {
        root->left = bench_new(r);
        reaper_write_barrier(r, root);
        root = root->left;
    }
    for (int i = 0; i < 250000; i++)
        (void)bench_new(r);

    /* Prime the cycle. */
    while (reaper_phase(r) == REAPER_IDLE) {
        (void)reaper_step(r, 1);
        if (reaper_phase(r) == REAPER_IDLE)
            (void)bench_new(r); /* force heap past trigger */
    }

    /* Time 1000 individual small steps. */
    const int steps = 1000;
    double t0 = bench_now();
    for (int i = 0; i < steps && reaper_phase(r) != REAPER_IDLE; i++)
        (void)reaper_step(r, 256);
    double dt = bench_now() - t0;

    bench_report("Incremental step latency", dt, steps, "steps");
    reaper_destroy(r);
}

static void bm_major_vs_minor(void) {
    /* Major cycle: clear marks, re-trace everything. */
    reaper_t *r_major = bench_create(0);
    g_host.roots[0] = bench_new(r_major);
    for (int i = 0; i < 49999; i++) {
        g_host.roots[0]->left = bench_new(r_major);
        reaper_write_barrier(r_major, g_host.roots[0]);
        g_host.roots[0] = g_host.roots[0]->left;
    }
    double t0 = bench_now();
    reaper_collect(r_major);
    bench_report("Major collection (50k live)", bench_now() - t0, 0, "");
    reaper_destroy(r_major);

    /* Minor cycle: survivors stay black, only young garbage swept. */
    reaper_t *r_minor = bench_create(0);
    g_host.roots[0] = bench_new(r_minor);
    for (int i = 0; i < 49999; i++) {
        g_host.roots[0]->left = bench_new(r_minor);
        reaper_write_barrier(r_minor, g_host.roots[0]);
        g_host.roots[0] = g_host.roots[0]->left;
    }
    /* First minor promotes the 50k-node chain to the old generation. */
    reaper_collect_minor(r_minor);
    t0 = bench_now();
    /* Second minor has almost no mark work because survivors are black. */
    reaper_collect_minor(r_minor);
    bench_report("Minor collection (50k old gen)", bench_now() - t0, 0, "");
    reaper_destroy(r_minor);
}

/* ------------------------------------------------------------------ */

int main(void) {
    printf("Reaper %d.%d.%d benchmarks\n",
           REAPER_VERSION_MAJOR, REAPER_VERSION_MINOR, REAPER_VERSION_PATCH);
    printf("--------------------------------------------------------------------------------\n");

    bm_allocation_throughput();
    bm_full_collection_latency();
    bm_incremental_step_latency();
    bm_major_vs_minor();

    return 0;
}
