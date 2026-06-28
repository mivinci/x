// dns_bench_client.go — DNS resolver benchmark in Go using pure-Go resolver
//
// Usage: GODEBUG=netdns=go go run dns_bench_client.go <local|remote>
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

type Result struct {
	Resolver  string `json:"resolver"`
	Mode      string `json:"mode"`
	Scenario  string `json:"scenario"`
	LatencyUs int64  `json:"latency_us"`
	Queries   int    `json:"queries,omitempty"`
	Success   int    `json:"success"`
	Failed    int    `json:"failed"`
}

var remoteHosts = []string{
	"google.com", "github.com", "amazon.com", "microsoft.com",
	"apple.com", "netflix.com", "stackoverflow.com", "youtube.com",
	"wikipedia.org", "reddit.com", "twitter.com", "linkedin.com",
	"cloudflare.com", "zoom.us", "dropbox.com", "spotify.com",
	"adobe.com", "oracle.com", "ibm.com", "intel.com",
}

func benchSingle(resolver *net.Resolver, name string) (elapsed int64, success bool) {
	start := time.Now()
	_, err := resolver.LookupHost(context.Background(), name)
	return time.Since(start).Microseconds(), err == nil
}

func main() {
	localMode := true
	if len(os.Args) > 1 && os.Args[1] == "remote" {
		localMode = false
	}
	mode := "local"
	if !localMode {
		mode = "remote"
	}

	var resolver *net.Resolver
	if localMode {
		resolver = &net.Resolver{
			PreferGo: true,
			Dial: func(ctx context.Context, network, address string) (net.Conn, error) {
				d := net.Dialer{}
				return d.DialContext(ctx, "udp", "127.0.0.1:15353")
			},
		}
	} else {
		resolver = &net.Resolver{PreferGo: true}
	}

	singleHost := "bench-0.local"
	if !localMode {
		singleHost = "google.com"
	}

	// Build batch names
	var batchNames []string
	batchCount := 100
	if !localMode {
		batchCount = len(remoteHosts)
	}
	for i := 0; i < batchCount; i++ {
		if localMode {
			batchNames = append(batchNames, fmt.Sprintf("bench-%d.local", i))
		} else {
			batchNames = append(batchNames, remoteHosts[i%len(remoteHosts)])
		}
	}

	results := []Result{}

	// Warmup: prime resolver cache with same domain as measurement
	benchSingle(resolver, singleHost)

	// Single query
	us, ok := benchSingle(resolver, singleHost)
	s, f := 0, 0
	if ok { s = 1 } else { f = 1 }
	results = append(results, Result{
		Resolver: "go", Mode: mode, Scenario: "single_query",
		LatencyUs: us, Queries: 1, Success: s, Failed: f,
	})

	// Batch — concurrent goroutines, measure wall time (matches xdns/xnet/cares)
	var wg sync.WaitGroup
	var okCount, failCount int64
	start := time.Now()
	for _, name := range batchNames {
		wg.Add(1)
		go func(n string) {
			defer wg.Done()
			_, err := resolver.LookupHost(context.Background(), n)
			if err == nil {
				atomic.AddInt64(&okCount, 1)
			} else {
				atomic.AddInt64(&failCount, 1)
			}
		}(name)
	}
	wg.Wait()
	elapsed := time.Since(start).Microseconds()
	results = append(results, Result{
		Resolver: "go", Mode: mode, Scenario: "batch",
		LatencyUs: elapsed, Queries: len(batchNames),
		Success: int(okCount), Failed: int(failCount),
	})

	// Cache hit
	us, ok = benchSingle(resolver, singleHost)
	s, f = 0, 0
	if ok { s = 1 } else { f = 1 }
	results = append(results, Result{
		Resolver: "go", Mode: mode, Scenario: "cache_hit",
		LatencyUs: us, Queries: 1, Success: s, Failed: f,
	})

	out, _ := json.MarshalIndent(results, "", "  ")
	fmt.Println(string(out))
}
