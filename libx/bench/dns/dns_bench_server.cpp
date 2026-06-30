/*
 * dns_bench_server.cpp — Local DNS benchmark server
 *
 * Runs an authoritative xDnsServer on 127.0.0.1:5353 that responds
 * to bench-<N>.local with 192.168.0.<N> for A-record queries.
 */
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <x/base/event.h>
#include <x/dns/dns.h>
}

int main(int argc, char **argv) {
  int port = 15353;
  if (argc > 1) port = atoi(argv[1]);

  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }
  xEventLoopEnter(loop);

  xDnsZone zone = xDnsZoneCreate();

  /* Register 100 zone entries: bench-0.local … bench-99.local → 192.168.0.0 … 192.168.0.99 */
  for (int i = 0; i < 100; i++) {
    char name[64];
    snprintf(name, sizeof(name), "bench-%d.local", i);
    uint8_t ip[4] = {192, 168, 0, (uint8_t)i};
    xDnsZoneAdd(zone, name, xDnsType_A, ip, 4, 3600);
  }

  xDnsServerConf conf   = {};
  xDnsServer     server = xDnsServerCreate(&conf);
  if (!server) {
    fprintf(stderr, "Failed to create server\n");
    return 1;
  }

  xDnsServerAddZone(server, zone);
  if (xDnsServerListen(server, "127.0.0.1", (uint16_t)port) != xErrno_Ok) {
    fprintf(stderr, "Failed to listen on port %d\n", port);
    return 1;
  }

  fprintf(stderr, "DNS bench server listening on 127.0.0.1:%d\n", port);
  xEventLoopRun(loop, X_RUN_DEFAULT);

  xDnsServerDestroy(server);
  xDnsZoneDestroy(zone);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
  return 0;
}
