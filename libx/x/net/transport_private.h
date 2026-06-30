/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_private.h - Private transport initialization functions
 *
 * This header is intended for cross-module use within the libx project
 * (e.g. xhttp consuming xnet). It is NOT part of the public API and
 * should not be included by external users.
 *
 * NOTE: These functions are declared with XCAPI (not XCAPI_LOCAL) because
 * they are called across module boundaries — e.g. xhttp calls
 * xTransportPlainInit from server.c. Under -fvisibility=hidden,
 * XCAPI_LOCAL would hide them inside libxnet.dylib and break cross-module
 * linking. "Private but cross-module" symbols must be exported from their
 * owning module's shared library so consumers can link against them.
 * Privacy is enforced by convention (don't include this header from
 * external code), not by visibility.
 */

#ifndef XNET_TRANSPORT_PRIVATE_H
#define XNET_TRANSPORT_PRIVATE_H

#include "transport.h"

/* ───────────────────── Transport initialization ───────────────────── */

/**
 * Initialize a Plain TCP transport for the given file descriptor.
 * The transport's read/writev map directly to read(2)/writev(2).
 * handshake and alpn are set to NULL.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param fd         File descriptor for the connection.
 */
XCAPI(void) xTransportPlainInit(xTransport *transport, int fd);

/**
 * Initialize a TLS server transport for the given file descriptor.
 * Creates an SSL object in accept mode using the shared server TLS context.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param tls_ctx    Server TLS context from xTlsCtxCreate() (must not be NULL).
 * @param fd         File descriptor for the accepted connection.
 */
XCAPI(void) xTransportTlsServerInit(xTransport *transport, xTlsCtx tls_ctx, int fd);

/**
 * Initialize a TLS client transport for the given file descriptor.
 * Uses the shared TLS context to create a per-connection SSL object
 * in connect mode.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param tls_ctx    Client TLS context from xTlsCtxCreate() (must not be NULL).
 * @param hostname   Server hostname for SNI and verification.
 * @param fd         File descriptor for the TCP connection.
 * @return           0 on success, -1 on error.
 */
XCAPI(int) xTransportTlsClientInit(xTransport *transport, xTlsCtx tls_ctx, const char *hostname, int fd);

#endif /* XNET_TRANSPORT_PRIVATE_H */
