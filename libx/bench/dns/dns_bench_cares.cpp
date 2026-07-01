/*
 * dns_bench_cares.cpp — DNS resolver benchmarks for c-ares
 *
 * Usage: dns_bench_cares <local|remote>
 *
 * Uses c-ares's external event-loop integration (ares_fds + select +
 * ares_process_fd) rather than xbase's event loop.
 */
#include <ares.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/select.h>

using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;

/* ─────────────────── Helpers ─────────────────── */

static const char *remote_hosts[] = {
  "google.com",  "github.com",        "amazon.com",     "microsoft.com", "apple.com",
  "netflix.com", "stackoverflow.com", "youtube.com",    "wikipedia.org", "reddit.com",
  "twitter.com", "linkedin.com",      "cloudflare.com", "zoom.us",       "dropbox.com",
  "spotify.com", "adobe.com",         "oracle.com",     "ibm.com",       "intel.com",
};
static const int n_remote_hosts = static_cast<int>(sizeof(remote_hosts) / sizeof(remote_hosts[0]));

struct BenchResult {
  const char *scenario;
  long long   latency_us;
  int         queries;
  int         success;
  int         failed;
};

static void json_out(const char *resolver, const char *mode, const BenchResult &r) {
  static int first = 1;
  if (first) {
    printf("[\n");
    first = 0;
  } else
    printf(",\n");
  printf("  {\"resolver\":\"%s\",\"mode\":\"%s\",\"scenario\":\"%s\",\"latency_us\":%lld", resolver,
         mode, r.scenario, r.latency_us);
  if (r.queries > 1) printf(",\"queries\":%d", r.queries);
  printf(",\"success\":%d,\"failed\":%d", r.success, r.failed);
  printf("}");
}

/* Pump c-ares event loop until the callback fires. */
static void cares_wait(ares_channel_t *ch, std::atomic<bool> &done) {
  while (!done.load()) {
    fd_set         read_fds, write_fds;
    int            nfds;
    struct timeval tv, *tvp;

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    nfds = ares_fds(ch, &read_fds, &write_fds);
    if (nfds == 0) break; /* no fds to wait on */

    tvp = ares_timeout(ch, NULL, &tv);
    select(nfds, &read_fds, &write_fds, NULL, tvp);
    ares_process(ch, &read_fds, &write_fds);
  }
}

static BenchResult bench_single(ares_channel_t *ch, const char *name) {
  struct {
    std::atomic<bool> done{false};
    int               success = 0;
  } ctx;

  Clock::time_point start = Clock::now();

  ares_getaddrinfo(
    ch, name, NULL, NULL,
    [](void *arg, int status, int, struct ares_addrinfo *res) {
      auto *c    = static_cast<decltype(&ctx)>(arg);
      c->success = (status == ARES_SUCCESS) ? 1 : 0;
      if (res) ares_freeaddrinfo(res);
      c->done.store(true);
    },
    &ctx);

  cares_wait(ch, ctx.done);

  auto lat = std::chrono::duration_cast<us>(Clock::now() - start);
  return BenchResult{nullptr, lat.count(), 1, ctx.success, ctx.success ? 0 : 1};
}

static BenchResult bench_batch(ares_channel_t *ch, const char **names, int count) {
  struct {
    std::atomic<int> ok{0};
    std::atomic<int> pending{0};
  } ctx;
  ctx.pending.store(count);

  Clock::time_point start = Clock::now();

  for (int i = 0; i < count; i++) {
    ares_getaddrinfo(
      ch, names[i], NULL, NULL,
      [](void *arg, int status, int, struct ares_addrinfo *res) {
        auto *c = static_cast<decltype(&ctx)>(arg);
        if (status == ARES_SUCCESS) c->ok.fetch_add(1);
        if (res) ares_freeaddrinfo(res);
        c->pending.fetch_sub(1);
      },
      &ctx);
  }

  /* Pump until all complete */
  while (ctx.pending.load() > 0) {
    fd_set         read_fds, write_fds;
    struct timeval tv, *tvp;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    (void)ares_fds(ch, &read_fds, &write_fds);
    tvp = ares_timeout(ch, NULL, &tv);
    select(FD_SETSIZE, &read_fds, &write_fds, NULL, tvp);
    ares_process(ch, &read_fds, &write_fds);
  }

  auto wall = std::chrono::duration_cast<us>(Clock::now() - start);
  return BenchResult{"batch", wall.count(), count, ctx.ok.load(), count - ctx.ok.load()};
}

int main(int argc, char **argv) {
  bool local = true;
  if (argc > 1 && strcmp(argv[1], "remote") == 0) local = false;
  const char *mode = local ? "local" : "remote";

  ares_channel_t     *ch   = NULL;
  struct ares_options opts = {};
  opts.timeout             = 2000;
  opts.tries               = 1;

  int status = ares_init_options(&ch, &opts, ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES);
  if (status != ARES_SUCCESS) {
    fprintf(stderr, "ares_init_options failed: %d\n", status);
    return 1;
  }
  ares_set_servers_csv(ch, local ? "127.0.0.1:15353" : "8.8.8.8");

  const char *single_host = local ? "bench-0.local" : "google.com";

  /* Build batch names */
  int          batch_count = local ? 100 : n_remote_hosts;
  const char **batch_names =
    static_cast<const char **>(malloc(static_cast<size_t>(batch_count) * sizeof(char *)));
  if (local) {
    for (int i = 0; i < 100; i++) {
      char *buf = static_cast<char *>(malloc(64));
      snprintf(buf, 64, "bench-%d.local", i);
      batch_names[i] = buf;
    }
  } else {
    for (int i = 0; i < batch_count; i++)
      batch_names[i] = remote_hosts[i];
  }

  /* Warmup */
  bench_single(ch, single_host);

  auto r_single     = bench_single(ch, single_host);
  r_single.scenario = "single_query";
  json_out("cares", mode, r_single);

  auto r_batch = bench_batch(ch, batch_names, batch_count);
  json_out("cares", mode, r_batch);

  if (local)
    for (int i = 0; i < 100; i++)
      free(const_cast<char *>(batch_names[i]));
  free(batch_names);
  ares_destroy(ch);

  printf("\n]\n");
  return 0;
}
