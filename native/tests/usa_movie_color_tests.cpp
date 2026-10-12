// The private diagnostic links the same policy and converter as the game.
// Optional media checks never open an audio device or a player profile.
#include "../usa_movie_color.h"
#include "../../kit/dx/timed_player.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)

static uint32_t bits(float value) {
    uint32_t out;
    memcpy(&out, &value, sizeof out);
    return out;
}

static bool write_pixels(const std::string &path, const uint32_t *data, size_t count) {
    FILE *f = fopen(path.c_str(), "wx");
    if (!f) return false;
    bool ok = fwrite(data, sizeof *data, count, f) == count;
    return fclose(f) == 0 && ok;
}

static void policy_tests() {
    using C = mf::VideoColorDescription;
    VideoColorConversion c;
    CHECK(usa_movie_color(1280, 720, {}, &c));
    CHECK(bits(c.y_offset) == 0x3d800000 && bits(c.uv_offset) == 0x3f000000);
    CHECK(bits(c.y_scale) == 0x3f950a7f);
    CHECK(bits(c.v_to_r) == 0x3fe575a2 && bits(c.u_to_g) == 0xbe5a5b23);
    CHECK(bits(c.v_to_g) == 0xbf0872f2 && bits(c.u_to_b) == 0x400731db);
    CHECK(usa_movie_color(1024, 576, {}, &c));
    CHECK(bits(c.v_to_r) == 0x3fcc4aa0 && bits(c.u_to_g) == 0xbec960c5);
    CHECK(bits(c.v_to_g) == 0xbf503a5e && bits(c.u_to_b) == 0x40010a0c);
    CHECK(!usa_movie_color(1280, 576, {}, &c));
    CHECK(!usa_movie_color(1024, 720, {}, &c));
    CHECK(!usa_movie_color(1920, 1080, {}, &c));
    CHECK(!usa_movie_color(0, 0, {}, &c));
    CHECK(!usa_movie_color(1280, 720, {}, nullptr));
    for (auto m : {C::Matrix::Bt601, C::Matrix::Bt709, C::Matrix::Other})
        CHECK(!usa_movie_color(1280, 720, {m, C::Range::Unspecified}, &c));
    for (auto r : {C::Range::Limited, C::Range::Full, C::Range::Other})
        CHECK(!usa_movie_color(1024, 576, {C::Matrix::Unspecified, r}, &c));

    // Hand-computable identity channels, clipping, odd width and untouched guards.
    c = {0, 0, 0, 1, 1, 0, 0}; // R=V, G=U, B=0
    const uint8_t y[] = {0, 100, 255, 1, 128}, u[] = {25, 50, 75}, v[] = {200, 150, 100};
    uint8_t dest[24]; memset(dest, 0xa5, sizeof dest);
    video_frame_convert_color_row(dest + 1, y, u, v, 5, c);
    CHECK(dest[0] == 0xa5 && dest[21] == 0xa5 && dest[23] == 0xa5);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(dest[1 + 4*i] == 0 && dest[2 + 4*i] == u[i/2]);
        CHECK(dest[3 + 4*i] == v[i/2] && dest[4 + 4*i] == 0);
    }
    video_frame_convert_color_row(dest + 1, y, u, v, 5, c, 2);
    CHECK(dest[15] == 200 && dest[19] == 150);
    c = {.5f, 0, 4, 0, 0, 0, 0};
    video_frame_convert_color_row(dest + 1, y, u, v, 5, c);
    CHECK(dest[1] == 0 && dest[9] == 255);
    CHECK(video_color_conversion_valid(c));
    c.y_scale = std::numeric_limits<float>::quiet_NaN();
    CHECK(!video_color_conversion_valid(c));
    c.y_scale = std::numeric_limits<float>::infinity();
    CHECK(!video_color_conversion_valid(c));
    c.y_scale = 65537;
    CHECK(!video_color_conversion_valid(c));
}

static bool rejected(uint32_t, uint32_t, mf::VideoColorDescription, VideoColorConversion *) {
    return false;
}
static bool invalid(uint32_t, uint32_t, mf::VideoColorDescription, VideoColorConversion *c) {
    c->y_scale = std::numeric_limits<float>::quiet_NaN();
    return true;
}

static bool first(mf::TimedMedia &media, mf::VideoFrame &frame) {
    mf::AudioFrame audio;
    for (unsigned n = 0; n < 10000; ++n) {
        if (media.pump() == mf::TimedMedia::Result::Error) return false;
        while (media.pop_audio(&audio)) {}
        if (media.pop_video(&frame)) return true;
        if (media.finished()) return false;
    }
    return false;
}

static void movie_test(const char *path, const char *prefix) {
    mf::TimedMedia media;
    std::string why;
    CHECK(media.open(path, {3, 12}, &why, usa_movie_color));
    mf::VideoFrame frame, start;
    CHECK(first(media, start));
    if (start.argb.empty()) { fprintf(stderr, "%s\n", media.error().c_str()); return; }
    CHECK(write_pixels(std::string(prefix) + ".first.bgra", start.argb.data(), start.argb.size()));
    unsigned count = 1;
    for (; count < 24 && first(media, frame); ++count) {
        CHECK(frame.source_color.matrix == mf::VideoColorDescription::Matrix::Unspecified);
        CHECK(frame.source_color.range == mf::VideoColorDescription::Range::Unspecified);
        CHECK(frame.width == start.width && frame.height == start.height);
        CHECK(frame.decode_errors == 0);
    }
    CHECK(count == 24 && media.error().empty());
    CHECK(write_pixels(std::string(prefix) + ".frame23.bgra", frame.argb.data(), frame.argb.size()));
    media.pause(true);
    CHECK(media.pump() == mf::TimedMedia::Result::Paused);
    CHECK(media.reset(&why));
    CHECK(media.pump() == mf::TimedMedia::Result::Paused);
    media.pause(false);
    CHECK(first(media, frame));
    CHECK(frame.argb == start.argb && frame.pts == start.pts);
    media.close();
    for (auto selector : {rejected, invalid}) {
        CHECK(media.open(path, {3, 12}, &why, selector));
        CHECK(!first(media, frame));
        CHECK(media.error() == "unsupported movie color conversion");
        CHECK(media.queued_video() == 0);
        media.close();
    }
    // A following open without a policy must still match the legacy pixels.
    mf::Media legacy;
    CHECK(legacy.open(path, &why));
    CHECK(legacy.next_video(&frame));
    CHECK(media.open(path, {3, 12}, &why));
    CHECK(first(media, start));
    CHECK(frame.argb == start.argb);
    printf("movie size=%ux%u sampled_frames=%u\n", start.width, start.height, count);
}

static void vectors(const char *prefix) {
    constexpr unsigned count = 65536;
    std::vector<uint8_t> y(count), u(count), v(count);
    std::vector<uint32_t> pixels(count);
    for (unsigned i = 0; i < count; ++i) {
        y[i] = uint8_t(i >> 8); u[i] = uint8_t(i); v[i] = uint8_t(i * 73);
    }
    for (bool hd : {false, true}) {
        VideoColorConversion c;
        CHECK(usa_movie_color(hd ? 1280 : 1024, hd ? 720 : 576, {}, &c));
        video_frame_convert_color_row(reinterpret_cast<uint8_t *>(pixels.data()), y.data(),
                                      u.data(), v.data(), count, c, 0);
        CHECK(write_pixels(std::string(prefix) + (hd ? ".hd.bgra" : ".sd.bgra"),
                           pixels.data(), pixels.size()));
    }
}

// Explicit opt-in real-clock run, always muted. Reset must preserve the policy;
// compare a full frame with an independently opened decoder in both epochs.
static void player_test(const char *path) {
    mf::TimedMedia decoder;
    mf::VideoFrame expected;
    std::string why;
    CHECK(decoder.open(path, {3, 12}, &why, usa_movie_color));
    CHECK(first(decoder, expected));
    decoder.close();
    if (expected.argb.empty()) return;
    mf::TimedPlayer player;
    CHECK(player.open(path, true, &why, mf::TimedPlayer::AudioTiming::PreserveDecodedSamples,
                      usa_movie_color));
    unsigned epoch = 0, frames[2]{};
    const uint64_t deadline = os_monotonic_ns() + 15000000000ull;
    auto deliver = [&](mf::VideoFrame &&f) {
        if (!frames[epoch]) CHECK(f.argb == expected.argb && f.pts == expected.pts);
        ++frames[epoch];
    };
    while (os_monotonic_ns() < deadline) {
        auto state = player.step(deliver);
        if (state == mf::TimedPlayer::Status::Error) {
            fprintf(stderr, "%s\n", player.error().c_str()); CHECK(false); break;
        }
        if (epoch == 0 && frames[0] >= 12) {
            CHECK(player.pause(true));
            auto count = frames[0];
            for (int i = 0; i < 5; ++i) {
                CHECK(player.step(deliver) == mf::TimedPlayer::Status::Paused);
                os_sleep_us(10000);
            }
            CHECK(count == frames[0]);
            CHECK(player.reset());
            epoch = 1;
        }
        if (epoch == 1 && frames[1] >= 24) break;
        if (state == mf::TimedPlayer::Status::Ended) break;
        os_sleep_us(2000);
    }
    CHECK(frames[0] >= 12 && frames[1] >= 24);
    CHECK(player.stats().native_discontinuities == 0);
    printf("player epochs=%u,%u late=%llu max_late=%.9f\n", frames[0], frames[1],
           (unsigned long long)player.stats().video_late, player.stats().max_video_lateness);
    player.close();
}

int main(int argc, char **argv) {
    policy_tests();
    if (argc == 3 && !strcmp(argv[1], "--vectors")) vectors(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "--silent-player")) player_test(argv[2]);
    else if (argc == 4 && !strcmp(argv[1], "--movie")) movie_test(argv[2], argv[3]);
    else if (argc != 1) return 2;
    printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
