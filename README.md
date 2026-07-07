# Reaper

A quad-color incremental, generational, non-moving garbage collector for C11
and C++17. The C library is a single header (`reaper.h`); a C++17 RAII wrapper
(`reaper.hpp`) is also provided. Both have no runtime dependencies beyond libc
and the C++ standard library.
Reaper is intended to pair with arena allocators and cooperatively scheduled
runtimes: short-lived data lives in bump arenas that are reclaimed wholesale,
while longer-lived objects are handed to Reaper and marking is driven in
bounded slices that interleave with the mutator.

Reaper is one of three sibling components for building language runtimes in C:

- **[suspenders.h](https://github.com/StraylightRunMOO/suspenders.h)** — stackful
coroutines and cooperative scheduling.
- **[memento](https://github.com/StraylightRunMOO/memento)** — arena/bump
allocators with backing-store adapters.
- **reaper** (this project) — incremental, generational GC for the long-lived
heap.

## Features

- **Single-header C11 library**: drop `include/reaper.h` into a project and
define `REAPER_IMPLEMENTATION` in exactly one `.c` file.
- **C++17 RAII wrapper**: `include/reaper.hpp` provides `reaper_cpp::Gc` with
move-only ownership, `reaper_cpp::make<T>()`, automatic destructors, and
`std::function` callbacks.
- **Quad-color marking**: white, light gray, dark gray, black. New traversable
objects are born light gray so initialization writes do not pay the write-barrier
cost.
- **Incremental**: `reaper_step(r, budget)` advances the collector by a bounded
amount of work; pauses are small and predictable.
- **Generational**: optional minor cycles keep old survivors black and collect
young garbage cheaply. Configured via `minor_per_major`.
- **Non-moving**: objects never relocate, so C pointers remain stable.
- **Paced collection**: Go-style pacer triggers a cycle when the heap grows past
`live * (1 + pace_percent/100)`. Allocation assist can drive stepping
automatically.
- **Deterministic finalization**: `finalize` runs exactly once per dead object
and once for every remaining object at `reaper_destroy`.
- **Bitmap-driven sweep**: word-parallel bitmap transforms; sweep never touches
object data.

## Quick start

```c
#define REAPER_IMPLEMENTATION
#include "reaper.h"

static reaper_t *g_r;

void trace(reaper_t *r, void *obj, uint8_t tag, void *ud) {
    my_object *o = obj;
    reaper_mark(r, o->child);
}

void roots(reaper_t *r, void *ud) {
    (void)ud;
    reaper_mark(r, g_root);
}

int main(void) {
    reaper_config cfg = {0};
    cfg.trace = trace;
    cfg.roots = roots;

    reaper_t *r = reaper_create(&cfg);
    g_root = reaper_alloc(r, sizeof(my_object), 1, REAPER_TRAVERSABLE);
    reaper_collect(r);
    reaper_destroy(r);
    return 0;
}
```

### C++ quick start

```cpp
#define REAPER_IMPLEMENTATION
#include "reaper.hpp"

struct node {
    node *left = nullptr;
    node *right = nullptr;
};

namespace reaper {
template<>
struct traits<node> {
    static constexpr uint8_t tag = 1;
    static constexpr bool traversable = true;
    static constexpr bool needs_finalize = false;
};
} // namespace reaper

static node *g_root = nullptr;

int main() {
    reaper_cpp::Gc::Config cfg;
    cfg.trace = [](reaper_cpp::Gc& gc, void *obj, uint8_t) {
        node *n = static_cast<node*>(obj);
        gc.mark(n->left);
        gc.mark(n->right);
    };
    cfg.roots = [](reaper_cpp::Gc& gc) { gc.mark(g_root); };

    reaper_cpp::Gc gc(cfg);
    g_root = gc.make<node>();
    gc.collect();
    return 0;
}
```

## Build and install

### CMake (recommended)

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run benchmarks
./build/benchmarks/reaper_bench
./build/benchmarks/reaper_cpp_bench

# Run examples
./build/examples/reaper_basic
./build/examples/reaper_cooperative
./build/examples/reaper_basic_cpp
./build/examples/reaper_cooperative_cpp
```

Build options (default ON when this is the top-level project):

```bash
cmake -B build -S . -DREAPER_BUILD_TESTS=OFF -DREAPER_BUILD_EXAMPLES=OFF
```

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    reaper
    GIT_REPOSITORY https://github.com/StraylightRunMOO/reaper.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(reaper)

target_link_libraries(my_runtime PRIVATE StraylightRunMOO::reaper)
```

### Vendored drop-in

Copy `include/reaper.h` (or `include/reaper.hpp`) into your project and add its
directory to the include path. In exactly one translation unit:

```c
#define REAPER_IMPLEMENTATION
#include "reaper.h"
```

For C++:

```cpp
#define REAPER_IMPLEMENTATION
#include "reaper.hpp"
```

### Install

```bash
cmake -B build -S . -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

After installation, downstream projects can use:

```cmake
find_package(reaper REQUIRED)
target_link_libraries(my_runtime PRIVATE StraylightRunMOO::reaper)
```

## C API overview

### Lifecycle

- `reaper_t *reaper_create(const reaper_config *cfg)`
- `void reaper_destroy(reaper_t *r)` — finalizes everything and frees backing
  memory.

### Allocation

- `void *reaper_alloc(reaper_t *r, size_t size, uint8_t tag, unsigned flags)`
- `void reaper_free(reaper_t *r, void *obj)` — only legal while
  `reaper_phase(r) == REAPER_IDLE`.

Flags:

- `REAPER_TRAVERSABLE` — object contains GC-managed references; the `trace`
  callback will visit it.
- `REAPER_FINALIZE` — object needs `cfg.finalize` called when collected.

### Roots, barriers, and marking

- `void reaper_mark(reaper_t *r, void *obj)` — call from `trace` and `roots`
  callbacks.
- `void reaper_write_barrier(reaper_t *r, void *obj)` — call after storing a
  GC reference **into** `obj`.

### Collection control

- `size_t reaper_step(reaper_t *r, size_t budget)` — bounded incremental work.
  Returns non-zero while a cycle is in flight.
- `bool reaper_needs_step(const reaper_t *r)`
- `void reaper_collect(reaper_t *r)` — full stop-the-world major cycle.
- `void reaper_collect_minor(reaper_t *r)` — full minor cycle.

### Introspection

- `int reaper_phase(const reaper_t *r)` — `REAPER_IDLE`, `REAPER_MARK`, or
  `REAPER_SWEEP`.
- `unsigned reaper_color(const reaper_t *r, const void *obj)` —
  `REAPER_WHITE`, `REAPER_LIGHT_GRAY`, `REAPER_DARK_GRAY`, `REAPER_BLACK`.
- `void reaper_get_stats(const reaper_t *r, reaper_stats *out)`
- `size_t reaper_obj_size(const void *obj)`
- `uint8_t reaper_obj_tag(const void *obj)`
- `uint16_t reaper_obj_userdata(const void *obj)`
- `void reaper_obj_set_userdata(void *obj, uint16_t v)`

## C++ API overview

### `reaper_cpp::Gc`

Move-only RAII handle. Destroying the `Gc` finalizes all remaining objects and
frees backing memory.

```cpp
reaper_cpp::Gc gc(cfg);
reaper_cpp::Gc moved = std::move(gc); // gc is left in a valid but empty state
```

### `reaper_cpp::traits<T>`

Specialize to control how `reaper_cpp::make<T>()` allocates objects:

```cpp
namespace reaper {
template<>
struct traits<MyType> {
    static constexpr uint8_t tag = 1;
    static constexpr bool traversable = true;
    static constexpr bool needs_finalize = true;
};
} // namespace reaper
```

- `tag` — passed to `reaper_alloc` and to `trace`/`finalize` callbacks.
- `traversable` — enables `REAPER_TRAVERSABLE`.
- `needs_finalize` — enables `REAPER_FINALIZE` and calls `T::~T()` when the
  object is collected. Defaults to `!std::is_trivially_destructible_v<T>`.

### Allocation

```cpp
void* raw = gc.allocate(sizeof(T), tag, flags);
T* obj = gc.make<T>(args...);  // allocate + placement-new
```

### Roots, barriers, and marking

```cpp
gc.mark(child);
gc.write_barrier(parent);
```

### Collection control

```cpp
gc.step(budget);
gc.needs_step();
gc.collect();       // major
gc.collect_minor(); // minor
```

### Introspection

```cpp
gc.phase();
gc.color(obj);
gc.stats();
reaper_cpp::Gc::object_size(obj);
reaper_cpp::Gc::object_tag(obj);
```

## C API reference

### Configuration

```c
typedef struct reaper_config {
    reaper_allocator backing;  /* zeroed = aligned_alloc/free */
    reaper_trace_fn  trace;    /* required for REAPER_TRAVERSABLE objects */
    reaper_roots_fn  roots;    /* required */
    reaper_final_fn  finalize; /* required if REAPER_FINALIZE used */
    void    *ud;               /* passed to all callbacks */
    size_t   arena_size;       /* power of two, >= 64 KiB; 0 = 256 KiB */
    unsigned pace_percent;     /* 0 = 100 */
    unsigned minor_per_major;  /* 0 disables minor/generational mode */
    size_t   assist_budget;    /* per-allocation step budget; 0 = host-driven */
} reaper_config;
```

## Runtime contracts

- **Single-threaded / cooperative only**: all calls, including callbacks, must
  come from one thread or one cooperative scheduler. Drive concurrency by
  calling `reaper_step` from a coroutine, not from another thread.
- **`trace` must not allocate or free**: it is called while the collector owns
  the gray stack and bitmaps.
- **`roots` must be idempotent**: it may be invoked several times per cycle.
- **`finalize` must not resurrect the object**: memory is reclaimed as soon as
  the callback returns.
- **`reaper_free` precondition**: calling `reaper_free` outside of
  `REAPER_IDLE` is an assertion failure.

## Integration with the sibling components

### suspenders.h (cooperative scheduling)

Run `reaper_step` from a scheduler coroutine at yield points:

```c
void gc_coroutine(void) {
    while (running) {
        if (reaper_needs_step(gc))
            reaper_step(gc, GC_BUDGET);
        suspenders_yield();
    }
}
```

This gives bounded pause times without preempting the mutator.

### memento (arena allocator)

Use Memento for short-lived bump allocations and hand escaping objects to
Reaper. A Memento allocator can be wired into Reaper's `backing` field:

```c
reaper_config cfg = {0};
cfg.backing.alloc  = my_memento_aligned_alloc;
cfg.backing.free_  = my_memento_aligned_free;
cfg.backing.ud     = my_arena;
```

Because arenas are released wholesale, the common case of short-lived objects
never touches the GC at all.

## Design notes

- **Memory layout**: power-of-two arenas aligned to their own size, carved into
  16-byte cells. Two bitmaps per arena (`block` and `mark`) encode four cell
  states.
- **Sweep transform**: word-parallel bitmap operations. Major sweep frees white
  objects and flips black to white; minor sweep frees white objects and keeps
  survivors black.
- **Write barrier**: fast path is one load + test of the gray bit. White objects
  become light gray; black objects become dark gray and are queued for
  re-traversal. Dark-gray queued objects double as the remembered set for minor
  cycles.
- **Huge objects**: allocations larger than a quarter of the arena data area are
  stored on a separate linked list.

See the top-of-file commentary in `include/reaper.h` for the full design
rationale, including the debt to Mike Pall's LuaJIT 3.0 GC and Go's pacer.

## Testing

The C test suite is in `tests/test_reaper.c`; the C++ suite is in
`tests/test_reaper_cpp.cpp`. Both are built and run via CMake:

```bash
ctest --test-dir build --output-on-failure
```

The tests cover creation/finalization, allocation metadata, major/minor
collection, write-barrier behavior, incremental stepping, remembered sets,
huge objects, explicit free, multi-arena chains, empty-arena release, pacer
triggering, allocation assist, and a randomized stress graph.

## Benchmarking

```bash
./build/benchmarks/reaper_bench
```

The C benchmarks are in `benchmarks/bench_reaper.c`; the C++ benchmarks are in
`benchmarks/bench_reaper_cpp.cpp`. They measure allocation throughput,
full-collection latency, incremental step latency, and the cost difference
between major and minor cycles.

## License

MIT — see [LICENSE](LICENSE).
