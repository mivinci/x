/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event.h - Cross-platform event loop (edge-triggered)
 *
 * Provides a thin abstraction over OS-specific I/O multiplexing:
 *
 *   kqueue   (macOS / BSD)   — selected when X_HAS_KQUEUE is defined
 *   epoll    (Linux)         — selected when X_HAS_EPOLL  is defined
 *   poll     (POSIX fallback)— used when neither of the above is available
 *
 * All backends operate in edge-triggered mode. Callers must drain the fd
 * completely on each notification or re-arm explicitly.
 */

#ifndef XBASE_EVENT_H
#define XBASE_EVENT_H

#include <stdint.h>
#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/task.h>
#include <x/base/time.h>

/**
 * @brief Bitmask of I/O events.
 */
XDEF_ENUM(xEventMask){
  xEvent_Read = 1 << 0, xEvent_Write = 1 << 1,
  xEvent_Timeout = 1 << 2, /* Used by higher-level modules (e.g., xSocket) */
  xEvent_LevelTriggered = 1 << 3, /* Registration flag: use level-triggered
                                     instead of edge-triggered.  Pass this in
                                     the mask to xEventAdd() to get repeated
                                     notifications while the fd remains ready.
                                     Has no effect in the callback mask. */
};

/**
 * @brief Callback invoked when an fd becomes ready.
 * @param fd    The file descriptor that triggered.
 * @param mask  Bitmask of ready events (xEvent_Read / xEvent_Write).
 * @param arg   User-provided argument.
 */
typedef void (*xEventFunc)(int fd, xEventMask mask, void *arg);

/**
 * @brief Opaque handle to an event loop.
 */
XDEF_HANDLE(xEventLoop);

/**
 * @brief Opaque handle to a registered event source.
 *
 * Valid until the source is removed via xEventDel().
 */
XDEF_HANDLE(xEventSource);

/**
 * @brief Opaque handle to a builtin event timer.
 *
 * Returned by xTimerStart.
 * Valid until the timer fires or is stopped.
 */
XDEF_HANDLE(xTimer);

/**
 * @brief Opaque handle to a submitted offload work item.
 *
 * Returned by xWorkSubmit() when a non-NULL @p out parameter is
 * provided. Can be passed to xWorkCancel() to attempt
 * cancellation.
 */
XDEF_HANDLE(xWork);

/**
 * @brief Callback invoked when a builtin event timer fires.
 * @param arg User-provided argument.
 */
typedef void (*xTimerFunc)(void *arg);

/**
 * @brief Callback invoked when a watched signal is delivered.
 * @param signo The signal number that was caught.
 * @param arg   User-provided argument.
 */
typedef void (*xSignalFunc)(int signo, void *arg);

/**
 * @brief Configuration for creating an event loop.
 *
 * Zero-initialize for defaults: no task group, no thread name.
 */
XDEF_STRUCT(xEventLoopConf) {
  xTaskGroup   group; /**< Default task group for offload, or NULL            */
  const char  *name;  /**< Thread name (max 15 chars, truncated), NULL = no-op */
};

/**
 * @brief Create an event loop with configuration.
 *
 * @param conf  Configuration, or NULL for defaults.
 * @return      A new event loop, or NULL on failure.
 */
XCAPI(xEventLoop) xEventLoopCreateWithConf(const xEventLoopConf *conf);

/**
 * @brief Create an event loop (convenience wrapper).
 *
 * Equivalent to @c xEventLoopCreateWithConf(NULL).
 *
 * @return A new event loop, or NULL on failure.
 */
XCAPI(xEventLoop) xEventLoopCreate(void);

/**
 * @brief Create an event loop with a default task group (convenience wrapper).
 *
 * Equivalent to @c xEventLoopCreateWithConf(&(xEventLoopConf){.group = group}).
 *
 * When xWorkSubmit() is called with a NULL group, the loop will
 * use @p group instead of falling back to xTaskGroupGlobal().
 *
 * @param group  Default task group, or NULL (same as xEventLoopCreate).
 * @return A new event loop, or NULL on failure.
 */
XCAPI(xEventLoop) xEventLoopCreateWithGroup(xTaskGroup group);

/**
 * @brief Destroy an event loop.
 *
 * All registered sources are implicitly removed. The caller is still
 * responsible for closing the underlying file descriptors.
 *
 * @param loop The event loop to destroy.
 */
XCAPI(void) xEventLoopDestroy(xEventLoop loop);

/**
 * @brief Register a file descriptor with the event loop.
 *
 * @param loop  The event loop.
 * @param fd    File descriptor to monitor.
 * @param mask  Events to watch for (xEvent_Read, xEvent_Write, or both).
 * @param fn    Callback invoked when the fd is ready (must not be NULL).
 * @param arg   Argument forwarded to @p fn.
 * @return      An event source handle, or NULL on failure.
 */
XCAPI(xEventSource) xEventAdd(int fd, xEventMask mask, xEventFunc fn, void *arg);

/**
 * @brief Modify the watched events for an existing source.
 *
 * @param loop  The event loop.
 * @param src   Source handle returned by xEventAdd().
 * @param mask  New event mask.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xEventMod(xEventSource src, xEventMask mask);

/**
 * @brief Remove a registered event source.
 *
 * After this call the source handle is invalid. The underlying fd is NOT
 * closed.
 *
 * @param loop  The event loop.
 * @param src   Source handle to remove.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xEventDel(xEventSource src);

/**
 * @brief Wake up a blocked event loop from another thread.
 *
 * Safe to call from any thread or signal handler. Multiple wakes before
 * the next iteration are coalesced.
 *
 * @param loop The event loop.
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xEventLoopWake(xEventLoop loop);

/**
 * @brief Start (or restart) a timer on the current event loop.
 *
 * Must be called from the event loop thread.
 *
 * @param fn         Callback to invoke on expiry (must not be NULL).
 * @param arg        Argument forwarded to @p fn.
 * @param timeout_ms Delay in milliseconds before the first fire.
 * @param repeat_ms  Interval for subsequent fires (0 = one-shot).
 * @return           A timer handle, or NULL on failure.
 */
XCAPI(xTimer) xTimerStart(xTimerFunc fn, void *arg, uint64_t timeout_ms, uint64_t repeat_ms);

/**
 * @brief Stop a pending timer.
 *
 * Safe to call from any thread. If called from outside the event loop
 * thread, the stop is deferred via xEventLoopPost().
 *
 * @param timer Timer handle to stop.
 * @return      xErrno_Ok if stopped before firing, xErrno_Unknown otherwise.
 */
XCAPI(xErrno) xTimerStop(xTimer timer);

/**
 * @brief Callback invoked on the event loop thread when offloaded work
 *        completes.
 * @param arg     User-provided argument (same as passed to xEventLoopSubmit).
 * @param result  Return value of the work function.
 */
typedef void (*xWorkDoneFunc)(void *arg, void *result);

/**
 * @brief Submit work to a thread pool; run @p done_fn on the loop thread
 *        when finished.
 *
 * The @p work_fn is executed on a worker thread from @p group. Once it
 * returns, @p done_fn is queued to the event loop and will be dispatched
 * during the next iteration, serialised with I/O and timer callbacks.
 *
 * @param loop     The event loop (must not be NULL).
 * @param group    Task group (thread pool). NULL = use xTaskGroupGlobal().
 * @param work_fn  Function executed on a worker thread (must not be NULL).
 * @param done_fn  Completion callback on the loop thread, or NULL for
 *                 fire-and-forget.
 * @param arg      Argument forwarded to both @p work_fn and @p done_fn.
 * @return         An xWork handle, or NULL on failure.
 */
XCAPI(xWork) xWorkSubmit(xTaskGroup group, xTaskFunc work_fn, xWorkDoneFunc done_fn, void *arg);

/**
 * @brief Cancel a previously submitted offload work item.
 *
 * Sets a cancelled flag that prevents @p done_fn from being invoked,
 * regardless of whether the task was queued or already executing.
 * After a successful cancel, the caller may safely release the
 * argument immediately — @p done_fn will never fire.
 *
 * Thread-safe: may be called from any thread.
 *
 * @param work  Work handle returned by xWorkSubmit(). NULL is safe.
 * @return      xErrno_Ok on success, xErrno_InvalidArg if work is NULL,
 *              xErrno_InvalidContext if work belongs to a different loop.
 */
XCAPI(xErrno) xWorkCancel(xWork work);

/**
 * @brief Callback invoked on the event loop thread by xEventLoopPost().
 * @param arg User-provided argument.
 */
typedef void (*xEventLoopPostFunc)(void *arg);

/**
 * @brief Post a callback to be executed on the event loop thread.
 *
 * The callback is queued and will be dispatched during the next
 * iteration, serialised with I/O, timer, and offload callbacks.
 * Unlike xWorkSubmit(), no thread pool is involved — the callback
 * runs directly on the loop thread.
 *
 * Thread-safe: may be called from any thread.
 *
 * @param loop  The event loop (must not be NULL).
 * @param fn    Callback to invoke on the loop thread (must not be NULL).
 * @param arg   Argument forwarded to @p fn.
 * @return      xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xEventLoopPost(xEventLoop loop, xEventLoopPostFunc fn, void *arg);

/**
 * @brief Run modes for xEventLoopRun().
 */
#define X_RUN_DEFAULT (-1) /**< Block until xEventLoopStop() or no active handles. */
#define X_RUN_ONCE    (-2) /**< Single iteration, block until at least one event. */
#define X_RUN_NOWAIT  (-3) /**< Single iteration, non-blocking poll. */

/**
 * @brief Run the event loop.
 *
 * Dispatches I/O, timers, and done-queue callbacks in a platform-agnostic
 * loop.  Returns 1 if the loop still has active handles, 0 if it exited
 * because all handles were closed (no more work to do).
 *
 * @param loop The event loop.
 * @param mode X_RUN_DEFAULT, X_RUN_ONCE, or X_RUN_NOWAIT.
 * @return    1 if alive, 0 if no more work.
 */
XCAPI(int) xEventLoopRun(xEventLoop loop, int mode);

/**
 * @brief Stop a running event loop.
 *
 * Sets an internal stop flag and wakes the loop so that xEventLoopRun()
 * returns promptly. Safe to call from any thread.
 */
XCAPI(void) xEventLoopStop(xEventLoop loop);

/**
 * @brief Watch for a POSIX signal on the event loop.
 *
 * Registers a callback to be invoked on the event loop thread when the
 * specified signal is delivered. The callback runs outside of signal
 * context, so it may safely call any function (including xEventLoopStop).
 *
 * - Register:  pass a non-NULL @p fn.
 * - Replace:   call again with the same @p signo and a new @p fn / @p arg.
 * - Cancel:    pass NULL for @p fn (and NULL for @p arg); the signal
 *              disposition is restored to SIG_DFL.
 *
 * @param loop  The event loop (must not be NULL).
 * @param signo Signal number to watch (e.g. SIGUSR1). SIGKILL and SIGSTOP
 *              are rejected.
 * @param fn    Callback, or NULL to cancel.
 * @param arg   Argument forwarded to @p fn.
 * @return      xErrno_Ok on success, xErrno_InvalidArg for bad arguments,
 *              xErrno_SysError if the underlying OS call fails.
 */
XCAPI(xErrno) xSignal(int signo, xSignalFunc fn, void *arg);

/**
 * @brief Register an event loop on the current thread.
 *
 * Associates @p loop with the calling thread via thread-local storage.
 * If a loop was already registered, it is pushed onto an internal stack;
 * the caller must call xEventLoopLeave() to restore it. This enables
 * safe nesting of event loops on the same thread.
 *
 * If the loop has a non-empty name, the calling thread's OS name is
 * set (via @c pthread_setname_np).  On xEventLoopLeave(), the name
 * is restored from the previous loop in the chain (or cleared if
 * there is none).
 *
 * xEventLoopRun() calls this internally, so code
 * that uses those functions rarely needs to call xEventLoopEnter()
 * directly. The primary use-case is integrating the libx event loop
 * into an external run loop (e.g. iOS CFRunLoop or Android Looper),
 * to pump the event loop manually in small bursts.
 *
 * @param loop  The event loop to register, or NULL to unregister.
 */
XCAPI(void) xEventLoopEnter(xEventLoop loop);

/**
 * @brief Unregister the current thread's event loop.
 *
 * Restores the loop that was active before the matching
 * xEventLoopEnter() call. Also restores the thread's OS name.
 *
 * This replaces the previous pattern of saving Enter's return value
 * and re-entering with it.
 */
XCAPI(void) xEventLoopLeave(void);

/**
 * @brief Return the event loop registered on the current thread.
 *
 * Returns NULL if no loop has been registered via xEventLoopEnter(),
 * xEventLoopRun().
 *
 * Use this inside callbacks (I/O, timer, offload completion) to
 * obtain the loop handle without storing it in user-provided context.
 *
 * @return The thread's event loop, or NULL if none is registered.
 */
XCAPI(xEventLoop) xEventLoopCurrent(void);

/**
 * @brief Get the global process-wide event loop.
 *
 * Returns the same singleton event loop for the entire process, created
 * on first call. Registered for automatic destruction via atexit().
 *
 * NOTE: xEventLoopCreateGlobalWithGroup is NOT thread-safe — it must be
 * called before any thread spawns. The global loop is designed for
 * single-threaded or carefully-coordinated usage (e.g. running
 * xEventLoopRun() on a dedicated thread).
 *
 * Use only when a shared event loop is appropriate. For most cases,
 * xEventLoopCreate() in the caller's thread is preferred.
 *
 * @return The global event loop, or NULL on failure.
 */
XCAPI(xEventLoop) xEventLoopGlobal(void);

/**
 * @brief Return the backend polling fd (kqueue/epoll fd) for embedding.
 *
 * Returns -1 for poll/WSAPoll backends which have no persistent fd.
 *
 * @param loop The event loop.
 * @return     Backend fd, or -1.
 */
XCAPI(int) xEventLoopFd(xEventLoop loop);

/**
 * @brief Return the timeout for the next poll, in milliseconds.
 *
 * Returns the time until the next timer fires, or -1 if no timers
 * are pending (poll can block indefinitely in X_RUN_DEFAULT).
 *
 * @param loop The event loop.
 * @return     Milliseconds until next timer, or -1.
 */
XCAPI(int) xEventLoopNextTimeout(xEventLoop loop);

#endif /* XBASE_EVENT_H */
