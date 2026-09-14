// profiles.h -- per-title compatibility profiles.
//
// The problem this solves: the shim grew ~58 [dx] knobs across six unrelated
// features, and a user can combine them into a client that does not work. Every
// display/input fault this project has hit is per-TITLE -- "windowed black-screens
// FFXI", "the FMV races windowed for FMO", "Tetra Master's cursor drifts" -- but the
// knobs are global, so the ini is a way to get exactly one title wrong.
//
// A profile is the KNOWN-GOOD configuration for one title, named by its module leaf.
// It is the AUTHORITY: when a title is active, its profile answers the per-title
// questions (leave the game fullscreen? give it a command-line switch? which cursor
// mode?) and the matching [dx] key is consulted only as an explicit per-install
// override. The table lives in CODE, so it reaches an install that never saw a
// shipped ini -- the iniheal.cpp "Rule 1" failure that a template-only fix always
// hits.
//
// Keyed by MODULE LEAF NAME because that is the one thing the shim can always resolve
// to a title: CreateDevice's / GetlpCmdLine's return address lands inside the calling
// title's DLL (d3d8hook.cpp caller_excepted(), the same fact d3d_dumpcaller relies
// on). No CLSID map, no region table.
#pragma once
#include <windows.h>

// How the shim should treat a title's window.
//
// WARNING: 2026-08-24: NO ROW IN THE TABLE IS PW_FULLSCREEN ANY MORE. The value is kept --
// the mechanism is correct and a future title may need it -- but the two rows that
// used it (FFXI, FMO) were deliberately dropped to PW_DEFAULT when windowed became
// the shipped default. Read the note on `fs_fallback` below and the two rows in
// profiles.cpp before adding a new one: an exclusive-fullscreen device is what
// takes the display hostage, and that is now a known cost, not a free default.
enum ProfileWindow {
    PW_DEFAULT = 0,   // no opinion -- the global [dx] d3d_windowed decides
                      // (which, since 2026-08-24, means WINDOWED)
    PW_FULLSCREEN,    // leave the title in the exclusive-fullscreen mode it asked for
                      // (== membership of d3d_windowed_except). Currently UNUSED.
    PW_SHIM_WINDOWED, // the shim's own windowed override is correct for this title
                      // (Tetra Master: fit_window + aspect lock).
    PW_NATIVE_WINDOWED// the TITLE has its own windowed mode; hand it the switch that
                      // turns it on and then LEAVE ITS DEVICE ALONE. Fantasy Earth
                      // (-windowmode).
};

struct TitleProfile {
    const char*   module;         // module leaf, case-insensitive (e.g. "FE_Client.dll")
    const char*   title;          // human name for the log
    ProfileWindow window;
    const char*   cmdline_append; // switches to add to the title's GetlpCmdLine, or NULL.
                                  // Only titles that read their command line (FE, FMO)
                                  // can use this; it is ignored for any that do not.
    const char*   note;           // one line, printed once when the profile is applied
    // The title's OWN action->button map: which 0-based DIJOYSTATE rgbButtons slot this
    // title reads for each shell action, in PadAction order (ok, cancel, menu, navi).
    // -1 = unmeasured, or the title has no equivalent action. padmap.cpp permutes a
    // title-created pad device against THIS map (the shell's map is wrong for a title:
    // each title carries its own config -- FFXI `padsin000`, TM `B_00OK`..`B_07MENU`).
    // A row of all -1 leaves that title's pad exactly as the OS reports it.
    int           pad_slot[4];

    // "This title has a RECORDED history of needing an exclusive fullscreen device."
    //
    // Windowed is the shipped default from 2026-08-24 (see the enum note above), so
    // nothing is left fullscreen by the table any more. But two titles were, on
    // measurements that are still in the record -- FFXI black-screened windowed on a
    // Steam Deck, FMO's windowless VMR-9 FMV was expected to race a windowed device --
    // and if either comes back the user needs a way to put THAT title back without
    // giving up windowed mode for everything else.
    //
    // The escape hatch is the ini list [dx] d3d_windowed_except, and this flag is what
    // fills its dropdown. It exists so the dialog and this table cannot drift: the
    // selftest in polsettings.cpp asserts the dropdown offers exactly the rows flagged
    // here. Two hand-maintained lists of the same fact is how a title gets added in one
    // place and stays silently missing from the other -- the same reason profiles_at()
    // exists at all.
    bool          fs_fallback;

    // "This title's DirectShow video window must be ADOPTED onto its game window."
    //
    // No POL title places its own video window: measured on live sessions, both FE's
    // and FMO's come up as DirectShow's untouched default -- class FilterGraphWindow,
    // top-level, un-owned, at (78,78) 320x240 (measured).
    // Left alone it plays OVER the Viewer's UI instead of in the game.
    //
    // KEY: THIS IS A PER-TITLE FACT WEARING A GLOBAL KNOB. The adoption is gated on
    // [dx] d3d_fitvideo, which ships 0 -- and it ships 0 because of a bug in ONE
    // title: build 37 made FMO's FMV UNSKIPPABLE (the placement re-imposed itself on
    // a 500 ms tick, so dismissing the movie just brought it back). That was fixed
    // in build 38 with "place once, then yield", but the mitigation default was never
    // lifted. So one title's since-fixed bug is still switching off another title's
    // needed fix -- exactly the failure profiles.h exists to end.
    //
    // A true row turns adoption on for THAT title regardless of the global knob. The
    // knob still forces it on for everything, and d3d_fitvideo=0 no longer means
    // "off for titles that need it".
    bool          adopt_fmv;

    // "[dx] fmv_skip APPLIES TO THIS TITLE ON WINDOWS TOO."
    //
    // fmv_skip is gated on Wine (fmvskip.cpp) because it is a Wine-quartz workaround
    // AND because level 4's no-movie path crashes Fantasy Earth on Windows. That gate
    // is right for FE and wrong for a title that has no movie switch of its own -- and
    // it was PROCESS-WIDE, so there was no way to say so per title. The result was a
    // shim setting that read "Skip the opening movie", was ticked, and did nothing:
    // reported live 2026-08-26 as "I have the FMO fmv set to skip and it does not".
    //
    // KEY: THIS IS THE LAST RESORT, NOT THE FIRST. A title with its OWN switch must use
    // its own switch -- FE's OPENING_MOVIE, the Viewer's PlayOpeningMovie, FFXI's
    // "Show opening movie", all of which are rows in the per-game settings window
    // (gamecfg.cpp). Set this true ONLY for a title that ships no such switch, and say
    // in the row's comment how that was established.
    //
    // It is the DEFAULT for [dx] fmv_skip_force, not an override of it: a per-title
    // user setting ([dx.<module>] fmv_skip_force) still wins, in either direction.
    //
    // KEY: AND IT CARRIES THE LEVEL, NOT JUST A YES. This was a bool for about an hour
    // and that was not enough, because the levels are not interchangeable per title:
    //   0 = leave this title gated on Wine (everything except FMO)
    //   2 = skip at RenderFile   -- a NO-OP for FE and FMO, which never call it
    //   4 = refuse the graph     -- the only level that works for FMO
    // The shipped GLOBAL [dx] fmv_skip is 2 on installs carrying the 2026-08-23 FE
    // crash mitigation, so "turn the gate on for FMO" delivered level 2 and FMO's
    // movie played exactly as before. Reported as "skipping fmo's opening movie just
    // doesn't work at all", and it did not.
    //
    // So this is the LEVEL used for this title on Windows when the user has not set
    // an explicit [dx.<module>] fmv_skip. A global fmv_skip=0 still means off
    // everywhere -- that is the user saying "play my movies", and it is obeyed.
    int           fmv_windows_level;

    // "THE LEVEL THIS TITLE NEEDS **UNDER WINE**, when the user has not named one."
    //
    // IMPORTANT: WHY THIS EXISTS AT ALL (2026-08-27). fmv_effective_level did this:
    //
    //     if (fmv_under_wine())
    //         return want;          // <-- the per-title gate below never ran
    //     // ON WINDOWS THE GATE IS PER TITLE, NOT PROCESS-WIDE.
    //     //   Fantasy Earth MUST stay gated (level 4 crashes it, RVA 0x1DBD24)
    //
    // The FE exclusion was real, deliberate and documented -- and unreachable off
    // Windows. So the Steam Deck handed FE the one level our own source says kills
    // it: level 4 refuses the graph, FE's no-movie path never builds the object its
    // forwarding thunk needs, and it takes a c0000005 reading [this+4]==NULL at
    // FE_Client+0x1DBD24 right after the two logos. FE then CATCHES that, exits via
    // pol.exe's SUCCESS path (GameStart returned 0 after ~15 s), and leaves its
    // window procedure behind on the Viewer's PlayOnlineUS window -- so the next
    // WM_MOUSEMOVE dispatches into an unloaded module and kills pol.exe. What that
    // looked like from outside was "Fantasy Earth crashes on the Deck", for months.
    //
    // Same binary, same ini, opposite branch: FE worked on Windows the whole time.
    // A per-title fact needs a per-title home on BOTH platforms, which is the entire
    // point of this file -- see the fmv_windows_level note above making the same
    // mistake one platform over.
    //
    //   0 = no opinion; the global [dx] fmv_skip decides (every title but FE)
    //   5 = build the graph, never start it (FE -- see the level table in polshim.ini)
    //
    // An explicit [dx.<module>] fmv_skip is still the user's word and wins, and a
    // global fmv_skip=0 still means "play my movies" everywhere.
    int           fmv_wine_level;
};

// Look up the profile for a module leaf name (the string after the last '\'), or
// NULL if the title has no profile. Case-insensitive whole-name match.
const TitleProfile* profile_for_module(const char* leaf);

// Resolve the profile for the module that OWNS a code address (a return address
// from a hooked call). NULL if the address is not in a profiled title. Safe on any
// pointer -- it VirtualQuerys first.
const TitleProfile* profile_for_addr(void* addr);

// True if ANY profile in the table requests a command-line append. Lets a hook skip
// arming its per-call machinery entirely when no title wants it. Cheap; computed once.
bool profiles_any_cmdline();

// Enumerate the table. Exists so a test can assert that a UI which OFFERS a
// per-title choice offers exactly the titles the table actually profiles --
// two hand-maintained lists of the same fact is how a title gets added in one
// place and stays silently missing from the other.
int profiles_count(void);
const TitleProfile* profiles_at(int i);

// Log the whole table once at startup, so a session log records which titles the
// build knows about and how it will treat each. Read from inject.cpp after log_open.
void profiles_log_table(void);
