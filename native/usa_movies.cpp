// Experimental USA WMV adapter for the pinned PC executable. Off by default.
// All calls (including focus) run under the guest scheduler baton. The native
// decoder never impersonates an RCMP descriptor, AV handle or guest pointer.
#include "runtime/guest.h"
extern "C" {
#include "funcs.h"
}
#include "dx/timed_player.h"
#include "dx/host_api.h"
#include "runtime/loader.h"
#include "runtime/memory.h"
#include "runtime/native_seam.h"
#include "usa_movie_manifest.h"
#include "usa_movie_color.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace {
constexpr uint32_t kTexture = 0x0091cb28, kPlatform = 0x00983e40;
constexpr uint32_t kTextureOwner = 0x00982d1c;
constexpr uint32_t kSubtitler = 0x0091cb1c, kSkip = 0x00926144;
mf::TimedPlayer player;
uint32_t owner = 0, pixels = 0, locked = 0;
int width = 0, height = 0;
bool texture_ready = false, original_finish = false, focused = true,
     game_paused = false;
bool subtitle_started = false, upload_failed = false;
uint64_t uploads = 0;
uint32_t logged_routes = 0;
std::string title;

bool enabled() {
  const char *v = std::getenv("RECOMP_NFSMW_USA_MOVIES");
  return v &&
         (std::strcmp(v, "silent-qa") == 0 || std::strcmp(v, "audio-qa") == 0);
}

bool silent() {
  const char *v = std::getenv("RECOMP_NFSMW_USA_MOVIES");
  return !v || std::strcmp(v, "audio-qa") != 0;
}

void ret(X86 *c) {
  c->eip = rd32(c->r[R_ESP]);
  c->r[R_ESP] += 4;
  // Like the other native overrides, the dispatcher owns the callback return.
}

uint32_t call(X86 *c, uint32_t fn, uint32_t self,
              const uint32_t *args = nullptr, int n = 0) {
  const X86 saved = *c;
  c->r[R_ECX] = self;
  const uint32_t result = guest_call(c, fn, args, n);
  *c = saved;
  return result;
}

// Logical PC requests may have an old PAL suffix; only the exact USA file in
// the allowlist can be opened. No arbitrary basename, alias movie or fallback.
const UsaMovie *lookup(std::string request) {
  std::replace(request.begin(), request.end(), '\\', '/');
  const auto slash = request.find_last_of('/');
  if (slash != std::string::npos)
    request.erase(0, slash + 1);
  for (char &c : request) {
    if (c >= 'A' && c <= 'Z')
      c = char(c + ('a' - 'A'));
  }
  const auto dot = request.find_last_of('.');
  if (dot == std::string::npos ||
      (request.substr(dot) != ".vp6" && request.substr(dot) != ".wmv"))
    return nullptr;
  request.resize(dot);
  for (const char *suffix : {"_english_ntsc", "_english_pal"}) {
    const size_t length = std::strlen(suffix);
    if (request.size() >= length &&
        request.compare(request.size() - length, length, suffix) == 0) {
      request.resize(request.size() - length);
      break;
    }
  }
  request += "_english_ntsc.wmv";
  for (const auto &movie : kUsaMovies)
    if (request == movie.name)
      return &movie;
  return nullptr;
}

std::string verified_path(const UsaMovie *movie) {
  const char *directory = std::getenv("NFSMW_USA_WMV_DIR");
  if (!movie || !directory || !*directory)
    return {};
  const std::string path = std::string(directory) + "/" + movie->name;
  OsStat stat{};
  // Bound the hash read by the exact corpus size; never hash an arbitrary file.
  if (os_stat(path.c_str(), &stat) != 0 || !stat.is_regular ||
      stat.size != movie->bytes ||
      loader_hash_file(path.c_str()) != movie->sha256)
    return {};
  return path;
}

// The movie queue's actual device/format at start and after each timeline
// break it survived, logged as soon as the player records them.
void log_routes() {
  const auto &s = player.stats();
  static const char *const sources[] = {"none", "learned", "default",
                                        "period-adjusted"};
  for (; logged_routes < s.route_notes; ++logged_routes) {
    const auto &n = s.routes[logged_routes];
    const auto &r = n.route;
    LOGW("USA movie: route %s %s at=%.3f breaks=%llu device='%s' uid='%s'%s "
         "queue_device=%.0f Hz %u ch nominal=%.0f io=%u latency=%u "
         "lead=%.2fms(%s) status=%d",
         title.c_str(), logged_routes ? "after-break" : "start", n.media,
         (unsigned long long)n.discontinuities, r.name, r.uid,
         r.follows_default ? " (default)" : "", r.device_rate,
         r.device_channels, r.nominal_rate, r.io_frames, r.latency_frames,
         n.lead * 1000.0,
         sources[n.lead_source >= 0 && n.lead_source < 4 ? n.lead_source : 0],
         n.status);
  }
}

void close_audio(const char *reason) {
  if (player.status() != mf::TimedPlayer::Status::Closed) {
    log_routes();
    const auto &s = player.stats();
    LOGW("USA movie: close %s reason=%s delivered=%llu uploaded=%llu late=%llu "
         "time=%.6f "
         "rate=%u channels=%u out=%u/%s source=%llu submitted=%llu "
         "route_rebases=%llu route_skip=%.3f route_resyncs=%llu lead=%.2fms "
         "error=%s",
         title.c_str(), reason, (unsigned long long)s.video_delivered,
         (unsigned long long)uploads, (unsigned long long)s.video_late, s.time,
         s.rate, s.channels, s.output_channels,
         s.presentation == 1   ? "stereo-fold"
         : s.presentation == 2 ? "mono-fold"
         : s.presentation == 3 ? "centre-lfe-exchanged"
                               : "as-decoded",
         (unsigned long long)s.audio_source_frames,
         (unsigned long long)s.audio_submitted_frames, (unsigned long long)s.clock_rebases,
         s.max_clock_resync, (unsigned long long)s.clock_resyncs,
         s.return_lead * 1000.0, player.error().c_str());
    player.close();
  }
}

// Keep the game's texture removal/list unlink, but invoke it through guest_call
// so a translated RET-to-callback cannot jump past our host allocation cleanup.
void release_texture(X86 *c) {
  if (texture_ready && rd32(kTextureOwner) == kTexture) {
    const uint32_t skip = rd32(kSkip);
    wr32(kSkip, 0);
    original_finish = true;
    call(c, 0x006dd1d0, 0);
    original_finish = false;
    wr32(kSkip, skip);
  }
  texture_ready = false;
  if (pixels)
    heap_free(pixels);
  if (locked)
    heap_free(locked);
  pixels = locked = 0;
  width = height = 0;
}

bool upload(X86 *c, const mf::VideoFrame &frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.width > 4096 ||
      frame.height > 4096 ||
      frame.argb.size() != size_t(frame.width) * size_t(frame.height))
    return false;
  const uint32_t bytes = uint32_t(frame.argb.size() * 4);
  const auto retain_pixels = [&] {
    // RCMP supplies bottom-up pixels to this game's movie quad, whose UVs
    // invert vertically. Native decoded frames are top-down. Keep both the
    // live texture and its device-reset backing buffer in the guest convention.
    for (int y = 0; y < height; ++y)
      std::memcpy(gm_ptr(pixels + unsigned(y) * unsigned(width) * 4),
                  frame.argb.data() + size_t(height - 1 - y) * width,
                  size_t(width) * 4);
  };
  if (!texture_ready) {
    width = frame.width;
    height = frame.height;
    pixels = heap_alloc(bytes, true);
    locked = heap_alloc(8, true);
    if (!pixels || !locked)
      return false;
    retain_pixels();
    wr32(kTexture, kPlatform);
    wr32(kTextureOwner, kTexture);
    // The original ctor takes ten stack arguments and keeps pixels for a
    // device reset. This buffer must outlive the guest TextureInfo.
    const uint32_t args[] = {
        0x008b0014, pixels, uint32_t(width), uint32_t(height), 32, 0, 0, 0,
        0,          0};
    call(c, 0x004fd070, kTexture, args, 10);
    wr32(kPlatform + 0x14, 21); // D3DFMT_A8R8G8B8
    wr32(kPlatform + 8, 0);
    wr32(kPlatform + 12, 0);
    wr32(kPlatform + 16, 0);
    call(c, 0x006ddd90, kTexture);
    texture_ready = true;
    LOGW("USA movie: texture %dx%d handle=%08x", width, height,
         rd32(kPlatform + 0x18));
  }
  if (frame.width != width || frame.height != height)
    return false;
  retain_pixels();
  const uint32_t texture = rd32(kPlatform + 0x18);
  if (!texture || !gm_valid(texture, 4))
    return false;
  const uint32_t vtable = rd32(texture);
  if (!gm_valid(vtable, 0x54))
    return false;
  const uint32_t args[] = {texture, 0, locked, 0, 0};
  if (call(c, rd32(vtable + 0x4c), 0, args, 5) != 0)
    return false;
  const uint32_t pitch = rd32(locked), dest = rd32(locked + 4);
  const uint64_t extent =
      uint64_t(pitch) * unsigned(height - 1) + unsigned(width) * 4;
  const bool valid = pitch >= unsigned(width) * 4 && extent <= UINT32_MAX &&
                     gm_valid(dest, uint32_t(extent));
  if (valid)
    for (int y = 0; y < height; ++y)
      std::memcpy(gm_ptr(dest + unsigned(y) * pitch),
                  gm_ptr(pixels + unsigned(y) * unsigned(width) * 4),
                  size_t(width) * 4);
  const uint32_t unlock[] = {texture, 0};
  const uint32_t result = call(c, rd32(vtable + 0x50), 0, unlock, 2);
  if (!valid || result)
    return false;
  ++uploads;
  if (!subtitle_started) {
    const uint32_t subtitles = rd32(kSubtitler);
    if (subtitles && gm_valid(subtitles, 0x1c))
      wr32(subtitles + 0x18, call(c, 0x0045ce40, 0));
    subtitle_started = true;
  }
  return true;
}

void advance(X86 *c) {
  if (!owner)
    return;
  if (rd32(owner + 0x120) != 5) {
    close_audio("game stop");
    return;
  }
  std::optional<mf::VideoFrame> newest;
  const auto state =
      player.step([&](mf::VideoFrame &&frame) { newest = std::move(frame); });
  if (player.stats().route_notes != logged_routes)
    log_routes();
  if (newest && !upload(c, *newest))
    upload_failed = true;
  if (upload_failed || state == mf::TimedPlayer::Status::Error ||
      state == mf::TimedPlayer::Status::Ended) {
    close_audio(upload_failed                             ? "texture failure"
                : state == mf::TimedPlayer::Status::Error ? "player failure"
                                                          : "eof");
    wr32(owner + 0x120, 0);
    // Original 00542c50 sends FE completion and resumes the game mixer.
  }
}
} // namespace

extern "C" void native_00542a40(X86 *c) {
  if (!enabled() || rd32(kSkip)) {
    fn_00542a40(c);
    return;
  }
  close_audio("new movie");
  release_texture(c);
  owner = c->r[R_ECX];
  title = gm_str(owner + 0x18, 256);
  uploads = 0;
  logged_routes = 0;
  subtitle_started = upload_failed = game_paused = false;
  wr32(owner + 0x140, 0);
  wr32(owner + 0x144, 0);
  wr32(owner + 0x11c, 5);
  wr32(owner + 0x120, 0); // Failed open still takes original FE completion.
  const UsaMovie *movie = lookup(title);
  const std::string path = verified_path(movie);
  std::string why;
  {
    // The game's current Audio Mode presentation (core.nfsmw -> host).
    uint32_t mode = 0, flags = 0;
    host_audio_get_output(&mode, &flags);
    player.set_output_policy(mode, (flags & 1u) != 0);
  }
  if (path.empty()) {
    LOGW("USA movie: reject unknown/missing/unverified request %s",
         title.c_str());
  } else if (!player.open(
                 path, silent(), &why,
                 mf::TimedPlayer::AudioTiming::PreserveDecodedSamples,
                 usa_movie_color)) {
    LOGW("USA movie: open failed %s: %s", title.c_str(), why.c_str());
  } else {
    // Original MoviePlayer::Setup stores its per-title volume (0..127) at
    // +0, and +15 disables the soundtrack when the game sound system is off.
    // Apply queue gain before decoding starts, including the PSA mute rule.
    const uint32_t volume = rd8(owner + 0x15) ? std::min(rd32(owner), 127u) : 0;
    if (!player.set_gain(float(volume) * rdf32(0x00890bf0))) {
      close_audio("volume failure");
      ret(c);
      return;
    }
    LOGW("USA movie: request=%s source=%s sha256=%s mode=%s volume=%u/127",
         title.c_str(), movie->name, movie->sha256,
         silent() ? "silent-qa" : "audio-qa", volume);
    wr32(owner + 0x118, rd32(owner + 0x118) + 1);
    wr32(owner + 0x120, 5);
    player.pause(!focused);
    advance(c);
  }
  ret(c);
}

extern "C" void native_0065f7a0(X86 *c) {
  const uint32_t caller = rd32(c->r[R_ESP]);
  if (!enabled() || (caller != 0x0055970f && caller != 0x0055972c)) {
    fn_0065f7a0(c);
    return;
  }
  // FE checks the legacy VP6 filename before constructing MoviePlayer. Supply
  // availability from the corresponding verified USA WMV only at these two
  // actual PC movie callsites. No dummy file and no global filesystem alias.
  const std::string request = gm_str(rd32(c->r[R_ESP] + 4), 256);
  const UsaMovie *movie = lookup(request);
  const bool valid = !verified_path(movie).empty();
  c->r[R_EAX] = valid ? uint32_t(movie->bytes + 1) : 0;
  LOGW("USA movie: availability request=%s verified=%d", request.c_str(),
       valid);
  ret(c);
}

extern "C" void native_00525f70(X86 *c) {
  if (!enabled()) {
    fn_00525f70(c);
    return;
  }
  if (owner == c->r[R_ECX] && !rd32(0x0091cc80)) {
    wr32(0x0091cc80, 1);
    advance(c);
    wr32(0x0091cc80, 0);
  }
  ret(c);
}

extern "C" void native_006dd1d0(X86 *c) {
  if (!enabled() || original_finish) {
    fn_006dd1d0(c);
    return;
  }
  close_audio("destroy");
  release_texture(c);
  owner = 0;
  ret(c);
}

extern "C" void native_00517660(X86 *c) {
  if (!enabled()) {
    fn_00517660(c);
    return;
  }
  game_paused = true;
  player.pause(true);
  ret(c);
}
extern "C" void native_00517690(X86 *c) {
  if (!enabled()) {
    fn_00517690(c);
    return;
  }
  game_paused = false;
  player.pause(!focused);
  ret(c);
}
extern "C" void native_00525f40(X86 *c) {
  if (!enabled()) {
    fn_00525f40(c);
    return;
  }
  if (owner == c->r[R_ECX]) {
    subtitle_started = false;
    log_routes();
    logged_routes = 0;
    if (!player.reset())
      wr32(owner + 0x120, 0);
    else {
      wr32(owner + 0x120, 5);
      player.pause(game_paused || !focused);
      advance(c);
    }
  }
  ret(c);
}

extern "C" void recomp_media_focus(int active) {
  focused = active != 0;
  if (enabled() && owner) {
    player.pause(game_paused || !focused);
    LOGW("USA movie: focus=%d time=%.6f delivered=%llu", active,
         player.stats().time,
         (unsigned long long)player.stats().video_delivered);
  }
}
extern "C" void recomp_media_shutdown() {
  // Host calls this only after guest workers stopped. GPU teardown owns its
  // resources here; do not re-enter a retired guest just to unlink textures.
  close_audio("host shutdown");
  if (pixels)
    heap_free(pixels);
  if (locked)
    heap_free(locked);
  pixels = locked = owner = 0;
  texture_ready = false;
}
