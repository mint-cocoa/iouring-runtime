#include <iouring_runtime/media/Hls.h>

#include <gtest/gtest.h>

#include <string>

namespace media = iouring_runtime::media;

TEST(HlsMediaUtils, UrlEncodeDecodeRoundTrip) {
    const std::string raw = "https://cdn.example.test/a b/seg_01.ts?token=a+b&x=1";
    const auto encoded = media::UrlEncode(raw);
    EXPECT_EQ(
        "https%3A%2F%2Fcdn.example.test%2Fa%20b%2Fseg_01.ts%3Ftoken%3Da%2Bb%26x%3D1",
        encoded);
    EXPECT_EQ(raw, media::UrlDecode(encoded));
}

TEST(HlsMediaUtils, ResolvesRelativeUrls) {
    EXPECT_EQ("https://cdn.example.test/live/seg.ts",
              media::ResolveUrl("https://cdn.example.test/live/index.m3u8", "seg.ts"));
    EXPECT_EQ("https://cdn.example.test/key.bin",
              media::ResolveUrl("https://cdn.example.test/live/index.m3u8", "/key.bin"));
    EXPECT_EQ("https://other.example.test/seg.ts",
              media::ResolveUrl("https://cdn.example.test/live/index.m3u8",
                                "https://other.example.test/seg.ts"));
}

TEST(HlsMediaUtils, RewritesManifestUrisThroughProxy) {
    const std::string manifest =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "seg_00001.ts\n";

    const auto rewritten =
        media::RewriteHlsManifest(manifest, "https://cdn.example.test/live/index.m3u8");

    EXPECT_NE(std::string::npos,
              rewritten.find("/proxy/hls?url=https%3A%2F%2Fcdn.example.test%2Flive%2Fkey.bin"));
    EXPECT_NE(std::string::npos,
              rewritten.find("/proxy/hls?url=https%3A%2F%2Fcdn.example.test%2Flive%2Fseg_00001.ts"));
}
