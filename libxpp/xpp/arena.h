/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arena.h - Arena<N>: a fixed-size bump allocator for short-lived objects.
 *
 *   Arena<256> a;            // buffer inline, 0 heap allocs
 *   void* p = a.allocate(64); // bump, returns nullptr if full
 *   a.owns(p);                // true — O(1) pointer range check
 *   a.reset();                // pos = begin, keep buffer for reuse
 *
 * Storage strategy (compile-time, via ArenaStorage specialization):
 *   N ≤ 256  → inline buffer (member), sizeof(Arena<N>) ≈ N + 16
 *   N > 256  → heap-allocated buffer, sizeof(Arena<N>) ≈ 24
 *
 * No destructor tracking — caller is responsible for ~T().
 * No chunk-list growth — allocate() returns nullptr on overflow.
 *
 * C++17-compatible. Header-only.
 */

#ifndef XPP_ARENA_H
#define XPP_ARENA_H

#include <cstddef>
#include <cstdint>
#include <utility>

namespace xpp {

/* ── Inline threshold ──────────────────────────────────────────────── */
// Arenas with N ≤ k_inline_threshold store the buffer inline (zero
// heap allocation). Larger arenas heap-allocate the buffer at
// construction (one allocation).
static const size_t k_arena_inline_threshold = 256;

namespace _ {

/* ── align_up helper ───────────────────────────────────────────────── */
inline char *align_up(char *p, size_t align) {
  uintptr_t i = reinterpret_cast<uintptr_t>(p);
  uintptr_t a = (i + align - 1) & ~(align - 1);
  return reinterpret_cast<char *>(a);
}

/* ── ArenaStorage: inline vs heap specialization ───────────────────── */

template <size_t N, bool Inline = (N <= k_arena_inline_threshold)> struct ArenaStorage;

/** @brief Inline storage: buffer is a member. Zero heap allocation. */
template <size_t N> struct ArenaStorage<N, true> {
  alignas(std::max_align_t) char buf[N];

  char *begin() const {
    return const_cast<char *>(buf);
  }
};

/** @brief Heap storage: buffer is heap-allocated at construction. */
template <size_t N> struct ArenaStorage<N, false> {
  char *ptr;

  ArenaStorage() : ptr(static_cast<char *>(::operator new(N))) {}
  ~ArenaStorage() {
    ::operator delete(ptr);
  }
  ArenaStorage(ArenaStorage &&o) noexcept : ptr(o.ptr) {
    o.ptr = nullptr;
  }
  ArenaStorage &operator=(ArenaStorage &&o) noexcept {
    if (this != &o) {
      ::operator delete(ptr);
      ptr   = o.ptr;
      o.ptr = nullptr;
    }
    return *this;
  }
  ArenaStorage(const ArenaStorage &)            = delete;
  ArenaStorage &operator=(const ArenaStorage &) = delete;

  char *begin() const {
    return ptr;
  }
};

} // namespace _

/* ── Arena<N> ──────────────────────────────────────────────────────── */

/**
 * @brief Fixed-size bump allocator.
 *
 * Allocates memory by bumping a pointer. Memory is never individually
 * freed — only bulk-freed via arena destruction or reset().
 *
 * @tparam N  Buffer size in bytes. N ≤ 256 uses inline storage (zero
 *            heap allocation); N > 256 uses heap storage (one
 *            allocation at construction).
 */
template <size_t N> class Arena {
public:
  static constexpr size_t capacity = N;

  Arena() : m_pos(m_storage.begin()), m_end(m_storage.begin() + N) {}

  Arena(Arena &&o) noexcept : m_storage(std::move(o.m_storage)) {
    // For inline storage, m_pos/m_end pointed into the old buffer.
    // Recompute relative to our buffer.
    if (N <= k_arena_inline_threshold) {
      size_t used = static_cast<size_t>(o.m_pos - o.m_end + N);
      m_pos       = m_storage.begin() + used;
      m_end       = m_storage.begin() + N;
    } else {
      // Heap storage: pointers are absolute, still valid after move.
      m_pos = o.m_pos;
      m_end = o.m_end;
    }
    o.m_pos = nullptr;
    o.m_end = nullptr;
  }

  Arena &operator=(Arena &&o) noexcept {
    if (this != &o) {
      m_storage = std::move(o.m_storage);
      if (N <= k_arena_inline_threshold) {
        size_t used = static_cast<size_t>(o.m_pos - (o.m_end - N));
        m_pos       = m_storage.begin() + used;
        m_end       = m_storage.begin() + N;
      } else {
        m_pos = o.m_pos;
        m_end = o.m_end;
      }
      o.m_pos = nullptr;
      o.m_end = nullptr;
    }
    return *this;
  }

  Arena(const Arena &)            = delete;
  Arena &operator=(const Arena &) = delete;

  ~Arena() = default;

  /**
   * @brief Bump-allocate @p size bytes with @p align alignment.
   * @return Pointer to allocated memory, or nullptr if the arena
   *         doesn't have enough space. Caller must check.
   */
  void *allocate(size_t size, size_t align = alignof(std::max_align_t)) {
    char *p = _::align_up(m_pos, align);
    if (p + size > m_end) return nullptr;
    m_pos = p + size;
    return p;
  }

  /**
   * @brief Allocate and construct a T in the arena.
   *
   * Caller is responsible for calling ~T() — the arena does not
   * track destructors.
   */
  template <class T, class... Args> T *make(Args &&...args) {
    void *p = allocate(sizeof(T), alignof(T));
    if (!p) return nullptr;
    return ::new (p) T(std::forward<Args>(args)...);
  }

  /** @brief True if @p p was allocated from this arena. O(1). */
  bool owns(const void *p) const {
    const char *cp = static_cast<const char *>(p);
    return cp >= m_storage.begin() && cp < m_end;
  }

  /** @brief Reset bump pointer to start. Buffer stays allocated. */
  void reset() {
    m_pos = m_storage.begin();
  }

  /** @brief Total buffer size in bytes. */
  static constexpr size_t total_capacity() {
    return N;
  }

  /** @brief Bytes still available (before any alignment padding). */
  size_t remaining() const {
    return static_cast<size_t>(m_end - m_pos);
  }

  /** @brief Bytes handed out so far. */
  size_t used() const {
    return static_cast<size_t>(m_pos - m_storage.begin());
  }

private:
  _::ArenaStorage<N> m_storage;
  char              *m_pos;
  char              *m_end;
};

/* ── Compile-time size guarantees ──────────────────────────────────── */

// Inline: sizeof includes the buffer.
static_assert(sizeof(Arena<128>) >= 128, "Inline Arena<N> must include the buffer in sizeof");
static_assert(sizeof(Arena<256>) >= 256, "Inline Arena<N> must include the buffer in sizeof");

// Heap: sizeof should NOT include the buffer (just pointers + storage).
static_assert(sizeof(Arena<4096>) < 256,
              "Heap Arena<N> must not include the full buffer in sizeof");

} // namespace xpp

#endif // XPP_ARENA_H
