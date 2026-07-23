/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.h - Asynchronous HTTP client powered by libcurl + xEventLoop
 *
 * Integrates libcurl's multi-socket API with xEventLoop to provide
 * a single-threaded, non-blocking HTTP client. All callbacks are
 * dispatched on the event loop thread.
 */

#ifndef XHTTP_CLIENT_H
#define XHTTP_CLIENT_H

#include <stddef.h>

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>
#include <x/net/tls.h>

/**
 * @brief Opaque handle to an HTTP client bound to an xEventLoop.
 */
XDEF_HANDLE(xHttpClient);

/**
 * @brief Per-request context shared between the client and the user callbacks.
 *
 * Built by the library and passed to @ref xHttpInitFunc (after response headers
 * are received) and @ref xHttpDoneFunc (after the transfer completes). All
 * pointers are valid only for the duration of the callback; the caller must
 * NOT free any of the fields.
 *
 * Note: there is no @c body field — response body data is delivered via
 * @ref xHttpDataFunc, or discarded if @ref xHttpRequestConf.on_data is NULL.
 */
XDEF_STRUCT(xHttpCtx) {
  const char *method;      /**< Request method (client: NULL)                */
  const char *url;         /**< Request URL (client: NULL)                   */
  long        status_code; /**< HTTP status code (e.g. 200), 0 on failure    */
  int         curl_code;   /**< CURLcode (0 = CURLE_OK on success)           */
  const char *curl_error;  /**< Human-readable curl error, or NULL           */
  const char *headers;     /**< Raw response headers (NUL-terminated)        */
  size_t      headers_len; /**< Length of @p headers in bytes                */
  void       *internal_;   /**< Internal use (server-side; client: NULL)     */
};

/**
 * @brief Callback invoked once after all response headers are received,
 *        before the first body chunk (if any).
 *
 * @param ctx  Request context with @p status_code and @p headers populated.
 * @param arg  User-provided argument.
 * @return     0 to continue the transfer, non-zero to abort.
 */
typedef int (*xHttpInitFunc)(xHttpCtx *ctx, void *arg);

/**
 * @brief Callback invoked when an HTTP request completes.
 * @param ctx  Request context (valid only during the callback).
 * @param arg  User-provided argument.
 */
typedef void (*xHttpDoneFunc)(xHttpCtx *ctx, void *arg);

/**
 * @brief Callback invoked for each chunk of response body data.
 *
 * Called only when @ref xHttpRequestConf.on_data is set (streaming mode).
 * The @p data pointer is valid only during the callback — copy if needed.
 *
 * @param data  Body chunk (not NUL-terminated).
 * @param len   Length of @p data in bytes.
 * @param arg   User-provided argument.
 * @return      0 to continue, non-zero to abort the transfer.
 */
typedef int (*xHttpDataFunc)(const char *data, size_t len, void *arg);

/**
 * @brief Callback invoked by libcurl to pull request body data for upload.
 *
 * Called only when @ref xHttpRequestConf.on_read is set.  Fill @p buf with
 * up to @p bufsize bytes and return the number of bytes written.  Return 0
 * to signal end of body (EOF).
 *
 * @param buf      Destination buffer (owned by libcurl).
 * @param bufsize  Maximum bytes to write.
 * @param arg      User-provided argument.
 * @return         Bytes written, or 0 for EOF.
 */
typedef size_t (*xHttpReadFunc)(char *buf, size_t bufsize, void *arg);

/**
 * @brief HTTP method constants.
 */
XDEF_ENUM(xHttpMethod){
  xHttpMethod_GET = 0,    xHttpMethod_POST = 1,  xHttpMethod_PUT = 2,
  xHttpMethod_DELETE = 3, xHttpMethod_PATCH = 4, xHttpMethod_HEAD = 5,
};

/**
 * @brief HTTP version preference for requests.
 *
 * Controls which HTTP protocol version libcurl will use.
 * Zero-initialized structs default to xHttpVersion_Default.
 */
XDEF_ENUM(xHttpVersion){
  xHttpVersion_Default = 0, /**< Use client default (initially HTTP/1.1)    */
  xHttpVersion_H1      = 1, /**< Force HTTP/1.1                              */
  xHttpVersion_H2      = 2, /**< HTTP/2 with TLS (ALPN), fallback to H1      */
  xHttpVersion_H2TLS   = 3, /**< HTTP/2 over TLS only, no fallback           */
  xHttpVersion_H2C     = 4, /**< HTTP/2 cleartext (Prior Knowledge)           */
  xHttpVersion_H3      = 5, /**< HTTP/3 over QUIC (requires ngtcp2+nghttp3)  */
};

/**
 * @brief Configuration for a custom HTTP request.
 *
 * Used with xHttpClientDo() for full control over the request.
 * Zero-initialize for defaults (GET, no headers, no timeout).
 *
 * Request body is provided via @ref on_read; set @ref content_length to the
 * known body size (0 = chunked transfer-encoding). Response body is delivered
 * via @ref on_data; if @ref on_data is NULL, the body is discarded.
 */
XDEF_STRUCT(xHttpRequestConf) {
  const char  *url;            /**< Request URL (must not be NULL)             */
  xHttpMethod  method;         /**< HTTP method (default: GET)                 */
  size_t       content_length; /**< Request body size for on_read (0=chunked)  */
  const char **headers;        /**< NULL-terminated array of "Key: Value"      */
  long         timeout_ms;     /**< Per-request timeout in ms (0 = no limit).
                                    For regular HTTP: total transfer timeout.
                                    For SSE: connection-phase timeout only;
                                    stalled streams are detected via
                                    low-speed-time instead.                  */
  xHttpVersion http_version;   /**< HTTP version (0 = use client default)      */

  /* ── Streaming callbacks (all optional, NULL = not used) ── */

  xHttpInitFunc on_response; /**< Called once after headers (NULL = skip)     */
  xHttpDataFunc on_data;     /**< Per body chunk callback (NULL = discard)    */
  xHttpReadFunc on_read;     /**< Request body provider (NULL = no body)      */
  xHttpDoneFunc on_done;     /**< Completion callback (NULL = fire-forget)    */
};

/**
 * @brief Backward-compatible alias for xTlsConf (defined in xnet/tls.h).
 */
typedef xTlsConf xHttpTlsClientConf;

/**
 * @brief Configuration for creating an HTTP client.
 *
 * Zero-initialize for defaults (no TLS, HTTP/1.1).
 * Pass NULL to xHttpClientCreate() for the same defaults.
 */
XDEF_STRUCT(xHttpClientConf) {
  const xTlsConf *tls;          /**< TLS config, or NULL         */
  xHttpVersion    http_version; /**< Default HTTP version (0=H1) */
};

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create an HTTP client bound to an event loop.
 *
 * Initialises a curl multi handle and registers socket/timer callbacks
 * with the given event loop. If @p conf is not NULL, the client is
 * configured with the given TLS and HTTP version settings.
 *
 * @param loop  The event loop (must not be NULL).
 * @param conf  Client configuration, or NULL for defaults.
 * @return      A new client handle, or NULL on failure.
 */
XCAPI(xHttpClient) xHttpClientCreate(const xHttpClientConf *conf);

/**
 * @brief Destroy an HTTP client and release all resources.
 *
 * Any in-flight requests are cancelled; their completion callbacks are
 * invoked with an error status before resources are freed.
 *
 * @param client  The client to destroy.
 */
XCAPI(void) xHttpClientDestroy(xHttpClient client);

/* ── Convenience request helpers ───────────────────────────────────────── */

/**
 * @brief Submit an asynchronous HTTP GET request.
 *
 * Forces @ref xHttpRequestConf.method to GET, then delegates to
 * xHttpClientDo().  The URL and all callbacks come from @p conf.
 *
 * @param client  The HTTP client.
 * @param conf    Request configuration (must not be NULL, conf->url required).
 * @param arg     User argument forwarded to all callbacks.
 * @return        xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpClientGet(xHttpClient client, const xHttpRequestConf *conf, void *arg);

/**
 * @brief Submit an asynchronous HTTP POST request.
 *
 * Forces @ref xHttpRequestConf.method to POST, then delegates to
 * xHttpClientDo().  Request body comes from @p conf->on_read (with
 * @p conf->content_length providing the size, 0 for chunked).
 *
 * @param client  The HTTP client.
 * @param conf    Request configuration (must not be NULL, conf->url required).
 * @param arg     User argument forwarded to all callbacks.
 * @return        xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpClientPost(xHttpClient client, const xHttpRequestConf *conf, void *arg);

/* ── Generic request ───────────────────────────────────────────────────── */

/**
 * @brief Submit a fully-configured asynchronous HTTP request.
 *
 * All callbacks (on_response, on_data, on_read, on_done) are read from
 * @p conf.  Zero-initialized conf fields mean: no init callback, discarded
 * response body, no upload streaming, fire-and-forget (no completion).
 *
 * @param client  The HTTP client.
 * @param conf    Request configuration (must not be NULL, conf->url required).
 * @param arg     User argument forwarded to all callbacks.
 * @return        xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpClientDo(xHttpClient client, const xHttpRequestConf *conf, void *arg);

/* ── SSE (Server-Sent Events) ──────────────────────────────────────────── */

/**
 * @brief SSE event delivered to the callback.
 *
 * All strings are NUL-terminated and valid only during the callback.
 * The caller must copy if needed.
 */
XDEF_STRUCT(xSseEvent) {
  const char *event; /**< event type, "message" if omitted     */
  const char *data;  /**< event data (may be multiline)        */
  const char *id;    /**< last event ID, or NULL               */
  int         retry; /**< retry delay in ms, or -1 if omitted  */
};

/**
 * @brief Callback invoked for each SSE event.
 *
 * @param ev   The SSE event (valid only during the callback).
 * @param arg  User-provided argument.
 * @return     0 to continue, non-zero to close the connection.
 */
typedef int (*xSseEventFunc)(const xSseEvent *ev, void *arg);

/**
 * @brief Callback invoked when the SSE stream ends.
 *
 * @param curl_code  CURLcode (0 = clean close, non-zero = error).
 * @param arg        User-provided argument.
 */
typedef void (*xSseDoneFunc)(int curl_code, void *arg);

/**
 * @brief Subscribe to an SSE endpoint.
 *
 * Sets Accept: text/event-stream and parses the stream according to
 * the W3C SSE specification.
 *
 * @param client    The HTTP client.
 * @param url       SSE endpoint URL.
 * @param on_event  Callback for each event (must not be NULL).
 * @param on_done   Callback when stream ends (may be NULL).
 * @param arg       User argument forwarded to callbacks.
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xHttpClientGetSse(xHttpClient client, const char *url, xSseEventFunc on_event,
                                xSseDoneFunc on_done, void *arg);

/**
 * @brief Submit a fully-configured SSE request.
 *
 * Like xHttpClientGetSse(), but uses xHttpRequestConf for full control
 * over the HTTP method, headers, and body. This is useful for LLM APIs
 * that require POST with a JSON body and custom Authorization headers.
 *
 * The Accept: text/event-stream header is added automatically.
 *
 * @param client    The HTTP client.
 * @param config    Request configuration (must not be NULL, config->url
 *                  must not be NULL).
 * @param on_event  Callback for each event (must not be NULL).
 * @param on_done   Callback when stream ends (may be NULL).
 * @param arg       User argument forwarded to callbacks.
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xHttpClientDoSse(xHttpClient client, const xHttpRequestConf *config,
                               xSseEventFunc on_event, xSseDoneFunc on_done, void *arg);

#endif /* XHTTP_CLIENT_H */
