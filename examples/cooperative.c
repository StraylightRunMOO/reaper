/* =====================================================================
 * examples/cooperative.c — incremental GC driven from a host loop
 *
 * In a cooperatively scheduled runtime the mutator interleaves its own
 * work with bounded calls to reaper_step(). This example simulates a
 * host that allocates work for a while and lets the GC advance a little
 * bit each iteration, keeping pause times short.
 * ===================================================================== */

#include <stdio.h>
#include <string.h>

#define REAPER_IMPLEMENTATION
#include "reaper.h"

typedef struct task {
    struct task *next;
    uint64_t id;
} task;

#define MAX_ROOTS 16

typedef struct host {
    reaper_t *r;
    task *roots[MAX_ROOTS];
    uint64_t next_id;
} host;

static host g;

static void trace_task(reaper_t *r, void *obj, uint8_t tag, void *ud) {
    (void)tag; (void)ud;
    task *t = (task *)obj;
    reaper_mark(r, t->next);
}

static void mark_roots(reaper_t *r, void *ud) {
    host *h = (host *)ud;
    for (int i = 0; i < MAX_ROOTS; i++)
        reaper_mark(r, h->roots[i]);
}

static task *new_task(reaper_t *r) {
    task *t = reaper_alloc(r, sizeof(task), 1, REAPER_TRAVERSABLE);
    t->next = NULL;
    t->id = g.next_id++;
    return t;
}

int main(void) {
    reaper_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.trace = trace_task;
    cfg.roots = mark_roots;
    cfg.ud = &g;
    /* Small arenas so the pacer triggers during this short example. */
    cfg.arena_size = 64 * 1024;
    /* Self-pacing: allocations that pass the pacer trigger will do a
     * little GC work automatically. */
    cfg.assist_budget = 256;

    g.r = reaper_create(&cfg);

    task *list = new_task(g.r);
    g.roots[0] = list;

    printf("running cooperative host for 10000 iterations...\n");
    for (int i = 0; i < 10000; i++) {
        /* Mutator work: append a new task to the list. */
        task *t = new_task(g.r);
        t->next = list;
        reaper_write_barrier(g.r, t);
        list = t;
        g.roots[0] = list;

        /* Explicit cooperative slice (in addition to allocation assist). */
        if ((i & 7) == 0)
            (void)reaper_step(g.r, 64);
    }

    /* Finish any in-flight cycle before shutdown. */
    while (reaper_phase(g.r) != REAPER_IDLE)
        (void)reaper_step(g.r, SIZE_MAX);

    reaper_stats st;
    reaper_get_stats(g.r, &st);
    printf("done. live bytes=%zu, major cycles=%zu, minor cycles=%zu\n",
           st.live_bytes, st.cycles_major, st.cycles_minor);

    reaper_destroy(g.r);
    return 0;
}
