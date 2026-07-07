/* =====================================================================
 * examples/basic.cpp — minimal C++ reaper.hpp example
 *
 * Shows the core lifecycle with RAII: create a Gc, allocate a small graph,
 * run a major collection, drop the root, collect again. All reachable
 * objects survive; unreachable objects are freed and their destructors run.
 * ===================================================================== */

#define REAPER_IMPLEMENTATION

#include <cstdio>
#include <cstring>

#include "reaper.hpp"

struct node {
    node *left = nullptr;
    node *right = nullptr;
    const char *name = nullptr;
    int value = 0;

    ~node() {
        printf("  ~node(%s)\n", name ? name : "?");
    }
};

namespace reaper_cpp {
template<>
struct traits<node> {
    static constexpr uint8_t tag = 1;
    static constexpr bool traversable = true;
    static constexpr bool needs_finalize = true;
};
} // namespace reaper_cpp

static node *g_root = nullptr;

int main(void) {
    reaper_cpp::Gc::Config cfg;
    cfg.trace = [](reaper_cpp::Gc& gc, void *obj, uint8_t /*tag*/) {
        node *n = static_cast<node*>(obj);
        gc.mark(n->left);
        gc.mark(n->right);
    };
    cfg.roots = [](reaper_cpp::Gc& gc) {
        gc.mark(g_root);
    };

    reaper_cpp::Gc gc(cfg);
    printf("created reaper_cpp::Gc\n");

    node *root = gc.make<node>();
    root->name = "root";
    root->value = 42;
    root->left = gc.make<node>();
    root->left->name = "left";
    root->right = gc.make<node>();
    root->right->name = "right";

    gc.write_barrier(root);
    g_root = root;

    for (int i = 0; i < 1000; i++)
        gc.make<node>();

    printf("running major collection...\n");
    gc.collect();
    printf("after major collection: root still reachable\n");

    g_root = nullptr;
    printf("running major collection after dropping root...\n");
    gc.collect();
    printf("after dropping root: graph collected\n");

    return 0;
}
