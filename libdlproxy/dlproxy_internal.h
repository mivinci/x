/*
 * dlproxy_internal.h - Internal structs shared by dlproxy modules
 */
#ifndef DLPROXY_INTERNAL_H
#define DLPROXY_INTERNAL_H

#include "bus.h"
#include "cache.h"
#include "scheduler.h"
#include "proxy.h"
#include <x/base/event.h>
#include <x/base/thread.h>

struct dlp_ctx {
  xEventLoop        loop;
  dlp_mode_t        mode;
  dlp_conf_t        conf;
  dlp_bus_t         bus;
  dlp_cache_t       cache;
  dlp_scheduler_t   scheduler;
  dlp_proxy_t       proxy;
  xThread           thread;
};

#endif
