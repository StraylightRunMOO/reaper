/* =========================================================================
 * reaper.h — quad-color incremental, generational, non-moving GC
 * v1.0.0 — single-header C11 library, no dependencies beyond libc.
 *
 * A generic mark-sweep collector after Mike Pall's LuaJIT 3.0 GC design
 * (http://wiki.luajit.org/New-Garbage-Collector), borrowing Go's
 * incremental-mark pacing model. Designed to pair with arena allocators
 * (memento) and cooperative runtimes (suspenders.h): short-lived data
 * lives in bump arenas reclaimed wholesale; whatever escapes is handed
 * to reaper, and a host coroutine drives marking in bounded slices that
 * interleave with the mutator.
 *
 * DESIGN
 *   Memory   Power-of-two arenas (default 256 KiB) aligned to their own
 *            size, carved into 16-byte cells. Two bitmaps per arena
 *            (block + mark) encode each cell in 2 bits:
 *                block mark
 *                  0    0   block extent (interior cell)
 *                  0    1   free cell (head of / inside a free run)
 *                  1    0   white object head
 *                  1    1   black object head
 *            Sweeping is a word-parallel bitmap transform that never
 *            touches object data:
 *                major:  block' = block & mark;  mark' = block ^ mark
 *                minor:  block' = block & mark;  mark' = block | mark
 *            (major frees white and flips black to white; minor frees
 *            white and keeps survivors black — that IS the generational
 *            mode). Freed blocks coalesce with neighbors for free, since
 *            their heads become free cells and extents stay extents.
 *            Objects too large for an arena go on a separate huge list.
 *
 *   Colors   The mark bit lives in the bitmap; the GRAY bit lives in the
 *            16-byte object header. Together they form four colors:
 *                white       mark=0 gray=0   condemned candidate
 *                light gray  mark=0 gray=1   newborn; barrier-exempt
 *                dark gray   mark=1 gray=1   queued for traversal
 *                black       mark=1 gray=0   traversed
 *            New traversable objects are born LIGHT GRAY: the gray bit
 *            suppresses the write barrier, so the common pattern of
 *            initializing an object right after allocation never pays
 *            for barriers. They are still white to the sweep, so objects
 *            that die young are collected in the very cycle of their
 *            birth unless something reachable picks them up.
 *
 *   Barrier  reaper_write_barrier(r, obj) after storing a GC reference
 *            INTO obj. Fast path is one load + test of the gray bit
 *            (2-3 instructions). On trigger: white objects turn light
 *            gray (flag only), black objects turn dark gray and are
 *            queued for re-traversal (a backward barrier). Queued black
 *            objects double as the remembered set for minor cycles.
 *
 *   Phases   IDLE -> MARK -> (atomic) -> SWEEP -> IDLE, advanced by
 *            reaper_step(r, budget). The atomic step re-scans roots and
 *            drains the gray stack to a fixpoint; in a cooperative
 *            (single-threaded) host that is naturally atomic. Allocation
 *            color adapts to the phase so objects born during a sweep
 *            in a not-yet-swept arena are born black and survive it.
 *
 *   Pacing   Go-style: a cycle is triggered when the heap grows past
 *            live * (1 + pace_percent/100). Hosts either poll
 *            reaper_needs_step() from a scheduler loop / coroutine, or
 *            set assist_budget so allocations self-pace by doing a
 *            slice of work once over the trigger.
 *
 * CONTRACTS
 *   - Single-threaded: all calls, including callbacks, from one thread
 *     (or one cooperative scheduler). Drive concurrency by calling
 *     reaper_step from a coroutine, not from another thread.
 *   - trace(r, obj, tag, ud) must call reaper_mark(r, child) for every
 *     GC-managed reference held by obj. It must not allocate or free.
 *   - roots(r, ud) must call reaper_mark(r, root) for every root. It
 *     may be invoked several times per cycle and must be idempotent.
 *   - finalize(obj, tag, ud) runs exactly once, when the object is
 *     found dead (or at reaper_destroy / reaper_free). No resurrection:
 *     the memory is reclaimed as soon as it returns.
 *   - reaper_free is legal only while reaper_phase() == REAPER_IDLE.
 *
 * USAGE
 *   #define REAPER_IMPLEMENTATION in exactly one translation unit
 *   before including this header.
 * ========================================================================= */

#ifndef REAPER_H
#define REAPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef REAPER_API
#define REAPER_API extern
#endif

#define REAPER_VERSION_MAJOR 1
#define REAPER_VERSION_MINOR 0
#define REAPER_VERSION_PATCH 0

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct reaper reaper_t;

/* Allocation flags */
#define REAPER_TRAVERSABLE 0x01u  /* holds GC references; will be traced */
#define REAPER_FINALIZE    0x02u  /* call cfg.finalize when collected    */

/* Colors (reaper_color) */
enum {
    REAPER_WHITE      = 0,
    REAPER_LIGHT_GRAY = 1,
    REAPER_DARK_GRAY  = 2,
    REAPER_BLACK      = 3
};

/* Phases (reaper_phase) */
enum {
    REAPER_IDLE  = 0,
    REAPER_MARK  = 1,
    REAPER_SWEEP = 2
};

/* Backing (page-level) allocator. `align` is always a power of two;
 * the returned pointer must honor it. Defaults to aligned_alloc/free.
 * Adapters for arena allocators like memento fit this shape directly. */
typedef struct reaper_allocator {
    void *(*alloc)(size_t size, size_t align, void *ud);
    void  (*free_)(void *ptr, size_t size, void *ud);
    void  *ud;
} reaper_allocator;

typedef void (*reaper_trace_fn)(reaper_t *r, void *obj, uint8_t tag, void *ud);
typedef void (*reaper_roots_fn)(reaper_t *r, void *ud);
typedef void (*reaper_final_fn)(void *obj, uint8_t tag, void *ud);

typedef struct reaper_config {
    reaper_allocator backing;   /* zeroed = aligned_alloc/free           */
    reaper_trace_fn  trace;     /* required if traversable objects used  */
    reaper_roots_fn  roots;     /* required                              */
    reaper_final_fn  finalize;  /* required if REAPER_FINALIZE used      */
    void    *ud;                /* passed to all callbacks               */
    size_t   arena_size;        /* pow2 >= 64 KiB; 0 = 256 KiB           */
    unsigned pace_percent;      /* 0 = 100: cycle at live*(1+pct/100)    */
    unsigned minor_per_major;   /* minor cycles between majors; 0 = off  */
    size_t   assist_budget;     /* per-alloc step budget once over the
                                 * trigger; 0 = host drives all stepping */
} reaper_config;

typedef struct reaper_stats {
    size_t live_bytes;       /* payload estimate at last cycle end      */
    size_t heap_bytes;       /* payload bytes currently accounted       */
    size_t footprint_bytes;  /* arenas + huge, incl. metadata           */
    size_t arenas;
    size_t huge_objects;
    size_t cycles_major;
    size_t cycles_minor;
    size_t objects_freed;
    size_t barrier_hits;     /* barrier slow-path executions            */
} reaper_stats;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

REAPER_API reaper_t *reaper_create(const reaper_config *cfg);
REAPER_API void      reaper_destroy(reaper_t *r);   /* finalizes everything */

REAPER_API void *reaper_alloc(reaper_t *r, size_t size, uint8_t tag,
                              unsigned flags);
REAPER_API void  reaper_free(reaper_t *r, void *obj);  /* IDLE phase only */

/* Call after storing a GC-managed reference into `obj`. */
REAPER_API void reaper_write_barrier(reaper_t *r, void *obj);

/* Only meaningful inside trace/roots callbacks (and harmless outside). */
REAPER_API void reaper_mark(reaper_t *r, void *obj);

/* Perform up to `budget` work units (~cells touched). Starts a cycle if
 * the pacer wants one. Returns nonzero while a cycle is in flight. */
REAPER_API size_t reaper_step(reaper_t *r, size_t budget);
REAPER_API bool   reaper_needs_step(const reaper_t *r);

/* Run a full stop-the-world cycle right now. */
REAPER_API void reaper_collect(reaper_t *r);        /* major */
REAPER_API void reaper_collect_minor(reaper_t *r);  /* minor */

/* Introspection */
REAPER_API int      reaper_phase(const reaper_t *r);
REAPER_API unsigned reaper_color(const reaper_t *r, const void *obj);
REAPER_API void     reaper_get_stats(const reaper_t *r, reaper_stats *out);
REAPER_API size_t   reaper_obj_size(const void *obj);
REAPER_API uint8_t  reaper_obj_tag(const void *obj);
REAPER_API uint16_t reaper_obj_userdata(const void *obj);
REAPER_API void     reaper_obj_set_userdata(void *obj, uint16_t v);

#ifdef __cplusplus
}
#endif

/* ===================================================================== */
/* IMPLEMENTATION                                                        */
/* ===================================================================== */
#ifdef REAPER_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

#ifndef REAPER_ASSERT
#include <assert.h>
#define REAPER_ASSERT(x) assert(x)
#endif

/* Fatal path for unrecoverable internal allocation failure (gray stack
 * growth inside a write barrier has no way to fail gracefully). */
#ifndef REAPER_FATAL
#include <stdio.h>
#define REAPER_FATAL(msg) do { \
    fprintf(stderr, "reaper: fatal: %s\n", (msg)); abort(); } while (0)
#endif

#define RPR_CELL          16u
#define RPR_CELL_SHIFT    4u
#define RPR_DEF_ARENA     (256u * 1024u)
#define RPR_MIN_ARENA     (64u * 1024u)

/* Object header — occupies the first cell of every block. The returned
 * object pointer is header + 16; payload cells follow. */
typedef struct rpr_head {
    uint32_t size;      /* payload bytes requested                */
    uint8_t  flags;     /* REAPER_* alloc flags + internal bits   */
    uint8_t  tag;       /* opaque host type tag                   */
    uint16_t userdata;  /* free for the host                      */
    uint32_t _rsvd0;
    uint32_t _rsvd1;
} rpr_head;

/* Internal flag bits (share the byte with the two public alloc flags) */
#define RPR_F_GRAY 0x40u
#define RPR_F_HUGE 0x80u

/* Huge object prefix: [rpr_huge][rpr_head][payload], payload 16-aligned. */
typedef struct rpr_huge {
    struct rpr_huge *next;
    struct rpr_huge *prev;
    size_t map_size;     /* total bytes handed to the backing allocator */
    uint8_t marked;
    uint8_t _pad[7];
} rpr_huge;

typedef struct rpr_arena {
    struct rpr_arena *next;
    uint32_t free_cells;
    uint32_t cursor;        /* cell index of a block boundary            */
    uint64_t swept_epoch;   /* == r->sweep_epoch once swept this cycle   */
} rpr_arena;

struct reaper {
    reaper_config cfg;
    size_t   arena_size;
    uint32_t ncells;        /* cells per arena (whole mapping)     */
    uint32_t data_start;    /* first usable cell                   */
    uint32_t nwords;        /* bitmap words per map                */

    rpr_arena *arenas;      /* all arenas, singly linked           */
    rpr_arena *alloc_arena; /* current allocation target           */
    size_t     n_arenas;

    rpr_huge  *huge;
    size_t     n_huge;
    size_t     huge_bytes;  /* payload bytes in huge objects       */

    void **gray;            /* global gray stack of object ptrs    */
    size_t gray_len, gray_cap;

    void **finreg;          /* registry of finalizable objects     */
    size_t finreg_len, finreg_cap;

    int      phase;
    bool     cycle_minor;
    bool     huge_swept;    /* huge list swept this cycle          */
    bool     in_step;       /* re-entrancy guard (assist/callback) */
    uint64_t sweep_epoch;
    rpr_arena *sweep_cursor;
    unsigned minors_done;

    size_t live_bytes;      /* estimate at last cycle end          */
    size_t heap_bytes;      /* live estimate + allocations since   */
    size_t trigger_bytes;

    reaper_stats stats;
};

/* ------------------------------------------------------------------ */
/* Backing allocator                                                   */
/* ------------------------------------------------------------------ */

static void *rpr_sys_alloc(size_t size, size_t align, void *ud) {
    (void)ud;
    /* aligned_alloc demands size be a multiple of align */
    size_t sz = (size + align - 1) & ~(align - 1);
    return aligned_alloc(align, sz);
}

static void rpr_sys_free(void *ptr, size_t size, void *ud) {
    (void)size; (void)ud;
    free(ptr);
}

static void *rpr_balloc(reaper_t *r, size_t size, size_t align) {
    return r->cfg.backing.alloc(size, align, r->cfg.backing.ud);
}

static void rpr_bfree(reaper_t *r, void *p, size_t size) {
    if (p) r->cfg.backing.free_(p, size, r->cfg.backing.ud);
}

/* ------------------------------------------------------------------ */
/* Bitmap primitives                                                   */
/* Arena layout: [rpr_arena][block bitmap][mark bitmap][... cells ...]  */
/* Cells are indexed over the whole mapping so that                     */
/*   cell(ptr) = (ptr - arena_base) >> 4                                */
/* ------------------------------------------------------------------ */

static inline uint64_t *rpr_bmap(const reaper_t *r, rpr_arena *a) {
    (void)r;
    return (uint64_t *)((char *)a + sizeof(rpr_arena));
}

static inline uint64_t *rpr_mmap_(const reaper_t *r, rpr_arena *a) {
    return (uint64_t *)((char *)a + sizeof(rpr_arena)) + r->nwords;
}

static inline bool rpr_bit(const uint64_t *bm, uint32_t i) {
    return (bm[i >> 6] >> (i & 63u)) & 1u;
}

static inline void rpr_bit_set(uint64_t *bm, uint32_t i) {
    bm[i >> 6] |= (uint64_t)1u << (i & 63u);
}

static inline void rpr_bit_clr(uint64_t *bm, uint32_t i) {
    bm[i >> 6] &= ~((uint64_t)1u << (i & 63u));
}

/* 2-bit cell state, matching the table at the top of the file */
enum { RPR_C_EXTENT = 0, RPR_C_FREE = 1, RPR_C_WHITE = 2, RPR_C_BLACK = 3 };

static inline unsigned rpr_cell_state(const reaper_t *r, rpr_arena *a,
                                      uint32_t i) {
    return ((unsigned)rpr_bit(rpr_bmap(r, a), i) << 1) |
            (unsigned)rpr_bit(rpr_mmap_((reaper_t *)r, a), i);
}

static inline void rpr_cell_set(reaper_t *r, rpr_arena *a, uint32_t i,
                                unsigned st) {
    uint64_t *b = rpr_bmap(r, a), *m = rpr_mmap_(r, a);
    if (st & 2u) rpr_bit_set(b, i); else rpr_bit_clr(b, i);
    if (st & 1u) rpr_bit_set(m, i); else rpr_bit_clr(m, i);
}

/* ------------------------------------------------------------------ */
/* Pointer geometry                                                    */
/* ------------------------------------------------------------------ */

static inline rpr_head *rpr_hdr(const void *obj) {
    return (rpr_head *)((char *)(uintptr_t)(const char *)obj - RPR_CELL);
}

static inline void *rpr_obj(rpr_head *h) {
    return (char *)h + RPR_CELL;
}

static inline bool rpr_is_huge(const rpr_head *h) {
    return (h->flags & RPR_F_HUGE) != 0;
}

static inline rpr_arena *rpr_arena_of(const reaper_t *r, const rpr_head *h) {
    return (rpr_arena *)((uintptr_t)h & ~((uintptr_t)r->arena_size - 1u));
}

static inline rpr_huge *rpr_huge_of(const rpr_head *h) {
    return (rpr_huge *)((char *)(uintptr_t)(const char *)h - sizeof(rpr_huge));
}

static inline uint32_t rpr_cell_of(const reaper_t *r, rpr_arena *a,
                                   const rpr_head *h) {
    (void)r;
    return (uint32_t)(((uintptr_t)h - (uintptr_t)a) >> RPR_CELL_SHIFT);
}

/* cells occupied by an object: 1 header cell + payload cells */
static inline uint32_t rpr_obj_cells(const rpr_head *h) {
    return 1u + (uint32_t)((h->size + RPR_CELL - 1u) >> RPR_CELL_SHIFT);
}

static inline bool rpr_is_marked(const reaper_t *r, const rpr_head *h) {
    if (rpr_is_huge(h)) return rpr_huge_of(h)->marked != 0;
    rpr_arena *a = rpr_arena_of(r, h);
    return rpr_bit(rpr_mmap_((reaper_t *)r, a), rpr_cell_of(r, a, h));
}

static void rpr_set_mark(reaper_t *r, rpr_head *h) {
    if (rpr_is_huge(h)) {
        rpr_huge_of(h)->marked = 1;
    } else {
        rpr_arena *a = rpr_arena_of(r, h);
        rpr_bit_set(rpr_mmap_(r, a), rpr_cell_of(r, a, h));
    }
}

/* ------------------------------------------------------------------ */
/* Gray stack / finalizer registry                                     */
/* ------------------------------------------------------------------ */

static void rpr_ptrvec_push(reaper_t *r, void ***vec, size_t *len,
                            size_t *cap, void *p) {
    if (*len == *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        void **nv = (void **)rpr_balloc(r, ncap * sizeof(void *), RPR_CELL);
        if (!nv) REAPER_FATAL("out of memory growing internal stack");
        if (*len) memcpy(nv, *vec, *len * sizeof(void *));
        rpr_bfree(r, *vec, *cap * sizeof(void *));
        *vec = nv;
        *cap = ncap;
    }
    (*vec)[(*len)++] = p;
}

static void rpr_ptrvec_remove(void **vec, size_t *len, const void *p) {
    for (size_t i = 0; i < *len; i++) {
        if (vec[i] == p) {
            vec[i] = vec[--(*len)];
            return;
        }
    }
}

static inline void rpr_gray_push(reaper_t *r, void *obj) {
    rpr_ptrvec_push(r, &r->gray, &r->gray_len, &r->gray_cap, obj);
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

static rpr_arena *rpr_arena_new(reaper_t *r) {
    rpr_arena *a = (rpr_arena *)rpr_balloc(r, r->arena_size, r->arena_size);
    if (!a) return NULL;
    a->next = r->arenas;
    a->cursor = r->data_start;
    a->free_cells = r->ncells - r->data_start;
    /* An arena born mid-sweep must not be visited by the pending sweep
     * with garbage bitmaps; stamp it as already swept. */
    a->swept_epoch = r->sweep_epoch;

    uint64_t *b = rpr_bmap(r, a), *m = rpr_mmap_(r, a);
    memset(b, 0, (size_t)r->nwords * 2u * sizeof(uint64_t));
    (void)m;
    /* reserved cells stay extent (00); one big free run after them */
    rpr_cell_set(r, a, r->data_start, RPR_C_FREE);

    r->arenas = a;
    r->n_arenas++;
    return a;
}

/* Walk block boundaries from `a->cursor`, looking for a free run of at
 * least `need` cells. Free runs start at a FREE cell and extend over
 * FREE and EXTENT cells until the next object head (adjacent freed
 * blocks coalesce implicitly). Returns cell index or UINT32_MAX. */
static uint32_t rpr_arena_fit(reaper_t *r, rpr_arena *a, uint32_t need) {
    if (a->free_cells < need) return UINT32_MAX;
    uint32_t start = a->cursor;
    uint32_t c = start;
    bool wrapped = false;

    for (;;) {
        if (c >= r->ncells) {
            if (wrapped) return UINT32_MAX;
            wrapped = true;
            c = r->data_start;
            if (c >= start) return UINT32_MAX;
        }
        unsigned st = rpr_cell_state(r, a, c);
        if (st == RPR_C_FREE) {
            uint32_t run = c + 1;
            while (run < r->ncells) {
                unsigned s2 = rpr_cell_state(r, a, run);
                if (s2 == RPR_C_WHITE || s2 == RPR_C_BLACK) break;
                run++;
            }
            if (run - c >= need) return c;
            c = run;
        } else {
            /* object head (or, right after data_start on a fresh
             * cursor, never EXTENT — walks start at boundaries) */
            REAPER_ASSERT(st == RPR_C_WHITE || st == RPR_C_BLACK);
            c++;
            while (c < r->ncells &&
                   rpr_cell_state(r, a, c) == RPR_C_EXTENT)
                c++;
        }
        if (wrapped && c >= start) return UINT32_MAX;
    }
}

/* Newborn head color per phase — see design notes at the top. */
static unsigned rpr_alloc_state(reaper_t *r, rpr_arena *a) {
    if (r->phase == REAPER_SWEEP && a->swept_epoch != r->sweep_epoch)
        return RPR_C_BLACK;   /* pending sweep must not eat the newborn */
    return RPR_C_WHITE;
}

/* Carve `need` cells at `c`; returns the header cell. */
static rpr_head *rpr_arena_carve(reaper_t *r, rpr_arena *a, uint32_t c,
                                 uint32_t need) {
    /* Bound the free run so we know whether a remainder head is needed */
    uint32_t run = c + 1;
    while (run < r->ncells) {
        unsigned s2 = rpr_cell_state(r, a, run);
        if (s2 == RPR_C_WHITE || s2 == RPR_C_BLACK) break;
        run++;
    }
    REAPER_ASSERT(run - c >= need);

    rpr_cell_set(r, a, c, rpr_alloc_state(r, a));
    for (uint32_t i = c + 1; i < c + need; i++)
        rpr_cell_set(r, a, i, RPR_C_EXTENT);
    if (c + need < run)
        rpr_cell_set(r, a, c + need, RPR_C_FREE);

    a->free_cells -= need;
    a->cursor = (c + need < r->ncells) ? c + need : r->data_start;
    return (rpr_head *)((char *)a + ((size_t)c << RPR_CELL_SHIFT));
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

static void rpr_maybe_assist(reaper_t *r);

static void *rpr_alloc_huge(reaper_t *r, size_t size, uint8_t tag,
                            unsigned flags) {
    size_t total = sizeof(rpr_huge) + sizeof(rpr_head) + size;
    total = (total + RPR_CELL - 1u) & ~((size_t)RPR_CELL - 1u);
    rpr_huge *hg = (rpr_huge *)rpr_balloc(r, total, RPR_CELL);
    if (!hg) return NULL;
    hg->map_size = total;
    /* A huge newborn during sweep must survive the pending huge sweep */
    hg->marked = (r->phase == REAPER_SWEEP && !r->huge_swept) ? 1 : 0;
    hg->prev = NULL;
    hg->next = r->huge;
    if (r->huge) r->huge->prev = hg;
    r->huge = hg;
    r->n_huge++;
    r->huge_bytes += size;
    r->stats.footprint_bytes += total;

    rpr_head *h = (rpr_head *)(hg + 1);
    h->size = (uint32_t)size;
    h->flags = (uint8_t)(flags | RPR_F_HUGE | RPR_F_GRAY);
    h->tag = tag;
    h->userdata = 0;
    return rpr_obj(h);
}

REAPER_API void *reaper_alloc(reaper_t *r, size_t size, uint8_t tag,
                              unsigned flags) {
    REAPER_ASSERT(r);
    REAPER_ASSERT(size > 0 && size <= UINT32_MAX);
    REAPER_ASSERT(!(flags & REAPER_TRAVERSABLE) || r->cfg.trace);
    REAPER_ASSERT(!(flags & REAPER_FINALIZE) || r->cfg.finalize);

    r->heap_bytes += size;
    if (r->cfg.assist_budget && !r->in_step) rpr_maybe_assist(r);

    uint32_t need = 1u + (uint32_t)((size + RPR_CELL - 1u) >> RPR_CELL_SHIFT);
    void *obj = NULL;

    /* huge threshold: anything over a quarter of an arena's data area */
    uint32_t usable = r->ncells - r->data_start;
    if (need > usable / 4u) {
        obj = rpr_alloc_huge(r, size, tag, flags);
        if (!obj) { r->heap_bytes -= size; return NULL; }
    } else {
        rpr_arena *a = r->alloc_arena;
        uint32_t c = a ? rpr_arena_fit(r, a, need) : UINT32_MAX;
        if (c == UINT32_MAX) {
            for (a = r->arenas; a; a = a->next) {
                if (a == r->alloc_arena) continue;
                c = rpr_arena_fit(r, a, need);
                if (c != UINT32_MAX) break;
            }
        }
        if (c == UINT32_MAX) {
            a = rpr_arena_new(r);
            if (!a) { r->heap_bytes -= size; return NULL; }
            r->stats.footprint_bytes += r->arena_size;
            c = rpr_arena_fit(r, a, need);
            REAPER_ASSERT(c != UINT32_MAX);
        }
        r->alloc_arena = a;
        rpr_head *h = rpr_arena_carve(r, a, c, need);
        h->size = (uint32_t)size;
        h->flags = (uint8_t)(flags | RPR_F_GRAY);  /* born light gray */
        h->tag = tag;
        h->userdata = 0;
        obj = rpr_obj(h);
    }

    if (flags & REAPER_FINALIZE)
        rpr_ptrvec_push(r, &r->finreg, &r->finreg_len, &r->finreg_cap, obj);
    return obj;
}

/* ------------------------------------------------------------------ */
/* Barrier and marking                                                 */
/* ------------------------------------------------------------------ */

REAPER_API void reaper_write_barrier(reaper_t *r, void *obj) {
    rpr_head *h = rpr_hdr(obj);
    if (h->flags & RPR_F_GRAY) return;          /* the whole fast path */
    h->flags |= RPR_F_GRAY;
    r->stats.barrier_hits++;
    if (rpr_is_marked(r, h)) {
        /* black -> dark gray: queue for re-traversal; doubles as the
         * remembered set entry for minor cycles */
        rpr_gray_push(r, obj);
    }
    /* white -> light gray: flag only, nothing else to do */
}

REAPER_API void reaper_mark(reaper_t *r, void *obj) {
    if (!obj) return;
    rpr_head *h = rpr_hdr(obj);
    if (rpr_is_marked(r, h)) return;            /* black or dark gray */
    rpr_set_mark(r, h);
    if (h->flags & (uint8_t)REAPER_TRAVERSABLE) {
        h->flags |= RPR_F_GRAY;                 /* dark gray, queued */
        rpr_gray_push(r, obj);
    }
}

/* Pop and trace one gray object; returns its cell cost. */
static size_t rpr_trace_one(reaper_t *r) {
    void *obj = r->gray[--r->gray_len];
    rpr_head *h = rpr_hdr(obj);
    /* Entries queued by the barrier while idle, or swept back to white
     * while queued, may arrive unmarked — treat them as reachable
     * (conservative: at worst one cycle of floating garbage). */
    if (!rpr_is_marked(r, h)) rpr_set_mark(r, h);
    h->flags &= (uint8_t)~RPR_F_GRAY;           /* -> black */
    r->cfg.trace(r, obj, h->tag, r->cfg.ud);
    return rpr_obj_cells(h);
}

/* ------------------------------------------------------------------ */
/* Cycle control                                                       */
/* ------------------------------------------------------------------ */

static void rpr_update_trigger(reaper_t *r) {
    unsigned pct = r->cfg.pace_percent ? r->cfg.pace_percent : 100u;
    size_t floor_ = r->arena_size;    /* don't thrash tiny heaps */
    size_t t = r->live_bytes + (r->live_bytes / 100u) * pct;
    r->trigger_bytes = t > floor_ ? t : floor_;
}

/* Clear every mark bit (major cycles must start from a clean slate —
 * minor sweeps deliberately leave survivors black). Word-parallel:
 * black 11 -> white 10, free 01 and extent 00 unchanged: m &= ~b. */
static void rpr_clear_marks(reaper_t *r) {
    for (rpr_arena *a = r->arenas; a; a = a->next) {
        uint64_t *b = rpr_bmap(r, a), *m = rpr_mmap_(r, a);
        for (uint32_t w = 0; w < r->nwords; w++) m[w] &= ~b[w];
    }
    for (rpr_huge *hg = r->huge; hg; hg = hg->next) hg->marked = 0;
}

static void rpr_cycle_begin(reaper_t *r, bool minor) {
    REAPER_ASSERT(r->phase == REAPER_IDLE);
    r->cycle_minor = minor;
    if (!minor) rpr_clear_marks(r);
    r->phase = REAPER_MARK;
    r->cfg.roots(r, r->cfg.ud);
}

/* Rebuild an arena's free-cell count and reset its cursor after its
 * bitmaps were transformed by a sweep. Serial boundary walk over the
 * bitmap only (still metadata-only; per-arena free lists are the
 * documented future optimization from Pall's design). */
static void rpr_arena_recount(reaper_t *r, rpr_arena *a) {
    uint32_t free_cells = 0;
    uint32_t c = r->data_start;
    bool in_free = false;
    while (c < r->ncells) {
        unsigned st = rpr_cell_state(r, a, c);
        if (st == RPR_C_FREE) { in_free = true; free_cells++; }
        else if (st == RPR_C_EXTENT) { if (in_free) free_cells++; }
        else in_free = false;
        c++;
    }
    a->free_cells = free_cells;
    a->cursor = r->data_start;
}

/* Sweep one arena (word-parallel bitmap transform + recount). */
static void rpr_sweep_arena(reaper_t *r, rpr_arena *a) {
    uint64_t *b = rpr_bmap(r, a), *m = rpr_mmap_(r, a);
    if (r->cycle_minor) {
        for (uint32_t w = 0; w < r->nwords; w++) {
            uint64_t bw = b[w], mw = m[w];
            b[w] = bw & mw;
            m[w] = bw | mw;
        }
    } else {
        for (uint32_t w = 0; w < r->nwords; w++) {
            uint64_t bw = b[w], mw = m[w];
            b[w] = bw & mw;
            m[w] = bw ^ mw;
        }
    }
    /* the data-start cell must remain a valid boundary even when the
     * arena is entirely empty */
    if (rpr_cell_state(r, a, r->data_start) == RPR_C_EXTENT)
        rpr_cell_set(r, a, r->data_start, RPR_C_FREE);
    a->swept_epoch = r->sweep_epoch;
    rpr_arena_recount(r, a);
}

/* Finalizers + huge sweep — runs once, when the sweep phase opens. */
static void rpr_sweep_begin(reaper_t *r) {
    r->sweep_epoch++;
    r->sweep_cursor = r->arenas;
    r->huge_swept = false;

    /* finalizables first: their memory dies in the bitmap/huge sweep */
    size_t i = 0;
    while (i < r->finreg_len) {
        void *obj = r->finreg[i];
        rpr_head *h = rpr_hdr(obj);
        if (!rpr_is_marked(r, h)) {
            r->cfg.finalize(obj, h->tag, r->cfg.ud);
            r->finreg[i] = r->finreg[--r->finreg_len];
        } else {
            i++;
        }
    }

    /* huge list: unlink and free the unmarked */
    rpr_huge *hg = r->huge;
    while (hg) {
        rpr_huge *next = hg->next;
        rpr_head *h = (rpr_head *)(hg + 1);
        if (!hg->marked) {
            if (hg->prev) hg->prev->next = hg->next; else r->huge = hg->next;
            if (hg->next) hg->next->prev = hg->prev;
            r->n_huge--;
            r->huge_bytes -= h->size;
            r->stats.footprint_bytes -= hg->map_size;
            r->stats.objects_freed++;
            rpr_bfree(r, hg, hg->map_size);
        } else if (!r->cycle_minor) {
            hg->marked = 0;   /* black -> white for the next major */
        }
        hg = next;
    }
    r->huge_swept = true;
}

/* Close the cycle: recompute the live estimate and re-arm the pacer. */
static void rpr_sweep_end(reaper_t *r) {
    size_t live_cells = 0;
    rpr_arena **link = &r->arenas;
    rpr_arena *keep_empty = NULL;
    while (*link) {
        rpr_arena *a = *link;
        uint32_t used = (r->ncells - r->data_start) - a->free_cells;
        if (used == 0 && a != r->alloc_arena) {
            /* release empty arenas, cache a single spare */
            if (!keep_empty) {
                keep_empty = a;
                link = &a->next;
            } else {
                *link = a->next;
                r->n_arenas--;
                r->stats.footprint_bytes -= r->arena_size;
                rpr_bfree(r, a, r->arena_size);
            }
        } else {
            live_cells += used;
            link = &a->next;
        }
    }
    if (r->alloc_arena) {
        /* make sure the cached target wasn't freed above (it can't be:
         * we skip alloc_arena), but its cursor may be stale */
        r->alloc_arena->cursor = r->data_start;
    }

    r->live_bytes = ((size_t)live_cells << RPR_CELL_SHIFT) + r->huge_bytes;
    r->heap_bytes = r->live_bytes;
    rpr_update_trigger(r);

    if (r->cycle_minor) { r->stats.cycles_minor++; r->minors_done++; }
    else                { r->stats.cycles_major++; r->minors_done = 0; }
    r->phase = REAPER_IDLE;
}

/* The atomic step: re-scan roots and drain to a fixpoint. In a
 * cooperative host nothing runs between these calls, so reaching an
 * empty gray stack right after a root scan is a true fixpoint. */
static void rpr_atomic(reaper_t *r) {
    for (;;) {
        r->cfg.roots(r, r->cfg.ud);
        if (r->gray_len == 0) break;
        while (r->gray_len) (void)rpr_trace_one(r);
    }
    rpr_sweep_begin(r);
    r->phase = REAPER_SWEEP;
}

REAPER_API size_t reaper_step(reaper_t *r, size_t budget) {
    REAPER_ASSERT(r);
    if (r->in_step) return 0;
    r->in_step = true;

    if (r->phase == REAPER_IDLE) {
        if (r->heap_bytes >= r->trigger_bytes) {
            bool minor = r->cfg.minor_per_major != 0 &&
                         r->minors_done < r->cfg.minor_per_major;
            rpr_cycle_begin(r, minor);
        } else {
            r->in_step = false;
            return 0;
        }
    }

    size_t spent = 0;
    while (spent < budget && r->phase != REAPER_IDLE) {
        if (r->phase == REAPER_MARK) {
            if (r->gray_len) {
                spent += rpr_trace_one(r);
            } else {
                rpr_atomic(r);
                spent += 64;   /* nominal cost for the phase change */
            }
        } else { /* REAPER_SWEEP */
            if (r->sweep_cursor) {
                rpr_arena *a = r->sweep_cursor;
                r->sweep_cursor = a->next;
                if (a->swept_epoch != r->sweep_epoch) {
                    rpr_sweep_arena(r, a);
                    spent += r->ncells - r->data_start;
                }
            } else {
                rpr_sweep_end(r);
            }
        }
    }

    r->in_step = false;
    return r->phase == REAPER_IDLE ? 0 : 1;
}

static void rpr_maybe_assist(reaper_t *r) {
    if (r->phase != REAPER_IDLE || r->heap_bytes >= r->trigger_bytes)
        (void)reaper_step(r, r->cfg.assist_budget);
}

REAPER_API bool reaper_needs_step(const reaper_t *r) {
    return r->phase != REAPER_IDLE || r->heap_bytes >= r->trigger_bytes;
}

static void rpr_collect(reaper_t *r, bool minor) {
    REAPER_ASSERT(!r->in_step);
    /* finish any in-flight cycle first */
    while (r->phase != REAPER_IDLE) (void)reaper_step(r, SIZE_MAX);
    rpr_cycle_begin(r, minor);
    while (r->phase != REAPER_IDLE) (void)reaper_step(r, SIZE_MAX);
}

REAPER_API void reaper_collect(reaper_t *r)       { rpr_collect(r, false); }
REAPER_API void reaper_collect_minor(reaper_t *r) { rpr_collect(r, true); }

/* ------------------------------------------------------------------ */
/* Explicit free                                                       */
/* ------------------------------------------------------------------ */

REAPER_API void reaper_free(reaper_t *r, void *obj) {
    REAPER_ASSERT(r && obj);
    REAPER_ASSERT(r->phase == REAPER_IDLE);
    rpr_head *h = rpr_hdr(obj);

    if (h->flags & (uint8_t)REAPER_FINALIZE) {
        r->cfg.finalize(obj, h->tag, r->cfg.ud);
        rpr_ptrvec_remove(r->finreg, &r->finreg_len, obj);
    }
    /* it may sit on the gray stack as a remembered-set entry */
    if ((h->flags & RPR_F_GRAY) && rpr_is_marked(r, h))
        rpr_ptrvec_remove(r->gray, &r->gray_len, obj);

    size_t size = h->size;
    if (rpr_is_huge(h)) {
        rpr_huge *hg = rpr_huge_of(h);
        if (hg->prev) hg->prev->next = hg->next; else r->huge = hg->next;
        if (hg->next) hg->next->prev = hg->prev;
        r->n_huge--;
        r->huge_bytes -= size;
        r->stats.footprint_bytes -= hg->map_size;
        rpr_bfree(r, hg, hg->map_size);
    } else {
        rpr_arena *a = rpr_arena_of(r, h);
        uint32_t c = rpr_cell_of(r, a, h);
        uint32_t n = rpr_obj_cells(h);
        /* head -> free; extents already read as free-run interior */
        rpr_cell_set(r, a, c, RPR_C_FREE);
        a->free_cells += n;
        if (a->cursor > c) a->cursor = c;
    }
    r->stats.objects_freed++;
    r->heap_bytes -= size < r->heap_bytes ? size : r->heap_bytes;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

REAPER_API int reaper_phase(const reaper_t *r) { return r->phase; }

REAPER_API unsigned reaper_color(const reaper_t *r, const void *obj) {
    const rpr_head *h = rpr_hdr(obj);
    bool marked = rpr_is_marked(r, h);
    bool gray = (h->flags & RPR_F_GRAY) != 0;
    if (marked) return gray ? REAPER_DARK_GRAY : REAPER_BLACK;
    return gray ? REAPER_LIGHT_GRAY : REAPER_WHITE;
}

REAPER_API void reaper_get_stats(const reaper_t *r, reaper_stats *out) {
    *out = r->stats;
    out->live_bytes = r->live_bytes;
    out->heap_bytes = r->heap_bytes;
    out->arenas = r->n_arenas;
    out->huge_objects = r->n_huge;
}

REAPER_API size_t   reaper_obj_size(const void *obj) { return rpr_hdr(obj)->size; }
REAPER_API uint8_t  reaper_obj_tag(const void *obj)  { return rpr_hdr(obj)->tag; }
REAPER_API uint16_t reaper_obj_userdata(const void *obj) {
    return rpr_hdr(obj)->userdata;
}
REAPER_API void reaper_obj_set_userdata(void *obj, uint16_t v) {
    rpr_hdr(obj)->userdata = v;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

REAPER_API reaper_t *reaper_create(const reaper_config *cfg) {
    REAPER_ASSERT(cfg && cfg->roots);

    reaper_config c = *cfg;
    if (!c.backing.alloc) {
        c.backing.alloc = rpr_sys_alloc;
        c.backing.free_ = rpr_sys_free;
        c.backing.ud = NULL;
    }
    if (!c.arena_size) c.arena_size = RPR_DEF_ARENA;
    REAPER_ASSERT(c.arena_size >= RPR_MIN_ARENA);
    REAPER_ASSERT((c.arena_size & (c.arena_size - 1)) == 0);

    reaper_t *r = (reaper_t *)c.backing.alloc(sizeof(reaper_t), RPR_CELL,
                                              c.backing.ud);
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->cfg = c;
    r->arena_size = c.arena_size;
    r->ncells = (uint32_t)(c.arena_size >> RPR_CELL_SHIFT);
    r->nwords = r->ncells >> 6;

    size_t meta = sizeof(rpr_arena) +
                  (size_t)r->nwords * 2u * sizeof(uint64_t);
    r->data_start = (uint32_t)((meta + RPR_CELL - 1u) >> RPR_CELL_SHIFT);

    r->phase = REAPER_IDLE;
    r->sweep_epoch = 1;
    rpr_update_trigger(r);
    return r;
}

REAPER_API void reaper_destroy(reaper_t *r) {
    if (!r) return;
    /* deterministic teardown: every finalizable object is finalized */
    for (size_t i = 0; i < r->finreg_len; i++) {
        void *obj = r->finreg[i];
        r->cfg.finalize(obj, rpr_hdr(obj)->tag, r->cfg.ud);
    }
    rpr_huge *hg = r->huge;
    while (hg) {
        rpr_huge *next = hg->next;
        rpr_bfree(r, hg, hg->map_size);
        hg = next;
    }
    rpr_arena *a = r->arenas;
    while (a) {
        rpr_arena *next = a->next;
        rpr_bfree(r, a, r->arena_size);
        a = next;
    }
    rpr_bfree(r, r->gray, r->gray_cap * sizeof(void *));
    rpr_bfree(r, r->finreg, r->finreg_cap * sizeof(void *));
    reaper_allocator backing = r->cfg.backing;
    backing.free_(r, sizeof(reaper_t), backing.ud);
}

#endif /* REAPER_IMPLEMENTATION */
#endif /* REAPER_H */
