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

#endif
