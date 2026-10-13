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
NSString *const kStretch = @"ios.stretch";           // retired: the output has the screen's shape; forced off
NSString *const kOutput = @"ios.outputRows";        // the game's own resolution (its height); 0 the screen's
NSString *const kEffects = @"ios.effectsInternal";   // TEST: glow/DoF/blur before the upscale (restart)
NSString *const kSim120 = @"ios.simulation120";      // retired: a 120 Hz simulation crashed races; forced off
NSString *const kSeeded = @"ios.seededDetail";       // the first-run Detail level was set (once per install)
NSString *const kMaxPerf = @"ios.maxPerformance";     // Max performance mode (next launch)
NSString *const kMaxPerfSaved = @"ios.maxPerfSaved";  // the player's own values, put back when the mode goes off

// The game's core mod's settings, the same store the F10 page edits (mods/settings.cpp), looked up by key each
// time: the mods declare them while the game boots, after the panel exists.
const char *const kCoreMod = "core." RECOMP_GAME_ID;
bool mod_setting(const char *key, uint32_t *owner, int64_t *value) {
    const uint32_t n = mods_settings_entry_count();
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t o = 0;
        const char *mod_id = nullptr, *k = nullptr, *label = nullptr;
        int32_t kind = 0;
        int64_t v = 0, lo = 0, hi = 0;
        if (mods_settings_entry(i, &o, &mod_id, &k, &label, &kind, &v, &lo, &hi) && mod_id && k &&
            !strcmp(mod_id, kCoreMod) && !strcmp(k, key)) {
            if (owner)
                *owner = o;
            if (value)
                *value = v;
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
        if (!mod_setting(k.c_str(), &owner, nullptr))
            return;
        const PopModStatus st = mods_settings_set(owner, k.c_str(), value);
        fprintf(stderr, "[ios] overlay: %s/%s = %lld%s\n", kCoreMod, k.c_str(), (long long)value,
                st == POP_OK ? "" : " (refused)");
    });
}

constexpr int kMinRows = 540; // dynamic resolution never goes below this
const int kRowSteps[] = {540, 600, 640, 720, 800, 900, 1080, 1440};
// The game's output heights. 720 is the default everywhere: every pixel of the frame (menus, HUD, effects) costs
// GPU time, and the screen's full size more than doubled the frame's cost on an iPhone 17 Pro Max (30-45 fps).
const int kOutputSteps[] = {720, 900, 1080};
constexpr int kDefaultOutput = 720;
int g_launch_output = kDefaultOutput; // the output height this launch uses (ios_overlay_apply_launch_settings)
const int kFpsSteps[] = {30, 60, 90, 120};

std::atomic<bool> g_visible{false};

NSUserDefaults *defaults() {
    static NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    return d;
}

// ---- controllers ------------------------------------------------------------------------------------------

// The core mod's `pad_kind`: 1 PlayStation (DualSense, DualShock, PlayStation-edition controllers such as the
// Backbone One PlayStation Edition), 2 anything else (Xbox letters).
int controller_kind(GCController *c) {
    if (!c)
        return 0;
    NSString *cat = c.productCategory ?: @"";
    if ([cat isEqualToString:GCProductCategoryDualSense] || [cat isEqualToString:GCProductCategoryDualShock4])
        return 1;
    GCExtendedGamepad *pad = c.extendedGamepad;
    if ([pad isKindOfClass:GCDualSenseGamepad.class] || [pad isKindOfClass:GCDualShockGamepad.class])
        return 1;
    NSString *name = [NSString stringWithFormat:@"%@ %@", c.vendorName ?: @"", cat].lowercaseString;
    for (NSString *word in @[ @"playstation", @"dualsense", @"dualshock", @"ps5", @"ps4" ])
        if ([name containsString:word])
            return 1;
    return 2;
}

// The kit's controller buttons, in game.toml [controls.native] order: the numbers the shift settings hold.
const char *const kPsButtons[] = {"Square", "Cross", "Circle", "Triangle", "L1", "R1", "L2", "R2",
                                  "Create", "Options", "L3", "R3", "PS"};
const char *const kXboxButtons[] = {"X", "A", "B", "Y", "LB", "RB", "LT", "RT",
                                    "View", "Menu", "LS", "RS", "Guide"};
constexpr int kButtonCount = 13;

// The kit index of each pressed button on `pad` (bit i = index i).
uint32_t pressed_buttons(GCExtendedGamepad *pad) {
    if (!pad)
        return 0;
    GCControllerButtonInput *b[kButtonCount] = {pad.buttonX,         pad.buttonA,          pad.buttonB,
                                                pad.buttonY,         pad.leftShoulder,     pad.rightShoulder,
                                                pad.leftTrigger,     pad.rightTrigger,     pad.buttonOptions,
                                                pad.buttonMenu,      pad.leftThumbstickButton,
                                                pad.rightThumbstickButton, pad.buttonHome};
    uint32_t bits = 0;
    for (int i = 0; i < kButtonCount; ++i)
        if (b[i] && b[i].isPressed)
            bits |= 1u << i;
    return bits;
}

// The F10 Controls page's button-number rows, by their label (mods/core/<game>/mod.toml), and their keys.
struct ButtonRow {
    const char *label, *key;
};
const ButtonRow kButtonRows[] = {{"Shift up button", "shift_up"},
                                 {"Shift down button", "shift_down"},
                                 {"Shift up: second button", "shift_up_secondary"},
                                 {"Shift down: second button", "shift_down_secondary"}};
const ButtonRow *button_row(const std::string &label) {
    for (const ButtonRow &r : kButtonRows)
        if (label == r.label)
            return &r;
    return nullptr;
}

// Home's Performance switches: the Definitive look's costliest extras, which the Detail level does not touch (it sets
// only the game's own video values). Each is the core mod's own setting, as the Graphics tab shows it.
struct PerfSwitch {
    const char *key, *title;
    bool restart;
};
const PerfSwitch kPerfSwitches[] = {
    {"ssao", "Ambient occlusion (contact shadows)", false},
    {"motion_blur", "Motion blur", false},
    {"cinematic_dof", "Movie-style background blur", false},
    {"mirror_full_scenery", "Mirror: all scenery (the mirror draws the world again)", false},
    {"soft_shadows", "Softer shadows", true},
    {"mirror_hd", "Mirror: sharp HD picture", true},
};

// A DualSense, drawn (UIKit has no picture of one): white body and grips, black centre, the touchpad with its blue
// light bar, two sticks, the d-pad and the four face buttons. `size` is the width; it is two thirds as tall.
UIImage *dualsense_image(CGFloat width) {
    const CGSize size = CGSizeMake(width, width * 2 / 3);
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:size];
    return [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
      CGContextRef g = ctx.CGContext;
      const CGFloat k = width / 120; // drawn in a 120 x 80 box
      CGContextScaleCTM(g, k, k);
      UIBezierPath *body = [UIBezierPath bezierPath];
      [body moveToPoint:CGPointMake(30, 12)];
      [body addCurveToPoint:CGPointMake(90, 12) controlPoint1:CGPointMake(48, 8) controlPoint2:CGPointMake(72, 8)];
      [body addCurveToPoint:CGPointMake(114, 46) controlPoint1:CGPointMake(104, 14) controlPoint2:CGPointMake(111, 28)];
      [body addCurveToPoint:CGPointMake(104, 76) controlPoint1:CGPointMake(118, 62) controlPoint2:CGPointMake(114, 76)];
      [body addCurveToPoint:CGPointMake(84, 58) controlPoint1:CGPointMake(96, 76) controlPoint2:CGPointMake(92, 62)];
      [body addCurveToPoint:CGPointMake(36, 58) controlPoint1:CGPointMake(70, 52) controlPoint2:CGPointMake(50, 52)];
      [body addCurveToPoint:CGPointMake(16, 76) controlPoint1:CGPointMake(28, 62) controlPoint2:CGPointMake(24, 76)];
      [body addCurveToPoint:CGPointMake(6, 46) controlPoint1:CGPointMake(6, 76) controlPoint2:CGPointMake(2, 62)];
      [body addCurveToPoint:CGPointMake(30, 12) controlPoint1:CGPointMake(9, 28) controlPoint2:CGPointMake(16, 14)];
      [body closePath];
      [[UIColor colorWithWhite:0.96 alpha:1] setFill];
      [body fill];
      [[UIColor colorWithRed:0.11 green:0.11 blue:0.13 alpha:1] setFill];
      [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(34, 34, 52, 20) cornerRadius:9] fill];
      [[UIColor colorWithWhite:0.86 alpha:1] setFill];
      [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(38, 13, 44, 22) cornerRadius:5] fill];
      [[UIColor colorWithRed:0.25 green:0.55 blue:1.0 alpha:1] setFill];
      [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(37, 13, 2.5, 20) cornerRadius:1.2] fill];
      [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(80.5, 13, 2.5, 20) cornerRadius:1.2] fill];
      for (NSValue *v in @[ [NSValue valueWithCGPoint:CGPointMake(44, 47)], [NSValue valueWithCGPoint:CGPointMake(76, 47)] ]) {
          const CGPoint c = v.CGPointValue;
          [[UIColor colorWithWhite:0.30 alpha:1] setFill];
          [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - 7, c.y - 7, 14, 14)] fill];
          [[UIColor colorWithWhite:0.12 alpha:1] setFill];
          [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - 4.5, c.y - 4.5, 9, 9)] fill];
      }
      [[UIColor colorWithWhite:0.55 alpha:1] setFill];
      [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(19, 23, 6, 16) cornerRadius:1.5] fill];
      [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(14, 28, 16, 6) cornerRadius:1.5] fill];
      for (NSValue *v in @[ [NSValue valueWithCGPoint:CGPointMake(98, 23)], [NSValue valueWithCGPoint:CGPointMake(98, 37)],
                            [NSValue valueWithCGPoint:CGPointMake(91, 30)], [NSValue valueWithCGPoint:CGPointMake(105, 30)] ]) {
          const CGPoint c = v.CGPointValue;
          [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - 3, c.y - 3, 6, 6)] fill];
      }
    }];
}

// Max performance: for hotter or older phones (an iPhone 15 Pro Max held 55-60 fps with dips to 48 at "serious"); the
// aim is 60 fps at least. The Definitive look, not the PC look: the PC look never upscales (upscale.h), so dynamic
// resolution could not lower its 3D scene when the phone heats up; with what the PC look leaves out turned off here
// (Xbox 360 textures and sky, light pools, smooth leaves) it costs about the same. Detail Low (the game's own lower
// world detail and draw distance), and every costly extra off:
// ambient occlusion, bloom and glow, motion and background blur, sun rays, soft particles, sparks, denser and Stuff
// Pack smoke, lit smoke, half the rain, softer shadows, mirror detail, wet roads, water, glass, wall relief, light
// pools. The resolution stays the player's (no lower than they chose), so the car keeps its look. The player's own
// values are saved first and put back when the mode is turned off.
struct PerfValue {
    const char *key;
    int64_t value;
};
const PerfValue kMaxPerfRecipe[] = {
    {"look", 0},           {"quality", 0},          {"antialiasing", 0},       {"ssao", 0},
    {"clean_effects", 0},  {"motion_blur", 0},      {"cinematic_dof", 0},      {"sun_rays", 0},
    {"lamp_glow", 0},      {"soft_particles", 0},   {"xbox_particles", 0},     {"denser_smoke", 0},
    {"stuffpack_smoke", 0}, {"lit_smoke", 0},       {"rain_amount", 50},       {"soft_shadows", 0},
    {"fe_floor_shadows", 0}, {"mirror_shadows", 0}, {"mirror_car_shadows", 0}, {"mirror_hd", 0},
    {"mirror_full_scenery", 0}, {"mirror_lights", 0}, {"road_sheen", 0},       {"wet_roads", 0},
    {"pbr_facades", 0},    {"natural_water", 0},    {"natural_glass", 0},      {"clean_pools", 0},
    {"atmosphere", 0},     {"vehicle_lights", 0},    {"x360_textures", 0},     {"x360_sky", 0},
    {"remaster_a2c", 0},
};

// The panel's tabs; the last three are the game's settings pages.
enum Tab { TAB_HOME, TAB_GRAPHICS, TAB_MODS, TAB_CONTROLS };
int menu_tab(NSInteger t) {
    return t == TAB_GRAPHICS ? MENU_TAB_GRAPHICS : t == TAB_MODS ? MENU_TAB_MODS : MENU_TAB_CONTROLS;
}

UIVisualEffect *glass_effect(BOOL interactive) {
    if (@available(iOS 26.0, *)) {
        UIGlassEffect *g = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
        g.interactive = interactive;
        return g;
    }
    return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterialDark];
}

} // namespace

// ---------------------------------------------------------------------------
// The panel and everything that drives it. One instance, main thread only.
// ---------------------------------------------------------------------------
@interface RecompIosOverlay : NSObject
@property(nonatomic, weak) UIWindow *window;
@property(nonatomic) UIView *root;  // full-screen dim layer; hidden when closed
@property(nonatomic) UISegmentedControl *tabs;
@property(nonatomic) UIScrollView *scroll;
@property(nonatomic) UIStackView *home, *menu;
@property(nonatomic) UIStackView *footer;
@property(nonatomic) UILabel *footerStatus;
@property(nonatomic) UIButton *apply, *discard;
@property(nonatomic) UISegmentedControl *resolution, *frameCap;
@property(nonatomic) UISegmentedControl *output;
@property(nonatomic) UILabel *outputNote;
@property(nonatomic) UISwitch *dynamic, *effects;
@property(nonatomic) UILabel *status;
@property(nonatomic) NSArray<UISwitch *> *perf; // kPerfSwitches, by index (tag)
@property(nonatomic) UISwitch *maxPerf;
@property(nonatomic) UIView *notice; // the restart notice
@property(nonatomic) CADisplayLink *rateHint; // carries the ProMotion frame-rate range
@property(nonatomic) NSTimer *poll;
@property(nonatomic) BOOL comboHeld, shortcutHeld;
// The controller bubble.
@property(nonatomic) UIVisualEffectView *bubble;
@property(nonatomic) UIImageView *bubbleIcon;
@property(nonatomic) UILabel *bubbleTitle, *bubbleDetail;
@property(nonatomic) NSUInteger bubbleSerial;
- (void)toggle;
- (void)openOnTab:(NSInteger)tab;
@end

@implementation RecompIosOverlay {
    std::vector<int> _rows; // the resolution choices, ascending; 0 (native) last
    std::vector<int> _fps;  // the frame caps this display can show
    int _nativeRows; // the back buffer's height: the output resolution in effect
    int _screenRows;
    std::vector<int> _outputs; // the output choices, ascending; 0 (the screen's own) last
    int _appliedRows;
    NSInteger _tab;
    uint64_t _menuGeneration;
    std::vector<std::string> _lineKeys; // per drawn menu line: the setting key a button row assigns ("" none)
    std::string _captureKey;            // a button row waiting for a press
    uint32_t _captureHeld;              // buttons already down when the capture began
    CFTimeInterval _captureUntil;
    int _publishedKind;
}

- (instancetype)initWithWindow:(UIWindow *)window {
    if (!(self = [super init]))
        return nil;
    _window = window;
    // The game's back buffer is the display's pixel size, landscape (the Auto resolution on iOS), so the
    // native height is the screen's shorter side in pixels.
    UIScreen *screen = window.windowScene.screen ?: window.screen;
    const CGSize px = screen.nativeBounds.size;
    _screenRows = (int)std::min(px.width, px.height);
    // The back buffer is the screen's shape at the output height chosen at launch (main.cpp note_screen_size).
    _nativeRows = g_launch_output > 0 && g_launch_output < _screenRows ? g_launch_output : _screenRows;
    for (int r : kOutputSteps)
        if (r < _screenRows)
            _outputs.push_back(r);
    _outputs.push_back(0);
    for (int r : kRowSteps)
        if (r < _nativeRows)
            _rows.push_back(r);
    _rows.push_back(0);
    const NSInteger maxFps = screen.maximumFramesPerSecond > 0 ? screen.maximumFramesPerSecond : 60;
    for (int f : kFpsSteps)
        if (f <= maxFps)
            _fps.push_back(f);
    // First run: the largest step at most 85% of the output height (600p at the 720p output), which MetalFX
    // brings up to it.
    int defaultRows = _rows.front();
    for (int r : _rows)
        if (r && r <= _nativeRows * 0.85)
            defaultRows = r;
    [defaults() registerDefaults:@{kRows : @(defaultRows), kFps : @60, kDynamic : @YES, kStretch : @NO,
                                   kEffects : @YES}];
    _appliedRows = -2;
    _tab = TAB_HOME;
    [self buildPanel];
    [self buildBubble];
    [self installTriggers];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:self selector:@selector(thermalChanged:) name:NSProcessInfoThermalStateDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(controllerConnected:) name:GCControllerDidConnectNotification object:nil];
    [nc addObserver:self selector:@selector(controllerDisconnected:) name:GCControllerDidDisconnectNotification object:nil];
    [self applyResolution];
    [self applyFrameCap];
    // Stretch is retired (the output resolution has the screen's own shape, so there is nothing to stretch);
    // a value saved by an earlier build is cleared.
    [defaults() removeObjectForKey:kStretch];
    host_compositor_set_stretch(0);
    fprintf(stderr, "[ios] overlay: output %d rows (screen %d), display up to %ld Hz, thermal %ld\n", _nativeRows,
            _screenRows, (long)maxFps, (long)NSProcessInfo.processInfo.thermalState);
    return self;
}

// ---- resolution and frame cap ----------------------------------------------------------------------------

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
// while dynamic resolution is on (critical goes straight to the floor), never below kMinRows. Re-evaluated at
// every thermal change, so it steps back up as the device cools.
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

// ---- small views ------------------------------------------------------------------------------------------

- (UILabel *)label:(NSString *)text style:(UIFontTextStyle)style {
    UILabel *l = [UILabel new];
    l.text = text;
    l.font = [UIFont preferredFontForTextStyle:style];
    l.adjustsFontForContentSizeCategory = YES;
    l.textColor = UIColor.labelColor;
    l.numberOfLines = 0;
    // Text wraps rather than widening its row.
    [l setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    return l;
}

- (UILabel *)note:(NSString *)text {
    UILabel *l = [self label:text style:UIFontTextStyleCaption1];
    l.textColor = UIColor.secondaryLabelColor;
    return l;
}

- (UIView *)row:(NSString *)title control:(UIView *)control {
    return [self rowWithTitle:[self label:title style:UIFontTextStyleBody] detail:nil control:control];
}

// A title (and a detail line below it) on the left, a control on the right.
- (UIView *)rowWithTitle:(UIView *)title detail:(UIView *)detail control:(UIView *)control {
    UIView *left = title;
    if (detail) {
        UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[ title, detail ]];
        s.axis = UILayoutConstraintAxisVertical;
        s.spacing = 2;
        left = s;
    }
    UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:control ? @[ left, control ] : @[ left ]];
    s.axis = UILayoutConstraintAxisHorizontal;
    s.alignment = UIStackViewAlignmentCenter;
    s.spacing = 16;
    [left setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [left setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    // Below required: a control too wide for the row must not make UIKit break a required constraint (it may pick
    // the panel's width and squeeze every row), it wraps the title instead.
    if (control) {
        [control setContentHuggingPriority:UILayoutPriorityDefaultHigh + 1 forAxis:UILayoutConstraintAxisHorizontal];
        [control setContentCompressionResistancePriority:UILayoutPriorityDefaultHigh + 1
                                                 forAxis:UILayoutConstraintAxisHorizontal];
    }
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

- (UIButton *)button:(NSString *)title action:(SEL)action tag:(NSInteger)tag prominent:(BOOL)prominent {
    UIButtonConfiguration *c = prominent ? UIButtonConfiguration.filledButtonConfiguration
                                         : UIButtonConfiguration.grayButtonConfiguration;
    if (@available(iOS 26.0, *))
        c = prominent ? UIButtonConfiguration.prominentGlassButtonConfiguration
                      : UIButtonConfiguration.glassButtonConfiguration;
    c.title = title;
    c.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    c.buttonSize = UIButtonConfigurationSizeSmall;
    if (prominent)
        c.baseBackgroundColor = UIColor.systemOrangeColor;
    UIButton *b = [UIButton buttonWithConfiguration:c primaryAction:nil];
    b.tag = tag;
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return b;
}

// ---- the panel --------------------------------------------------------------------------------------------

- (void)buildPanel {
    UIView *root = [[UIView alloc] initWithFrame:self.window.bounds];
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    root.backgroundColor = [UIColor colorWithWhite:0 alpha:0.2];
    root.hidden = YES;
    UITapGestureRecognizer *outside = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapOutside:)];
    outside.cancelsTouchesInView = NO;
    [root addGestureRecognizer:outside];

    // Liquid Glass (iOS 26+), tinted dark enough to keep the text readable over a busy game; a dark material below.
    UIVisualEffect *material = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterialDark];
    if (@available(iOS 26.0, *)) {
        UIGlassEffect *glass = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
        glass.tintColor = [UIColor colorWithWhite:0.05 alpha:0.45];
        material = glass;
    }
    UIVisualEffectView *panel = [[UIVisualEffectView alloc] initWithEffect:material];
    panel.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 26.0, *)) {
        panel.cornerConfiguration = [UICornerConfiguration configurationWithUniformRadius:[UICornerRadius fixedRadius:32]];
    } else {
        panel.layer.cornerRadius = 24;
        panel.layer.cornerCurve = kCACornerCurveContinuous;
        panel.clipsToBounds = YES;
    }
    panel.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    [root addSubview:panel];

    // Header: the tabs and Done.
    self.tabs = [[UISegmentedControl alloc] initWithItems:@[ @"Home", @"Graphics", @"Mods", @"Controls" ]];
    self.tabs.selectedSegmentIndex = TAB_HOME;
    [self.tabs addTarget:self action:@selector(tabChanged:) forControlEvents:UIControlEventValueChanged];
    UIButton *done = [self button:@"Done" action:@selector(close) tag:0 prominent:YES];
    UIStackView *header = [[UIStackView alloc] initWithArrangedSubviews:@[ self.tabs, done ]];
    header.alignment = UIStackViewAlignmentCenter;
    header.spacing = 12;
    header.translatesAutoresizingMaskIntoConstraints = NO;

    // Footer of the F10 pages: their status line, Apply and Discard.
    self.footerStatus = [self note:@""];
    self.apply = [self button:@"Apply" action:@selector(applyTapped:) tag:0 prominent:YES];
    self.discard = [self button:@"Discard" action:@selector(discardTapped:) tag:0 prominent:NO];
    self.footer = [[UIStackView alloc] initWithArrangedSubviews:@[ self.footerStatus, self.discard, self.apply ]];
    self.footer.alignment = UIStackViewAlignmentCenter;
    self.footer.spacing = 10;
    self.footer.hidden = YES;
    [self.footerStatus setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    [self buildHome];
    self.menu = [UIStackView new];
    self.menu.axis = UILayoutConstraintAxisVertical;
    self.menu.spacing = 12;
    self.menu.hidden = YES;

    UIStackView *body = [[UIStackView alloc] initWithArrangedSubviews:@[ self.home, self.menu ]];
    body.axis = UILayoutConstraintAxisVertical;
    body.translatesAutoresizingMaskIntoConstraints = NO;
    self.scroll = [UIScrollView new];
    self.scroll.translatesAutoresizingMaskIntoConstraints = NO;
    self.scroll.alwaysBounceVertical = YES;
    [self.scroll addSubview:body];

    UIStackView *column = [[UIStackView alloc] initWithArrangedSubviews:@[ header, self.scroll, self.footer ]];
    column.axis = UILayoutConstraintAxisVertical;
    column.spacing = 14;
    column.translatesAutoresizingMaskIntoConstraints = NO;
    [panel.contentView addSubview:column];

    UILayoutGuide *safe = root.safeAreaLayoutGuide;
    // The width is the panel's own (700 points, or the screen's less a margin): nothing inside may resize it, or a
    // long line (a controller's name) squeezes every tab into a narrow column.
    NSLayoutConstraint *preferredWidth = [panel.widthAnchor constraintEqualToConstant:700];
    preferredWidth.priority = UILayoutPriorityRequired - 1;
    // As tall as the content wants, within the screen: the scroll view takes the rest.
    NSLayoutConstraint *fitHeight = [self.scroll.heightAnchor constraintEqualToAnchor:body.heightAnchor];
    fitHeight.priority = UILayoutPriorityDefaultLow;
    [NSLayoutConstraint activateConstraints:@[
        [panel.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [panel.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        preferredWidth,
        [panel.widthAnchor constraintLessThanOrEqualToConstant:700],
        [panel.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-24],
        [panel.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor constant:-16],
        fitHeight,
        [column.topAnchor constraintEqualToAnchor:panel.contentView.topAnchor constant:16],
        [column.bottomAnchor constraintEqualToAnchor:panel.contentView.bottomAnchor constant:-16],
        [column.leadingAnchor constraintEqualToAnchor:panel.contentView.leadingAnchor constant:20],
        [column.trailingAnchor constraintEqualToAnchor:panel.contentView.trailingAnchor constant:-20],
        [body.topAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.topAnchor],
        [body.bottomAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.bottomAnchor],
        [body.leadingAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.leadingAnchor],
        [body.trailingAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.trailingAnchor],
        [body.widthAnchor constraintEqualToAnchor:self.scroll.frameLayoutGuide.widthAnchor],
    ]];
    [self.window addSubview:root];
    self.root = root;
}

- (void)buildHome {
    NSMutableArray *rowTitles = [NSMutableArray array];
    for (int r : _rows)
        [rowTitles addObject:r ? [NSString stringWithFormat:@"%dp", r] : @"Native · MetalFX off"];
    self.resolution = [[UISegmentedControl alloc] initWithItems:rowTitles];
    self.resolution.apportionsSegmentWidthsByContent = YES;
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
    self.effects = [self switchFor:kEffects action:@selector(effectsChanged:)];
    self.status = [self note:@""];

    NSMutableArray *outputTitles = [NSMutableArray array];
    NSInteger outputIndex = (NSInteger)_outputs.size() - 1;
    const int savedOutput = (int)[defaults() integerForKey:kOutput];
    for (size_t i = 0; i < _outputs.size(); ++i) {
        const int r = _outputs[i];
        [outputTitles addObject:r ? [NSString stringWithFormat:@"%dp", r] : [NSString stringWithFormat:@"Screen %dp", _screenRows]];
        if (r == savedOutput)
            outputIndex = (NSInteger)i;
    }
    self.output = [[UISegmentedControl alloc] initWithItems:outputTitles];
    self.output.selectedSegmentIndex = outputIndex;
    [self.output addTarget:self action:@selector(outputChanged:) forControlEvents:UIControlEventValueChanged];
    self.outputNote = [self note:@""];
    [self updateOutputNote];

    NSMutableArray *perf = [NSMutableArray array], *perfRows = [NSMutableArray array];
    for (size_t i = 0; i < sizeof kPerfSwitches / sizeof kPerfSwitches[0]; ++i) {
        UISwitch *sw = [UISwitch new];
        sw.tag = (NSInteger)i;
        [sw addTarget:self action:@selector(perfChanged:) forControlEvents:UIControlEventValueChanged];
        [perf addObject:sw];
        NSString *title = [NSString stringWithUTF8String:kPerfSwitches[i].title];
        if (kPerfSwitches[i].restart)
            title = [title stringByAppendingString:@" (next launch)"];
        [perfRows addObject:[self row:title control:sw]];
    }
    self.perf = perf;
    self.maxPerf = [self switchFor:kMaxPerf action:@selector(maxPerfChanged:)];
    [perfRows insertObject:[self rowWithTitle:[self label:@"Max performance (experimental)" style:UIFontTextStyleHeadline]
                                       detail:[self note:@"For hotter or older devices, aiming at 60 fps: Low detail "
                                                         @"with every heavy effect off (shadows, smoke, particles, "
                                                         @"bloom, blur, mirror detail) and dynamic resolution on. The "
                                                         @"car keeps its HD textures and your resolution stays as you "
                                                         @"set it. Your own settings come back when you turn it off. "
                                                         @"Restart to apply."]
                                      control:self.maxPerf]
                   atIndex:0];
    UIStackView *perfStack = [[UIStackView alloc] initWithArrangedSubviews:perfRows];
    perfStack.axis = UILayoutConstraintAxisVertical;
    perfStack.spacing = 12;

    UILabel *hint = [self note:@"Open or close: tap or hold three fingers, L3+R3 on a controller, or ⌘S."];
    hint.textColor = UIColor.tertiaryLabelColor;

    NSArray *views = @[
        [self section:@"Display"],
        [self label:@"Output resolution: the game's own resolution, shaped to the screen and scaled to fit. "
                    @"Higher is sharper and slower; 720p keeps an iPhone at 60 fps and cool."
              style:UIFontTextStyleSubheadline],
        self.output,
        self.outputNote,
        [self label:@"3D resolution: MetalFX upscales the 3D scene to the output; the HUD and menus stay at the "
                    @"output resolution. Native draws the scene at the output resolution, without MetalFX."
              style:UIFontTextStyleSubheadline],
        self.resolution,
        [self row:@"Frame cap (fps)" control:self.frameCap],
        [self rowWithTitle:[self label:@"Dynamic resolution" style:UIFontTextStyleBody]
                    detail:[self note:@"Steps the 3D scene down as the device heats up (fair: one step, serious: "
                                      @"two, critical: 540p) and back up as it cools."]
                   control:self.dynamic],
        self.status,
        [self section:@"Performance"],
        [self note:@"The Definitive look's heaviest extras; the Detail level leaves these on. Turning them off keeps "
                   @"the phone cooler. The PC look has none of them."],
        perfStack,
        [self section:@"Experimental (next launch)"],
        [self row:@"Effects (glow, depth of field, motion blur) at the 3D resolution" control:self.effects],
        hint,
    ];
    self.home = [[UIStackView alloc] initWithArrangedSubviews:views];
    self.home.axis = UILayoutConstraintAxisVertical;
    self.home.spacing = 12;
    [self.home setCustomSpacing:6 afterView:views[1]];
    [self.home setCustomSpacing:4 afterView:views[2]];
    [self.home setCustomSpacing:6 afterView:views[4]];
}

- (void)refreshStatus {
    if (!self.root || self.root.hidden || _tab != TAB_HOME)
        return;
    int iw = 0, ih = 0, ow = 0, oh = 0;
    host_upscale_last_sizes(&iw, &ih, &ow, &oh);
    static const char *const thermal[] = {"nominal", "fair", "serious", "critical"};
    const NSInteger t = std::clamp<NSInteger>(NSProcessInfo.processInfo.thermalState, 0, 3);
    NSString *scene = iw ? [NSString stringWithFormat:@"3D scene %d×%d → %d×%d (MetalFX)", iw, ih, ow, oh]
                         : (_appliedRows == 0 ? @"3D scene at native resolution (MetalFX off)"
                                              : @"3D scene: not upscaled yet (in the front end it draws native)");
    NSString *stepped = _appliedRows != [self userRows]
                            ? [NSString stringWithFormat:@" · stepped down to %dp", _appliedRows]
                            : @"";
    self.status.text = [NSString stringWithFormat:@"%@ · thermal %s%@", scene, thermal[t], stepped];
}

// ---- the F10 pages, drawn natively ------------------------------------------------------------------------

- (void)showTab:(NSInteger)tab {
    _tab = tab;
    _captureKey.clear();
    const BOOL home = tab == TAB_HOME;
    self.home.hidden = !home;
    self.menu.hidden = home;
    self.footer.hidden = home;
    if (home) {
        mods_menu_close_headless();
        [self refreshPerf];
        [self refreshStatus];
    } else {
        _menuGeneration = 0;
        [self rebuildMenu];
    }
    [self.scroll setContentOffset:CGPointZero animated:NO];
}

// The page's lines, as the F10 menu lists them. Rebuilt whenever its content changes (each action, and the
// poll while it shows); the scroll position is kept.
- (void)rebuildMenu {
    for (UIView *v in self.menu.arrangedSubviews) {
        [self.menu removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    _lineKeys.clear();
    MenuSnapshot s;
    if (!mods_menu_open_headless(menu_tab(_tab)) || !mods_menu_snapshot(&s)) {
        [self.menu addArrangedSubview:[self note:mods_menu_enabled()
                                                     ? @"These settings are busy. Close this panel and open it again."
                                                     : @"This game has no settings pages."]];
        self.footer.hidden = YES;
        return;
    }
    _menuGeneration = s.generation;
    if (_tab == TAB_CONTROLS)
        [self addControllerCard];

    NSString *(^str)(const std::string &) = ^NSString *(const std::string &v) {
      return [NSString stringWithUTF8String:v.c_str()] ?: @"";
    };
    BOOL applyEnabled = NO, discardEnabled = NO;
    for (size_t i = 0; i < s.lines.size(); ++i) {
        const MenuLine &l = s.lines[i];
        _lineKeys.emplace_back();
        const NSInteger tag = (NSInteger)i;
        if (l.kind == MenuLine::HEADER) {
            if (l.label == "Changes")
                continue; // Apply and Discard live in the footer
            UILabel *h = [self section:str(l.label)];
            [self.menu addArrangedSubview:h];
            continue;
        }
        if (l.kind == MenuLine::ACTION) {
            if (l.label == "Apply") {
                applyEnabled = l.enabled;
                continue;
            }
            if (l.label == "Discard") {
                discardEnabled = l.enabled;
                continue;
            }
            UIButton *b = [self button:str(l.label) action:@selector(activateLine:) tag:tag prominent:NO];
            b.enabled = l.enabled;
            UIStackView *wrap = [[UIStackView alloc] initWithArrangedSubviews:@[ b, [UIView new] ]];
            [self.menu addArrangedSubview:wrap];
            continue;
        }
        // A row: its title (a dot when a change waits for Apply), its help, and its control.
        NSString *title = l.pending ? [@"● " stringByAppendingString:str(l.label)] : str(l.label);
        UILabel *titleLabel = [self label:title style:UIFontTextStyleBody];
        titleLabel.enabled = l.enabled;
        NSString *help = str(l.help);
        if (l.timing != "Applies now" && !l.timing.empty())
            help = help.length ? [NSString stringWithFormat:@"%@ %@.", help, str(l.timing)] : str(l.timing);
        UILabel *detail = nil;
        if (help.length) {
            detail = [self note:help];
            detail.numberOfLines = 3;
        }
        UIView *control = nil;
        if (const ButtonRow *br = button_row(l.label)) {
            _lineKeys.back() = br->key;
            control = [self buttonRowControl:br line:tag];
        } else if (l.adjustable && (l.value == "On" || l.value == "Off")) {
            UISwitch *sw = [UISwitch new];
            sw.on = l.value == "On";
            sw.tag = tag;
            sw.enabled = l.enabled;
            [sw addTarget:self action:@selector(switchLine:) forControlEvents:UIControlEventValueChanged];
            control = sw;
        } else if (l.adjustable) {
            control = [self stepperFor:str(l.value) line:tag enabled:l.enabled];
        } else {
            UIButton *b = [self button:l.value.empty() || l.value == ">" ? @"Open" : str(l.value)
                                action:@selector(activateLine:)
                                   tag:tag
                             prominent:NO];
            b.enabled = l.enabled;
            control = b;
        }
        [self.menu addArrangedSubview:[self rowWithTitle:titleLabel detail:detail control:control]];
    }
    self.apply.enabled = applyEnabled;
    self.discard.enabled = discardEnabled;
    self.footerStatus.text = str(s.status);
    self.footer.hidden = !applyEnabled && !discardEnabled && s.status.empty();
}

// ‹ value ›: the value itself steps forward (wrapping), the arrows step either way.
- (UIView *)stepperFor:(NSString *)value line:(NSInteger)tag enabled:(BOOL)enabled {
    UIButtonConfiguration *(^plain)(NSString *) = ^UIButtonConfiguration *(NSString *symbol) {
      UIButtonConfiguration *c = UIButtonConfiguration.plainButtonConfiguration;
      c.image = [UIImage systemImageNamed:symbol];
      c.baseForegroundColor = UIColor.systemOrangeColor;
      return c;
    };
    UIButton *less = [UIButton buttonWithConfiguration:plain(@"chevron.left") primaryAction:nil];
    UIButton *more = [UIButton buttonWithConfiguration:plain(@"chevron.right") primaryAction:nil];
    UIButton *v = [self button:value action:@selector(activateLine:) tag:tag prominent:NO];
    less.tag = more.tag = tag;
    [less addTarget:self action:@selector(decreaseLine:) forControlEvents:UIControlEventTouchUpInside];
    [more addTarget:self action:@selector(increaseLine:) forControlEvents:UIControlEventTouchUpInside];
    for (UIButton *b in @[ less, v, more ])
        b.enabled = enabled;
    UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[ less, v, more ]];
    s.alignment = UIStackViewAlignmentCenter;
    s.spacing = 2;
    return s;
}

- (void)lineAction:(NSInteger)line action:(int)action {
    if (!mods_menu_open_headless(menu_tab(_tab)))
        return;
    mods_menu_pointer_line(menu_tab(_tab), (size_t)line, action);
    [self rebuildKeepingScroll];
}
- (void)rebuildKeepingScroll {
    const CGPoint offset = self.scroll.contentOffset;
    [self rebuildMenu];
    [self.scroll layoutIfNeeded];
    const CGFloat maxY = std::max<CGFloat>(0, self.scroll.contentSize.height - self.scroll.bounds.size.height);
    [self.scroll setContentOffset:CGPointMake(0, std::min(offset.y, maxY)) animated:NO];
}
- (void)activateLine:(UIButton *)b {
    [self lineAction:b.tag action:MENU_POINT_ACTIVATE];
}
- (void)decreaseLine:(UIButton *)b {
    [self lineAction:b.tag action:MENU_POINT_DECREASE];
}
- (void)increaseLine:(UIButton *)b {
    [self lineAction:b.tag action:MENU_POINT_INCREASE];
}
- (void)switchLine:(UISwitch *)s {
    [self lineAction:s.tag action:s.on ? MENU_POINT_INCREASE : MENU_POINT_DECREASE];
}
- (void)applyTapped:(UIButton *)b {
    [self footerAction:"Apply"];
}
- (void)discardTapped:(UIButton *)b {
    [self footerAction:"Discard"];
}
- (void)footerAction:(const char *)label {
    MenuSnapshot s;
    if (!mods_menu_open_headless(menu_tab(_tab)) || !mods_menu_snapshot(&s))
        return;
    for (size_t i = 0; i < s.lines.size(); ++i)
        if (s.lines[i].kind == MenuLine::ACTION && s.lines[i].label == label) {
            mods_menu_pointer_line(menu_tab(_tab), i, MENU_POINT_PRESS);
            break;
        }
    [self rebuildKeepingScroll];
}

// ---- Controls: the controller, and buttons assigned by pressing them --------------------------------------

- (void)addControllerCard {
    GCController *c = GCController.current ?: GCController.controllers.firstObject;
    NSString *title = c ? (c.vendorName ?: @"Controller") : @"No controller connected";
    NSString *detail;
    if (c) {
        NSString *kind = controller_kind(c) == 1 ? @"PlayStation buttons" : @"Xbox buttons";
        GCDeviceBattery *bat = c.battery;
        detail = bat && bat.batteryLevel > 0
                     ? [NSString stringWithFormat:@"%@ · battery %d%%", kind, (int)lround(bat.batteryLevel * 100)]
                     : kind;
    } else {
        detail = @"Pair one in Settings > General > Game Controller, or plug one in.";
    }
    // A DualSense for PlayStation controllers (and when none is connected), the controller symbol for the rest;
    // a fixed size either way, so the image never stretches to fill the row.
    UIImageView *icon = [UIImageView new];
    if (!c || controller_kind(c) == 1) {
        icon.image = dualsense_image(72);
    } else {
        icon.image = [UIImage systemImageNamed:@"gamecontroller.fill"];
        icon.tintColor = UIColor.systemOrangeColor;
        icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:34];
    }
    icon.contentMode = UIViewContentModeScaleAspectFit;
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[ [icon.widthAnchor constraintEqualToConstant:72],
                                               [icon.heightAnchor constraintEqualToConstant:48] ]];
    [icon setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView *card = (UIStackView *)[self rowWithTitle:[self label:title style:UIFontTextStyleHeadline]
                                                   detail:[self note:detail]
                                                  control:nil];
    [card insertArrangedSubview:icon atIndex:0];
    card.spacing = 12;
    [self.menu addArrangedSubview:[self section:@"Controller"]];
    [self.menu addArrangedSubview:card];
    [self.menu addArrangedSubview:[self note:@"Driving buttons (accelerate, brake, nitrous...) are remapped in the "
                                             @"game's own Options > Controls. The shift buttons below: tap Assign, "
                                             @"then press the button."]];
}

- (NSString *)buttonName:(int64_t)index {
    if (index < 0 || index >= kButtonCount)
        return @"Off";
    const int style = [self glyphStyle];
    return [NSString stringWithUTF8String:style == 2 ? kXboxButtons[index] : kPsButtons[index]];
}
// The button names to show: the icon style the menus use (pad_glyphs, else the connected controller).
- (int)glyphStyle {
    int64_t choice = 0;
    mod_setting("pad_glyphs", nullptr, &choice);
    if (choice == 1 || choice == 2)
        return (int)choice;
    const int kind = controller_kind(GCController.current ?: GCController.controllers.firstObject);
    return kind == 2 ? 2 : 1;
}

- (UIView *)buttonRowControl:(const ButtonRow *)row line:(NSInteger)tag {
    int64_t v = -1;
    mod_setting(row->key, nullptr, &v);
    const BOOL capturing = _captureKey == row->key;
    UILabel *value = [self label:capturing ? @"Press a button…" : [self buttonName:v] style:UIFontTextStyleBody];
    value.textColor = capturing ? UIColor.systemOrangeColor : UIColor.secondaryLabelColor;
    UIButton *assign = [self button:capturing ? @"Cancel" : @"Assign" action:@selector(assignTapped:) tag:tag prominent:!capturing];
    UIButton *off = [self button:@"Off" action:@selector(clearTapped:) tag:tag prominent:NO];
    off.enabled = v >= 0 && !capturing;
    UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[ value, assign, off ]];
    s.alignment = UIStackViewAlignmentCenter;
    s.spacing = 8;
    return s;
}

- (void)assignTapped:(UIButton *)b {
    const size_t i = (size_t)b.tag;
    if (i >= _lineKeys.size() || _lineKeys[i].empty())
        return;
    if (_captureKey == _lineKeys[i]) {
        _captureKey.clear();
    } else {
        GCController *c = GCController.current ?: GCController.controllers.firstObject;
        _captureKey = _lineKeys[i];
        _captureHeld = pressed_buttons(c.extendedGamepad);
        _captureUntil = CACurrentMediaTime() + 8;
    }
    [self rebuildKeepingScroll];
}
- (void)clearTapped:(UIButton *)b {
    const size_t i = (size_t)b.tag;
    if (i >= _lineKeys.size() || _lineKeys[i].empty())
        return;
    [self setButtonSetting:_lineKeys[i] value:-1];
}
- (void)setButtonSetting:(const std::string &)key value:(int64_t)v {
    // Runs now when this is the host's main thread (it is on iOS), else at the next game frame; the poll redraws
    // the row once the page's content shows the change.
    set_mod_setting(key.c_str(), v);
    _captureKey.clear();
    [self rebuildKeepingScroll];
}

// From the poll: the first button newly pressed while a row waits for one.
- (void)pollCapture {
    if (_captureKey.empty())
        return;
    if (CACurrentMediaTime() > _captureUntil) {
        _captureKey.clear();
        [self rebuildKeepingScroll];
        return;
    }
    uint32_t now = 0;
    for (GCController *c in GCController.controllers)
        now |= pressed_buttons(c.extendedGamepad);
    _captureHeld &= now; // a button released since the capture began may be pressed again
    const uint32_t fresh = now & ~_captureHeld;
    for (int i = 0; i < kButtonCount; ++i)
        if (fresh & (1u << i)) {
            [self setButtonSetting:std::string(_captureKey) value:i];
            return;
        }
}

// ---- the controller bubble --------------------------------------------------------------------------------

- (void)buildBubble {
    UIVisualEffectView *b = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    b.userInteractionEnabled = NO;
    b.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    if (@available(iOS 26.0, *))
        b.cornerConfiguration = [UICornerConfiguration capsuleConfiguration];
    else {
        b.layer.cornerRadius = 28;
        b.clipsToBounds = YES;
    }
    b.alpha = 0;
    self.bubbleIcon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"gamecontroller.fill"]];
    self.bubbleIcon.tintColor = UIColor.whiteColor;
    self.bubbleIcon.contentMode = UIViewContentModeScaleAspectFit;
    self.bubbleIcon.preferredSymbolConfiguration =
        [UIImageSymbolConfiguration configurationWithPointSize:24 weight:UIImageSymbolWeightSemibold];
    self.bubbleTitle = [self label:@"" style:UIFontTextStyleHeadline];
    self.bubbleTitle.numberOfLines = 1;
    self.bubbleDetail = [self note:@""];
    self.bubbleDetail.numberOfLines = 1;
    // The bubble sizes itself to its text (up to 60% of the screen): its labels keep their width, unlike the panel's,
    // which wrap ("Backbone One PlayStation® Edition" read "Back...").
    for (UILabel *l in @[ self.bubbleTitle, self.bubbleDetail ])
        [l setContentCompressionResistancePriority:UILayoutPriorityRequired - 1 forAxis:UILayoutConstraintAxisHorizontal];
    self.bubbleTitle.lineBreakMode = NSLineBreakByTruncatingMiddle;
    UIStackView *text = [[UIStackView alloc] initWithArrangedSubviews:@[ self.bubbleTitle, self.bubbleDetail ]];
    text.axis = UILayoutConstraintAxisVertical;
    text.spacing = 1;
    UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[ self.bubbleIcon, text ]];
    s.alignment = UIStackViewAlignmentCenter;
    s.spacing = 12;
    s.translatesAutoresizingMaskIntoConstraints = NO;
    [b.contentView addSubview:s];
    [self.window addSubview:b];
    [NSLayoutConstraint activateConstraints:@[
        [b.topAnchor constraintEqualToAnchor:self.window.safeAreaLayoutGuide.topAnchor constant:10],
        [b.centerXAnchor constraintEqualToAnchor:self.window.centerXAnchor],
        [b.widthAnchor constraintLessThanOrEqualToAnchor:self.window.widthAnchor multiplier:0.6],
        [s.topAnchor constraintEqualToAnchor:b.contentView.topAnchor constant:10],
        [s.bottomAnchor constraintEqualToAnchor:b.contentView.bottomAnchor constant:-10],
        [s.leadingAnchor constraintEqualToAnchor:b.contentView.leadingAnchor constant:18],
        [s.trailingAnchor constraintEqualToAnchor:b.contentView.trailingAnchor constant:-22],
        [self.bubbleIcon.widthAnchor constraintEqualToConstant:32],
    ]];
    self.bubble = b;
}

- (void)showBubbleFor:(GCController *)c connected:(BOOL)connected {
    NSString *name = c.vendorName.length ? c.vendorName : @"Controller";
    NSString *detail = connected ? @"Connected" : @"Disconnected";
    if (connected) {
        GCDeviceBattery *bat = c.battery;
        if (bat && bat.batteryLevel > 0)
            detail = [NSString stringWithFormat:@"Connected · %d%%", (int)lround(bat.batteryLevel * 100)];
    }
    NSString *symbol = @"gamecontroller.fill";
    const int kind = controller_kind(c);
    if (kind == 1 && [UIImage systemImageNamed:@"playstation.logo"])
        symbol = @"playstation.logo";
    else if (kind == 2 && [c.productCategory isEqualToString:GCProductCategoryXboxOne] &&
             [UIImage systemImageNamed:@"xbox.logo"])
        symbol = @"xbox.logo";
    self.bubbleIcon.image = [UIImage systemImageNamed:symbol];
    self.bubbleIcon.tintColor = connected ? UIColor.whiteColor : UIColor.secondaryLabelColor;
    self.bubbleTitle.text = name;
    self.bubbleDetail.text = detail;
    [self.window bringSubviewToFront:self.bubble];
    const NSUInteger serial = ++self.bubbleSerial;
    self.bubble.transform = CGAffineTransformMakeTranslation(0, -24);
    [UIView animateWithDuration:0.45
                          delay:0
         usingSpringWithDamping:0.75
          initialSpringVelocity:0.5
                        options:UIViewAnimationOptionBeginFromCurrentState
                     animations:^{
                       self.bubble.alpha = 1;
                       self.bubble.transform = CGAffineTransformIdentity;
                     }
                     completion:nil];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
      if (serial != self.bubbleSerial)
          return; // a newer bubble is showing
      [UIView animateWithDuration:0.35
                       animations:^{
                         self.bubble.alpha = 0;
                         self.bubble.transform = CGAffineTransformMakeTranslation(0, -24);
                       }];
    });
}

- (void)controllerConnected:(NSNotification *)note {
    GCController *c = note.object;
    dispatch_async(dispatch_get_main_queue(), ^{
      fprintf(stderr, "[ios] controller connected: %s (%s, kind %d)\n", c.vendorName.UTF8String ?: "?",
              c.productCategory.UTF8String ?: "?", controller_kind(c));
      [self showBubbleFor:c connected:YES];
      [self publishKind];
      if (!self.root.hidden && self->_tab == TAB_CONTROLS)
          [self rebuildKeepingScroll];
    });
}
- (void)controllerDisconnected:(NSNotification *)note {
    GCController *c = note.object;
    dispatch_async(dispatch_get_main_queue(), ^{
      [self showBubbleFor:c connected:NO];
      [self publishKind];
      if (!self.root.hidden && self->_tab == TAB_CONTROLS)
          [self rebuildKeepingScroll];
    });
}

// The core mod's `pad_kind`, for the menus' button icons: the controller in use (the last one used, else any).
// Also from the poll, since controllers connected before the game declared the setting are only told then.
- (void)publishKind {
    const int kind = controller_kind(GCController.current ?: GCController.controllers.firstObject);
    if (!kind || kind == _publishedKind)
        return;
    int64_t stored = -1;
    if (!mod_setting("pad_kind", nullptr, &stored))
        return; // not declared yet: the poll tries again
    _publishedKind = kind;
    if (stored != kind)
        set_mod_setting("pad_kind", kind);
}

// A fresh profile has the manifest's defaults, the heaviest Detail level (Custom, every feature on): on its first
// run on iOS the Detail level is Low instead (owner, 12 Oct: Low holds 55-70 fps on an iPhone 17 Pro Max). Once:
// a later choice is the player's. Waits for the core mod to declare the setting.
- (void)seedFirstRun {
    if ([defaults() boolForKey:kSeeded])
        return;
    int64_t quality = -1;
    if (!mod_setting("quality", nullptr, &quality))
        return;
    [defaults() setBool:YES forKey:kSeeded];
    if (quality == 0)
        return;
    set_mod_setting("quality", 0);
}

// ---- actions ----------------------------------------------------------------------------------------------

- (void)tabChanged:(UISegmentedControl *)c {
    [self showTab:c.selectedSegmentIndex];
}
- (void)resolutionChanged:(UISegmentedControl *)c {
    [defaults() setInteger:_rows[(size_t)c.selectedSegmentIndex] forKey:kRows];
    [self applyResolution];
}
- (void)frameCapChanged:(UISegmentedControl *)c {
    [defaults() setInteger:_fps[(size_t)c.selectedSegmentIndex] forKey:kFps];
    [self applyFrameCap];
}
- (void)dynamicChanged:(UISwitch *)s {
    [defaults() setBool:s.on forKey:kDynamic];
    [self applyResolution];
    [self refreshStatus];
}
- (void)effectsChanged:(UISwitch *)s {
    [defaults() setBool:s.on forKey:kEffects];
}
- (void)maxPerfChanged:(UISwitch *)sw {
    NSUserDefaults *d = defaults();
    [d setBool:sw.on forKey:kMaxPerf];
    if (sw.on) {
        // Keep the player's own values once (turning it on twice does not overwrite them with the mode's).
        if (![d dictionaryForKey:kMaxPerfSaved]) {
            NSMutableDictionary *saved = [NSMutableDictionary dictionary];
            for (const PerfValue &v : kMaxPerfRecipe) {
                int64_t cur = 0;
                if (mod_setting(v.key, nullptr, &cur))
                    saved[@(v.key)] = @(cur);
            }
            [d setObject:saved forKey:kMaxPerfSaved];
        }
        for (const PerfValue &v : kMaxPerfRecipe)
            set_mod_setting(v.key, v.value);
        // Heat is then met by the 3D resolution stepping down (to 540p at the most), not by dropped frames.
        [d setBool:YES forKey:kDynamic];
        self.dynamic.on = YES;
        [self applyResolution];
    } else {
        NSDictionary *saved = [d dictionaryForKey:kMaxPerfSaved];
        for (NSString *k in saved)
            set_mod_setting(k.UTF8String, [saved[k] longLongValue]);
        [d removeObjectForKey:kMaxPerfSaved];
    }
    fprintf(stderr, "[ios] overlay: max performance %s (restart to apply)\n", sw.on ? "on" : "off");
    [self refreshPerf];
    [self showRestartNotice:sw.on];
}

// A Liquid Glass card in the middle of the screen: the mode's settings take full effect after a restart.
- (void)showRestartNotice:(BOOL)on {
    [self.notice removeFromSuperview];
    UIView *dim = [[UIView alloc] initWithFrame:self.window.bounds];
    dim.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    dim.backgroundColor = [UIColor colorWithWhite:0 alpha:0.25];
    UIVisualEffectView *card = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    if (@available(iOS 26.0, *)) {
        card.cornerConfiguration = [UICornerConfiguration configurationWithUniformRadius:[UICornerRadius fixedRadius:30]];
    } else {
        card.layer.cornerRadius = 30;
        card.clipsToBounds = YES;
    }
    UIImageView *icon = [[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:on ? @"bolt.circle.fill" : @"arrow.counterclockwise.circle.fill"]];
    icon.tintColor = UIColor.systemOrangeColor;
    icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:44];
    icon.contentMode = UIViewContentModeScaleAspectFit;
    UILabel *title = [self label:on ? @"Max performance is on" : @"Max performance is off" style:UIFontTextStyleTitle2];
    title.textAlignment = NSTextAlignmentCenter;
    UILabel *body = [self label:@"Restart the game so every setting applies: close it and open it again."
                          style:UIFontTextStyleBody];
    body.textAlignment = NSTextAlignmentCenter;
    body.textColor = UIColor.secondaryLabelColor;
    UIButton *close = [self button:@"Close game now" action:@selector(closeForRestart) tag:0 prominent:YES];
    UIButton *later = [self button:@"Later" action:@selector(dismissNotice) tag:0 prominent:NO];
    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[ later, close ]];
    buttons.spacing = 12;
    buttons.distribution = UIStackViewDistributionFillEqually;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[ icon, title, body, buttons ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentFill;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [card.contentView addSubview:stack];
    [dim addSubview:card];
    [NSLayoutConstraint activateConstraints:@[
        [card.centerXAnchor constraintEqualToAnchor:dim.centerXAnchor],
        [card.centerYAnchor constraintEqualToAnchor:dim.centerYAnchor],
        [card.widthAnchor constraintEqualToConstant:380],
        [stack.topAnchor constraintEqualToAnchor:card.contentView.topAnchor constant:26],
        [stack.bottomAnchor constraintEqualToAnchor:card.contentView.bottomAnchor constant:-22],
        [stack.leadingAnchor constraintEqualToAnchor:card.contentView.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:card.contentView.trailingAnchor constant:-24],
    ]];
    [self.window addSubview:dim];
    self.notice = dim;
    dim.alpha = 0;
    card.transform = CGAffineTransformMakeScale(0.9, 0.9);
    [UIView animateWithDuration:0.4
                          delay:0
         usingSpringWithDamping:0.8
          initialSpringVelocity:0.5
                        options:0
                     animations:^{
                       dim.alpha = 1;
                       card.transform = CGAffineTransformIdentity;
                     }
                     completion:nil];
}
- (void)dismissNotice {
    UIView *n = self.notice;
    self.notice = nil;
    [UIView animateWithDuration:0.25 animations:^{ n.alpha = 0; } completion:^(BOOL done) { [n removeFromSuperview]; }];
}
// The settings are saved first (the store writes each change, and once more here); iOS has no relaunch, so the
// player opens the game again from the Home Screen or LiveContainer.
- (void)closeForRestart {
    mods_settings_save();
    [defaults() synchronize];
    fprintf(stderr, "[ios] overlay: closing for a restart (max performance)\n");
    exit(0);
}

- (void)perfChanged:(UISwitch *)sw {
    set_mod_setting(kPerfSwitches[sw.tag].key, sw.on ? 1 : 0);
}
// The switches show the settings as they are (the Graphics tab or F10 may have changed them); not while one is
// being changed, which the poll's next read shows anyway.
- (void)refreshPerf {
    for (UISwitch *sw in self.perf) {
        int64_t v = 0;
        const bool known = mod_setting(kPerfSwitches[sw.tag].key, nullptr, &v);
        sw.enabled = known;
        if (known && !sw.tracking)
            sw.on = v != 0;
    }
}
- (void)outputChanged:(UISegmentedControl *)c {
    [defaults() setInteger:_outputs[(size_t)c.selectedSegmentIndex] forKey:kOutput];
    [self updateOutputNote];
}
- (void)updateOutputNote {
    const int chosen = (int)[defaults() integerForKey:kOutput];
    const int next = chosen > 0 && chosen < _screenRows ? chosen : _screenRows;
    self.outputNote.text = next == _nativeRows
                               ? [NSString stringWithFormat:@"Now %dp.", _nativeRows]
                               : [NSString stringWithFormat:@"Now %dp; %dp from the next launch.", _nativeRows, next];
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
    [self showTab:_tab];
}
- (void)close {
    _captureKey.clear();
    mods_menu_close_headless(); // as closing the F10 page: an unapplied Graphics change is discarded
    self.root.hidden = YES;
    g_visible = false;
}
- (void)openOnTab:(NSInteger)tab {
    _tab = tab;
    self.tabs.selectedSegmentIndex = tab;
    if (self.root.hidden)
        [self open];
    else
        [self showTab:tab];
}
- (void)toggle {
    self.root.hidden ? [self open] : [self close];
}

// ---- triggers ---------------------------------------------------------------------------------------------

- (void)installTriggers {
    // Three fingers held: cancels its touches when it fires, so the kit's mapper sees a cancel, not a
    // three-finger tap (its F10 page), and the game sees no click.
    UILongPressGestureRecognizer *hold = [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                                                       action:@selector(held:)];
    hold.numberOfTouchesRequired = 3;
    hold.minimumPressDuration = 0.6;
    hold.cancelsTouchesInView = YES;
    [self.window addGestureRecognizer:hold];
    // A three-finger tap too (on iOS the touch mapper no longer turns it into F10, input_touch.cpp).
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped3:)];
    tap.numberOfTouchesRequired = 3;
    tap.cancelsTouchesInView = YES;
    [tap requireGestureRecognizerToFail:hold];
    [self.window addGestureRecognizer:tap];
    // Controllers and the keyboard are polled, not observed: SDL owns their change handlers.
    self.poll = [NSTimer timerWithTimeInterval:1.0 / 30 target:self selector:@selector(pollInputs:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:self.poll forMode:NSRunLoopCommonModes];
}

- (void)tapped3:(UITapGestureRecognizer *)g {
    if (g.state == UIGestureRecognizerStateRecognized)
        [self toggle];
}
- (void)held:(UILongPressGestureRecognizer *)g {
    if (g.state == UIGestureRecognizerStateBegan)
        [self toggle];
}

- (void)pollInputs:(NSTimer *)t {
    static unsigned n = 0;
    ++n;
    [self pollCapture];
    // L3+R3 on any controller: an edge, so holding both toggles once. Not while a row waits for a press.
    BOOL combo = NO;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *pad = c.extendedGamepad;
        if (pad && pad.leftThumbstickButton.isPressed && pad.rightThumbstickButton.isPressed)
            combo = YES;
    }
    if (combo && !self.comboHeld && _captureKey.empty())
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
    // Twice a second: the controller kind (the one in use can change without a connection), the status
    // line, and an F10 page's content changed from elsewhere.
    if (n % 15 == 0) {
        [self publishKind];
        [self seedFirstRun];
        if (self.root.hidden)
            return;
        if (mods_page_visible()) { // the F10 page opened (Edit controls, F10): it takes over
            [self close];
            return;
        }
        if (_tab == TAB_HOME) {
            [self refreshStatus];
        } else if (_captureKey.empty()) {
            MenuSnapshot s;
            if (!mods_menu_open_headless(menu_tab(_tab)) || !mods_menu_snapshot(&s) || s.generation != _menuGeneration)
                [self rebuildKeepingScroll];
        }
    }
}

@end

// ---------------------------------------------------------------------------

namespace {
RecompIosOverlay *g_overlay;
// mods_page_set_redirect: F10 (tab -1: toggle) and every request for the old settings page open this panel instead,
// on the tab the page would have shown. Called on whichever thread delivered the key.
void page_redirect(int tab) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!g_overlay)
          return;
      if (tab < 0)
          [g_overlay toggle];
      else
          [g_overlay openOnTab:tab == MENU_TAB_MODS ? TAB_MODS : tab == MENU_TAB_CONTROLS ? TAB_CONTROLS : TAB_GRAPHICS];
    });
}
}

void ios_overlay_apply_launch_settings() {
    NSUserDefaults *d = defaults();
    // The scene markers: the core mod sends them when this is set, and the renderer's scene height then comes
    // from host_upscale_set_rows (the panel). Native until the panel applies the saved choice.
    setenv("RECOMP_REMASTER_UPSCALE", "2", 0);
    setenv("RECOMP_UPSCALE_FRONTEND", "1", 0); // the menus' garage scene too, not only races
    setenv("RECOMP_FE_FIT_WIDE", "1", 0);      // the widescreen menus fit a 4:3 iPad (core mod video_modes.h)
    setenv("RECOMP_HIDE_GAME_CURSOR", "1", 0); // only the iPad's own pointer, not the game's arrow as well
    setenv("RECOMP_TOD_STEPS", "1200", 0);     // the day cycle's sun moves in steps: steady car shadows in races
    host_upscale_set_rows(0);
    // The game's resolution (main.cpp note_screen_size reads it before the game boots).
    // Effects (glow, depth of field, motion blur) at the 3D resolution is the default: on an iPhone it saves GPU
    // time at no visible cost (owner, 12 Oct).
    [d registerDefaults:@{kOutput : @(kDefaultOutput), kEffects : @YES}];
    g_launch_output = (int)[d integerForKey:kOutput];
    if ([d boolForKey:kEffects])
        setenv("RECOMP_UPSCALE_EFFECTS_INTERNAL", "1", 0);
    setenv("RECOMP_IOS_OUTPUT_ROWS", std::to_string(g_launch_output).c_str(), 1);
    // Max performance: the host's event wait sleeps through the sub-millisecond tail of a frame wait instead of
    // polling (kit/host/sdl/main.cpp service), so the CPU idles while it waits for the next frame.
    if ([d boolForKey:kMaxPerf])
        setenv("RECOMP_IOS_LOW_POWER", "1", 1);
    // The 120 Hz simulation test crashed races (SIGSEGV at the start, 12 Oct): off, whatever was saved.
    [d removeObjectForKey:kSim120];
    fprintf(stderr, "[ios] launch options: output %s%d rows, effects at internal resolution %s\n",
            g_launch_output > 0 ? "" : "the screen's, ", g_launch_output, [d boolForKey:kEffects] ? "ON (test)" : "off");
}

void ios_overlay_install(SDL_Window *window) {
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                     SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (!uiwindow || g_overlay)
        return;
    g_overlay = [[RecompIosOverlay alloc] initWithWindow:uiwindow];
    mods_page_set_redirect(page_redirect); // the iOS panel replaces the F10 page
}

bool ios_overlay_visible() {
    return g_visible.load();
}
