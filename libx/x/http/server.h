/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.h - Asynchronous HTTP/1.1 + HTTP/2 server powered by xEventLoop
 *
 * Integrates llhttp (H1) / nghttp2 (H2) with xEventLoop to provide a
 * single-threaded, non-blocking HTTP server. All callbacks are dispatched
 * on the event loop thread.
 */

#ifndef XHTTP_SERVER_H
#define XHTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>
#include <x/http/client.h> /* xHttpCtx, xHttpInitFunc, xHttpDataFunc, xHttpDoneFunc */
#include <x/net/tls.h>

/* ── Types ─────────────────────────────────────────────────────────────── */

/**
 * @brief Opaque handle to an HTTP server bound to an xEventLoop.
 */
XDEF_HANDLE(xHttpServer);

/**
 * @brief Opaque handle to a built-in HTTP multiplexer (router).
 */
XDEF_HANDLE(xHttpMux);

/**
 * @brief Route information returned by the resolver.
 *
 * Returned by @ref xHttpResolveFunc after the request headers are parsed.
 *
 * Callbacks:
 *   - @p on_request: called when request headers are parsed. Handler sets
 *     status/headers on @p ctx and returns 0 (OK) or non-0 (abort).
 *   - @p on_read: called when the server needs response body data. Mirror of
 *     client-side xHttpReadFunc. Return >0 bytes, 0 for EOF, <0 to pause.
 *   - @p on_done: called when the response is fully sent.
 *
 * All callbacks receive @p arg as the user-provided context.
 */
XDEF_STRUCT(xHttpRouteInfo) {
  xHttpInitFunc       on_request; /**< Called once after headers (may be NULL) */
  xHttpReadFunc on_read;    /**< Pull response body (may be NULL)         */
  xHttpDoneFunc       on_done;    /**< Called when response fully sent          */
  void               *arg;        /**< User argument forwarded to callbacks      */
};

/**
 * @brief Resolver callback: maps a request to a route.
 *
 * Called after the request headers are fully parsed. The @p ctx has
 * @p method, @p url, and @p headers populated. The resolver may also
 * set route parameters on @p ctx via @ref xHttpCtxParam (internally
 * stored on the stream).
 *
 * @param router  Opaque router context from @ref xHttpServerConf.
 * @param ctx     Request context with headers populated.
 * @return        Pointer to a @ref xHttpRouteInfo (must remain valid
 *                for the duration of the request), or NULL if no route
 *                matches (the server will send 404).
 */
typedef const xHttpRouteInfo *(*xHttpResolveFunc)(void *router, xHttpCtx *ctx);

/** @brief Called by xHttpServerDestroy to notify the caller that the server is gone. */
typedef void (*xHttpServerShutdownFunc)(void *arg);

/**
 * @brief Configuration for creating an HTTP server.
 *
 * Zero-initialize for defaults (no resolver → all requests get 404,
 * default idle timeout and header size).
 */
XDEF_STRUCT(xHttpServerConf) {
  xHttpResolveFunc        resolve;       /**< Resolver callback (may be NULL)    */
  void                   *router;        /**< Opaque router passed to @p resolve */
  int                      idle_timeout_ms; /**< 0 = default (60000 ms)          */
  size_t                   max_header_size; /**< 0 = default (8192 bytes)        */
  xHttpServerShutdownFunc  on_shutdown;  /**< Called when server is destroyed    */
  void                   *shutdown_arg;  /**< Opaque arg passed to on_shutdown   */
};

/**
 * @brief Configuration for registering a route with @ref xHttpMux.
 */
XDEF_STRUCT(xHttpRouteConf) {
  const char         *pattern;    /**< "METHOD /path" or "/path" (any method)   */
  xHttpInitFunc       on_request; /**< Called after headers (may be NULL)       */
  xHttpReadFunc on_read;    /**< Pull response body (may be NULL)         */
  xHttpDoneFunc       on_done;    /**< Called at request completion (may be NULL) */
  void               *arg;        /**< User argument forwarded to callbacks      */
};

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create an HTTP server bound to the current event loop.
 *
 * @param conf  Server configuration, or NULL for defaults.
 * @return      A new server handle, or NULL on failure.
 */
XCAPI(xHttpServer) xHttpServerCreate(const xHttpServerConf *conf);

/**
 * @brief Start listening for HTTP connections.
 */
XCAPI(xErrno) xHttpServerListen(xHttpServer server, const char *host, uint16_t port);

/**
 * @brief Return the actual listening port (use after xHttpServerListen).
 *
 * When @p port in xHttpServerListen is 0, the kernel assigns a free port.
 * This function returns the assigned port (or 0 if not yet listening).
 */
XCAPI(uint16_t) xHttpServerPort(xHttpServer server);

/**
 * @brief Destroy an HTTP server and release all resources. Safe to call NULL.
 */
XCAPI(void) xHttpServerDestroy(xHttpServer server);

/* ── Configuration ─────────────────────────────────────────────────────── */

/**
 * @brief Set the maximum allowed size for request headers.
 * Must be called before xHttpServerListen().
 */
XCAPI(xErrno) xHttpServerSetMaxHeaderSize(xHttpServer server, size_t max_size);

/* ── Mux (built-in router) ─────────────────────────────────────────────── */

/**
 * @brief Create a new HTTP multiplexer.
 */
XCAPI(xHttpMux) xHttpMuxCreate(void);

/**
 * @brief Destroy a multiplexer and free all registered routes.
 */
XCAPI(void) xHttpMuxDestroy(xHttpMux mux);

/**
 * @brief Register a route with the multiplexer.
 *
 * @p pattern follows the Go http.HandleFunc convention:
 *   - "GET /users/:id" — matches only GET to /users/:id
 *   - "/users/:id"     — matches all methods to /users/:id
 *
 * Routes are matched in registration order (first match wins).
 */
XCAPI(xErrno) xHttpMuxHandle(xHttpMux mux, const xHttpRouteConf *conf);

/**
 * @brief Resolver function compatible with @ref xHttpServerConf.resolve.
 *
 * Pass this as @c resolve and the mux as @c router in @ref xHttpServerConf.
 */
XCAPI(const xHttpRouteInfo *) xHttpMuxResolve(void *router, xHttpCtx *ctx);

/* ── Response (xHttpCtx write functions) ──────────────────────────────── */

/**
 * @brief Set the HTTP response status code (default 200).
 *
 * Must be called inside on_request, before the server starts the body pump.
 */
XCAPI(void) xHttpCtxSetStatus(xHttpCtx *ctx, int code);

/**
 * @brief Add a response header. Must be called inside on_request.
 */
XCAPI(xErrno) xHttpCtxSetHeader(xHttpCtx *ctx, const char *key, const char *value);

/**
 * @brief Look up a path parameter by name.
 *
 * For a route "/users/:id", xHttpCtxParam(ctx, "id", &len) returns
 * the matched segment value (NOT NUL-terminated).
 *
 * @return Pointer to the value, or NULL if not found.
 */
XCAPI(const char *) xHttpCtxParam(xHttpCtx *ctx, const char *name, size_t *len);

/* ── Request Body (pull model) ─────────────────────────────────────────── */

/**
 * @brief Read a chunk of request body via TryRead semantics.
 *
 * Body data is pre-buffered during HTTP parsing (llhttp on_body → stream buffer).
 * Handler calls this from on_request after the full body has arrived.
 *
 * @param buf  Output buffer.
 * @param cap  Output buffer capacity.
 * @return     >0 bytes, 0 = EOF, <0 = error.
 */
XCAPI(ssize_t) xHttpCtxBodyRead(xHttpCtx *ctx, char *buf, size_t cap);

/**
 * @brief Total request body length (available after parsing completes).
 * @return Body size in bytes, 0 if no body was sent.
 */
XCAPI(size_t) xHttpCtxBodyLen(xHttpCtx *ctx);

/* ── TLS ───────────────────────────────────────────────────────────────── */

typedef xTlsConf xHttpTlsServerConf;

/**
 * @brief Start listening for HTTPS connections with TLS.
 *
 * ALPN negotiation selects HTTP/1.1 or HTTP/2.
 */
XCAPI(xErrno) xHttpServerListenTls(xHttpServer server, const char *host, uint16_t port,
                                   const xTlsConf *config);

#endif /* XHTTP_SERVER_H */
