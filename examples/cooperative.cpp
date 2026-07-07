/* =====================================================================
 * examples/cooperative.cpp — incremental GC driven from a C++ host loop
 *
 * Simulates a cooperatively scheduled runtime that interleaves mutator
 * work with bounded calls to reaper_cpp::Gc::step().
 * ===================================================================== */

#define REAPER_IMPLEMENTATION

#include <cstdio>
#include <cstring>

#include "reaper.hpp"

struct task {
    task *next = nullptr;
    uint64_t id = 0;
};

namespace reaper_cpp {
template<>
struct traits<task> {
    static constexpr uint8_t tag = 1;
    static constexpr bool traversable = true;
    static constexpr bool needs_finalize = false;
};
} // namespace reaper_cpp

#define MAX_ROOTS 16

struct host {
    task *roots[MAX_ROOTS] = {};
    uint64_t next_id = 0;
};

static host g;

int main(void) {
    reaper_cpp::Gc::Config cfg;
    cfg.trace = [](reaper_cpp::Gc& gc, void *obj, uint8_t /*tag*/) {
        task *t = static_cast<task*>(obj);
        gc.mark(t->next);
    };
    cfg.roots = [](reaper_cpp::Gc& gc) {
        for (int i = 0; i < MAX_ROOTS; i++)
            gc.mark(g.roots[i]);
    };
    /* Small arenas so the pacer triggers during this short example. */
    cfg.arena_size = 64 * 1024;
    /* Self-pacing: allocations past the trigger do a little GC work. */
    cfg.assist_budget = 256;

    reaper_cpp::Gc gc(cfg);

    task *list = gc.make<task>();
    g.roots[0] = list;

    printf("running cooperative host for 10000 iterations...\n");
    for (int i = 0; i < 10000; i++) {
        task *t = gc.make<task>();
        t->id = g.next_id++;
        t->next = list;
        gc.write_barrier(t);
        list = t;
        g.roots[0] = list;

        if ((i & 7) == 0)
            (void)gc.step(64);
    }

    while (gc.phase() != REAPER_IDLE)
        (void)gc.step(SIZE_MAX);

    reaper_stats st = gc.stats();
    printf("done. live bytes=%zu, major cycles=%zu, minor cycles=%zu\n",
           st.live_bytes, st.cycles_major, st.cycles_minor);

    return 0;
}
