/*
 * m3u8.c - HLS m3u8 playlist parser (VOD subset)
 *
 * Handles: #EXTM3U, #EXT-X-VERSION, #EXT-X-TARGETDURATION,
 *           #EXTINF, #EXT-X-BYTERANGE, #EXT-X-STREAM-INF,
 *           #EXT-X-ENDLIST, #EXT-X-MEDIA-SEQUENCE, #EXT-X-KEY
 * Unknown tags are silently ignored (per spec).
 */
#include "m3u8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static char *xstrdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char  *p = (char *)malloc(n + 1);
  if (p) memcpy(p, s, n + 1);
  return p;
}

static char *xstrndup(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1);
  if (p) {
    memcpy(p, s, n);
    p[n] = '\0';
  }
  return p;
}

/* Skip leading whitespace, return pointer to first non-ws char */
static const char *skip_ws(const char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  return s;
}

/* Check if line starts with tag (case-sensitive, m3u8 tags are case-sensitive) */
static bool starts_with(const char *line, const char *tag) {
  size_t n = strlen(tag);
  return strncmp(line, tag, n) == 0;
}

/* Extract attribute value from a #EXT-X-STREAM-INF line.
 * Returns NULL if not found. Caller does NOT free (points into line buffer). */
static const char *find_attr(const char *line, const char *key) {
  size_t      klen = strlen(key);
  const char *p    = line;
  while (*p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *v = p + klen + 1;
      if (*v == '"') {
        /* Quoted value — return pointer after quote */
        return v + 1;
      }
      return v;
    }
    p++;
  }
  return NULL;
}

/* Copy a quoted attribute value (between quotes). Returns heap-allocated string. */
static char *copy_quoted_attr(const char *line, const char *key) {
  const char *v = find_attr(line, key);
  if (!v) return NULL;
  const char *start = v;
  /* If value started with a quote, find closing quote */
  if (v > line && v[-1] == '"') {
    const char *end = strchr(v, '"');
    if (end) return xstrndup(v, (size_t)(end - v));
    return xstrdup(v);
  }
  /* Unquoted — read until comma or end */
  const char *end = v;
  while (*end && *end != ',')
    end++;
  return xstrndup(v, (size_t)(end - v));
}

/* Copy an unquoted numeric attribute */
static uint32_t parse_uint_attr(const char *line, const char *key) {
  const char *v = find_attr(line, key);
  if (!v) return 0;
  return (uint32_t)strtoul(v, NULL, 10);
}

/* ── URI resolution ────────────────────────────────────────────────── */

/* Resolve a relative URI against a base URL.
 * Returns a heap-allocated absolute URI, or NULL if base is NULL.
 * Caller frees. */
static char *resolve_uri(const char *uri, const char *base_url) {
  if (!uri) return NULL;

  /* If URI is already absolute (has scheme), return a copy */
  if (strstr(uri, "://") != NULL) return xstrdup(uri);

  /* If no base URL, return a copy of the URI */
  if (!base_url || !*base_url) return xstrdup(uri);

  /* If URI starts with //, prepend scheme from base */
  if (uri[0] == '/' && uri[1] == '/') {
    const char *scheme = strstr(base_url, "://");
    if (!scheme) return xstrdup(uri);
    size_t slen = (size_t)(scheme - base_url) + 1; /* include "://" */
    /* "https" + "://" + uri */
    size_t n = slen + 2 + strlen(uri); /* scheme:// + //uri... wait */
    /* Actually: scheme + "://" + uri (uri already has //) */
    char *result = (char *)malloc(slen + strlen(uri) + 1);
    if (result) {
      memcpy(result, base_url, slen); /* "https://" */
      strcpy(result + slen, uri);     /* "//cdn..." */
    }
    (void)n;
    return result;
  }

  /* If URI starts with /, replace path in base URL */
  if (uri[0] == '/') {
    const char *path_start = strstr(base_url, "://");
    if (!path_start) return xstrdup(uri);
    path_start += 3; /* skip "://" */
    const char *path = strchr(path_start, '/');
    if (path) {
      size_t prefix_len = (size_t)(path - base_url);
      char  *result     = (char *)malloc(prefix_len + strlen(uri) + 1);
      if (result) {
        memcpy(result, base_url, prefix_len);
        strcpy(result + prefix_len, uri);
      }
      return result;
    }
    /* No path in base — append */
    size_t blen   = strlen(base_url);
    char  *result = (char *)malloc(blen + strlen(uri) + 1);
    if (result) {
      memcpy(result, base_url, blen);
      strcpy(result + blen, uri);
    }
    return result;
  }

  /* Relative path — replace last component of base URL path */
  const char *path_start = strstr(base_url, "://");
  if (!path_start) return xstrdup(uri);
  path_start += 3;
  const char *last_slash = strrchr(path_start, '/');
  if (!last_slash) {
    /* No path in base — just append */
    size_t blen   = strlen(base_url);
    char  *result = (char *)malloc(blen + 1 + strlen(uri) + 1);
    if (result) {
      memcpy(result, base_url, blen);
      result[blen] = '/';
      strcpy(result + blen + 1, uri);
    }
    return result;
  }
  size_t prefix_len = (size_t)(last_slash - base_url + 1);
  char  *result     = (char *)malloc(prefix_len + strlen(uri) + 1);
  if (result) {
    memcpy(result, base_url, prefix_len);
    strcpy(result + prefix_len, uri);
  }
  return result;
}

/* ── Dynamic array helpers ─────────────────────────────────────────── */

static bool segments_push(struct hls_segment **arr, size_t *count, size_t *cap,
                          struct hls_segment seg) {
  if (*count == *cap) {
    size_t              ncap = *cap ? *cap * 2 : 16;
    struct hls_segment *n    = (struct hls_segment *)realloc(*arr, ncap * sizeof(**arr));
    if (!n) return false;
    *arr = n;
    *cap = ncap;
  }
  (*arr)[(*count)++] = seg;
  return true;
}

static bool variants_push(struct hls_variant **arr, size_t *count, size_t *cap,
                          struct hls_variant var) {
  if (*count == *cap) {
    size_t              ncap = *cap ? *cap * 2 : 8;
    struct hls_variant *n    = (struct hls_variant *)realloc(*arr, ncap * sizeof(**arr));
    if (!n) return false;
    *arr = n;
    *cap = ncap;
  }
  (*arr)[(*count)++] = var;
  return true;
}

/* ── Main parser ───────────────────────────────────────────────────── */

struct hls_playlist *hls_parse_playlist(const char *text, const char *base_url) {
  if (!text) return NULL;

  /* Must start with #EXTM3U */
  if (!starts_with(text, "#EXTM3U")) return NULL;

  struct hls_playlist *pl = (struct hls_playlist *)calloc(1, sizeof(*pl));
  if (!pl) return NULL;
  pl->is_master = false;
  pl->is_vod    = false;
  pl->encrypted = false;
  pl->version   = 1;
  pl->media_seq = 0;

  struct hls_segment *segs      = NULL;
  size_t              seg_count = 0, seg_cap = 0;
  struct hls_variant *vars      = NULL;
  size_t              var_count = 0, var_cap = 0;

  /* Per-segment state */
  double   pending_duration      = 0.0;
  bool     has_pending_duration  = false;
  bool     has_pending_byterange = false;
  uint64_t pending_byte_offset   = 0;
  uint64_t pending_byte_length   = 0;
  uint32_t next_seq              = 0; /* will be set by EXT-X-MEDIA-SEQUENCE */

  /* Line-by-line parse */
  const char *cursor = text;

  /* Skip first line (#EXTM3U) */
  const char *nl = strchr(cursor, '\n');
  if (!nl) {
    /* Entire file is just #EXTM3U */
    pl->segments      = NULL;
    pl->segment_count = 0;
    pl->variants      = NULL;
    pl->variant_count = 0;
    return pl;
  }
  cursor = nl + 1;

  while (*cursor) {
    /* Extract one line */
    const char *line_end = strchr(cursor, '\n');
    size_t      line_len;
    if (line_end)
      line_len = (size_t)(line_end - cursor);
    else
      line_len = strlen(cursor);

    /* Strip trailing \r */
    if (line_len > 0 && cursor[line_len - 1] == '\r') line_len--;

    /* Skip empty lines */
    if (line_len == 0) {
      if (line_end) {
        cursor = line_end + 1;
        continue;
      }
      break;
    }

    /* Make a NUL-terminated copy for easier parsing */
    char *line = xstrndup(cursor, line_len);
    if (!line) goto fail;

    const char *trimmed = skip_ws(line);

    if (trimmed[0] == '\0') {
      /* Empty line */
      free(line);
    } else if (trimmed[0] == '#') {
      /* Tag or comment */
      if (starts_with(trimmed, "#EXT-X-VERSION:")) {
        pl->version = (uint32_t)strtoul(trimmed + 15, NULL, 10);
      } else if (starts_with(trimmed, "#EXT-X-TARGETDURATION:")) {
        pl->target_duration = (uint32_t)strtoul(trimmed + 22, NULL, 10);
      } else if (starts_with(trimmed, "#EXT-X-MEDIA-SEQUENCE:")) {
        pl->media_seq = (uint32_t)strtoul(trimmed + 22, NULL, 10);
        next_seq      = pl->media_seq;
      } else if (starts_with(trimmed, "#EXTINF:")) {
        /* #EXTINF:<duration>,<title> */
        pending_duration     = strtod(trimmed + 8, NULL);
        has_pending_duration = true;
      } else if (starts_with(trimmed, "#EXT-X-BYTERANGE:")) {
        /* #EXT-X-BYTERANGE:<length>@<offset> */
        const char *p       = trimmed + 17;
        pending_byte_length = strtoull(p, NULL, 10);
        const char *at      = strchr(p, '@');
        if (at) {
          pending_byte_offset = strtoull(at + 1, NULL, 10);
        } else {
          /* Offset omitted — follows previous segment (set later) */
          pending_byte_offset = (uint64_t)-1;
        }
        has_pending_byterange = true;
      } else if (starts_with(trimmed, "#EXT-X-STREAM-INF:")) {
        /* Master playlist variant */
        struct hls_variant var = {0};
        var.bandwidth          = parse_uint_attr(trimmed, "BANDWIDTH");
        var.codecs             = copy_quoted_attr(trimmed, "CODECS");

        /* RESOLUTION=<w>x<h> */
        const char *res = find_attr(trimmed, "RESOLUTION");
        if (res) {
          var.width     = (uint32_t)strtoul(res, NULL, 10);
          const char *x = strchr(res, 'x');
          if (x) var.height = (uint32_t)strtoul(x + 1, NULL, 10);
        }

        /* URI might be in the attribute (URI="...") or on the next line */
        char *inline_uri = copy_quoted_attr(trimmed, "URI");
        if (inline_uri) {
          var.uri = resolve_uri(inline_uri, base_url);
          free(inline_uri);
          if (!variants_push(&vars, &var_count, &var_cap, var)) {
            free(var.codecs);
            free(var.uri);
            free(line);
            goto fail;
          }
          pl->is_master = true;
        } else {
          /* URI is on the next non-tag line — store variant, URI filled later */
          if (!variants_push(&vars, &var_count, &var_cap, var)) {
            free(var.codecs);
            free(line);
            goto fail;
          }
          pl->is_master = true;
        }
      } else if (starts_with(trimmed, "#EXT-X-ENDLIST")) {
        pl->is_vod = true;
      } else if (starts_with(trimmed, "#EXT-X-KEY:")) {
        pl->encrypted = true;
      }
      /* Unknown tags: ignore */
      free(line);
    } else {
      /* URI line */
      char *resolved = resolve_uri(trimmed, base_url);
      free(line);

      if (var_count > 0 && !vars[var_count - 1].uri) {
        /* This URI belongs to the last variant (STREAM-INF without inline URI) */
        vars[var_count - 1].uri = resolved;
      } else if (has_pending_duration) {
        /* Media segment */
        struct hls_segment seg = {0};
        seg.seq                = next_seq++;
        seg.duration           = pending_duration;
        seg.uri                = resolved;
        seg.has_byterange      = has_pending_byterange;
        seg.byte_offset        = pending_byte_offset;
        seg.byte_length        = pending_byte_length;

        if (!segments_push(&segs, &seg_count, &seg_cap, seg)) {
          free(resolved);
          goto fail;
        }
        has_pending_duration  = false;
        has_pending_byterange = false;
        pending_byte_offset   = 0;
        pending_byte_length   = 0;
      } else {
        /* URI without preceding EXTINF — treat as a segment with duration 0 */
        struct hls_segment seg = {0};
        seg.seq                = next_seq++;
        seg.duration           = 0.0;
        seg.uri                = resolved;
        if (!segments_push(&segs, &seg_count, &seg_cap, seg)) {
          free(resolved);
          goto fail;
        }
      }
    }

    if (line_end)
      cursor = line_end + 1;
    else
      break;
  }

  /* Handle byterange offset continuation (offset omitted = follows previous) */
  for (size_t i = 1; i < seg_count; i++) {
    if (segs[i].has_byterange && segs[i].byte_offset == (uint64_t)-1) {
      /* Find previous segment with same URI */
      for (size_t j = i; j > 0; j--) {
        if (segs[j - 1].uri && segs[i].uri && strcmp(segs[j - 1].uri, segs[i].uri) == 0) {
          segs[i].byte_offset = segs[j - 1].byte_offset + segs[j - 1].byte_length;
          break;
        }
      }
      if (segs[i].byte_offset == (uint64_t)-1) segs[i].byte_offset = 0;
    }
  }

  pl->segments      = segs;
  pl->segment_count = seg_count;
  pl->variants      = vars;
  pl->variant_count = var_count;
  return pl;

fail:
  free(segs);
  for (size_t i = 0; i < var_count; i++) {
    free(vars[i].codecs);
    free(vars[i].uri);
  }
  free(vars);
  free(pl);
  return NULL;
}

void hls_playlist_free(struct hls_playlist *pl) {
  if (!pl) return;
  for (size_t i = 0; i < pl->segment_count; i++) {
    free(pl->segments[i].uri);
  }
  free(pl->segments);
  for (size_t i = 0; i < pl->variant_count; i++) {
    free(pl->variants[i].codecs);
    free(pl->variants[i].uri);
  }
  free(pl->variants);
  free(pl);
}
