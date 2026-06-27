/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_serve.c - WebSocket-only server convenience function
 */

#include <x/http/server.h>
#include <x/http/ws.h>

#include "server_private.h"

#include <stdlib.h>

/* ── Internal state for the catch-all upgrade handler ──── */

XDEF_STRUCT(xWsServeCtx) {
  xWsCallbacks callbacks;
  void        *user_arg;
};

static void ws_serve_handler(xHttpCtx *ctx, void *arg) {
  xWsServeCtx *cbs_ctx = (xWsServeCtx *)arg;
  xWsUpgrade(ctx, &cbs_ctx->callbacks, cbs_ctx->user_arg);
}

/* ── Public API ────────────────────────────────────────── */

xHttpServer xWsServe( const char *host, uint16_t port,
                     const xWsCallbacks *callbacks, void *arg) {
  xEventLoop loop = xEventLoopCurrent();
  if (!loop || !callbacks) return NULL;

  xHttpMux mux = xHttpMuxCreate();
  if (!mux) return NULL;

  xWsServeCtx *ctx = (xWsServeCtx *)calloc(1, sizeof(xWsServeCtx));
  if (!ctx) {
    xHttpMuxDestroy(mux);
    return NULL;
  }

  ctx->callbacks = *callbacks;
  ctx->user_arg  = arg;

  xHttpRouteConf route_conf = {};
  route_conf.pattern        = "GET /";
  route_conf.on_done        = ws_serve_handler;
  route_conf.arg            = ctx;

  xErrno err = xHttpMuxHandle(mux, &route_conf);
  if (err != xErrno_Ok) {
    free(ctx);
    xHttpMuxDestroy(mux);
    return NULL;
  }

  xHttpServerConf sconf = {};
  sconf.resolve         = xHttpMuxResolve;
  sconf.router          = mux;
  sconf.idle_timeout_ms = XHTTP_DEFAULT_IDLE_TIMEOUT_MS;

  xHttpServer server = xHttpServerCreate(&sconf);
  if (!server) {
    free(ctx);
    xHttpMuxDestroy(mux);
    return NULL;
  }

  /* Attach ctx and mux to the server so they are freed automatically
   * when xHttpServerDestroy() is called. */
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  /* Store mux in aux_data with a custom free that also frees ctx.
   * We use a small helper struct to hold both. */
  XDEF_STRUCT(xWsServeAux) {
    xWsServeCtx *ctx;
    xHttpMux     mux;
  };
  struct xWsServeAux *aux = (struct xWsServeAux *)calloc(1, sizeof(*aux));
  if (!aux) {
    xHttpServerDestroy(server);
    free(ctx);
    xHttpMuxDestroy(mux);
    return NULL;
  }
  aux->ctx = ctx;
  aux->mux = mux;

  /* Use a trampoline free function */
  XDEF_STRUCT(xWsServeAuxHolder) {
    void (*free_fn)(void *);
    struct xWsServeAux *aux;
  };

  /* We need a static free function, so store aux directly and use a
   * function pointer indirection. Since aux_data is a void* and aux_free
   * is a void(*)(void*), we can embed the mux pointer in the struct and
   * use a single free. Let's simplify: just store the aux struct, and
   * set aux_free to a function that frees both ctx and mux. */

  s->aux_data = aux;
  /* Can't easily set a custom free function that knows about both.
   * Instead, free them in xHttpServerDestroy's aux_free callback. */
  extern void xWsServeAuxFree(void *p);
  s->aux_free = xWsServeAuxFree;

  err = xHttpServerListen(server, host, port);
  if (err != xErrno_Ok) {
    xHttpServerDestroy(server);
    return NULL;
  }

  return server;
}

/* Free helper for xWsServe's auxiliary data */
void xWsServeAuxFree(void *p) {
  struct {
    xWsServeCtx *ctx;
    xHttpMux     mux;
  } *aux = p;
  if (aux) {
    free(aux->ctx);
    xHttpMuxDestroy(aux->mux);
    free(aux);
  }
}
