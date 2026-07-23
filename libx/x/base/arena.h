/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arena.h - Fixed-Capacity Bump Allocator
 *
 * xArena is a simple bump allocator: allocate a block of memory up front and
 * hand out variable-sized slices by bumping a pointer forward.  Memory is
 * never individually freed — it is reclaimed only by destroying the arena or
 * calling xArenaReset().  This makes it ideal for phase-scoped allocations
 * where every object shares the same lifetime (parse trees, request-scoped data,
 * temporary scratch buffers, ...).
 *
 * Key properties:
 *   - O(1) allocation (bump + alignment check).
 *   - O(1) ownership test via pointer-range comparison.
 *   - Fails gracefully (returns NULL) when full — no growth, no chunk chaining.
 *   - No destructor tracking — caller is responsible for any cleanup of
 *     arena-allocated objects before xArenaDestroy / xArenaReset.
 */

#ifndef XBASE_ARENA_H
#define XBASE_ARENA_H

#include <stddef.h>

#include <x/base/base.h>

/** @brief Default alignment when 0 is passed to xArenaAllocAligned. */
#define XARENA_DEFAULT_ALIGN ((size_t)16)

/** @brief Opaque handle for a fixed-capacity bump allocator. */
XDEF_HANDLE(xArena);

/* ─────────────────────── Lifecycle ─────────────────────── */

/**
 * @brief Create an arena with @p capacity bytes pre-allocated.
 * @ingroup xArena
 *
 * Allocates a single buffer of @p capacity bytes.  The arena will never
 * grow beyond this limit.  Returns NULL on OOM.
 */
XCAPI(xArena *)
xArenaCreate(size_t capacity);

/**
 * @brief Destroy the arena and release the underlying buffer.
 * @ingroup xArena
 *
 * All pointers previously returned by xArenaAlloc / xArenaAllocAligned
 * become invalid.  Passing NULL is a no-op.
 */
XCAPI(void) xArenaDestroy(xArena *a);

/* ─────────────────────── Allocation ─────────────────────── */

/**
 * @brief Bump-allocate @p size bytes with default alignment.
 * @ingroup xArena
 *
 * Equivalent to xArenaAllocAligned(a, size, XARENA_DEFAULT_ALIGN).
 * Returns NULL if the arena does not have enough space.
 */
XCAPI(void *) xArenaAlloc(xArena *a, size_t size);

/**
 * @brief Bump-allocate @p size bytes with explicit alignment.
 * @ingroup xArena
 *
 * @param size  Bytes requested.  May be 0 (returns a valid, non-NULL pointer).
 * @param align Required alignment.  Must be a power of two.  0 selects
 *              XARENA_DEFAULT_ALIGN (16 bytes).
 * @return Pointer to aligned memory, or NULL if the arena is full.
 *
 * The returned memory is uninitialised.  Callers that need zeroed memory
 * must memset it explicitly.
 */
XCAPI(void *) xArenaAllocAligned(xArena *a, size_t size, size_t align);

/* ─────────────────────── Queries ─────────────────────── */

/**
 * @brief Total capacity in bytes (the value passed to xArenaCreate).
 * @ingroup xArena
 */
XCAPI(size_t) xArenaCapacity(const xArena *a);

/**
 * @brief Bytes handed out so far (bump position relative to start).
 * @ingroup xArena
 *
 * This includes alignment padding.  Useful for reporting how much of the
 * arena has been consumed.
 */
XCAPI(size_t) xArenaUsed(const xArena *a);

/**
 * @brief Bytes still available (without accounting for future alignment
 *        padding that may be required by subsequent allocations).
 * @ingroup xArena
 */
XCAPI(size_t) xArenaRemaining(const xArena *a);

/**
 * @brief O(1) check: was @p p allocated from this arena?
 * @ingroup xArena
 *
 * Returns non-zero (true) if @p p points within the arena's backing buffer.
 * Returns 0 (false) for NULL, stack pointers, heap pointers, and pointers
 * from a different arena.
 */
XCAPI(int) xArenaOwns(const xArena *a, const void *p);

/* ─────────────────────── Bulk Reclaim ─────────────────────── */

/**
 * @brief Reset the bump pointer to the start of the buffer.
 * @ingroup xArena
 *
 * The buffer itself is NOT freed — it stays allocated and can be reused
 * immediately.  All pointers previously returned by xArenaAlloc become
 * dangling after this call.
 *
 * Useful for request-scoped arenas: reset between requests instead of
 * destroying and recreating the arena.
 */
XCAPI(void) xArenaReset(xArena *a);

#endif /* XBASE_ARENA_H */
