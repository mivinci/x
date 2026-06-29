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
 * The library calls @p on_request (if non-NULL) right after resolution,
 * streams the body via @p on_data (if non-NULL), and finally invokes
 * @p on_done when the request is fully received.
 *
 * All callbacks receive @p arg as the user-provided context.
 */
XDEF_STRUCT(xHttpRouteInfo) {
  xHttpInitFunc on_request; /**< Called once after headers (may be NULL) */
  xHttpDataFunc on_data;    /**< Per body chunk callback (may be NULL)    */
  xHttpDoneFunc on_done;    /**< Called when request is complete           */
  void         *arg;        /**< User argument forwarded to callbacks      */
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

/**
 * @brief Configuration for creating an HTTP server.
 *
 * Zero-initialize for defaults (no resolver → all requests get 404,
 * default idle timeout and header size).
 */
XDEF_STRUCT(xHttpServerConf) {
  xHttpResolveFunc resolve;         /**< Resolver callback (may be NULL)   */
  void            *router;          /**< Opaque router passed to @p resolve */
  int              idle_timeout_ms; /**< 0 = default (60000 ms)             */
  size_t           max_header_size; /**< 0 = default (8192 bytes)           */
};

/**
 * @brief Configuration for registering a route with @ref xHttpMux.
 */
XDEF_STRUCT(xHttpRouteConf) {
  const char  *pattern;    /**< "METHOD /path" or "/path" (any method)   */
  xHttpInitFunc on_request; /**< Called after headers (may be NULL)       */
  xHttpDataFunc on_data;    /**< Per body chunk callback (may be NULL)    */
  xHttpDoneFunc on_done;    /**< Called at request completion (may be NULL) */
  void         *arg;        /**< User argument forwarded to callbacks      */
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
 */
XCAPI(void) xHttpCtxSetStatus(xHttpCtx *ctx, int code);

/**
 * @brief Add a response header. Must be called before xHttpCtxSend().
 */
XCAPI(xErrno) xHttpCtxSetHeader(xHttpCtx *ctx, const char *key, const char *value);

/**
 * @brief Send a complete HTTP response (status + headers + body).
 *
 * Mutually exclusive with xHttpCtxWrite(). May only be called once.
 */
XCAPI(xErrno) xHttpCtxSend(xHttpCtx *ctx, const char *body, size_t body_len);

/**
 * @brief Write streaming response data (no Content-Length).
 *
 * On the first call, flushes status line + headers. Subsequent calls
 * append data. Mutually exclusive with xHttpCtxSend(). The stream is
 * auto-ended when xHttpCtxEndStream is called.
 */
XCAPI(xErrno) xHttpCtxWrite(xHttpCtx *ctx, const char *data, size_t len);

/**
 * @brief End a streaming response and finalize the connection.
 *
 * Must be called after xHttpCtxWrite() to signal that the response
 * is complete.  For xHttpCtxSend(), finalization is implicit.
 * If a handler returns without calling send/write/endstream, the
 * connection stays open until idle timeout or explicit close.
 */
XCAPI(xErrno) xHttpCtxEndStream(xHttpCtx *ctx);

/**
 * @brief Look up a path parameter by name.
 *
 * For a route "/users/:id", xHttpCtxParam(ctx, "id", &len) returns
 * the matched segment value (NOT NUL-terminated).
 *
 * @return Pointer to the value, or NULL if not found.
 */
XCAPI(const char *) xHttpCtxParam(xHttpCtx *ctx, const char *name, size_t *len);

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
