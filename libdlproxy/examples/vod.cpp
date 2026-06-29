/*
 * vod.cpp - dlproxy POLL mode example
 *
 * Usage: vod [port] [cache_dir] [url1] [url2] ...
 *
 * Each URL creates a task. .m3u8 → HLS (rid=test-hls), otherwise MP4 (rid=test-mp4).
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

  /* Create a task for each URL argument */
  for (int i = 3; i < argc; i++) {
    dlp_task_conf_t tc = {0};
    tc.url  = argv[i];
    tc.size = 0;

    if (ends_with(argv[i], ".m3u8")) {
      tc.format = DLP_FMT_HLS;
      tc.rid    = "test-hls";
    } else {
      tc.format = DLP_FMT_MP4;
      tc.rid    = "test-mp4";
    }

    dlp_task_t task = dlp_task_create(g_ctx, &tc);
    if (task) {
      dlp_task_start(task);
      char proxy_url[256];
      dlp_task_proxy_url(task, proxy_url, sizeof(proxy_url));
      fprintf(stderr, "Task '%s' (%s): %s\n", tc.rid,
              tc.format == DLP_FMT_HLS ? "HLS" : "MP4", argv[i]);
      fprintf(stderr, "Proxy URL: %s\n", proxy_url);
    } else {
      fprintf(stderr, "dlp_task_create failed for %s\n", argv[i]);
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  fprintf(stderr, "Starting dlproxy (port=%d, cache=%s)...\n", port, cache_dir);
  dlp_run(g_ctx, DL_MODE_POLL);

  fprintf(stderr, "dlproxy stopped\n");
  return 0;
}
