/* =====================================================================
 * tests/test_reaper_cpp.cpp — C++17 reaper.hpp test suite
 *
 * Mirrors many of the C tests but exercises the RAII wrapper, make<T>(),
 * automatic destructors, and move semantics.
 * ===================================================================== */

#include "reaper.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#define RT_TAG_NODE  1
#define RT_TAG_BLOB  2
#define RT_MAX_ROOTS 64
#define RT_MAX_SERIAL 65536
#define RT_MAGIC 0xC0FFEE00u

struct rt_blob;

struct rt_node {
    rt_node *left = nullptr;
    rt_node *right = nullptr;
    rt_blob *blob = nullptr;
    uint32_t magic = 0;
    uint32_t serial = 0;
    std::string name;
    ~rt_node();
};

struct rt_blob {
    uint8_t data[16] = {0};
    ~rt_blob();
};

namespace reaper_cpp {
template<>
struct traits<rt_node> {
    static constexpr uint8_t tag = RT_TAG_NODE;
    static constexpr bool traversable = true;
    static constexpr bool needs_finalize = true;
};
template<>
struct traits<rt_blob> {
    static constexpr uint8_t tag = RT_TAG_BLOB;
    static constexpr bool traversable = false;
    static constexpr bool needs_finalize = true;
};
} // namespace reaper_cpp

struct rt_host {
    rt_node *roots[RT_MAX_ROOTS] = {};
    std::atomic<int> finalized{0};
    std::atomic<int> node_destructors{0};
    std::atomic<int> blob_destructors{0};
    uint32_t next_serial = 0;
};

static rt_host g_h;
static uint8_t rt_dead[RT_MAX_SERIAL];

rt_node::~rt_node() { g_h.node_destructors++; }
rt_blob::~rt_blob() { g_h.blob_destructors++; }

static void rt_trace(reaper_cpp::Gc& gc, void *obj, uint8_t tag) {
    if (tag == RT_TAG_NODE) {
        rt_node *n = static_cast<rt_node*>(obj);
        gc.mark(n->left);
        gc.mark(n->right);
        gc.mark(n->blob);
    }
}

static void rt_roots(reaper_cpp::Gc& gc) {
    for (int i = 0; i < RT_MAX_ROOTS; i++)
        gc.mark(g_h.roots[i]);
}

static void rt_finalize(void *obj, uint8_t tag) {
    (void)obj;
    g_h.finalized++;
    if (tag == RT_TAG_NODE) {
        rt_node *n = static_cast<rt_node*>(obj);
        if (n->serial < RT_MAX_SERIAL) rt_dead[n->serial] = 1;
    }
}

static reaper_cpp::Gc rt_create(unsigned minor_per_major, size_t assist) {
    memset(g_h.roots, 0, sizeof(g_h.roots));
    g_h.finalized = 0;
    g_h.node_destructors = 0;
    g_h.blob_destructors = 0;
    g_h.next_serial = 0;
    memset(rt_dead, 0, sizeof(rt_dead));

    reaper_cpp::Gc::Config cfg;
    cfg.trace = rt_trace;
    cfg.roots = rt_roots;
    cfg.finalize = rt_finalize;
    cfg.arena_size = 64 * 1024;
    cfg.minor_per_major = minor_per_major;
    cfg.assist_budget = assist;
    return reaper_cpp::Gc(cfg);
}

static rt_node *rt_new(reaper_cpp::Gc& gc) {
    rt_node *n = gc.make<rt_node>();
    n->left = n->right = nullptr;
    n->blob = nullptr;
    n->serial = g_h.next_serial++;
    n->magic = RT_MAGIC ^ n->serial;
    n->name = "node";
    return n;
}

/* Allocate a traversable node without registering a finalizer. Used when the
 * test wants to create garbage that does not increment g_h.finalized. */
static rt_node *rt_new_unfinalized(reaper_cpp::Gc& gc) {
    void *mem = gc.allocate(sizeof(rt_node), RT_TAG_NODE, REAPER_TRAVERSABLE);
    rt_node *n = new (mem) rt_node();
    n->left = n->right = nullptr;
    n->blob = nullptr;
    n->serial = g_h.next_serial++;
    n->magic = RT_MAGIC ^ n->serial;
    n->name = "node";
    return n;
}

static bool rt_ok(const rt_node *n) {
    return n->magic == (RT_MAGIC ^ n->serial) &&
           (n->serial >= RT_MAX_SERIAL || !rt_dead[n->serial]);
}

/* ------------------------------------------------------------------ */

TEST(cpp_create_destroy_finalizes) {
    reaper_cpp::Gc gc = rt_create(0, 0);
    ASSERT_EQ(gc.phase(), REAPER_IDLE);

    rt_new(gc);
    rt_new(gc);
    rt_new(gc);
    gc.collect();
    ASSERT_EQ(g_h.finalized.load(), 3);
}

TEST(cpp_make_basics) {
    reaper_cpp::Gc gc = rt_create(0, 0);
    rt_node *n = rt_new(gc);
    ASSERT_NOT_NULL(n);

    ASSERT_EQ(reaper_cpp::Gc::object_size(n), sizeof(rt_node));
    ASSERT_EQ(reaper_cpp::Gc::object_tag(n), RT_TAG_NODE);
    ASSERT_EQ(reaper_cpp::Gc::object_userdata(n), 0);
    reaper_cpp::Gc::object_set_userdata(n, 0xBEEF);
    ASSERT_EQ(reaper_cpp::Gc::object_userdata(n), 0xBEEF);

    ASSERT_EQ(gc.phase(), REAPER_IDLE);
    gc.collect();
}

TEST(cpp_destructor_called_on_collect) {
    reaper_cpp::Gc gc = rt_create(0, 0);
    rt_node *n = rt_new(gc);
    g_h.roots[0] = n;
    gc.collect();
    g_h.roots[0] = nullptr;
    gc.collect();
    ASSERT_EQ(g_h.node_destructors.load(), 1);
}

TEST(cpp_collect_keeps_rooted_frees_garbage) {
    reaper_cpp::Gc gc = rt_create(0, 0);

    rt_node *root = rt_new(gc);
    root->left = rt_new(gc);
    root->right = rt_new(gc);
    g_h.roots[0] = root;

    for (int i = 0; i < 100; i++)
        rt_new(gc);

    gc.collect();
    ASSERT_EQ(g_h.finalized.load(), 100);
    ASSERT_TRUE(rt_ok(root));
    ASSERT_TRUE(rt_ok(root->left));
    ASSERT_TRUE(rt_ok(root->right));

    root->left = nullptr;
    gc.collect();
    ASSERT_TRUE(rt_ok(root));
}

TEST(cpp_write_barrier_mid_mark) {
    reaper_cpp::Gc gc = rt_create(0, 0);

    rt_node *root = rt_new(gc);
    root->left = rt_new(gc);
    g_h.roots[0] = root;

    for (int i = 0; i < 2500; i++) rt_new(gc);

    ASSERT_TRUE(gc.needs_step());
    (void)gc.step(1);
    ASSERT_EQ(gc.phase(), REAPER_MARK);

    rt_node *b = rt_new(gc);
    root->right = b;
    gc.write_barrier(root);

    while (gc.step(1024)) {}
    ASSERT_EQ(gc.phase(), REAPER_IDLE);
    ASSERT_TRUE(rt_ok(b));
    ASSERT_TRUE(rt_ok(root->left));
}

TEST(cpp_incremental_steps) {
    reaper_cpp::Gc gc = rt_create(0, 0);

    rt_node *head = rt_new(gc);
    g_h.roots[0] = head;
    rt_node *tail = head;
    for (int i = 0; i < 999; i++) {
        tail->left = rt_new(gc);
        gc.write_barrier(tail);
        tail = tail->left;
    }
    for (int i = 0; i < 2500; i++) rt_new(gc);

    ASSERT_TRUE(gc.needs_step());
    bool saw_mark = false, saw_sweep = false;
    int iters = 0;
    while (gc.step(32)) {
        if (gc.phase() == REAPER_MARK) saw_mark = true;
        if (gc.phase() == REAPER_SWEEP) saw_sweep = true;
        if ((iters++ & 15) == 0) rt_new_unfinalized(gc);
        ASSERT_TRUE(iters < 100000);
    }
    ASSERT_TRUE(saw_mark);
    ASSERT_TRUE(saw_sweep);
    ASSERT_EQ(gc.phase(), REAPER_IDLE);
    ASSERT_EQ(g_h.finalized.load(), 2500);

    size_t n = 1;
    for (rt_node *p = head; p->left; p = p->left) {
        ASSERT_TRUE(rt_ok(p));
        n++;
    }
    ASSERT_EQ(n, (size_t)1000);
}

TEST(cpp_minor_cycles_remembered_set) {
    reaper_cpp::Gc gc = rt_create(0, 0);

    rt_node *root = rt_new(gc);
    root->left = rt_new(gc);
    g_h.roots[0] = root;

    gc.collect_minor();
    ASSERT_EQ(g_h.finalized.load(), 0);
    ASSERT_EQ(gc.color(root), REAPER_BLACK);
    ASSERT_EQ(gc.color(root->left), REAPER_BLACK);

    rt_new(gc);
    gc.collect_minor();
    ASSERT_EQ(g_h.finalized.load(), 1);
    ASSERT_EQ(gc.color(root), REAPER_BLACK);

    rt_node *c = rt_new(gc);
    root->right = c;
    gc.write_barrier(root);
    gc.collect_minor();
    ASSERT_TRUE(rt_ok(c));
    ASSERT_EQ(gc.color(c), REAPER_BLACK);

    gc.collect();
    ASSERT_EQ(gc.color(root), REAPER_WHITE);

    g_h.roots[0] = nullptr;
    gc.collect();
    ASSERT_EQ(g_h.finalized.load(), 4);
}

TEST(cpp_stats_and_phase) {
    reaper_cpp::Gc gc = rt_create(0, 0);
    ASSERT_EQ(gc.phase(), REAPER_IDLE);

    reaper_stats st = gc.stats();
    ASSERT_EQ(st.arenas, (size_t)0);

    rt_new(gc);
    st = gc.stats();
    ASSERT_EQ(st.arenas, (size_t)1);

    gc.collect();
    st = gc.stats();
    ASSERT_EQ(st.cycles_major, (size_t)1);
}

TEST(cpp_move_semantics) {
    reaper_cpp::Gc gc = rt_create(0, 0);
    rt_node *n = rt_new(gc);
    g_h.roots[0] = n;

    reaper_cpp::Gc moved = std::move(gc);
    moved.collect();
    ASSERT_EQ(gc.phase(), REAPER_IDLE); /* moved-from object is valid but empty */
    ASSERT_EQ(moved.phase(), REAPER_IDLE);
    ASSERT_TRUE(rt_ok(n));
}

TEST(cpp_blob_with_destructor) {
    reaper_cpp::Gc gc = rt_create(0, 0);
    rt_node *root = rt_new(gc);
    g_h.roots[0] = root;

    rt_blob *blob = gc.make<rt_blob>();
    memset(blob->data, 0x5A, sizeof(blob->data));
    root->blob = blob;
    gc.write_barrier(root);

    gc.collect();
    ASSERT_EQ(g_h.blob_destructors.load(), 0);
    ASSERT_EQ(blob->data[0], 0x5A);

    root->blob = nullptr;
    gc.collect();
    ASSERT_EQ(g_h.blob_destructors.load(), 1);
}

/* ------------------------------------------------------------------ */

void run_reaper_cpp_tests(void) {
    TEST_SUITE("Reaper C++");
    RUN_TEST(cpp_create_destroy_finalizes);
    RUN_TEST(cpp_make_basics);
    RUN_TEST(cpp_destructor_called_on_collect);
    RUN_TEST(cpp_collect_keeps_rooted_frees_garbage);
    RUN_TEST(cpp_write_barrier_mid_mark);
    RUN_TEST(cpp_incremental_steps);
    RUN_TEST(cpp_minor_cycles_remembered_set);
    RUN_TEST(cpp_stats_and_phase);
    RUN_TEST(cpp_move_semantics);
    RUN_TEST(cpp_blob_with_destructor);
}
