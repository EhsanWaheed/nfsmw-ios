// launcher_ios.mm - the launcher's native iOS view (v4.1): a Liquid Glass screen over the SDL window that shows the
// launcher model's content (launcher_ui.h View) instead of the pixel canvas. The cover art (LauncherArt.png in the
// bundle, from build/ios-icon.png) blurred behind and sharp on a card, the title and the status, and one glass button
// per launcher button; Play is the prominent one. Touches call Launcher::activate; keys and controllers move the
// launcher's focus through SDL (launcher_sdl.cpp), and the focused button lifts and gets a ring while a controller or
// keyboard is in use. Quit ends the app (iOS has no other way out of the launcher).
#include "../launcher/launcher_sdl.h"

#import <GameController/GameController.h>
#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

NSString *ns(const std::string &s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

UIVisualEffect *glass_or_blur() {
    if (@available(iOS 26.0, *))
        return [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
    return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterialDark];
}

bool controller_in_use() {
    return GCController.controllers.count > 0 || GCKeyboard.coalescedKeyboard != nil;
}

// The connected controller's family, by the same rules as the panel (ios_overlay.mm controller_kind): 1 PlayStation
// (DualSense, DualShock, PlayStation-edition controllers such as the Backbone One PlayStation Edition), 2 any other
// controller (Xbox letters), 0 none (a keyboard alone).
int pad_family() {
    GCController *c = GCController.current ?: GCController.controllers.firstObject;
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

} // namespace

@interface RecompLauncherView : UIView
@property(nonatomic) launcher::Launcher *model;
@property(nonatomic) UIImageView *backdrop;
@property(nonatomic) UIImageView *art;
@property(nonatomic) UILabel *titleLabel;
@property(nonatomic) UILabel *versionLabel;
@property(nonatomic) UIView *statusPill;
@property(nonatomic) UIImageView *statusIcon;
@property(nonatomic) UILabel *statusLabel;
@property(nonatomic) UILabel *detailLabel;
@property(nonatomic) UIStackView *buttonStack;
@property(nonatomic) UIView *progressCard;
@property(nonatomic) UIProgressView *progress;
@property(nonatomic) UILabel *progressLabel;
@property(nonatomic) UILabel *hintLabel;
@property(nonatomic) NSMutableArray<UIButton *> *buttons;
@property(nonatomic) std::vector<std::pair<int, std::string>> shown;   // the buttons on screen (id, label)
@property(nonatomic) int shownScreen;
@property(nonatomic) int focus;
@property(nonatomic) BOOL compact;   // a phone in landscape (under 520 points tall): smaller type and buttons
@property(nonatomic) int lastFocus;  // the launcher's focus as last shown (a controller connecting redraws it)
@end

@implementation RecompLauncherView {
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame]))
        return nil;
    self.backgroundColor = UIColor.blackColor;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.buttons = [NSMutableArray array];
    self.shownScreen = -1;
    self.focus = -1;
    NSString *artPath = [NSBundle.mainBundle pathForResource:@"LauncherArt" ofType:@"png"];
    UIImage *artImage = artPath ? [UIImage imageWithContentsOfFile:artPath] : nil;

    // Behind everything: the art, filling the screen, softened and darkened towards the text.
    self.backdrop = [[UIImageView alloc] initWithImage:artImage];
    self.backdrop.contentMode = UIViewContentModeScaleAspectFill;
    self.backdrop.clipsToBounds = YES;
    self.backdrop.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:self.backdrop];
    UIVisualEffectView *soften =
        [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterialDark]];
    soften.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:soften];
    CAGradientLayer *shade = [CAGradientLayer layer];
    shade.colors = @[ (id)[UIColor colorWithWhite:0 alpha:0.15].CGColor, (id)[UIColor colorWithWhite:0 alpha:0.65].CGColor ];
    shade.startPoint = CGPointMake(0, 0.5);
    shade.endPoint = CGPointMake(1, 0.5);
    UIView *shadeView = [UIView new];
    shadeView.translatesAutoresizingMaskIntoConstraints = NO;
    shadeView.userInteractionEnabled = NO;
    [shadeView.layer addSublayer:shade];
    [self addSubview:shadeView];

    // The hero: the cover on a card, with a soft shadow.
    UIView *artShadow = [UIView new];
    artShadow.translatesAutoresizingMaskIntoConstraints = NO;
    artShadow.layer.shadowColor = UIColor.blackColor.CGColor;
    artShadow.layer.shadowOpacity = 0.55;
    artShadow.layer.shadowRadius = 28;
    artShadow.layer.shadowOffset = CGSizeMake(0, 14);
    [self addSubview:artShadow];
    self.art = [[UIImageView alloc] initWithImage:artImage];
    self.art.translatesAutoresizingMaskIntoConstraints = NO;
    self.art.contentMode = UIViewContentModeScaleAspectFill;
    self.art.clipsToBounds = YES;
    self.art.layer.cornerRadius = 26;
    self.art.layer.cornerCurve = kCACornerCurveContinuous;
    self.art.layer.borderWidth = 0.5;
    self.art.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.18].CGColor;
    [artShadow addSubview:self.art];

    // The text column.
    self.titleLabel = [UILabel new];
    self.titleLabel.font = [UIFont systemFontOfSize:34 weight:UIFontWeightBold];
    self.titleLabel.textColor = UIColor.whiteColor;
    self.titleLabel.numberOfLines = 2;
    self.titleLabel.adjustsFontSizeToFitWidth = YES;
    self.titleLabel.minimumScaleFactor = 0.7;
    self.versionLabel = [UILabel new];
    self.versionLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    self.versionLabel.textColor = [UIColor colorWithWhite:1 alpha:0.6];
    NSString *v = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"";
    self.versionLabel.text = [NSString stringWithFormat:@"Black Edition  ·  iOS %@", v];

    self.statusPill = [[UIVisualEffectView alloc] initWithEffect:glass_or_blur()];
    self.statusPill.layer.cornerRadius = 15;
    self.statusPill.layer.cornerCurve = kCACornerCurveContinuous;
    self.statusPill.clipsToBounds = YES;
    self.statusIcon = [UIImageView new];
    self.statusIcon.preferredSymbolConfiguration =
        [UIImageSymbolConfiguration configurationWithPointSize:14 weight:UIImageSymbolWeightSemibold];
    self.statusLabel = [UILabel new];
    self.statusLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    self.statusLabel.textColor = UIColor.whiteColor;
    UIStackView *pill = [[UIStackView alloc] initWithArrangedSubviews:@[ self.statusIcon, self.statusLabel ]];
    pill.spacing = 7;
    pill.alignment = UIStackViewAlignmentCenter;
    pill.translatesAutoresizingMaskIntoConstraints = NO;
    [((UIVisualEffectView *)self.statusPill).contentView addSubview:pill];
    [NSLayoutConstraint activateConstraints:@[
        [pill.leadingAnchor constraintEqualToAnchor:self.statusPill.leadingAnchor constant:12],
        [pill.trailingAnchor constraintEqualToAnchor:self.statusPill.trailingAnchor constant:-14],
        [pill.topAnchor constraintEqualToAnchor:self.statusPill.topAnchor constant:6],
        [pill.bottomAnchor constraintEqualToAnchor:self.statusPill.bottomAnchor constant:-6],
    ]];
    UIStackView *pillRow = [[UIStackView alloc] initWithArrangedSubviews:@[ self.statusPill, [UIView new] ]];

    self.detailLabel = [UILabel new];
    self.detailLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    self.detailLabel.textColor = [UIColor colorWithWhite:1 alpha:0.55];
    self.detailLabel.numberOfLines = 4;

    // The import's progress, in place of the buttons while it runs.
    self.progressCard = [UIView new];
    self.progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.progress.progressTintColor = UIColor.whiteColor;
    self.progress.trackTintColor = [UIColor colorWithWhite:1 alpha:0.18];
    self.progressLabel = [UILabel new];
    self.progressLabel.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular];
    self.progressLabel.textColor = [UIColor colorWithWhite:1 alpha:0.7];
    self.progressLabel.numberOfLines = 3;
    UIStackView *prog = [[UIStackView alloc] initWithArrangedSubviews:@[ self.progress, self.progressLabel ]];
    prog.axis = UILayoutConstraintAxisVertical;
    prog.spacing = 10;
    prog.translatesAutoresizingMaskIntoConstraints = NO;
    [self.progressCard addSubview:prog];
    [NSLayoutConstraint activateConstraints:@[
        [prog.leadingAnchor constraintEqualToAnchor:self.progressCard.leadingAnchor],
        [prog.trailingAnchor constraintEqualToAnchor:self.progressCard.trailingAnchor],
        [prog.topAnchor constraintEqualToAnchor:self.progressCard.topAnchor],
        [prog.bottomAnchor constraintEqualToAnchor:self.progressCard.bottomAnchor],
    ]];
    self.progressCard.hidden = YES;

    self.compact = UIScreen.mainScreen.bounds.size.height < 520 || UIScreen.mainScreen.bounds.size.width < 520;
    if (self.compact) {
        self.titleLabel.font = [UIFont systemFontOfSize:26 weight:UIFontWeightBold];
        self.titleLabel.numberOfLines = 1;
        self.detailLabel.numberOfLines = 3;
    }
    self.buttonStack = [UIStackView new];
    self.buttonStack.axis = UILayoutConstraintAxisVertical;
    self.buttonStack.spacing = self.compact ? 8 : 10;

    self.hintLabel = [UILabel new];
    self.hintLabel.textColor = [UIColor colorWithWhite:1 alpha:0.55];
    self.hintLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];

    UIStackView *column = [[UIStackView alloc] initWithArrangedSubviews:@[
        self.titleLabel, self.versionLabel, pillRow, self.detailLabel, self.progressCard, self.buttonStack, self.hintLabel
    ]];
    column.axis = UILayoutConstraintAxisVertical;
    column.spacing = 10;
    [column setCustomSpacing:2 afterView:self.titleLabel];
    [column setCustomSpacing:self.compact ? 12 : 18 afterView:self.versionLabel];
    [column setCustomSpacing:self.compact ? 12 : 20 afterView:self.detailLabel];
    [column setCustomSpacing:self.compact ? 10 : 16 afterView:self.buttonStack];
    [column setCustomSpacing:self.compact ? 12 : 16 afterView:pillRow];
    column.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:column];

    UILayoutGuide *safe = self.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.backdrop.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.backdrop.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.backdrop.topAnchor constraintEqualToAnchor:self.topAnchor],
        [self.backdrop.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [soften.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [soften.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [soften.topAnchor constraintEqualToAnchor:self.topAnchor],
        [soften.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [shadeView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [shadeView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [shadeView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [shadeView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],

        // the cover: a square on the left, most of the height
        [artShadow.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:36],
        [artShadow.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [artShadow.heightAnchor constraintEqualToAnchor:safe.heightAnchor multiplier:0.74],
        [artShadow.widthAnchor constraintEqualToAnchor:artShadow.heightAnchor],
        [self.art.leadingAnchor constraintEqualToAnchor:artShadow.leadingAnchor],
        [self.art.trailingAnchor constraintEqualToAnchor:artShadow.trailingAnchor],
        [self.art.topAnchor constraintEqualToAnchor:artShadow.topAnchor],
        [self.art.bottomAnchor constraintEqualToAnchor:artShadow.bottomAnchor],

        // the column: beside it, centred, at most 380 wide
        [column.leadingAnchor constraintEqualToAnchor:artShadow.trailingAnchor constant:44],
        [column.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-36],
        [column.widthAnchor constraintLessThanOrEqualToConstant:380],
        [column.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [column.topAnchor constraintGreaterThanOrEqualToAnchor:safe.topAnchor constant:12],
        [column.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor constant:-12],
    ]];
    NSLayoutConstraint *wide = [column.widthAnchor constraintEqualToConstant:380];
    wide.priority = UILayoutPriorityDefaultHigh;
    wide.active = YES;
    // the gradient follows the view's size
    self.layer.name = @"launcher";
    shadeView.layer.name = @"shade";
    for (NSNotificationName n in @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification,
                                     GCControllerDidBecomeCurrentNotification, GCKeyboardDidConnectNotification,
                                     GCKeyboardDidDisconnectNotification ])
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(controllersChanged:) name:n object:nil];
    self.alpha = 0;
    [UIView animateWithDuration:0.45 animations:^{
      self.alpha = 1;
    }];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    for (UIView *v in self.subviews)
        if ([v.layer.name isEqualToString:@"shade"])
            for (CALayer *l in v.layer.sublayers)
                l.frame = v.bounds;
}

- (UIButton *)makeButton:(const launcher::Button &)b primary:(BOOL)primary {
    UIButtonConfiguration *c = nil;
    if (@available(iOS 26.0, *))
        c = primary ? [UIButtonConfiguration prominentGlassButtonConfiguration] : [UIButtonConfiguration glassButtonConfiguration];
    else
        c = primary ? [UIButtonConfiguration filledButtonConfiguration] : [UIButtonConfiguration grayButtonConfiguration];
    c.title = ns(b.label);
    c.buttonSize = self.compact ? UIButtonConfigurationSizeMedium : UIButtonConfigurationSizeLarge;
    c.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    c.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *in) {
      NSMutableDictionary *out = [in mutableCopy];
      const BOOL small = self.compact;
      out[NSFontAttributeName] = [UIFont systemFontOfSize:primary ? (small ? 17 : 19) : (small ? 15 : 16)
                                                   weight:primary ? UIFontWeightSemibold : UIFontWeightMedium];
      return out;
    };
    NSString *symbol = nil;
    switch (b.id) {
    case launcher::kPlay: symbol = @"play.fill"; break;
    case launcher::kImportFolder: symbol = @"folder"; break;
    case launcher::kImportZip: symbol = @"doc.zipper"; break;
    case launcher::kManage: symbol = @"slider.horizontal.3"; break;
    case launcher::kExportSaves: symbol = @"square.and.arrow.up"; break;
    case launcher::kImportSaves: symbol = @"square.and.arrow.down"; break;
    case launcher::kDeleteData: symbol = @"trash"; break;
    case launcher::kBack: symbol = @"chevron.backward"; break;
    case launcher::kCancel: symbol = @"xmark"; break;
    case launcher::kOk: symbol = @"checkmark"; break;
    case launcher::kQuit: symbol = @"power"; break;
    case launcher::kStore: symbol = @"bag"; break;
    default: break;
    }
    if (symbol) {
        c.image = [UIImage systemImageNamed:symbol];
        c.imagePadding = 9;
        c.preferredSymbolConfigurationForImage =
            [UIImageSymbolConfiguration configurationWithPointSize:primary ? 17 : 15 weight:UIImageSymbolWeightSemibold];
    }
    if (primary)
        c.baseBackgroundColor = [UIColor colorWithRed:0.20 green:0.47 blue:1.0 alpha:1.0];
    if (b.id == launcher::kDeleteData || b.id == launcher::kQuit)
        c.baseForegroundColor = b.id == launcher::kDeleteData ? UIColor.systemRedColor : [UIColor colorWithWhite:1 alpha:0.8];
    UIButton *button = [UIButton buttonWithConfiguration:c primaryAction:nil];
    button.enabled = b.enabled;
    button.tag = b.id;
    [button addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [button.heightAnchor constraintEqualToConstant:self.compact ? (primary ? 46 : 36) : (primary ? 56 : 46)].active = YES;
    button.layer.cornerCurve = kCACornerCurveContinuous;
    return button;
}

- (void)tapped:(UIButton *)b {
    if (!self.model)
        return;
    UIImpactFeedbackGenerator *h = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    [h impactOccurred];
    self.model->activate((int)b.tag);
}

- (void)showView:(const launcher::View &)v {
    self.titleLabel.text = ns(v.title);
    // status: an SF Symbol and colour by tone
    NSString *symbol = v.tone == 1 ? @"checkmark.circle.fill" : v.tone == 2 ? @"exclamationmark.triangle.fill"
                                                                            : @"xmark.octagon.fill";
    UIColor *tint = v.tone == 1 ? UIColor.systemGreenColor : v.tone == 2 ? UIColor.systemOrangeColor : UIColor.systemRedColor;
    std::string head = v.headline, detail = v.detail;
    if (v.screen == launcher::Screen::Importing) {
        head = "Importing your game";
        symbol = @"arrow.down.circle.fill";
        tint = UIColor.systemBlueColor;
        detail = "Keep the app open. If it stops, importing again continues where it left off.";
    } else if (v.screen == launcher::Screen::Message) {
        head = "Notice";
        symbol = @"info.circle.fill";
        tint = UIColor.systemBlueColor;
        detail = v.message;
    } else if (v.screen == launcher::Screen::Manage) {
        head = "Manage";
        symbol = @"slider.horizontal.3";
        tint = UIColor.systemGrayColor;
        detail = v.detail2;
    }
    self.statusIcon.image = [UIImage systemImageNamed:symbol];
    self.statusIcon.tintColor = tint;
    self.statusLabel.text = ns(head);
    self.detailLabel.text = ns(detail);
    // compact: the ready screen's folder path is left out (room for the buttons); problems keep their detail
    self.detailLabel.hidden = detail.empty() || (self.compact && v.tone == 1 && v.screen == launcher::Screen::Main &&
                                                 v.countdown <= 0);

    // progress
    const bool importing = v.screen == launcher::Screen::Importing;
    self.progressCard.hidden = !importing;
    if (importing) {
        const launcher::Progress &p = v.progress;
        const float f = p.bytes_total ? (float)((double)p.bytes_done / (double)p.bytes_total) : 0.0f;
        [self.progress setProgress:f animated:YES];
        self.progressLabel.text =
            [NSString stringWithFormat:@"%llu of %llu files  ·  %@ of %@\n%@", (unsigned long long)p.files_done,
                                       (unsigned long long)p.files_total, ns(launcher::human_bytes(p.bytes_done)),
                                       ns(launcher::human_bytes(p.bytes_total)), ns(p.current).lastPathComponent];
    }

    // buttons: rebuilt when the set changes
    std::vector<std::pair<int, std::string>> want;
    for (const launcher::Button &b : v.buttons)
        want.push_back({b.id, b.label + (b.enabled ? "" : "\x01")});
    if (want != self.shown || (int)v.screen != self.shownScreen) {
        self.shown = want;
        self.shownScreen = (int)v.screen;
        for (UIButton *b in self.buttons)
            [b removeFromSuperview];
        [self.buttons removeAllObjects];
        for (size_t i = 0; i < v.buttons.size(); ++i) {
            const launcher::Button &b = v.buttons[i];
            UIButton *button = [self makeButton:b primary:(b.id == launcher::kPlay || (i == 0 && b.id == launcher::kOk))];
            [self.buttons addObject:button];
            [self.buttonStack addArrangedSubview:button];
            if (b.id == launcher::kQuit || b.id == launcher::kBack)
                [self.buttonStack setCustomSpacing:18 afterView:self.buttons.count > 1 ? self.buttons[self.buttons.count - 2] : button];
        }
        self.focus = -1;
    }
    [self showFocus:v.focus];

    self.lastFocus = v.focus;
    [self refreshHints];
    if (v.countdown > 0) {
        self.detailLabel.text = controller_in_use() ? @"Press any button to stay on this screen."
                                                    : @"Tap anywhere to stay on this screen.";
        self.detailLabel.hidden = NO;
    }
}

// The hints at the bottom, in the connected controller's own glyphs: PlayStation cross / circle in their colours,
// otherwise A / B; a keyboard alone gets Return / Esc. None for touch alone.
- (void)refreshHints {
    const int family = pad_family();
    if (!controller_in_use()) {
        self.hintLabel.hidden = YES;
        return;
    }
    NSMutableAttributedString *h = [NSMutableAttributedString new];
    UIColor *dim = [UIColor colorWithWhite:1 alpha:0.7];
    auto glyph = [&](NSString *name, UIColor *colour, NSString *text) {
        UIImage *img = [[UIImage systemImageNamed:name] imageWithTintColor:colour renderingMode:UIImageRenderingModeAlwaysOriginal];
        if (img)
            [h appendAttributedString:[NSAttributedString attributedStringWithAttachment:[NSTextAttachment textAttachmentWithImage:img]]];
        [h appendAttributedString:[[NSAttributedString alloc] initWithString:text]];
    };
    if (family == 1) {   // PlayStation: cross selects, circle goes back
        glyph(@"xmark.circle.fill", [UIColor colorWithRed:0.49 green:0.69 blue:0.97 alpha:1], @" Select     ");
        glyph(@"circle.circle.fill", [UIColor colorWithRed:0.95 green:0.39 blue:0.43 alpha:1], @" Back     ");
        glyph(@"dpad.fill", dim, @" Move");
    } else if (family == 2) {
        glyph(@"a.circle.fill", dim, @" Select     ");
        glyph(@"b.circle.fill", dim, @" Back     ");
        glyph(@"dpad.fill", dim, @" Move");
    } else {
        glyph(@"return", dim, @" Select     ");
        glyph(@"escape", dim, @" Back     ");
        glyph(@"arrow.up.arrow.down", dim, @" Move");
    }
    self.hintLabel.attributedText = h;
    self.hintLabel.hidden = NO;
}

// A controller connected or went away: its glyphs and the focus ring follow at once.
- (void)controllersChanged:(NSNotification *)note {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self refreshHints];
      self.focus = -2;   // force the ring to be re-evaluated
      [self showFocus:self.lastFocus];
    });
}

// The controller's focus: the focused button lifts a little and gets a white ring; none for touch alone.
- (void)showFocus:(int)focus {
    const bool show = controller_in_use();
    if (focus == self.focus && show == (self.focus >= 0) && self.focus != -2)
        return;
    self.focus = show ? focus : -1;
    [UIView animateWithDuration:0.18
                     animations:^{
                       for (NSUInteger i = 0; i < self.buttons.count; ++i) {
                           UIButton *b = self.buttons[i];
                           const bool on = show && (int)i == focus;
                           b.transform = on ? CGAffineTransformMakeScale(1.035, 1.035) : CGAffineTransformIdentity;
                           b.layer.borderWidth = on ? 2.0 : 0.0;
                           b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.9].CGColor;
                           b.layer.cornerRadius = b.bounds.size.height / 2;
                           b.layer.shadowColor = UIColor.whiteColor.CGColor;
                           b.layer.shadowOpacity = on ? 0.35 : 0.0;
                           b.layer.shadowRadius = 12;
                           b.layer.shadowOffset = CGSizeZero;
                       }
                     }];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    if (self.model && self.model->counting_down())   // a touch outside the buttons stops the countdown
        self.model->key(launcher::Key::Back);
}
@end

namespace {

class IosLauncherView final : public launcher::NativeView {
  public:
    explicit IosLauncherView(SDL_Window *window) {
        UIWindow *w = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                  SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
        UIView *host = w.rootViewController.view ?: w;
        view_ = [[RecompLauncherView alloc] initWithFrame:host.bounds];
        [host addSubview:view_];
    }
    void update(launcher::Launcher &l) override {
        view_.model = &l;
        [view_ showView:l.view()];
    }
    void finish(bool quit) override {
        RecompLauncherView *v = view_;
        v.model = nullptr;
        if (quit)
            exit(0);   // iOS: nothing else ends the app from the launcher
        [UIView animateWithDuration:0.3
            animations:^{
              v.alpha = 0;
            }
            completion:^(BOOL) {
              [v removeFromSuperview];
            }];
    }

  private:
    RecompLauncherView *view_ = nil;
};

} // namespace

launcher::NativeView *ios_launcher_view_make(SDL_Window *window) {
    return new IosLauncherView(window);
}
