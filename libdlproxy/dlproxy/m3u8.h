/*
 * m3u8.h - HLS m3u8 playlist parser (VOD subset)
 */
#ifndef DLP_M3U8_H
#define DLP_M3U8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>

/* ── Data structures ───────────────────────────────────────────────── */

struct hls_segment {
  uint32_t seq;           /* sequence number (from EXT-X-MEDIA-SEQUENCE) */
  double   duration;      /* EXTINF duration in seconds                   */
  char    *uri;           /* resolved absolute URI (heap-allocated)       */
  bool     has_byterange; /* EXT-X-BYTERANGE present                      */
  uint64_t byte_offset;   /* byterange offset                             */
  uint64_t byte_length;   /* byterange length (0 = entire segment)        */
};

struct hls_variant {
  uint32_t bandwidth; /* BANDWIDTH attribute                          */
  uint32_t width;     /* RESOLUTION width  (0 if absent)              */
  uint32_t height;    /* RESOLUTION height (0 if absent)              */
  char    *codecs;    /* CODECS attribute (NULL if absent)            */
  char    *uri;       /* variant playlist URI (heap-allocated)        */
};

struct hls_playlist {
  bool     is_master;       /* true = master, false = media          */
  bool     is_vod;          /* EXT-X-ENDLIST present                 */
  bool     encrypted;       /* EXT-X-KEY present                     */
  uint32_t version;         /* EXT-X-VERSION                         */
  uint32_t target_duration; /* EXT-X-TARGETDURATION (seconds)     */
  uint32_t media_seq;       /* EXT-X-MEDIA-SEQUENCE                   */

  /* Media playlist segments */
  struct hls_segment *segments;
  size_t              segment_count;

  /* Master playlist variants */
  struct hls_variant *variants;
  size_t              variant_count;
};

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * @brief Parse an m3u8 playlist from text.
 *
 * @param text      NUL-terminated m3u8 content.
 * @param base_url  Base URL for resolving relative URIs (may be NULL).
 * @return Heap-allocated playlist, or NULL on parse failure.
 *         Caller must free with hls_playlist_free().
 */
XCAPI(struct hls_playlist *) hls_parse_playlist(const char *text, const char *base_url);

/**
 * @brief Free a playlist and all owned resources.
 */
XCAPI(void) hls_playlist_free(struct hls_playlist *pl);

#endif /* DLP_M3U8_H */
