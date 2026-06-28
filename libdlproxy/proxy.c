/*
 * proxy.c - Local HTTP proxy server stub
 */
#include "proxy.h"
#include "dlproxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <x/base/event.h>
#include <x/http/server.h>

struct dlp_proxy {
  xHttpServer server;
  xHttpMux    mux;
  uint16_t    port;
  xEventLoop  loop;
};

static int on_request(xHttpCtx *ctx, void *arg) {
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, "dlproxy skeleton\n", 17);
  return 0;
}

dlp_proxy_t dlp_proxy_init(dlp_ctx_t ctx, xEventLoop loop, uint16_t port) {
  (void)ctx;
  struct dlp_proxy *p = (struct dlp_proxy *)calloc(1, sizeof(*p));
  if (!p) return NULL;

  p->loop = loop;
  p->port = port;
  xEventLoopEnter(loop);

  p->mux = xHttpMuxCreate();
  if (!p->mux) goto fail;

  xHttpRouteConf rc = {0};
  rc.pattern    = "GET /:rid";
  rc.on_request = on_request;
  xHttpMuxHandle(p->mux, &rc);

  xHttpServerConf sc = {0};
  sc.resolve = xHttpMuxResolve;
  sc.router  = p->mux;
  p->server  = xHttpServerCreate(&sc);
  if (!p->server) goto fail;

  if (xHttpServerListen(p->server, "127.0.0.1", port) != xErrno_Ok)
    goto fail;

  fprintf(stderr, "dlproxy listening on 127.0.0.1:%d\n", port);
  xEventLoopLeave();
  return p;

fail:
  if (p->server) xHttpServerDestroy(p->server);
  if (p->mux) xHttpMuxDestroy(p->mux);
  xEventLoopLeave();
  free(p);
  return NULL;
}

void dlp_proxy_deinit(dlp_proxy_t p) {
  if (!p) return;
  if (p->server) xHttpServerDestroy(p->server);
  if (p->mux) xHttpMuxDestroy(p->mux);
  free(p);
}
