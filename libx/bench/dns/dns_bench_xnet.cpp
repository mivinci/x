/*
 * dns_bench_xnet.cpp — DNS resolver benchmarks for xnet/dns (getaddrinfo pool)
 *
 * Usage: dns_bench_xnet <local|remote>
 *
 * Note: local mode is not supported — getaddrinfo doesn't use our local DNS
 * server.  Skip or use /etc/hosts entries.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>

extern "C" {
#include <x/base/event.h>
#include <x/net/dns.h>
}

using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;

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

static BenchResult bench_single(const char *name) {
  xEventLoop loop    = xEventLoopCurrent();
  int        success = 0;
  Clock::time_point start = Clock::now();

  xDnsResolve(name, nullptr, nullptr,
    [](xDnsResult *result, void *arg) {
      int *ok = (int *)arg;
      *ok = (result->error == xErrno_Ok) ? 1 : 0;
      xDnsResultFree(result);
      xEventLoopStop(xEventLoopCurrent());
    }, &success);

  xEventLoopRun(loop, X_RUN_DEFAULT);

  auto lat = std::chrono::duration_cast<us>(Clock::now() - start);
  return BenchResult{nullptr, lat.count(), 1, success, success ? 0 : 1};
}

static BenchResult bench_batch(const char **names, int count) {
  struct {
    std::atomic<int> ok{0};
    std::atomic<int> pending{0};
  } ctx;
  ctx.pending.store(count);
  xEventLoop loop = xEventLoopCurrent();

  Clock::time_point start = Clock::now();

  for (int i = 0; i < count; i++) {
    xDnsResolve(names[i], nullptr, nullptr,
      [](xDnsResult *result, void *arg) {
        auto *c = (decltype(&ctx))arg;
        if (result->error == xErrno_Ok) c->ok.fetch_add(1);
        xDnsResultFree(result);
        if (c->pending.fetch_sub(1) == 1) xEventLoopStop(xEventLoopCurrent());
      }, &ctx);
  }

  xEventLoopRun(loop, X_RUN_DEFAULT);

  auto wall = std::chrono::duration_cast<us>(Clock::now() - start);
  return BenchResult{"batch", wall.count(), count, ctx.ok.load(), count - ctx.ok.load()};
}

int main(int argc, char **argv) {
  bool local = true;
  if (argc > 1 && strcmp(argv[1], "remote") == 0) local = false;
  const char *mode = local ? "local" : "remote";

  if (local) {
    printf("[{\"resolver\":\"xnet_dns\",\"mode\":\"local\",\"skipped\":true,"
           "\"reason\":\"getaddrinfo does not use custom DNS server\"}]\n");
    return 0;
  }

  xEventLoop loop = xEventLoopCreate();
  if (!loop) return 1;
  xEventLoopEnter(loop);

  const char *single_host = "google.com";

  int batch_count = n_remote_hosts;
  const char **batch_hosts = (const char **)malloc((size_t)batch_count * sizeof(char *));
  for (int i = 0; i < batch_count; i++) batch_hosts[i] = remote_hosts[i];

  /* Warmup: prime system resolver cache with same domain */
  bench_single(single_host);

  auto r_single = bench_single(single_host);
  r_single.scenario = "single_query";
  json_out("xnet_dns", mode, r_single);

  auto r_batch = bench_batch(batch_hosts, batch_count);
  json_out("xnet_dns", mode, r_batch);

  free(batch_hosts);
  xEventLoopLeave();
  xEventLoopDestroy(loop);

  printf("\n]\n");
  return 0;
}
