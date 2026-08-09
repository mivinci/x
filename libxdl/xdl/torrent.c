/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * torrent.c - Torrent metadata parser implementation
 */

#include <stdlib.h>
#include <string.h>

#include <xdl/bencode.h>
#include <xdl/torrent.h>

struct xdl_torrent_t *xdl_torrent_parse(const uint8_t *data, size_t len) {
  if (!data || !len) return NULL;

  xdl_bencode_value_t *root = NULL;
  if (xdl_bencode_parse(data, len, &root) < 0) return NULL;
  if (root->type != XDL_BENCODE_DICT) {
    xdl_bencode_value_free(root);
    return NULL;
  }

  xdl_bencode_value_t *info = xdl_bencode_dict_find(root, "info");
  if (!info || info->type != XDL_BENCODE_DICT) {
    xdl_bencode_value_free(root);
    return NULL;
  }

  struct xdl_torrent_t *t = calloc(1, sizeof(*t));
  if (!t) {
    xdl_bencode_value_free(root);
    return NULL;
  }

  /* announce (optional) */
  xdl_bencode_value_t *an = xdl_bencode_dict_find(root, "announce");
  if (an && an->type == XDL_BENCODE_STRING) {
    t->announce = malloc(an->str.len + 1);
    if (t->announce) {
      memcpy(t->announce, an->str.data, an->str.len);
      t->announce[an->str.len] = '\0';
    }
  }

  /* info.name */
  xdl_bencode_value_t *name = xdl_bencode_dict_find(info, "name");
  if (name && name->type == XDL_BENCODE_STRING) {
    t->name = malloc(name->str.len + 1);
    if (t->name) {
      memcpy(t->name, name->str.data, name->str.len);
      t->name[name->str.len] = '\0';
    }
  }

  /* info.length (file size) */
  xdl_bencode_value_t *length = xdl_bencode_dict_find(info, "length");
  if (length && length->type == XDL_BENCODE_INTEGER) {
    t->length = (uint64_t)length->integer;
  }

  /* info.block length (block size, optional, default 256KB) */
  xdl_bencode_value_t *bl = xdl_bencode_dict_find(info, "block length");
  if (bl && bl->type == XDL_BENCODE_INTEGER && bl->integer > 0) {
    t->block_length = (uint32_t)bl->integer;
  } else {
    t->block_length = XDL_TORRENT_DEFAULT_BLOCK_LENGTH;
  }

  /* info.blocks (SHA1 hashes) */
  xdl_bencode_value_t *blocks = xdl_bencode_dict_find(info, "blocks");
  if (blocks && blocks->type == XDL_BENCODE_STRING && (blocks->str.len % 20) == 0) {
    t->block_count  = (uint32_t)(blocks->str.len / 20);
    t->block_hashes = malloc(blocks->str.len);
    if (t->block_hashes) memcpy(t->block_hashes, blocks->str.data, blocks->str.len);
  } else if (t->length > 0 && t->block_length > 0) {
    t->block_count = (uint32_t)((t->length + t->block_length - 1) / t->block_length);
  }

  /* info.url-list (HTTP fallback URLs) */
  xdl_bencode_value_t *ul = xdl_bencode_dict_find(info, "url-list");
  if (ul) {
    if (ul->type == XDL_BENCODE_LIST) {
      for (size_t i = 0; i < ul->list.count && t->http_url_count < XDL_TORRENT_MAX_URLS; i++) {
        if (ul->list.items[i]->type == XDL_BENCODE_STRING) {
          t->http_urls[t->http_url_count] = malloc(ul->list.items[i]->str.len + 1);
          if (t->http_urls[t->http_url_count]) {
            memcpy(t->http_urls[t->http_url_count], ul->list.items[i]->str.data,
                   ul->list.items[i]->str.len);
            t->http_urls[t->http_url_count][ul->list.items[i]->str.len] = '\0';
            t->http_url_count++;
          }
        }
      }
    } else if (ul->type == XDL_BENCODE_STRING) {
      t->http_urls[0] = malloc(ul->str.len + 1);
      if (t->http_urls[0]) {
        memcpy(t->http_urls[0], ul->str.data, ul->str.len);
        t->http_urls[0][ul->str.len] = '\0';
        t->http_url_count            = 1;
      }
    }
  }

  xdl_bencode_value_free(root);
  return t;
}

void xdl_torrent_destroy(struct xdl_torrent_t *t) {
  if (!t) return;
  free(t->name);
  free(t->block_hashes);
  free(t->announce);
  for (int i = 0; i < t->http_url_count; i++)
    free(t->http_urls[i]);
  free(t);
}

uint32_t xdl_torrent_block_count(const struct xdl_torrent_t *t) {
  if (!t) return 0;
  return t->block_count;
}
