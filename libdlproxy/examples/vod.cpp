/*
 * vod.cpp - dlproxy POLL mode example
 *
 * Usage: vod [port] [cache_dir] [test_url]
 *
 * If test_url ends in .m3u8, an HLS task is created; otherwise MP4.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

extern "C" {
#include <dlproxy.h>
}

static dlp_ctx_t g_ctx = nullptr;

static void on_signal(int) {
  if (g_ctx) dlp_stop(g_ctx);
}

static bool ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), lf = strlen(suffix);
  return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

int main(int argc, char **argv) {
  const char *cache_dir = "./.cache";
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

  /* Create a test task if a URL is provided */
  if (argc > 3) {
    dlp_task_conf_t tc = {0};
    tc.rid  = "test";
    tc.url  = argv[3];
    tc.size = 0;

    /* Auto-detect HLS from .m3u8 extension */
    if (ends_with(argv[3], ".m3u8")) {
      tc.format = DLP_FMT_HLS;
      fprintf(stderr, "Task 'test' (HLS): %s\n", argv[3]);
      fprintf(stderr, "Test with: http://127.0.0.1:%d/test/playlist.m3u8\n", port);
    } else {
      tc.format = DLP_FMT_MP4;
      fprintf(stderr, "Task 'test' (MP4): %s\n", argv[3]);
      fprintf(stderr, "Test with: curl -H 'Range: bytes=0-262143' http://127.0.0.1:%d/test\n", port);
    }

    dlp_task_t task = dlp_task_create(g_ctx, &tc);
    if (task) {
      dlp_task_start(task);
    } else {
      fprintf(stderr, "dlp_task_create failed\n");
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  fprintf(stderr, "Starting dlproxy (port=%d, cache=%s)...\n", port, cache_dir);
  dlp_run(g_ctx, DL_MODE_POLL);

  fprintf(stderr, "dlproxy stopped\n");
  return 0;
}
