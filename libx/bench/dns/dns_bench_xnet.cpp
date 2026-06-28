/*
 * dns_bench_xnet.cpp — DNS resolver benchmarks for xnet/dns (getaddrinfo pool)
 *
 * Usage: dns_bench_xnet <local|remote>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>
#include <string>

extern "C" {
#include <x/base/event.h>
#include <x/net/dns.h>
}

using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;

static us bench_single(const char *name) {
  xEventLoop loop = xEventLoopCurrent();
  Clock::time_point  start = Clock::now();

  xDnsResolve(name, nullptr, nullptr,
    [](xDnsResult *result, void *arg) {
      xDnsResultFree(result);
      (void)arg;
      xEventLoopStop(xEventLoopCurrent());
    }, nullptr);

  xEventLoopRun(loop, X_RUN_DEFAULT);

  return std::chrono::duration_cast<us>(Clock::now() - start);
}

static us bench_batch(const char **names, int count) {
  std::atomic<int> pending{count};
  xEventLoop loop = xEventLoopCurrent();
  Clock::time_point start = Clock::now();

  for (int i = 0; i < count; i++) {
    xDnsResolve(names[i], nullptr, nullptr,
      [](xDnsResult *result, void *arg) {
        xDnsResultFree(result);
        std::atomic<int> *p = (std::atomic<int> *)arg;
        if (--*p == 0) xEventLoopStop(xEventLoopCurrent());
      }, &pending);
  }

  xEventLoopRun(loop, X_RUN_DEFAULT);

  return std::chrono::duration_cast<us>(Clock::now() - start);
}

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

int main(int argc, char **argv) {
  bool local = true;
  if (argc > 1 && strcmp(argv[1], "remote") == 0) local = false;
  const char *mode = local ? "local" : "remote";

  xEventLoop loop = xEventLoopCreate();
  if (!loop) return 1;
  xEventLoopEnter(loop);

  const char *single_host = local ? "bench-0.local" : "google.com";

  /* Build batch host list */
  int batch_count = local ? 100 : 20;
  const char **batch_hosts = (const char **)malloc((size_t)batch_count * sizeof(char *));
  if (local) {
    for (int i = 0; i < 100; i++) {
      char *buf = (char *)malloc(64);
      snprintf(buf, 64, "bench-%d.local", i);
      batch_hosts[i] = buf;
    }
  } else {
    static const char *hosts[] = {
      "google.com","github.com","amazon.com","microsoft.com",
      "apple.com","netflix.com","stackoverflow.com","youtube.com",
      "wikipedia.org","reddit.com","twitter.com","linkedin.com",
      "cloudflare.com","zoom.us","dropbox.com","spotify.com",
      "adobe.com","oracle.com","ibm.com","intel.com",
    };
    batch_count = (int)(sizeof(hosts) / sizeof(hosts[0]));
    for (int i = 0; i < batch_count; i++) batch_hosts[i] = hosts[i];
  }

  auto us_single = bench_single(single_host);
  json_out("xnet_dns", mode, "single_query", us_single.count());

  auto us_batch = bench_batch(batch_hosts, batch_count);
  json_out("xnet_dns", mode, "batch", us_batch.count(), batch_count);

  if (local) for (int i = 0; i < 100; i++) free((void *)batch_hosts[i]);
  free(batch_hosts);

  xEventLoopLeave();
  xEventLoopDestroy(loop);

  printf("\n]\n");
  return 0;
}
