/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * echo_server.cpp - xpp::http::Server end-to-end benchmark server
 *
 * Usage: echo_server [port]
 *   Default port: 8080
 *
 * Routes:
 *   GET  /ping        → 200 "pong"           (minimal response — pure QPS)
 *   GET  /echo?size=N → 200 with N bytes of 'x' (payload throughput)
 *   POST /echo        → 200 echoing the request body
 *
 * Designed to be benchmarked with wrk:
 *   wrk -t4 -c100 -d10s http://127.0.0.1:8080/ping
 *   wrk -t4 -c100 -d10s http://127.0.0.1:8080/echo?size=1024
 *   wrk -t4 -c100 -d10s -s post.lua http://127.0.0.1:8080/echo
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include <xpp/event.h>
#include <xpp/http/server.h>

namespace http = xpp::http;

static size_t parse_size(const xpp::String &url) {
  size_t size = 64; // default
  auto   q    = url.find("size=");
  if (q.is_some()) {
    auto   b = url.as_bytes();
    size_t v = 0;
    for (size_t i = q.unwrap() + 5; i < b.size(); ++i) {
      uint8_t c = b.data()[i];
      if (c < '0' || c > '9') break;
      v = v * 10 + static_cast<size_t>(c - '0');
    }
    if (v > 0) size = v;
    if (size > 1048576) size = 1048576; // cap at 1 MB
  }
  return size;
}

int main(int argc, char *argv[]) {
  uint16_t port = 8080;
  if (argc > 1) port = static_cast<uint16_t>(atoi(argv[1]));

  xpp::EventLoop loop;
  if (!loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }
  xpp::WaitScope scope(loop);

  auto server_r =
    http::Server::builder()
      .route("GET /ping", [](http::Request) { return http::Response::ok("pong"); })
      .route("GET /echo",
             [](http::Request req) {
               std::string body(parse_size(req.url()), 'x');
               return http::Response::ok(xpp::Bytes::copy(body.data(), body.size()));
             })
      .route("POST /echo",
             [](http::Request req) -> xpp::Promise<http::Result<http::Response>> {
               return req.into_body().bytes().then(
                 [](http::Result<xpp::Bytes> b) -> http::Result<http::Response> {
                   if (b.is_err()) {
                     return http::Result<http::Response>(
                       xpp::err, http::Error(http::Error::Kind::Io,
                                             xpp::String::from_utf8("read body failed").unwrap()));
                   }
                   return http::Result<http::Response>(xpp::ok, http::Response::ok(b.unwrap()));
                 });
             })
      .bind("0.0.0.0", port)
      .build();

  if (server_r.is_err()) {
    fprintf(stderr, "Failed to build server\n");
    return 1;
  }
  http::Server server = std::move(server_r).unwrap();

  auto running = server.serve();

  fprintf(stdout, "xpp echo server listening on 0.0.0.0:%u\n", server.port());
  fprintf(stdout, "  GET  /ping        → 200 \"pong\"\n");
  fprintf(stdout, "  GET  /echo?size=N → 200 with N bytes\n");
  fprintf(stdout, "  POST /echo        → 200 echo body\n");
  fflush(stdout);

  loop.run();

  return 0;
}
