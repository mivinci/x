/*
 * vod.cpp - dlproxy POLL mode example
 */
#include <cstdio>
#include <cstdlib>
#include <csignal>

extern "C" {
#include <dlproxy.h>
}

static dlp_ctx_t g_ctx = nullptr;

static void on_signal(int) {
  if (g_ctx) dlp_stop(g_ctx);
}

int main(int argc, char **argv) {
  const char *cache_dir = "./cache";
  uint16_t    port      = 19080;

  if (argc > 1) port = (uint16_t)atoi(argv[1]);
  if (argc > 2) cache_dir = argv[2];

  dlp_conf_t conf = {0};
  conf.port      = port;
  conf.cache_dir = cache_dir;

  g_ctx = dlp_init(&conf);
  if (!g_ctx) {
    fprintf(stderr, "dlp_init failed\n");
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  fprintf(stderr, "Starting dlproxy in POLL mode (port=%d, cache=%s)...\n", port, cache_dir);
  dlp_run(g_ctx, DL_MODE_POLL);

  fprintf(stderr, "dlproxy stopped\n");
  return 0;
}
