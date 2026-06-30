/*
 * proxy.h - Local HTTP proxy server
 */
#ifndef DLP_PROXY_H
#define DLP_PROXY_H

#include "dlproxy.h"

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

typedef struct dlp_proxy *dlp_proxy_t;

XCAPI(dlp_proxy_t) dlp_proxy_init(dlp_ctx_t ctx, xEventLoop loop, uint16_t port);
XCAPI(void)        dlp_proxy_deinit(dlp_proxy_t p);

#endif
