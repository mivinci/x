// dns_bench_client.go — DNS resolver benchmark in Go using pure-Go resolver
//
// Usage: GODEBUG=netdns=go go run dns_bench_client.go <local|remote>
//
// Outputs JSON to stdout matching the libx benchmark format.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"sync"
	"time"
)

type Result struct {
	Resolver  string `json:"resolver"`
	Mode      string `json:"mode"`
	Scenario  string `json:"scenario"`
	LatencyUs int64  `json:"latency_us"`
	Queries   int    `json:"queries,omitempty"`
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
		// Custom resolver pointed at local benchmark server
		resolver = &net.Resolver{
			PreferGo: true,
			Dial: func(ctx context.Context, network, address string) (net.Conn, error) {
				d := net.Dialer{}
				return d.DialContext(ctx, "udp", "127.0.0.1:15353")
			},
		}
	} else {
		// Default system resolver, but force pure Go
		resolver = &net.Resolver{
			PreferGo: true,
		}
	}

	singleHost := "bench-0.local"
	batchNames := make([]string, 100)
	for i := 0; i < 100; i++ {
		if localMode {
			batchNames[i] = fmt.Sprintf("bench-%d.local", i)
		} else {
			hosts := []string{
				"google.com", "github.com", "amazon.com", "microsoft.com",
				"apple.com", "netflix.com", "stackoverflow.com", "youtube.com",
				"wikipedia.org", "reddit.com", "twitter.com", "linkedin.com",
				"cloudflare.com", "zoom.us", "dropbox.com", "spotify.com",
				"adobe.com", "oracle.com", "ibm.com", "intel.com",
			}
			batchNames[i] = hosts[i%len(hosts)]
		}
	}
	if !localMode {
		singleHost = "google.com"
	}

	results := []Result{}

	// Single query
	start := time.Now()
	ctx := context.Background()
	_, err := resolver.LookupHost(ctx, singleHost)
	elapsed := time.Since(start).Microseconds()
	_ = err
	results = append(results, Result{
		Resolver: "go", Mode: mode, Scenario: "single_query", LatencyUs: elapsed,
	})

	// Batch query — concurrent goroutines, measure wall time
	start = time.Now()
	var wg sync.WaitGroup
	for _, name := range batchNames {
		wg.Add(1)
		go func(n string) {
			defer wg.Done()
			resolver.LookupHost(ctx, n)
		}(name)
	}
	wg.Wait()
	elapsed = time.Since(start).Microseconds()
	results = append(results, Result{
		Resolver: "go", Mode: mode, Scenario: "batch",
		LatencyUs: elapsed, Queries: len(batchNames),
	})

	// Cache hit (Go may have its own caching)
	start = time.Now()
	resolver.LookupHost(ctx, singleHost)
	elapsed = time.Since(start).Microseconds()
	results = append(results, Result{
		Resolver: "go", Mode: mode, Scenario: "cache_hit", LatencyUs: elapsed,
	})

	// Output JSON
	out, _ := json.MarshalIndent(results, "", "  ")
	fmt.Println(string(out))
}
