/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * relay.h - 1:N fan-out pub/sub with event-loop-aware dispatch
 *
 * xRelay lets modules communicate without knowing about each other.
 * One module publishes a message via xRelayEmit(); every module that
 * called xRelayOn() on the same relay handle receives it.
 *
 * The interesting part: dispatch is event-loop-aware.
 *
 *   - If the publisher is on the same event loop as a subscriber,
 *     the callback fires synchronously (zero-copy, zero-allocation).
 *   - If a subscriber runs on a different event loop, the relay
 *     uses xEventLoopPost() to enqueue the callback onto that loop,
 *     copying the data payload once.
 *
 * Named topics are implemented OUTSIDE the relay — just keep your
 * relay handles as global or module-scoped variables:
 *
 *   // temperature.h
 *   extern xRelay *g_temperature_relay;   // declared, not defined here
 *
 *   // main.c
 *   xRelay *g_temperature_relay = xRelayCreate();
 *   xRelay *g_pressure_relay    = xRelayCreate();
 *
 *   // sensor_reader.c (runs on its own event loop)
 *   static void on_temp(void *data, void *arg) {
 *       float *t = (float *)data;
 *       display_update(*t, arg);
 *   }
 *   void sensor_init(void) {
 *       xRelayOn(g_temperature_relay, on_temp, display_ctx);
 *   }
 *
 *   // thermometer.c (runs on a different event loop)
 *   void reading_ready(float celsius) {
 *       xRelayEmit(g_temperature_relay, &celsius, sizeof(celsius));
 *   }
 *
 * Thread safety: all APIs are internally synchronised with a mutex.
 * Emit holds the lock only long enough to snapshot the subscriber
 * list; callback execution happens outside the critical section.  The
 * snapshot copies subscriber metadata {loop, fn, arg} by value, so
 * concurrent xRelayOff (cross-thread) and in-callback xRelayOff
 * (same-loop) are safe — callbacks never dereference freed memory.
 */

#ifndef XBASE_RELAY_H
#define XBASE_RELAY_H

#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief Callback invoked on xRelayEmit.
 *
 * @param data  Opaque pointer to the published payload.  For same-loop
 *              subscribers this points directly into the publisher's
 *              stack frame and is valid for the duration of the call
 *              only.  For cross-loop subscribers this points to a
 *              heap-allocated copy that the relay frees after the
 *              callback returns.
 * @param arg   The opaque pointer registered in xRelayOn().
 */
typedef void (*xRelayFunc)(void *data, void *arg);

/**
 * @brief Opaque relay handle — create with xRelayCreate(), destroy
 *        with xRelayDestroy().
 */
typedef struct xRelay_ xRelay;

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a new relay with zero subscribers.
 *
 * @return Heap-allocated relay handle, or NULL on OOM.
 *         Pair with xRelayDestroy().
 */
XCAPI(xRelay *) xRelayCreate(void);

/**
 * @brief Subscribe @p fn with @p arg.
 *
 * The current event loop (xEventLoopCurrent()) is recorded at
 * subscription time and later used by xRelayEmit() to decide
 * whether to call @p fn synchronously or enqueue it via
 * xEventLoopPost().
 *
 * Called from a thread that has no event loop?  That's fine —
 * the recorded loop will be NULL and all emits to this subscriber
 * will be dispatched via xEventLoopPost().
 *
 * Multiple subscriptions with the same {fn, arg} pair are allowed;
 * each triggers independently on every emit.
 *
 * @param r    Relay handle.
 * @param fn   Callback function.
 * @param arg  Opaque user pointer forwarded to @p fn.
 * @return     xErrno_Ok on success, xErrno_NoMemory on OOM.
 */
XCAPI(xErrno) xRelayOn(xRelay *r, xRelayFunc fn, void *arg);

/**
 * @brief Remove the first subscriber matching {fn, arg}.
 *
 * No-op if no matching subscriber exists.
 *
 * @param r    Relay handle.
 * @param fn   Callback function to remove.
 * @param arg  Opaque user pointer to match.
 */
XCAPI(void) xRelayOff(xRelay *r, xRelayFunc fn, void *arg);

/**
 * @brief Emit @p data to every subscriber.
 *
 * Dispatch strategy (per subscriber):
 *
 *   Same loop or NULL loop → callback fires synchronously on the
 *     publisher's stack.  @p data points directly to the caller's
 *     buffer — zero-copy, zero-allocation.  The caller must keep
 *     @p data alive until Emit returns.
 *
 *   Different loop → the relay heap-copies @p size bytes, enqueues
 *     a dispatch callback onto the subscriber's event loop via
 *     xEventLoopPost(), and returns immediately.  The subscriber's
 *     callback fires on a future iteration of that loop.  The relay
 *     frees the copy after the callback returns.
 *
 * @param r     Relay handle.
 * @param data  Pointer to the payload.  Must be valid until the
 *              call returns (same-loop path will read it).
 * @param size  Size of the payload in bytes.  Pass 0 for empty
 *              notification (subscriber receives a NULL data pointer).
 */
XCAPI(void) xRelayEmit(xRelay *r, const void *data, size_t size);

/**
 * @brief Destroy the relay, freeing all subscribers and internal
 *        resources.
 *
 * After this call the relay handle is invalid.  Any pending
 * cross-loop dispatches that have already been enqueued are still
 * delivered (they own their own copies of the data and subscriber
 * metadata).
 *
 * @param r Relay handle to destroy.
 */
XCAPI(void) xRelayDestroy(xRelay *r);

#endif /* XBASE_RELAY_H */
