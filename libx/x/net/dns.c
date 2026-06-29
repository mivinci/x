/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns.c - Asynchronous DNS resolution via thread-pool offload
 */

#include <x/net/dns.h>

#include <x/base/atomic.h>

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal types ───────────────────── */

/**
 * @brief Internal request state, shared between the submitting thread,
 *        the worker thread, and the done callback.
 */
struct xDnsRequest_ {
  /* Inputs (immutable after creation) */
  char           *hostname;
  char           *service;
  struct addrinfo hints;

  /* Callback (immutable after creation) */
  xDnsCallback callback;
  void        *arg;

  /* Work handle for xWorkCancel() */
  xWork work;
};

/* ───────────────────── Error mapping ───────────────────── */

/**
 * @brief Map getaddrinfo EAI_* error codes to xErrno.
 */
static xErrno eai_to_xerrno(int eai) {
  switch (eai) {
  case 0:
    return xErrno_Ok;
  case EAI_NONAME:
    return xErrno_DnsNotFound;
  case EAI_AGAIN:
    return xErrno_DnsTempFail;
  case EAI_MEMORY:
    return xErrno_NoMemory;
  default:
    return xErrno_DnsError;
  }
}

/* ───────────────────── Result helpers ───────────────────── */

/**
 * @brief Build an xDnsResult from a getaddrinfo result chain.
 *
 * On success, returns a heap-allocated xDnsResult with error == xErrno_Ok
 * and a linked list of xDnsAddr nodes. On failure (OOM while building the
 * list), returns a result with error == xErrno_NoMemory and addrs == NULL.
 */
static xDnsResult *result_from_addrinfo(struct addrinfo *res) {
  xDnsResult *result = (xDnsResult *)calloc(1, sizeof(*result));
  if (!result) return NULL;

  result->error = xErrno_Ok;
  result->addrs = NULL;

  xDnsAddr **tail = &result->addrs;

  for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
    xDnsAddr *node = (xDnsAddr *)calloc(1, sizeof(*node));
    if (!node) {
      /* OOM: free what we've built so far */
      xDnsResultFree(result);
      xDnsResult *err_result = (xDnsResult *)calloc(1, sizeof(*err_result));
      if (err_result) {
        err_result->error = xErrno_NoMemory;
        err_result->addrs = NULL;
      }
      return err_result;
    }

    memcpy(&node->addr, ai->ai_addr, ai->ai_addrlen);
    node->addrlen  = ai->ai_addrlen;
    node->family   = ai->ai_family;
    node->socktype = ai->ai_socktype;
    node->protocol = ai->ai_protocol;
    node->next     = NULL;

    *tail = node;
    tail  = &node->next;
  }

  return result;
}

/**
 * @brief Build an error-only xDnsResult (no addresses).
 */
static xDnsResult *result_error(xErrno err) {
  xDnsResult *result = (xDnsResult *)calloc(1, sizeof(*result));
  if (!result) return NULL;
  result->error = err;
  result->addrs = NULL;
  return result;
}

/* ───────────────────── IP literal detection ───────────────────── */

/**
 * @brief Check if hostname is an IP address literal (IPv4 or IPv6).
 *
 * If it is, set AI_NUMERICHOST in the hints to skip actual DNS lookup.
 */
static void detect_ip_literal(const char *hostname, struct addrinfo *hints) {
  unsigned char buf[sizeof(struct in6_addr)];

  if (inet_pton(AF_INET, hostname, buf) == 1 || inet_pton(AF_INET6, hostname, buf) == 1) {
    hints->ai_flags |= AI_NUMERICHOST;
  }
}

/* ───────────────────── Thread-pool callbacks ───────────────────── */

/**
 * @brief Worker function executed on a thread-pool thread.
 *
 * Calls getaddrinfo() and converts the result to an xDnsResult.
 * The returned pointer is passed as `result` to the done callback.
 */
static void *dns_work_fn(void *arg) {
  struct xDnsRequest_ *req = (struct xDnsRequest_ *)arg;

  /* Detect IP literals and set AI_NUMERICHOST to avoid DNS lookup */
  detect_ip_literal(req->hostname, &req->hints);

  struct addrinfo *res = NULL;
  int              eai = getaddrinfo(req->hostname, req->service, &req->hints, &res);

  xDnsResult *result;
  if (eai != 0) {
    result = result_error(eai_to_xerrno(eai));
  } else {
    result = result_from_addrinfo(res);
    freeaddrinfo(res);
    if (!result) {
      result = result_error(xErrno_NoMemory);
    }
  }

  return (void *)result;
}

/**
 * @brief Done callback invoked on the event loop thread.
 */
static void dns_done_fn(void *arg, void *work_result) {
  struct xDnsRequest_ *req    = (struct xDnsRequest_ *)arg;
  xDnsResult          *result = (xDnsResult *)work_result;

  if (!result) result = result_error(xErrno_NoMemory);
  req->callback(result, req->arg);

  free(req->hostname);
  free(req->service);
  free(req);
}

/**
 * @brief Cancel cleanup — frees worker-allocated result and req.
 *
 * Called on the event loop thread after the worker finishes (or
 * immediately if the task was cancelled before it started).  This
 * is the ONLY place req should be freed when cancelled — freeing it
 * synchronously in xDnsCancel would race with the worker thread.
 */
static void dns_on_cancel(void *arg, void *result) {
  struct xDnsRequest_ *req = (struct xDnsRequest_ *)arg;
  if (result) xDnsResultFree((xDnsResult *)result);
  free(req->hostname);
  free(req->service);
  free(req);
}

/* ───────────────────── Public API ───────────────────── */

xDnsQuery xDnsResolve( const char *hostname, const char *service,
                      const struct addrinfo *hints, xDnsCallback callback, void *arg) {
  xEventLoop loop = xEventLoopCurrent();
  if (!loop || !hostname || !hostname[0] || !callback) {
    return NULL;
  }

  struct xDnsRequest_ *req = (struct xDnsRequest_ *)calloc(1, sizeof(*req));
  if (!req) return NULL;

  req->hostname = strdup(hostname);
  if (!req->hostname) goto fail;

  if (service) {
    req->service = strdup(service);
    if (!req->service) goto fail;
  }

  if (hints) {
    req->hints = *hints;
  } else {
    /* Default hints: AF_UNSPEC + SOCK_STREAM */
    memset(&req->hints, 0, sizeof(req->hints));
    req->hints.ai_family   = AF_UNSPEC;
    req->hints.ai_socktype = SOCK_STREAM;
  }

  req->callback  = callback;
  req->arg       = arg;

  req->work = xWorkSubmit(NULL, dns_work_fn, dns_done_fn, dns_on_cancel, req);
  if (!req->work) goto fail;

  return (xDnsQuery)req;

fail:
  if (req) {
    free(req->hostname);
    free(req->service);
    free(req);
  }
  return NULL;
}

void xDnsCancel(xDnsQuery query) {
  xEventLoop loop = xEventLoopCurrent();
  if (!loop || !query) return;

  struct xDnsRequest_ *req = (struct xDnsRequest_ *)query;

  /* Mark the work as cancelled.  req is freed by dns_on_cancel (or
   * dns_done_fn if the work already completed) on the event loop thread
   * — NOT here, because the worker thread may still be accessing it. */
  xWorkCancel(req->work);
}

void xDnsResultFree(xDnsResult *result) {
  if (!result) return;

  xDnsAddr *addr = result->addrs;
  while (addr) {
    xDnsAddr *next = addr->next;
    free(addr);
    addr = next;
  }
  free(result);
}
