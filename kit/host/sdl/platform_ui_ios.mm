// platform_ui_ios.mm - the SDL host on iPadOS: one fullscreen Metal window, the
// game in Documents/game (imported by the launcher, from the bundle of a
// developer build or from files the player chose), the launcher's platform,
// and the app lifecycle mapped onto audio and presentation.
#include "platform_ui.h"
#include "ios_overlay.h"

#include "../../platform/os.h"
#include "../../runtime/layout.h"
#include "../audio.h"
#include "../launcher/launcher_sdl.h"
#include "../present.h"
#include "game_config.h"
#include "version.h"

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string bundle_dir() {
    char path[4096];
    if (os_exe_path(path, sizeof path) != 0)
        return "";
    return fs::path(path).parent_path().string();
}

std::string documents_dir() {
    NSArray *paths =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return paths.count ? std::string([paths[0] fileSystemRepresentation]) : "";
}

// Keeps the `keep` newest *.log files in `dir`.
void prune_logs(const fs::path &dir, size_t keep) {
    std::vector<fs::directory_entry> logs;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".log")
            logs.push_back(e);
    std::sort(logs.begin(), logs.end(), [](const auto &a, const auto &b) {
        std::error_code e1, e2;
        return a.last_write_time(e1) > b.last_write_time(e2);
    });
    for (size_t i = keep; i < logs.size(); ++i)
        fs::remove(logs[i].path(), ec);
}

} // namespace

void platform_ui_ios_prepare() {
    const fs::path docs = documents_dir();
    if (docs.empty())
        return;
    std::error_code ec;
    // RECOMP_* switches first (Documents/switches.txt), so one can move the profile too.
    recomp_env_apply_file((docs / "switches.txt").c_str());

    // The log: stdout and stderr into Documents/Logs/<date>-<time>.log, the ten newest kept. A device has no
    // console, and this is what a player can send (Files app, On My iPad / the app's folder).
    const fs::path logs = docs / "Logs";
    fs::create_directories(logs, ec);
    prune_logs(logs, 9);
    char stamp[32];
    const time_t now = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    const std::string log = (logs / (std::string(stamp) + ".log")).string();
    if (freopen(log.c_str(), "a", stderr)) {
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(fileno(stderr), fileno(stdout));
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    fprintf(stderr, "[ios] %s %s, log %s\n", RECOMP_APP_NAME, POP_RECOMP_VERSION, log.c_str());

    // The profile (registry, saves, settings) where the Files app reaches it, seeded from the bundle's Defaults
    // the way the Mac app's Launch script seeds its own: a file the player already has is never replaced.
    const char *profile_env = recomp_env("PROFILE_DIR");
    const fs::path profile = profile_env && *profile_env ? fs::path(profile_env) : docs / "Profile";
    if (!profile_env || !*profile_env)
        setenv("RECOMP_PROFILE_DIR", profile.c_str(), 1);
    fs::create_directories(profile, ec);
    const fs::path defaults = fs::path(bundle_dir()) / "Defaults";
    for (const char *name : {"registry.json", "mod-settings.json", "NativeOptions.ini"}) {
        if (fs::exists(profile / name, ec) || !fs::is_regular_file(defaults / name, ec))
            continue;
        fs::copy_file(defaults / name, profile / name, ec);
        fprintf(stderr, "[ios] profile: %s from the bundle's defaults%s\n", name, ec ? " FAILED" : "");
    }
    // The packaged Mac app's environment (tools/setup_kit/defaults/Launch); a switches.txt value wins.
    setenv("RECOMP_D3D9_SCALE", "1", 0);
    setenv("RECOMP_HOST_PRESENT_PACING", "duration", 0);
    setenv("RECOMP_D3D9_SHADER_WARM_DIR", (profile / "ShaderWarm").c_str(), 0);
    // A read-only shader seed in the bundle (the private test build only: recorded from a played profile), so a fresh
    // install (LiveContainer gives each install a new data folder) starts with a warm shader cache.
    {
        NSString *seed = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"ShaderWarmSeed"];
        if ([NSFileManager.defaultManager fileExistsAtPath:[seed stringByAppendingPathComponent:@"order.txt"]]) {
            setenv("RECOMP_D3D9_SHADER_WARM_SEED", seed.UTF8String, 0);
            fprintf(stderr, "[ios] shader seed: bundled\n");
        }
    }
    ios_overlay_apply_launch_settings(); // MetalFX markers, and the overlay's restart-only options
}

void platform_ui_init_hints() {
    // Landscape on every device (the Info.plist keys match): the game's picture
    // fills the screen edge to edge with no portrait letterbox.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // The game's Auto resolution is the display's own pixel size, so the HUD and
    // menus draw at native and only the 3D scene is upscaled (MetalFX).
    setenv("RECOMP_AUTO_RESOLUTION_NATIVE", "1", 0);
    // The mapper turns fingers into mouse and key events itself.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    // RECOMP_* switches for a device with no shell: Documents/switches.txt,
    // put there with devicectl (see tools/ios_logs.py for the container).
    recomp_env_apply_file((documents_dir() + "/switches.txt").c_str());
}

// Documents/game when it is ready. Anything else - no game yet, a bundled copy
// still to import, a wrong or partial import - is the launcher's to show.
GamePath platform_ui_resolve_game(const char *, std::string *error) {
    GamePath g;
    if (error)
        error->clear();
    const std::string docs = documents_dir() + "/game";
    const launcher::Status st = launcher::check(launcher::spec_from_config(), docs);
    if (st.state == launcher::State::Ready) {
        g.exe = st.exe;
        g.source = GamePathSource::Saved;
    }
    return g;
}

// ---------------------------------------------------------------------------
// The launcher's iPadOS platform: the document picker for a folder or a ZIP,
// imports into Documents/game (kept out of iCloud backups), the Files app and
// Finder as another way in, and a share sheet for exported saves.
// ---------------------------------------------------------------------------
@interface RecompPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) launcher::PickDone done;
@property(nonatomic) bool scoped;
@end

namespace {
std::vector<NSURL *> g_scoped; // security-scoped URLs held for an import
NSMutableArray *g_delegates;   // picker delegates alive until they answer
UIBackgroundTaskIdentifier g_background = UIBackgroundTaskInvalid;
} // namespace

@implementation RecompPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    std::vector<launcher::Picked> picked;
    for (NSURL *url in urls) {
        if (self.scoped && [url startAccessingSecurityScopedResource])
            g_scoped.push_back(url);
        launcher::Picked p;
        p.path = url.fileSystemRepresentation;
        p.name = url.lastPathComponent.UTF8String;
        picked.push_back(p);
    }
    if (self.done)
        self.done(picked, "");
    [g_delegates removeObject:self];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    if (self.done)
        self.done({}, "");
    [g_delegates removeObject:self];
}
@end

namespace {

UIViewController *top_controller(SDL_Window *window) {
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController *vc = uiwindow.rootViewController;
    while (vc.presentedViewController)
        vc = vc.presentedViewController;
    return vc;
}

class IosPlatform final : public launcher::Platform {
  public:
    explicit IosPlatform(SDL_Window *window) : window_(window) {}

    launcher::PlatformInfo info() override {
        launcher::PlatformInfo i;
        i.plays_in_place = false;
        i.can_pick_folder = true;
        i.can_pick_zip = true;
        i.can_open_folder = false;
        i.touch = true;
        i.import_root = documents_dir() + "/game";
        i.profile_dir = host_layout().profile_dir;
        i.drop_hint =
            "Or copy the game folder into this app's files with Finder (iPad connected to a Mac) "
            "or the Files app, then open the app again.";
        const fs::path bundled = fs::path(bundle_dir()) / "game";
        std::error_code ec;
        if (fs::is_regular_file(bundled / RECOMP_EXECUTABLE, ec))
            i.auto_import = bundled.string();
        return i;
    }
    void present(UIDocumentPickerViewController *picker, launcher::PickDone done, bool scoped) {
        RecompPickerDelegate *delegate = [RecompPickerDelegate new];
        delegate.done = std::move(done);
        delegate.scoped = scoped;
        if (!g_delegates)
            g_delegates = [NSMutableArray new];
        [g_delegates addObject:delegate];
        picker.delegate = delegate;
        picker.modalPresentationStyle = UIModalPresentationFormSheet;
        [top_controller(window_) presentViewController:picker animated:YES completion:nil];
    }
    void pick_folder(launcher::PickDone done) override {
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
        present(picker, std::move(done), true);
    }
    void pick_zip(launcher::PickDone done) override {
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeZIP ]];
        present(picker, std::move(done), true);
    }
    void pick_saves(launcher::PickDone done) override {
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeZIP ]
                                                                        asCopy:YES];
        present(picker, std::move(done), false);
    }
    // Written to a temporary file first; export_ready offers it to the player.
    void pick_export(const std::string &suggested, launcher::PickDone done) override {
        launcher::Picked p;
        p.path = std::string(NSTemporaryDirectory().fileSystemRepresentation) + "/" + suggested;
        p.name = suggested;
        done({p}, "");
    }
    void export_ready(const launcher::Picked &p) override {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.path.c_str()]];
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForExportingURLs:@[ url ] asCopy:YES];
        present(picker, nullptr, false);
    }
    std::vector<std::string> candidates(const launcher::Spec &spec) override {
        // A game folder the player copied into Documents under any name.
        std::vector<std::string> out;
        const std::string docs = documents_dir();
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(docs, ec)) {
            const std::string name = entry.path().filename().string();
            if (!entry.is_directory(ec) || name == "game" || name[0] == '.')
                continue;
            // The folder itself, so a move can remove all of it afterwards.
            if (!launcher::find_root(spec, entry.path().string()).empty())
                out.push_back(entry.path().string());
        }
        return out;
    }
    // A folder the player copied into Documents is moved into Documents/game.
    bool movable(const launcher::Picked &p) override {
        const std::string docs = documents_dir() + "/";
        return !p.path.empty() && p.path.compare(0, docs.size(), docs) == 0 &&
               p.path.find('/', docs.size()) == std::string::npos && p.path != docs + "game";
    }
    void open_url(const std::string &url) override {
        NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (u)
            [[UIApplication sharedApplication] openURL:u options:@{} completionHandler:nil];
    }
    void import_activity(bool active, const launcher::Progress *progress) override {
        if (progress)
            return;
        dispatch_block_t update = ^{
          [UIApplication sharedApplication].idleTimerDisabled = active;
          if (active && g_background == UIBackgroundTaskInvalid)
              g_background = [[UIApplication sharedApplication]
                  beginBackgroundTaskWithName:@"Import game"
                            expirationHandler:^{
                              [[UIApplication sharedApplication] endBackgroundTask:g_background];
                              g_background = UIBackgroundTaskInvalid;
                            }];
          else if (!active && g_background != UIBackgroundTaskInvalid) {
              [[UIApplication sharedApplication] endBackgroundTask:g_background];
              g_background = UIBackgroundTaskInvalid;
          }
        };
        if ([NSThread isMainThread])
            update();
        else
            dispatch_async(dispatch_get_main_queue(), update);
    }
    void protect_import(const std::string &root) override {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:root.c_str()]
                                isDirectory:YES];
        NSError *err = nil;
        if (![url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&err])
            fprintf(stderr, "[ios] could not exclude %s from backup: %s\n", root.c_str(),
                    err.localizedDescription.UTF8String);
    }
    void release(const launcher::Picked &p) override {
        for (auto it = g_scoped.begin(); it != g_scoped.end(); ++it)
            if (p.path == (*it).fileSystemRepresentation) {
                [*it stopAccessingSecurityScopedResource];
                g_scoped.erase(it);
                return;
            }
    }

  private:
    SDL_Window *window_;
};

} // namespace

namespace launcher {
std::unique_ptr<Platform> make_platform(SDL_Window *window) {
    return std::make_unique<IosPlatform>(window);
}
} // namespace launcher

namespace {
// SDL3 does not queue the app lifecycle events; it hands them to event
// watchers on the UIKit callstack that raised them (SDL_SendAppEvent). The
// host's event loop therefore never sees them, and this watcher is the only
// place the background transition can be acted on before iOS freezes the
// process.
bool lifecycle_watch(void *, SDL_Event *e) {
    platform_ui_handle_lifecycle(*e);
    return true;
}
} // namespace

SDL_Window *platform_ui_create_window(const char *title, int, int, int, int,
                                      SDL_WindowFlags surface_flag, int *window_mode) {
    static bool watching = false;
    if (!watching) {
        SDL_AddEventWatch(lifecycle_watch, nullptr);
        watching = true;
    }
    SDL_Window *w = SDL_CreateWindow(
        title, 0, 0, surface_flag | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_mode)
        *window_mode = 2;
    if (w)
        ios_overlay_install(w); // the iOS display menu, above the Metal view
    return w;
}

// Set by a real trip to the background (not a Control Centre pull, which only resigns active).
static bool g_was_backgrounded = false;

bool platform_ui_handle_lifecycle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        fprintf(stderr, "[ios] will enter background: suspending presentation and audio\n");
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        fprintf(stderr, "[ios] did enter background\n");
        g_was_backgrounded = true;
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        fprintf(stderr, "[ios] will enter foreground\n");
        return true;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        fprintf(stderr, "[ios] did enter foreground: resuming audio and presentation\n");
        host_audio_pause(false);
        // A screen lock stops the system's audio output under us and does not hand it back, so the game came back
        // silent (going to the home screen and back fixed it). Reopen the output after any real background.
        if (g_was_backgrounded) {
            g_was_backgrounded = false;
            host_audio_restart_output("back from the background");
        }
        host_present_suspend(false);
        return true;
    case SDL_EVENT_TERMINATING:
        fprintf(stderr, "[ios] terminating\n");
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    default:
        return false;
    }
}

bool platform_ui_keypad_wanted() {
    return !SDL_HasKeyboard();
}

bool platform_ui_touch_device() {
    return true;
}

bool platform_ui_pointer_capture_supported() {
    return false;
}

int platform_ui_default_overlay() {
    return 0;
}

void platform_ui_process_exit(int code) {
    // UIApplicationMain never returns, so the game's own Exit would leave its
    // last frame on the screen; the game asked to end, and this ends it.
    fprintf(stderr, "[ios] the game exited (%d); ending the app\n", code);
    fflush(stderr);
    exit(code);
}

// ---------------------------------------------------------------------------
// Haptics: a light impact tick for on-screen control presses, and a Core
// Haptics continuous player standing in for game rumble on a device with no
// controller. Both must run on the main thread.
// ---------------------------------------------------------------------------
namespace {
UIImpactFeedbackGenerator *g_tap_generator;        // prepared once, reused for every tap
CHHapticEngine *g_haptic_engine;                   // created once, started lazily
id<CHHapticAdvancedPatternPlayer> g_rumble_player; // the current 30 s rumble, or nil

bool device_supports_haptics() {
    static const bool supported = CHHapticEngine.capabilitiesForHardware.supportsHaptics;
    return supported;
}

// The engine, created on first use. A stop or reset (background, audio
// session interruption) drops the player, since it dies with the engine.
CHHapticEngine *haptic_engine() {
    if (!g_haptic_engine) {
        NSError *error = nil;
        g_haptic_engine = [[CHHapticEngine alloc] initAndReturnError:&error];
        if (error) {
            fprintf(stderr, "[ios] CHHapticEngine init failed: %s\n",
                    error.localizedDescription.UTF8String);
            return nil;
        }
        // Core Haptics calls both handlers on its own background queue; every
        // other read/write of g_rumble_player runs on the main queue (the
        // functions below), so hop there before touching it.
        g_haptic_engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
          dispatch_async(dispatch_get_main_queue(), ^{
            g_rumble_player = nil;
          });
        };
        g_haptic_engine.resetHandler = ^{
          dispatch_async(dispatch_get_main_queue(), ^{
            g_rumble_player = nil;
          });
        };
    }
    return g_haptic_engine;
}
} // namespace

void platform_ui_haptic_tap() {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!g_tap_generator) {
          g_tap_generator =
              [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
          [g_tap_generator prepare];
      }
      [g_tap_generator impactOccurred];
    });
}

void platform_ui_device_rumble(uint16_t low, uint16_t high) {
    const float intensity = std::max(low, high) / 65535.0f;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!device_supports_haptics())
          return;
      if (intensity <= 0.0f) {
          [g_rumble_player cancelAndReturnError:nil];
          g_rumble_player = nil;
          return;
      }
      NSError *error = nil;
      if (g_rumble_player) {
          // Already rumbling: retune it in place rather than restart it.
          CHHapticDynamicParameter *param = [[CHHapticDynamicParameter alloc]
              initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                            value:intensity
                     relativeTime:0];
          [g_rumble_player sendParameters:@[ param ] atTime:CHHapticTimeImmediate error:&error];
          if (!error)
              return;
          g_rumble_player = nil; // the player died under us; fall through and rebuild it
      }
      CHHapticEngine *engine = haptic_engine();
      if (!engine)
          return;
      [engine startAndReturnError:&error];
      if (error)
          return;
      CHHapticEventParameter *intensity_param = [[CHHapticEventParameter alloc]
          initWithParameterID:CHHapticEventParameterIDHapticIntensity
                        value:intensity];
      CHHapticEventParameter *sharpness_param = [[CHHapticEventParameter alloc]
          initWithParameterID:CHHapticEventParameterIDHapticSharpness
                        value:0.3f];
      CHHapticEvent *event =
          [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                        parameters:@[ intensity_param, sharpness_param ]
                                      relativeTime:0
                                          duration:30.0];
      CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ]
                                                              parameters:@[]
                                                                   error:&error];
      if (error)
          return;
      id<CHHapticAdvancedPatternPlayer> player = [engine createAdvancedPlayerWithPattern:pattern
                                                                                   error:&error];
      if (error || !player)
          return;
      [player startAtTime:CHHapticTimeImmediate error:&error];
      if (!error)
          g_rumble_player = player;
    });
}
