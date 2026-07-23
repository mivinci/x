/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h1.c - HTTP/1.1 protocol handler implementation (llhttp)
 */

#include "proto_h1.h"

#include "server_private.h"

#include <llhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal state for HTTP/1.1 protocol handler
 * ═══════════════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xHttpProtoH1) {
  llhttp_t          parser;
  llhttp_settings_t settings;
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  llhttp callbacks
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int on_url(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;
  stream->header_bytes += len;

  if (!stream->url) stream->url = xBufferCreate(256);
  if (!stream->url) return HPE_INTERNAL;
  if (xBufferAppend(&stream->url, at, len) != xErrno_Ok) return HPE_INTERNAL;
  return HPE_OK;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;
  stream->header_bytes += len;

  if (stream->header_bytes > conn->server->max_header_size) {
    stream->pending_error        = 431;
    stream->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  if (!stream->headers_raw) stream->headers_raw = xBufferCreate(512);
  if (!stream->headers_raw) return HPE_INTERNAL;
  if (xBufferAppend(&stream->headers_raw, at, len) != xErrno_Ok) return HPE_INTERNAL;

  if (stream->header_field) {
    xBufferReset(stream->header_field);
  } else {
    stream->header_field = xBufferCreate(128);
    if (!stream->header_field) return HPE_INTERNAL;
  }
  if (xBufferAppend(&stream->header_field, at, len) != xErrno_Ok) return HPE_INTERNAL;

  return HPE_OK;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;
  stream->header_bytes += len;

  if (stream->header_bytes > conn->server->max_header_size) {
    stream->pending_error        = 431;
    stream->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  if (xBufferAppend(&stream->headers_raw, ": ", 2) != xErrno_Ok) return HPE_INTERNAL;
  if (xBufferAppend(&stream->headers_raw, at, len) != xErrno_Ok) return HPE_INTERNAL;
  if (xBufferAppend(&stream->headers_raw, "\r\n", 2) != xErrno_Ok) return HPE_INTERNAL;

  return HPE_OK;
}

static int on_headers_complete(llhttp_t *parser) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  conn->keep_alive = llhttp_should_keep_alive(parser);

  /* Resolve route and call on_request (if no error so far) */
  if (!conn->stream->pending_error) {
    xHttpStreamResolve(conn->stream);
  }

  return HPE_OK;
}

static int on_body(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;

  /* If there's a pending error or request was aborted, skip body */
  if (stream->pending_error || stream->request_aborted) return HPE_OK;

  /* If no route or no on_data callback, discard body */
  if (!stream->route_info || !stream->route_info->on_data) return HPE_OK;

  /* Deliver body chunk to on_data */
  int rc = stream->route_info->on_data(at, len, stream->route_info->arg);
  if (rc != 0) {
    stream->pending_error        = 413;
    stream->pending_error_reason = "Content Too Large";
    return HPE_USER;
  }

  return HPE_OK;
}

static int on_message_complete(llhttp_t *parser) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  conn->stream->request_complete = 1;
  llhttp_pause(parser);

  return HPE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  vtable method implementations
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h1_on_data(struct xHttpConn_ *conn, const char *buf, size_t len,
                       int64_t stream_id) {
  (void)stream_id;
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;

  enum llhttp_errno err = llhttp_execute(&h1->parser, buf, len);

  if (conn->stream->pending_error) {
    return -1;
  }

  if (conn->stream->request_complete) {
    return 1;
  }

  if (err != HPE_OK && err != HPE_PAUSED) {
    return -1;
  }

  return 0;
}

static void h1_reset(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;
  if (h1) {
    llhttp_reset(&h1->parser);
  }
}

static void h1_destroy(struct xHttpConn_ *conn) {
  if (conn->proto.state) {
    free(conn->proto.state);
    conn->proto.state = NULL;
  }
}

static const char *h1_method(struct xHttpStream_ *stream) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)stream->conn->proto.state;
  return llhttp_method_name((llhttp_method_t)h1->parser.method);
}

static int h1_should_keep_alive(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;
  return llhttp_should_keep_alive(&h1->parser);
}

static int h1_send_response(struct xHttpStream_ *stream, int status, struct xHttpHeader_ *headers,
                            const char *body, size_t body_len) {
  struct xHttpConn_ *conn = stream->conn;
  xIOBuffer         *wb   = &conn->write_buf;

  char status_line[64];
  int  slen = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n", status,
                       xHttpStatusReason(status));
  xIOBufferAppend(wb, status_line, (size_t)slen);

  char cl_buf[48];
  int  cl_len = snprintf(cl_buf, sizeof(cl_buf), "Content-Length: %zu\r\n", body_len);
  xIOBufferAppend(wb, cl_buf, (size_t)cl_len);

  if (conn->keep_alive) {
    xIOBufferAppendStr(wb, "Connection: keep-alive\r\n");
  } else {
    xIOBufferAppendStr(wb, "Connection: close\r\n");
  }

  /* User-supplied headers. Skip any "Connection" header the caller may have
   * set via xHttpCtxSetHeader: the keep-alive policy is authoritative
   * (derived from the parser), and emitting two Connection headers would
   * be a protocol violation. */
  struct xHttpHeader_ *h = headers;
  while (h) {
    if (strcasecmp(h->key, "Connection") != 0) {
      xIOBufferAppendStr(wb, h->key);
      xIOBufferAppendStr(wb, ": ");
      xIOBufferAppendStr(wb, h->value);
      xIOBufferAppendStr(wb, "\r\n");
    }
    h = h->next;
  }

  xIOBufferAppendStr(wb, "\r\n");

  if (body && body_len > 0) {
    xIOBufferAppend(wb, body, body_len);
  }

  return 0;
}

static int h1_write_data(struct xHttpStream_ *stream, const char *data, size_t len) {
  struct xHttpConn_           *conn = stream->conn;
  struct xHttpResponseWriter_ *w    = &stream->writer;

  if (!w->streaming) {
    w->streaming     = 1;
    conn->keep_alive = 0;

    xIOBuffer *wb = &conn->write_buf;

    char status_line[64];
    int  slen = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n", w->status_code,
                         xHttpStatusReason(w->status_code));
    xIOBufferAppend(wb, status_line, (size_t)slen);

    xIOBufferAppendStr(wb, "Connection: close\r\n");

    /* Skip user-supplied "Connection" header — see h1_send_response. */
    struct xHttpHeader_ *h = w->headers;
    while (h) {
      if (strcasecmp(h->key, "Connection") != 0) {
        xIOBufferAppendStr(wb, h->key);
        xIOBufferAppendStr(wb, ": ");
        xIOBufferAppendStr(wb, h->value);
        xIOBufferAppendStr(wb, "\r\n");
      }
      h = h->next;
    }

    xIOBufferAppendStr(wb, "\r\n");
  }

  if (data && len > 0) {
    xIOBufferAppend(&conn->write_buf, data, len);
  }

  return 0;
}

static int h1_end_stream(struct xHttpStream_ *stream) {
  stream->writer.sent = 1;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════
 */

int xHttpProtoH1Init(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)calloc(1, sizeof(xHttpProtoH1));
  if (!h1) return -1;

  conn->stream = xHttpStreamCreate(conn, 0);
  if (!conn->stream) {
    free(h1);
    return -1;
  }

  memset(&h1->settings, 0, sizeof(h1->settings));
  h1->settings.on_url              = on_url;
  h1->settings.on_header_field     = on_header_field;
  h1->settings.on_header_value     = on_header_value;
  h1->settings.on_headers_complete = on_headers_complete;
  h1->settings.on_body             = on_body;
  h1->settings.on_message_complete = on_message_complete;

  llhttp_init(&h1->parser, HTTP_REQUEST, &h1->settings);
  h1->parser.data = conn;

  conn->proto.on_data           = h1_on_data;
  conn->proto.reset             = h1_reset;
  conn->proto.destroy           = h1_destroy;
  conn->proto.method            = h1_method;
  conn->proto.should_keep_alive = h1_should_keep_alive;
  conn->proto.send_response     = h1_send_response;
  conn->proto.write_data        = h1_write_data;
  conn->proto.end_stream        = h1_end_stream;
  conn->proto.state             = h1;

  return 0;
}
