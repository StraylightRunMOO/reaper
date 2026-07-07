/* =========================================================================
 * reaper.hpp — C++17 RAII facade over reaper.h
 *
 * The C++ wrapper preserves the single-header, dependency-free model of the
 * C library. Consumers define `REAPER_IMPLEMENTATION` in exactly one
 * translation unit before including this header:
 *
 *     #define REAPER_IMPLEMENTATION
 *     #include "reaper.hpp"
 *
 * The wrapper adds:
 *   - Move-only RAII ownership of the `reaper_t*`.
 *   - `reaper_cpp::make<T>(args...)` that forwards to `reaper_alloc` + placement new.
 *   - Per-type traits (`reaper_cpp::traits<T>`) for tag, traversability, and
 *     finalization.
 *   - Automatic C++ destructor calls for collected objects.
 *   - Idiomatic C++ callbacks (`std::function`) while keeping the hot allocation
 *     / barrier / mark paths inline and allocation-free.
 *
 * The wrapper stores its configuration and per-tag finalizer table in a stable
 * heap-allocated `Impl`. The C API userdata points to this `Impl`, and `Impl`
 * tracks the current owning `Gc*` so that callbacks receive a valid `Gc&` even
 * after the `Gc` object has been moved.
 * ========================================================================= */

#ifndef REAPER_HPP
#define REAPER_HPP

#include "reaper.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace reaper_cpp {

/* ---------------------------------------------------------------------- */
/* Type traits (user-specializable)                                       */
/* ---------------------------------------------------------------------- */

/**
 * Specialize `reaper_cpp::traits<MyType>` to control how `reaper_cpp::make<MyType>`
 * allocates and registers objects.
 *
 *   tag            — passed to `reaper_alloc` and to trace/finalize callbacks.
 *   traversable    — true  => object may hold GC references (REAPER_TRAVERSABLE).
 *   needs_finalize — true  => object's destructor must run when collected.
 */
template<typename T>
struct traits {
    static constexpr uint8_t tag = 0;
    static constexpr bool traversable = false;
    static constexpr bool needs_finalize = !std::is_trivially_destructible_v<T>;
};

/* ---------------------------------------------------------------------- */
/* GC handle                                                              */
/* ---------------------------------------------------------------------- */

class Gc {
public:
    struct Config {
        std::function<void(Gc&, void* obj, uint8_t tag)> trace;
        std::function<void(Gc&)> roots;
        std::function<void(void* obj, uint8_t tag)> finalize;

        size_t arena_size = 0;
        unsigned pace_percent = 0;
        unsigned minor_per_major = 0;
        size_t assist_budget = 0;
    };

    explicit Gc(const Config& cfg);
    ~Gc();

    Gc(Gc&& other) noexcept;
    Gc& operator=(Gc&& other) noexcept;

    Gc(const Gc&) = delete;
    Gc& operator=(const Gc&) = delete;

    /* Raw allocation (exposes the underlying C API). */
    void* allocate(size_t size, uint8_t tag = 0, unsigned flags = 0);

    /* Explicit free. Precondition: `phase() == REAPER_IDLE`. */
    void free(void* obj);

    /**
     * Allocate and construct a T in GC-managed memory.
     * Forwards `args...` to T's constructor via placement new.
     */
    template<typename T, typename... Args>
    T* make(Args&&... args) {
        static_assert(std::is_constructible_v<T, Args...>,
                      "T must be constructible from the provided arguments");

        constexpr uint8_t tag = traits<T>::tag;
        constexpr unsigned flags =
            (traits<T>::traversable ? REAPER_TRAVERSABLE : 0) |
            (traits<T>::needs_finalize ? REAPER_FINALIZE : 0);

        if constexpr (traits<T>::needs_finalize) {
            register_finalizer<T>(tag);
        }

        void* mem = allocate(sizeof(T), tag, flags);
        return new (mem) T(std::forward<Args>(args)...);
    }

    /* Call after storing a GC-managed reference into `obj`. */
    void write_barrier(void* obj);

    /* Mark a GC-managed reference (only meaningful inside trace/roots). */
    void mark(void* obj);

    /* Bounded incremental work. Returns non-zero while a cycle is in flight. */
    size_t step(size_t budget);

    /* True if the collector wants a step. */
    bool needs_step() const;

    /* Full stop-the-world cycles. */
    void collect();
    void collect_minor();

    /* Introspection. */
    int phase() const;
    unsigned color(const void* obj) const;
    reaper_stats stats() const;

    static size_t object_size(const void* obj);
    static uint8_t object_tag(const void* obj);
    static uint16_t object_userdata(const void* obj);
    static void object_set_userdata(void* obj, uint16_t v);

private:
    struct Impl {
        reaper_t* r = nullptr;
        Config cfg;
        std::array<std::function<void(void*, uint8_t)>, 256> finalizers{};
        Gc* owner = nullptr;

        Impl(const Config& c, Gc* self) : cfg(c), owner(self) {}
        ~Impl() {
            if (r) reaper_destroy(r);
        }
    };

    Impl* impl_ = nullptr;

    static void trace_thunk(reaper_t* r, void* obj, uint8_t tag, void* ud);
    static void roots_thunk(reaper_t* r, void* ud);
    static void finalize_thunk(void* obj, uint8_t tag, void* ud);

    template<typename T>
    void register_finalizer(uint8_t tag) {
        auto& slot = impl_->finalizers[tag];
        if (!slot) {
            slot = [](void* obj, uint8_t) {
                static_cast<T*>(obj)->~T();
            };
        }
    }
};

/* ---------------------------------------------------------------------- */
/* Inline implementation                                                  */
/* ---------------------------------------------------------------------- */

inline Gc::Gc(const Config& cfg) : impl_(new Impl(cfg, this)) {
    reaper_config rc;
    std::memset(&rc, 0, sizeof(rc));
    rc.trace = trace_thunk;
    rc.roots = roots_thunk;
    rc.finalize = finalize_thunk;
    rc.ud = impl_;
    rc.arena_size = cfg.arena_size;
    rc.pace_percent = cfg.pace_percent;
    rc.minor_per_major = cfg.minor_per_major;
    rc.assist_budget = cfg.assist_budget;
    impl_->r = reaper_create(&rc);
}

inline Gc::~Gc() {
    delete impl_;
}

inline Gc::Gc(Gc&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
    if (impl_) impl_->owner = this;
}

inline Gc& Gc::operator=(Gc&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
        if (impl_) impl_->owner = this;
    }
    return *this;
}

inline void* Gc::allocate(size_t size, uint8_t tag, unsigned flags) {
    return impl_ ? reaper_alloc(impl_->r, size, tag, flags) : nullptr;
}

inline void Gc::free(void* obj) {
    if (impl_) reaper_free(impl_->r, obj);
}

inline void Gc::write_barrier(void* obj) {
    if (impl_) reaper_write_barrier(impl_->r, obj);
}

inline void Gc::mark(void* obj) {
    if (impl_) reaper_mark(impl_->r, obj);
}

inline size_t Gc::step(size_t budget) {
    return impl_ ? reaper_step(impl_->r, budget) : 0;
}

inline bool Gc::needs_step() const {
    return impl_ && reaper_needs_step(impl_->r);
}

inline void Gc::collect() {
    if (impl_) reaper_collect(impl_->r);
}

inline void Gc::collect_minor() {
    if (impl_) reaper_collect_minor(impl_->r);
}

inline int Gc::phase() const {
    return impl_ ? reaper_phase(impl_->r) : REAPER_IDLE;
}

inline unsigned Gc::color(const void* obj) const {
    return impl_ ? reaper_color(impl_->r, obj) : REAPER_WHITE;
}

inline reaper_stats Gc::stats() const {
    reaper_stats s{};
    if (impl_) reaper_get_stats(impl_->r, &s);
    return s;
}

inline size_t Gc::object_size(const void* obj) {
    return reaper_obj_size(obj);
}

inline uint8_t Gc::object_tag(const void* obj) {
    return reaper_obj_tag(obj);
}

inline uint16_t Gc::object_userdata(const void* obj) {
    return reaper_obj_userdata(obj);
}

inline void Gc::object_set_userdata(void* obj, uint16_t v) {
    reaper_obj_set_userdata(obj, v);
}

inline void Gc::trace_thunk(reaper_t* /*r*/, void* obj, uint8_t tag, void* ud) {
    Impl* impl = static_cast<Impl*>(ud);
    if (impl->cfg.trace) impl->cfg.trace(*impl->owner, obj, tag);
}

inline void Gc::roots_thunk(reaper_t* /*r*/, void* ud) {
    Impl* impl = static_cast<Impl*>(ud);
    if (impl->cfg.roots) impl->cfg.roots(*impl->owner);
}

inline void Gc::finalize_thunk(void* obj, uint8_t tag, void* ud) {
    Impl* impl = static_cast<Impl*>(ud);
    auto& fin = impl->finalizers[tag];
    if (fin) fin(obj, tag);
    if (impl->cfg.finalize) impl->cfg.finalize(obj, tag);
}

} // namespace reaper_cpp

#endif /* REAPER_HPP */
