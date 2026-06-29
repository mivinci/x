/*
 * http.h - Async HTTP Range download helper
 */
#ifndef DLP_HTTP_H
#define DLP_HTTP_H

#include "dlproxy.h"
#include <x/base/event.h>
#include <x/base/error.h>
#include <x/http/client.h>

typedef struct dlp_http *dlp_http_t;

struct dlp_task; /* forward declaration */

dlp_http_t dlp_http_init(dlp_ctx_t ctx);
void       dlp_http_deinit(dlp_http_t h);

/** Issue an HTTP Range GET. @p task is optional (for on_block_done update). */
xErrno dlp_http_fetch(dlp_http_t h, const char *rid, const char *clip_id,
                       const char *url, uint64_t offset, size_t len,
                       struct dlp_task *task);

/** Issue an HTTP GET without Range header (full content). @p task is optional. */
xErrno dlp_http_fetch_full(dlp_http_t h, const char *rid, const char *clip_id,
                            const char *url, struct dlp_task *task);

/** Fetch a URL and return the response body to @p on_data/@p on_done callbacks. */
xErrno dlp_http_fetch_text(dlp_http_t h, const char *url,
                            int (*on_data)(const char *data, size_t len, void *arg),
                            void (*on_done)(xHttpCtx *ctx, void *arg),
                            void *arg);

#endif
