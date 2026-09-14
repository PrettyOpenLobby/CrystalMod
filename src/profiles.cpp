// profiles.cpp -- the per-title compatibility profile table. See profiles.h for why.
//
// This is the one place that says, for each title, what a modern PC needs it to do.
// The one-line notes on each row summarise what was measured behind it.
#include "polshim.h"
#include "profiles.h"

// ---------------------------------------------------------------------------
// THE TABLE. One row per title we have measured a working configuration for.
// ---------------------------------------------------------------------------
//
// Order is documentation, not logic: the two titles that want to be LEFT ALONE
// first, then the one the shim actively drives.
//
// IMPORTANT: 2026-08-24: WINDOWED IS NOW THE DEFAULT FOR EVERY TITLE. FFXI and FMO used to be
// PW_FULLSCREEN and that verdict OUTRANKED the global switch, so "Run games in a
// window" silently did nothing for them. Both are PW_DEFAULT now -- see the long notes
// on each row for what that trades and how to put one back. The mechanism is unchanged:
// the profile is still the authority, it just no longer has a fullscreen opinion.
static const TitleProfile g_profiles[] = {

    // Fantasy Earth -- SE shipped a windowed mode and we never used it. FE_Client.dll
    // parses `-windowmode` off the command line it gets from IPOLCoreCom::GetlpCmdLine
    // and makes its own framed, centred, 800x600 window. Hand it the switch and
    // stand off its device -- do NOT run fit_window or force Windowed=TRUE over a title
    // that is already windowed correctly, or the two fight.
    // Pad map DELIBERATELY EMPTY. Measured 2026-08-19: DIDATAFORMAT at rva 31456C is a
    // plain DIJOYSTATE (buttons at 48), so it IS permutable -- but FE carries its own
    // button-name table `JBTN_1`..`JBTN_16` (rva 35C1A0..35C218, sitting directly beside
    // its DirectInput8Create/DINPUT8 strings) and its own key-config that binds actions
    // to those names. Sixteen named buttons and FE's own binding UI, not our four
    // actions. Remap FE in FE.
    { "FE_Client.dll",  "Fantasy Earth", PW_NATIVE_WINDOWED, "-windowmode",
      "SE's own windowed mode via -windowmode; shim leaves the device alone",
      { -1, -1, -1, -1 }, false, /*adopt_fmv*/ true,  /*fmv_windows_level*/ 0,
      /*fmv_wine_level*/ 5 },   // IMPORTANT: NOT 4: level 4 crashes FE at RVA 0x1DBD24 (see profiles.h)
    // NO SEPARATE ROW FOR AN ENGLISH BUILD, and the reason is not the obvious one.
    //
    // The FE deploy script DOES build an "FE_Client.en.dll" -- and installs it AS
    // <tree>/FE_Client.dll, over the original (kept as FE_Client.dll.orig). The
    // ".en" name is a BUILD ARTIFACT that never reaches a running process, so a
    // profile keyed on it could never match, however the game was translated.
    //
    // It cost nothing at runtime and everything in the settings dialog, where it
    // became a second "Fantasy Earth" to choose between. Removed 2026-08-25.
    //
    // NOTE fepatch.cpp still lists both names in FE_MODULES[]. Harmless -- the
    // extra entry simply never matches -- but it is the SECOND hand-maintained
    // list of FE module names, so anyone adding a real variant must touch both.

    // Front Mission Online -- asks for exclusive fullscreen and plays its FMV through a
    // VMR-9 in WINDOWLESS mode, which composites into the game's HWND with no window of
    // its own. Forced windowed, the game's Present and the VMR's composite race and the
    // movie flickers unusably. No -window switch
    // exists. Left fullscreen until the renderless allocator lands.
    //
    // WARNING: THE FLICKER DID NOT REPRODUCE ON THE FIRST WINDOWED RUN THIS VERDICT HAS EVER
    // HAD (2026-08-21, a Windows desktop, via [dx] d3d_windowed_force). The account
    // holder saw a BLACK/WHITE opener and pressed Enter into the game -- which is what
    // the Deck's own FULLSCREEN session called normal for this windowless VMR-9 ("white
    // /static opener ... then real gameplay"). So on that machine
    // the movie looked the same windowed as it is documented to look fullscreen, and the
    // one thing this verdict exists to prevent was not observed.
    //
    // That is ONE machine and ONE run, and it is not a retraction: a compositing race is
    // exactly the kind of fault that is hardware- and timing-dependent, so "did not
    // flicker here" cannot generalise. It is recorded because the verdict's cost is now
    // measured on one platform instead of assumed on all of them, and because the next
    // person to ask "why is FMO fullscreen-only?" deserves to see the counter-evidence
    // rather than rediscover it.
    //
    // IMPORTANT: CHANGED TO PW_DEFAULT 2026-08-24, DELIBERATELY, AND THIS IS NOT A MEASUREMENT.
    //
    // The account holder's report is that exclusive fullscreen "fights for screen
    // dominance" so hard the game cannot be left, and asked for windowed by default
    // "by force if we have to". So this row is no longer the authority; the global
    // [dx] d3d_windowed (now 1) decides, and FMO runs windowed.
    //
    // Be honest about what that trades. The FMV race is NOT disproved -- it is one
    // clean windowed run on one Windows desktop against a documented mechanism. What
    // changed is which cost we are choosing to pay: a movie that may flicker on some
    // machines, instead of a device that takes the display hostage on all of them.
    // fmv_skip=4 has been the shipped default since config rev 6 anyway, so on a stock
    // install FMO's opening graph is refused before it can race anything.
    //
    // fs_fallback=true: FMO is offered in the "Titles left fullscreen" dropdown. If the
    // flicker turns up, that is the one-click way back -- for FMO alone, without giving
    // up windowed mode everywhere else. If it turns up on a machine somebody is sitting
    // at, record it HERE.
    { "FrontMissionOnline.dll", "Front Mission Online", PW_DEFAULT, NULL,
      "windowed by default; its windowless VMR-9 FMV may race (d3d_windowed_except to revert)",
      // DELIBERATELY EMPTY. Measured 2026-08-19: DIDATAFORMAT at rva 36DEDC is a plain
      // DIJOYSTATE (buttons at 48), so it IS permutable -- but FMO's pad config is a
      // TWENTY-FOUR entry assignment array, read from `GamePadAssin0` under
      // HKLM\SOFTWARE\PlayOnline\SQUARE ENIX\FrontMissionOnline (reader 0x61080160:
      // RegQueryValueEx, strtok on 0x6132FAB8, atoi per token, 0x18 entries, 0xFF =
      // unassigned), applied via 0x61212B20. It is written by FMO's own "Gamepad Config
      // Settings" UI. Twenty-four actions do not reduce to our four, so mapping them
      // here would invent a layout the game does not have. Remap FMO in FMO.
      //
      // IMPORTANT: fmv_windows=true, ADDED 2026-08-26 ON A LIVE REPORT: "I have the FMO fmv set
      // to skip in our shim settings. It does not - instead it flickers all over the
      // gameplay." Both halves of that are explained here, and the second one is the
      // flicker this very comment block asked to have recorded:
      //
      //   * the shim's [dx] fmv_skip was process-wide Wine-only, so on Windows it was
      //     read, logged as IGNORED, and the movie played. The claim a few lines above
      //     -- "fmv_skip=4 has been the shipped default since config rev 6 anyway, so on
      //     a stock install FMO's opening graph is refused before it can race anything"
      //     -- IS FALSE ON WINDOWS, for exactly that reason. It held only under Proton.
      //   * with the graph built and adopt_fmv=false, FMO's VMR-9 video window is left
      //     as DirectShow's untouched default: top-level, un-owned, 320x240 at (78,78),
      //     over whatever is on screen. That is the flicker.
      //
      // FMO IS THE RIGHT TITLE FOR THE LAST RESORT because it ships no movie switch of
      // its own -- established three ways 2026-08-26: FrontMissionOnlineConfig.exe's
      // string table (dumped to JSON) offers only resolution, refresh rate,
      // Media Center support and the gamepad; its registry key holds only displayW/H/R,
      // resetflg and the four VoiceChat values; and FrontMissionOnline.dll contains no
      // movie/opening/container string at all. There is no SE setting to prefer here,
      // so the shim's own is legitimate.
      //
      // KEY: LEVEL 3, FROM THE DECOMPILE (2026-08-26, FrontMissionOnline.dll). The movie
      // object (ctor 0x6122AE00, made unconditionally at init by 0x6122B070 from
      // 0x6100A0C7 "Debug Init...") AddFilters a VMR-9, sets it WINDOWLESS, and then
      // calls IGraphBuilder::RenderFile(<install>\fmo.dat) -- slot 0x34. The movie
      // file is fmo.dat, a 270 MB MPEG-1 behind a .dat extension, which is why the
      // shim's video-extension filter never saw a RenderFile from FMO and the claim
      // "FMO never calls RenderFile" got written down three times. On a FAILED
      // RenderFile FMO jumps to 0x6122AF0F: release the graph (0x61229A10, which
      // also clears the movie flag 0x613CEF7C), then 0x6102D000(0x80010101,
      // 0x10101, 0) -- the SAME call its tick (0x61229B40) makes on EC_COMPLETE.
      // So a failed RenderFile is FMO's own "movie finished" path, with nothing
      // dangling. That is what level 3 delivers, and it is the smallest deviation
      // from SE's code: the graph is built exactly as SE built it, one file load
      // says no. Level 4 (refuse the graph, verified live on the Deck) works too and
      // takes the ctor's other bail-out (0x6122AE59: release, deleting-dtor, same
      // 0x6102D000 request); pick it with [dx.FrontMissionOnline.dll] fmv_skip=4.
      //
      // IMPORTANT: BUILDS 145/146 SHIPPED LEVEL 4 HERE AND IT NEVER RAN ON WINDOWS. Third fault
      // in the chain (after the Wine gate and the bool-not-level): fmvskip_wrap read
      // the process-wide g_level, which is 0 on Windows until a CreateDevice -- after
      // the movie. The per-caller level was computed, logged, and dropped on the
      // wrapper's first line. fmvskip_wrap takes the level as an argument now.
      // OPEN: NOT YET RUN ON WINDOWS. If FMO fails to start, the revert is one line:
      // [dx.FrontMissionOnline.dll] fmv_skip_force=0. Expected log lines, in order:
      // `[fmv] wrapped filter graph ... at level 3` then
      // `[fmv] SKIP RenderFile("...\fmo.dat") from FrontMissionOnline.dll...`.
      { -1, -1, -1, -1 }, true, /*adopt_fmv*/ false, /*fmv_windows_level*/ 3,
      // IMPORTANT: WAS 0 ("the global fmv_skip decides") UNTIL 2026-09-08, AND THE GLOBAL IS
      // FE's LEVEL. Measured on the Deck, build 168, FMO's launch:
      //   [fmv] skip level 5 -- BUILD the graph but never Run it (FE's level ...)
      //   [fmv] SKIP RenderFile("Z:\...mo.dat") ... returning 0x00000000
      //   [crash1st] ACCESS_VIOLATION at quartz.dll+0x1E32F  READ from 0000004C
      //   [crash]   frame 0  ret FrontMissionOnline.dll+0x22AFF0
      // Level 5 is only coherent for a title that never calls RenderFile -- that is
      // what makes it FE's. FMO DOES call it, so at 5 it was told S_OK for a graph
      // nobody built, used it, and Wine's quartz dereferenced a filter that did not
      // exist. 3 is FMO's own clean no-movie path and is already its Windows level;
      // there was never a reason for the two platforms to differ here.
      /*fmv_wine_level*/ 3 },

    // FFXI -- windowed black-screened it: it drew its title screen, went black and had
    // to be force-quit (measured on a Steam Deck, 2026-08-15).
    //
    // IMPORTANT: CHANGED TO PW_DEFAULT 2026-08-24, DELIBERATELY, AND THIS IS THE RISKY ONE.
    //
    // Requested explicitly ("everything windowed, no exceptions") against the symptom
    // that an exclusive device cannot be escaped. The black screen is a REAL, MEASURED
    // failure and dropping this row does not fix it -- it accepts it, on the bet that
    // one Deck measurement from 2026-08-15 does not describe every machine, and with a
    // one-click way back if it does.
    //
    // WHAT IS STILL GUARDING FFXI (neither of these was touched):
    //   * the add-on-core stand-down in caller_except_why() -- with Ashita or Windower
    //     in the process FFXI's device is left entirely alone regardless of this row.
    //     That is a correctness guard (two device wrappers on one device = a lost
    //     device), not a preference, and it is tested FIRST.
    //   * d3d_fs_rescue, which retries a failed fullscreen device windowed.
    //
    // IF THE BLACK SCREEN COMES BACK: [dx] d3d_windowed_except=FFXiMain.dll, or pick
    // "Final Fantasy XI" in the settings dialog's "Titles left fullscreen" -- that is
    // what fs_fallback=true below puts in the dropdown. Then put PW_FULLSCREEN
    // back here; do not leave the ini as the only record.
    //
    // WARNING: THE BETTER FIX, UNBUILT: FFXI has its OWN windowed mode, written by its own
    // config app into the numbered values under
    // HKLM\SOFTWARE\PlayOnlineUS\SquareEnix\FinalFantasyXI (0000..0043, gamecfg.cpp).
    // Setting the title's native mode is [[match-the-original]] and would not go
    // anywhere near the forced-windowed path that black-screened. It is not done here
    // because WHICH numbered value that is has never been measured, and gamecfg.cpp's
    // own header records that guessing at this exact registry layout already cost two
    // rounds. Measure it (run FFXI's config app between two gamecfg diff views and the
    // value names itself), then make this row PW_NATIVE_WINDOWED.
    { "FFXiMain.dll", "Final Fantasy XI", PW_DEFAULT, NULL,
      "windowed by default; black-screened windowed on a Deck 2026-08-15 (d3d_windowed_except to revert)",
      // DELIBERATELY EMPTY, and NOT a gap to be filled in. Measured 2026-08-19 off
      // FFXiMain.dll.mem_10010000.bin:
      //   * its DIDATAFORMAT (rva 34A0FC) is DIJOYSTATE2 with buttons at offset 48, so
      //     the transport IS permutable -- that part of the old warning is resolved;
      //   * but its pad config is a 256-byte BLOB in `padsin000` (reader at 0x100128EC,
      //     registry helper 0x10028FD0), written by FFXI's OWN in-game config, with
      //     `padexsin000` / `padmode000` / `padguid000` beside it. There is no
      //     four-action {ok,cancel,menu,navi} table here to map onto;
      //   * and `padmode000` feeds a gate at 0x10465C38 that selects FFXI's XInput
      //     path -- SE SHIPS `xinputdll.dll` (a 6.6 KB forwarder to XINPUT1_3.dll,
      //     exports XInputGetState_ etc., resolved at 0x10018E37). On an XInput pad,
      //     which is what a Steam Deck presents, FFXI can read its buttons through
      //     XInput and never touch the DirectInput device we permute at all.
      // So a pad_slot row here would be a mapping FFXI does not have, applied through a
      // path it may not be using. Remap FFXI in FFXI.
      { -1, -1, -1, -1 }, true, /*adopt_fmv*/ false, /*fmv_windows_level*/ 0,
      /*fmv_wine_level*/ 0 },

    // Tetra Master -- the shim's windowed override plus the direct cursor write is the
    // proven config (dinput_mouseabs=2 + dinput_cursor_xy + freecursor; shipped
    // build 29). This is the one title the shim actively drives windowed.
    //
    // Pad map MEASURED from the TM.dll memory dump:
    // the config reader at TM.dll+0x187100 (dump 0x051171xx) resolves the eight
    // B_00OK..B_07MENU HKLM values over compiled defaults at +0x2365FC, and the consumer
    // at +0x180603 tests `rgbButtons[value-1]` against the DIJOYSTATE copy -- so the
    // stored values are 1-BASED button numbers. Defaults (= this desktop's registry too):
    // OK=2 CANCEL=3 MENU=1 FRIENDLIST=4 PAGEDOWN=5 PAGEUP=6 CHAT=9 PLAYONLINE=10.
    // 0-based: ok=1 cancel=2 menu=0 -- PS2 face order, same as the shell. TM has no
    // Navigate; its slot 3 is FRIENDLIST, the analogous fourth utility action, so the
    // navi binding drives it. That also keeps the layout the account holder has been
    // playing with: before per-title maps the shell permutation leaked onto TM's device
    // and, because TM's slots equal the shell's, happened to produce exactly this.
    { "TM.dll", "Tetra Master", PW_SHIM_WINDOWED, NULL,
      "shim-windowed + dinput_mouseabs=2 direct cursor write",
      { 1, 2, 0, 3 }, false, /*adopt_fmv*/ false, /*fmv_windows_level*/ 0,
      /*fmv_wine_level*/ 0 },
};

static const int g_nprofiles = (int)(sizeof(g_profiles) / sizeof(g_profiles[0]));

const TitleProfile* profile_for_module(const char* leaf)
{
    if (!leaf || !*leaf) return NULL;
    for (int i = 0; i < g_nprofiles; i++)
        if (_stricmp(leaf, g_profiles[i].module) == 0)
            return &g_profiles[i];
    return NULL;
}

const TitleProfile* profile_for_addr(void* addr)
{
    if (!addr) return NULL;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return NULL;
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) return NULL;
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;
    return profile_for_module(leaf);
}

int profiles_count(void) { return g_nprofiles; }

const TitleProfile* profiles_at(int i)
{
    return (i >= 0 && i < g_nprofiles) ? &g_profiles[i] : NULL;
}

bool profiles_any_cmdline()
{
    for (int i = 0; i < g_nprofiles; i++)
        if (g_profiles[i].cmdline_append && g_profiles[i].cmdline_append[0])
            return true;
    return false;
}

static const char* window_name(ProfileWindow w)
{
    switch (w) {
        case PW_FULLSCREEN:      return "fullscreen (leave alone)";
        case PW_SHIM_WINDOWED:   return "shim-windowed";
        case PW_NATIVE_WINDOWED: return "native-windowed";
        default:                 return "default (global d3d_windowed)";
    }
}

void profiles_log_table(void)
{
    logf("[prof] per-title compatibility profiles (%d):", g_nprofiles);
    for (int i = 0; i < g_nprofiles; i++) {
        const TitleProfile* p = &g_profiles[i];
        char pad[64] = "(unmeasured)";
        if (p->pad_slot[0] >= 0 || p->pad_slot[1] >= 0 ||
            p->pad_slot[2] >= 0 || p->pad_slot[3] >= 0)
            _snprintf_s(pad, sizeof(pad), _TRUNCATE, "ok=%d cancel=%d menu=%d navi=%d",
                        p->pad_slot[0], p->pad_slot[1], p->pad_slot[2], p->pad_slot[3]);
        logf("[prof]   %-24s %-16s window=%-26s cmdline=%-12s pad=%s%s%s",
             p->module, p->title, window_name(p->window),
             (p->cmdline_append && p->cmdline_append[0]) ? p->cmdline_append : "(none)",
             pad,
             p->adopt_fmv  ? "  [FMV window adopted, regardless of d3d_fitvideo]" : "",
             p->fmv_windows_level ? "  [fmv_skip applies on Windows too]" : "",
             p->fs_fallback ? "  [can be put back fullscreen: d3d_windowed_except]" : "");
    }
    // Say the global verdict on the same lines, so a log that shows every title as
    // "default" also says what default MEANS in this build. Reading "window=default"
    // and having to go and find d3d_windowed in a different section of the log is
    // exactly how the old "the setting does nothing" reports started.
    logf("[prof]   window=default resolves to the global [dx] d3d_windowed, which "
         "ships as 1 (WINDOWED) since 2026-08-24. No row is PW_FULLSCREEN any more.");
}

// ---------------------------------------------------------------------------
// THE TITLE IN SCOPE -- see the
// ini_int_title/ini_str_title helpers in polshim.h.
// ---------------------------------------------------------------------------
//
// Lives here because this file is where "which title is this?" is already
// answered. It is deliberately AMBIENT rather than a parameter: a module adopts
// a per-title key by changing GetPrivateProfileIntW -> ini_int_title at the call
// site and nothing else, instead of threading a leaf through 25 `*_reload(ini)`
// signatures.
//
// THREAD-LOCAL, and the distinction matters. The scope guards only the window in
// which settings are READ -- shim_reload_for_title sets it, calls the module
// reloads on that thread, and clears it. What those reloads write into module
// globals is process-wide on purpose: while a title runs, its overrides ARE the
// live settings, for every thread that reads them.
//
// A copy, not a borrowed pointer: the caller's buffer is usually a stack MAX_PATH
// from GetModuleFileNameA, and the scope outlives that frame.
static __declspec(thread) char t_scope[80] = "";

void title_scope_set(const char* leaf)
{
    if (!leaf || !*leaf) { t_scope[0] = 0; return; }
    strncpy_s(t_scope, sizeof(t_scope), leaf, _TRUNCATE);
}

const char* title_scope(void) { return t_scope; }

// ---------------------------------------------------------------------------
// WHICH TITLE IS RUNNING RIGHT NOW -- process-wide, unlike the thread-local
// reading scope above.
// ---------------------------------------------------------------------------
//
// Set by d3d8hook's title boundary, read by the settings dialog so a per-title row
// can say which of the four layers answered FOR THE TITLE IN FRONT OF YOU. Lives
// here rather than in d3d8hook because "which title" is not a D3D question -- and
// the settings selftest proved it, by failing to link when it was.
static char g_current[80] = "";

void title_current_set(const char* leaf)
{
    if (!leaf || !*leaf) { g_current[0] = 0; return; }
    strncpy_s(g_current, sizeof(g_current), leaf, _TRUNCATE);
}

const char* title_current(void) { return g_current; }


// The four layers, in the user's words rather than the ini's. Deliberately says
// WHO chose it as well as WHAT scope it applies to, because those are the two
// things a person needs to know to change it -- "the shim default" tells you to go
// looking for a setting, "your setting, this game" tells you it is already yours.
const wchar_t* value_source_text(ValueSource s)
{
    switch (s) {
        case VS_TITLE_INI:  return L"your setting, this game";
        case VS_GLOBAL_INI: return L"your setting, every game";
        case VS_PROFILE:    return L"this game's compat profile";
        default:            return L"the shim default";
    }
}
