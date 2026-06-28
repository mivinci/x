/*
 * dns_bench_xdns.cpp — DNS resolver benchmarks for xdns (protocol-native)
 *
 * Usage: dns_bench_xdns <local|remote>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>

extern "C" {
#include <x/base/event.h>
#include <x/dns/dns.h>
}

using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;

/* ─────────────────── Helpers ─────────────────── */

static void json_out(const char *resolver, const char *mode,
                     const char *scenario, long long us_val,
                     int queries = 0) {
  static int first = 1;
  if (first) { printf("[\n"); first = 0; }
  else printf(",\n");

  printf("  {\"resolver\":\"%s\",\"mode\":\"%s\",\"scenario\":\"%s\",\"latency_us\":%lld",
         resolver, mode, scenario, us_val);
  if (queries > 0) printf(",\"queries\":%d", queries);
  printf("}");
}

static us bench_single(xDnsClient client, const char *name) {
  xEventLoop loop = xEventLoopCurrent();
  Clock::time_point start = Clock::now();

  xDnsClientDo(client, name, xDnsType_A,
    [](xErrno err, const xDnsRecord *records, void *arg) {
      (void)err; (void)records; (void)arg;
      xEventLoopStop(xEventLoopCurrent());
    }, nullptr);

  xEventLoopRun(loop, X_RUN_DEFAULT);

  return std::chrono::duration_cast<us>(Clock::now() - start);
}

static us bench_batch(xDnsClient client, const char **names, int count) {
  std::atomic<int> pending{count};
  xEventLoop loop = xEventLoopCurrent();
  Clock::time_point start = Clock::now();

  for (int i = 0; i < count; i++) {
    xDnsClientDo(client, names[i], xDnsType_A,
      [](xErrno err, const xDnsRecord *records, void *arg) {
        (void)err; (void)records;
        std::atomic<int> *p = (std::atomic<int> *)arg;
        if (--*p == 0) xEventLoopStop(xEventLoopCurrent());
      }, &pending);
  }

  xEventLoopRun(loop, X_RUN_DEFAULT);

  return std::chrono::duration_cast<us>(Clock::now() - start);
}

static us bench_cache(xDnsClient client, const char *name) {
  bench_single(client, name); /* cold */

  xEventLoop loop = xEventLoopCurrent();
  Clock::time_point start = Clock::now();

  xDnsClientDo(client, name, xDnsType_A,
    [](xErrno err, const xDnsRecord *records, void *arg) {
      (void)err; (void)records; (void)arg;
      xEventLoopStop(xEventLoopCurrent());
    }, nullptr);

  xEventLoopRun(loop, X_RUN_DEFAULT);

  return std::chrono::duration_cast<us>(Clock::now() - start);
}

static const char *remote_hosts[] = {
  "google.com","github.com","amazon.com","microsoft.com",
  "apple.com","netflix.com","stackoverflow.com","youtube.com",
  "wikipedia.org","reddit.com","twitter.com","linkedin.com",
  "cloudflare.com","zoom.us","dropbox.com","spotify.com",
  "adobe.com","oracle.com","ibm.com","intel.com",
};
static const int n_remote_hosts = (int)(sizeof(remote_hosts) / sizeof(remote_hosts[0]));

int main(int argc, char **argv) {
  bool local = true;
  if (argc > 1 && strcmp(argv[1], "remote") == 0) local = false;
  const char *mode = local ? "local" : "remote";

  xEventLoop loop = xEventLoopCreate();
  if (!loop) return 1;
  xEventLoopEnter(loop);

  xDnsClientConf conf = {};
  conf.enable_cache = 1;
  if (local) {
    conf.nameservers[0] = "127.0.0.1:15353";
    conf.timeout_ms = 2000;
    conf.retries = 1;
  } else {
    conf.nameservers[0] = "8.8.8.8";
    conf.timeout_ms = 5000;
    conf.retries = 2;
  }

  xDnsClient client = xDnsClientCreate(&conf);
  if (!client) { fprintf(stderr, "Failed to create xdns client\n"); return 1; }

  const char *single_host = local ? "bench-0.local" : "google.com";

  /* Build batch names */
  int batch_count = local ? 100 : n_remote_hosts;
  const char **batch_names = (const char **)malloc((size_t)batch_count * sizeof(char *));
  if (local) {
    for (int i = 0; i < 100; i++) {
      char *buf = (char *)malloc(64);
      snprintf(buf, 64, "bench-%d.local", i);
      batch_names[i] = buf;
    }
  } else {
    for (int i = 0; i < batch_count; i++) batch_names[i] = remote_hosts[i];
  }

  auto us_single = bench_single(client, single_host);
  json_out("xdns", mode, "single_query", us_single.count());

  auto us_batch = bench_batch(client, batch_names, batch_count);
  json_out("xdns", mode, "batch", us_batch.count(), batch_count);

  auto us_cache = bench_cache(client, single_host);
  json_out("xdns", mode, "cache_hit", us_cache.count());

  if (local) for (int i = 0; i < 100; i++) free((void *)batch_names[i]);
  free(batch_names);

  xDnsClientDestroy(client);
  xEventLoopLeave();
  xEventLoopDestroy(loop);

  printf("\n]\n");
  return 0;
}
