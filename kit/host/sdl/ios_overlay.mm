// ios_overlay.mm - see ios_overlay.h.
#include "ios_overlay.h"

#include "../gpu/upscale_control.h"
#include "../../mods/mods_internal.h"
#include "../../mods/settings_menu.h"
#include "../../runtime/display_seam.h"
#include "game_config.h"

#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

// NSUserDefaults keys.
NSString *const kRows = @"ios.internalRows";         // scene height in pixels; 0 native
NSString *const kFps = @"ios.frameCap";              // 30, 60, 90 or 120
NSString *const kDynamic = @"ios.dynamicResolution"; // step down while the device is hot
NSString *const kStretch = @"ios.stretch";           // fill the screen whatever the game's aspect
NSString *const kEffects = @"ios.effectsInternal";   // TEST: glow/DoF/blur before the upscale (restart)
NSString *const kSim120 = @"ios.simulation120";      // TEST: 120 Hz simulation (restart)

// The game's core mod's settings, the same store the F10 page edits (mods/settings.cpp). The few the panel
// shows are looked up by key each time it opens: the mods declare them while the game boots, after the
// panel exists, and a game without one of them simply has that row disabled.
const char *const kCoreMod = "core." RECOMP_GAME_ID;
bool mod_setting(const char *key, uint32_t *owner, int64_t *value, int64_t *min, int64_t *max) {
    const uint32_t n = mods_settings_entry_count();
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t o = 0;
        const char *mod_id = nullptr, *k = nullptr, *label = nullptr;
        int32_t kind = 0;
        int64_t v = 0, lo = 0, hi = 0;
        if (mods_settings_entry(i, &o, &mod_id, &k, &label, &kind, &v, &lo, &hi) && mod_id && k &&
            !strcmp(mod_id, kCoreMod) && !strcmp(k, key)) {
            if (owner) *owner = o;
            if (value) *value = v;
            if (min) *min = lo;
            if (max) *max = hi;
            return true;
        }
    }
    return false;
}
// Written the way the F10 page applies a change: on the host's main thread, as the player's choice (saved).
void set_mod_setting(const char *key, int64_t value) {
    std::string k = key;
    mods_menu_main_thread_call([k, value] {
        uint32_t owner = 0;
        if (!mod_setting(k.c_str(), &owner, nullptr, nullptr, nullptr))
            return;
        const PopModStatus st = mods_settings_set(owner, k.c_str(), value);
        fprintf(stderr, "[ios] overlay: %s/%s = %lld%s\n", kCoreMod, k.c_str(), (long long)value,
                st == POP_OK ? "" : " (refused)");
    });
}

constexpr int kDefaultRows = 1080;
constexpr int kMinRows = 600; // dynamic resolution never goes below this
const int kRowSteps[] = {600, 720, 900, 1080, 1440};
const int kFpsSteps[] = {30, 60, 90, 120};

std::atomic<bool> g_visible{false};

NSUserDefaults *defaults() {
    static NSUserDefaults *d = [] {
        NSUserDefaults *u = NSUserDefaults.standardUserDefaults;
        [u registerDefaults:@{kRows : @(kDefaultRows), kFps : @60, kDynamic : @YES, kStretch : @NO, kEffects : @NO,
                              kSim120 : @NO}];
        return u;
    }();
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// The panel and everything that drives it. One instance, main thread only.
// ---------------------------------------------------------------------------
@interface RecompIosOverlay : NSObject
@property(nonatomic, weak) UIWindow *window;
@property(nonatomic) UIView *root;  // full-screen dim layer; hidden when closed
@property(nonatomic) UISegmentedControl *resolution, *frameCap;
@property(nonatomic) UISwitch *dynamic, *stretch, *effects, *sim120;
@property(nonatomic) UILabel *status;
// Controls bound to core-mod settings: the control's tag indexes _modKeys.
@property(nonatomic) NSMutableArray<UIControl *> *modControls;
@property(nonatomic) CADisplayLink *rateHint; // carries the ProMotion frame-rate range
@property(nonatomic) NSTimer *poll;
@property(nonatomic) BOOL comboHeld, shortcutHeld;
@end

@implementation RecompIosOverlay {
    std::vector<int> _rows; // the resolution choices, ascending; 0 (native) last
    std::vector<int> _fps;  // the frame caps this display can show
    int _nativeRows;
    int _appliedRows;
    std::vector<std::string> _modKeys; // the core-mod key of each bound control (by its tag)
}

- (instancetype)initWithWindow:(UIWindow *)window {
    if (!(self = [super init]))
        return nil;
    _window = window;
    // The game's back buffer is the display's pixel size, landscape (the Auto resolution on iOS), so the
    // native height is the screen's shorter side in pixels.
    UIScreen *screen = window.windowScene.screen ?: window.screen;
    const CGSize px = screen.nativeBounds.size;
    _nativeRows = (int)std::min(px.width, px.height);
    for (int r : kRowSteps)
        if (r < _nativeRows)
            _rows.push_back(r);
    _rows.push_back(0);
    const NSInteger maxFps = screen.maximumFramesPerSecond > 0 ? screen.maximumFramesPerSecond : 60;
    for (int f : kFpsSteps)
        if (f <= maxFps)
            _fps.push_back(f);
    _appliedRows = -2;
    [self buildPanel];
    [self installTriggers];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(thermalChanged:)
                                               name:NSProcessInfoThermalStateDidChangeNotification
                                             object:nil];
    [self applyResolution];
    [self applyFrameCap];
    host_compositor_set_stretch([defaults() boolForKey:kStretch]);
    fprintf(stderr, "[ios] overlay: native %d rows, display up to %ld Hz, thermal %ld\n", _nativeRows, (long)maxFps,
            (long)NSProcessInfo.processInfo.thermalState);
    return self;
}

// ---- settings -------------------------------------------------------------

- (NSInteger)rowIndexFor:(int)rows {
    for (size_t i = 0; i < _rows.size(); ++i)
        if (_rows[i] == rows)
            return (NSInteger)i;
    // A saved height this display does not list (another device's): the nearest step below it, else native.
    NSInteger best = (NSInteger)_rows.size() - 1;
    for (size_t i = 0; i + 1 < _rows.size(); ++i)
        if (_rows[i] <= rows)
            best = (NSInteger)i;
    return rows > 0 ? best : (NSInteger)_rows.size() - 1;
}

- (int)userRows {
    return _rows[(size_t)[self rowIndexFor:(int)[defaults() integerForKey:kRows]]];
}

// The scene height in effect: the player's choice, stepped down one step per thermal level above nominal
// while dynamic resolution is on (critical goes straight to the floor), never below kMinRows.
- (int)effectiveRows {
    NSInteger i = [self rowIndexFor:[self userRows]];
    if ([defaults() boolForKey:kDynamic]) {
        NSInteger down = 0;
        switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateFair: down = 1; break;
        case NSProcessInfoThermalStateSerious: down = 2; break;
        case NSProcessInfoThermalStateCritical: down = (NSInteger)_rows.size(); break;
        default: break;
        }
        NSInteger floorIndex = 0;
        while (floorIndex + 1 < (NSInteger)_rows.size() && _rows[(size_t)floorIndex] > 0 &&
               _rows[(size_t)floorIndex] < kMinRows)
            ++floorIndex;
        i = std::max(floorIndex, i - down);
    }
    return _rows[(size_t)i];
}

- (void)applyResolution {
    const int rows = [self effectiveRows];
    if (rows == _appliedRows)
        return;
    _appliedRows = rows;
    host_upscale_set_rows(rows); // 0: native, no upscale
    fprintf(stderr, "[ios] overlay: internal resolution %s%d (chosen %d, thermal %ld, dynamic %s)\n",
            rows ? "" : "native ", rows ? rows : _nativeRows, [self userRows],
            (long)NSProcessInfo.processInfo.thermalState, [defaults() boolForKey:kDynamic] ? "on" : "off");
    [self refreshStatus];
}

- (void)applyFrameCap {
    int cap = (int)[defaults() integerForKey:kFps];
    if (std::find(_fps.begin(), _fps.end(), cap) == _fps.end())
        cap = 60;
    mods_display_set_fps_override(cap); // the game's Present deadline and the presenter's pacing
    // ProMotion picks its refresh from the frame-rate ranges in flight; a display link that does nothing but
    // ask for the cap keeps the panel from settling below it (or racing above it) while the game presents.
    if (!self.rateHint) {
        self.rateHint = [CADisplayLink displayLinkWithTarget:self selector:@selector(rateTick:)];
        [self.rateHint addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    self.rateHint.preferredFrameRateRange = CAFrameRateRangeMake((float)cap, (float)cap, (float)cap);
    fprintf(stderr, "[ios] overlay: frame cap %d fps\n", cap);
}

- (void)rateTick:(CADisplayLink *)link {
}

- (void)thermalChanged:(NSNotification *)note {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self applyResolution];
      [self refreshStatus];
    });
}

// ---- the panel ------------------------------------------------------------

- (UILabel *)label:(NSString *)text style:(UIFontTextStyle)style {
    UILabel *l = [UILabel new];
    l.text = text;
    l.font = [UIFont preferredFontForTextStyle:style];
    l.adjustsFontForContentSizeCategory = YES;
    l.textColor = UIColor.labelColor;
    l.numberOfLines = 0;
    return l;
}

- (UIView *)row:(NSString *)title control:(UIView *)control {
    UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[ [self label:title style:UIFontTextStyleBody], control ]];
    s.axis = UILayoutConstraintAxisHorizontal;
    s.alignment = UIStackViewAlignmentCenter;
    s.spacing = 16;
    [s.arrangedSubviews[0] setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [control setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return s;
}

- (UISwitch *)switchFor:(NSString *)key action:(SEL)action {
    UISwitch *s = [UISwitch new];
    s.on = [defaults() boolForKey:key];
    [s addTarget:self action:action forControlEvents:UIControlEventValueChanged];
    return s;
}

- (UILabel *)section:(NSString *)title {
    UILabel *l = [self label:title.uppercaseString style:UIFontTextStyleFootnote];
    l.textColor = UIColor.systemOrangeColor;
    return l;
}

// A control bound to a core-mod setting; its value is read when the panel opens (refreshModControls).
- (UIControl *)bind:(UIControl *)c key:(const char *)key {
    c.tag = (NSInteger)_modKeys.size();
    _modKeys.emplace_back(key);
    [c addTarget:self action:@selector(modChanged:) forControlEvents:UIControlEventValueChanged];
    [self.modControls addObject:c];
    return c;
}
- (UISegmentedControl *)modSegment:(const char *)key titles:(NSArray<NSString *> *)titles {
    return (UISegmentedControl *)[self bind:[[UISegmentedControl alloc] initWithItems:titles] key:key];
}
- (UISwitch *)modSwitch:(const char *)key {
    return (UISwitch *)[self bind:[UISwitch new] key:key];
}
- (void)refreshModControls {
    for (UIControl *c in self.modControls) {
        int64_t v = 0, lo = 0, hi = 0;
        const bool known = mod_setting(_modKeys[(size_t)c.tag].c_str(), nullptr, &v, &lo, &hi);
        c.enabled = known;
        if ([c isKindOfClass:UISwitch.class])
            ((UISwitch *)c).on = known && v != 0;
        else if ([c isKindOfClass:UISegmentedControl.class]) {
            UISegmentedControl *s = (UISegmentedControl *)c;
            s.selectedSegmentIndex = known && v >= 0 && v < s.numberOfSegments ? (NSInteger)v : UISegmentedControlNoSegment;
        }
    }
}
- (void)modChanged:(UIControl *)c {
    const char *key = _modKeys[(size_t)c.tag].c_str();
    if ([c isKindOfClass:UISwitch.class])
        set_mod_setting(key, ((UISwitch *)c).on ? 1 : 0);
    else if ([c isKindOfClass:UISegmentedControl.class])
        set_mod_setting(key, ((UISegmentedControl *)c).selectedSegmentIndex);
}

- (void)buildPanel {
    UIView *root = [[UIView alloc] initWithFrame:self.window.bounds];
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    root.backgroundColor = [UIColor colorWithWhite:0 alpha:0.35];
    root.hidden = YES;
    UITapGestureRecognizer *outside = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapOutside:)];
    [root addGestureRecognizer:outside];

    UIVisualEffectView *panel =
        [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterialDark]];
    panel.translatesAutoresizingMaskIntoConstraints = NO;
    panel.layer.cornerRadius = 20;
    panel.clipsToBounds = YES;
    panel.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    [root addSubview:panel];

    NSMutableArray *rowTitles = [NSMutableArray array];
    for (int r : _rows)
        [rowTitles addObject:r ? [NSString stringWithFormat:@"%dp", r] : [NSString stringWithFormat:@"Native %dp", _nativeRows]];
    self.resolution = [[UISegmentedControl alloc] initWithItems:rowTitles];
    self.resolution.selectedSegmentIndex = [self rowIndexFor:[self userRows]];
    [self.resolution addTarget:self action:@selector(resolutionChanged:) forControlEvents:UIControlEventValueChanged];

    NSMutableArray *fpsTitles = [NSMutableArray array];
    NSInteger fpsIndex = 0;
    for (size_t i = 0; i < _fps.size(); ++i) {
        [fpsTitles addObject:[NSString stringWithFormat:@"%d", _fps[i]]];
        if (_fps[i] == (int)[defaults() integerForKey:kFps])
            fpsIndex = (NSInteger)i;
    }
    self.frameCap = [[UISegmentedControl alloc] initWithItems:fpsTitles];
    self.frameCap.selectedSegmentIndex = fpsIndex;
    [self.frameCap addTarget:self action:@selector(frameCapChanged:) forControlEvents:UIControlEventValueChanged];

    self.dynamic = [self switchFor:kDynamic action:@selector(dynamicChanged:)];
    self.stretch = [self switchFor:kStretch action:@selector(stretchChanged:)];
    self.effects = [self switchFor:kEffects action:@selector(effectsChanged:)];
    self.sim120 = [self switchFor:kSim120 action:@selector(sim120Changed:)];
    self.status = [self label:@"" style:UIFontTextStyleFootnote];
    self.status.textColor = UIColor.secondaryLabelColor;
    self.modControls = [NSMutableArray array];

    UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
    [done setTitle:@"Done" forState:UIControlStateNormal];
    done.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    [done addTarget:self action:@selector(close) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *header = [[UIStackView alloc]
        initWithArrangedSubviews:@[ [self label:@"Settings (iOS)" style:UIFontTextStyleTitle3], done ]];
    header.alignment = UIStackViewAlignmentCenter;

    UILabel *hint = [self label:@"Open or close: L3+R3, ⌘S, or hold three fingers. "
                                @"Every other setting: F10 or a three-finger tap."
                          style:UIFontTextStyleCaption1];
    hint.textColor = UIColor.tertiaryLabelColor;

    NSArray *views = @[
        header,
        [self section:@"Display"],
        [self label:@"Internal resolution (MetalFX upscales the 3D scene; the HUD stays native)"
              style:UIFontTextStyleSubheadline],
        self.resolution,
        [self row:@"Frame cap (fps)" control:self.frameCap],
        [self row:@"Dynamic resolution (steps down when hot, never below 600p)" control:self.dynamic],
        [self row:@"Stretch to fill the screen" control:self.stretch],
        self.status,

        [self section:@"Graphics"],
        [self row:@"Look" control:[self modSegment:"look" titles:@[ @"Definitive", @"Xbox 360", @"PC" ]]],
        [self row:@"Detail" control:[self modSegment:"quality"
                                              titles:@[ @"Low", @"Medium", @"High", @"Ultra", @"Custom" ]]],
        [self row:@"Smooth edges (MSAA; costs GPU time and memory)"
          control:[self modSegment:"antialiasing" titles:@[ @"Off", @"2x", @"4x" ]]],
        [self row:@"Motion blur" control:[self modSwitch:"motion_blur"]],
        [self row:@"Movie-style background blur (cut-scenes)" control:[self modSwitch:"cinematic_dof"]],

        [self section:@"Controller"],
        [self row:@"PlayStation-style button layout" control:[self modSwitch:"ps2_controls"]],
        [self row:@"PlayStation button icons when a controller is used (next launch; needs "
                  @"GLOBAL/XtendedInputButtons.tpk in the game)"
          control:[self modSwitch:"ps_icons"]],

        [self section:@"World (next launch)"],
        [self row:@"Day cycle (the light moves through the day)" control:[self modSwitch:"time_of_day"]],
        [self row:@"Weather cycle" control:[self modSwitch:"weather"]],

        [self section:@"Experimental (next launch)"],
        [self row:@"Effects (glow, depth of field, motion blur) at internal resolution" control:self.effects],
        [self row:@"Simulation at 120 Hz (for the 120 cap; off = 60 Hz)" control:self.sim120],
        hint,
    ];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:views];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    [stack setCustomSpacing:4 afterView:views[2]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];
    [panel.contentView addSubview:scroll];

    UILayoutGuide *safe = root.safeAreaLayoutGuide;
    NSLayoutConstraint *preferredWidth = [panel.widthAnchor constraintEqualToConstant:620];
    preferredWidth.priority = UILayoutPriorityDefaultHigh;
    NSLayoutConstraint *fitHeight = [panel.heightAnchor constraintEqualToAnchor:stack.heightAnchor constant:40];
    fitHeight.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [panel.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [panel.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        preferredWidth,
        [panel.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-32],
        fitHeight,
        [panel.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor constant:-24],
        [scroll.topAnchor constraintEqualToAnchor:panel.contentView.topAnchor constant:20],
        [scroll.bottomAnchor constraintEqualToAnchor:panel.contentView.bottomAnchor constant:-20],
        [scroll.leadingAnchor constraintEqualToAnchor:panel.contentView.leadingAnchor constant:24],
        [scroll.trailingAnchor constraintEqualToAnchor:panel.contentView.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
    ]];
    [self.window addSubview:root];
    self.root = root;
}

- (void)refreshStatus {
    if (!self.root || self.root.hidden)
        return;
    int iw = 0, ih = 0, ow = 0, oh = 0;
    host_upscale_last_sizes(&iw, &ih, &ow, &oh);
    static const char *const thermal[] = {"nominal", "fair", "serious", "critical"};
    const NSInteger t = std::clamp<NSInteger>(NSProcessInfo.processInfo.thermalState, 0, 3);
    NSString *scene = iw ? [NSString stringWithFormat:@"3D scene %d×%d → %d×%d", iw, ih, ow, oh]
                         : (_appliedRows == 0 ? @"3D scene at native resolution"
                                              : @"3D scene: not upscaled yet (in the front end it draws native)");
    self.status.text = [NSString stringWithFormat:@"%@ · thermal %s%@", scene, thermal[t],
                                                  _appliedRows != [self userRows] ? @" (stepped down)" : @""];
}

// ---- actions --------------------------------------------------------------

- (void)resolutionChanged:(UISegmentedControl *)c {
    [defaults() setInteger:_rows[(size_t)c.selectedSegmentIndex] forKey:kRows];
    [self applyResolution];
}
- (void)frameCapChanged:(UISegmentedControl *)c {
    [defaults() setInteger:_fps[(size_t)c.selectedSegmentIndex] forKey:kFps];
    [self applyFrameCap];
}
- (void)stretchChanged:(UISwitch *)s {
    [defaults() setBool:s.on forKey:kStretch];
    host_compositor_set_stretch(s.on);
}
- (void)dynamicChanged:(UISwitch *)s {
    [defaults() setBool:s.on forKey:kDynamic];
    [self applyResolution];
    [self refreshStatus];
}
- (void)effectsChanged:(UISwitch *)s {
    [defaults() setBool:s.on forKey:kEffects];
}
- (void)sim120Changed:(UISwitch *)s {
    [defaults() setBool:s.on forKey:kSim120];
}
- (void)tapOutside:(UITapGestureRecognizer *)g {
    UIView *hit = [self.root hitTest:[g locationInView:self.root] withEvent:nil];
    if (hit == self.root)
        [self close];
}

- (void)open {
    if (!self.root.hidden)
        return;
    [self.window bringSubviewToFront:self.root];
    self.root.hidden = NO;
    g_visible = true;
    [self refreshStatus];
}
- (void)close {
    self.root.hidden = YES;
    g_visible = false;
}
- (void)toggle {
    self.root.hidden ? [self open] : [self close];
}

// ---- triggers -------------------------------------------------------------

- (void)installTriggers {
    // Three fingers held: cancels its touches when it fires, so the kit's mapper sees a cancel, not a
    // three-finger tap (its F10 page), and the game sees no click.
    UILongPressGestureRecognizer *hold = [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                                                       action:@selector(held:)];
    hold.numberOfTouchesRequired = 3;
    hold.minimumPressDuration = 0.6;
    hold.cancelsTouchesInView = YES;
    [self.window addGestureRecognizer:hold];
    // Controllers and the keyboard are polled, not observed: SDL owns their change handlers.
    self.poll = [NSTimer timerWithTimeInterval:1.0 / 30 target:self selector:@selector(pollInputs:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:self.poll forMode:NSRunLoopCommonModes];
}

- (void)held:(UILongPressGestureRecognizer *)g {
    if (g.state == UIGestureRecognizerStateBegan)
        [self toggle];
}

- (void)pollInputs:(NSTimer *)t {
    // L3+R3 on any controller: an edge, so holding both toggles once.
    BOOL combo = NO;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *pad = c.extendedGamepad;
        if (pad && pad.leftThumbstickButton.isPressed && pad.rightThumbstickButton.isPressed)
            combo = YES;
    }
    if (combo && !self.comboHeld)
        [self toggle];
    self.comboHeld = combo;
    // Cmd+S on a hardware keyboard.
    BOOL shortcut = NO;
    if (GCKeyboardInput *k = GCKeyboard.coalescedKeyboard.keyboardInput) {
        const BOOL cmd = [k buttonForKeyCode:GCKeyCodeLeftGUI].isPressed || [k buttonForKeyCode:GCKeyCodeRightGUI].isPressed;
        shortcut = cmd && [k buttonForKeyCode:GCKeyCodeKeyS].isPressed;
    }
    if (shortcut && !self.shortcutHeld)
        [self toggle];
    self.shortcutHeld = shortcut;
    // The status line, twice a second while open.
    static unsigned n = 0;
    if (++n % 15 == 0)
        [self refreshStatus];
}

@end

// ---------------------------------------------------------------------------

namespace {
RecompIosOverlay *g_overlay;
}

void ios_overlay_apply_launch_settings() {
    NSUserDefaults *d = defaults();
    // The scene markers: the core mod sends them when this is set, and the renderer's scene height then comes
    // from host_upscale_set_rows (the panel). Native until the panel applies the saved choice.
    setenv("RECOMP_REMASTER_UPSCALE", "2", 0);
    host_upscale_set_rows(0);
    if ([d boolForKey:kEffects])
        setenv("RECOMP_UPSCALE_EFFECTS_INTERNAL", "1", 0);
    if ([d boolForKey:kSim120])
        setenv("NFSMW_SIM_RATE", "120", 0);
    fprintf(stderr, "[ios] launch options: effects at internal resolution %s, simulation %s Hz\n",
            [d boolForKey:kEffects] ? "ON (test)" : "off", [d boolForKey:kSim120] ? "120 (test)" : "60");
}

void ios_overlay_install(SDL_Window *window) {
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                     SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (!uiwindow || g_overlay)
        return;
    g_overlay = [[RecompIosOverlay alloc] initWithWindow:uiwindow];
}

bool ios_overlay_visible() {
    return g_visible.load();
}
