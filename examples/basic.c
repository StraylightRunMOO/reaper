/* =====================================================================
 * examples/basic.c — minimal reaper.h example
 *
 * Shows the core lifecycle: create a GC, allocate a small object graph,
 * register a root, run a major collection, drop the root, and collect
 * again. All reachable objects survive; unreachable objects are freed.
 * ===================================================================== */

#include <stdio.h>
#include <string.h>

#define REAPER_IMPLEMENTATION
#include "reaper.h"

typedef struct node {
    struct node *left;
    struct node *right;
    const char *name;
} node;

#define MAX_ROOTS 8

typedef struct host {
    reaper_t *r;
    node *roots[MAX_ROOTS];
} host;

static host g;

static void trace_node(reaper_t *r, void *obj, uint8_t tag, void *ud) {
    (void)tag; (void)ud;
    node *n = (node *)obj;
    reaper_mark(r, n->left);
    reaper_mark(r, n->right);
}

static void mark_roots(reaper_t *r, void *ud) {
    host *h = (host *)ud;
    for (int i = 0; i < MAX_ROOTS; i++)
        reaper_mark(r, h->roots[i]);
}

int main(void) {
    reaper_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.trace = trace_node;
    cfg.roots = mark_roots;
    cfg.ud = &g;

    g.r = reaper_create(&cfg);
    printf("created reaper\n");

    /* Build a tiny graph. */
    node *root = reaper_alloc(g.r, sizeof(node), 1, REAPER_TRAVERSABLE);
    root->left = reaper_alloc(g.r, sizeof(node), 1, REAPER_TRAVERSABLE);
    root->right = reaper_alloc(g.r, sizeof(node), 1, REAPER_TRAVERSABLE);
    root->name = "root";

    reaper_write_barrier(g.r, root);
    g.roots[0] = root;

    /* Allocate some garbage that will be collected. */
    for (int i = 0; i < 1000; i++)
        (void)reaper_alloc(g.r, sizeof(node), 1, REAPER_TRAVERSABLE);

    reaper_collect(g.r);
    printf("after major collection: root still reachable\n");

    /* Drop the root and collect again. */
    g.roots[0] = NULL;
    reaper_collect(g.r);
    printf("after dropping root: graph collected\n");

    reaper_destroy(g.r);
    return 0;
}
