# CLAUDE.md

Need for Speed: Most Wanted (2005), statically recompiled for Apple platforms with the recomp kit in `kit/`.
The owner works on the iOS port; read `docs/IOS_PORT.md` first: building (incl. the Ninja route when running inside
Xcode), the iOS settings panel, controller icons, MetalFX, findings and open items.

- The owner's game files live in `original/retail/` and must never be committed or redistributed (`.gitignore`
  has `*.tpk`, `original/retail/`). Generated assets go to `build/generated/`.
- Commit as `e5n97 <58152408+EhsanWaheed@users.noreply.github.com>`.
- The owner tests on an iPhone 17 Pro Max and an iPad through LiveContainer; the bar is 60 fps minimum on the
  iPhone, and settings that cost performance stay the player's choice (manual, not automatic).
- The Xcode project at `~/Desktop/NFSMW_IOS` is an abandoned early experiment, not this game.
