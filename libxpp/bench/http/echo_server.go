/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * echo_server.go - Go net/http echo server for comparison benchmarking
 *
 * Mirrors echo_server.cpp (xpp::http::Server) route-for-route so wrk
 * numbers are directly comparable:
 *   GET  /ping        → 200 "pong"           (minimal response — pure QPS)
 *   GET  /echo?size=N → 200 with N bytes of 'x' (payload throughput)
 *   POST /echo        → 200 echoing the request body
 *
 * Usage: go run echo_server.go [port]
 *   Default port: 8080
 */

package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
)

func handlePing(w http.ResponseWriter, r *http.Request) {
	w.Write([]byte("pong"))
}

func handleEcho(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, "read body failed", http.StatusInternalServerError)
			return
		}
		w.Write(body)
		return
	}

	// GET: parse ?size=N from URL
	size := 64 // default
	if s := r.URL.Query().Get("size"); s != "" {
		if n, err := strconv.Atoi(s); err == nil && n > 0 {
			size = n
			if size > 1048576 { // cap at 1MB
				size = 1048576
			}
		}
	}

	w.Write(bytes.Repeat([]byte("x"), size))
}

func main() {
	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/ping", handlePing)
	mux.HandleFunc("/echo", handleEcho)

	addr := "0.0.0.0:" + port
	fmt.Printf("Go echo server listening on %s\n", addr)
	fmt.Printf("  GET  /ping        → 200 \"pong\"\n")
	fmt.Printf("  GET  /echo?size=N → 200 with N bytes\n")
	fmt.Printf("  POST /echo        → 200 echo body\n")

	if err := http.ListenAndServe(addr, mux); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}
}
