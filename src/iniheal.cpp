// iniheal.cpp -- the CURATED OPTION TABLE, and the startup heal that writes it.
//
// One table, two consumers, so they can never drift:
//   * iniheal()      -- on startup, add any MISSING key to the install's polshim.ini
//                       with its default, so a new shim option becomes visible and
//                       editable on a deployed install (which never runs build.bat's
//                       merge-ini.ps1). Each key is written only if ABSENT -- checked
//                       with a sentinel -- so a value you set is NEVER touched.
//   * polsettings.cpp -- draws the in-game settings dialog straight off this table,
//                       which is why the rows carry a type + label as well.
//
// This is the set a user might actually flip; internal/dev-only keys (probes, capture,
// tabletrace, ...) are deliberately left out so a normal ini stays clean and the
// dialog stays short. Comments are not written (the dist ini template documents them).
//
// dist/install.sh has a THIRD implementation of the same rule (ini_heal, in awk) for
// the keys the DLL has not seen yet -- it heals from the shipped template at install
// time, before this code ever runs. Same semantics: add absent, never overwrite.
//
// ---------------------------------------------------------------------------
// TWO RULES ADDED 2026-08-17, both learned from the same failure
// ---------------------------------------------------------------------------
//
// A user moved to a second computer and found the Tetra Master mouse broken and
// Fantasy Earth mis-patching -- both of which had been fixed months earlier --
// and could not say which settings had fixed them. They were right not to know:
// the fixes were in dist/polshim.ini and NOWHERE ELSE. An install that never
// received that template (or received an older one) fell back to the COMPILED
// defaults, and the compiled defaults for [dx] enable, d3d_enable, dinput_enable,
// dinput_mouseabs and [polshim] patches are all OFF.
//
// RULE 1 -- IF IT IS PART OF THE TESTED CONFIGURATION, IT IS A ROW HERE.
// Being documented in the shipped template is not delivery: the template is
// written only when an install has none. A key that must be on for a title to
// work belongs in this table with the value we actually test, so a stale install
// heals into the working configuration instead of quietly running without it.
//
// RULE 2 -- LABEL THE OUTCOME, NOT THE MECHANISM.
// "dinput_mouseabs" is what the code calls it. "Tetra Master: the card cursor
// lands where you point" is what the user is looking for. Where a fix is several
// keys, it gets ONE row (OPT_PACK) and the members go hidden -- nobody should
// have to know that a working mouse in one game is four ini keys.

#include "polshim.h"
#include "hotkeydef.h"   // POLSHIM_DEFAULT_HOTKEY -- one definition, two readers

// A GROUP'S `choices` IS ITS SIDEBAR NAME. Headings read in place ("Logging in, and
// staying up to date"); the sidebar has 150px, so it needs a short one. OPT_GROUP has
// no other use for the field, so the pair lives on one row rather than in a second
// list somebody has to remember to extend -- which is the failure this whole file
// exists to stop repeating.
//
// Types drive the dialog control; iniheal ignores them.
//   OPT_BOOL "0"/"1" -> checkbox     OPT_ENUM choices "a|b|c" -> combo
//   OPT_TEXT free string -> edit box  OPT_GROUP -> a heading, no key
//   OPT_PACK -> one checkbox owning several keys, listed in `choices`
static const ShimOption g_opts[] = {

  // ==========================================================================
  // IMPORTANT: A DEVELOPER GROUP SINCE 2026-09-08. Everything under it is a repair that is
  // simply ON in the configuration we ship and test -- there was never a reason for a
  // player to see, let alone weigh, "Tetra Master: make the mouse work". The rows that
  // were NOT repairs (the close prompt, mouse speed, Fantasy Earth's opening movie)
  // moved out to the sections they belong to; what is left here is the fixes, which is
  // what the heading always said. `shim_reset_fixes` still walks this group by label.
  { NULL, NULL, NULL, OPT_GROUP, SHIM_GROUP_FIXES, L"Game fixes", true, NULL },
  // ==========================================================================
  //
  // Everything in this group is ON in the configuration we test. They are rows
  // so that an install which never saw the shipped template still gets them --
  // see RULE 1 above; this group is the entire reason that rule exists.

  // THE TETRA MASTER MOUSE. Four keys, one outcome, so one checkbox.
  //
  // The game drives an internal cursor from DirectInput deltas and warps the OS
  // pointer to (30,30) every frame. Untouched, the two diverge and the card
  // cursor flies away from the pointer -- the game is not playable. mouseabs=2
  // WRITES the game's own cursor variable (two int16s at a fixed RVA, which the
  // game clamps itself) instead of steering it with deltas, so it lands exactly
  // where you point at any window size; freecursor swallows the warp so the
  // pointer is never captured. Neither does anything without dinput_enable,
  // which is why this is a pack and not a checkbox next to three traps.
  //
  // d3d_cursor is NOT a member: it is a prerequisite (the freecursor hooks are
  // installed inside its `if`), and unticking it would also drop the focus
  // hooks, which have nothing to do with the mouse. It is healed at 2 below.
  { L"dx", NULL, NULL, OPT_PACK, L"Tetra Master: make the mouse work",
    L"dx.dinput_enable=1:0|dx.dinput_nonexclusive=1:0|dx.dinput_mouseabs=2:0|dx.d3d_freecursor=1:0",
    true,
    L"Without this the card cursor drifts away from the pointer and TM is unplayable." },
  { L"dx",        L"dinput_enable",  L"1",    OPT_HIDDEN, L"Wrap the game's mouse device",       NULL },
  { L"dx",        L"dinput_nonexclusive", L"1", OPT_HIDDEN, L"Refuse exclusive mouse capture",   NULL },
  { L"dx",        L"dinput_mouseabs", L"2",   OPT_HIDDEN, L"Absolute cursor mode (2 = write it outright)", NULL, false, NULL, true },
  { L"dx",        L"d3d_freecursor", L"1",    OPT_HIDDEN, L"Swallow the game's pointer warp",    NULL, false, NULL, true },
  // Prerequisite for the pack, and for the focus hooks. Healed, not drawn.
  { L"dx",        L"d3d_cursor",     L"2",    OPT_HIDDEN, L"Cursor hook level (0 off, 1 log, 2 translate)", NULL },

  // FANTASY EARTH'S CLICKS. RULE 1 again, in its quietest form: this compiles to
  // the right value and every install has been getting it, but it was not a row,
  // so nobody could see it was on -- and "we fixed this once and now it is gone"
  // is unanswerable when the fix is invisible. Reported 2026-08-17.
  //
  // DirectInput UNACQUIRES a FOREGROUND device the moment its window stops being
  // foreground, and a buffered reader then just sees an empty queue -- no error,
  // nothing. Measured on FE: it acquires NONEXCLUSIVE|FOREGROUND, the first
  // GetDeviceData returns 31 events and every call after it returns 0 for ever.
  // That is why the pointer and the keyboard keep working while CLICKS do not --
  // FE takes cursor position from the WH_MOUSE chain and keys from its own
  // WH_KEYBOARD hook, but the BUTTONS come only from DirectInput. BACKGROUND
  // keeps the device acquired regardless of focus; it cannot steal input from
  // other applications, which is EXCLUSIVE's doing and dinput_nonexclusive's job.
  // IMPORTANT: HIDDEN 2026-08-24. It was a visible checkbox, and "off" has no correct
  // machine: off means Fantasy Earth's mouse CLICKS do nothing at all
  // (pointer and keyboard keep working, which makes it read as an FE bug, not a setting).
  // A control whose only function is to break a title is not a setting, it is a
  // trap with a label. Same reasoning that RETIRED d3d_fitwindow and hid
  // d3d_fitvideo. The key is still healed and still read, so a diagnosing hand
  // can turn it from the ini -- what goes away is the invitation.
  { L"dx",        L"dinput_background", L"1", OPT_HIDDEN,
    L"Fantasy Earth: make mouse clicks work", NULL },

  // THE MOUSE WHILE YOU ARE SOMEWHERE ELSE. Reported 2026-08-26: "when I
  // don't have FMO selected, it's still rotating my character. Tetra
  // sometimes does this."
  //
  // Deliberately ONE row, sitting immediately under dinput_background,
  // because that hidden key is half the cause: it rewrites the title's
  // DISCL_FOREGROUND to DISCL_BACKGROUND, which is literally "keep feeding
  // this device when the window is not focused". It cannot simply be turned
  // off -- off means FE's clicks stop working at all -- so the gate withholds
  // the DATA instead and leaves the acquisition alone. inputgate.cpp has the
  // full argument, and applies the same rule to the two SE-owned paths
  // (app.dll's GetCursorPos poll and SE's system-wide WH_MOUSE hook), which
  // is why this is not a dinput_* key.
  // THE CLOSE BOX. A windowed title has an X and SE never wrote any behaviour
  // for one -- these games were left through their own in-game menus. Before
  // 2026-08-26 it silently ended the title and dropped you back to the Viewer,
  // which is right about half the time; the rest of the time "close" means
  // "I am finished". So it asks. Values: 1 ask, 0 the old silent behaviour,
  // 2 always exit to the desktop. Only 0 and 1 are offered here -- 2 is a
  // standing instruction to end the session on a stray click, which is a
  // reasonable thing to choose deliberately in an ini and a bad thing to put
  // one tick away from the default.

  // A PACK, since 2026-09-09, and for the reason packs exist here: the player
  // complaint is one sentence -- "the game still takes my input when I am not
  // on its window" -- and it took SIX code paths to be true of. A tick that
  // fixed the mouse and left the keyboard reads, to the person who reported it,
  // exactly like a setting that does nothing. That is
  // [[shim-windowed-by-default]]'s four-hidden-gates failure, and it has now
  // happened to this same complaint twice.
  //
  //   mouse_focus_gate  seams 1-3: app.dll's GetCursorPos poll, SE's
  //                     system-wide WH_MOUSE chain, the DirectInput mouse.
  //   key_focus_gate    seams 4-6: the DirectInput keyboard, the focus-blind
  //                     GetAsyncKeyState/GetKeyState/GetKeyboardState family,
  //                     and a system-wide keyboard hook chain.
  //
  // They stay two KEYS rather than one so a diagnosis can turn off exactly one
  // of them; they are one TICK because no player has a reason to want half of
  // this. Both re-read live.
  { L"dx", NULL, NULL, OPT_PACK,
    L"Ignore the mouse and keyboard while another program is in front",
    L"dx.mouse_focus_gate=1:0|dx.key_focus_gate=1:0",
    true,
    L"Stops a game moving your character, turning the camera or acting on your typing while you are working in another window. The game gets your input back the instant you click into it." },
  { L"dx",        L"mouse_focus_gate", L"1", OPT_HIDDEN,
    L"Withhold the mouse while another program is in front", NULL },
  { L"dx",        L"key_focus_gate",   L"1", OPT_HIDDEN,
    L"Withhold the keyboard while another program is in front", NULL },

  // GETTING OUT OF A GAME. The complaint, verbatim: "most POL games don't respond
  // to a Windows key press or alt+tab, which makes it even more frustrating trying
  // to get out of them."
  //
  // TWO independent mechanisms produce that one symptom, and either one alone
  // leaves it in place -- which is precisely why this is a PACK and not two
  // checkboxes. [[shim-windowed-by-default]] is the standing lesson: one
  // user-facing knob held down by several hidden ones reads as "the setting does
  // nothing", four separate times.
  //
  //   key_escape      the titles install a system-wide WH_KEYBOARD_LL hook
  //                   (measured: FFXiMain.dll and FrontMissionOnline.dll, in our
  //                   own logs) and a low-level hook proc that returns non-zero
  //                   swallows the keystroke before the shell ever sees it.
  //                   hookspy.cpp now declines to offer that proc the escape keys.
  //   dinput_winkey   the titles also ask DirectInput for DISCL_NOWINKEY, and the
  //                   shim was rewriting the rest of that cooperative level while
  //                   forwarding the Windows-key kill untouched. Stripped now.
  //
  // Only the escape keys are affected -- Win, Alt+Tab, Alt+Esc, Ctrl+Esc. Nothing
  // a POL title binds is one of them, so ON has no in-game cost, which is why it
  // ships on. OFF is a real choice for anyone who wants the 2003 behaviour back.
  { L"dx", NULL, NULL, OPT_PACK, L"Let the Windows key and Alt+Tab out of a game",
    L"dx.key_escape=1:0|dx.dinput_winkey=1:0",
    true,
    L"The games ask Windows to disable these keys. Ticked, the shim declines on your behalf, so you can always get back to the desktop." },
  { L"dx",        L"key_escape",     L"1",    OPT_HIDDEN,
    L"Let Win / Alt+Tab past a title's keyboard hook", NULL },
  { L"dx",        L"dinput_winkey",  L"1",    OPT_HIDDEN,
    L"Strip DISCL_NOWINKEY from a title's DirectInput cooperative level", NULL },

  // MODERN MICE. Compiled 100, and all three shipped templates say 25 -- so an
  // install that never saw a template got the untested value. DirectInput hands
  // the application RAW device counts: a 2003 mouse was ~400 CPI, a modern one is
  // commonly 1600+, so the same movement travels ~4x as far and a pointer clamped
  // to a 640x480 field spends its life pegged at the edge. Compiled default is 25
  // now, matching what we ship and test.
  // THE one number that decides whether "land where you point" actually lands.
  // Visible, not hidden: it is title-specific, it cannot be derived, and the
  // supported way to set it is to walk it with Ctrl+Alt+[ / ] while playing.
  { L"dx",        L"dinput_abs_gain", L"100", OPT_HIDDEN, L"Tetra Master: cursor speed (%)", NULL, false,
    L"Only if the cursor over- or under-shoots. Ctrl+Alt+[ and ] tune it live, in game." },
  // WHO OWNS THE CURSOR. Visible, because it is the difference between "the
  // controller works" and "the controller does nothing", and somebody who plays
  // with a pad needs to be able to find it. Mode 2 writing the pointer's position
  // every poll made the shim the only writer of TM's cursor variable, which erased
  // pad navigation (the glide at TM.dll+0x1892FE) and any screen that holds a
  // selection. On means: the mouse owns the cursor while it is MOVING, the game
  // owns it the rest of the time -- which is TM's own last-writer-wins rule.
  // IMPORTANT: HIDDEN 2026-08-24. It was a visible checkbox, and "off" has no correct
  // machine: off makes the mouse the ONLY writer of TM's cursor
  // variable, which erases pad navigation and every screen that holds a selection.
  // A control whose only function is to break a title is not a setting, it is a
  // trap with a label. Same reasoning that RETIRED d3d_fitwindow and hid
  // d3d_fitvideo. The key is still healed and still read, so a diagnosing hand
  // can turn it from the ini -- what goes away is the invitation.
  { L"dx",        L"dinput_abs_yield", L"1", OPT_HIDDEN,
    L"Tetra Master: let the controller and menus move the cursor too", NULL },
  { L"dx",        L"dinput_abs_follow", L"1", OPT_HIDDEN,
    L"Tetra Master: keep the pointer on the cursor while the game moves it", NULL, false,
    L"So the next mouse movement carries on from where the pad left the cursor "
    L"instead of jumping." },
  { L"dx",        L"dinput_abs_wake", L"3", OPT_HIDDEN,
    L"Tetra Master: mouse movement that takes the cursor back (counts)", NULL, false,
    L"Raise it if pad navigation gets interrupted with your hand off the mouse." },
  // The other two of the abs family. Their four siblings above are rows and these
  // were not, which is the inconsistency that makes a stale install unpredictable:
  // half the group heals and half falls back to whatever the code says.
  { L"dx",        L"dinput_abs_home", L"6",  OPT_HIDDEN, L"Polls spent homing the cursor on acquire (mode 1 only)", NULL },
  // WARNING: COMPILED DEFAULT WAS 1 AND EVERY SHIPPED INI SAYS 0. Confinement traps the
  // pointer inside the game's field, which over remote desktop -- or on any
  // windowed title -- means it cannot reach the title bar to move the window.
  // Now 0 here and in the code, matching what we ship and test.
  { L"dx",        L"dinput_abs_clip", L"0",  OPT_HIDDEN, L"Confine the pointer to the game's field", NULL, false, NULL, true },
  // Hidden: it trades a guarantee for a capability, and the only way to know
  // whether it is wanted is to find a screen that needs mouse MOTION.
  { L"dx",        L"dinput_abs_keepdelta", L"0", OPT_HIDDEN,
    L"Keep the mouse deltas live under the absolute cursor (pre-subtracts the game's own add)", NULL },
  // The cursor variable the closed loop reads. Read from TM's code, not probed.
  { L"dx",        L"dinput_cursor_xy", L"TM.dll+0x2FF59C", OPT_HIDDEN, L"Tetra Master's cursor variable", NULL },
  // Found by the probe and written back here, so it is HIDDEN rather than an
  // invitation to type an address: clearing it is the supported gesture (it
  // makes the probe run again), and a wrong one is detected and dropped at run
  // time anyway.
  { L"dx",        L"dinput_cursor_addr", L"", OPT_HIDDEN, L"Game cursor variable (MODULE+0xRVA; the probe writes this)", NULL },
  // NO dinput_callsite (or any other trace_at-defaulted key) row here: healing one in
  // as "0" writes an explicit override that permanently defeats [polshim] trace -- the
  // rev-5 migration below exists to delete exactly such rows. See the rev-5 comment.

  // FANTASY EARTH. Three keys, and `patches` compiled OFF while every shipped
  // ini set it to 1 -- so this is the group's other RULE 1 casualty.
  //
  // app.dll walks the content table and marks every id "rejected", then runs a
  // hardcoded per-id chain that un-rejects the ones it knows. Fantasy Earth is
  // id 11 and has NO case, so it is dropped before the client opens its
  // directory; the patch NOPs the `jne` guarding the fall-through case so it
  // gets the reprieve. contentiid_alias is the COM half (the "Class not
  // registered" failure it shares with Front Mission Online), and comtrace
  // installs the hook both ride on.
  { L"polshim", NULL, NULL, OPT_PACK, L"Fantasy Earth / Front Mission: let them install and launch",
    L"polshim.comtrace=1:0|polshim.contentiid_alias=1:0|polshim.patches=1:0|polfetch.enable=1:0", true,
    L"Without it Fantasy Earth never appears in Check Files, and FMO fails with \"Class not registered\" or hangs at startup." },
  { L"polshim",   L"comtrace",       L"1",    OPT_HIDDEN, L"Install the COM activation hook",    NULL },
  { L"polshim",   L"contentiid_alias", L"1",  OPT_HIDDEN, L"Retry a failed activation via the region's IID", NULL },
  { L"polshim",   L"patches",        L"1",    OPT_HIDDEN, L"Apply the app.dll byte patches",     NULL },
  { L"polshim",   L"patches_optional", L"0",  OPT_HIDDEN, L"Also apply the patches marked optional", NULL },
  // The FMO livelock timeout (src/polfetch.cpp). Compiled ON and in no table
  // until now, which is rule 1's exact failure mode: it is a FIX, not a probe,
  // so a healed install must carry it. Without it FMO busy-waits on a fetch that
  // never completes and Windows paints a blank ghost over the window.
  { L"polfetch",  L"enable",         L"1",    OPT_HIDDEN, L"Stop FMO hanging on its startup fetch", NULL },
  { L"polfetch",  L"timeout_ms",     L"20000", OPT_HIDDEN, L"ms before that fetch is called dead", NULL },

  // IMPORTANT: The old help said "Never overwrites one that is there", which was both the promise
  // and the bug: on a machine where a real installer ran, every value IS there, so
  // create-if-absent did nothing exactly where a repair was needed. Fantasy Earth's
  // ContentsIID and FMO's DLL both reached a second machine broken that way.
  { L"regfix",    L"enable",         L"1",    OPT_BOOL, L"Fix games that say they are not installed", NULL, true,
    L"Re-registers a game with Windows so PlayOnline can find it, and corrects the "
    L"few entries whose right value is known." },

  // FE's config utility writes the UAC VirtualStore while an ELEVATED Fantasy Earth
  // reads the install tree, so the two copies diverge silently -- six settings had,
  // including an opening movie the user had switched off and which kept playing.
  // OFF for one release: this writes a TITLE's config file, and the last migration
  // that turned something on for every install broke FE on Windows (fmv_skip=4).
  // A DIAGNOSTIC, not a settings screen -- so it is a developer row now. It dumps every
  // value the titles store and marks what changed since the last look, which is the only
  // way FFXI's 0000..0043 will ever be decoded: open it, change the setting in FFXI's own
  // config tool, open it again and read the * line. The settings themselves are reached
  // through each game's own tool, from that game's section.
  { NULL, NULL, NULL, OPT_BUTTON, L"Show every value the games store...", L"gamecfg", true,
    L"Read-only. For working out which stored number is which setting." },
  { L"fecfg",     L"enable",         L"0",    OPT_BOOL, L"Fix Fantasy Earth forgetting its settings", NULL, true,
    L"FantasyEarthConfig saves to a redirected copy that an elevated Fantasy Earth "
    L"never reads, so changes appear to do nothing. This moves the newer values into "
    L"the file the game reads and removes the duplicate. Every field moved is logged." },

  // FANTASY EARTH'S OPENING MOVIE, using SE's OWN switch.
  //
  // Placed HERE, in Games beside the other Fantasy Earth row, and NOT beside
  // [dx] fmv_skip where it was first written -- that row sits in a developer
  // group, so the setting was compiled, healed and completely invisible. Which
  // is the failure this project keeps repeating in a new costume: a knob that
  // exists, works, and cannot be found ([[shim-windowed-by-default]]).
  //
  // fmv_skip is the SHIM interfering with a DirectShow graph, is Wine-gated, and
  // broke FE on Windows when it was turned on for everyone. This is FE's own
  // OPENING_MOVIE, in <install>\\Settings\\GLOBAL.INI -- the switch SE shipped
  // ([[match-the-original-not-a-stopgap]]). FE keeps NOTHING in the registry, so
  // this file is the only place the setting exists.
  //
  // KEY: SEED, NOT ENFORCE. Fires at most ONCE per install and records that it did
  // ([fecfg] movie_seeded). Turn the movie back on in FE's own config app and it
  // stays on. Unticking this stops the one-time write; it does NOT put the movie
  // back, which would be us overriding the player in the other direction.
  { L"fecfg",     L"path",           L"",     OPT_HIDDEN, L"Fantasy Earth folder (empty = from the registry)", NULL },

  // THE MASK WINDOW. pol.exe's fullscreen black cover; polcore binds its input
  // mapping to that HWND and the class wndproc is a bare DefWindowProcA, so a
  // window manager's close request destroys it and the Viewer then errors out
  // and exits. Since 2026-09-07 level 1 also holds the mask invisible, click-through,
  // out of Alt+Tab and never on top or in front, from its own wndproc, for every
  // title (maskguard.cpp). Level 2 additionally shrinks it off-screen so a
  // one-window compositor (gamescope) cannot pick it instead of the game.
  { L"dx",        L"mask_guard",     L"1",    OPT_ENUM, L"Keep the Viewer's mask window alive and off your screen",
    L"0=Off|1=On|2=On, and shrink it for the Deck compositor", true,
    L"On = the black full-screen mask can never sit over a game or block Alt+Tab. "
    L"Steam Deck under gamescope: try \"shrink it\" if a game still dies while loading." },
  // HIDDEN: a millisecond timeout nobody should need to touch, and getting it
  // wrong is expensive in both directions -- too short re-opens the Deck bug the
  // guard exists for, 0 re-opens the hang it was causing (pol.exe surviving every
  // exit and then blocking the next launch on the single-instance mutex).
  { L"dx",        L"mask_close_grace", L"2000", OPT_HIDDEN, L"ms before a refused mask close is honoured as a shutdown (0 = never)", NULL },

  // The master switches the two packs above sit on. Healed at the values every
  // shipped ini uses; absent, they compile to 0 and take the whole DirectX and
  // audio layer down with them, silently.
  { L"dx",        L"enable",         L"1",    OPT_HIDDEN, L"Audio / DirectDraw layer",           NULL },
  { L"dx",        L"d3d_enable",     L"1",    OPT_HIDDEN, L"Direct3D layer (windowing, cursor)", NULL },
  { L"dx",        L"sound_globalfocus", L"1", OPT_HIDDEN, L"Keep game audio when the window loses focus", NULL },
  { L"dx",        L"sound_coop",     L"1",    OPT_HIDDEN, L"Downgrade exclusive audio cooperation", NULL },
  { L"dx",        L"d3d_nosteal",    L"1",    OPT_HIDDEN, L"Refuse cross-application focus grabs", NULL, false, NULL, true },
  { L"dx",        L"d3d_nograb",     L"1",    OPT_HIDDEN, L"Refuse the game's mouse-capture re-grab (keeps a windowed frame draggable)", NULL, false, NULL, true },
  // d3d_fitwindow's row is GONE, 2026-08-17: the key is retired and the code
  // ignores it (d3d8hook.cpp says why -- 0 was never right, and reading like a
  // peer of d3d_windowed is what got it switched off). Healing a key nothing
  // reads would write a row into every install that cannot do anything.
  //
  // Deliberately NOT "heal it back to 1": this table only ever ADDS an absent
  // key, and an install already carrying =0 is exactly the broken case. Rewriting
  // a value the user set would break the one promise iniheal makes; the code
  // logs the row as ignored instead, which fixes the screen and explains itself.
  { L"dx",        L"d3d_unmask",     L"2",    OPT_HIDDEN, L"Align the mask to the game and park it behind", NULL, false, NULL, true },

  // SLEEP AND WAKE (src/wakerecover.cpp). Direct3D 8 and 9 lose the device when
  // the machine suspends, and the application is required to notice and rebuild
  // it. A 2003 title written for a desktop that never slept does not, so it
  // presents into a dead device for ever -- the black window after a Steam Deck
  // sleep. The shim supplies the missing half. It stands down the instant a
  // title turns out to handle its own device loss, so a well-behaved title is
  // untouched by it.
  { L"dx",        L"wake_enable",    L"1",    OPT_BOOL,
    L"Bring the picture back after the machine sleeps", NULL, true,
    L"Steam Deck especially: without this a game is a black window from the "
    L"moment you wake it up, and only closing and relaunching it helps." },
  // The action arm, split from detection so a session can watch what happens
  // without anything being done to the device. Dev-only: the visible switch
  // above is the one a user should ever need.
  { L"dx",        L"wake_reset",     L"1",    OPT_BOOL,
    L"...by rebuilding the Direct3D device itself", NULL, true,
    L"Off = detect and log only. On = the shim calls Reset when the title will "
    L"not. The runtime refuses a Reset that would lose the title's resources, "
    L"so the bad case is a logged refusal, not a broken game." },
  { L"dx",        L"wake_nudge",     L"1",    OPT_HIDDEN, L"Kick the game window on resume (restore + reframe)", NULL },
  { L"dx",        L"wake_grace_ms",  L"3000", OPT_HIDDEN, L"ms the title gets to reset its own device before we do", NULL },
  { L"dx",        L"wake_gap_ms",    L"15000",OPT_HIDDEN, L"ms of unaccounted-for clock that counts as a resume", NULL },
  { L"dx",        L"wake_max_resets",L"8",    OPT_HIDDEN, L"Reset attempts per episode before giving up", NULL },
  { L"dx",        L"wake_cooldown_ms",L"30000",OPT_HIDDEN, L"ms of quiet after an episode that did not recover the device", NULL },
  // DEBUG LEVER, not a fix -- it calls a non-thread-safe device from the
  // watchdog thread when the render thread has stopped calling us. It answers
  // "would a Reset have worked?" for a wedged title and can crash it.
  { L"dx",        L"wake_offthread", L"0",    OPT_HIDDEN, L"DEBUG: drive the recovery off the render thread (can crash the title)", NULL },

  // ==========================================================================
  // TAGGED WITH THE TITLE IT IS ABOUT. On an OPT_GROUP the `key` field carries
  // no setting, so it is free to say "this whole category belongs to one game".
  // The sidebar then folds it into that game's section instead of listing it
  // separately -- FFXI used to appear twice, as this category and as its own
  // per-game section, which is the two-Fantasy-Earths problem wearing a
  // different hat: one game, two doors.
  { NULL, NULL, NULL, OPT_GROUP, L"Final Fantasy XI", L"Final Fantasy XI", false, NULL,
    /*per_title*/ false, OPTM_ANY, /*title_of*/ L"FFXiMain.dll" },
  // ==========================================================================
  //
  // Ashita and Windower inject THEMSELVES into pol.exe -- the same process this
  // shim is already inside, because PolHook.dll is a proxy pol.exe statically
  // imports. So there is nothing here that installs or launches an add-on: you
  // point Ashita's or Windower's own launcher at this install's pol.exe and both
  // are loaded. What these two rows decide is what the SHIM does about it.

  // The stand-down. AUTO is the default because the honest answer is "only when
  // one is actually there": on a machine with no add-on, standing our own FFXI
  // handling down would silently remove fixes the user has been relying on, and
  // that is exactly the class of invisible regression RULE 1 exists to stop.
  // WARNING: THIS ROW AND [ashita] enable BELOW ARE NOT THE SAME SWITCH, and their old
  // labels -- "Let Ashita or Windower run alongside" and "Load Ashita automatically"
  // -- read as if they were. A user typed /polexport with the loader unticked and got
  // nothing, then described the row they had missed using words from BOTH (2026-08-25).
  // The distinction, stated in each label rather than left to the hint:
  //   * this one  = WHO DRIVES FFXI when an add-on core is present (we stand aside)
  //   * [ashita] enable = WHETHER A CORE IS THERE AT ALL (we load it)
  // Neither does the other's job, and ticking only this one loads nothing.
  { L"ffxi",      L"addons",         L"auto", OPT_ENUM, L"Stand aside when Ashita or Windower is running",
    L"auto=When one is running|on=Always|off=Never", false,
    L"Lets the add-on take over the picture and mouse. Does not start it." },

  // NO [ffxi] trace row: it defaults from trace_at(2), and healing it in as "0" writes
  // an explicit override that permanently defeats [polshim] trace. See the rev-5
  // migration, which deletes exactly such rows.

  // Import posts a character dump to the server's bridge and needs the POL session
  // it attributes against ([[poltoken]]), which only exists on the private server.
  // Pointed at SE, the session id it would send means nothing to that bridge -- so
  // the button being dead there is the honest state, not a restriction.
  { NULL, NULL, NULL, OPT_BUTTON, L"Import FFXI character...", L"import_char", false,
    L"Loads a character file onto your account here.", false, OPTM_PRIVATE },

  // ==========================================================================
  { NULL, NULL, NULL, OPT_GROUP, L"Display and window", L"Display and window", false, NULL },
  // THE ONE DISPLAY CONTROL (2026-09-08). Three rows used to share this decision --
  // d3d_windowed, d3d_windowed_except ("Titles left fullscreen") and d3d_borderless
  // -- and a player had to combine them in their head, with the nonsensical
  // combinations all allowed. This row RESOLVES INTO those three (d3d8hook.cpp
  // display_apply / caller_except_why); they are hidden now and remain only as the
  // compatibility path for an ini that predates this key (inimigrate rev 10 derives
  // this row from them once). Per-title: the same row appears in each game's own
  // section and writes [dx.<module>] display. Alt+Enter ([settings] display_hotkey)
  // switches a running game between the first two live.
  { L"dx",        L"display",        L"windowed", OPT_ENUM, L"Display",
    L"windowed=In a window|borderless=Borderless fullscreen|"
    L"borderless_crisp=Borderless, whole pixels (black bars)|"
    L"fullscreen=Fullscreen (the game's own -- takes the display over)", false,
    L"Alt+Enter switches this while you play.", true },
  // ==========================================================================

  // THE SINGLE BIGGEST FRAMERATE SETTING ON A STEAM DECK, AND IT IS NOT OURS.
  // Every POL title but Front Mission Online is Direct3D 8, and stock Proton runs d3d8
  // through wined3d (OpenGL) unless PROTON_DXVK_D3D8=1 -- even though the DXVK d3d8
  // build ships inside that same Proton. Measured on a Deck 2026-09-07, Tetra Master:
  // 61% -> 24% of one core, and the user's verdict was "WAY better".
  //
  // IMPORTANT: The key lives in the PROTON folder, so a Proton version change silently reverts
  // it. That is not a worry, it is history: it happened 2026-08-27 and cost eleven days
  // (see protondxvk.cpp). Hence the shim repairing it itself rather than trusting that
  // somebody re-runs install.sh.
  //
  // WARNING: Writes OUTSIDE the game folder, into the active Proton's user_settings.py, and it
  // affects EVERY title under that Proton -- the widest write this shim makes. Named
  // plainly in the label for that reason, and one click from off.
  // Read-only, and the cheap half of the pair: without it nothing in the client ever
  // said which renderer was in use, which is the entire reason the regression above
  // survived eleven days. One log line.
  { L"proton",    L"report",         L"1",    OPT_HIDDEN,
    L"Log which Direct3D 8 driver is loaded (DXVK or wined3d)", NULL, true, NULL },

  // WARNING: THE ONE OPTION HERE THAT CAN BREAK A GAME -- AND IT IS ON BY DEFAULT FROM
  // 2026-08-24. It defaulted to 0 until then, guarded by an except list carrying
  // FFXiMain.dll, because with it on FFXI drew its title screen, went black and had
  // to be force-quit (Steam Deck, 2026-08-15).
  //
  // Flipped deliberately, on a report that outranks that one: an exclusive fullscreen
  // device fights so hard for the display that the game cannot be LEFT. Windowed is
  // now the default for every title, the except list ships empty, and the two rows in
  // profiles.cpp that used to override this are PW_DEFAULT. The FFXI black screen is
  // not fixed by any of that -- it is accepted, with the row below as the way back.
  // HIDDEN since 2026-09-08: superseded by the Display row above, kept as the
  // fallback for an ini without it. Same for the two rows below.
  { L"dx",        L"d3d_windowed",   L"1",    OPT_HIDDEN, L"Run games in a window", NULL, false,
    L"On by default. Exclusive fullscreen takes over the whole display, which is what "
    L"makes a game hard to leave. If one game misbehaves windowed, put just that one "
    L"back below rather than turning this off.", true },
  // THE WAY BACK, and it is a VISIBLE dropdown now rather than a hidden
  // comma-separated string. While windowed was opt-in this was a safety guard nobody
  // needed to see; now that windowed is the default it is the only escape hatch, and
  // an escape hatch nobody can find is not one. Choices are pinned to the profile
  // table's fs_fallback rows by the selftest in polsettings.cpp -- add a title there
  // and forget this row and the dialog silently stops offering it.
  { L"dx",        L"d3d_windowed_except", L"", OPT_HIDDEN,
    L"Titles left fullscreen",
    L"=None -- every game runs in a window|"
    L"FFXiMain.dll=Final Fantasy XI|"
    L"FrontMissionOnline.dll=Front Mission Online|"
    L"FFXiMain.dll,FrontMissionOnline.dll=Both of them", false,
    L"Only for a game that is worse in a window than out of it. FFXI black-screened "
    L"windowed on one Steam Deck (2026-08-15), and FMO's opening movie may flicker "
    L"because it is drawn into the game's own window. Everything else should stay "
    L"windowed -- a fullscreen game takes the display over and is hard to get out of." },
  // FMO has no software keyboard on PC (its kana grids are dead PS2 data with no
  // code references) -- it drives the Windows IME. On by default because the
  // install is English now and typing Japanese into an English client is the rare
  // case, not the common one. WARNING: The trade is real and belongs in the note: the
  // IME still toggles by hand inside a field, but that choice stops being carried
  // to the NEXT field, because what this switches off is the restore of a
  // remembered flag -- there is no initialiser constant to change. See fmoime.cpp.
  // The opposite list: titles whose COMPILED profile verdict is cleared, so the
  // global d3d_windowed decides for them after all. Added 2026-08-21, because before
  // it there was NO way to run a title whose profile demanded a fullscreen mode the
  // adapter could not serve.
  //
  // IMPORTANT: HIDDEN FROM 2026-08-24, AND A NO-OP ON A STOCK BUILD. It offered exactly the
  // two PW_FULLSCREEN titles, FMO and FFXI, and both are PW_DEFAULT now -- so there is
  // no fullscreen profile verdict left for it to clear. The key is NOT deleted and
  // caller_except_why() still honours it: the mechanism is right, a future title may
  // need PW_FULLSCREEN, and an install carrying this is carrying something redundant
  // rather than something wrong.
  //
  // The control a user actually wants now is the OPPOSITE one, d3d_windowed_except
  // above, which is the visible dropdown. Leaving a live-looking "Ignore a title's
  // fullscreen profile" combo sitting next to it with every choice a no-op is exactly
  // the "previous iterations of fixes still in the ini" trap, one level up in the UI.
  { L"dx",        L"d3d_windowed_force", L"", OPT_HIDDEN,
    L"Clear a title's compiled fullscreen profile (no title has one since 2026-08-24)", NULL },
  // A fullscreen CreateDevice the shim passed through VERBATIM can fail when the
  // adapter no longer lists the requested mode (modern panels drop 800x600), and
  // FMO answers that with a deliberate crash. This retries the failed call
  // windowed -- it fires ONLY after the title's own request already failed, so a
  // machine that can serve fullscreen (the Deck) never sees it. d3d9hook.cpp.
  { L"dx",        L"d3d_fs_rescue",  L"1",    OPT_HIDDEN, L"Retry a failed fullscreen device windowed", NULL },
  { L"dx",        L"d3d_scale",      L"0",    OPT_ENUM, L"Game window size",
    L"0=Biggest that fits|1=1x (640x480)|2=2x|3=3x|4=4x|5=5x|6=6x", false,
    L"How big the window opens.", true },
  { L"dx",        L"d3d_remember_window", L"1", OPT_BOOL, L"Remember the size I drag the window to", NULL, false,
    L"Your last size wins over the setting above." },
  { L"dx",        L"d3d_window_w", L"0", OPT_HIDDEN, L"Remembered client width (the shim writes this)", NULL },
  { L"dx",        L"d3d_window_h", L"0", OPT_HIDDEN, L"Remembered client height (the shim writes this)", NULL },
  { L"dx",        L"d3d_window_rev", L"0", OPT_HIDDEN, L"Marks the remembered size as user-chosen (the shim writes this)", NULL },
  { L"dx",        L"d3d_aspect",     L"1",    OPT_BOOL, L"Keep the picture's shape when resizing", NULL, false,
    L"Off lets the picture stretch.", true },
  { L"dx",        L"close_prompt",   L"1",    OPT_BOOL,
    L"Ask what to do when you close a game window", NULL, false,
    L"The X offers Return to PlayOnline, Exit to desktop, or Cancel." },
  { L"dx",        L"dinput_mousescale", L"25", OPT_TEXT,
    L"Mouse speed in games (%)", NULL, false,
    L"Raise it if the pointer feels slow.", true },
  // The FMV window. Both have run ON with no row since they were written, which
  // is how "the movie plays in a corner and the logo is drawn twice" stayed a
  // report with nothing in the ini to point at. The class list is HIDDEN -- it
  // is a diagnostic knob, and the log's window census is how anyone would learn
  // what to put in it.
  // HIDDEN, not a checkbox. It was visible for about an hour, until fitting
  // learned to keep the movie's SHAPE (d3d8hook.cpp, adopt_video_proc). While it
  // stretched, "off" was a real escape hatch for a title whose movie we would
  // distort; now that it letterboxes, "off" only means "leave the movie in a
  // corner box", which nobody wants -- the same argument that retired
  // d3d_fitwindow. Healed so it is still repairable from the ini.
  { L"dx",        L"d3d_fitvideo",   L"0",    OPT_HIDDEN, L"Fit movie sequences to the game window", NULL, false, NULL, true },
  { L"dx",        L"d3d_videoclass", L"VideoRenderer,FilterGraphWindow", OPT_HIDDEN,
    L"Window classes that are the movie window (comma-separated)", NULL },
  // THE VIEWER SHELL's own size -- a different window from the game's, and until
  // 2026-08-17 it had no row at all, though the scaler that enlarges it has run
  // ON BY DEFAULT since it was written. "Why is the PlayOnline window this size?"
  // was unanswerable from the ini, which is exactly the question the
  // d3d_windowed shrink turned out to be (see d3d8hook.cpp hook_GetSystemMetrics).
  { L"dx",        L"d3d_borderless", L"0",    OPT_HIDDEN, L"Borderless fullscreen",
    L"0=Off|1=Fill the screen|2=Crisp (whole multiples, black bars)", false, NULL, true },
  { L"dx",        L"dpi_aware",      L"1",    OPT_BOOL, L"Sharp on high-DPI displays", NULL, false,
    L"Off lets Windows blur the window." },

  // ==========================================================================
  { NULL, NULL, NULL, OPT_GROUP, L"Controller", L"Controller", false, NULL },
  // Was a button in the bottom bar, three rows away from the controller settings
  // it edits.
  { NULL, NULL, NULL, OPT_BUTTON, L"Change button mapping...", L"padmap", false,
    L"Set which button does what, per game." },
  // ==========================================================================

  { L"inputmode", L"mode",           L"off",  OPT_ENUM, L"Controller mode",
    L"off=Whatever the game decides|force_gamepad=Always gamepad|force_keyboard=Always keyboard and mouse",
    false,
    L"A Steam Deck needs \"Always gamepad\"." },
  // The BUTTON MAP is edited by the "Controller mapping..." window, which binds by
  // pressing a button and applies live, so none of its keys is drawn here -- a text box
  // holding a stale copy beside a live editor would overwrite it on Save. They are still
  // rows so iniheal writes them into a deployed install: updating through POL Viewer's
  // patch manager replaces the DLL and never touches polshim.ini, so a key documented
  // only in dist/polshim.ini never reaches an install updated that way.
  { L"inputmode", L"pad_remap",      L"0",    OPT_HIDDEN, L"Live controller button mapping",     NULL },
  { L"inputmode", L"pad_bind",       L"",     OPT_HIDDEN, L"Button bindings (mapper writes this)", NULL },
  { L"inputmode", L"pad_title",      L"1",    OPT_HIDDEN, L"Apply a title's own pad map to its pad", NULL },
  // A VISIBLE row, unlike the rest of the pad machinery, because it is the answer to
  // the complaint that the mapping "gives no feedback": over the Viewer's DirectDraw
  // surface the mapper WINDOW can be invisible, and this draws the same mapper on the
  // surface instead. Discoverability is the feature.
  { L"inputmode", L"pad_overlay",    L"1",    OPT_BOOL, L"Show the controller mapper on screen", NULL, false,
    L"Draws the mapper over the game, for screens with no windows." },
  // The LAUNCH-TIME registry route, superseded by the mapper but kept working for
  // installs already relying on it. Hidden for the same reason: two editors for one
  // outcome is what made this feature confusing in the first place. The mapper's
  // "Details..." shows their live state, and they remain hand-editable here.
  { L"inputmode", L"pad_layout",     L"off",  OPT_HIDDEN, L"Face-button preset (legacy)",        L"off|xbox|ps" },
  { L"inputmode", L"swap_confirm",   L"0",    OPT_HIDDEN, L"Swap A/B (legacy)",                  NULL },
  { L"inputmode", L"force_pad",      L"1",    OPT_HIDDEN, L"Force each title's pad gate ON",     NULL },
  { L"inputmode", L"seed_pad",       L"1",    OPT_HIDDEN, L"Supply a working pad map when the registry has none", NULL },
  // IMPORTANT: HIDDEN 2026-08-24. It was a visible checkbox, and "off" has no correct
  // machine: off means FMO dies entering the game on Proton, and on costs
  // NOTHING anywhere else -- the guard only does anything when the call already failed.
  // A control whose only function is to break a title is not a setting, it is a
  // trap with a label. Same reasoning that RETIRED d3d_fitwindow and hid
  // d3d_fitvideo. The key is still healed and still read, so a diagnosing hand
  // can turn it from the ini -- what goes away is the invitation.
  { L"dx",        L"d3d9_decl_guard", L"1", OPT_HIDDEN,
    L"Front Mission Online: survive a failed vertex declaration", NULL },
  { L"inputmode", L"btn_ok",         L"-1",   OPT_HIDDEN, L"Confirm button index (legacy)",      NULL },
  { L"inputmode", L"btn_cancel",     L"-1",   OPT_HIDDEN, L"Cancel button index (legacy)",       NULL },
  // All FOUR, not two. swapconfirm_configure() reads btn_navi and btn_menu the
  // same way it reads the pair above (regredir.cpp, g_btns[i].ini_key), and
  // pad_layout sets all four -- so healing only two left the other half of the
  // preset un-overridable on any install that never saw the shipped template.
  { L"inputmode", L"btn_navi",       L"-1",   OPT_HIDDEN, L"Navi button index (legacy)",         NULL },
  { L"inputmode", L"btn_menu",       L"-1",   OPT_HIDDEN, L"Menu button index (legacy)",         NULL },

  // ==========================================================================
  { NULL, NULL, NULL, OPT_GROUP, L"Server", L"Server", false, NULL },
  // Both were buttons in the bottom bar. They act on the address field below, so
  // this is where they can be understood.
  { NULL, NULL, NULL, OPT_BUTTON, L"Save this server under a name...", L"srv_save", false,
    L"Keeps the address below so you can pick it again." },
  { NULL, NULL, NULL, OPT_BUTTON, L"Forget a saved server...", L"srv_del", false, NULL },
  // ==========================================================================

  // THE LABEL USED TO READ "Use a custom server", WHICH IS THE OPPOSITE OF WHAT
  // THIS DOES. [redirect] enable=1 points every pol.com name at REAL SQUARE ENIX,
  // bypassing whatever the machine's DNS/hosts would have answered -- the log
  // line it prints is "REDIRECT ACTIVE: ... This install is NOT talking to the
  // private server". So UNCHECKING it does not mean "go to Square Enix", it means
  // "stop bypassing", and on a box pointed at the private server you land there.
  // Cost a real test session on the Deck, 2026-08-15.
  { L"redirect",  L"enable",         L"0",    OPT_BOOL, L"Connect to the real Square Enix servers instead", NULL, false,
    L"Off plays here. On connects to Square Enix instead." },
  { L"redirect",  L"server",         L"",     OPT_SERVER, L"Private server address (when NOT bypassing)", NULL },
  // IMPORTANT: A DEVELOPER ROW SINCE 2026-09-08, and it ships ON. It is not a preference: it
  // is what stops an SE-domain name with no [redirect] entry falling through to real
  // DNS. Off, a client aimed at the private server can silently reach Square Enix --
  // there is no version of "yes please" to that, and offering it beside "Connect to
  // the real Square Enix servers" (which already decides where you connect) made the
  // pair read as a contradiction. Kept as a row so a diagnosis can still turn it off.
  { L"redirect",  L"strict",         L"1",    OPT_BOOL, L"Never fall back to Square Enix's own servers", NULL, true,
    L"Off lets an unlisted address reach Square Enix. Leave it on." },

  // ==========================================================================
  { NULL, NULL, NULL, OPT_GROUP, L"Login and updates", L"Login and updates", false, NULL },
  // ==========================================================================

  { L"session",   L"watch",          L"1",    OPT_HIDDEN, L"Log when the session socket is lost", NULL },

  // THE LOGIN THAT HANGS ON "Connecting to PlayOnline" -- and the POL-0008 /
  // POL-2059 it turns into. RULE 1: the fix has existed since authkey.cpp was
  // written, and it was reachable ONLY by hand-editing [auth] in polshim.ini --
  // a section NEITHER shipped template contains, so on every deployed install
  // it was the compiled default and nothing else. That is invisible delivery,
  // which is the failure this table exists to stop.
  //
  // What it is: the Viewer sometimes sends its NICK under a Blowfish session
  // key CACHED from an earlier dial instead of re-keying from the greeting the
  // server just sent. The server tries K=0, the last-issued key and every
  // persisted stamp, none of them decrypt it, and it drops the login -- which
  // the client draws as a hang and then an error. It cannot be repaired
  // server-side (post-NICK the channel is under the key the server does not
  // hold), so the lever is here: force K=0 before the keying, which our server
  // always tries and so always decrypts. A no-op when the login is healthy.
  //
  // IMPORTANT: Off by default, and the label says "if" rather than promising a cure:
  // authkey.cpp ships this OFF until a live log proves the hook fires on a
  // STUCK login (see `log` below). It is also REFUSED automatically whenever
  // [redirect] enable=1 -- real Square Enix issues a genuine non-zero token and
  // needs the real key -- which is why the hint names that row.
  { L"auth",      L"rekey_zero",     L"0",    OPT_BOOL,
    L"If a login hangs on \"Connecting to PlayOnline\", re-key it", NULL, false,
    L"Try this if a login hangs. Ignored when connecting to Square Enix." },
  // SELF-UPDATE. These MUST be healed rather than left to the shipped template:
  // the installer writes that template only when an install has none, so on every
  // EXISTING install the section would simply be absent, the code default (0)
  // would win, and turning it on in polshim.ini.production would reach nobody but
  // fresh installs. Healing is the whole delivery mechanism here.
  //
  // enable defaults to 1 -- deliberately, and it is the one row in this table that
  // changes behaviour on a machine that never asked for it. The justification is
  // that the alternative is worse: the Viewer's patch channel CANNOT replace
  // PolHook.dll on Windows (pol.exe maps it, so polcore's overwrite always fails),
  // so without this a shim fix reaches a Windows user only if they go and re-run
  // the installer, which is exactly how build 25 sat published with a known
  // hang-on-exit bug. Every refusal path is covered by the autoupdate test harness,
  // and the previous DLL is always kept beside the new one as PolHook.dll.b<NN>.old.
  { L"autoupdate", L"enable",   L"1",     OPT_BOOL, L"Keep the shim up to date automatically", NULL, false,
    L"The shim itself. Game updates are PlayOnline's own and are untouched." },
  { L"autoupdate", L"prompt",   L"title", OPT_ENUM, L"Tell me when it updates",
    L"title=In the title bar|dialog=Pop up a message|off=Don't tell me", false, NULL },
  { L"autoupdate", L"url",      L"",      OPT_TEXT, L"Update source (blank = this server)",  NULL },
  { L"autoupdate", L"delay_ms", L"10000", OPT_HIDDEN, L"Wait before checking (milliseconds)",            NULL },
  // Keep checking while the Viewer is open. A launch-only check missed anything
  // published during a session, and a POL session is an evening long.
  { L"autoupdate", L"interval_min", L"60", OPT_HIDDEN, L"Check again every (minutes, 0 = only at launch)",
    NULL, false,
    L"One tiny request per check. An update found mid-session is staged for your next restart." },
  { L"autoupdate", L"keep",     L"3",     OPT_HIDDEN, L"Old versions to keep",              NULL },
  // The boot guard's knobs (autoupdate_boot_guard). Healed so a diagnosing hand
  // can find and turn them, hidden because nobody should need to: the defaults
  // ARE the design -- confirm past the window where hook installs run (and past
  // the 5 s minimum-uptime rule for clean exits), roll back on the third boot
  // that never confirmed. Defaults here MUST match the compiled ones in
  // autoupdate_boot_guard -- the ini-defaults check holds the two together.
  { L"autoupdate", L"confirm_ms",     L"20000", OPT_HIDDEN, L"How long a staged build must survive to be confirmed (ms)", NULL },
  { L"autoupdate", L"rollback_after", L"2",     OPT_HIDDEN, L"Unconfirmed boots of a staged build before rolling back",   NULL },
  { L"patch",     L"noupdate",       L"0",    OPT_BOOL, L"Never let POL patch the Viewer",   NULL, true,
    L"Stops the Viewer overwriting a build you made yourself." },

  // Boot straight into a game, the way a title's own desktop shortcut does. The
  // stored value is the content id; a real polboot launch (or nothing selected)
  // always wins. Takes effect on the next Viewer launch.
  { L"shortcut",  L"game",           L"0",    OPT_ENUM, L"Open the Viewer as if from a game's shortcut",
    L"0=Off (normal Viewer)|1=Final Fantasy XI|2=Tetra Master|3=Mahjong (Janhourou)|"
    L"4=Front Mission Online|10=Dirge of Cerberus|11=Fantasy Earth|13=EverQuest II|"
    L"14=Friend List|15=FFXI Test Client", true,
    L"Same as launching that game's desktop shortcut. You still log in first." },

  // ==========================================================================
  { NULL, NULL, NULL, OPT_GROUP, L"Other", L"Other", false, NULL },
  { L"proton",    L"dxvk_d3d8",      L"1",    OPT_BOOL,
    L"Steam Deck: faster graphics for the older games", NULL, false,
    L"Applies to every game under that Proton, from the next launch." },
  // ==========================================================================

  { L"polshim",   L"titletag",       L"1",    OPT_BOOL, L"Show the shim version in the title bar", NULL, false,
    L"Adds the shim version after the window title." },
  // THE SERVER LABEL. On by default, and the default is the point: this exists
  // because on 2026-08-19 a laptop believed to be on PROD was talking to DEV,
  // nothing on screen said so, and the mistake was only found in the SERVER's
  // logs after hours on a "prod bug" that was a client on the wrong host. An
  // indicator nobody switches on cannot prevent that, so it ships on.
  { L"polshim",   L"titletag_server", L"1",   OPT_HIDDEN, L"Show which server you are connected to", NULL, false,
    L"Puts the login server's address in the title bar -- resolved live, so it says where you WILL go, not where you meant to." },
  { L"polshim",   L"titletag_probe", L"ci000.pol.com", OPT_TEXT, L"Name resolved for the server label", NULL, true,
    L"The login directory, hardcoded in polcore.dll -- whatever it resolves to IS the server you log in to. Change only if you know why." },
  // THE CRASH REPORTER (crashlog.cpp). Visible and on, because the thing it
  // replaces is reading one address out of the Windows event log with no stack --
  // and two of the six pol.exe crashes in Aug 2026 named PolHook.dll as the
  // faulting module, so "was it the shim" has to be answerable from the log the
  // user already has. It never handles the exception: a crash stays a crash.
  { L"polshim",   L"crashlog",       L"1",    OPT_HIDDEN, L"Record why the game crashed", NULL, false,
    L"Writes the faulting address and the call stack into the log. Costs nothing until a crash." },
  // HIDDEN diagnostic half of crashlog: log first-chance exceptions -- the
  // crash a title's OWN handler eats before it can go unhandled (FMO's "Force
  // quitting" teardown on the Deck is the motivating case: the primary fault
  // was consumed by the title's SEH and only a secondary teardown AV ever
  // reached a log). Observer only, capped, never handles anything.
  { L"polshim",   L"crashlog_first", L"1",    OPT_HIDDEN, L"Log exceptions the game handles itself", NULL },
  // Wine NULLs GlobalLock on an unaligned resource pointer (it pokes the
  // read-only .rsrc page below it for a lock count); Windows hands the pointer
  // back. The silent NULL left FMO's shader effect uncreated on the Deck and
  // crashed game entry (fmo-deck-gamestart-crash). Identity is Windows'
  // behaviour, so ON is a no-op everywhere the bug doesn't bite.
  { L"polshim",   L"reslock",        L"1",    OPT_HIDDEN, L"GlobalLock on a resource returns it as-is", NULL },
  // The Friend List's SetWindowRgn-shaped window shows a black surround under
  // Wine/KWin; give it a black colour key so the surround composites away.
  // Scoped to the WInFriendListWindow class, so nothing else is affected.
  { L"dx",        L"fl_window_transparent", L"1", OPT_HIDDEN, L"Key out the Friend List window's black surround", NULL },
  // The chord that opens this dialog is rebindable FROM this dialog. [settings] enable
  // is deliberately NOT a row: unticking it here would remove the only way back in.
  // An unparsable hotkey is not a lockout either -- polsettings falls back to the
  // default and says so in the log.
  // A LIST -- any chord opens the dialog. See hotkeydef.h for why the default is two.
  { L"settings",  L"hotkey",         POLSHIM_DEFAULT_HOTKEY, OPT_TEXT, L"Key that opens this window", NULL, false,
    L"Comma-separated -- any of them opens it." },
  { L"settings",  L"pad_chord",      L"back+start",   OPT_TEXT, L"Controller buttons that open this window", NULL, false,
    L"On a Steam Deck, \"back\" is the View button." },
  { L"settings",  L"display_hotkey", L"alt+enter", OPT_TEXT,
    L"Key that switches a game between windowed and borderless", NULL, false,
    L"Works while the game is in front. Comma-separated for more than one." },
  // The CONTROLLER MAPPER's own chord -- a separate one on purpose. The mapper has to
  // stay reachable on a machine where this dialog cannot be seen at all, which is the
  // entire reason it exists, so it must not depend on a chord bound from in here.
  { L"settings",  L"padui_chord",    L"back+lb",      OPT_HIDDEN, L"Controller buttons that open the mapper", NULL, false,
    L"Shares \"back\" with the chord above but adds a shoulder, so the two cannot be confused." },
  { L"settings",  L"padui_hotkey",   L"ctrl+shift+p", OPT_HIDDEN, L"Key that opens the mapper", NULL, false,
    L"For a desktop; on a Deck use the controller chord above." },
  // HIDDEN on purpose: this re-enables a MEASURED CRASH (opening the dialog over
  // FFXI's exclusive fullscreen device kills the title -- Steam Deck, 2026-08-15).
  // It is an escape hatch for someone who knows their title survives it, not a
  // checkbox to invite clicking. Healed into the ini so it is discoverable there.
  { L"settings",  L"fullscreen_ok",  L"0",    OPT_HIDDEN, L"Open the dialog even over a fullscreen title", NULL },

  // --- THE DEVELOPER GATE, and it is deliberately a VISIBLE row -------------
  //
  // Ticking this reveals every `dev` row below. It is not itself a dev row --
  // that would be a lockout of the same shape as [settings] enable, which is
  // why THAT is not a row at all -- and it persists, so it is ticked once per
  // install rather than every launch.
  { L"settings",  L"show_dev",       L"0",    OPT_BOOL, L"Show developer options", NULL, false,
    L"Diagnostics and logging. None of it is needed to play." },

  // ONE level for what used to be nine independent trace keys (polshim.h says
  // which, and what is deliberately not folded in). Every old key still works as
  // an override, so a tuned developer ini keeps its behaviour; this is what a
  // player's install gets, and 0 is the point -- the old compiled defaults had
  // most of them ON, contradicting the shipping rule.
  { L"polshim",   L"trace",          L"0",    OPT_ENUM, L"Log detail",
    L"0=Off (decisions and errors)|1=What each layer did|2=+ per-message spies|3=+ image dumps and call sites",
    true,
    L"Level 3 writes a dump of the running game's memory image next to the shim." },

  // --- DEVELOPER ROWS: drawn only with the box above ticked -----------------
  //
  // The separator is a dev OPT_GROUP rather than a special case in the dialog: it
  // then hides and lays out by the same rule as every other heading, which is one
  // fewer thing that can disagree with the height the layout measured.
  { NULL, NULL, NULL, OPT_GROUP, L"Developer options", L"Developer", true, NULL },
  //
  // THE MASTER BYPASS -- the answer to "is the shim causing this?".
  //
  // Ticking it stands the WHOLE shim down on the next launch: no hooks, no
  // probes, no patches, no DNS redirect, no autoupdate, no automation. The COM
  // proxy still forwards (the Viewer gets polcore through it) but hands back the
  // genuine object unwrapped. It exists because the obvious way to test without
  // the shim does not work -- pulling the injector leaves the self-loading proxy
  // in the install tree loading anyway, so "I ran without the shim" has been
  // said about runs that were fully armed.
  //
  // THE WAY BACK IS THIS SAME BOX. The settings watcher is the ONE thing the
  // bypass keeps running (see the gate in inject.cpp), specifically so a bypass
  // ticked here can be unticked here. The first cut made it a one-way door on
  // the reasoning that a surviving watcher makes "the shim was off" almost-true
  // rather than true; that was overruled, and rightly -- on a Deck in
  // Game Mode "edit the ini" is not a way back at all, and a switch you cannot
  // undo is one nobody dares use. The honesty is paid for in the LOG instead,
  // which names the watcher as the single exception rather than claiming the
  // process is clean.
  //
  // Developer row: this is a debugging instrument, not a setting, and it belongs
  // behind the same box as the traces. RB_ALWAYS in polsettings' restart table
  // would be redundant -- it is read once at startup and never re-read, which
  // `shim_reload` cannot change.
  { L"polshim",   L"bypass",         L"0",    OPT_BOOL,
    L"Disable the ENTIRE shim (takes effect next launch; untick here to restore)",
    NULL, true },
  // The half-step bypass leaves out: keep the network routing, login and logging
  // that reach our server, but turn OFF everything that touches the GAME (all the
  // Direct3D/windowing/cursor/mask, DirectInput, FMV and byte-patch hooks). The
  // answer to "is a problem the shim or the game itself?" without losing the
  // connection. inject.cpp reads [polshim] minimal once at startup.
  { L"polshim",   L"minimal",        L"0",    OPT_BOOL,
    L"Testing: routing only -- keep the server connection, turn off all game fixes (next launch)",
    NULL, true },
  // FANTASY EARTH TEARDOWN GUARD (fepatch.cpp). Trampolines the NULL-child read at
  // FE_Client.dll+0x2AC5C4 so a NULL scene-graph link is treated as end-of-list
  // instead of faulting -- the crash that kills FE just past its logos under
  // Wine/Proton (Steam Deck). A no-op on desktop Windows, where the graph is
  // sentinel-terminated and esi is never NULL.
  //
  // RESOLVED: DEFAULT 0 -> 1, AND HIDDEN, 2026-08-24. This row shipped off with an explicit
  // promotion condition written into this comment -- "flip to 1 as its default once a
  // live run past the logos confirms it" -- because at the time it was byte-verified
  // but UNMEASURED. **That run happened and is recorded**:
  // Steam Deck, 2026-08-20, polshim.328.log. The guard applied once
  // (`[fe] guard @ 04B9C5C4 ... applied`), 0x2AC5C4 NEVER FAULTED AGAIN (zero AVs, no
  // polshim-crash.txt), and FE advanced far past the logos -- its own CreateDevice, a
  // full DirectSound bank load, a quartz filter graph, and LIVE WORLD-CONNECT
  // (`[connect] 198.51.100.10:51300`, `HTTP/1.1 200 OK`). FE then dies at a LATER,
  // DIFFERENT wall (layer 8, at/after the FMV graph). That is the condition met: the
  // guard does what it claims and does not cause the remaining failure.
  //
  // Hidden rather than left a dev checkbox for the same reason as d3d9_decl_guard
  // above: now that it is on, "off" only means "put the Deck crash back", and it costs
  // nothing where it is not needed. Still healed, so it is repairable from the ini.
  // Config rev 8 carries the flip to installs that already have the 0.
  { L"dx",        L"fe_teardown_guard", L"1", OPT_HIDDEN,
    L"Fantasy Earth: guard the scene-graph teardown crash (Proton/Deck)", NULL },
  // The game window's class brush is swapped to BLACK when the shim learns the
  // window (d3d_set_game_window). FMO registers FMOClass with COLOR_WINDOW+1, so
  // forced-windowed loading showed a WHITE client ("white box in a black screen",
  // 2026-08-26, two machines); fullscreen retail never let the brush show. Skipped
  // for native-windowed titles. 0 restores SE's brush behaviour.
  { L"dx",        L"d3d_blackbg",     L"1", OPT_HIDDEN,
    L"Paint the game window's background black (its own brush is white)", NULL },
  // SKIP THE OPENING/FMV MOVIE (fmvskip.cpp). FE and FMO play a DirectShow movie
  // that Wine's quartz crashes decoding; on the Deck FE dies the instant it renders
  // its opening .avi. This wraps the graph and skips RenderFile for a video file.
  // DEFAULT 4 (refuse the graph) since 2026-08-23 -- matches the compiled literal.
  // Level 2 was the obvious choice and is a PLACEBO for FE: it only intercepts
  // IGraphBuilder::RenderFile, and FE never calls it. WARNING: The 2026-08-23 reading that
  // FMO does not either was WRONG: FMO calls RenderFile on `fmo.dat`, a .dat the
  // video-extension filter never matched, so no line was ever logged (fixed
  // 2026-08-26; FMO's profile level is 3 now). Level 4 refuses the graph at creation,
  // which works whatever the title does with it. LIVE on the Deck: `[fmv] REFUSED
  // filter graph for FrontMissionOnline.dll ... returning REGDB_E_CLASSNOTREG` and
  // FMO then loads and plays. Levels: 0 off, 1 trace, 2 skip-success, 3 skip-failure,
  // 4 refuse, 5 build-but-never-run. A no-op for a title with no movie (TM/FFXI).
  //
  // IMPORTANT: "FE uses the same DirectShow path but is NOT yet verified at level 4" -- it is
  // verified now, and the answer is that IT CRASHES (2026-08-27). Level 4 denies FE
  // the graph its no-movie path assumes, and it dies at RVA 0x1DBD24 seconds later.
  // FE's level is 5, carried by its profile, and profiles now apply under Wine too --
  // which they did not, which is why the Deck fed FE level 4 for months.
  // WARNING: fmv_skip IS GATED ON WINE AT RUNTIME (fmvskip.cpp). On Windows any level >0
  // is ignored and logged, because the skip is a Wine-quartz workaround and level
  // 4's no-movie path crashes Fantasy Earth on Windows (RVA 0x1DBD24, live
  // 2026-08-23 -- the same day rev 6 below turned it on for everyone). The row
  // below is left at 4 deliberately: it is the right value where it applies, and
  // the gate means it costs nothing where it does not.
  // per_title, because the answer differs BY TITLE on Windows: FE must stay gated
  // (level 4 crashes it) and FMO must not be (it has no movie switch of its own).
  // The profile supplies the default. A GLOBAL 1 forces every title; a [dx.<module>]
  // value is that title's explicit answer either way. WARNING: The healed global 0 below is
  // NOT "off": it means "no global force, the profile decides" -- because this row
  // guarantees the key is always present, reading it with the profile as the default
  // shadowed the profile on every machine (2026-08-26, fmvskip.cpp fmv_force_for_title).
  { L"dx",        L"fmv_skip_force", L"0", OPT_HIDDEN,
    L"Apply fmv_skip on Windows too (it is Wine-only by default)", NULL, false, NULL, true },
  // WARNING: THE LABEL USED TO OVERPROMISE. It said "Skip the opening movie" with no hint
  // that on Windows it applied to nothing, which is exactly how it came to be ticked
  // while the movie played (2026-08-26). The row now says where the REAL per-game
  // switches live, because for every title except FMO that is the right answer and
  // this one is the last resort.
  { L"dx",        L"fmv_skip", L"4", OPT_ENUM, L"Skip the opening movie (shim-side, last resort)",
    L"0=Play the movie|1=Trace only|2=Skip it|3=Skip (report failure)|4=Refuse the graph"
    L"|5=Build it but never play", true,
    L"Prefer each game's OWN switch, in \"Each game's own settings\" -- they work on "
    L"Windows, this does not. You should not need to touch this: each title now "
    L"carries its own level and uses it under Proton as well as on Windows. 4 REFUSES "
    L"the graph, which crashes Fantasy Earth -- FE's level is 5, which builds the "
    L"graph and simply never starts it. On Windows this applies only to Front Mission "
    L"Online, which ships no switch of its own.", true },
  // =====================================================================
  // EACH GAME'S OWN SETTINGS -- ITS OWN SETTINGS APP
  // =====================================================================
  //
  // KEY: ONE BUTTON PER TITLE, AND NOT ONE SETTING OF ITS OWN.
  //
  // This block briefly held ~30 rows that read and wrote the titles' registry keys and
  // ini files directly. They are gone on purpose (2026-08-26). Every one of those
  // settings already has a screen written by the people who wrote the game, and that
  // screen knows what we cannot: the resolutions the game accepts, what its quality
  // levels are called, and which combinations are valid. Duplicating it bought two
  // editors that had to agree about hives, value types, UAC virtualization and
  // defaults -- and a column of numbers with no scale next to them.
  //
  // The group is tagged with `title_of`, so it is NOT a sidebar entry of its own: its
  // button appears inside that game's existing section. One game, one door.
  //
  // A game with no tool installed shows nothing here -- see row_absent_here.

  { NULL, NULL, NULL, OPT_GROUP, L"PlayOnline Viewer", L"PlayOnline Viewer", false,
    L"The PlayOnline menus and portal -- not the games." },
  { L"dx",        L"shell_scale_enable", L"1", OPT_BOOL, L"Enlarge the PlayOnline window", NULL, false,
    L"The menus and the portal. Off leaves them at 640x480." },
  { L"dx",        L"shell_scale",    L"0",    OPT_ENUM, L"How much to enlarge it",
    L"0=Biggest that fits|1=1x (640x480)|2=2x|3=3x|4=4x|5=5x|6=6x", false, NULL },
  { NULL, NULL, NULL, OPT_BUTTON, L"Open PlayOnline Viewer settings...", L"cfgapp:PlayOnline Viewer", false, NULL },

  { NULL, NULL, NULL, OPT_GROUP, L"Fantasy Earth's own settings", NULL, false,
    L"Resolution, texture and shadow quality, water reflections, the opening movie.",
    false, OPTM_ANY, L"FE_Client.dll" },
  { L"fecfg",     L"skip_opening_movie", L"1", OPT_BOOL,
    L"Skip the opening movie", NULL, false,
    L"Changes the game's own setting, once. Change it back in its config app." },
  { NULL, NULL, NULL, OPT_BUTTON, L"Open Fantasy Earth Config...", L"cfgapp:Fantasy Earth", false, NULL },
  { NULL, NULL, NULL, OPT_BUTTON, L"Fantasy Earth gamepad setup...", L"padapp:Fantasy Earth", false, NULL },

  { NULL, NULL, NULL, OPT_GROUP, L"Front Mission Online's own settings", NULL, false,
    L"Resolution, refresh rate and gamepad. Close the Viewer before opening it.",
    false, OPTM_ANY, L"FrontMissionOnline.dll" },
  { L"dx",        L"fmo_ime_direct", L"1", OPT_BOOL, L"Type English without switching keyboards", NULL, false,
    L"Text boxes start in English." },
  { NULL, NULL, NULL, OPT_BUTTON, L"Open Front Mission Online Config...", L"cfgapp:Front Mission Online", false, NULL },

  { NULL, NULL, NULL, OPT_GROUP, L"Tetra Master's own settings", NULL, false,
    L"Sound, gamepad, and its button assignments.",
    false, OPTM_ANY, L"TM.dll" },
  { NULL, NULL, NULL, OPT_BUTTON, L"Open Tetra Master Config...", L"cfgapp:Tetra Master", false, NULL },

  { NULL, NULL, NULL, OPT_GROUP, L"Final Fantasy XI's own settings", NULL, false,
    L"Resolution, sound, mip and bump mapping, the opening movie. FFXI stores these "
    L"under numbered names, so its own tool is the only place they have labels.",
    false, OPTM_ANY, L"FFXiMain.dll" },
  { NULL, NULL, NULL, OPT_BUTTON, L"Open FINAL FANTASY XI Config...", L"cfgapp:Final Fantasy XI", false, NULL },
  { NULL, NULL, NULL, OPT_BUTTON, L"FINAL FANTASY XI gamepad setup...", L"padapp:Final Fantasy XI", false, NULL },

};

const ShimOption* shim_options(int* count)
{
    if (count) *count = (int)_countof(g_opts);
    return g_opts;
}

void iniheal(const wchar_t* ini)
{
    static const wchar_t* SENTINEL = L"\x01\x7f\x01";   // a value no real ini would hold
    int added = 0;
    for (int i = 0; i < _countof(g_opts); i++) {
        // Headings and fix-packs own no key of their own -- a pack's members are
        // ordinary rows elsewhere in the table and heal on their own account.
        if (!g_opts[i].key || !g_opts[i].key[0]) continue;
        wchar_t cur[256];
        // The one profile read in the shim that is deliberately NOT ini_str: this asks
        // "is the key THERE", not "what does it say", and the sentinel answers that
        // whatever text follows. Stripping a comment first could only turn a present
        // key into an absent-looking empty one and heal a default over the user's line.
        GetPrivateProfileStringW(g_opts[i].sec, g_opts[i].key, SENTINEL, cur, _countof(cur), ini);
        if (wcscmp(cur, SENTINEL) == 0) {               // key absent -> write its default
            if (WritePrivateProfileStringW(g_opts[i].sec, g_opts[i].key, g_opts[i].def, ini))
                added++;
        }
    }
    if (added) logf("[iniheal] added %d missing option(s) to polshim.ini with defaults", added);
}

// ===========================================================================
// MIGRATION -- the half iniheal cannot do
// ===========================================================================
//
// Read polshim.h's note on inimigrate() first; this is the table it walks.
//
// EVERY ENTRY HERE IS EVIDENCE, NOT AN OPINION. `was` lists values that THIS
// PROJECT shipped as a default -- in dist/polshim.ini, polshim.ini.production or
// the table above -- and has since replaced. That is the whole admission test,
// and it is what makes the rewrite defensible: we are correcting our own old
// answer on an install that never got the new one, not overruling a choice.
//
// The rule it deliberately does NOT cover: a key whose COMPILED literal was once
// wrong but whose shipped templates were always right (dinput_mousescale 100,
// dinput_abs_clip 1, d3d_clientrect 1). A stale literal is only ever reached when
// the key is ABSENT from the ini -- in which case iniheal writes the current
// default and there is nothing to migrate. A key physically present at that value
// got there by hand, and a hand-set value is exactly what this must not touch.
//
// TO ADD ONE: bump POLSHIM_CONFIG_REV, give the new entries that rev, and say in
// `why` what BREAKS on the old value -- that string is logged and shown to the
// user, so it is the only explanation anybody gets.
//
// IMPORTANT: TWO PROPERTIES OF THIS TABLE THAT ARE EASY TO MISS, both learned the hard way on
// 2026-08-24 while making windowed mode the default. A sabotage run proved both.
//
// 1. A rev-1 ROW CAN REACH A MACHINE BUILT TODAY. inject.cpp runs iniheal() and THEN
//    inimigrate(). A brand-new ini has no config_rev, which reads as 0, so EVERY
//    migration ever written is walked over a file iniheal filled in moments earlier
//    with today's defaults. So a row whose `was` equals a key's PRESENT-DAY default
//    fires on every new install and silently un-does that default, for ever.
//    => WHENEVER YOU CHANGE AN iniheal DEFAULT, grep this table for that key and
//       delete or retarget any row whose `was` is the value you just adopted.
//       Changing d3d_windowed_except's default to L"" made the rev-1 row that read
//       L"" -> L"FFXiMain.dll" start matching fresh installs, which would have undone
//       the entire windowed-by-default change on exactly the machines it was for.
//
// 2. ORDER IN THIS ARRAY IS LOAD-BEARING, AND IT IS NOT THE SAME AS rev ORDER. The
//    walk is a single top-to-bottom pass, so when two rows touch one key BOTH can run
//    in the same pass and the LAST one listed wins -- regardless of rev. That is why
//    the newest rev goes at the TOP: a superseded older row sitting below it would
//    quietly overwrite the new value. (This is also why the sabotage run above only
//    reproduced the bug once the old row was put back at the BOTTOM, where it really
//    lived; from the top it was harmlessly corrected by the newer row below it.)
//
// polsettings.cpp's selftest "a NEW install survives the migration pass with its
// defaults intact" pins property 1 for every keyed option at once. It is the check to
// run after touching either this table or a default in g_opts.
#define POLSHIM_CONFIG_REV 10

struct ShimMigration {
    int            rev;    // applied when the ini's config_rev is below this
    const wchar_t* sec;
    const wchar_t* key;
    // '|'-separated superseded values, compared case-insensitively after the
    // trailing-comment strip. L"" matches a key present but empty. L"*" matches
    // any value and is only for a retired key being deleted.
    const wchar_t* was;
    const wchar_t* now;    // NULL = delete the key (it is retired and reads as live)
    const wchar_t* why;    // what the old value breaks, in the user's terms
};

static const ShimMigration g_migrations[] = {

  // --- rev 8: 2026-08-24 ------------------------------------------------------
  //
  // 1. THE LOGIN-KEY DIAGNOSTIC STOPS BEING ON BY DEFAULT. `[auth] log` printed
  //    `polcryptInit K=<hex> IV=<hex> sbox=<hex>` -- the Blowfish SESSION KEY -- into
  //    the shim log on every login. A diagnostic that ships ON must be safe ON. It
  //    is still exactly the right switch for a stuck login.
  //
  // 2. THE FANTASY EARTH TEARDOWN GUARD GETS PROMOTED. Its row carried an explicit
  //    promotion condition -- "flip to 1 as its default once a live run past the logos
  //    confirms it" -- written when it was byte-verified but unmeasured. The run is
  //    recorded: Deck, 2026-08-20. Guard applied,
  //    FE_Client+0x2AC5C4 never faulted again, FE reached live world-connect, and the
  //    remaining death is a different, later wall. Wine-gated, so a no-op on Windows.
  //
  // Admission test satisfied for both: `1` and `0` are the values this project shipped
  // as those rows' defaults, not values a person typed.
  //
  // WARNING: Trap check (see the notes above this table), done for both keys: neither `0` nor
  // `1` is now the *current* default of any OTHER row these could collide with, and
  // no earlier rev touches `[auth] log` or `[dx] fe_teardown_guard` -- so nothing
  // below can undo either on a fresh install. The selftest "a NEW install survives the
  // migration pass with its defaults intact" is what actually proves that.
  { 8, L"auth", L"log", L"1", L"0",
    L"It wrote your login session key into the log, and shipping a log sent it too." },
  { 8, L"dx", L"fe_teardown_guard", L"0", L"1",
    L"Fantasy Earth crashed just past its logos under Proton; the guard is proven and free." },

  // --- rev 7: 2026-08-24 ------------------------------------------------------
  //
  // WINDOWED MODE BECOMES THE DEFAULT, AND IT HAS TO BE A MIGRATION.
  //
  // The reported symptom is not "I would prefer a window": it is that an exclusive
  // fullscreen device fights so hard for the display that the game cannot be LEFT.
  // An exclusive device owns the screen, so alt-tab, the settings chord (a NO-OP
  // while one is live -- polsettings.cpp exclusive_display_blocks_us), overlays and
  // recovery from sleep are all things it takes away.
  //
  // A default change alone would reach NOBODY. d3d_windowed shipped as an explicit
  // `0` in the iniheal table and in all three templates (polshim.ini,
  // polshim.ini.production, dist/polshim.ini), so every install that exists carries
  // that 0 physically, and iniheal only ever ADDS absent keys. Same for the except
  // list at FFXiMain.dll. This is [[shim-old-default-migration]], the failure mode
  // that has now cost this project four separate "the setting does nothing" reports.
  //
  // Admission test satisfied for both: 0 and FFXiMain.dll are values this project
  // SHIPPED as row defaults, not values a person typed. Someone who sets either back
  // after this rev keeps their choice -- config_rev only steps once.
  //
  // WARNING: Two more halves of this change are NOT here, because a migration cannot make
  // them: the profile rows in profiles.cpp (FFXI and FMO were PW_FULLSCREEN, which
  // outranked d3d_windowed entirely) ship in CODE, and the rev-1 row that used to add
  // FFXiMain.dll to this list had to be DELETED rather than reversed -- see the note
  // where it used to be, at the bottom of this table.
  { 7, L"dx", L"d3d_windowed", L"0", L"1",
    L"A fullscreen game takes over the whole display and is very hard to get back out of." },
  { 7, L"dx", L"d3d_windowed_except", L"FFXiMain.dll", L"",
    L"FFXI was being held fullscreen; it runs in a window like everything else now." },

  // --- rev 6: 2026-08-23 ------------------------------------------------------
  //
  // FMV SKIP ON BY DEFAULT. Fantasy Earth and Front Mission Online build a
  // DirectShow graph for their opening movie and Wine's quartz dies decoding it;
  // FE's own OPENING_MOVIE=0 does not stop the graph being built. fmv_skip=4
  // refuses the graph at creation so the title takes its no-movie path.
  //
  // This has to be a migration, not just a default change: fmv_skip shipped as an
  // explicit 0 in the option table, so EVERY install healed before today carries
  // that 0 and a new default would reach none of them.
  //
  // Admission test satisfied: 0 is a value this project shipped as the row default
  // (src/iniheal.cpp) and in polshim.ini -- not a value a person typed. Anyone who
  // sets it back to 0 after this rev keeps 0, because config_rev only steps once.
  { 6, L"dx", L"fmv_skip", L"0", L"4",
    L"Fantasy Earth and Front Mission Online die under Proton decoding their opening movie." },

  // --- rev 5: 2026-08-19 ------------------------------------------------------
  //
  // THE PER-LAYER TRACE ROWS THAT DEFEAT THE ONE TRACE LEVEL. Every one of these
  // keys defaults from trace_at(N) so that [polshim] trace is the single control --
  // but "an explicit row still wins" (polshim.h), and this project SHIPPED all of
  // them as explicit 0 rows (dist/polshim.ini, polshim.ini.production; two were
  // even iniheal rows). Measured live on the Steam Deck 2026-08-19: the user
  // set trace=3 in the dialog and the render-spy stayed dark, because a template-era
  // d3d_renderspy=0 row was overriding it. Removing the row (only where it still
  // holds the shipped 0 -- a deliberate 1 survives, and so does a 0 re-added after
  // this rev) hands control back to the level. The templates no longer carry these
  // rows and iniheal no longer heals any of them in.
  { 5, L"dx",       L"trace",           L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the DirectDraw/Sound layer." },
  { 5, L"dx",       L"hook_trace",      L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the hook-chain spy." },
  { 5, L"dx",       L"d3d_trace",       L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the d3d8/d3d9 layer." },
  { 5, L"dx",       L"d3d_msgspy",      L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the window-message spy." },
  { 5, L"dx",       L"d3d_dumpcaller",  L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the caller dump." },
  { 5, L"dx",       L"d3d_renderspy",   L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the render spy -- the Deck measurement." },
  { 5, L"dx",       L"dinput_trace",    L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the DirectInput layer." },
  { 5, L"dx",       L"dinput_callsite", L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the poll-site reporter." },
  { 5, L"polfetch", L"trace",           L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the fetch hook." },
  { 5, L"ffxi",     L"trace",           L"0", NULL,
    L"An explicit 0 here overrode [polshim] trace for the FFXI plugin loader." },

  // --- rev 1: everything shipped before 2026-08-18 ---------------------------

  // Tetra Master's card cursor. Mode 1 steers the game's cursor with DirectInput
  // deltas and the two drift apart; mode 2 writes the game's own cursor variable,
  // so it lands where you point. Shipped as 1 until 2026-08-16.
  { 1, L"dx", L"dinput_mouseabs", L"1", L"2",
    L"Tetra Master: the card cursor drifted away from the pointer on the old setting." },

  // The mask window. 1 only HIDES it, which leaves polcore's CreateInput mapping
  // against the rectangle it had before -- so the pointer and the game disagree
  // about where the window is. 2 ALIGNS it and parks it behind. Shipped as 1 in
  // polshim.ini.production until 2026-08-18.
  { 1, L"dx", L"d3d_unmask", L"1", L"2",
    L"The mask window was hidden rather than aligned, so input landed in the wrong place." },

  // IMPORTANT: REMOVED 2026-08-24. It read:
  //
  //   { 1, L"dx", L"d3d_windowed_except", L"", L"FFXiMain.dll",
  //     L"FFXI black-screens in windowed mode; it has to be left fullscreen." },
  //
  // Rev 7 reverses it, so leaving it here would be two rows fighting -- but the reason
  // it is DELETED rather than left as dead history is an ordering hazard that only
  // appears now that the key's iniheal default is empty:
  //
  //   inject.cpp runs iniheal() and THEN inimigrate(). On a fresh install iniheal
  //   writes the current default (now "") and config_rev is absent, i.e. 0 -- so
  //   EVERY migration is walked, including this one. Its `was` is L"", which is
  //   exactly what iniheal just wrote. It would match, and put FFXiMain.dll back on
  //   a brand-new install, silently, for ever. Rev 7 would not save us: its `was` is
  //   FFXiMain.dll, which does not match the freshly-healed empty value.
  //
  // This was harmless while the healed default WAS FFXiMain.dll (the `was` could never
  // match a fresh file). Changing an iniheal default therefore means re-reading every
  // migration whose `was` is that key's OLD default -- a rev-1 row can reach a machine
  // built today.

  // Registration repair. Off in the first shipped template. app.dll asks
  // ProgIDFromCLSID for content ids 1/2/14, so an unregistered class drops the
  // title out of Check Files ENTIRELY -- which is what hid Tetra Master and the
  // Friend List on a second machine.
  { 1, L"regfix", L"enable", L"0", L"1",
    L"Titles whose registration is missing or wrong vanish from Check Files completely." },

  // The settings chord. A Steam Deck in Game Mode has no keyboard to press
  // Ctrl+Shift+S with, so Home was added as a second chord on 2026-08-15 -- and
  // an install stuck on the old single chord cannot open this dialog at all.
  { 1, L"settings", L"hotkey", L"ctrl+shift+s", POLSHIM_DEFAULT_HOTKEY,
    L"On a Steam Deck there is no keyboard, so there was no way to open this window." },

  // POL spells its own registry key BOTH ways ("Controler" is SE's typo). Reading
  // only the correct spelling missed the button map on the installs that have the
  // other one.
  { 1, L"inputmode", L"swap_key", L"Controller", L"Controller,Controler",
    L"POL misspells its own controller registry key on some installs." },

  // RETIRED, and the code ignores it (d3d8hook.cpp says why). Left in the ini it
  // reads as a live knob sitting at a value that looks like it turns a fix off,
  // which is exactly the "previous iterations of fixes still in the ini" problem.
  // Deleted rather than corrected: there is nothing to correct it to.
  { 1, L"dx", L"d3d_fitwindow", L"*", NULL,
    L"Retired -- fitting the window is unconditional now, and this row did nothing." },
};

// A present-but-empty value and an absent key are different questions, and the
// profile API answers both with "". Same sentinel trick iniheal uses.
static bool ini_present(const wchar_t* sec, const wchar_t* key, const wchar_t* ini,
                        wchar_t* out, size_t cch)
{
    static const wchar_t* SENTINEL = L"\x01\x7f\x01";
    GetPrivateProfileStringW(sec, key, SENTINEL, out, (DWORD)cch, ini);
    if (wcscmp(out, SENTINEL) == 0) { out[0] = 0; return false; }
    ini_decomment(out);
    return true;
}

static bool was_matches(const wchar_t* was, const wchar_t* cur)
{
    if (wcscmp(was, L"*") == 0) return true;
    // Walk the '|'-separated alternatives without copying: the field is a literal.
    const wchar_t* p = was;
    for (;;) {
        const wchar_t* e = wcschr(p, L'|');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        if (wcslen(cur) == n && _wcsnicmp(cur, p, n) == 0) return true;
        if (!e) return false;
        p = e + 1;
    }
}

// Applies one entry. Returns 1 if it changed the file. `report` is optional.
static int migrate_one(const ShimMigration& m, const wchar_t* ini,
                       char* report, size_t cap, size_t* used)
{
    wchar_t cur[256];
    if (!ini_present(m.sec, m.key, ini, cur, _countof(cur))) return 0;  // iniheal owns absent keys
    if (!was_matches(m.was, cur)) return 0;                             // a value we did not ship: leave it
    if (m.now && wcscmp(cur, m.now) == 0) return 0;                     // already there

    if (!WritePrivateProfileStringW(m.sec, m.key, m.now, ini)) {
        logf("[inimigrate] could NOT rewrite [%ls] %ls (%lu) -- is the ini writable?",
             m.sec, m.key, GetLastError());
        return 0;
    }
    if (m.now)
        logf("[inimigrate] [%ls] %ls: '%ls' -> '%ls'  (%ls)", m.sec, m.key, cur, m.now, m.why);
    else
        logf("[inimigrate] [%ls] %ls: removed (was '%ls')  (%ls)", m.sec, m.key, cur, m.why);

    if (report && used && *used < cap) {
        int n;
        if (m.now)
            n = _snprintf_s(report + *used, cap - *used, _TRUNCATE,
                            "  [%ls] %ls: %ls -> %ls\n      %ls\n",
                            m.sec, m.key, cur, m.now, m.why);
        else
            n = _snprintf_s(report + *used, cap - *used, _TRUNCATE,
                            "  [%ls] %ls: removed (was %ls)\n      %ls\n",
                            m.sec, m.key, cur, m.why);
        if (n > 0) *used += (size_t)n;
    }
    return 1;
}

// rev 10 (2026-09-08): derive the new [dx] display row from the three legacy rows it
// replaces, so an install that had chosen borderless, or had left a title fullscreen,
// keeps exactly that choice under the new control. iniheal has ALREADY added
// display=windowed by the time this runs (it only adds absent keys), so this
// overwrites that placeholder from the legacy values, once. A title in the old
// "Titles left fullscreen" list becomes [dx.<module>] display=fullscreen -- the one
// per-title write this file makes, and it is a conversion of the user's own choice,
// not a default (the spec's rule against healing per-title sections is about
// DEFAULTS, which would freeze every later global change).
static int migrate_display(const wchar_t* ini)
{
    int changed = 0;
    const int windowed   = GetPrivateProfileIntW(L"dx", L"d3d_windowed",   1, ini);
    const int borderless = GetPrivateProfileIntW(L"dx", L"d3d_borderless", 0, ini);
    const wchar_t* want = !windowed        ? L"fullscreen"
                        : borderless >= 2  ? L"borderless_crisp"
                        : borderless       ? L"borderless" : L"windowed";
    wchar_t cur[64] = L"";
    GetPrivateProfileStringW(L"dx", L"display", L"", cur, _countof(cur), ini);
    { wchar_t* c = wcschr(cur, L';'); if (c) *c = 0; }
    if (_wcsicmp(cur, want) != 0) {
        if (WritePrivateProfileStringW(L"dx", L"display", want, ini)) {
            logf("[inimigrate] [dx] display: '%ls' -> '%ls'  (derived from d3d_windowed=%d "
                 "d3d_borderless=%d, the rows it replaces)", cur, want, windowed, borderless);
            changed++;
        }
    }
    wchar_t exc[256] = L"";
    GetPrivateProfileStringW(L"dx", L"d3d_windowed_except", L"", exc, _countof(exc), ini);
    { wchar_t* c = wcschr(exc, L';'); if (c) *c = 0; }
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(exc, L", ", &ctx); t; t = wcstok_s(NULL, L", ", &ctx)) {
        if (!t[0]) continue;
        wchar_t sec[96], had[64] = L"";
        _snwprintf_s(sec, _countof(sec), _TRUNCATE, L"dx.%s", t);
        GetPrivateProfileStringW(sec, L"display", L"", had, _countof(had), ini);
        if (had[0]) continue;                    // the user already chose under the new row
        if (WritePrivateProfileStringW(sec, L"display", L"fullscreen", ini)) {
            logf("[inimigrate] [%ls] display: -> 'fullscreen'  (this title was in the old "
                 "\"Titles left fullscreen\" list)", sec);
            changed++;
        }
    }
    return changed;
}

void inimigrate(const wchar_t* ini)
{
    const int have = GetPrivateProfileIntW(L"polshim", L"config_rev", 0, ini);
    if (have >= POLSHIM_CONFIG_REV) return;

    int changed = 0;
    for (int i = 0; i < _countof(g_migrations); i++) {
        if (g_migrations[i].rev <= have) continue;         // this install already has it
        changed += migrate_one(g_migrations[i], ini, NULL, 0, NULL);
    }
    if (have < 10) changed += migrate_display(ini);

    wchar_t rev[16];
    swprintf_s(rev, L"%d", POLSHIM_CONFIG_REV);
    // Stamped even when nothing changed -- the stamp records "this install has
    // been asked", not "this install was repaired". Without that, an install that
    // legitimately matched nothing would be re-walked at every launch for ever,
    // and a value set back to an old one by hand would be re-migrated behind the
    // user's back, which is the complaint this feature exists to stop causing.
    WritePrivateProfileStringW(L"polshim", L"config_rev", rev, ini);
    if (changed)
        logf("[inimigrate] repaired %d setting(s) left at an old default (config_rev %d -> %d). "
             "Each line above says what the old value broke.", changed, have, POLSHIM_CONFIG_REV);
    else
        logf("[inimigrate] nothing to repair (config_rev %d -> %d)", have, POLSHIM_CONFIG_REV);
}

int shim_reset_fixes(const wchar_t* ini, char* report, size_t cap)
{
    size_t used = 0;
    int changed = 0;
    if (report && cap) report[0] = 0;

    // 1. Every row under the fix heading, back to the table's default. The walk
    //    ends at the NEXT heading, so a row added to the group is covered without
    //    anything else being listed here -- the failure mode this whole file is
    //    about is a second list that has to be remembered.
    int n = 0; const ShimOption* opts = shim_options(&n);
    bool in_group = false;
    for (int i = 0; i < n; i++) {
        const ShimOption& o = opts[i];
        if (o.type == OPT_GROUP) {
            in_group = (o.label && wcscmp(o.label, SHIM_GROUP_FIXES) == 0);
            continue;
        }
        if (!in_group) continue;
        if (!o.key || !o.key[0]) continue;               // OPT_PACK owns no key of its own
        wchar_t cur[256];
        bool present = ini_present(o.sec, o.key, ini, cur, _countof(cur));
        if (present && wcscmp(cur, o.def) == 0) continue;
        if (!WritePrivateProfileStringW(o.sec, o.key, o.def, ini)) {
            logf("[reset-fixes] could NOT write [%ls] %ls (%lu)", o.sec, o.key, GetLastError());
            continue;
        }
        logf("[reset-fixes] [%ls] %ls: '%ls' -> '%ls'  (%ls)",
             o.sec, o.key, present ? cur : L"(absent)", o.def, o.label ? o.label : L"");
        changed++;
        if (report && used < cap) {
            int w = _snprintf_s(report + used, cap - used, _TRUNCATE,
                                "  [%ls] %ls: %ls -> %ls\n      %ls\n",
                                o.sec, o.key, present ? cur : L"(not set)", o.def,
                                o.label ? o.label : L"");
            if (w > 0) used += (size_t)w;
        }
    }

    // 2. And the retired keys, whatever config_rev says. A reset that leaves
    //    d3d_fitwindow=0 sitting in the file has not answered the question the
    //    button was pressed to answer.
    for (int i = 0; i < _countof(g_migrations); i++)
        if (!g_migrations[i].now)
            changed += migrate_one(g_migrations[i], ini, report, cap, &used);

    // 3. Cross-group LAUNCH fixes. These sit under other headings by their nature
    //    (windowing, login/updates) but are what let a title run AT ALL, so the button
    //    that "puts the game fixes back" has to restore them too. A short, explicit
    //    list -- the deliberate exception to the walk-the-group rule in step 1, for keys
    //    whose home group is not "Games" but whose failure stops a game. Keep it tiny and
    //    only for that: anything a user would reasonably want off does NOT belong here.
    static const struct { const wchar_t* sec; const wchar_t* key;
                          const wchar_t* def; const wchar_t* why; } k_launch[] = {
        { L"dx",      L"d3d_fs_rescue",   L"1",
          L"Front Mission Online: rescue a failed fullscreen device to windowed" },
    };
    for (int i = 0; i < (int)_countof(k_launch); i++) {
        wchar_t cur[64];
        bool present = ini_present(k_launch[i].sec, k_launch[i].key, ini, cur, _countof(cur));
        if (present && wcscmp(cur, k_launch[i].def) == 0) continue;
        if (!WritePrivateProfileStringW(k_launch[i].sec, k_launch[i].key,
                                        k_launch[i].def, ini)) continue;
        logf("[reset-fixes] [%ls] %ls: '%ls' -> '%ls'  (%ls)",
             k_launch[i].sec, k_launch[i].key, present ? cur : L"(absent)",
             k_launch[i].def, k_launch[i].why);
        changed++;
        if (report && used < cap) {
            int w = _snprintf_s(report + used, cap - used, _TRUNCATE,
                                "  [%ls] %ls: %ls -> %ls\n      %ls\n",
                                k_launch[i].sec, k_launch[i].key,
                                present ? cur : L"(not set)", k_launch[i].def, k_launch[i].why);
            if (w > 0) used += (size_t)w;
        }
    }

    logf("[reset-fixes] %d setting(s) put back to the tested configuration", changed);
    return changed;
}
