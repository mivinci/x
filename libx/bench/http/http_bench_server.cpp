/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * http_bench_server.cpp - HTTP server for end-to-end benchmarking
 *
 * Usage: http_bench_server [port]
 *   Default port: 8080
 *
 * Routes:
 *   GET  /ping          → 200 "pong"  (minimal response)
 *   GET  /echo?size=N   → 200 with N bytes of 'x' (payload test)
 *   POST /echo          → 200 echoing the request body
 *
 * Built on the current xhttp server API (xHttpMux resolver + xHttpCtx
 * handlers). Designed to be benchmarked with wrk, ab, or hey:
 *   wrk -t4 -c100 -d10s http://127.0.0.1:8080/ping
 *   hey -n 100000 -c 50 http://127.0.0.1:8080/ping
 */

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <x/base/event.h>
#include <x/http/server.h>

static xEventLoop g_loop = nullptr;

/* GET /ping → "pong" */
static void handle_ping(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, "pong", 4);
}

/* GET /echo?size=N → N bytes of 'x' */
static void handle_echo(xHttpCtx *ctx, void *arg) {
  (void)arg;

  size_t      size = 64; /* default */
  const char *q    = strchr(ctx->url, '?');
  if (q) {
    const char *sp = strstr(q, "size=");
    if (sp) {
      size = static_cast<size_t>(atoi(sp + 5));
      if (size == 0) size = 64;
      if (size > 1048576) size = 1048576; /* cap at 1MB */
    }
  }

  std::vector<char> body(size, 'x');
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "application/octet-stream");
  xHttpCtxSend(ctx, body.data(), body.size());
}

/* POST /echo → echo the request body.
 *
 * Each request gets its own EchoBody via on_request (xHttpCtxSetUser);
 * on_data accumulates chunks and on_done writes it. The EchoBody is
 * freed by on_done on the happy path (and the user pointer nulled so
 * on_close does not double-free); on_close frees it when the request
 * dies before on_done (client disconnect / pending error / abort).
 * Concurrent requests on the same route stay isolated because
 * on_data/on_done see the per-request user data, not the route-level arg. */
struct EchoBody {
  std::vector<char> body;
};

static int echo_on_request(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetUser(ctx, new EchoBody());
  return 0;
}

static int echo_on_data(const char *data, size_t len, void *arg) {
  auto *c = static_cast<EchoBody *>(arg);
  c->body.insert(c->body.end(), data, data + len);
  return 0;
}

static void echo_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<EchoBody *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "application/octet-stream");
  xHttpCtxSend(ctx, c->body.data(), c->body.size());
  delete c;
  xHttpCtxSetUser(ctx, NULL); /* on_close must not double-free */
}

/* Arg is the route-level arg (may be NULL) — read the per-request state
 * from the ctx. on_close fires on EVERY teardown path (including ones that
 * never reach on_done), so it is the leak guard for aborted requests. */
static void echo_on_close(xHttpCtx *ctx, void *arg) {
  (void)arg;
  auto *c = static_cast<EchoBody *>(xHttpCtxUser(ctx));
  if (c) {
    delete c;
    xHttpCtxSetUser(ctx, NULL);
  }
}

int main(int argc, char *argv[]) {
  uint16_t port = 8080;
  if (argc > 1) port = static_cast<uint16_t>(atoi(argv[1]));

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }
  xEventLoopEnter(g_loop); /* xHttpServerCreate requires a bound loop */

  /* Watch SIGINT to stop gracefully */
  xSignal(SIGINT, [](int, void *) { xEventLoopStop(g_loop); }, nullptr);

  xHttpMux mux = xHttpMuxCreate();
  if (!mux) {
    fprintf(stderr, "Failed to create HTTP mux\n");
    xEventLoopLeave();
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xHttpServerConf conf = {};
  conf.resolve         = xHttpMuxResolve;
  conf.router          = mux;
  conf.idle_timeout_ms = 60000;
  xHttpServer server   = xHttpServerCreate(&conf);
  if (!server) {
    fprintf(stderr, "Failed to create HTTP server\n");
    xHttpMuxDestroy(mux);
    xEventLoopLeave();
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xHttpRouteConf rc = {};
  rc.pattern        = "GET /ping";
  rc.on_done        = handle_ping;
  xHttpMuxHandle(mux, &rc);

  rc         = {};
  rc.pattern = "GET /echo";
  rc.on_done = handle_echo;
  xHttpMuxHandle(mux, &rc);

  rc            = {};
  rc.pattern    = "POST /echo";
  rc.on_request = echo_on_request;
  rc.on_data    = echo_on_data;
  rc.on_done    = echo_on_done;
  rc.on_close   = echo_on_close;
  xHttpMuxHandle(mux, &rc);

  xErrno err = xHttpServerListen(server, "0.0.0.0", port);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to listen on port %u\n", port);
    xHttpServerDestroy(server);
    xHttpMuxDestroy(mux);
    xEventLoopLeave();
    xEventLoopDestroy(g_loop);
    return 1;
  }

  fprintf(stdout, "HTTP bench server listening on 0.0.0.0:%u\n", port);
  fprintf(stdout, "Routes:\n");
  fprintf(stdout, "  GET  /ping        → 200 \"pong\"\n");
  fprintf(stdout, "  GET  /echo?size=N → 200 with N bytes\n");
  fprintf(stdout, "  POST /echo        → 200 echo body\n");
  fflush(stdout);

  xEventLoopRun(g_loop, X_RUN_DEFAULT);

  xHttpServerDestroy(server);
  xHttpMuxDestroy(mux);
  xEventLoopLeave();
  xEventLoopDestroy(g_loop);
  return 0;
}
