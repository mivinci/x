/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h2.c - HTTP/2 protocol handler implementation (nghttp2)
 */

#include "proto_h2.h"

#include "server_private.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp2/nghttp2.h>

#include <x/base/log.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal state for HTTP/2 protocol handler
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define XHTTP_H2_MAX_PENDING_DISPATCH 16

XDEF_STRUCT(xHttpProtoH2) {
  nghttp2_session     *session;
  struct xHttpStream_ *pending_dispatch[XHTTP_H2_MAX_PENDING_DISPATCH];
  int                  pending_count;
};

XDEF_STRUCT(xH2StreamData) {
  struct xHttpStream_ *stream;
  char                *method;
  xIOBuffer            stream_buf;
  int                  stream_eof;
  int                  buf_initialized;
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  nghttp2 callbacks
 * ═══════════════════════════════════════════════════════════════════════════
 */

static ssize_t h2_send_callback(nghttp2_session *session, const uint8_t *data, size_t length,
                                int flags, void *user_data) {
  (void)session;
  (void)flags;
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  xIOBufferAppend(&conn->write_buf, (const char *)data, length);
  return (ssize_t)length;
}

static int h2_on_begin_headers_callback(nghttp2_session *session, const nghttp2_frame *frame,
                                        void *user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;

  if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  struct xHttpStream_ *stream = xHttpStreamCreate(conn, frame->hd.stream_id);
  if (!stream) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  xH2StreamData *sd = (xH2StreamData *)calloc(1, sizeof(xH2StreamData));
  if (!sd) {
    xHttpStreamDestroy(stream);
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  sd->stream = stream;
  sd->method = NULL;

  nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, sd);

  conn->stream = stream;

  return 0;
}

static int h2_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame,
                                 const uint8_t *name, size_t namelen, const uint8_t *value,
                                 size_t valuelen, uint8_t flags, void *user_data) {
  (void)flags;
  (void)user_data;

  if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  xH2StreamData *sd =
    (xH2StreamData *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
  if (!sd) return 0;

  struct xHttpStream_ *stream = sd->stream;

  if (namelen > 0 && name[0] == ':') {
    if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
      free(sd->method);
      sd->method = strndup((const char *)value, valuelen);
    } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
      if (!stream->url) stream->url = xBufferCreate(256);
      if (!stream->url) return NGHTTP2_ERR_CALLBACK_FAILURE;
      xBufferReset(stream->url);
      if (xBufferAppend(&stream->url, (const char *)value, valuelen) != xErrno_Ok)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  } else {
    if (!stream->headers_raw) stream->headers_raw = xBufferCreate(512);
    if (!stream->headers_raw) return NGHTTP2_ERR_CALLBACK_FAILURE;

    if (xBufferAppend(&stream->headers_raw, (const char *)name, namelen) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (xBufferAppend(&stream->headers_raw, ": ", 2) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (xBufferAppend(&stream->headers_raw, (const char *)value, valuelen) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (xBufferAppend(&stream->headers_raw, "\r\n", 2) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  stream->header_bytes += namelen + valuelen;

  return 0;
}

static int h2_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                          int32_t stream_id, const uint8_t *data, size_t len,
                                          void *user_data) {
  (void)flags;
  (void)user_data;

  xH2StreamData *sd = (xH2StreamData *)nghttp2_session_get_stream_user_data(session, stream_id);
  if (!sd) return 0;

  struct xHttpStream_ *stream = sd->stream;

  /* If there's a pending error or request was aborted, skip body */
  if (stream->pending_error || stream->request_aborted) return 0;

  /* If no route or no on_data callback, discard body */
  if (!stream->route_info || !stream->route_info->on_data) return 0;

  int rc = stream->route_info->on_data((const char *)data, len, stream->route_info->arg);
  if (rc != 0) {
    stream->pending_error        = 413;
    stream->pending_error_reason = "Content Too Large";
  }

  return 0;
}

static int h2_on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame,
                                     void *user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  xHttpProtoH2      *h2   = (xHttpProtoH2 *)conn->proto.state;

  switch (frame->hd.type) {
  case NGHTTP2_HEADERS:
    /* Resolve route after headers are complete (if not already done) */
    {
      xH2StreamData *sd =
        (xH2StreamData *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
      if (sd && sd->stream && !sd->stream->on_request_done && !sd->stream->pending_error) {
        xHttpStreamResolve(sd->stream);
      }
    }
    /* Fall through to check END_STREAM */
    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      xH2StreamData *sd =
        (xH2StreamData *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
      if (sd && sd->stream) {
        sd->stream->request_complete = 1;
        if (h2->pending_count < XHTTP_H2_MAX_PENDING_DISPATCH) {
          h2->pending_dispatch[h2->pending_count++] = sd->stream;
        }
      }
    }
    break;
  case NGHTTP2_DATA:
    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      xH2StreamData *sd =
        (xH2StreamData *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
      if (sd && sd->stream) {
        sd->stream->request_complete = 1;
        if (h2->pending_count < XHTTP_H2_MAX_PENDING_DISPATCH) {
          h2->pending_dispatch[h2->pending_count++] = sd->stream;
        }
      }
    }
    break;
  default:
    break;
  }

  return 0;
}

static int h2_on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                       uint32_t error_code, void *user_data) {
  (void)error_code;
  (void)user_data;

  xH2StreamData *sd = (xH2StreamData *)nghttp2_session_get_stream_user_data(session, stream_id);
  if (sd) {
    if (sd->stream) {
      struct xHttpConn_ *conn = sd->stream->conn;
      if (conn->stream == sd->stream) {
        sd->stream->closed_by_peer = 1;
      } else {
        xHttpStreamDestroy(sd->stream);
      }
      sd->stream = NULL;
    }
    if (sd->buf_initialized) {
      xIOBufferDeinit(&sd->stream_buf);
    }
    free(sd->method);
    free(sd);
    nghttp2_session_set_stream_user_data(session, stream_id, NULL);
  }

  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  vtable method implementations
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h2_on_data(struct xHttpConn_ *conn, const char *buf, size_t len) {
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)conn->proto.state;

  h2->pending_count = 0;

  ssize_t rv = nghttp2_session_mem_recv(h2->session, (const uint8_t *)buf, len);
  if (rv < 0) {
    xLog(false, "xhttp h2: nghttp2_session_mem_recv error: %s", nghttp2_strerror((int)rv));
    return -1;
  }

  rv = nghttp2_session_send(h2->session);
  if (rv != 0) {
    xLog(false, "xhttp h2: nghttp2_session_send error: %s", nghttp2_strerror((int)rv));
    return -1;
  }

  /* Dispatch all completed streams (safe: outside mem_recv) */
  for (int i = 0; i < h2->pending_count; i++) {
    struct xHttpStream_ *stream = h2->pending_dispatch[i];
    if (stream) {
      conn->stream = stream;
      xHttpConnDispatchRequest(conn);
    }
  }
  h2->pending_count = 0;

  rv = nghttp2_session_send(h2->session);
  if (rv != 0) {
    xLog(false, "xhttp h2: nghttp2_session_send (post-dispatch) error: %s",
         nghttp2_strerror((int)rv));
    return -1;
  }

  xHttpConnTryFlush(conn);

  return 0;
}

static void h2_reset(struct xHttpConn_ *conn) {
  (void)conn;
}

static void h2_destroy(struct xHttpConn_ *conn) {
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)conn->proto.state;
  if (h2) {
    if (h2->session) {
      nghttp2_session_del(h2->session);
      h2->session = NULL;
    }
    free(h2);
    conn->proto.state = NULL;
  }
}

static const char *h2_method(struct xHttpStream_ *stream) {
  if (!stream || !stream->conn) return "GET";

  xHttpProtoH2 *h2 = (xHttpProtoH2 *)stream->conn->proto.state;
  if (!h2 || !h2->session) return "GET";

  xH2StreamData *sd =
    (xH2StreamData *)nghttp2_session_get_stream_user_data(h2->session, stream->stream_id);
  if (sd && sd->method) {
    return sd->method;
  }
  return "GET";
}

static int h2_should_keep_alive(struct xHttpConn_ *conn) {
  (void)conn;
  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  H2 response serialization
 * ══════════════════════════════════════════════���════════════════════════════
 */

XDEF_STRUCT(h2_body_source) {
  char  *data;
  size_t len;
  size_t pos;
};

static ssize_t h2_body_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                                     size_t length, uint32_t *data_flags,
                                     nghttp2_data_source *source, void *user_data) {
  (void)session;
  (void)stream_id;
  (void)user_data;
  h2_body_source *src = (h2_body_source *)source->ptr;

  size_t remaining = src->len - src->pos;
  size_t to_copy   = remaining < length ? remaining : length;

  if (to_copy > 0) {
    memcpy(buf, src->data + src->pos, to_copy);
    src->pos += to_copy;
  }

  if (src->pos >= src->len) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    free(src->data);
    src->data = NULL;
    free(src);
  }

  return (ssize_t)to_copy;
}

static int h2_send_response(struct xHttpStream_ *stream, int status, struct xHttpHeader_ *headers,
                            const char *body, size_t body_len) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH2      *h2   = (xHttpProtoH2 *)conn->proto.state;

  int                  nheader = 1;
  struct xHttpHeader_ *h       = headers;
  while (h) {
    nheader++;
    h = h->next;
  }

  nghttp2_nv *nva = (nghttp2_nv *)calloc((size_t)nheader, sizeof(nghttp2_nv));
  if (!nva) return -1;

  char status_str[16];
  int  status_len = snprintf(status_str, sizeof(status_str), "%d", status);

  nva[0].name     = (uint8_t *)":status";
  nva[0].namelen  = 7;
  nva[0].value    = (uint8_t *)status_str;
  nva[0].valuelen = (size_t)status_len;
  nva[0].flags    = NGHTTP2_NV_FLAG_NO_COPY_NAME;

  int i = 1;
  h     = headers;
  while (h) {
    for (char *p = h->key; *p; p++)
      *p = (char)tolower((unsigned char)*p);
    nva[i].name     = (uint8_t *)h->key;
    nva[i].namelen  = strlen(h->key);
    nva[i].value    = (uint8_t *)h->value;
    nva[i].valuelen = strlen(h->value);
    nva[i].flags    = NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE;
    i++;
    h = h->next;
  }

  int rv;
  if (body && body_len > 0) {
    h2_body_source *src = (h2_body_source *)malloc(sizeof(h2_body_source));
    if (!src) {
      free(nva);
      return -1;
    }
    src->data = (char *)malloc(body_len);
    if (!src->data) {
      free(src);
      free(nva);
      return -1;
    }
    memcpy(src->data, body, body_len);
    src->len = body_len;
    src->pos = 0;

    nghttp2_data_provider data_prd;
    data_prd.source.ptr    = src;
    data_prd.read_callback = h2_body_read_callback;

    rv = nghttp2_submit_response(h2->session, stream->stream_id, nva, (size_t)nheader, &data_prd);
    free(nva);
    if (rv != 0) {
      free(src);
      return -1;
    }
  } else {
    rv = nghttp2_submit_response(h2->session, stream->stream_id, nva, (size_t)nheader, NULL);
    free(nva);
    if (rv != 0) return -1;
  }

  return 0;
}

static ssize_t h2_streaming_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                                          size_t length, uint32_t *data_flags,
                                          nghttp2_data_source *source, void *user_data) {
  (void)session;
  (void)stream_id;
  (void)user_data;
  xH2StreamData *sd = (xH2StreamData *)source->ptr;

  size_t avail = xIOBufferLen(&sd->stream_buf);
  if (avail == 0) {
    if (sd->stream_eof) {
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
      return 0;
    }
    return NGHTTP2_ERR_DEFERRED;
  }

  size_t to_copy = avail < length ? avail : length;
  xIOBufferRead(&sd->stream_buf, buf, to_copy);

  if (sd->stream_eof && xIOBufferEmpty(&sd->stream_buf)) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  }

  return (ssize_t)to_copy;
}

static int h2_write_data(struct xHttpStream_ *stream, const char *data, size_t len) {
  struct xHttpConn_           *conn = stream->conn;
  xHttpProtoH2                *h2   = (xHttpProtoH2 *)conn->proto.state;
  struct xHttpResponseWriter_ *w    = &stream->writer;

  xH2StreamData *sd =
    (xH2StreamData *)nghttp2_session_get_stream_user_data(h2->session, stream->stream_id);
  if (!sd) return -1;

  if (!w->streaming) {
    w->streaming = 1;
    if (!sd->buf_initialized) {
      xIOBufferInit(&sd->stream_buf);
      sd->buf_initialized = 1;
    }
    sd->stream_eof = 0;

    int                  nheader = 1;
    struct xHttpHeader_ *h       = w->headers;
    while (h) {
      nheader++;
      h = h->next;
    }

    nghttp2_nv *nva = (nghttp2_nv *)calloc((size_t)nheader, sizeof(nghttp2_nv));
    if (!nva) return -1;

    char status_str[16];
    int  status_len = snprintf(status_str, sizeof(status_str), "%d", w->status_code);
    nva[0].name     = (uint8_t *)":status";
    nva[0].namelen  = 7;
    nva[0].value    = (uint8_t *)status_str;
    nva[0].valuelen = (size_t)status_len;
    nva[0].flags    = NGHTTP2_NV_FLAG_NO_COPY_NAME;

    int i = 1;
    h     = w->headers;
    while (h) {
      for (char *p = h->key; *p; p++)
        *p = (char)tolower((unsigned char)*p);
      nva[i].name     = (uint8_t *)h->key;
      nva[i].namelen  = strlen(h->key);
      nva[i].value    = (uint8_t *)h->value;
      nva[i].valuelen = strlen(h->value);
      nva[i].flags    = NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE;
      i++;
      h = h->next;
    }

    nghttp2_data_provider data_prd;
    data_prd.source.ptr    = sd;
    data_prd.read_callback = h2_streaming_read_callback;

    int rv =
      nghttp2_submit_response(h2->session, stream->stream_id, nva, (size_t)nheader, &data_prd);
    free(nva);
    if (rv != 0) return -1;
  }

  if (data && len > 0) {
    xIOBufferAppend(&sd->stream_buf, data, len);
  }

  nghttp2_session_resume_data(h2->session, stream->stream_id);

  int rv = nghttp2_session_send(h2->session);
  if (rv != 0) return -1;

  return 0;
}

static int h2_end_stream(struct xHttpStream_ *stream) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH2      *h2   = (xHttpProtoH2 *)conn->proto.state;

  stream->writer.sent = 1;

  xH2StreamData *sd =
    (xH2StreamData *)nghttp2_session_get_stream_user_data(h2->session, stream->stream_id);
  if (sd) {
    sd->stream_eof = 1;
    nghttp2_session_resume_data(h2->session, stream->stream_id);
    nghttp2_session_send(h2->session);
  }

  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════
 */

int xHttpProtoH2Init(struct xHttpConn_ *conn) {
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)calloc(1, sizeof(xHttpProtoH2));
  if (!h2) return -1;

  nghttp2_session_callbacks *callbacks;
  int                        rv = nghttp2_session_callbacks_new(&callbacks);
  if (rv != 0) {
    free(h2);
    return -1;
  }

  nghttp2_session_callbacks_set_send_callback(callbacks, h2_send_callback);
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, h2_on_begin_headers_callback);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, h2_on_header_callback);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                            h2_on_data_chunk_recv_callback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, h2_on_frame_recv_callback);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, h2_on_stream_close_callback);

  rv = nghttp2_session_server_new(&h2->session, callbacks, conn);
  nghttp2_session_callbacks_del(callbacks);

  if (rv != 0) {
    free(h2);
    return -1;
  }

  nghttp2_settings_entry settings[] = {
    {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
  };
  rv = nghttp2_submit_settings(h2->session, NGHTTP2_FLAG_NONE, settings, 1);
  if (rv != 0) {
    nghttp2_session_del(h2->session);
    free(h2);
    return -1;
  }

  rv = nghttp2_session_send(h2->session);
  if (rv != 0) {
    nghttp2_session_del(h2->session);
    free(h2);
    return -1;
  }

  conn->proto.on_data           = h2_on_data;
  conn->proto.reset             = h2_reset;
  conn->proto.destroy           = h2_destroy;
  conn->proto.method            = h2_method;
  conn->proto.should_keep_alive = h2_should_keep_alive;
  conn->proto.send_response     = h2_send_response;
  conn->proto.write_data        = h2_write_data;
  conn->proto.end_stream        = h2_end_stream;
  conn->proto.state             = h2;

  conn->keep_alive = 1;

  return 0;
}
