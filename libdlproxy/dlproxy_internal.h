/*
 * dlproxy_internal.h - Internal structs shared by dlproxy modules
 */
#ifndef DLPROXY_INTERNAL_H
#define DLPROXY_INTERNAL_H

#include "bus.h"
#include "cache.h"
#include "http.h"
#include "proxy.h"
#include <x/base/event.h>
#include <x/base/map.h>
#include <x/base/thread.h>

struct dlp_task;

struct dlp_scheduler_vtable {
  void (*on_tick)(struct dlp_task *task);
  void (*on_block_done)(struct dlp_task *task, uint64_t offset, size_t len);
  void (*on_start)(struct dlp_task *task);
  void (*on_stop)(struct dlp_task *task);
};

extern const struct dlp_scheduler_vtable dlp_sched_mp4;

struct dlp_task {
  char            rid[64];
  char            url[512];
  struct dlp_ctx *ctx;
  bool            running;
  xTimer          tick_timer;
  const struct dlp_scheduler_vtable *sched;

  /* Scheduling state */
  uint64_t        read_offset;     /* player current byte position         */
  int             remain_time_ms;  /* remaining playable time in buffer    */
  int             emergency_ms;    /* emergency buffer threshold            */
  int             safe_ms;         /* safe buffer target                    */
  uint32_t        bitrate;         /* estimated bitrate (bytes/sec)         */
  bool            was_pulling;     /* hysteresis: was pulling last tick      */
  xHttpClient     dl_client;       /* dedicated HTTP client for this task   */
};

struct dlp_ctx {
  xEventLoop        loop;
  dlp_mode_t        mode;
  dlp_conf_t        conf;
  dlp_bus_t         bus;
  dlp_cache_t       cache;
  dlp_http_t   dl_http;
  dlp_proxy_t       proxy;
  xMap              url_map;    /* rid → cdn_url (strdup'd) */
  xMap              task_map;   /* rid → dlp_task_t          */
  xThread           thread;
};

#endif
