/*
 * scheduler.c - Async Range download scheduler stub
 */
#include "scheduler.h"
#include <stdlib.h>

dlp_scheduler_t dlp_scheduler_init(dlp_ctx_t ctx, xEventLoop loop) { (void)ctx; (void)loop; return NULL; }
void            dlp_scheduler_deinit(dlp_scheduler_t s) { (void)s; }
xErrno          dlp_scheduler_fetch(dlp_scheduler_t s, const char *rid, const char *url, uint64_t offset, size_t len) { (void)s; (void)rid; (void)url; (void)offset; (void)len; return xErrno_Ok; }
