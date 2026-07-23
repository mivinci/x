/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json_sax.h - SAX (Streaming) JSON Parser
 *
 * xJsonSax provides a callback-driven, streaming JSON parser for
 * memory-constrained or incremental cases.  Instead of building a DOM
 * tree, it fires user-supplied callbacks as tokens are parsed.
 *
 * Two parsing modes are supported:
 *
 *   One-shot:
 *     xJsonSaxParse(json, len, &handler, ctx)
 *     Parse a complete JSON document in one call.  All callbacks fire
 *     synchronously.  String values passed to callbacks point into a
 *     temporary arena that is valid for the duration of the callback only;
 *     the caller must copy strings if they need to persist them.
 *
 *   Streaming (incremental feed):
 *     xJsonSaxCreate(&handler, ctx, max_depth) → xJsonSax *
 *     xJsonSaxFeed(sax, data, len)          → xJsonSaxResult
 *     xJsonSaxFinalize(sax)                     → xJsonSaxResult
 *     xJsonSaxDestroy(sax)
 *     Feed bytes as they arrive.  Callbacks fire when complete tokens are
 *     available.  Returns xJsonSaxResult_NeedMore when more data is
 *     required, xJsonSaxResult_Done when the document is complete, or
 *     xJsonSaxResult_Error on parse failure.
 *
 * The streaming API follows the same pattern as xWsFrameParse().
 */

#ifndef XJSON_SAX_H
#define XJSON_SAX_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief User-supplied callbacks for SAX parse events.
 * @ingroup xJson
 *
 * Each callback receives the user-supplied @p ctx pointer.  Return 0 to
 * continue parsing or non-zero to abort immediately; the non-zero value
 * is returned by xJsonSaxParse / xJsonSaxFeed / xJsonSaxFinalize.
 *
 * The @p s pointer passed to on_string and on_key is NUL-terminated and
 * valid only for the duration of the callback.  Copy it if needed.
 */
XDEF_STRUCT(xJsonSaxHandler) {
  int (*on_null)(void *ctx);
  int (*on_bool)(void *ctx, int v);
  int (*on_int)(void *ctx, int64_t v);
  int (*on_double)(void *ctx, double v);
  int (*on_string)(void *ctx, const char *s, size_t len);
  int (*on_key)(void *ctx, const char *s, size_t len);
  int (*on_array_begin)(void *ctx);
  int (*on_array_end)(void *ctx);
  int (*on_object_begin)(void *ctx);
  int (*on_object_end)(void *ctx);
};

/* ───────────────────── One-shot SAX ───────────────────── */

/**
 * @brief Parse a complete JSON document, firing callbacks synchronously.
 * @ingroup xJson
 *
 * All callbacks fire before the function returns.  String values are
 * arena-backed and valid for the duration of each callback only.
 *
 * @param json    NUL-terminated or length-delimited JSON string.
 * @param len     Length in bytes.
 * @param handler Callback table (must not be NULL).
 * @param ctx     Opaque pointer passed to every callback.
 * @return 0 on success, -1 on parse error, or the non-zero value
 *         returned by a callback that aborted parsing.
 */
XCAPI(int) xJsonSaxParse(const char *json, size_t len,
                          const xJsonSaxHandler *handler, void *ctx);

/* ───────────────────── Streaming SAX ───────────────────── */

/** @brief Opaque handle for an incremental SAX parser. */
XDEF_HANDLE(xJsonSax);

/**
 * @brief Result codes for xJsonSaxFeed() and xJsonSaxFinalize().
 * @ingroup xJson
 */
XDEF_ENUM(xJsonSaxResult) {
  /** More data is needed; call xJsonSaxFeed() again with additional bytes. */
  xJsonSaxResult_NeedMore = 1,
  /** The JSON document has been fully parsed.  No more data is expected. */
  xJsonSaxResult_Done = 0,
  /** A parse error was encountered.  The parser is unusable after this. */
  xJsonSaxResult_Error = -1,
};

/**
 * @brief Create a new streaming SAX parser.
 * @ingroup xJson
 *
 * The parser maintains internal state across xJsonSaxFeed() calls.
 *
 * @param handler   Callback table (must not be NULL).
 * @param ctx       Opaque pointer passed to every callback.
 * @param max_depth Maximum nesting depth (0 = default of 32).
 * @return New parser handle, or NULL on allocation failure.
 */
XCAPI(xJsonSax *) xJsonSaxCreate(const xJsonSaxHandler *handler, void *ctx, int max_depth);

/**
 * @brief Feed bytes into the streaming parser.
 * @ingroup xJson
 *
 * Call this repeatedly as data arrives.  Returns xJsonSaxResult_Done when
 * the complete document has been consumed, xJsonSaxResult_NeedMore when
 * the parser expects more data, or xJsonSaxResult_Error on a parse error.
 *
 * @param sax  Parser handle.
 * @param data Next chunk of JSON data.
 * @param len  Length of @p data in bytes.
 * @return One of xJsonSaxResult_NeedMore, xJsonSaxResult_Done, or
 *         xJsonSaxResult_Error.
 */
XCAPI(xJsonSaxResult) xJsonSaxFeed(xJsonSax *sax, const char *data, size_t len);

/**
 * @brief Signal end of input and finalise parsing.
 * @ingroup xJson
 *
 * Must be called after the last xJsonSaxFeed() to detect truncated
 * documents.  Returns xJsonSaxResult_Done if the document was complete,
 * or xJsonSaxResult_Error if the document was truncated.
 *
 * @param sax  Parser handle.
 * @return xJsonSaxResult_Done or xJsonSaxResult_Error.
 */
XCAPI(xJsonSaxResult) xJsonSaxFinalize(xJsonSax *sax);

/**
 * @brief Reset the parser for reuse with a new document.
 * @ingroup xJson
 *
 * Clears all internal state but retains the handler and context.
 *
 * @param sax  Parser handle.
 */
XCAPI(void) xJsonSaxReset(xJsonSax *sax);

/**
 * @brief Free a streaming SAX parser and all associated resources.
 * @ingroup xJson
 *
 * @param sax  Parser handle (NULL is a no-op).
 */
XCAPI(void) xJsonSaxDestroy(xJsonSax *sax);

#endif /* XJSON_SAX_H */
