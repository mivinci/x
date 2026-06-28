/*
 * dns_bench_cares.cpp — DNS resolver benchmarks for c-ares
 *
 * Usage: dns_bench_cares <local|remote>
 *
 * Uses c-ares's external event-loop integration (ares_fds + select +
 * ares_process_fd) rather than xbase's event loop.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>
#include <sys/select.h>

#include <ares.h>

using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;

/* ─────────────────── Helpers ─────────────────── */

static const char *remote_hosts[] = {
  "google.com","github.com","amazon.com","microsoft.com",
  "apple.com","netflix.com","stackoverflow.com","youtube.com",
  "wikipedia.org","reddit.com","twitter.com","linkedin.com",
  "cloudflare.com","zoom.us","dropbox.com","spotify.com",
  "adobe.com","oracle.com","ibm.com","intel.com",
};
static const int n_remote_hosts = (int)(sizeof(remote_hosts) / sizeof(remote_hosts[0]));

struct BenchResult {
  const char *scenario;
  long long latency_us;
  int       queries;
  int       success;
  int       failed;
};

static void json_out(const char *resolver, const char *mode, const BenchResult &r) {
  static int first = 1;
  if (first) { printf("[\n"); first = 0; }
  else printf(",\n");
  printf("  {\"resolver\":\"%s\",\"mode\":\"%s\",\"scenario\":\"%s\",\"latency_us\":%lld",
         resolver, mode, r.scenario, r.latency_us);
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
    int success = 0;
  } ctx;

  Clock::time_point start = Clock::now();

  ares_getaddrinfo(ch, name, NULL, NULL,
    [](void *arg, int status, int, struct ares_addrinfo *res) {
      auto *c = (decltype(&ctx))arg;
      c->success = (status == ARES_SUCCESS) ? 1 : 0;
      if (res) ares_freeaddrinfo(res);
      c->done.store(true);
    }, &ctx);

  cares_wait(ch, ctx.done);

  auto lat = std::chrono::duration_cast<us>(Clock::now() - start);
  return BenchResult{nullptr, lat.count(), 1, ctx.success, ctx.success ? 0 : 1};
}

static BenchResult bench_batch(ares_channel_t *ch, const char **names, int count) {
  long long total = 0;
  int ok = 0, fail = 0;

  for (int i = 0; i < count; i++) {
    auto r = bench_single(ch, names[i]);
    total += r.latency_us;
    ok    += r.success;
    fail  += r.failed;
  }
  return BenchResult{"batch", total, count, ok, fail};
}

int main(int argc, char **argv) {
  bool local = true;
  if (argc > 1 && strcmp(argv[1], "remote") == 0) local = false;
  const char *mode = local ? "local" : "remote";

  if (local) {
    /* c-ares uses /etc/resolv.conf, not a custom UDP address */
    printf("[{\"resolver\":\"cares\",\"mode\":\"local\",\"skipped\":true,"
           "\"reason\":\"c-ares uses system resolver config\"}]\n");
    return 0;
  }

  ares_channel_t *ch = NULL;
  struct ares_options opts = {};
  opts.timeout = 5000;
  opts.tries = 2;

  int status = ares_init_options(&ch, &opts, ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES);
  if (status != ARES_SUCCESS) {
    fprintf(stderr, "ares_init_options failed: %d\n", status);
    return 1;
  }

  const char *single_host = "google.com";

  int batch_count = n_remote_hosts;
  const char **batch_names = (const char **)malloc((size_t)batch_count * sizeof(char *));
  for (int i = 0; i < batch_count; i++) batch_names[i] = remote_hosts[i];

  /* Warmup */
  bench_single(ch, single_host);

  auto r_single = bench_single(ch, single_host);
  r_single.scenario = "single_query";
  json_out("cares", mode, r_single);

  auto r_batch = bench_batch(ch, batch_names, batch_count);
  json_out("cares", mode, r_batch);

  free(batch_names);
  ares_destroy(ch);

  printf("\n]\n");
  return 0;
}
