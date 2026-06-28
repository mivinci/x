/*
 * scheduler.h - Async Range download scheduler
 */
#ifndef DLP_SCHEDULER_H
#define DLP_SCHEDULER_H

#include "dlproxy.h"
#include <x/base/event.h>
#include <x/base/error.h>

typedef struct dlp_scheduler *dlp_scheduler_t;

dlp_scheduler_t dlp_scheduler_init(dlp_ctx_t ctx, xEventLoop loop);
void            dlp_scheduler_deinit(dlp_scheduler_t s);
xErrno          dlp_scheduler_fetch(dlp_scheduler_t s, const char *rid, const char *url, uint64_t offset, size_t len);

#endif
