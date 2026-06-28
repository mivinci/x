/*
 * scheduler.h - Async Range download scheduler
 */
#ifndef DLP_SCHEDULER_H
#define DLP_SCHEDULER_H

#include "dlproxy.h"
#include <x/base/event.h>
#include <x/base/error.h>
#include <x/http/client.h>

typedef struct dlp_scheduler *dlp_scheduler_t;

struct dlp_task; /* forward declaration */

dlp_scheduler_t dlp_scheduler_init(dlp_ctx_t ctx, xEventLoop loop);
void            dlp_scheduler_deinit(dlp_scheduler_t s);

/** Issue an HTTP Range GET. @p task is optional (for remain_time update). */
xErrno dlp_scheduler_fetch(dlp_scheduler_t s, const char *rid,
                            const char *clip_id, const char *url,
                            uint64_t offset, size_t len,
                            struct dlp_task *task);

#endif
