/* =====================================================================
 * benchmarks/bench_reaper_cpp.cpp — C++17 reaper.hpp micro-benchmarks
 *
 * Mirrors the C benchmarks but uses reaper_cpp::Gc, reaper_cpp::make<T>(), and
 * the RAII wrapper.
 * ===================================================================== */

#define REAPER_IMPLEMENTATION
#include "reaper.hpp"

#include <inttypes.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#if defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
#include <time.h>
#define BENCH_HAS_CLOCK_GETTIME 1
#else
#include <time.h>
#define BENCH_HAS_CLOCK_GETTIME 0
#endif

struct bench_node {
    bench_node *left = nullptr;
    bench_node *right = nullptr;
    uint64_t payload[2] = {};
};

namespace reaper_cpp {
template<>
struct traits<bench_node> {
    static constexpr uint8_t tag = 1;
    static constexpr bool traversable = true;
    static constexpr bool needs_finalize = false;
};
} // namespace reaper_cpp

#define BENCH_ROOTS 64

struct bench_host {
    bench_node *roots[BENCH_ROOTS] = {};
    uint64_t allocated = 0;
};

static bench_host g_host;

static void bench_trace(reaper_cpp::Gc& gc, void *obj, uint8_t /*tag*/) {
    bench_node *n = static_cast<bench_node*>(obj);
    gc.mark(n->left);
    gc.mark(n->right);
}

static void bench_roots(reaper_cpp::Gc& gc) {
    for (int i = 0; i < BENCH_ROOTS; i++)
        gc.mark(g_host.roots[i]);
}

static reaper_cpp::Gc bench_create(unsigned minor_per_major) {
    memset(g_host.roots, 0, sizeof(g_host.roots));
    g_host.allocated = 0;
    reaper_cpp::Gc::Config cfg;
    cfg.trace = bench_trace;
    cfg.roots = bench_roots;
    cfg.arena_size = 256 * 1024;
    cfg.minor_per_major = minor_per_major;
    return reaper_cpp::Gc(cfg);
}

static bench_node *bench_new(reaper_cpp::Gc& gc) {
    bench_node *n = gc.make<bench_node>();
    g_host.allocated++;
    return n;
}

static double bench_now(void) {
#if BENCH_HAS_CLOCK_GETTIME
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
    return static_cast<double>(clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

static void bench_report(const char *name, double seconds, uint64_t count,
                         const char *unit) {
    if (count > 0)
        printf("%-40s %10.4f s  %12" PRIu64 " %s  (%12.3f %s/s)\n",
               name, seconds, count, unit, count / seconds, unit);
    else
        printf("%-40s %10.4f s\n", name, seconds);
}

static void bm_allocation_throughput(void) {
    reaper_cpp::Gc gc = bench_create(0);

    const uint64_t n = 1000000;
    double t0 = bench_now();
    for (uint64_t i = 0; i < n; i++)
        (void)bench_new(gc);
    double dt = bench_now() - t0;

    bench_report("C++ allocation throughput (major)", dt, n, "allocs");
}

static void bm_full_collection_latency(void) {
    reaper_cpp::Gc gc = bench_create(0);

    bench_node *root = bench_new(gc);
    g_host.roots[0] = root;
    bench_node *tail = root;
    for (int i = 0; i < 999999; i++) {
        tail->left = bench_new(gc);
        gc.write_barrier(tail);
        tail = tail->left;
    }

    double t0 = bench_now();
    gc.collect();
    double dt = bench_now() - t0;

    bench_report("C++ full major collection (1M live)", dt, 0, "");
}

static void bm_incremental_step_latency(void) {
    reaper_cpp::Gc gc = bench_create(0);

    bench_node *root = bench_new(gc);
    g_host.roots[0] = root;
    for (int i = 0; i < 99; i++) {
        root->left = bench_new(gc);
        gc.write_barrier(root);
        root = root->left;
    }
    for (int i = 0; i < 250000; i++)
        (void)bench_new(gc);

    while (gc.phase() == REAPER_IDLE) {
        (void)gc.step(1);
        if (gc.phase() == REAPER_IDLE)
            (void)bench_new(gc);
    }

    const int steps = 1000;
    double t0 = bench_now();
    for (int i = 0; i < steps && gc.phase() != REAPER_IDLE; i++)
        (void)gc.step(256);
    double dt = bench_now() - t0;

    bench_report("C++ incremental step latency", dt, steps, "steps");
}

static void bm_major_vs_minor(void) {
    reaper_cpp::Gc gc_major = bench_create(0);
    g_host.roots[0] = bench_new(gc_major);
    for (int i = 0; i < 49999; i++) {
        g_host.roots[0]->left = bench_new(gc_major);
        gc_major.write_barrier(g_host.roots[0]);
        g_host.roots[0] = g_host.roots[0]->left;
    }
    double t0 = bench_now();
    gc_major.collect();
    bench_report("C++ major collection (50k live)", bench_now() - t0, 0, "");

    reaper_cpp::Gc gc_minor = bench_create(0);
    g_host.roots[0] = bench_new(gc_minor);
    for (int i = 0; i < 49999; i++) {
        g_host.roots[0]->left = bench_new(gc_minor);
        gc_minor.write_barrier(g_host.roots[0]);
        g_host.roots[0] = g_host.roots[0]->left;
    }
    gc_minor.collect_minor();
    t0 = bench_now();
    gc_minor.collect_minor();
    bench_report("C++ minor collection (50k old gen)", bench_now() - t0, 0, "");
}

int main(void) {
    printf("Reaper C++ %d.%d.%d benchmarks\n",
           REAPER_VERSION_MAJOR, REAPER_VERSION_MINOR, REAPER_VERSION_PATCH);
    printf("--------------------------------------------------------------------------------\n");

    bm_allocation_throughput();
    bm_full_collection_latency();
    bm_incremental_step_latency();
    bm_major_vs_minor();

    return 0;
}
