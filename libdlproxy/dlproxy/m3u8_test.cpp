/*
 * m3u8_test.cpp - Unit tests for HLS m3u8 parser
 */
#include "m3u8.h"

#include <gtest/gtest.h>

/* ── Fixtures ──────────────────────────────────────────────────────── */

static const char *kSimpleMediaPlaylist = "#EXTM3U\n"
                                          "#EXT-X-VERSION:3\n"
                                          "#EXT-X-TARGETDURATION:10\n"
                                          "#EXT-X-MEDIA-SEQUENCE:0\n"
                                          "#EXTINF:10.0,\n"
                                          "segment0.ts\n"
                                          "#EXTINF:10.0,\n"
                                          "segment1.ts\n"
                                          "#EXTINF:8.5,\n"
                                          "segment2.ts\n"
                                          "#EXT-X-ENDLIST\n";

static const char *kMasterPlaylist =
  "#EXTM3U\n"
  "#EXT-X-VERSION:3\n"
  "#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=720x480,CODECS=\"avc1.42c01e\"\n"
  "low.m3u8\n"
  "#EXT-X-STREAM-INF:BANDWIDTH=2560000,RESOLUTION=1280x720,CODECS=\"avc1.42c01e\"\n"
  "high.m3u8\n";

static const char *kByterangePlaylist = "#EXTM3U\n"
                                        "#EXT-X-VERSION:4\n"
                                        "#EXT-X-TARGETDURATION:10\n"
                                        "#EXT-X-MEDIA-SEQUENCE:0\n"
                                        "#EXTINF:10.0,\n"
                                        "#EXT-X-BYTERANGE:1000@5000\n"
                                        "all.ts\n"
                                        "#EXTINF:10.0,\n"
                                        "#EXT-X-BYTERANGE:2000\n"
                                        "all.ts\n"
                                        "#EXT-X-ENDLIST\n";

static const char *kEncryptedPlaylist = "#EXTM3U\n"
                                        "#EXT-X-VERSION:3\n"
                                        "#EXT-X-TARGETDURATION:10\n"
                                        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
                                        "#EXTINF:10.0,\n"
                                        "segment0.ts\n"
                                        "#EXT-X-ENDLIST\n";

static const char *kAbsoluteUriPlaylist = "#EXTM3U\n"
                                          "#EXT-X-TARGETDURATION:5\n"
                                          "#EXTINF:5.0,\n"
                                          "https://cdn.example.com/seg1.ts\n"
                                          "#EXTINF:5.0,\n"
                                          "https://cdn.example.com/seg2.ts\n"
                                          "#EXT-X-ENDLIST\n";

/* ── Tests ─────────────────────────────────────────────────────────── */

TEST(M3u8Parser, ParseSimpleMediaPlaylist) {
  auto *pl = hls_parse_playlist(kSimpleMediaPlaylist, "https://cdn.example.com/playlist.m3u8");
  ASSERT_NE(pl, nullptr);
  EXPECT_FALSE(pl->is_master);
  EXPECT_TRUE(pl->is_vod);
  EXPECT_FALSE(pl->encrypted);
  EXPECT_EQ(pl->version, 3u);
  EXPECT_EQ(pl->target_duration, 10u);
  EXPECT_EQ(pl->media_seq, 0u);
  EXPECT_EQ(pl->segment_count, 3u);

  EXPECT_EQ(pl->segments[0].seq, 0u);
  EXPECT_DOUBLE_EQ(pl->segments[0].duration, 10.0);
  EXPECT_STREQ(pl->segments[0].uri, "https://cdn.example.com/segment0.ts");
  EXPECT_FALSE(pl->segments[0].has_byterange);

  EXPECT_EQ(pl->segments[1].seq, 1u);
  EXPECT_DOUBLE_EQ(pl->segments[2].duration, 8.5);

  hls_playlist_free(pl);
}

TEST(M3u8Parser, ParseMasterPlaylist) {
  auto *pl = hls_parse_playlist(kMasterPlaylist, "https://cdn.example.com/master.m3u8");
  ASSERT_NE(pl, nullptr);
  EXPECT_TRUE(pl->is_master);
  EXPECT_EQ(pl->variant_count, 2u);

  EXPECT_EQ(pl->variants[0].bandwidth, 1280000u);
  EXPECT_EQ(pl->variants[0].width, 720u);
  EXPECT_EQ(pl->variants[0].height, 480u);
  EXPECT_STREQ(pl->variants[0].codecs, "avc1.42c01e");
  EXPECT_STREQ(pl->variants[0].uri, "https://cdn.example.com/low.m3u8");

  EXPECT_EQ(pl->variants[1].bandwidth, 2560000u);
  EXPECT_EQ(pl->variants[1].width, 1280u);
  EXPECT_EQ(pl->variants[1].height, 720u);
  EXPECT_STREQ(pl->variants[1].uri, "https://cdn.example.com/high.m3u8");

  hls_playlist_free(pl);
}

TEST(M3u8Parser, ParseByterange) {
  auto *pl = hls_parse_playlist(kByterangePlaylist, "https://cdn.example.com/playlist.m3u8");
  ASSERT_NE(pl, nullptr);
  EXPECT_EQ(pl->segment_count, 2u);

  /* First segment: explicit offset */
  EXPECT_TRUE(pl->segments[0].has_byterange);
  EXPECT_EQ(pl->segments[0].byte_offset, 5000u);
  EXPECT_EQ(pl->segments[0].byte_length, 1000u);

  /* Second segment: offset omitted = previous offset + length */
  EXPECT_TRUE(pl->segments[1].has_byterange);
  EXPECT_EQ(pl->segments[1].byte_offset, 6000u); /* 5000 + 1000 */
  EXPECT_EQ(pl->segments[1].byte_length, 2000u);

  hls_playlist_free(pl);
}

TEST(M3u8Parser, RelativeUriResolution) {
  auto *pl =
    hls_parse_playlist(kSimpleMediaPlaylist, "https://cdn.example.com/subdir/playlist.m3u8");
  ASSERT_NE(pl, nullptr);
  EXPECT_STREQ(pl->segments[0].uri, "https://cdn.example.com/subdir/segment0.ts");
  hls_playlist_free(pl);
}

TEST(M3u8Parser, AbsoluteUriKept) {
  auto *pl = hls_parse_playlist(kAbsoluteUriPlaylist, "https://other.example.com/playlist.m3u8");
  ASSERT_NE(pl, nullptr);
  EXPECT_EQ(pl->segment_count, 2u);
  EXPECT_STREQ(pl->segments[0].uri, "https://cdn.example.com/seg1.ts");
  EXPECT_STREQ(pl->segments[1].uri, "https://cdn.example.com/seg2.ts");
  hls_playlist_free(pl);
}

TEST(M3u8Parser, EncryptedFlag) {
  auto *pl = hls_parse_playlist(kEncryptedPlaylist, "https://cdn.example.com/playlist.m3u8");
  ASSERT_NE(pl, nullptr);
  EXPECT_TRUE(pl->encrypted);
  EXPECT_EQ(pl->segment_count, 1u);
  hls_playlist_free(pl);
}

TEST(M3u8Parser, UnknownTagsIgnored) {
  const char *playlist = "#EXTM3U\n"
                         "#EXT-X-VERSION:3\n"
                         "#EXT-X-TARGETDURATION:10\n"
                         "#EXT-X-ALLOW-CACHE:YES\n"
                         "#EXT-X-PLAYLIST-TYPE:VOD\n"
                         "#EXTINF:5.0,\n"
                         "seg.ts\n"
                         "#EXT-X-ENDLIST\n";
  auto       *pl       = hls_parse_playlist(playlist, nullptr);
  ASSERT_NE(pl, nullptr);
  EXPECT_EQ(pl->segment_count, 1u);
  EXPECT_TRUE(pl->is_vod);
  hls_playlist_free(pl);
}

TEST(M3u8Parser, NullInput) {
  EXPECT_EQ(hls_parse_playlist(nullptr, nullptr), nullptr);
}

TEST(M3u8Parser, NoExtm3uHeader) {
  const char *not_m3u8 = "just some text\n";
  EXPECT_EQ(hls_parse_playlist(not_m3u8, nullptr), nullptr);
}

TEST(M3u8Parser, MediaSequenceOffset) {
  const char *playlist = "#EXTM3U\n"
                         "#EXT-X-MEDIA-SEQUENCE:100\n"
                         "#EXT-X-TARGETDURATION:5\n"
                         "#EXTINF:5.0,\n"
                         "seg100.ts\n"
                         "#EXTINF:5.0,\n"
                         "seg101.ts\n"
                         "#EXT-X-ENDLIST\n";
  auto       *pl       = hls_parse_playlist(playlist, nullptr);
  ASSERT_NE(pl, nullptr);
  EXPECT_EQ(pl->media_seq, 100u);
  EXPECT_EQ(pl->segments[0].seq, 100u);
  EXPECT_EQ(pl->segments[1].seq, 101u);
  hls_playlist_free(pl);
}
