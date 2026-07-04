/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * allocate_promise.h — Internal: arena-aware allocation for PromiseNode.
 *
 * KJ-style explicit arena passing: no thread-local, no guard. All
 * PromiseNode allocations go through allocate_promise(), which adds an
 * 8-byte header storing the arena pointer:
 *
 *   Layout: [arena_ptr (8B)][node data]
 *                             ^
 *                             returned pointer
 *
 *   arena_ptr != null → arena-allocated, deallocate skips (arena bulk-frees)
 *   arena_ptr == null → heap-allocated, deallocate frees via ::operator delete
 *
 * PromiseNodeAllocator::deallocate reads the header to route correctly.
 * Own<PromiseNode<T>, PromiseNodeAllocator> is used for all PromiseNode
 * ownership. sizeof is unchanged (PromiseNodeAllocator is empty → EBO).
 *
 * The arena itself is owned by Promise<T> (m_arena member) and freed
 * when the Promise is destroyed.
 *
 * C++11-compatible. Header-only. Internal — not part of public API.
 */

#ifndef XPP_PROMISE_ALLOC_H
#define XPP_PROMISE_ALLOC_H

#include <cstddef>
#include <utility>

#include <xpp/allocator.h>
#include <xpp/arena.h>

namespace xpp {
namespace _ {

/* ── PromiseArena: the arena size used for PromiseNode chains ─────── */
using PromiseArena = Arena<256>;

/* ── PromiseNodeAllocator: custom Allocator with header-based routing ─ */
// All PromiseNode allocations have a fixed-size header (k_header) before
// the node data. The header stores the arena pointer (null = heap).
// deallocate reads the header to decide: arena → skip, heap → free.

static const size_t k_promise_header = alignof(std::max_align_t); // 16 on most platforms

class PromiseNodeAllocator {
public:
  Result<Span<uint8_t>, AllocError> allocate(Layout layout) const {
    // Never called through Own — nodes are created via allocate_promise().
    return GlobalAllocator{}.allocate(layout);
  }

  void deallocate(void *ptr, Layout layout) const {
    if (!ptr) return;
    void *raw   = static_cast<char *>(ptr) - k_promise_header;
    void *arena = *static_cast<void **>(raw);
    if (!arena) {
      // Heap-allocated: free the whole block (header + node).
      GlobalAllocator{}.deallocate(raw, Layout{layout.size + k_promise_header, layout.align});
    }
    // Arena-allocated: skip. Arena is freed when Promise is destroyed.
  }
};

/* ── allocate_promise: arena-aware node construction ─────────────────── */
// Always allocates with an 8-byte header. If arena is non-null and
// has room: bump-allocate in arena, header = arena pointer. Otherwise:
// heap new, header = nullptr.
//
// All PromiseNode creation must go through this function (or the
// convenience wrappers below) so deallocate can uniformly read the
// header.
template <class T, class... Args> T *allocate_promise(PromiseArena *arena, Args &&...args) {
  const size_t total = k_promise_header + sizeof(T);
  void        *mem;

  if (arena) {
    mem = arena->allocate(total, k_promise_header);
    if (mem) {
      *static_cast<void **>(mem) = arena;
    } else {
      // Arena full — heap fallback.
      mem                        = ::operator new(total);
      *static_cast<void **>(mem) = nullptr;
    }
  } else {
    mem                        = ::operator new(total);
    *static_cast<void **>(mem) = nullptr;
  }

  return ::new (static_cast<char *>(mem) + k_promise_header) T(std::forward<Args>(args)...);
}

} // namespace _
} // namespace xpp

#endif // XPP_PROMISE_ALLOC_H
