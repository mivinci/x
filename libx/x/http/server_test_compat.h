/*
 * server_test_compat.h — Legacy push-model declarations for test code.
 *
 * These functions exist ONLY in server.c (NOT in the public server.h).
 * Include this header in test files that haven't been migrated to the
 * pull model (on_request + on_read) yet.
 *
 * DO NOT include in production code or new tests.
 */

#ifndef XHTTP_SERVER_TEST_COMPAT_H
#define XHTTP_SERVER_TEST_COMPAT_H

#include <x/http/server.h>  // xHttpCtx, xErrno, etc.

#ifdef __cplusplus
extern "C" {
#endif

xErrno xHttpCtxSend(xHttpCtx *ctx, const char *body, size_t body_len);
xErrno xHttpCtxWrite(xHttpCtx *ctx, const char *data, size_t len);
xErrno xHttpCtxEndStream(xHttpCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* XHTTP_SERVER_TEST_COMPAT_H */
