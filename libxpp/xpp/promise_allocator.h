/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_allocator.h — Internal: arena-aware allocation for PromiseNode.
 *
 * KJ-style: arena ownership lives in the node (via header), not in
 * Promise<T>. No thread-local, no guard. All PromiseNode allocations
 * go through allocate_promise() or append_promise(), which add a
 * fixed-size header storing the arena pointer + owns_arena flag:
 *
 *   Layout: [arena_ptr (8B)][owns_arena (1B)][pad (7B)][node data]
 *                                                             ^
 *                                                             returned pointer
 *
 *   arena_ptr == null     → heap-allocated, deallocate frees
 *   arena_ptr != null, owns_arena == false → arena-allocated, skip
 *   arena_ptr != null, owns_arena == true  → arena-allocated + tail,
 *           deallocate skips node, deletes arena (bulk-frees chain)
 *
 * Promise<T> has no m_arena — sizeof(Promise<T>) == sizeof(ptr) == 8.
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

/* ── Header layout ────────────────────────────────────────────────── */
struct PromiseNodeHeader {
  PromiseArena *arena;      // null = heap, non-null = in this arena
  bool          owns_arena; // true = I am the tail, I free the arena
};

// Header size: must fit PromiseNodeHeader AND be a multiple of
// alignof(max_align_t) so the object after the header is properly
// aligned. On macOS arm64: max(16, 8) = 16. On Linux x86_64: max(16, 16) = 16.
static const size_t kPromiseNodeHeaderSize = sizeof(PromiseNodeHeader) > alignof(std::max_align_t)
                                               ? sizeof(PromiseNodeHeader)
                                               : alignof(std::max_align_t);

/* ── PromiseNodeAllocator: custom Allocator with header-based routing ─ */
class PromiseNodeAllocator {
public:
  Result<Span<uint8_t>, AllocError> allocate(Layout layout) const {
    // Never called through Own — nodes are created via allocate_promise().
    return GlobalAllocator{}.allocate(layout);
  }

  void deallocate(void *ptr, Layout layout) const {
    if (!ptr) return;
    auto *hdr =
      reinterpret_cast<PromiseNodeHeader *>(static_cast<char *>(ptr) - kPromiseNodeHeaderSize);
    // Save header values BEFORE any delete — hdr lives inside the arena
    // buffer, so delete arena frees hdr's memory. Reading hdr after
    // delete would be use-after-free.
    PromiseArena *arena = hdr->arena;
    bool          owns  = hdr->owns_arena;

    if (owns) {
      delete arena; // frees arena buffer (includes this node + all chain nodes)
    }
    if (!arena) {
      // Heap-allocated: free the whole block (header + node).
      void *raw = static_cast<char *>(ptr) - kPromiseNodeHeaderSize;
      GlobalAllocator{}.deallocate(raw, Layout{layout.size + kPromiseNodeHeaderSize, layout.align});
    }
    // Arena-allocated (non-owner): skip. Arena freed by the tail node.
  }
};

/* ── allocate_promise: create a node with NO arena (heap-only) ─────── */
// Used for standalone nodes: resolve(), yield(), adapt(), all(), race(),
// coroutine start. These are not part of a .then() chain.
template <class T, class... Args> T *allocate_promise(PromiseArena * /*arena*/, Args &&...args) {
  // arena is always nullptr for this overload — kept for API uniformity.
  void *mem       = ::operator new(kPromiseNodeHeaderSize + sizeof(T));
  auto *hdr       = static_cast<PromiseNodeHeader *>(mem);
  hdr->arena      = nullptr;
  hdr->owns_arena = false;
  return ::new (static_cast<char *>(mem) + kPromiseNodeHeaderSize) T(std::forward<Args>(args)...);
}

/* ── append_promise: create a node in the predecessor's arena ──────── */
// KJ-style: reads the arena from the predecessor's header, bump-allocates
// the new node in the same arena (or heap if full/no arena), transfers
// arena ownership from predecessor to the new node.
//
// pred is the predecessor's raw node pointer (from OwnPromiseNode).
// Returns the new node pointer. The caller must update pred's header
// to release ownership (owns_arena = false).
template <class T, class... Args> T *append_promise(void *pred_raw, Args &&...args) {
  // Read predecessor's header to find the arena.
  auto *pred_hdr =
    reinterpret_cast<PromiseNodeHeader *>(static_cast<char *>(pred_raw) - kPromiseNodeHeaderSize);
  PromiseArena *arena = pred_hdr->arena;

  void *mem;
  if (arena) {
    mem = arena->allocate(kPromiseNodeHeaderSize + sizeof(T), kPromiseNodeHeaderSize);
    if (mem) {
      // Bump-allocated in arena. Transfer ownership.
      pred_hdr->owns_arena = false; // predecessor releases
      auto *hdr            = static_cast<PromiseNodeHeader *>(mem);
      hdr->arena           = arena;
      hdr->owns_arena      = true; // new node is now the tail
    } else {
      // Arena full — heap fallback. Predecessor keeps ownership.
      mem             = ::operator new(kPromiseNodeHeaderSize + sizeof(T));
      auto *hdr       = static_cast<PromiseNodeHeader *>(mem);
      hdr->arena      = nullptr;
      hdr->owns_arena = false;
    }
  } else {
    // No arena — create one. New node owns it.
    arena           = new PromiseArena();
    mem             = arena->allocate(kPromiseNodeHeaderSize + sizeof(T), kPromiseNodeHeaderSize);
    auto *hdr       = static_cast<PromiseNodeHeader *>(mem);
    hdr->arena      = arena;
    hdr->owns_arena = true;

    // If predecessor was heap-allocated, it has no arena to release.
    // If predecessor was arena-allocated (non-owner), its arena pointer
    // is non-null but owns_arena is false — nothing to do.
  }

  return ::new (static_cast<char *>(mem) + kPromiseNodeHeaderSize) T(std::forward<Args>(args)...);
}

} // namespace _
} // namespace xpp

#endif // XPP_PROMISE_ALLOC_H
