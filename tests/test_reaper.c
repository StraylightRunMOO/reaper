/* =====================================================================
 * test_reaper.c — reaper.h quad-color GC test suite
 *
 * Test host: a graph of rt_node objects (left/right child pointers plus
 * an optional non-traversable blob), rooted in a fixed slot array. Every
 * node carries a serial + magic word; the finalizer records deaths in a
 * shadow table so use-after-collect and double-finalize are detectable.
 * ===================================================================== */

#include "reaper.h"

#include <stdint.h>
#include <string.h>

#define RT_TAG_NODE  1
#define RT_TAG_BLOB  2
#define RT_MAX_ROOTS 64
#define RT_MAX_SERIAL 65536
#define RT_MAGIC 0xC0FFEE00u

typedef struct rt_node {
    struct rt_node *left, *right;
    void *blob;
    uint32_t magic;
    uint32_t serial;
} rt_node;

typedef struct rt_host {
    reaper_t *r;
    rt_node *roots[RT_MAX_ROOTS];
    int finalized;
    uint32_t next_serial;
} rt_host;

static rt_host g_h;
static uint8_t rt_dead[RT_MAX_SERIAL];

static void rt_trace(reaper_t *r, void *obj, uint8_t tag, void *ud) {
    (void)ud;
    if (tag != RT_TAG_NODE) return;
    rt_node *n = (rt_node *)obj;
    reaper_mark(r, n->left);
    reaper_mark(r, n->right);
    reaper_mark(r, n->blob);
}

static void rt_roots(reaper_t *r, void *ud) {
    rt_host *h = (rt_host *)ud;
    for (int i = 0; i < RT_MAX_ROOTS; i++)
        reaper_mark(r, h->roots[i]);
}

static void rt_finalize(void *obj, uint8_t tag, void *ud) {
    rt_host *h = (rt_host *)ud;
    h->finalized++;
    if (tag == RT_TAG_NODE) {
        rt_node *n = (rt_node *)obj;
        if (n->serial < RT_MAX_SERIAL) rt_dead[n->serial] = 1;
        n->magic = 0xDEADDEADu;
    }
}

static reaper_t *rt_create(unsigned minor_per_major, size_t assist) {
    memset(&g_h, 0, sizeof(g_h));
    memset(rt_dead, 0, sizeof(rt_dead));
    reaper_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.trace = rt_trace;
    cfg.roots = rt_roots;
    cfg.finalize = rt_finalize;
    cfg.ud = &g_h;
    cfg.arena_size = 64 * 1024;   /* small arenas: multi-arena + huge paths */
    cfg.minor_per_major = minor_per_major;
    cfg.assist_budget = assist;
    g_h.r = reaper_create(&cfg);
    return g_h.r;
}

static rt_node *rt_new(reaper_t *r, unsigned extra_flags) {
    rt_node *n = (rt_node *)reaper_alloc(r, sizeof(rt_node), RT_TAG_NODE,
                                         REAPER_TRAVERSABLE | extra_flags);
    if (!n) return NULL;
    n->left = n->right = NULL;
    n->blob = NULL;
    n->serial = g_h.next_serial++;
    n->magic = RT_MAGIC ^ n->serial;
    return n;
}

static bool rt_ok(const rt_node *n) {
    return n->magic == (RT_MAGIC ^ n->serial) &&
           (n->serial >= RT_MAX_SERIAL || !rt_dead[n->serial]);
}

/* Iterative reachability walk using the header userdata as visit stamp. */
static rt_node *rt_wstack[RT_MAX_SERIAL];

static size_t rt_walk_verify(uint16_t stamp, int *bad) {
    size_t top = 0, seen = 0;
    *bad = 0;
    for (int i = 0; i < RT_MAX_ROOTS; i++)
        if (g_h.roots[i]) rt_wstack[top++] = g_h.roots[i];
    while (top) {
        rt_node *n = rt_wstack[--top];
        if (reaper_obj_userdata(n) == stamp) continue;
        reaper_obj_set_userdata(n, stamp);
        seen++;
        if (!rt_ok(n)) (*bad)++;
        if (n->left)  rt_wstack[top++] = n->left;
        if (n->right) rt_wstack[top++] = n->right;
    }
    return seen;
}

/* Deterministic LCG so failures reproduce */
static uint32_t rt_rng;
static uint32_t rt_rand(void) {
    rt_rng = rt_rng * 1664525u + 1013904223u;
    return rt_rng >> 8;
}

/* ------------------------------------------------------------------ */

TEST(reaper_create_destroy_finalizes) {
    reaper_t *r = rt_create(0, 0);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(reaper_phase(r), REAPER_IDLE);

    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_EQ(st.arenas, (size_t)0);
    ASSERT_EQ(st.cycles_major, (size_t)0);

    /* pending finalizables are finalized on destroy */
    rt_new(r, REAPER_FINALIZE);
    rt_new(r, REAPER_FINALIZE);
    rt_new(r, REAPER_FINALIZE);
    reaper_destroy(r);
    ASSERT_EQ(g_h.finalized, 3);
}

TEST(reaper_alloc_basics) {
    reaper_t *r = rt_create(0, 0);
    rt_node *n = rt_new(r, 0);
    ASSERT_NOT_NULL(n);

    ASSERT_EQ(reaper_obj_size(n), sizeof(rt_node));
    ASSERT_EQ(reaper_obj_tag(n), RT_TAG_NODE);
    ASSERT_EQ(reaper_obj_userdata(n), 0);
    reaper_obj_set_userdata(n, 0xBEEF);
    ASSERT_EQ(reaper_obj_userdata(n), 0xBEEF);
    reaper_obj_set_userdata(n, 0);

    /* newborns are light gray: barrier-exempt, still white to the sweep */
    ASSERT_EQ(reaper_color(r, n), REAPER_LIGHT_GRAY);
    ASSERT_EQ(reaper_phase(r), REAPER_IDLE);

    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_EQ(st.arenas, (size_t)1);
    reaper_destroy(r);
}

TEST(reaper_collect_keeps_rooted_frees_garbage) {
    reaper_t *r = rt_create(0, 0);

    rt_node *root = rt_new(r, 0);
    root->left = rt_new(r, 0);
    root->right = rt_new(r, 0);
    g_h.roots[0] = root;

    for (int i = 0; i < 100; i++)
        rt_new(r, REAPER_FINALIZE);   /* garbage, dies young */

    reaper_collect(r);
    ASSERT_EQ(g_h.finalized, 100);
    ASSERT_TRUE(rt_ok(root));
    ASSERT_TRUE(rt_ok(root->left));
    ASSERT_TRUE(rt_ok(root->right));

    /* a major sweep flips survivors black -> white */
    ASSERT_EQ(reaper_color(r, root), REAPER_WHITE);
    ASSERT_EQ(reaper_color(r, root->left), REAPER_WHITE);

    /* dropping the root condemns the whole subtree next cycle */
    root->left = NULL;   /* detach: no barrier needed for removals */
    reaper_collect(r);
    ASSERT_EQ(g_h.finalized, 100);   /* left had no FINALIZE flag */
    ASSERT_TRUE(rt_ok(root));
    reaper_destroy(r);
}

TEST(reaper_write_barrier_idle_white) {
    reaper_t *r = rt_create(0, 0);
    rt_node *n = rt_new(r, 0);
    g_h.roots[0] = n;
    reaper_collect(r);
    ASSERT_EQ(reaper_color(r, n), REAPER_WHITE);

    /* white -> light gray: flag only, no queueing */
    n->left = rt_new(r, 0);
    reaper_write_barrier(r, n);
    ASSERT_EQ(reaper_color(r, n), REAPER_LIGHT_GRAY);

    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_TRUE(st.barrier_hits >= 1);

    reaper_collect(r);
    ASSERT_TRUE(rt_ok(n->left));
    reaper_destroy(r);
}

TEST(reaper_write_barrier_mid_mark) {
    reaper_t *r = rt_create(0, 0);

    rt_node *root = rt_new(r, 0);
    root->left = rt_new(r, 0);
    g_h.roots[0] = root;

    /* arm the pacer (trigger floor is one arena = 64 KiB) */
    for (int i = 0; i < 2500; i++) rt_new(r, 0);

    /* one tiny step: cycle begins, root is traced black, child queued */
    ASSERT_TRUE(reaper_needs_step(r));
    (void)reaper_step(r, 1);
    ASSERT_EQ(reaper_phase(r), REAPER_MARK);
    ASSERT_EQ(reaper_color(r, root), REAPER_BLACK);

    /* mutate the black root mid-cycle; without the backward barrier the
     * atomic root re-scan would NOT re-trace root (already marked) and
     * the newborn would be swept */
    rt_node *b = rt_new(r, REAPER_FINALIZE);
    root->right = b;
    reaper_write_barrier(r, root);
    ASSERT_EQ(reaper_color(r, root), REAPER_DARK_GRAY);

    while (reaper_step(r, 1024)) { /* drain the cycle */ }
    ASSERT_EQ(reaper_phase(r), REAPER_IDLE);
    ASSERT_EQ(g_h.finalized, 0);          /* b survived */
    ASSERT_TRUE(rt_ok(b));
    ASSERT_TRUE(rt_ok(root->left));
    reaper_destroy(r);
}

TEST(reaper_incremental_steps) {
    reaper_t *r = rt_create(0, 0);

    /* rooted chain of 1000 gives real mark work; 2500 garbage nodes give
     * real sweep work across several arenas */
    rt_node *head = rt_new(r, 0);
    g_h.roots[0] = head;
    rt_node *tail = head;
    for (int i = 0; i < 999; i++) {
        tail->left = rt_new(r, 0);
        reaper_write_barrier(r, tail);
        tail = tail->left;
    }
    for (int i = 0; i < 2500; i++) rt_new(r, REAPER_FINALIZE);

    ASSERT_TRUE(reaper_needs_step(r));
    bool saw_mark = false, saw_sweep = false;
    int iters = 0;
    while (reaper_step(r, 32)) {
        if (reaper_phase(r) == REAPER_MARK)  saw_mark = true;
        if (reaper_phase(r) == REAPER_SWEEP) saw_sweep = true;
        /* the mutator keeps allocating between slices */
        if ((iters++ & 15) == 0) rt_new(r, 0);
        ASSERT_TRUE(iters < 100000);
    }
    ASSERT_TRUE(saw_mark);
    ASSERT_TRUE(saw_sweep);
    ASSERT_EQ(reaper_phase(r), REAPER_IDLE);
    ASSERT_EQ(g_h.finalized, 2500);

    size_t n = 1;
    for (rt_node *p = head; p->left; p = p->left) {
        ASSERT_TRUE(rt_ok(p));
        n++;
    }
    ASSERT_EQ(n, (size_t)1000);
    reaper_destroy(r);
}

TEST(reaper_minor_cycles_remembered_set) {
    reaper_t *r = rt_create(0, 0);

    rt_node *root = rt_new(r, REAPER_FINALIZE);
    root->left = rt_new(r, REAPER_FINALIZE);
    g_h.roots[0] = root;

    /* minor sweep keeps survivors black — that IS generational mode */
    reaper_collect_minor(r);
    ASSERT_EQ(g_h.finalized, 0);
    ASSERT_EQ(reaper_color(r, root), REAPER_BLACK);
    ASSERT_EQ(reaper_color(r, root->left), REAPER_BLACK);

    /* young garbage dies in a minor while old survivors are not re-traced */
    rt_new(r, REAPER_FINALIZE);
    reaper_collect_minor(r);
    ASSERT_EQ(g_h.finalized, 1);
    ASSERT_EQ(reaper_color(r, root), REAPER_BLACK);

    /* store a newborn into an old black object: the barrier queues root
     * as a remembered-set entry, or the minor would sweep the newborn */
    rt_node *c = rt_new(r, REAPER_FINALIZE);
    root->right = c;
    reaper_write_barrier(r, root);
    ASSERT_EQ(reaper_color(r, root), REAPER_DARK_GRAY);
    reaper_collect_minor(r);
    ASSERT_EQ(g_h.finalized, 1);
    ASSERT_TRUE(rt_ok(c));
    ASSERT_EQ(reaper_color(r, c), REAPER_BLACK);

    /* a major clears the generational marks back to white */
    reaper_collect(r);
    ASSERT_EQ(g_h.finalized, 1);
    ASSERT_EQ(reaper_color(r, root), REAPER_WHITE);
    ASSERT_EQ(reaper_color(r, c), REAPER_WHITE);

    g_h.roots[0] = NULL;
    reaper_collect(r);
    ASSERT_EQ(g_h.finalized, 4);   /* root, left, c */
    reaper_destroy(r);
}

TEST(reaper_huge_objects) {
    reaper_t *r = rt_create(0, 0);

    /* > 1/4 of a 64 KiB arena's data area — takes the huge path */
    size_t huge_sz = 20000;
    rt_node *root = rt_new(r, 0);
    g_h.roots[0] = root;
    uint8_t *blob = (uint8_t *)reaper_alloc(r, huge_sz, RT_TAG_BLOB,
                                            REAPER_FINALIZE);
    ASSERT_NOT_NULL(blob);
    ASSERT_EQ(reaper_obj_size(blob), huge_sz);
    memset(blob, 0x5A, huge_sz);
    root->blob = blob;
    reaper_write_barrier(r, root);

    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_EQ(st.huge_objects, (size_t)1);

    reaper_collect(r);
    ASSERT_EQ(g_h.finalized, 0);
    ASSERT_EQ(blob[0], 0x5A);
    ASSERT_EQ(blob[huge_sz - 1], 0x5A);

    root->blob = NULL;
    reaper_collect(r);
    ASSERT_EQ(g_h.finalized, 1);
    reaper_get_stats(r, &st);
    ASSERT_EQ(st.huge_objects, (size_t)0);
    reaper_destroy(r);
}

TEST(reaper_explicit_free) {
    reaper_t *r = rt_create(0, 0);

    rt_node *n = rt_new(r, REAPER_FINALIZE);
    reaper_free(r, n);
    ASSERT_EQ(g_h.finalized, 1);

    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_EQ(st.objects_freed, (size_t)1);

    /* freeing an object that sits on the gray stack as a remembered-set
     * entry must remove it (or the next mark would trace freed memory) */
    rt_node *m = rt_new(r, REAPER_FINALIZE);
    g_h.roots[0] = m;
    reaper_collect_minor(r);
    ASSERT_EQ(reaper_color(r, m), REAPER_BLACK);
    reaper_write_barrier(r, m);   /* black -> dark gray, queued */
    ASSERT_EQ(reaper_color(r, m), REAPER_DARK_GRAY);
    g_h.roots[0] = NULL;
    reaper_free(r, m);
    ASSERT_EQ(g_h.finalized, 2);
    reaper_collect(r);            /* must not touch the freed entry */
    ASSERT_EQ(g_h.finalized, 2);  /* and must not double-finalize */
    reaper_destroy(r);
}

TEST(reaper_multi_arena_chain) {
    reaper_t *r = rt_create(0, 0);

    /* ~3000 rooted nodes at 3 cells apiece spill over several 64 KiB
     * arenas, interleaved with equal parts garbage */
    rt_node *head = rt_new(r, 0);
    g_h.roots[0] = head;
    rt_node *tail = head;
    for (int i = 0; i < 2999; i++) {
        tail->left = rt_new(r, 0);
        reaper_write_barrier(r, tail);
        tail = tail->left;
        rt_new(r, REAPER_FINALIZE);
    }
    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_TRUE(st.arenas >= 3);

    reaper_collect(r);
    ASSERT_EQ(g_h.finalized, 2999);
    size_t n = 1;
    for (rt_node *p = head; p->left; p = p->left) {
        ASSERT_TRUE(rt_ok(p));
        n++;
    }
    ASSERT_EQ(n, (size_t)3000);
    reaper_destroy(r);
}

TEST(reaper_empty_arena_release) {
    reaper_t *r = rt_create(0, 0);

    for (int i = 0; i < 5000; i++) rt_new(r, 0);   /* pure garbage */
    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_TRUE(st.arenas >= 4);

    reaper_collect(r);
    reaper_get_stats(r, &st);
    ASSERT_TRUE(st.arenas <= 2);   /* alloc target + one cached spare */
    ASSERT_EQ(st.live_bytes, (size_t)0);
    reaper_destroy(r);
}

TEST(reaper_pacer) {
    reaper_t *r = rt_create(0, 0);
    ASSERT_FALSE(reaper_needs_step(r));
    ASSERT_EQ(reaper_step(r, 1024), (size_t)0);   /* nothing to do */

    for (int i = 0; i < 2500; i++) rt_new(r, 0);  /* ~80 KiB > 64 KiB floor */
    ASSERT_TRUE(reaper_needs_step(r));

    reaper_collect(r);
    ASSERT_FALSE(reaper_needs_step(r));
    reaper_destroy(r);
}

TEST(reaper_allocation_assist) {
    reaper_t *r = rt_create(0, 512);   /* allocations self-pace the GC */

    rt_node *root = rt_new(r, 0);
    g_h.roots[0] = root;
    for (int i = 0; i < 10000; i++) rt_new(r, 0);   /* garbage storm */

    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_TRUE(st.cycles_major >= 1);   /* cycles ran with no manual step */
    ASSERT_TRUE(st.arenas <= 5);         /* heap stayed bounded */
    ASSERT_TRUE(rt_ok(root));
    reaper_destroy(r);
}

TEST(reaper_stress_random_graph) {
    reaper_t *r = rt_create(2, 0);   /* 2 minors per major when paced */
    rt_rng = 0x9E3779B9u;
    uint16_t stamp = 0;

    for (int iter = 0; iter < 20000; iter++) {
        uint32_t op = rt_rand() % 100;
        uint32_t slot = rt_rand() % RT_MAX_ROOTS;

        if (op < 35) {
            /* fresh node into a root slot */
            g_h.roots[slot] = rt_new(r, REAPER_FINALIZE);
        } else if (op < 65) {
            /* fresh node attached somewhere down a random path */
            rt_node *p = g_h.roots[slot];
            for (int d = 0; p && d < 8; d++) {
                rt_node *nx = (rt_rand() & 1) ? p->left : p->right;
                if (!nx) break;
                p = nx;
            }
            rt_node *n = rt_new(r, REAPER_FINALIZE);
            if (p) {
                if (rt_rand() & 1) p->left = n; else p->right = n;
                reaper_write_barrier(r, p);
            } else {
                g_h.roots[slot] = n;
            }
        } else if (op < 72) {
            /* cross-link two reachable nodes: sharing and cycles */
            rt_node *a = g_h.roots[slot];
            rt_node *b = g_h.roots[rt_rand() % RT_MAX_ROOTS];
            for (int d = 0; a && d < 4; d++) {
                rt_node *nx = (rt_rand() & 1) ? a->left : a->right;
                if (!nx) break;
                a = nx;
            }
            if (a && b) {
                if (rt_rand() & 1) a->left = b; else a->right = b;
                reaper_write_barrier(r, a);
            }
        } else if (op < 78) {
            /* create-then-discard; explicit free when legal */
            rt_node *n = rt_new(r, REAPER_FINALIZE);
            if (reaper_phase(r) == REAPER_IDLE) reaper_free(r, n);
        } else if (op < 88) {
            g_h.roots[slot] = NULL;   /* orphan a subtree */
        } else {
            (void)reaper_step(r, 64);
        }

        if ((iter & 127) == 0) (void)reaper_step(r, 256);
        if ((iter & 2047) == 2047) reaper_collect_minor(r);
        if ((iter & 8191) == 8191) reaper_collect(r);
    }

    /* two majors, then verify: stale remembered-set entries buy exactly
     * one cycle of floating garbage (by design), so the second major
     * makes the census exact. Every reachable node must be intact and
     * absent from the shadow death table; everything else finalized
     * exactly once. */
    reaper_collect(r);
    reaper_collect(r);
    int bad = 0;
    size_t reachable = rt_walk_verify(++stamp, &bad);
    ASSERT_EQ(bad, 0);
    ASSERT_EQ((size_t)g_h.finalized + reachable, (size_t)g_h.next_serial);

    /* drop everything: the whole graph (cycles included) must die */
    for (int i = 0; i < RT_MAX_ROOTS; i++) g_h.roots[i] = NULL;
    reaper_collect(r);
    ASSERT_EQ((size_t)g_h.finalized, (size_t)g_h.next_serial);

    reaper_stats st;
    reaper_get_stats(r, &st);
    ASSERT_EQ(st.live_bytes, (size_t)0);
    ASSERT_EQ(st.huge_objects, (size_t)0);
    reaper_destroy(r);
}

/* ------------------------------------------------------------------ */

void run_reaper_tests(void) {
    TEST_SUITE("Reaper GC");
    RUN_TEST(reaper_create_destroy_finalizes);
    RUN_TEST(reaper_alloc_basics);
    RUN_TEST(reaper_collect_keeps_rooted_frees_garbage);
    RUN_TEST(reaper_write_barrier_idle_white);
    RUN_TEST(reaper_write_barrier_mid_mark);
    RUN_TEST(reaper_incremental_steps);
    RUN_TEST(reaper_minor_cycles_remembered_set);
    RUN_TEST(reaper_huge_objects);
    RUN_TEST(reaper_explicit_free);
    RUN_TEST(reaper_multi_arena_chain);
    RUN_TEST(reaper_empty_arena_release);
    RUN_TEST(reaper_pacer);
    RUN_TEST(reaper_allocation_assist);
    RUN_TEST(reaper_stress_random_graph);
}
