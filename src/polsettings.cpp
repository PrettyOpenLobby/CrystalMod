// polsettings.cpp -- the in-game shim settings dialog.
//
// WHY THIS EXISTS
//
// Every shim option lives in polshim.ini, and on a Steam Deck editing that file means
// leaving Game Mode for the desktop, finding the Proton prefix, and typing into a text
// editor with a touchscreen keyboard. That is most of the friction in a Deck session,
// and it is why options like [regfix] enable sat at their default on a machine whose
// owner wanted them on. This puts the same options behind a chord.
//
// WHAT IT DOES
//
// A watcher thread polls for two chords and opens a dialog built at RUNTIME from
// shim_options() -- the same table iniheal writes -- so adding a row there gives you
// both the ini key and the dialog control with no work here.
//
//   keyboard   [settings] hotkey=ctrl+shift+s     (GetAsyncKeyState, no global hook)
//   gamepad    [settings] pad_chord=back+start    (XInput, resolved dynamically)
//
// The dialog runs on the watcher thread with its own message loop and window class, so
// it never blocks or re-enters the Viewer's UI thread. Saving writes only the values
// that actually changed, via WritePrivateProfileString -- the same file iniheal and
// install.sh's ini_heal maintain, so all three agree by construction.
//
// WHY GetAsyncKeyState AND NOT A KEYBOARD HOOK: a WH_KEYBOARD_LL hook is process-wide
// and its callback runs on whatever thread pumped the message, so a slow frame in the
// Viewer stalls every keystroke on the system. Polling at 16 Hz costs nothing, cannot
// wedge the input stack, and needs no extra privileges under Proton.
//
// IMPORTANT: KNOWN LIMIT -- READ BEFORE RELYING ON IT: the Viewer's shell is DirectDraw and may
// hold the display EXCLUSIVELY. Over an exclusive-fullscreen surface a normal top-level
// window can be invisible even though it exists and has focus, which reads exactly like
// "the chord did nothing". This is the same caveat that governs polctl's screenshots.
// If the chord appears dead, check the log for the `[settings] opened`
// line first -- if that line is there, the dialog IS open and this is a compositing
// problem, not an input one. Now MEASURED against the shell, not merely suspected.
//
// IMPORTANT: AND IT IS WORSE THAN "INVISIBLE" -- MEASURED ON A STEAM DECK, 2026-08-15: pressing
// the chord inside FFXI CRASHED THE TITLE. FFXI holds an exclusive fullscreen Direct3D
// device; creating and activating a top-level window takes the display from it, the
// device is lost, and the game dies. So the chord is now a NO-OP while such a device is
// live (see exclusive_display_blocks_us below) and says so in the log. Note that
// [dx] d3d_windowed=1 is NOT the workaround the old note above implied: for FFXI
// specifically it black-screens the title (measured 2026-08-15). Use the chord
// in the Viewer shell, which is windowed. [settings] fullscreen_ok=1 opts back in.
//
// RESOLVED: 2026-08-24: d3d_windowed NOW DEFAULTS TO 1, so on a stock install there should be
// no exclusive device in the process at all and the chord should work everywhere,
// including inside FFXI. exclusive_display_blocks_us is NOT removed -- it is the guard
// for a title someone has deliberately put back with [dx] d3d_windowed_except, and for
// the add-on-core case where FFXI's device is left alone regardless. What changed is
// that it should now be the rare path rather than the FFXI path.
//
// WARNING: The FFXI caveat above is still live and still unretracted: nobody has watched FFXI
// run windowed since the Deck measurement that black-screened it. If it does, the fix
// is d3d_windowed_except=FFXiMain.dll (the "Titles left fullscreen" dropdown), which
// also brings this guard back into play for that title.

#include "polshim.h"
#include <commdlg.h>     // GetOpenFileNameW -- the character-import file picker
#include <shellapi.h>    // ShellExecuteW -- open the polexport dump folder
#include "hotkeydef.h"   // POLSHIM_DEFAULT_HOTKEY -- one definition, two readers
#include "profiles.h"    // the selftest pins the d3d_windowed_force dropdown to it

#define IDC_FIRST   2000
// Separate ranges so WM_CTLCOLORSTATIC can tell a hint line and a heading apart
// from an ordinary label without keeping a list of HWNDs. One slot per table row,
// same index as g_ctl, and both are below the mapper's 3100+ ids.
// IMPORTANT: EVERY RANGE MUST BE AT LEAST POLSET_MAX_ROWS WIDE. They were 200 apart when the
// table held 151 rows; adding each game's own settings took it past 200 and rows
// 200+ would have carried hint ids -- a control answering to two owners, which is the
// silent-haunting class of bug, not a crash. C_ASSERTed below.
#define IDC_HINT_FIRST 2400
#define IDC_HDR_FIRST  2800
// The label STATIC beside a non-checkbox row. It used to be created with no id at
// all -- nothing ever needed to find it again. The filter does: hiding a row means
// hiding its label too, and a control with no id cannot be looked up. 2600 is past
// IDC_HDR_FIRST's 200-wide window (WM_CTLCOLORSTATIC tests `< IDC_HDR_FIRST + 200`),
// so a label is not mistaken for a heading and painted in the highlight colour.
#define IDC_LBL_FIRST  3200
#define IDC_SAVE    1001
#define IDC_CANCEL  1002
#define IDC_REPAIR  1003
#define IDC_ABDIAG  1004
#define IDC_SRVSAVE 1005
#define IDC_SRVDEL  1006
#define IDC_GAMECFG 1007
#define IDC_FILTER  1008
#define IDC_CATS    1009      // the category sidebar (a listbox)
#define IDC_RESETFIX 1010     // "Put the game fixes back"
#define IDC_STATUS  1012      // the save-status line along the bottom
// 1012 was IDC_IMPORTCHAR, the bottom-strip "Import FFXI character..." button. The
// import and export actions are OPT_BUTTON rows in the FFXI group now and take their
// ids from the option range; the id is left unused rather than recycled, so an old
// build's WM_COMMAND cannot land on a new action.

static wchar_t  g_ini[MAX_PATH];
static int      g_enabled  = 0;
// The hotkey is a LIST, not one chord: `hotkey=home,ctrl+shift+s` arms both. One
// binding cannot suit every keyboard -- `home` is a single key anyone can find but is
// missing from compact boards and the Deck, while `ctrl+shift+s` is universal and
// forgettable -- so the default ships both and the user narrows it in the dialog.
#define HK_MAX 6
static UINT     g_hk_mods[HK_MAX];   // bitmask: 1 ctrl, 2 shift, 4 alt
static UINT     g_hk_vk[HK_MAX];
static int      g_hk_count = 0;
// The SECOND chord: the on-surface controller mapper (padoverlay.cpp). Kept separate
// from the dialog's rather than added to its list because the two do different things
// and the mapper's must remain reachable on a machine where the dialog cannot be seen
// at all -- which is the whole reason the mapper exists.
static UINT     g_ui_hk_mods[HK_MAX];
static UINT     g_ui_hk_vk[HK_MAX];
static int      g_ui_hk_count = 0;
// The DISPLAY switch (windowed <-> borderless for the running game), Alt+Enter.
static UINT     g_dp_hk_mods[HK_MAX];
static UINT     g_dp_hk_vk[HK_MAX];
static int      g_dp_hk_count = 0;
static WORD     g_padui_mask = 0;
// The REPORT key (polreport.cpp). Parsed here because this file owns parse_hotkey,
// parse_pad and the watcher that polls them; a second parser is a second set of
// rules about what "back+rb" means.
static UINT     g_rp_hk_mods[HK_MAX];
static UINT     g_rp_hk_vk[HK_MAX];
static int      g_rp_hk_count = 0;
static WORD     g_rp_pad_mask = 0;
static WORD     g_pad_mask = 0;
static int      g_pad_debug = 0;
static int      g_fullscreen_ok = 0; // open even over an exclusive fullscreen title
static HANDLE   g_thread   = NULL;
static LONG     g_open     = 0;      // interlocked: dialog currently up
// ONE SLOT PER TABLE ROW, and it must not be smaller than the table: build_and_pump
// clamps `n` to this, so an option past the end is silently not drawn AND not saved.
// The 2026-08-17 pass took the table from ~50 rows to 81 and 64 stopped being enough,
// which is invisible unless you go looking -- so the self-test now asserts the fit.
// Raised from 192 when each game's OWN settings became rows in its section. The
// selftest asserts the table fits, so this is checked rather than hoped.
#define POLSET_MAX_ROWS 288
static HWND     g_ctl[POLSET_MAX_ROWS];
// "Show developer options", LIVE. Read from the ini when the dialog opens and
// updated the moment the box is ticked -- it used to be read once per open, so
// ticking it did nothing until the window was closed and reopened, which reads
// exactly like a broken setting. The controls for dev rows are now created
// ALWAYS (see the creation loop) and this only decides whether the layout gives
// them a position, so the toggle is a re-layout of controls that already exist,
// the same operation the search box performs on every keystroke.
static bool     g_show_dev = false;
// Defined with the dialog builder; the live developer toggle (in the wndproc, above
// it) has to rebuild the sidebar too.
static void cats_fill(HWND cats);

// Checked at COMPILE TIME, because an id collision does not crash -- it makes one
// control answer to another's messages, which reads as a haunted dialog.
C_ASSERT(IDC_FIRST      + POLSET_MAX_ROWS <= IDC_HINT_FIRST);
C_ASSERT(IDC_HINT_FIRST + POLSET_MAX_ROWS <= IDC_HDR_FIRST);
C_ASSERT(IDC_HDR_FIRST  + POLSET_MAX_ROWS <= IDC_LBL_FIRST);
// The live filter. Empty means "everything", which is the state the dialog opens in
// -- the filter is an accelerator for a table that has outgrown one glance, not a
// mode the user has to dismiss before they can see their settings.
static wchar_t  g_filter[64] = L"";
// THE SELECTED CATEGORY: the table index of an OPT_GROUP row, or CAT_ALL.
//
// The dialog opens on the FIX group rather than on everything, because "everything"
// is the state that made this window a behemoth and because the fixes are what
// somebody with a broken game is looking for. Everything is one click away, and
// typing in the search box switches there by itself -- a search that only looked
// inside the open category would be a search that lies.
#define CAT_ALL (-1)

// A PER-GAME SECTION IS JUST ANOTHER CATEGORY.
//
// The sidebar already selects by item data, so "settings for Tetra Master" needs no
// new view, no picker and no second layout path -- it is a category whose value
// encodes a profile index instead of a table index. Categories from the option table
// are >= 0; CAT_ALL is -1; a per-game section is CAT_GAME_BASE - profileIndex.
//
// The list is enumerated from profiles_at(), never hand-written, so the sidebar and
// profiles.cpp cannot drift into disagreeing about which titles exist. That is the
// same reason profiles_at() exists -- see the fs_fallback selftest.
#define CAT_GAME_BASE (-100)
#define CAT_IS_GAME(c)   ((c) <= CAT_GAME_BASE)
#define CAT_GAME_INDEX(c) (CAT_GAME_BASE - (c))

// The module leaf of the game section currently selected, or "" in a normal
// category. Reads resolve against it and Save writes into [<sec>.<leaf>].
static const char* cat_game_leaf(int cat)
{
    if (!CAT_IS_GAME(cat)) return "";
    const TitleProfile* p = profiles_at(CAT_GAME_INDEX(cat));
    return p ? p->module : "";
}

// A GAME IS NOT A MODULE. Fantasy Earth ships two profile rows -- FE_Client.dll and
// the English-patched FE_Client.en.dll -- and they are one game to the person using
// this dialog. Listing both, disambiguated by module name, exposed an implementation
// detail in order to answer a question they should never have been asked.
//
// So: the sidebar shows the FIRST row of each distinct title, and a write goes to
// EVERY row sharing that title. Keeping the variants in step is what makes it safe
// for nobody to know they exist.
static bool cat_game_is_first(int i)
{
    const TitleProfile* p = profiles_at(i);
    if (!p) return false;
    for (int j = 0; j < i; j++) {
        const TitleProfile* q = profiles_at(j);
        if (q && _stricmp(q->title, p->title) == 0) return false;   // a later variant
    }
    return true;
}

// Call `fn` for each module that belongs to the game selected by `cat`.
template <typename F>
static void cat_game_for_each_module(int cat, F fn)
{
    if (!CAT_IS_GAME(cat)) return;
    const TitleProfile* first = profiles_at(CAT_GAME_INDEX(cat));
    if (!first) return;
    for (int j = 0; j < profiles_count(); j++) {
        const TitleProfile* q = profiles_at(j);
        if (q && _stricmp(q->title, first->title) == 0) fn(q->module);
    }
}

// Defined with the layout below; the game-section test needs it here.
static int cat_of_row(const ShimOption* opts, int i);

// Is `i` an OPT_GROUP tagged as belonging to a profiled title? Returns its module
// leaf, or NULL. See ShimOption::title_of -- it is a dedicated field because the
// option-table selftest (rightly) refuses to let a heading carry a key.
static const wchar_t* group_title_leaf(const ShimOption* opts, int i)
{
    if (opts[i].type != OPT_GROUP) return NULL;
    if (!opts[i].title_of || !opts[i].title_of[0]) return NULL;
    return opts[i].title_of;
}

// AN ACTION ROW'S ARGUMENT. `choices` on an OPT_BUTTON names the action, and three of
// them are parameterised by game: "cfgapp:Fantasy Earth", "padapp:Final Fantasy XI",
// "gamecfg:Tetra Master". Splitting here rather than inventing a fourth field keeps
// the table readable and the selftest's "headings and packs own no key" invariant
// untouched.
static bool action_is(const wchar_t* choices, const wchar_t* verb, char* arg, size_t cch)
{
    if (arg && cch) *arg = 0;
    if (!choices || !verb) return false;
    size_t vl = wcslen(verb);
    if (_wcsnicmp(choices, verb, vl) != 0 || choices[vl] != L':') return false;
    if (arg && cch)
        WideCharToMultiByte(CP_ACP, 0, choices + vl + 1, -1, arg, (int)cch, NULL, NULL);
    return true;
}

// IS THERE ANYTHING BEHIND THIS ROW ON THIS MACHINE?
//
// A game-owned row whose title is not installed has no value to show and no key to
// write, and an "open its config" button for a tool that is not there is a button
// that can only fail. Those are HIDDEN rather than disabled -- unlike the mode gate,
// where greying-out carries information ("this is for the other server"), here the
// answer is simply that this game is not on this computer, and a section full of
// dead rows for games you do not own is exactly the clutter this pass is removing.
static bool row_absent_here(const ShimOption* o)
{
    if (!o) return false;
    if (o->type == OPT_BUTTON && o->choices) {
        char game[96]; wchar_t path[MAX_PATH];
        if (action_is(o->choices, L"cfgapp", game, sizeof(game)))
            return !gamecfg_configapp(game, 0, path, _countof(path));
        if (action_is(o->choices, L"padapp", game, sizeof(game)))
            return !gamecfg_configapp(game, 1, path, _countof(path));
    }
    return false;
}

// Does the row at `i` belong in game section `cat`? Either because the row is
// per-title-capable, or because its whole CATEGORY is tagged for that game. The
// second half is what merges "Final Fantasy XI add-ons" into "Final Fantasy XI".
static bool row_in_game_section(const ShimOption* opts, int i, int cat)
{
    if (opts[i].per_title) return true;
    int g = cat_of_row(opts, i);
    const wchar_t* gl = (g >= 0) ? group_title_leaf(opts, g) : NULL;
    if (!gl) return false;
    bool hit = false;
    cat_game_for_each_module(cat, [&](const char* mod) {
        wchar_t w[80] = L"";
        if (MultiByteToWideChar(CP_ACP, 0, mod, -1, w, (int)80) > 0 && _wcsicmp(w, gl) == 0)
            hit = true;
    });
    return hit;
}
static int      g_cat = CAT_ALL;
static HFONT    g_font     = NULL;
static HFONT    g_font_hint = NULL;   // the small grey explanation under a row
static HFONT    g_font_hdr  = NULL;   // the bold group heading

// ---------------------------------------------------------------- chord parsing

struct VkName { const wchar_t* name; UINT vk; };
static const VkName g_vknames[] = {
    { L"f1", VK_F1 }, { L"f2", VK_F2 },   { L"f3", VK_F3 },   { L"f4",  VK_F4 },
    { L"f5", VK_F5 }, { L"f6", VK_F6 },   { L"f7", VK_F7 },   { L"f8",  VK_F8 },
    { L"f9", VK_F9 }, { L"f10", VK_F10 }, { L"f11", VK_F11 }, { L"f12", VK_F12 },
    { L"home", VK_HOME }, { L"end", VK_END }, { L"insert", VK_INSERT }, { L"pause", VK_PAUSE },
    // Non-alphanumeric keys people actually reach for in a chord. Without these,
    // `hotkey=shift+tab` does not merely misbehave -- parse_hotkey rejects the whole
    // spec and silently falls back to the default, so the chord the ini asks for is
    // not the chord that is armed.
    // NOTE on shift+tab specifically: Steam's overlay owns it, and Steam sees it first
    // in any Steam-launched title (and on the Deck). It parses and arms fine here; it
    // just may never reach us under Steam.
    { L"tab", VK_TAB }, { L"space", VK_SPACE }, { L"enter", VK_RETURN }, { L"return", VK_RETURN },
    { L"esc", VK_ESCAPE }, { L"escape", VK_ESCAPE }, { L"backspace", VK_BACK },
    { L"delete", VK_DELETE }, { L"del", VK_DELETE },
    { L"pgup", VK_PRIOR }, { L"pgdn", VK_NEXT },
};

// "ctrl+shift+s" -> mods + vk. Returns false on an unparsable spec so the caller can
// fall back to the default rather than silently arming a chord nobody can press.
static bool parse_hotkey(const wchar_t* spec, UINT* mods, UINT* vk)
{
    wchar_t buf[128]; wcsncpy_s(buf, spec, _TRUNCATE);
    _wcslwr_s(buf);
    *mods = 0; *vk = 0;
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(buf, L"+ \t", &ctx); t; t = wcstok_s(NULL, L"+ \t", &ctx)) {
        if      (!wcscmp(t, L"ctrl") || !wcscmp(t, L"control")) *mods |= 1;
        else if (!wcscmp(t, L"shift"))                          *mods |= 2;
        else if (!wcscmp(t, L"alt"))                            *mods |= 4;
        else {
            bool named = false;
            for (int i = 0; i < _countof(g_vknames); i++)
                if (!wcscmp(t, g_vknames[i].name)) { *vk = g_vknames[i].vk; named = true; break; }
            if (!named) {
                if (wcslen(t) != 1) return false;
                wchar_t c = t[0];
                if (c >= L'a' && c <= L'z') *vk = (UINT)(c - L'a' + 'A');
                else if (c >= L'0' && c <= L'9') *vk = (UINT)c;
                else return false;
            }
        }
    }
    return *vk != 0;
}

// "home, ctrl+shift+s" -> as many chords as parse. Returns how many were armed and
// writes the unparsable entries into `bad` for the log, because a typo in one chord
// must not cost you the others -- and must not be silent either. Separator is a comma
// or a semicolon; `+` already means "held together", so it cannot double as a list.
static int parse_hotkey_list(const wchar_t* spec, UINT* mods, UINT* vks, int cap,
                             wchar_t* bad, size_t badcch)
{
    if (bad && badcch) bad[0] = 0;
    wchar_t buf[256]; wcsncpy_s(buf, spec, _TRUNCATE);
    int n = 0;
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(buf, L",;", &ctx); t; t = wcstok_s(NULL, L",;", &ctx)) {
        while (*t == L' ' || *t == L'\t') t++;
        if (!*t) continue;
        if (n >= cap) break;
        UINT m = 0, v = 0;
        if (parse_hotkey(t, &m, &v)) { mods[n] = m; vks[n] = v; n++; }
        else if (bad && badcch) {
            if (bad[0]) wcsncat_s(bad, badcch, L", ", _TRUNCATE);
            wcsncat_s(bad, badcch, t, _TRUNCATE);
        }
    }
    return n;
}

struct PadName { const wchar_t* name; WORD bit; };
static const PadName g_padnames[] = {
    { L"back",  0x0020 }, { L"select", 0x0020 }, { L"start", 0x0010 },
    { L"lb",    0x0100 }, { L"rb",     0x0200 },
    { L"l3",    0x0040 }, { L"r3",     0x0080 },
    { L"up",    0x0001 }, { L"down",   0x0002 }, { L"left", 0x0004 }, { L"right", 0x0008 },
    { L"a",     0x1000 }, { L"b",      0x2000 }, { L"x",    0x4000 }, { L"y",     0x8000 },
};

static WORD parse_pad(const wchar_t* spec)
{
    wchar_t buf[128]; wcsncpy_s(buf, spec, _TRUNCATE);
    _wcslwr_s(buf);
    WORD m = 0; wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(buf, L"+ \t", &ctx); t; t = wcstok_s(NULL, L"+ \t", &ctx)) {
        for (int i = 0; i < _countof(g_padnames); i++)
            if (!wcscmp(t, g_padnames[i].name)) { m |= g_padnames[i].bit; break; }
    }
    return m;
}

// ---------------------------------------------------------------- XInput (dynamic)
//
// Linking xinput.lib would add a hard dependency on a redistributable the Deck's
// prefix may not carry; resolving by name lets a machine with no XInput at all simply
// have no pad chord instead of failing to load the whole shim.

typedef struct { DWORD dwPacketNumber; struct { WORD wButtons; BYTE bLT, bRT; SHORT sTLX, sTLY, sTRX, sTRY; } Gamepad; } XI_STATE;
typedef DWORD (WINAPI *PFN_XIGET)(DWORD, XI_STATE*);
static PFN_XIGET g_xiget = NULL;

static void xinput_resolve()
{
    static const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < _countof(dlls); i++) {
        HMODULE h = LoadLibraryW(dlls[i]);
        if (!h) continue;
        g_xiget = (PFN_XIGET)GetProcAddress(h, "XInputGetState");
        if (g_xiget) { logf("[settings] pad chord via %ls", dlls[i]); return; }
    }
    logf("[settings] no XInput available -- keyboard chord only");
}

// ---------------------------------------------------------------- dialog

static const wchar_t* opt_value(const ShimOption* o, wchar_t* buf, size_t cch)
{
    ini_str(o->sec, o->key, o->def, buf, (DWORD)cch, g_ini);
    return buf;
}

// THE SAME VALUE, RESOLVED FOR ONE TITLE, AND WHERE IT CAME FROM.
//
// Mirrors ini_int_title's order exactly -- [<sec>.<leaf>] then [<sec>] then the
// compiled default -- because a dialog that resolved differently from the runtime
// would be a confident liar, which is worse than no dialog.
//
// `leaf` NULL/empty means no title in scope: answers exactly what opt_value does.
//
// The L"\x01" default tells PRESENT from ABSENT. An absent per-title key must fall
// through to the global value, and an empty string is a legitimate value for a list
// key, so emptiness cannot distinguish them.
//
// VS_PROFILE is declared but not reachable here, deliberately. A TitleProfile
// verdict is per-KEY C++ (window mode, adopt_fmv, pad_slot -- not "the value of
// d3d_windowed"), so there is no generic way to ask the table for a key's value,
// and inventing one that guessed would be the wrong-source log line all over again.
static const wchar_t* opt_value_for(const ShimOption* o, const char* leaf,
                                    wchar_t* buf, size_t cch, ValueSource* src)
{
    if (leaf && *leaf && o->sec && o->key) {
        wchar_t wleaf[80], sec[128];
        if (MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf, _countof(wleaf)) > 0) {
            _snwprintf_s(sec, _countof(sec), _TRUNCATE, L"%ls.%ls", o->sec, wleaf);
            wchar_t probe[512] = L"";
            GetPrivateProfileStringW(sec, o->key, L"\x01", probe,
                                     (DWORD)_countof(probe), g_ini);
            if (probe[0] != 1) {
                ini_decomment(probe);
                wcsncpy_s(buf, cch, probe, _TRUNCATE);
                if (src) *src = VS_TITLE_INI;
                return buf;
            }
        }
    }
    wchar_t g[512] = L"";
    GetPrivateProfileStringW(o->sec, o->key, L"\x01", g, (DWORD)_countof(g), g_ini);
    if (g[0] != 1) {
        ini_decomment(g);
        wcsncpy_s(buf, cch, g, _TRUNCATE);
        if (src) *src = VS_GLOBAL_INI;
        return buf;
    }
    wcsncpy_s(buf, cch, o->def ? o->def : L"", _TRUNCATE);
    if (src) *src = VS_DEFAULT;
    return buf;
}

// --- friendly ENUM choices ---------------------------------------------------
//
// `choices` fields may read "value=Label". The user reads the Label; the ini
// keeps the value. A field with no '=' is both, so every pre-existing row
// ("off|xbox|ps") still works untouched.
static void enum_split(const wchar_t* field, wchar_t* val, size_t nv,
                       wchar_t* lab, size_t nl)
{
    const wchar_t* eq = wcschr(field, L'=');
    if (!eq) { wcsncpy_s(val, nv, field, _TRUNCATE); wcsncpy_s(lab, nl, field, _TRUNCATE); return; }
    size_t n = (size_t)(eq - field);
    if (n >= nv) n = nv - 1;
    memcpy(val, field, n * sizeof(wchar_t)); val[n] = 0;
    wcsncpy_s(lab, nl, eq + 1, _TRUNCATE);
}

// --- THE FILTER --------------------------------------------------------------
//
// Matches on everything the row can be looked for BY: the label and hint (what the
// user reads), and the ini section and key (what the docs, the log and the notes
// call it). Someone who has read `d3d_windowed` in a log should be able to type
// it and land on "Run games in a window" without knowing they are the same thing.
//
// Space-separated terms are ANDed, in any order, so "ffxi window" narrows rather
// than searching for that phrase.
static bool row_matches(const ShimOption& o, const wchar_t* filter)
{
    if (!filter || !filter[0]) return true;

    wchar_t hay[1024];
    swprintf_s(hay, L"%ls %ls %ls %ls",
               o.label   ? o.label   : L"",
               o.hint    ? o.hint    : L"",
               o.sec     ? o.sec     : L"",
               o.key     ? o.key     : L"");
    _wcslwr_s(hay);

    wchar_t needles[64]; wcsncpy_s(needles, filter, _TRUNCATE); _wcslwr_s(needles);
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(needles, L" \t", &ctx); t; t = wcstok_s(NULL, L" \t", &ctx))
        if (!wcsstr(hay, t)) return false;
    return true;
}

// --- LAYOUT METRICS ----------------------------------------------------------
//
// HEIGHT IS SUMMED, NOT COUNTED, since rows stopped being uniform: a heading is a
// different height from an option, and an option with a hint is a line taller. The
// walk that measures and the walk that draws must agree, so the constants live here
// and both use them.
//
// It is a function, and the self-test calls it, because THE WINDOW HAS TO FIT THE
// SCREEN IT IS USED ON. The target device is a Steam Deck: 1280x800, and the
// dialog does not scroll, so a table that grows past that height silently pushes
// Save off the bottom edge -- on the one machine where there is no keyboard to tab
// to it with. Adding rows is cheap; discovering this in Game Mode is not.
static const int LY_ROW = 25, LY_HINT = 14, LY_GROUP = 28;
// An OPT_BUTTON row draws a real 26px pushbutton, which does not fit the 25px an
// option row gets. Its own constant rather than a fudge inside layout_compute, so
// the creation pass and the filter's re-layout cannot disagree about it.
static const int LY_BTNROW = 32, LY_BTNH = 26, LY_BTNW = 200;
static const int LY_LBLW = 300, LY_CTLW = 195, LY_PAD = 12, LY_TOP = 12;
static const int LY_COLW = LY_LBLW + 8 + LY_CTLW + 20;   // one column, plus a gutter
// ONE COLUMN, since the category sidebar landed. The second column existed to fit a
// 1348px table onto an 800px Steam Deck, and it worked -- at the cost of a ~1230px
// wide window and of reading order jumping from the bottom of one column back to the
// top of the other. Showing one category at a time fits the same table in a window
// half the width, so the split is gone rather than kept as a second answer to a
// question that now has one. "All settings" scrolls, which the pane already does.
static const int LY_COLS = 1;
// Everything below the options: two action rows, the note row and the buttons.
// Two action rows now, not three: "Import FFXI character..." moved up into the
// FFXI section (an OPT_BUTTON row), so the strip along the bottom is back to the
// repair/mapping row and the server row. Keep this in step with the by += 32 walk
// in the creation pass -- it is the same rows counted twice, by necessity.
static const int LY_CHROME = 16 + 34 + 34 + 32 + LY_PAD;
// The filter box sits ABOVE the scrolling pane, as a child of the window -- so it
// cannot scroll away from the rows it is filtering. Its height comes off the space
// the pane is allowed, which is why the fit arithmetic subtracts it before
// pane_geometry() rather than after: the window still has to fit the screen.
static const int LY_FILTERBAR = 30;

// TWO COLUMNS, SPLIT ON GROUP BOUNDARIES -- and only in the "All settings" view.
//
// The split exists because one column was 1348px tall once the options grew labels
// and hints, and a Steam Deck is 800. The CATEGORY SIDEBAR solves that same problem
// better: showing one group at a time makes the tallest thing the dialog ever has
// to draw a single group, which is a few hundred pixels. So a picked category lays
// out in ONE column, and the split is kept for "All settings" alone, where there is
// still a whole table to fit. Groups are never split across columns: a heading in
// one column with its rows in the other is worse than an uneven pair.
#define LY_MAXROWS POLSET_MAX_ROWS
struct LyRow { int col, y; };

// THE SIDEBAR. A child of the WINDOW, beside the pane rather than inside it, for
// the same reason the search box is: it must not scroll away from the rows it is
// selecting. Its width is fixed because the longest short-name ("Login & updates")
// is known at compile time -- see the `choices` field on each OPT_GROUP row.
static const int LY_SIDEW = LY_PAD + 150;

// A GROUP'S SHORT NAME, for the sidebar. Headings are written to be read in place
// ("Logging in, and staying up to date"), which is right above a block of rows and
// far too long in a 150px list. OPT_GROUP has no use for `choices`, so the short
// name goes there -- one row still describes one category, and there is no second
// list to keep in step. Falls back to the heading when a group has none.
static const wchar_t* cat_name(const ShimOption& g)
{
    if (g.choices && g.choices[0]) return g.choices;
    return g.label ? g.label : L"";
}

// Which category a row belongs to: the index of the nearest preceding OPT_GROUP.
// Rows before the first heading (there are none today) answer CAT_ALL, so they are
// visible in every view rather than in none -- a row nobody can reach is the worse
// failure, and it is silent.
static int cat_of_row(const ShimOption* opts, int i)
{
    for (int j = i; j >= 0; j--)
        if (opts[j].type == OPT_GROUP) return j;
    return CAT_ALL;
}

// Returns the number of table rows described. A row that is NOT drawn (hidden, dev
// while dev rows are off, filtered out, or a heading whose whole group went with it)
// is given `pos[i].y = -1` -- so a caller can ask "was this drawn" without repeating
// the rules. It used to be left untouched and documented as meaningless, which was
// safe only while the single caller drew everything: the filter re-runs the layout
// and would otherwise read the last one's coordinates for a row that is now gone.
static int layout_compute(bool show_dev, const wchar_t* filter, int cat,
                          LyRow* pos, int cap, int* cw, int* ch)
{
    int n = 0; const ShimOption* opts = shim_options(&n);
    if (n > cap) n = cap;

    // Pass 1 -- row heights, and which block (group) each row belongs to.
    static int  h[LY_MAXROWS], blk[LY_MAXROWS], blkh[LY_MAXROWS];
    static bool draw[LY_MAXROWS];
    ZeroMemory(h, sizeof(h)); ZeroMemory(blkh, sizeof(blkh)); ZeroMemory(draw, sizeof(draw));
    int nblk = 0;
    // A HEADING IS DRAWN ONLY IF SOMETHING UNDER IT IS. Filtering to one row and
    // getting six bare group titles with it would be worse than not filtering: the
    // headings are the landmarks, and landmarks pointing at nothing are noise. So
    // the filter is applied to option rows here and the headings follow in the walk
    // below, which is also what keeps the two-column split honest -- an empty block
    // must not be given a column's worth of height.
    // OPTION ROWS FIRST, HEADINGS SECOND -- and they cannot share a pass. A heading
    // is drawn only if something under it is, which is a question about rows the
    // walk has not reached yet; asked in one pass it reads `draw[j]` while every
    // later entry is still false, so every heading disappears, `nblk` stays 0, and
    // the two-column split quietly becomes one very tall column.
    for (int i = 0; i < n; i++) {
        const ShimOption& o = opts[i];
        if (o.type == OPT_GROUP || o.type == OPT_HIDDEN) continue;
        if (o.dev && !show_dev) continue;
        // A setting for a game this computer does not have, or a button for a tool
        // that is not installed. See row_absent_here.
        if (row_absent_here(&o)) continue;
        // A per-game section shows the per-title-capable rows, whatever
        // category they live in normally -- that is the whole point: they are
        // scattered through the table by feature, and the question "what can I
        // set for THIS game" cuts across all of them.
        if (CAT_IS_GAME(cat)) { if (!row_in_game_section(opts, i, cat)) continue; }
        else if (cat != CAT_ALL && cat_of_row(opts, i) != cat) continue;
        if (!row_matches(o, filter)) continue;
        draw[i] = true;
    }
    // Headings are drawn only in the "All settings" view. With one category picked
    // the sidebar's own selection IS the heading, and repeating it at the top of the
    // pane costs a row of height to say what the highlighted item already said.
    for (int i = 0; cat == CAT_ALL && i < n; i++) {
        const ShimOption& o = opts[i];
        if (o.type != OPT_GROUP) continue;
        if (o.dev && !show_dev) continue;
        for (int j = i + 1; j < n && opts[j].type != OPT_GROUP; j++)
            if (draw[j]) { draw[i] = true; break; }
    }

    // Now the heights, in table order, with the blocks a heading opens.
    for (int i = 0; i < n; i++) {
        const ShimOption& o = opts[i];
        if (!draw[i]) continue;
        if (o.type == OPT_GROUP && nblk < LY_MAXROWS) nblk++;
        h[i] = (o.type == OPT_GROUP)  ? LY_GROUP
             : (o.type == OPT_BUTTON) ? LY_BTNROW
             :                          LY_ROW;
        // A per-title row always gets the hint line even with no hint text,
        // because it always has something to say there: which of the four
        // layers answered.
        if ((o.hint || o.per_title) && o.type != OPT_GROUP) h[i] += LY_HINT;
        int b = nblk ? nblk - 1 : 0;
        blk[i] = b;
        blkh[b] += h[i];
    }
    if (!nblk) nblk = 1;

    // Pass 2 -- lay the blocks out in table order, one column. `blkh` is still
    // summed above because the total is what tells the pane how far it scrolls.
    int ycol[LY_COLS];
    for (int c = 0; c < LY_COLS; c++) ycol[c] = LY_TOP;
    for (int i = 0; i < n; i++) {
        if (!draw[i]) { pos[i].col = 0; pos[i].y = -1; continue; }
        const int c = 0;
        pos[i].col = c;
        pos[i].y   = ycol[c];
        ycol[c]   += h[i];
    }

    int body = 0;
    for (int c = 0; c < LY_COLS; c++) if (ycol[c] - LY_TOP > body) body = ycol[c] - LY_TOP;

    // The action buttons are wider than one option row, and sizing the window to
    // the options alone clipped the last button off the right edge.
    // The action buttons are wider than one option row, and sizing the window to the
    // options alone clipped the last button off the right edge. Keep these four
    // widths in step with the CreateWindow calls that draw them.
    const int ACTIONW = LY_PAD + 185 + 8 + 185 + 8 + 150 + 8 + 140 + LY_PAD;
    const int used = 1;    // one column since the sidebar landed; see LY_COLS
    // The sidebar is part of the WINDOW's width, not the pane's -- the pane is
    // created at x = LY_SIDEW and every row coordinate below stays pane-relative,
    // so the sidebar is added exactly once, here.
    const int optw = LY_SIDEW + LY_PAD + used * LY_COLW + LY_PAD;
    if (cw) *cw = optw > ACTIONW ? optw : ACTIONW;
    if (ch) *ch = LY_TOP + body + LY_CHROME;
    return n;
}

// THE WINDOW IS SIZED FOR THE TALLEST SINGLE CATEGORY, NOT FOR THE WHOLE TABLE.
//
// Two properties have to hold at once, and they pull in opposite directions:
//   * switching category must NOT resize the window -- a dialog that jumps every
//     time you click the list is unusable, especially with a thumbstick;
//   * the window must be as small as the content honestly allows, which is the
//     entire point of adding categories to a behemoth.
// Sizing to the biggest category satisfies both: every category fits without a
// scrollbar, and no category makes the window bigger than the one that needs it
// most. "All settings" is the one view that overflows, and it scrolls -- which the
// pane has done since the clamp was removed, so this adds no new mechanism.
void settings_dialog_size(bool show_dev, int* cw, int* ch)
{
    static LyRow pos[LY_MAXROWS];
    int n = 0; const ShimOption* opts = shim_options(&n);
    int bw = 0, bh = 0;
    for (int i = 0; i < n; i++) {
        if (opts[i].type != OPT_GROUP) continue;
        if (opts[i].dev && !show_dev) continue;
        int w = 0, hh = 0;
        layout_compute(show_dev, NULL, i, pos, (int)_countof(pos), &w, &hh);
        if (w  > bw) bw = w;
        if (hh > bh) bh = hh;
    }
    // No headings at all (an empty or filtered-to-nothing table) -- fall back to the
    // whole-table measure rather than handing back a zero-sized window.
    if (!bh) layout_compute(show_dev, NULL, CAT_ALL, pos, (int)_countof(pos), &bw, &bh);
    if (cw) *cw = bw;
    if (ch) *ch = bh;
}

// --- THE SCREEN IS NOT A BUDGET THE TABLE HAS TO LIVE INSIDE -----------------
//
// It used to be. The window was sized to the WHOLE table and then clamped to the
// work area, which cut the last option rows off below the bottom edge -- drawn,
// saved, and unreachable. That was chosen as the lesser evil against putting Save
// off-screen, and against developer options being a one-way door. It is the wrong
// trade: on the machine that reported it, 792px of table against 672px of work
// area meant most of a column could not be reached at all.
//
// So the options now live in a PANE that scrolls, and the action rows, Save and
// Cancel are children of the WINDOW, below it. Nothing can scroll away from Save,
// and nothing can be cut off -- the two properties the clamp was trading between.
// The table is free to grow.
//
// Pure arithmetic, in its own function, because the self-test has to drive it with
// a Steam Deck's numbers on a desktop -- and because "does it fit" and "where does
// it draw" answering differently is how the old bug stayed invisible.
struct PaneGeom { int win_h, pane_h, content_h, scroll_max; };

static PaneGeom pane_geometry(int desired_h, int avail_h)
{
    PaneGeom g;
    g.content_h = desired_h - LY_CHROME;             // what the option rows need
    // avail_h <= 200 means we could not read a sane work area; trust the layout
    // rather than squeeze the window into a number we do not believe.
    g.win_h  = (avail_h > 200 && desired_h > avail_h) ? avail_h : desired_h;
    g.pane_h = g.win_h - LY_CHROME;
    if (g.pane_h < LY_ROW) g.pane_h = LY_ROW;        // never a zero-height pane
    g.scroll_max = g.content_h > g.pane_h ? g.content_h - g.pane_h : 0;
    return g;
}

// WHERE A ROW'S FOUR CONTROLS SIT, relative to the row's own origin. ONE function,
// used by the creation pass AND by the filter's re-layout, because the two agreeing
// is not optional: if they drift, filtering silently moves every label a few pixels
// off its control and nothing in the code says why.
enum RowPart { RP_HDR, RP_LABEL, RP_CTL, RP_HINT, RP_COUNT };

static void row_origin(const ShimOption* o, int part, int x, int y, int* ox, int* oy)
{
    switch (part) {
    case RP_HDR:   *ox = x;          *oy = y + 10;       break;
    case RP_LABEL: *ox = x;          *oy = y + 3;        break;
    // A BUTTON ROW IS TALLER THAN AN OPTION ROW, so its hint cannot hang at the
    // option-row offset: LY_ROW (25) lands inside a 26px button and the reason
    // text draws through the control that it explains. Measured on screen, not
    // reasoned about -- the first build put it there and the two overlapped.
    case RP_HINT:  *ox = x + 16;
                   *oy = y + (o->type == OPT_BUTTON ? LY_BTNH + 4 : LY_ROW);
                   break;
    default:
        // A checkbox IS its own label, so it starts at the row's left edge; every
        // other control sits in the second column, past the label.
        // A checkbox IS its own label, and so is a button: both start at the row's
        // left edge. Everything else sits in the second column, past the label.
        if (o->type == OPT_BOOL || o->type == OPT_PACK) { *ox = x; *oy = y + 2; }
        else if (o->type == OPT_BUTTON) { *ox = x; *oy = y + 2; }
        else { *ox = x + LY_LBLW + 8; *oy = y; }
        break;
    }
}

// --- the option pane ---------------------------------------------------------

static HWND g_pane        = NULL;
static int  g_pane_scroll = 0;      // pixels the options are scrolled DOWN by
static int  g_pane_max    = 0;

static void pane_scroll_to(HWND hp, int pos)
{
    if (pos < 0)          pos = 0;
    if (pos > g_pane_max) pos = g_pane_max;
    if (pos == g_pane_scroll) return;
    int dy = g_pane_scroll - pos;
    g_pane_scroll = pos;
    // SW_SCROLLCHILDREN moves EVERY child, including the label and hint STATICs
    // that nothing holds a handle to. That is why this scrolls the window rather
    // than repositioning controls out of the layout table: the layout knows where
    // the option controls go, not where their prose went.
    ScrollWindowEx(hp, 0, dy, NULL, NULL, NULL, NULL,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    SCROLLINFO si; ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = pos;
    SetScrollInfo(hp, SB_VERT, &si, TRUE);
    UpdateWindow(hp);
}

// Tab can put the focus on a control that is scrolled out of sight, which reads
// as the keyboard having stopped working. Bring it back instead.
static void pane_reveal(HWND ctl)
{
    if (!g_pane || !g_pane_max || !ctl) return;
    if (GetParent(ctl) != g_pane) return;
    RECT rc, rp;
    GetWindowRect(ctl, &rc);
    MapWindowPoints(NULL, g_pane, (POINT*)&rc, 2);
    GetClientRect(g_pane, &rp);
    if (rc.top < 0)                 pane_scroll_to(g_pane, g_pane_scroll + rc.top - 8);
    else if (rc.bottom > rp.bottom) pane_scroll_to(g_pane, g_pane_scroll + (rc.bottom - rp.bottom) + 8);
}

// RE-LAY-OUT THE PANE FOR THE CURRENT FILTER.
//
// Every control already exists and keeps its value -- filtering MOVES and HIDES, it
// never rebuilds. That is deliberate: a rebuild would have to write the visible
// values back into the ini or lose them, and "typing in the search box discarded my
// unsaved changes" is a far worse bug than a slightly longer function here. It also
// means Save still walks the whole table, so a change made to a row that is now
// filtered out is still saved.
static void apply_filter(HWND win, bool show_dev)
{
    if (!g_pane) return;

    static LyRow pos[LY_MAXROWS];
    int cw = 0, ch = 0;
    int n = layout_compute(show_dev, g_filter, g_cat, pos, (int)_countof(pos), &cw, &ch);
    const ShimOption* opts = shim_options(NULL);

    // Back to the top first: the old scroll offset indexes a layout that no longer
    // exists, and every position below is computed in unscrolled pane coordinates.
    pane_scroll_to(g_pane, 0);
    g_pane_scroll = 0;

    SendMessageW(g_pane, WM_SETREDRAW, FALSE, 0);
    int shown = 0;
    for (int i = 0; i < n && i < (int)_countof(g_ctl); i++) {
        const ShimOption* o = &opts[i];
        HWND parts[RP_COUNT];
        parts[RP_HDR]     = GetDlgItem(g_pane, IDC_HDR_FIRST  + i);
        parts[RP_LABEL]   = GetDlgItem(g_pane, IDC_LBL_FIRST  + i);
        parts[RP_CTL]     = g_ctl[i];
        parts[RP_HINT]    = GetDlgItem(g_pane, IDC_HINT_FIRST + i);

        // ONE source of truth for "is this row on screen": the layout. Repeating its
        // rules here is how a filtered heading or a dev row ends up visible in one
        // pass and not the other.
        const bool vis = pos[i].y >= 0;

        const int x = LY_PAD + pos[i].col * LY_COLW;
        const int y = pos[i].y;
        for (int p = 0; p < 4; p++) {
            if (!parts[p]) continue;
            if (!vis) { ShowWindow(parts[p], SW_HIDE); continue; }
            int ox, oy; row_origin(o, p, x, y, &ox, &oy);
            SetWindowPos(parts[p], NULL, ox, oy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(parts[p], SW_SHOW);
        }
        if (vis && o->type != OPT_GROUP) shown++;
    }
    SendMessageW(g_pane, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_pane, NULL, TRUE);

    // The pane did not change size, only its contents did -- so all that is left is
    // to tell the scrollbar how much there is to travel through now.
    RECT pr; GetClientRect(g_pane, &pr);
    const int pane_h   = pr.bottom - pr.top;
    const int content  = ch - LY_CHROME;
    g_pane_max = content > pane_h ? content - pane_h : 0;
    SCROLLINFO si; ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = content > 0 ? content - 1 : 0;
    si.nPage  = (UINT)pane_h;
    si.nPos   = 0;
    SetScrollInfo(g_pane, SB_VERT, &si, TRUE);
    // The style bit, not just the range: a scrollbar left on an empty pane is a
    // control that does nothing, and one missing from a full pane is a trap.
    LONG st = GetWindowLongW(g_pane, GWL_STYLE);
    LONG want = g_pane_max ? (st | WS_VSCROLL) : (st & ~WS_VSCROLL);
    if (want != st) {
        SetWindowLongW(g_pane, GWL_STYLE, want);
        SetWindowPos(g_pane, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    (void)win;
    if (g_filter[0]) logf("[settings] filter '%ls' -> %d option(s)", g_filter, shown);
}

static LRESULT CALLBACK paneproc(HWND hp, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    // The controls live here now, so their notifications and colour requests
    // arrive here -- but every handler for them is on the parent. Forward rather
    // than duplicate: a second copy of the Save loop is exactly the kind of drift
    // this file has paid for before.
    case WM_COMMAND:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetParent(hp), msg, wp, lp);
    case WM_VSCROLL: {
        SCROLLINFO si; ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        GetScrollInfo(hp, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:        pos -= LY_ROW;      break;
        case SB_LINEDOWN:      pos += LY_ROW;      break;
        case SB_PAGEUP:        pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:      pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos  = si.nTrackPos; break;
        case SB_TOP:           pos  = 0;           break;
        case SB_BOTTOM:        pos  = g_pane_max;  break;
        default: return 0;
        }
        pane_scroll_to(hp, pos);
        return 0;
    }
    case WM_MOUSEWHEEL:
        pane_scroll_to(hp, g_pane_scroll -
                       (GET_WHEEL_DELTA_WPARAM(wp) * LY_ROW * 3) / WHEEL_DELTA);
        return 0;
    }
    return DefWindowProcW(hp, msg, wp, lp);
}

// --- WHAT STILL NEEDS A RESTART ----------------------------------------------
//
// Save now re-reads every safe key into the running shim (shim_reload), so the
// old blanket "restart the Viewer" message was wrong for most of the table. What
// is left is the keys whose effect is a ONE-SHOT startup decision -- a hook only
// installed at attach, a byte patch with no unpatch path, a thread that only
// starts at attach. DIRECTION matters and the save loop knows both values, so it
// is part of the entry: un-ticking a byte patch needs a restart (the bytes stay)
// while ticking it does not (the applier is retried from hot paths); ticking a
// hook ON needs a restart (its IAT entry was never patched) while un-ticking it
// does not (the installed hook tests the flag per call).
enum RestartDir { RB_ALWAYS, RB_ON, RB_OFF };
struct RestartKey { const wchar_t* sec; const wchar_t* key; RestartDir dir; };
static const RestartKey g_restart_keys[] = {
    // one-shot install gates, process-wide declarations, startup-only moments
    { L"dx",        L"dpi_aware",          RB_ALWAYS },
    { L"dx",        L"enable",             RB_ALWAYS },
    { L"dx",        L"d3d_enable",         RB_ALWAYS },
    { L"dx",        L"hook_enable",        RB_ALWAYS },
    { L"dx",        L"shell_scale_enable", RB_ALWAYS },
    { L"inputmode", L"mode",               RB_ALWAYS },
    { L"shortcut",  L"game",               RB_ALWAYS },   // /game injected once, at CRT argv parse
    { L"regfix",    L"enable",             RB_ALWAYS },
    { L"autoupdate",L"delay_ms",           RB_ALWAYS },
    { L"polshim",   L"modules",            RB_ALWAYS },
    { L"polshim",   L"minimal",            RB_ALWAYS },   // read once at startup
    { L"polshim",   L"log",                RB_ALWAYS },
    // hooks armed only when the key was on at startup: turning ON needs the restart
    { L"redirect",  L"enable",             RB_ON },
    { L"polshim",   L"comtrace",           RB_ON },
    { L"dx",        L"mask_guard",         RB_ON },
    { L"dx",        L"d3d_cursor",         RB_ON },
    { L"polshim",   L"startup_state",      RB_ON },
    { L"polshim",   L"titletag",           RB_ON },
    { L"auth",      L"rekey_zero",         RB_ON },
    { L"ffxi",      L"plugins",            RB_ON },
    { L"inputmode", L"swap_confirm",       RB_ON },
    { L"inputmode", L"pad_layout",         RB_ON },
    // byte patches / detours with no un-apply path: turning OFF needs the restart
    { L"polshim",   L"patches",            RB_OFF },
    { L"polshim",   L"patches_optional",   RB_OFF },
    { L"polshim",   L"filecheck_fl",       RB_OFF },   // site B, same no-unpatch rule
    { L"dx",        L"fe_teardown_guard",  RB_OFF },
    { L"dx",        L"fmo_ime_direct",     RB_OFF },
};

// "Off" for the directional test: empty, "off", or a value that parses to zero.
// Everything the restart table names is a switch, so that is the whole vocabulary
// -- free-text keys (server lists, module lists) are only ever RB_ALWAYS or
// RB_ON-from-empty, which this answers correctly too.
static bool rb_val_off(const wchar_t* v)
{
    if (!v || !v[0]) return true;
    if (!_wcsicmp(v, L"off")) return true;
    return v[0] == L'0' && wcstol(v, NULL, 0) == 0;
}

static bool change_needs_restart(const wchar_t* sec, const wchar_t* key,
                                 const wchar_t* oldv, const wchar_t* newv)
{
    for (int i = 0; i < (int)_countof(g_restart_keys); i++) {
        if (_wcsicmp(sec, g_restart_keys[i].sec) || _wcsicmp(key, g_restart_keys[i].key))
            continue;
        switch (g_restart_keys[i].dir) {
        case RB_ALWAYS: return true;
        case RB_ON:     return  rb_val_off(oldv) && !rb_val_off(newv);
        case RB_OFF:    return !rb_val_off(oldv) &&  rb_val_off(newv);
        }
    }
    return false;
}

// The labels of this save's restart-bound changes, for the message box. A pack
// contributes its own label once, however many of its members tripped.
static wchar_t g_restart_labels[640];

// Defined with polsettings_start at the bottom of the file; Save re-arms the
// chords through it after a write.
static void settings_load_chords(const wchar_t* ini);

static void restart_note(const wchar_t* label)
{
    if (!label || !label[0]) return;
    if (wcsstr(g_restart_labels, label)) return;         // already listed
    if (g_restart_labels[0])
        wcsncat_s(g_restart_labels, L"\n  \x2022 ", _TRUNCATE);
    wcsncat_s(g_restart_labels, label, _TRUNCATE);
}

// --- FIX PACKS ---------------------------------------------------------------
//
// One checkbox, several keys. `choices` is "sec.key=on:off|sec.key=on:off".
// A missing ":off" means 0, which is right for every member so far (they are all
// switches) and is parsed rather than assumed, so a future non-boolean member only
// has to spell it out.
struct PackMember { wchar_t sec[32], key[64], on[64], off[64]; };

static bool pack_member(const wchar_t** p, PackMember* m)
{
    const wchar_t* s = *p;
    while (*s == L'|' || *s == L' ') s++;
    if (!*s) return false;
    const wchar_t* end = s;
    while (*end && *end != L'|') end++;
    *p = end;

    wchar_t field[192];
    size_t n = (size_t)(end - s);
    if (n >= _countof(field)) n = _countof(field) - 1;
    memcpy(field, s, n * sizeof(wchar_t)); field[n] = 0;

    wchar_t* dot = wcschr(field, L'.');
    wchar_t* eq  = dot ? wcschr(dot, L'=') : NULL;
    if (!dot || !eq) return false;
    *dot = 0; *eq = 0;
    wcsncpy_s(m->sec, field, _TRUNCATE);
    wcsncpy_s(m->key, dot + 1, _TRUNCATE);

    wchar_t* colon = wcschr(eq + 1, L':');
    if (colon) { *colon = 0; wcsncpy_s(m->off, colon + 1, _TRUNCATE); }
    else       { wcscpy_s(m->off, L"0"); }
    wcsncpy_s(m->on, eq + 1, _TRUNCATE);
    return true;
}

// Ticked means EVERY member is at its on-value. Anything else is a partial state
// and must read as unticked: a pack that showed ticked while one of its keys was
// off would be the exact confusion it exists to remove.
static bool pack_is_on(const ShimOption* o)
{
    if (!o->choices) return false;
    const wchar_t* p = o->choices;
    PackMember m;
    int seen = 0;
    while (pack_member(&p, &m)) {
        seen++;
        wchar_t cur[128];
        ini_str(m.sec, m.key, L"", cur, _countof(cur), g_ini);
        if (wcscmp(cur, m.on) != 0) return false;
    }
    return seen > 0;
}

static int pack_write(const ShimOption* o, bool on)
{
    if (!o->choices) return 0;
    const wchar_t* p = o->choices;
    PackMember m;
    int changed = 0;
    while (pack_member(&p, &m)) {
        const wchar_t* want = on ? m.on : m.off;
        wchar_t cur[128];
        ini_str(m.sec, m.key, L"", cur, _countof(cur), g_ini);
        if (wcscmp(cur, want) == 0) continue;
        WritePrivateProfileStringW(m.sec, m.key, want, g_ini);
        logf("[settings] [%ls] %ls: '%ls' -> '%ls'  (pack: %ls)",
             m.sec, m.key, cur, want, o->label);
        if (change_needs_restart(m.sec, m.key, cur, want))
            restart_note(o->label);
        changed++;
    }
    return changed;
}

// ---------------------------------------------------------------- saved servers
//
// [servers] in polshim.ini is a plain name=address list, so switching between a dev box
// and a live one is a dropdown instead of retyping an IP on a touchscreen keyboard.
// The combo is EDITABLE, so an address that is not saved yet still works exactly as the
// old plain text field did -- the list is an accelerator, never a restriction.
//
// Items read "name = address" and the address is parsed back out on save. That is
// deliberately simpler than driving the combo's edit field from CBN_SELCHANGE, which
// means fighting the control over who owns the text; here the displayed string IS the
// data, so selecting and typing cannot disagree.

#define MAX_SERVERS 32
static wchar_t g_srv_name[MAX_SERVERS][64];
static wchar_t g_srv_addr[MAX_SERVERS][96];
static int     g_nsrv = 0;

static void servers_load()
{
    g_nsrv = 0;
    static wchar_t buf[8192];
    DWORD n = GetPrivateProfileSectionW(L"servers", buf, _countof(buf), g_ini);
    if (!n) return;
    for (wchar_t* p = buf; *p && g_nsrv < MAX_SERVERS; p += wcslen(p) + 1) {
        wchar_t* eq = wcschr(p, L'=');
        if (!eq || eq == p) continue;
        *eq = 0;
        wcsncpy_s(g_srv_name[g_nsrv], p, _TRUNCATE);
        wcsncpy_s(g_srv_addr[g_nsrv], eq + 1, _TRUNCATE);
        ini_decomment(g_srv_addr[g_nsrv]);   // section reads bypass ini_str
        *eq = L'=';
        if (g_srv_addr[g_nsrv][0]) g_nsrv++;
    }
}

// "home = 198.51.100.10" -> "198.51.100.10"; a bare address passes through untouched.
static void server_text_to_addr(const wchar_t* in, wchar_t* out, size_t cch)
{
    const wchar_t* eq = wcsrchr(in, L'=');
    const wchar_t* v  = eq ? eq + 1 : in;
    while (*v == L' ' || *v == L'\t') v++;
    wcsncpy_s(out, cch, v, _TRUNCATE);
    size_t l = wcslen(out);
    while (l && (out[l-1] == L' ' || out[l-1] == L'\t')) out[--l] = 0;
}

// A small modal text prompt. Win32 has no InputBox, and the name for a saved server is
// the one thing here that cannot come from a fixed list.
static wchar_t g_prompt_buf[128];
static bool    g_prompt_ok;

static LRESULT CALLBACK promptproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetWindowTextW(GetDlgItem(h, 100), g_prompt_buf, _countof(g_prompt_buf));
            g_prompt_ok = true; DestroyWindow(h); return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { g_prompt_ok = false; DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: g_prompt_ok = false; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool prompt_text(HWND parent, const wchar_t* title, const wchar_t* label,
                        const wchar_t* initial, wchar_t* out, size_t cch)
{
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!reg) {
        WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = promptproc; wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PolShimPrompt";
        if (!RegisterClassExW(&wc)) return false;
        reg = true;
    }
    g_prompt_ok = false; g_prompt_buf[0] = 0;
    RECT r = { 0, 0, 340, 130 };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"PolShimPrompt", title,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             parent, NULL, inst, NULL);
    if (!h) return false;
    HWND st = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE, 12, 12, 316, 18, h, NULL, inst, NULL);
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              12, 34, 316, 24, h, (HMENU)100, inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              152, 70, 84, 26, h, (HMENU)IDOK, inst, NULL);
    HWND ca = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              244, 70, 84, 26, h, (HMENU)IDCANCEL, inst, NULL);
    HFONT f = g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(st, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(ed, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(ca, WM_SETFONT, (WPARAM)f, TRUE);
    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(h, SW_SHOW); SetForegroundWindow(h); SetFocus(ed);
    SendMessageW(ed, EM_SETSEL, 0, -1);

    MSG m;
    while (IsWindow(h) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { g_prompt_ok = false; DestroyWindow(h); break; }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    if (parent) { EnableWindow(parent, TRUE); SetForegroundWindow(parent); }
    if (g_prompt_ok) wcsncpy_s(out, cch, g_prompt_buf, _TRUNCATE);
    return g_prompt_ok;
}

// A scrollable read-only text window. MessageBox truncates long text, wraps badly and
// cannot be scrolled -- and a regfix report is exactly the sort of multi-line output
// somebody needs to read carefully, or copy out of, when it says a title is missing.
#define IDC_TEXT_EDIT  1
#define IDC_TEXT_CLOSE 2

static LRESULT CALLBACK textproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        // Keep the Close button pinned bottom-right and the text filling the rest, so
        // the dismiss affordance survives any resize.
        int w = LOWORD(lp), t = HIWORD(lp);
        HWND e = GetDlgItem(h, IDC_TEXT_EDIT);
        HWND b = GetDlgItem(h, IDC_TEXT_CLOSE);
        if (e) MoveWindow(e, 8, 8, w - 16, t - 8 - 42, TRUE);
        if (b) MoveWindow(b, w - 8 - 96, t - 8 - 28, 96, 28, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TEXT_CLOSE || LOWORD(wp) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:   DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void show_text(HWND parent, const wchar_t* title, const char* body)
{
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!reg) {
        WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = textproc; wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PolShimText";
        if (!RegisterClassExW(&wc)) return;
        reg = true;
    }
    // Fit the work area rather than assuming a desktop-sized screen: on a Steam Deck
    // (1280x800, and scaled) a fixed 640x460 can land larger than the usable area.
    RECT wa = { 0, 0, 1024, 768 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
    int ww = 640 < aw - 40 ? 640 : aw - 40;
    int wh = 460 < ah - 40 ? 460 : ah - 40;
    if (ww < 320) ww = 320;
    if (wh < 240) wh = 240;

    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"PolShimText", title,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
                             parent, NULL, inst, NULL);
    if (!h) return;
    // NO WS_HSCROLL and NO ES_AUTOHSCROLL: either one turns wrapping OFF in a multiline
    // EDIT, so a narrow window clipped the report instead of reflowing it. Without them
    // it wraps at the window width, and WM_SIZE below re-flows on every resize.
    HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                             8, 8, ww - 32, wh - 100, h, (HMENU)IDC_TEXT_EDIT, inst, NULL);
    // There was NO dismiss affordance here but the title-bar X, which on a Steam Deck
    // (gamepad, and a shell that may own the screen) is effectively unreachable. A real
    // default-push button takes Enter / the pad's confirm, and ESC is handled in the
    // loop below -- three ways out instead of one that does not work on the target.
    HWND cb = CreateWindowExW(0, L"BUTTON", L"Close",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              ww - 32 - 96, wh - 88, 96, 28, h, (HMENU)IDC_TEXT_CLOSE, inst, NULL);
    SendMessageW(cb, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    // Fixed-pitch: the report is column-aligned, and a proportional font destroys that.
    HFONT mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(e, WM_SETFONT, (WPARAM)mono, TRUE);

    // EDIT wants CRLF; the report is LF-separated.
    int n = (int)strlen(body);
    wchar_t* w = (wchar_t*)malloc((size_t)(n * 2 + 4) * sizeof(wchar_t));
    if (w) {
        int j = 0;
        for (int i = 0; i < n; i++) {
            if (body[i] == '\n') w[j++] = L'\r';
            w[j++] = (wchar_t)(unsigned char)body[i];
        }
        w[j] = 0;
        SetWindowTextW(e, w);
        free(w);
    }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(cb);          // focus the button, not the text: Enter/A dismisses immediately

    MSG m;
    while (IsWindow(h) && GetMessageW(&m, NULL, 0, 0) > 0) {
        // ESC before IsDialogMessage: this is a plain window, not a real dialog, so
        // nothing maps ESC to cancel for us.
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { DestroyWindow(h); break; }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    DeleteObject(mono);
}

// ---------------------------------------------------------------- controller mapper
//
// WHY THIS IS A SEPARATE WINDOW AND NOT THREE MORE ROWS IN THE OPTION TABLE
//
// The old controller options were `btn_ok=-1` style TEXT FIELDS holding a DirectInput
// button INDEX. To use them you had to already know the index space, already know which
// slot the client reads for confirm, and then relaunch to find out whether you were
// right. Every one of those is a thing the machine knows and the user does not, and the
// feature failed twice in a row on exactly that gap.
//
// So this window asks the only question a user can actually answer -- "press the button
// you want for Confirm" -- and shows, live, what the client will be handed as a result.
// padmap.cpp applies each change on the client's very next poll, so the shell behind
// this window is testable WITHOUT closing it and without relaunching, which is the
// whole point: bind, look, press, adjust.
//
// Cancel restores the bindings that were live when the window opened, so experimenting
// is free. Save writes them to polshim.ini.

#define IDC_PM_PRESS   3100      // +action
#define IDC_PM_CLEAR   3200      // +action
#define IDC_PM_BOUND   3300      // +action   (static: which button)
#define IDC_PM_SLOT    3400      // +action   (static: which slot, and from where)
#define IDC_PM_DEVICE  3500
#define IDC_PM_LIVE    3501
#define IDC_PM_LAST    3502
#define IDC_PM_APPLY   3503
#define IDC_PM_LAYOUT  3510      // +i, one per padmap layout (deck / xbox / ps / ...)
#define IDC_PM_SWAP    3520
#define IDC_PM_CLEARALL 3521
#define IDC_PM_LEARN   3530      // +action: "the button that does this TODAY"
#define IDC_PM_LEARNCLR 3540
#define IDC_PM_HELP    3541
#define IDC_PM_DETAILS 3514
#define IDC_PM_SAVE    3515
#define IDC_PM_CLOSE   3516

static int  g_pm_capturing = -1;      // action awaiting a press, or -1
static int  g_pm_learning  = -1;      // action whose SLOT we are measuring, or -1
static int  g_pm_learn_was = 0;       // remap state to restore after a measurement
static int  g_pm_undo[PAD_NACTIONS];
static int  g_pm_undo_on = 0;
static int  g_pm_dirty   = 0;

// Setting a binding ARMS the mapping, and clearing them all disarms it. Without this the
// switch is a second thing to discover: you press "Xbox layout", nothing happens because
// pad_remap is still 0, and the feature looks broken in precisely the way its two
// predecessors did. There is still an explicit checkbox, because being able to toggle the
// mapping off and on is how you prove it is the thing doing something.
static void pm_arm_if_bound(HWND h)
{
    int any = 0;
    for (int a = 0; a < PAD_NACTIONS; a++) if (padmap_binding(a) >= 0) any = 1;
    padmap_set_enabled(any);
    SendMessageW(GetDlgItem(h, IDC_PM_APPLY), BM_SETCHECK,
                 any ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void pm_refresh_rows(HWND h)
{
    for (int a = 0; a < PAD_NACTIONS; a++) {
        wchar_t t[128];
        if (g_pm_capturing == a) {
            wcscpy_s(t, L"press a button...");
        } else {
            char n[32]; padmap_button_name(padmap_binding(a), n, sizeof(n));
            swprintf_s(t, L"%hs", n);
        }
        SetDlgItemTextW(h, IDC_PM_BOUND + a, t);

        char whence[160];
        int slot = padmap_pol_index(a, whence, sizeof(whence));
        if (g_pm_learning == a)
            swprintf_s(t, L"press what %hs does TODAY...", padmap_action_name(a));
        else
            swprintf_s(t, L"slot %d  (%hs)", slot, whence);
        SetDlgItemTextW(h, IDC_PM_SLOT + a, t);
    }
}

// The live readout. This is the half that makes the window a DIAGNOSTIC as well as an
// editor: if nothing ever appears here, the problem is upstream of any mapping and no
// amount of binding will fix it -- so say which upstream problem it is.
static void pm_refresh_live(HWND h)
{
    unsigned char btns[32]; int nbtn = 0; const char* src = "none";
    int have = padmap_live_buttons(btns, &nbtn, &src);

    wchar_t line[512];
    char prod[128];
    int ndev = padmap_device_seen(prod, sizeof(prod));
    if (ndev && !strcmp(src, "game"))
        swprintf_s(line, L"Pad: %hs  -- %d buttons, read from the client's own device.",
                   prod[0] ? prod : "(unnamed)", nbtn);
    else if (!strcmp(src, "xinput"))
        swprintf_s(line, L"Pad: read through XInput. The client has NOT opened a "
                         L"controller yet, so NOTHING is being remapped -- turn on "
                         L"Controller mode and restart the Viewer.");
    else
        swprintf_s(line, L"No controller is answering. On a Steam Deck, check that Steam "
                         L"Input is using a CONTROLLER layout, not Keyboard/Mouse.");
    SetDlgItemTextW(h, IDC_PM_DEVICE, line);

    line[0] = 0;
    if (have) {
        for (int i = 0; i < 32; i++) {
            if (!btns[i]) continue;
            char n[32]; padmap_button_name(i, n, sizeof(n));
            wchar_t one[48]; swprintf_s(one, L"%s%hs", line[0] ? L"  " : L"", n);
            wcsncat_s(line, one, _TRUNCATE);
        }
    }
    wchar_t down[560];
    swprintf_s(down, L"Held now: %s", line[0] ? line : L"(nothing)");
    SetDlgItemTextW(h, IDC_PM_LIVE, down);

    // IS THE MAPPING ACTUALLY RUNNING? A count of polls we permuted, straight off the
    // client's own input path. Zero with bindings set means the permutation is not
    // reaching the client at all -- a different fault from "the buttons are wrong", and
    // the two are indistinguishable without this number.
    if (padmap_enabled()) {
        int n = padmap_remapped_count();
        wchar_t st[200];
        if (n > 0) swprintf_s(st, L"LIVE - %d polls remapped. Save is only for next time.", n);
        else       swprintf_s(st, L"On, but NOTHING remapped yet - see Details...");
        SetDlgItemTextW(h, IDC_PM_HELP, st);
    } else {
        SetDlgItemTextW(h, IDC_PM_HELP,
            L"Applies the moment you change it. Save is only for next time.");
    }

    // "What did that button just do" -- the test half, answered from the same permutation
    // the client is being served, not from a second model of it.
    int last = padmap_last_down();
    if (last >= 0) {
        char pn[32], an[32];
        padmap_button_name(last, pn, sizeof(pn));
        int slot = padmap_logical_for_phys(last);
        int act  = padmap_action_for_phys(last);
        padmap_button_name(slot, an, sizeof(an));
        swprintf_s(down, L"Last press: %hs  ->  the client is handed slot %d%s%hs%s",
                   pn, slot,
                   act >= 0 ? L" = " : L" (no action bound to that slot)",
                   act >= 0 ? padmap_action_name(act) : "",
                   padmap_enabled() ? L"" : L"   [mapping is OFF]");
    } else {
        swprintf_s(down, L"Last press: press any button to see where it lands.");
    }
    SetDlgItemTextW(h, IDC_PM_LAST, down);
}

static LRESULT CALLBACK padmapproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (g_pm_capturing >= 0 || g_pm_learning >= 0) {
            int got = padmap_capture_poll();
            if (got >= 0) {
                if (g_pm_learning >= 0) {
                    // The press we just saw IS the slot, because the remap was forced off
                    // for the measurement -- see padmap_learn_slot.
                    padmap_learn_slot(g_pm_learning, got);
                    g_pm_learning = -1;
                    padmap_set_enabled(g_pm_learn_was);
                } else {
                    padmap_bind(g_pm_capturing, got);
                    g_pm_capturing = -1;
                    pm_arm_if_bound(h);
                }
                g_pm_dirty = 1;
                pm_refresh_rows(h);
            }
        }
        pm_refresh_live(h);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= IDC_PM_PRESS && id < IDC_PM_PRESS + PAD_NACTIONS) {
            g_pm_capturing = id - IDC_PM_PRESS;
            padmap_capture_begin();
            pm_refresh_rows(h);
            return 0;
        }
        if (id >= IDC_PM_CLEAR && id < IDC_PM_CLEAR + PAD_NACTIONS) {
            padmap_bind(id - IDC_PM_CLEAR, -1);
            g_pm_dirty = 1;
            pm_arm_if_bound(h);
            pm_refresh_rows(h);
            return 0;
        }
        if (id >= IDC_PM_LEARN && id < IDC_PM_LEARN + PAD_NACTIONS) {
            // Measuring through a live permutation would measure the permutation, so the
            // remap goes off for the duration and is put back afterwards.
            g_pm_learning = id - IDC_PM_LEARN;
            g_pm_learn_was = padmap_enabled();
            padmap_set_enabled(0);
            padmap_capture_begin();
            pm_refresh_rows(h);
            return 0;
        }
        if (id == IDC_PM_LEARNCLR) {
            for (int a = 0; a < PAD_NACTIONS; a++) padmap_learn_slot(a, -1);
            g_pm_dirty = 1;
            pm_refresh_rows(h);
            return 0;
        }
        if (id >= IDC_PM_LAYOUT && id < IDC_PM_LAYOUT + padmap_layout_count()) {
            padmap_preset(padmap_layout_key(id - IDC_PM_LAYOUT));
            g_pm_dirty = 1; pm_arm_if_bound(h); pm_refresh_rows(h);
            return 0;
        }
        switch (id) {
        case IDC_PM_CLEARALL: padmap_preset("off");  g_pm_dirty = 1; pm_arm_if_bound(h); pm_refresh_rows(h); return 0;
        case IDC_PM_SWAP:
            padmap_swap(PAD_OK, PAD_CANCEL);
            g_pm_dirty = 1; pm_arm_if_bound(h); pm_refresh_rows(h);
            return 0;
        case IDC_PM_APPLY:
            padmap_set_enabled(SendMessageW(GetDlgItem(h, IDC_PM_APPLY), BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_pm_dirty = 1;
            pm_refresh_rows(h);
            return 0;
        case IDC_PM_DETAILS: {
            static char rep[16384];
            padmap_report(rep, sizeof(rep));
            size_t used = strlen(rep);
            // The registry picture stays reachable: the two mechanisms coexist, and the
            // old report is still the only thing that shows what is actually in the hive.
            strncat_s(rep, sizeof(rep), "\n\n--- registry button table (the older, "
                                        "launch-time mechanism) ---\n\n", _TRUNCATE);
            used = strlen(rep);
            swapconfirm_report(rep + used, sizeof(rep) - used);
            show_text(h, L"Controller mapping details", rep);
            return 0;
        }
        case IDC_PM_SAVE:
            padmap_save(g_ini);
            g_pm_dirty = 0;
            // NO "restart to take effect" here. It is already in effect -- saying
            // otherwise next to a control labelled "applies immediately" is what made
            // this dialog contradict itself.
            MessageBoxW(h, L"Written to polshim.ini, so it will still be here next time.\n\n"
                           L"Nothing changed just now: the mapping has been live since you "
                           L"set it.",
                        L"Controller mapping", MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDC_PM_CLOSE:
        case IDCANCEL:
            if (g_pm_capturing >= 0 || g_pm_learning >= 0) {   // ESC cancels the press, not the window
                if (g_pm_learning >= 0) padmap_set_enabled(g_pm_learn_was);
                g_pm_capturing = -1;
                g_pm_learning  = -1;
                padmap_capture_cancel();
                pm_refresh_rows(h);
                return 0;
            }
            if (g_pm_dirty) {
                int r = MessageBoxW(h, L"Keep these bindings?\n\nYes keeps them for this "
                                       L"session, No puts back what was there when you "
                                       L"opened this window.\n\nEither way they are only "
                                       L"permanent once you press Save.",
                                    L"Controller mapping", MB_YESNO | MB_ICONQUESTION);
                if (r == IDNO) padmap_restore_bindings(g_pm_undo, g_pm_undo_on);
            }
            DestroyWindow(h);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        PostMessageW(h, WM_COMMAND, IDC_PM_CLOSE, 0);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        padmap_capture_cancel();
        g_pm_capturing = -1;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void padmap_open(HWND parent)
{
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!reg) {
        WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = padmapproc; wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PolShimPadMap";
        if (!RegisterClassExW(&wc)) return;
        reg = true;
    }
    padmap_snapshot_bindings(g_pm_undo, &g_pm_undo_on);
    g_pm_dirty = 0;
    g_pm_capturing = -1;

    const int W = 760, PAD = 12, ROW = 30;
    const int H = 10 + 20 + 22 + 26 + 18 + PAD_NACTIONS * ROW + 28 + 34 + 44 + 16;
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"PolShimPadMap", L"Controller mapping",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             parent, NULL, inst, NULL);
    if (!h) { logf("[pad] mapper CreateWindow failed (%lu)", GetLastError()); return; }

    HFONT f = g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    struct Mk {
        static HWND st(HWND h, HINSTANCE i, HFONT f, const wchar_t* t, int x, int y, int w, int hh, int id) {
            HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     x, y, w, hh, h, (HMENU)(INT_PTR)id, i, NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return c;
        }
        static HWND bt(HWND h, HINSTANCE i, HFONT f, const wchar_t* t, int x, int y, int w, int hh, int id) {
            HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     x, y, w, hh, h, (HMENU)(INT_PTR)id, i, NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return c;
        }
    };

    int y = 10;
    Mk::st(h, inst, f, L"", PAD, y, W - 2 * PAD, 18, IDC_PM_DEVICE);      y += 20;
    Mk::st(h, inst, f, L"", PAD, y, W - 2 * PAD, 18, IDC_PM_LIVE);        y += 22;
    Mk::st(h, inst, f, L"", PAD, y, W - 2 * PAD, 18, IDC_PM_LAST);        y += 26;

    Mk::st(h, inst, f, L"I want this action on...", PAD + 80, y, 200, 16, 0);
    Mk::st(h, inst, f, L"...and the client reads it from", PAD + 440, y, 240, 16, 0);
    y += 18;

    for (int a = 0; a < PAD_NACTIONS; a++) {
        wchar_t lbl[32]; swprintf_s(lbl, L"%hs", padmap_action_name(a));
        Mk::st(h, inst, f, lbl,  PAD,       y + 5, 76,  18, 0);
        Mk::st(h, inst, f, L"",  PAD + 78,  y + 5, 96,  18, IDC_PM_BOUND + a);
        Mk::bt(h, inst, f, L"Press a button", PAD + 176, y, 116, 26, IDC_PM_PRESS + a);
        Mk::bt(h, inst, f, L"Clear",          PAD + 296, y,  56, 26, IDC_PM_CLEAR + a);
        // The MEASURE button. Everything left of here is what you want; this is what the
        // client actually does, and it is the only thing here that is not an inference.
        Mk::bt(h, inst, f, L"Measure",        PAD + 358, y,  72, 26, IDC_PM_LEARN + a);
        Mk::st(h, inst, f, L"",  PAD + 438, y + 5, W - PAD - 438 - PAD, 18, IDC_PM_SLOT + a);
        y += ROW;
    }
    y += 4;
    Mk::st(h, inst, f,
           L"Still wrong? Turn mapping off, find which button really does an action, "
           L"then Measure it.",
           PAD, y, W - 2 * PAD - 140, 18, 0);
    Mk::bt(h, inst, f, L"Forget measured", W - PAD - 128, y - 4, 128, 24, IDC_PM_LEARNCLR);
    y += 24;

    // One button per layout, from padmap's own table -- so a layout somebody measures
    // later appears here by adding it there, and the button can never name a preset that
    // does not exist. The Deck is first because it is the platform this is for.
    int bx = PAD;
    for (int i = 0; i < padmap_layout_count(); i++) {
        wchar_t lb[48]; swprintf_s(lb, L"%hs", padmap_layout_label(i));
        int w = 8 * (int)wcslen(lb) + 24;
        Mk::bt(h, inst, f, lb, bx, y, w, 26, IDC_PM_LAYOUT + i);
        bx += w + 6;
    }
    Mk::bt(h, inst, f, L"Swap Confirm/Cancel", bx, y, 150, 26, IDC_PM_SWAP);   bx += 156;
    Mk::bt(h, inst, f, L"Clear all",           bx, y,  80, 26, IDC_PM_CLEARALL); bx += 86;
    Mk::bt(h, inst, f, L"Details...",          bx, y,  90, 26, IDC_PM_DETAILS);
    y += 34;

    HWND ap = CreateWindowExW(0, L"BUTTON", L"Mapping on",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              PAD, y + 6, 100, 22, h, (HMENU)IDC_PM_APPLY, inst, NULL);
    SendMessageW(ap, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(ap, BM_SETCHECK, padmap_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    // ONE status line, owned by pm_refresh_live, saying either "it is live, N polls
    // remapped" or "everything applies as you change it; Save is only for next time".
    // The old pair -- a checkbox promising "takes effect immediately" beside a Save box
    // announcing a restart -- said both at once and neither was wrong, which is worse.
    Mk::st(h, inst, f, L"", PAD + 106, y + 10, W - PAD - 106 - 200, 18, IDC_PM_HELP);
    Mk::bt(h, inst, f, L"Save", W - PAD - 190, y + 4, 90, 28, IDC_PM_SAVE);
    Mk::bt(h, inst, f, L"Close", W - PAD - 94, y + 4, 90, 28, IDC_PM_CLOSE);

    pm_refresh_rows(h);
    pm_refresh_live(h);
    SetTimer(h, 1, 60, NULL);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    logf("[pad] mapper opened (remap=%d)", padmap_enabled());

    if (parent) EnableWindow(parent, FALSE);
    MSG m;
    while (IsWindow(h) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            SendMessageW(h, WM_COMMAND, IDC_PM_CLOSE, 0);
            continue;
        }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    if (parent) { EnableWindow(parent, TRUE); SetForegroundWindow(parent); }
    logf("[pad] mapper closed");
}

// ---------------------------------------------------------------- HiDPI fit
//
// The PROCESS declares per-monitor-v2 DPI awareness for the GAME's sake
// (dxhook.cpp dpi_declare, [dx] dpi_aware=1 by default): physical pixels, no
// bitmap stretch, correct DirectInput cursor mapping. This dialog, though, is
// hand-laid in 96-DPI pixel constants (LY_*, three CreateFontW heights, ~15
// bare literals), so under that context a 200% monitor draws it at half size
// -- "incredibly small on a super high res monitor", reported 2026-09-02.
//
// Scaling all 69 LY_* sites is the big, risky fix. The contained one: make
// JUST THIS THREAD DPI-unaware while the dialog lives, so Windows scales the
// window for us. GDISCALED (-5, Win10 1809+) keeps GDI text crisp; plain
// UNAWARE (-1) is the fallback. The context binds at window CREATION and the
// message pump runs inside the scope, so the prompt / report / padmap children
// opened from WM_COMMAND inherit it too. SPI_GETWORKAREA virtualizes under an
// unaware context, so pane_geometry()'s fit arithmetic stays consistent by
// construction. With [dx] dpi_aware=0 the process is unaware already and this
// whole thing is a harmless no-op. Resolved dynamically: the API is Win10
// 1607+; where it is absent nothing declared awareness either, so there is
// nothing to undo.
typedef HANDLE (WINAPI *ui_set_thread_dpi_t)(HANDLE);
static ui_set_thread_dpi_t ui_thread_dpi_fn(void)
{
    static ui_set_thread_dpi_t fn = (ui_set_thread_dpi_t)(uintptr_t)1;
    if (fn == (ui_set_thread_dpi_t)(uintptr_t)1)
        fn = (ui_set_thread_dpi_t)GetProcAddress(
                 GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    return fn;
}
// NOT static: a dialog opened from the WATCHER THREAD rather than from inside
// this dialog's scope inherits nothing and needs its own fit. Sharing this one
// rather than copying it -- a second implementation of the DPI dance is a
// second dialog that can go back to being half-size.
HANDLE ui_dpi_fit_begin(void)
{
    ui_set_thread_dpi_t fn = ui_thread_dpi_fn();
    if (!fn) return NULL;
    HANDLE prev = fn((HANDLE)(intptr_t)-5);            // UNAWARE_GDISCALED
    if (!prev) prev = fn((HANDLE)(intptr_t)-1);        // UNAWARE
    if (prev) logf("[settings] thread DPI context -> unaware for the dialog "
                   "(OS scales it; was %p)", prev);
    return prev;
}
void ui_dpi_fit_end(HANDLE prev)
{
    ui_set_thread_dpi_t fn = ui_thread_dpi_fn();
    if (fn && prev) fn(prev);
}

// Open the mapper on its own -- used by the standalone test build, and by anything that
// wants the controller UI without the whole settings dialog behind it.
void polsettings_open_padmap(const wchar_t* ini)
{
    if (ini) wcsncpy_s(g_ini, ini, _TRUNCATE);
    if (!g_font)
        g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HANDLE dpi_prev = ui_dpi_fit_begin();
    padmap_open(NULL);
    ui_dpi_fit_end(dpi_prev);
}

// ===========================================================================
// PER-GAME SETTINGS EDITOR -- the titles' own options, without their config apps
// ===========================================================================
//
// Same shape as padmap_open above: a runtime-built window, the measured rows as
// editable boxes, and the raw DIFF report (which is the whole surface, including
// everything not yet decoded) one Details... button away.
//
// It writes only boxes whose text actually CHANGED. Rewriting an untouched value is
// how a stale control silently overwrites something a different editor owns -- the
// hazard OPT_HIDDEN exists for in iniheal's table.
//
// IMPORTANT: THESE ARE HKLM WRITES. pol.exe is elevated (regfix.cpp says so, and depends on
// it), but a Viewer started some other way is not, and then every write here is
// refused. That MUST be loud: the status line names the error and says what it means.

// THE DIFF REPORT, IN A WINDOW. Not a settings screen.
//
// There WAS a settings screen here -- ~130 controls over the titles' own registry keys
// and ini files -- and it was removed on 2026-08-26 in favour of a button per game that
// opens that game's OWN settings app. See the note at the top of gamecfg.cpp for why:
// SE's tools know the valid resolutions, the names of the quality levels and which
// combinations work, and none of that is recoverable from here.
//
// What survives is the read-only half, because it answers a question no config app
// does: WHICH stored number is the setting you just changed. Open it, change something
// in the title's own tool, open it again, read the * lines.
void gamecfg_show_report(HWND parent, const wchar_t* ini)
{
    if (ini && ini != g_ini) wcsncpy_s(g_ini, ini, _TRUNCATE);
    static char gbuf[65536];
    gamecfg_report(g_ini, gbuf, sizeof(gbuf));
    show_text(parent, L"Every value the games store (and what changed)", gbuf);
}

// ---------------------------------------------------------------------------
// IS THIS COPY POINTED AT REAL SQUARE ENIX -- as the DIALOG currently stands.
//
// Deliberately not read from the ini. The "Connect to REAL Square Enix" checkbox is
// in this same window, so an ini read would leave the two FFXI buttons a Save-and-
// reopen behind the tick box that governs them: you would tick it, watch nothing
// change, and reasonably conclude the gate was broken. Filtering MOVES controls and
// never destroys them, so the checkbox exists whatever category is open; the ini is
// the fallback for the case where the row was somehow not drawn at all.
// ---------------------------------------------------------------------------
static bool dlg_bool(const wchar_t* sec, const wchar_t* key, int fallback)
{
    int n = 0; const ShimOption* opts = shim_options(&n);
    for (int i = 0; i < n && i < (int)_countof(g_ctl); i++) {
        const ShimOption& o = opts[i];
        if (o.type != OPT_BOOL || !o.sec || !o.key) continue;
        if (_wcsicmp(o.sec, sec) != 0 || _wcsicmp(o.key, key) != 0) continue;
        if (g_ctl[i]) return SendMessageW(g_ctl[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
        break;
    }
    return GetPrivateProfileIntW(sec, key, fallback, g_ini) != 0;
}

static bool dlg_se_mode(void) { return dlg_bool(L"redirect", L"enable", 0); }

// GREY IT OUT, AND SAY WHY -- do not hide it. An action that is simply absent in one
// mode cannot be asked about, cannot be found by the search box, and reads as a build
// that shipped without the feature; a disabled row with its reason in the hint line
// answers "where is Import?" on its own. The hint text is swapped rather than appended
// so repeated calls (this runs on every checkbox click) cannot pile the reason up.
static void apply_mode_gates(void)
{
    if (!g_pane) return;
    const bool se = dlg_se_mode();
    int n = 0; const ShimOption* opts = shim_options(&n);
    for (int i = 0; i < n && i < (int)_countof(g_ctl); i++) {
        const ShimOption& o = opts[i];
        if (o.mode == OPTM_ANY || !g_ctl[i]) continue;
        const bool ok = (o.mode == OPTM_SE) ? se : !se;
        EnableWindow(g_ctl[i], ok ? TRUE : FALSE);
        HWND hint = GetDlgItem(g_pane, IDC_HINT_FIRST + i);
        if (!hint) continue;
        SetWindowTextW(hint, ok ? (o.hint ? o.hint : L"")
                       : (o.mode == OPTM_SE
                          ? L"Tick \"Connect to REAL Square Enix\" first -- you export out of retail."
                          : L"Untick \"Connect to REAL Square Enix\" first -- this imports onto this server."));
    }
}

// ---------------------------------------------------------------------------
// "Import FFXI character..." -- the self-serve half of the polexport pipeline.
//
// The player ran `/polexport` on their retail character (the addon is in our
// Ashita catalog) and has a JSON dump. This picks the file and POSTs it to the
// bridge's import endpoint (ffxi_bridge.py, port 54004) with the POL session
// id as `X-POL-Session` -- the SAME sha1(USER token) digest poltoken stamps
// into lobby packets, so the server attributes the import exactly the way it
// attributes a launch, and the character can only land on the account that is
// signed in right here. Synchronous on purpose: this dialog runs on the
// settings watcher thread, not the Viewer's UI thread, and the POST is
// seconds at worst (same reasoning as the Ashita manager's install).
// ---------------------------------------------------------------------------
static void import_character_clicked(HWND h)
{
    char sid[17];
    if (!poltoken_session_hex(sid)) {
        MessageBoxW(h,
            L"Sign into PlayOnline first, then come back here.\n\n"
            L"The shim learns your session id at sign-in, and the server uses "
            L"it to decide which account receives the character.",
            L"Import FFXI character", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = h;
    ofn.lpstrFilter = L"polexport dump (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = _countof(path);
    ofn.lpstrTitle  = L"Pick the JSON that /polexport wrote";
    ofn.lpstrInitialDir = NULL;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        MessageBoxW(h, L"Could not open that file.",
                    L"Import FFXI character", MB_OK | MB_ICONWARNING);
        return;
    }
    DWORD size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 2 * 1024 * 1024) {
        CloseHandle(f);
        MessageBoxW(h, L"That file is empty or over 2MB -- not a polexport dump.",
                    L"Import FFXI character", MB_OK | MB_ICONWARNING);
        return;
    }
    char* body = (char*)malloc(size);
    DWORD got = 0;
    BOOL rd = body && ReadFile(f, body, size, &got, NULL) && got == size;
    CloseHandle(f);
    if (!rd) {
        free(body);
        MessageBoxW(h, L"Could not read that file.",
                    L"Import FFXI character", MB_OK | MB_ICONWARNING);
        return;
    }
    if (MessageBoxW(h,
            L"Import this character onto YOUR PlayOnline account?\n\n"
            L"Your account's one FFXI character slot must be free. The server "
            L"answers in a few seconds.",
            L"Import FFXI character", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        free(body);
        return;
    }

    // The endpoint lives on the BRIDGE (54004), not the band port, so take the
    // host from shim_http_bases' portless door and dial the bridge directly.
    char bases[2][256];
    char url[320] = "";
    if (shim_http_bases(bases, "")) {
        const char* s = strstr(bases[1], "://");
        if (s) {
            char hostb[256];
            _snprintf_s(hostb, sizeof(hostb), _TRUNCATE, "%s", s + 3);
            char* slash = strchr(hostb, '/');
            if (slash) *slash = 0;
            if (hostb[0])
                _snprintf_s(url, sizeof(url), _TRUNCATE,
                            "http://%s:54004/import", hostb);
        }
    }
    if (!url[0]) {
        free(body);
        MessageBoxW(h, L"No server configured ([redirect] server=).",
                    L"Import FFXI character", MB_OK | MB_ICONWARNING);
        return;
    }
    char hdrs[64];
    _snprintf_s(hdrs, sizeof(hdrs), _TRUNCATE, "X-POL-Session: %s\r\n", sid);
    char resp[1024] = "";
    DWORD status = 0;
    HCURSOR oldcur = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    bool sent = shim_http_post(url, body, size, "application/json", hdrs,
                               resp, sizeof(resp), &status);
    SetCursor(oldcur);
    free(body);

    // The reply is {"ok":..,"message":"..",..}; surface the message and fall
    // back to the raw body -- honest beats pretty when the shape surprises us.
    char text[768];
    const char* mk = strstr(resp, "\"message\": \"");
    if (mk) {
        mk += 12;
        size_t o = 0;
        while (*mk && *mk != '"' && o < sizeof(text) - 1) {
            if (*mk == '\\' && mk[1]) mk++;   // keep it readable, drop the escape
            text[o++] = *mk++;
        }
        text[o] = 0;
    } else {
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%s",
                    resp[0] ? resp : (sent ? "(empty reply)"
                                           : "The server did not answer. Is the "
                                             "bridge's import endpoint up (port 54004)?"));
    }
    bool okd = sent && status == 200;
    logf("[settings] character import -> HTTP %lu: %s",
         (unsigned long)status, text);
    MessageBoxA(h, text, "Import FFXI character",
                MB_OK | (okd ? MB_ICONINFORMATION : MB_ICONWARNING));
}


static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    // Hints grey, headings in the highlight colour. Both are STATICs on a
    // COLOR_BTNFACE background, so the brush has to be handed back or they paint
    // white rectangles over the dialog face.
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)lp);
        if (id >= IDC_HINT_FIRST && id < IDC_HDR_FIRST) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (id >= IDC_HDR_FIRST && id < IDC_HDR_FIRST + POLSET_MAX_ROWS) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_HOTLIGHT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_COMMAND:
        // EN_CHANGE, i.e. every keystroke. The re-layout is a few hundred SetWindowPos
        // calls against controls that already exist, inside SetWindowRedraw(FALSE) --
        // cheaper than the combo repaint that happens when you open a dropdown, and
        // this dialog is not on any hot path to begin with.
        if (LOWORD(wp) == IDC_FILTER && HIWORD(wp) == EN_CHANGE) {
            GetWindowTextW((HWND)lp, g_filter, _countof(g_filter));
            // A SEARCH LOOKS EVERYWHERE. Searching inside the open category only
            // would answer "no such setting" for a setting that is one click away,
            // which is worse than no search at all -- so the first keystroke moves
            // to All settings, and the sidebar is moved with it so the window never
            // shows a selection that disagrees with what is drawn. Clearing the box
            // does NOT jump back: by then the user is reading the results.
            if (g_filter[0] && g_cat != CAT_ALL) {
                g_cat = CAT_ALL;
                HWND cl = GetDlgItem(h, IDC_CATS);
                if (cl) SendMessageW(cl, LB_SETCURSEL, 0, 0);   // item 0 is All settings
            }
            apply_filter(h, g_show_dev);
            return 0;
        }
        if (LOWORD(wp) == IDC_CATS && HIWORD(wp) == LBN_SELCHANGE) {
            HWND cl = (HWND)lp;
            int sel = (int)SendMessageW(cl, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) return 0;
            int cat = (int)(LONG_PTR)SendMessageW(cl, LB_GETITEMDATA, sel, 0);
            if (cat == g_cat) return 0;
            g_cat = cat;
            // The filter and the category are independent: picking a category while a
            // search is live narrows the search to it, which is what the two controls
            // sitting side by side promises.
            apply_filter(h, g_show_dev);
            return 0;
        }
        if (LOWORD(wp) == IDC_RESETFIX) {
            if (MessageBoxW(h,
                    L"Put every setting under \"Making the games work\" back to the "
                    L"configuration this build is tested with, and remove the retired "
                    L"ones that no longer do anything?\n\n"
                    L"Nothing else is touched -- your server, window size, controller "
                    L"map and login settings are left exactly as they are.\n\n"
                    L"Close and reopen the Viewer afterwards.",
                    L"Put the game fixes back",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return 0;
            static char rep[8192];
            int changed = shim_reset_fixes(g_ini, rep, sizeof(rep));
            static char full[8704];
            _snprintf_s(full, sizeof(full), _TRUNCATE,
                        changed ? "%d setting(s) put back. Restart the Viewer for them to take effect.\n\n%s"
                                : "Nothing to change -- the game fixes are already at their tested values.\n\n%s",
                        changed, rep);
            show_text(h, L"Put the game fixes back", full);
            // The dialog is drawing values it read at OPEN time, so every control the
            // reset just changed is now stale on screen -- and Save would write the
            // stale value straight back over the repair. Closing is the honest answer:
            // reopening reads the file again. (Cancel, not Save: nothing here is a
            // pending edit the user made.)
            PostMessageW(h, WM_COMMAND, IDC_CANCEL, 0);
            return 0;
        }
        if (LOWORD(wp) == IDC_REPAIR) {
            static char report[16384];
            int rc = regfix_scan(report, sizeof(report), 1);
            char head[256];
            _snprintf_s(head, sizeof(head), _TRUNCATE,
                        rc < 0 ? "Registration repair could not run.\n\n"
                               : (rc > 0 ? "Registered %d title(s). Restart the Viewer to see them.\n\n"
                                         : "No new titles to register.\n\n"), rc);
            static char full[16640];
            _snprintf_s(full, sizeof(full), _TRUNCATE, "%s%s", head, report);
            show_text(h, L"Game registration repair", full);
            return 0;
        }
        if (LOWORD(wp) == IDC_SRVSAVE || LOWORD(wp) == IDC_SRVDEL) {
            int n = 0; const ShimOption* opts = shim_options(&n);
            int si = -1;
            for (int i = 0; i < n && i < _countof(g_ctl); i++)
                if (opts[i].type == OPT_SERVER) { si = i; break; }
            if (si < 0) return 0;
            wchar_t raw[256]; GetWindowTextW(g_ctl[si], raw, _countof(raw));
            wchar_t addr[128]; server_text_to_addr(raw, addr, _countof(addr));

            if (LOWORD(wp) == IDC_SRVDEL) {
                // Forget the saved entry whose ADDRESS matches; the text box may show
                // either form, so match on the parsed address, not the raw string.
                for (int s2 = 0; s2 < g_nsrv; s2++) {
                    if (_wcsicmp(addr, g_srv_addr[s2]) != 0) continue;
                    WritePrivateProfileStringW(L"servers", g_srv_name[s2], NULL, g_ini);
                    logf("[settings] forgot server '%ls' (%ls)", g_srv_name[s2], addr);
                    break;
                }
            } else {
                if (!addr[0]) {
                    MessageBoxW(h, L"Enter a server address first, then save it under a name.",
                                L"Save server", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t name[128] = L"";
                for (int s2 = 0; s2 < g_nsrv; s2++)          // offer the existing name back
                    if (!_wcsicmp(addr, g_srv_addr[s2])) { wcscpy_s(name, g_srv_name[s2]); break; }
                if (!prompt_text(h, L"Save server", L"Name for this server (e.g. dev, prod):",
                                 name, name, _countof(name))) return 0;
                // A name with '=' would round-trip wrong through the "name = addr" item.
                for (wchar_t* q = name; *q; q++) if (*q == L'=') *q = L'-';
                if (!name[0]) return 0;
                WritePrivateProfileStringW(L"servers", name, addr, g_ini);
                logf("[settings] saved server '%ls' = %ls", name, addr);
            }

            // Rebuild the list in place so the combo reflects the change
            // immediately. EVERY server combo, not just the one the button read
            // from: the [servers] list is shared, so a save/forget on one
            // (the redirect address) must show up in the other (the log endpoint)
            // without waiting for a reopen. Each combo keeps its OWN current text.
            servers_load();
            for (int j = 0; j < n && j < _countof(g_ctl); j++) {
                if (opts[j].type != OPT_SERVER || !g_ctl[j]) continue;
                wchar_t cur[256]; GetWindowTextW(g_ctl[j], cur, _countof(cur));
                wchar_t caddr[128]; server_text_to_addr(cur, caddr, _countof(caddr));
                SendMessageW(g_ctl[j], CB_RESETCONTENT, 0, 0);
                for (int s2 = 0; s2 < g_nsrv; s2++) {
                    wchar_t item[176];
                    swprintf_s(item, L"%ls = %ls", g_srv_name[s2], g_srv_addr[s2]);
                    SendMessageW(g_ctl[j], CB_ADDSTRING, 0, (LPARAM)item);
                }
                wchar_t shown[176]; wcsncpy_s(shown, caddr, _TRUNCATE);
                for (int s2 = 0; s2 < g_nsrv; s2++)
                    if (!_wcsicmp(caddr, g_srv_addr[s2])) {
                        swprintf_s(shown, L"%ls = %ls", g_srv_name[s2], g_srv_addr[s2]); break;
                    }
                SetWindowTextW(g_ctl[j], shown);
            }
            return 0;
        }
        if (LOWORD(wp) == IDC_GAMECFG) {
            gamecfg_show_report(h, g_ini);
            return 0;
        }
        // AN IN-PANE ACTION ROW (OPT_BUTTON). Its id is in the option range, so the
        // table is what tells an action from a setting -- `choices` names the action,
        // and a row added to the table needs no second registration here beyond its
        // name. A disabled button sends nothing, so the mode gate needs no re-test.
        // "SHOW DEVELOPER OPTIONS", LIVE. Ticking it re-lays-out immediately rather
        // than at the next open. The ini is NOT written here -- Save owns that, like
        // every other row -- so cancelling still leaves the stored value alone; what
        // changes now is only what is on screen.
        if (HIWORD(wp) == BN_CLICKED &&
            LOWORD(wp) >= IDC_FIRST && LOWORD(wp) < IDC_FIRST + POLSET_MAX_ROWS) {
            int nn = 0; const ShimOption* oo = shim_options(&nn);
            int ii = LOWORD(wp) - IDC_FIRST;
            if (ii < nn && oo[ii].sec && oo[ii].key &&
                !wcscmp(oo[ii].sec, L"settings") && !wcscmp(oo[ii].key, L"show_dev")) {
                g_show_dev = g_ctl[ii] &&
                             SendMessageW(g_ctl[ii], BM_GETCHECK, 0, 0) == BST_CHECKED;
                // The open category may have just disappeared (it is a dev group and
                // the box was cleared). Fall back to everything rather than leaving a
                // selection that names a category the sidebar no longer lists.
                if (!g_show_dev && g_cat >= 0 && g_cat < nn && oo[g_cat].dev)
                    g_cat = CAT_ALL;
                cats_fill(GetDlgItem(h, IDC_CATS));
                apply_filter(h, g_show_dev);
                logf("[settings] developer options %s (applied live)",
                     g_show_dev ? "SHOWN" : "hidden");
                return 0;
            }
            int n = 0; const ShimOption* opts = shim_options(&n);
            int i = LOWORD(wp) - IDC_FIRST;
            if (i < n && opts[i].type == OPT_BUTTON && opts[i].choices) {
                char garg[96];
                if (action_is(opts[i].choices, L"cfgapp", garg, sizeof(garg)) ||
                    action_is(opts[i].choices, L"padapp", garg, sizeof(garg))) {
                    int which = action_is(opts[i].choices, L"padapp", NULL, 0) ? 1 : 0;
                    // SAY WHAT IS ABOUT TO HAPPEN when the tool has a known obstacle.
                    // FMO's refuses to start while the Viewer is open and its labels
                    // are unreadable outside a Japanese locale -- finding that out by
                    // clicking and watching nothing happen is the experience this
                    // whole pass exists to stop.
                    const char* warn = gamecfg_configapp_warning(garg, which);
                    if (warn) {
                        wchar_t w[700];
                        _snwprintf_s(w, _countof(w), _TRUNCATE, L"%hs\n\nOpen it now?", warn);
                        if (MessageBoxW(h, w, L"PoL-Shim Settings",
                                        MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                            return 0;
                    }
                    DWORD e = gamecfg_configapp_launch(garg, which);
                    if (e) {
                        wchar_t w[400];
                        _snwprintf_s(w, _countof(w), _TRUNCATE,
                                     e == ERROR_FILE_NOT_FOUND
                                       ? L"That tool is not installed with this copy of the game."
                                       : L"Could not open it (error %lu).", e);
                        MessageBoxW(h, w, L"PoL-Shim Settings", MB_OK | MB_ICONWARNING);
                    }
                    return 0;
                }
                if (!wcscmp(opts[i].choices, L"import_char"))      import_character_clicked(h);
                else if (!wcscmp(opts[i].choices, L"padmap"))
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(IDC_ABDIAG, BN_CLICKED), 0);
                else if (!wcscmp(opts[i].choices, L"gamecfg"))
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(IDC_GAMECFG, BN_CLICKED), 0);
                else if (!wcscmp(opts[i].choices, L"srv_save"))
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(IDC_SRVSAVE, BN_CLICKED), 0);
                else if (!wcscmp(opts[i].choices, L"srv_del"))
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(IDC_SRVDEL, BN_CLICKED), 0);
                else logf("[settings] OPT_BUTTON '%ls' has no handler -- the row is in the "
                          "table but nothing acts on it", opts[i].choices);
                return 0;
            }
            // ANY other checkbox may have been the SE toggle. Re-running the gate is two
            // EnableWindow calls; working out whether THIS click was the one costs more
            // to keep right than it saves.
            apply_mode_gates();
        }
        if (LOWORD(wp) == IDC_ABDIAG) {
            padmap_open(h);      // the interactive mapper; its Details... still shows the
                                 // old registry report, which is now a sub-view of it
            return 0;
        }
        if (LOWORD(wp) == IDC_SAVE) {
            int n = 0; const ShimOption* opts = shim_options(&n);
            int changed = 0;
            g_restart_labels[0] = 0;   // fresh per save; stale labels lie
            for (int i = 0; i < n && i < _countof(g_ctl); i++) {
                wchar_t now[256] = L"", cur[256] = L"";
                const ShimOption* o = &opts[i];
                if (o->type == OPT_HIDDEN || o->type == OPT_GROUP || !g_ctl[i])
                    continue;                       // no control, nothing to save
                // An OPT_BUTTON HAS a control, which is exactly why it needs saying:
                // it owns no key, so the compare below would read [NULL] NULL.
                if (o->type == OPT_BUTTON) continue;
                // A PACK owns no key of its own: it writes its members and logs each
                // one, so it reports its own change count and skips the single-key
                // compare below entirely.
                if (o->type == OPT_PACK) {
                    changed += pack_write(o, SendMessageW(g_ctl[i], BM_GETCHECK, 0, 0) == BST_CHECKED);
                    continue;
                }
                if (o->type == OPT_BOOL)
                    wcscpy_s(now, SendMessageW(g_ctl[i], BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
                else if (o->type == OPT_ENUM) {
                    // The combo holds LABELS; recover the value by index from the
                    // same choices string that filled it.
                    int sel = (int)SendMessageW(g_ctl[i], CB_GETCURSEL, 0, 0);
                    wchar_t ch2[512]; wcsncpy_s(ch2, o->choices ? o->choices : L"", _TRUNCATE);
                    wchar_t* ctx = NULL; int idx = 0;
                    for (wchar_t* t = wcstok_s(ch2, L"|", &ctx); t;
                         t = wcstok_s(NULL, L"|", &ctx), idx++) {
                        if (idx != sel) continue;
                        wchar_t cl[192];
                        enum_split(t, now, _countof(now), cl, _countof(cl));
                        break;
                    }
                } else if (o->type == OPT_SERVER) {
                    // The field may hold "name = address" or a bare address; store the
                    // ADDRESS, because that is what netredir parses.
                    wchar_t raw[256]; GetWindowTextW(g_ctl[i], raw, _countof(raw));
                    server_text_to_addr(raw, now, _countof(now));
                } else
                    GetWindowTextW(g_ctl[i], now, _countof(now));
                const char* gleaf = cat_game_leaf(g_cat);
                if (*gleaf) {
                    // PER-GAME SAVE. The rule that keeps this from silting up: a value
                    // equal to the global one is NOT an override, it is the absence of
                    // one -- so it DELETES the key rather than writing a copy. Without
                    // that, every row the user merely looked at would become a
                    // permanent per-game pin that later global changes could not move,
                    // which is iniheal's frozen-key failure invented a second time.
                    wchar_t gl[256] = L"";
                    ini_str(o->sec, o->key, o->def, gl, _countof(gl), g_ini);
                    bool want_override = (wcscmp(now, gl) != 0);
                    // Applied to EVERY module of this game, so an English-patched
                    // build cannot silently keep an older answer than the one the
                    // user just gave.
                    bool did = false;
                    const ShimOption* oo = o;
                    const wchar_t* nowp = now; const wchar_t* glp = gl;
                    cat_game_for_each_module(g_cat, [&](const char* mod) {
                        wchar_t s2[128], w2[80] = L"";
                        MultiByteToWideChar(CP_ACP, 0, mod, -1, w2, _countof(w2));
                        _snwprintf_s(s2, _countof(s2), _TRUNCATE, L"%ls.%ls", oo->sec, w2);
                        wchar_t had2[256] = L""; ValueSource hs = VS_DEFAULT;
                        opt_value_for(oo, mod, had2, _countof(had2), &hs);
                        if (want_override) {
                            if (wcscmp(nowp, had2) != 0 || hs != VS_TITLE_INI) {
                                WritePrivateProfileStringW(s2, oo->key, nowp, g_ini);
                                logf("[settings] [%ls] %ls: '%ls' -> '%ls' (per-game)",
                                     s2, oo->key, had2, nowp);
                                did = true;
                            }
                        } else if (hs == VS_TITLE_INI) {
                            WritePrivateProfileStringW(s2, oo->key, NULL, g_ini);
                            logf("[settings] [%ls] %ls: override REMOVED (follows the "
                                 "every-game value '%ls')", s2, oo->key, glp);
                            did = true;
                        }
                    });
                    if (did) changed++;
                    continue;
                }
                opt_value(o, cur, _countof(cur));
                if (wcscmp(now, cur) != 0) {
                    WritePrivateProfileStringW(o->sec, o->key, now, g_ini);
                    logf("[settings] [%ls] %ls: '%ls' -> '%ls'", o->sec, o->key, cur, now);
                    if (change_needs_restart(o->sec, o->key, cur, now))
                        restart_note(o->label);
                    changed++;
                }
            }
            logf("[settings] saved %d change(s) to %ls", changed, g_ini);
            if (!changed) {
                // A Save that did nothing must still answer. Silence reads as a dead
                // button, which is how "I pressed Save and it did not take" starts.
                HWND sl0 = GetDlgItem(h, IDC_STATUS);
                if (sl0) SetWindowTextW(sl0, L"Nothing to save \x2014 no settings changed.");
            }
            if (changed) {
                // Apply what was just written to the RUNNING shim. shim_reload
                // re-reads the safe subset of every module's keys; the chords are
                // re-armed here because they are this file's own globals.
                shim_reload(g_ini);
                settings_load_chords(g_ini);
                // INTO THE STATUS LINE, not a modal box. Save no longer closes the
                // window, so a dialog that has to be dismissed before the next change
                // is pure friction -- and "OK" on a box that only says "saved" is a
                // click that carries no decision.
                wchar_t st[600];
                if (g_restart_labels[0])
                    swprintf_s(st, L"Saved %d change(s) and applied them. Needs a "
                                   L"restart: %ls", changed, g_restart_labels);
                else
                    swprintf_s(st, L"Saved %d change(s) \x2014 applied, no restart needed.",
                               changed);
                HWND sl = GetDlgItem(h, IDC_STATUS);
                if (sl) SetWindowTextW(sl, st);
            }
            // NO DestroyWindow HERE. Save used to close the window, so changing
            // two unrelated things meant two full trips through it -- open, hunt,
            // change, save, reopen, hunt. Saving is not finishing; Close is.
            return 0;
        }
        if (LOWORD(wp) == IDC_CANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:   DestroyWindow(h); return 0;
    // NO PostQuitMessage here. The loop now exits on IsWindow(h), so posting WM_QUIT is
    // redundant -- and worse, a WM_QUIT left in this thread's queue would make the NEXT
    // GetMessage loop return 0 immediately, i.e. the dialog would open and vanish the
    // second time you pressed the chord.
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// FILL (or REFILL) the category sidebar from the option table, honouring g_show_dev
// and re-selecting whatever g_cat is now. Its own function because the developer
// toggle has to rebuild it live: ticking the box adds a whole category, and a
// sidebar that still lists yesterday's categories is the same lie as a pane that
// still draws yesterday's rows.
static void cats_fill(HWND cats)
{
    if (!cats) return;
    int n = 0; const ShimOption* opts = shim_options(&n);
    SendMessageW(cats, WM_SETREDRAW, FALSE, 0);
    SendMessageW(cats, LB_RESETCONTENT, 0, 0);
    int sel = 0, item = 0;
    int ai = (int)SendMessageW(cats, LB_ADDSTRING, 0, (LPARAM)L"All settings");
    SendMessageW(cats, LB_SETITEMDATA, ai, (LPARAM)CAT_ALL);
    // THE GAMES COME FIRST (2026-09-08). They were listed under every global group,
    // which put "settings for Tetra Master" below half a dozen headings -- while one
    // of those headings was itself holding a Front Mission setting. Most of what
    // anybody opens this window to change belongs to one game, so the games are the
    // top of the list and the global defaults follow them.
    //
    // The title only -- no module name: which DLL a game happens to load is not
    // something anybody should have to read to change a setting. Variants of one
    // title collapse into one entry and are written together
    // (cat_game_for_each_module).
    for (int i = 0; i < profiles_count(); i++) {
        const TitleProfile* p = profiles_at(i);
        if (!p || !cat_game_is_first(i)) continue;
        wchar_t wtit[96] = L"";
        MultiByteToWideChar(CP_ACP, 0, p->title, -1, wtit, _countof(wtit));
        item = (int)SendMessageW(cats, LB_ADDSTRING, 0, (LPARAM)wtit);
        SendMessageW(cats, LB_SETITEMDATA, item, (LPARAM)(CAT_GAME_BASE - i));
        if ((CAT_GAME_BASE - i) == g_cat) sel = item;
    }
    for (int i = 0; i < n; i++) {
        if (opts[i].type != OPT_GROUP) continue;
        if (opts[i].dev && !g_show_dev) continue;
        // A group tagged for a title is NOT listed on its own: its rows appear
        // inside that game's section above, so listing it here would put the
        // same game in the sidebar twice.
        if (group_title_leaf(opts, i)) continue;
        item = (int)SendMessageW(cats, LB_ADDSTRING, 0, (LPARAM)cat_name(opts[i]));
        SendMessageW(cats, LB_SETITEMDATA, item, (LPARAM)i);
        if (i == g_cat) sel = item;
    }
    SendMessageW(cats, LB_SETCURSEL, sel, 0);
    SendMessageW(cats, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(cats, NULL, TRUE);
}

static void build_and_pump()
{
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = wndproc; wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PolShimSettings";
        if (!RegisterClassExW(&wc)) { logf("[settings] RegisterClass failed (%lu)", GetLastError()); return; }
        // The scrolling option pane. Same background brush, so the seam between it
        // and the window below is invisible.
        WNDCLASSEXW wp2; ZeroMemory(&wp2, sizeof(wp2));
        wp2.cbSize = sizeof(wp2); wp2.lpfnWndProc = paneproc; wp2.hInstance = inst;
        wp2.hCursor = LoadCursor(NULL, IDC_ARROW);
        wp2.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wp2.lpszClassName = L"PolShimSettingsPane";
        if (!RegisterClassExW(&wp2)) { logf("[settings] pane RegisterClass failed (%lu)", GetLastError()); return; }
        registered = true;
    }
    if (!g_font)
        g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!g_font_hint)
        g_font_hint = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!g_font_hdr)
        g_font_hdr = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    g_filter[0] = 0;         // every open starts unfiltered -- see g_filter
    servers_load();          // must precede control creation: the combo is filled from it
    int n = 0; const ShimOption* opts = shim_options(&n);
    if (n > _countof(g_ctl)) n = _countof(g_ctl);
    // DEVELOPER ROWS. Read once per open, so ticking the box and reopening the
    // dialog is what reveals them -- the window is sized at creation and cannot
    // grow a row under the user's finger.
    g_show_dev = GetPrivateProfileIntW(L"settings", L"show_dev", 0, g_ini) != 0;
    const bool show_dev = g_show_dev;
    // Hidden rows are healed into the ini but never drawn, so the window is sized to the
    // VISIBLE ones and each visible row gets the next slot -- indexing the layout by the
    // table position would leave a blank gap wherever a hidden key sits.
    int nvis = 0;
    for (int i = 0; i < n; i++) {
        if (opts[i].type == OPT_HIDDEN) continue;
        if (opts[i].dev && !show_dev) continue;
        nvis++;
    }

    // OPEN ON THE FIXES, not on everything. Somebody who reaches for this window
    // because a game is misbehaving is looking for that group, and it is short.
    // Re-resolved on every open rather than remembered: the previous session's
    // category is not a setting anybody asked to persist, and a stale index into a
    // table whose rows moved would select the wrong group in silence.
    g_cat = CAT_ALL;
    for (int i = 0; i < n; i++)
        if (opts[i].type == OPT_GROUP && opts[i].label
            && wcscmp(opts[i].label, L"Display and window") == 0) { g_cat = i; break; }
    g_filter[0] = 0;

    // Measured by settings_dialog_size(), which the self-test also calls -- see the
    // Steam Deck note there.
    // IMPORTANT: SIZE FOR WHAT IS ACTUALLY DRAWN. This measured the DEVELOPER layout whatever
    // the box said, so that revealing rows would never resize the window -- and the
    // cost was a window as tall as the developer table for a player who will never
    // see it (reported 2026-09-08: "the window is very tall and idek why"). The pane
    // already scrolls, so ticking the box just gives it more to scroll through, which
    // is the ordinary behaviour of every other row-count change in this dialog.
    int cw = 0, ch = 0;
    settings_dialog_size(show_dev, &cw, &ch);

    // Fit the WINDOW to the screen and let the option pane scroll -- see
    // pane_geometry(). Save, Cancel and the action rows hang off the window below
    // the pane, so they are on screen by construction whatever the table does.
    RECT wa;
    int avail = 0;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        avail = (wa.bottom - wa.top) - 48;          // title bar and Game Mode furniture
    // The filter bar's height comes out of the pane's budget, BEFORE the fit is
    // computed -- otherwise the window grows by exactly that much and the Steam Deck
    // fit the self-test asserts is quietly 30px wrong.
    if (avail > 200) avail -= LY_FILTERBAR;
    const PaneGeom pg = pane_geometry(ch, avail);
    ch = pg.win_h;
    g_pane_scroll = 0;
    g_pane_max    = pg.scroll_max;
    if (pg.scroll_max)
        logf("[settings] %dpx of options in a %dpx pane (%dpx of work area) -- the "
             "list scrolls, %dpx of travel", pg.content_h, pg.pane_h, avail, pg.scroll_max);

    RECT r = { 0, 0, cw, ch + LY_FILTERBAR };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"PolShimSettings",
                             L"PlayOnline Shim Settings  [v" _CRT_WIDE(POLSHIM_VERSION) L"]",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             NULL, NULL, inst, NULL);
    if (!h) { logf("[settings] CreateWindow failed (%lu)", GetLastError()); return; }

    // The option rows go in here, at the same coordinates layout_compute gives them:
    // the pane's origin IS the window's origin, so nothing about the layout changes
    // when it scrolls. The scrollbar is only styled on when there is travel, so a
    // dialog that fits looks exactly as it did before.
    // WS_EX_CONTROLPARENT IS LOAD-BEARING, not decoration: IsDialogMessage does not
    // descend into a child window without it, so Tab would walk straight past the
    // pane and every option would become keyboard-unreachable -- the same class of
    // lockout the old clamp existed to avoid, reintroduced by the cure.
    // x = LY_SIDEW leaves the sidebar's strip clear. Row coordinates stay PANE-
    // relative, so nothing in layout_compute or apply_filter has to know the sidebar
    // is there -- it is subtracted once, here, and once in the width it reports.
    g_pane = CreateWindowExW(WS_EX_CONTROLPARENT, L"PolShimSettingsPane", NULL,
                             WS_CHILD | WS_VISIBLE | (pg.scroll_max ? WS_VSCROLL : 0),
                             LY_SIDEW, LY_FILTERBAR, cw - LY_SIDEW, pg.pane_h, h, NULL, inst, NULL);
    if (!g_pane) { logf("[settings] pane CreateWindow failed (%lu)", GetLastError()); DestroyWindow(h); return; }
    if (pg.scroll_max) {
        SCROLLINFO si; ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin   = 0;
        si.nMax   = pg.content_h - 1;    // with nPage, this caps travel at scroll_max
        si.nPage  = (UINT)pg.pane_h;
        si.nPos   = 0;
        SetScrollInfo(g_pane, SB_VERT, &si, TRUE);
    }

    // THE CATEGORY SIDEBAR. A plain LISTBOX, deliberately: this process ships no
    // comctl32 v6 manifest (the cue-banner attempt proved that the hard way), so a
    // tab control or a treeview would be the v5 ones -- and a v5 tab strip across a
    // 700px window is eight targets a thumbstick cannot hit. A listbox is one tall
    // column of big rows, which is the shape that works on a Deck.
    //
    // Item data is the TABLE INDEX of the group row, so the selection needs no
    // parallel array to be interpreted -- it is the same identity layout_compute and
    // cat_of_row() use.
    HWND cats = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                LBS_NOTIFY | LBS_HASSTRINGS,
                                LY_PAD, LY_FILTERBAR, LY_SIDEW - LY_PAD - LY_PAD, pg.pane_h,
                                h, (HMENU)IDC_CATS, inst, NULL);
    if (cats) {
        SendMessageW(cats, WM_SETFONT, (WPARAM)g_font, TRUE);
        cats_fill(cats);
    } else {
        // Not fatal: without the sidebar the dialog is exactly what it was before it,
        // one long scrolling list. Say so rather than opening a window with a hole.
        logf("[settings] category list CreateWindow failed (%lu) -- showing everything",
             GetLastError());
        g_cat = CAT_ALL;
    }

    // Positions come from layout_compute -- the SAME function the self-test measures
    // with, so "it fits a Deck" and "this is where it draws" can never disagree.
    static LyRow lypos[LY_MAXROWS];
    layout_compute(show_dev, NULL, g_cat, lypos, (int)_countof(lypos), NULL, NULL);

    for (int i = 0; i < n; i++) {
        const ShimOption* o = &opts[i];
        if (o->type == OPT_HIDDEN) { g_ctl[i] = NULL; continue; }
        // A DEVELOPER ROW STILL GETS ITS CONTROLS. Skipping creation here is what
        // made "Show developer options" a restart-the-dialog setting: with no
        // control there was nothing for a re-layout to move. They are created at
        // whatever position layout_compute gave (y = -1 while hidden, exactly like
        // a row outside the open category) and apply_filter shows or hides them.
        const int x = LY_PAD + lypos[i].col * LY_COLW;
        const int y = lypos[i].y;
        // A HEADING, not an option: it owns no key and no control, so it must not
        // consume a g_ctl slot the save loop would then try to read. The
        // "Developer options" separator is one of these, declared in the table --
        // there is deliberately no special case for it here.
        if (o->type == OPT_GROUP) {
            g_ctl[i] = NULL;
            int hx, hy; row_origin(o, RP_HDR, x, y, &hx, &hy);
            HWND hg = CreateWindowExW(0, L"STATIC", o->label,
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      hx, hy, LY_LBLW + 8 + LY_CTLW, LY_GROUP - 10,
                                      g_pane, (HMENU)(INT_PTR)(IDC_HDR_FIRST + i), inst, NULL);
            SendMessageW(hg, WM_SETFONT, (WPARAM)g_font_hdr, TRUE);
            continue;
        }
        // An OPT_BUTTON owns no section and no key, so there is no value to read --
        // and ini_str with a NULL section does not mean "nothing" to the Win32 ini
        // API, it means "enumerate every section".
        wchar_t val[256] = L"";
        if (o->type != OPT_BUTTON) opt_value(o, val, _countof(val));

        if (o->type != OPT_BOOL && o->type != OPT_PACK && o->type != OPT_BUTTON) {
            int lx, ly; row_origin(o, RP_LABEL, x, y, &lx, &ly);
            HWND s = CreateWindowExW(0, L"STATIC", o->label,
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                     lx, ly, LY_LBLW, LY_ROW - 6, g_pane,
                                     (HMENU)(INT_PTR)(IDC_LBL_FIRST + i), inst, NULL);
            SendMessageW(s, WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        if (o->type == OPT_BUTTON) {
            // An ACTION row. It shares the option id range so apply_filter moves it
            // with everything else, and WM_COMMAND tells it apart by the table type
            // rather than by a second id range that would have to stay in step.
            int cx, cy; row_origin(o, RP_CTL, x, y, &cx, &cy);
            g_ctl[i] = CreateWindowExW(0, L"BUTTON", o->label,
                                       WS_CHILD | WS_VISIBLE,
                                       cx, cy, LY_BTNW, LY_BTNH,
                                       g_pane, (HMENU)(INT_PTR)(IDC_FIRST + i), inst, NULL);
        } else if (o->type == OPT_BOOL || o->type == OPT_PACK) {
            int cx, cy; row_origin(o, RP_CTL, x, y, &cx, &cy);
            g_ctl[i] = CreateWindowExW(0, L"BUTTON", o->label,
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       cx, cy, LY_LBLW + 8 + LY_CTLW, LY_ROW - 4,
                                       g_pane, (HMENU)(INT_PTR)(IDC_FIRST + i), inst, NULL);
            bool on = (o->type == OPT_PACK) ? pack_is_on(o) : (wcscmp(val, L"1") == 0);
            SendMessageW(g_ctl[i], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (o->type == OPT_ENUM) {
            int cx, cy; row_origin(o, RP_CTL, x, y, &cx, &cy);
            g_ctl[i] = CreateWindowExW(0, L"COMBOBOX", NULL,
                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       cx, cy, LY_CTLW, 200,
                                       g_pane, (HMENU)(INT_PTR)(IDC_FIRST + i), inst, NULL);
            wchar_t ch2[512]; wcsncpy_s(ch2, o->choices ? o->choices : L"", _TRUNCATE);
            wchar_t* ctx = NULL; int idx = 0;
            for (wchar_t* t = wcstok_s(ch2, L"|", &ctx); t; t = wcstok_s(NULL, L"|", &ctx), idx++) {
                wchar_t cv[64], cl[192];
                enum_split(t, cv, _countof(cv), cl, _countof(cl));
                // The LABEL goes in the list; the value is recovered on save by
                // splitting the same choices string again, so the control never
                // has to carry it.
                SendMessageW(g_ctl[i], CB_ADDSTRING, 0, (LPARAM)cl);
                if (!wcscmp(cv, val)) SendMessageW(g_ctl[i], CB_SETCURSEL, idx, 0);
            }
            if (SendMessageW(g_ctl[i], CB_GETCURSEL, 0, 0) == CB_ERR)
                SendMessageW(g_ctl[i], CB_SETCURSEL, 0, 0);
        } else if (o->type == OPT_SERVER) {
            // Editable combo: pick a saved server or type an address that is not saved.
            int cx, cy; row_origin(o, RP_CTL, x, y, &cx, &cy);
            g_ctl[i] = CreateWindowExW(0, L"COMBOBOX", NULL,
                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL | CBS_AUTOHSCROLL,
                                       cx, cy, LY_CTLW, 240,
                                       g_pane, (HMENU)(INT_PTR)(IDC_FIRST + i), inst, NULL);
            for (int s = 0; s < g_nsrv; s++) {
                wchar_t item[176];
                swprintf_s(item, L"%ls = %ls", g_srv_name[s], g_srv_addr[s]);
                SendMessageW(g_ctl[i], CB_ADDSTRING, 0, (LPARAM)item);
            }
            // Show the saved NAME when the current address matches one, so the field
            // says "dev = ..." rather than a bare IP you then have to recognise.
            const wchar_t* shown = val;
            wchar_t named[176];
            for (int s = 0; s < g_nsrv; s++)
                if (!_wcsicmp(val, g_srv_addr[s])) {
                    swprintf_s(named, L"%ls = %ls", g_srv_name[s], g_srv_addr[s]);
                    shown = named; break;
                }
            SetWindowTextW(g_ctl[i], shown);
        } else {
            int cx, cy; row_origin(o, RP_CTL, x, y, &cx, &cy);
            g_ctl[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", val,
                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                       cx, cy, LY_CTLW, LY_ROW - 4,
                                       g_pane, (HMENU)(INT_PTR)(IDC_FIRST + i), inst, NULL);
        }
        SendMessageW(g_ctl[i], WM_SETFONT, (WPARAM)g_font, TRUE);

        // THE HINT: what this is for, in the user's terms. Its own control id range
        // so WM_CTLCOLORSTATIC can grey it -- the point is that it reads as
        // explanation, not as another label competing with the option's own.
        if (o->hint || o->per_title) {
            int nx, ny; row_origin(o, RP_HINT, x, y, &nx, &ny);
            // PROVENANCE. The recorded failure this answers is "the setting
            // does nothing" -- which was never really about the setting, but
            // about not knowing who had already decided. A per-title row
            // states its source, resolved against the title ACTUALLY RUNNING,
            // so the same row reads differently at the Viewer menu and inside
            // a game. That difference is the truth, and it was invisible.
            wchar_t hint[512];
            if (o->per_title) {
                wchar_t cur[256]; ValueSource vs = VS_DEFAULT;
                opt_value_for(o, title_current(), cur, _countof(cur), &vs);
                const char* tl = title_current();
                wchar_t wl[80] = L"";
                if (tl && *tl)
                    MultiByteToWideChar(CP_ACP, 0, tl, -1, wl, _countof(wl));
                _snwprintf_s(hint, _countof(hint), _TRUNCATE,
                             L"%ls%ls[per-game] now: %ls \x2014 %ls%ls%ls",
                             o->hint ? o->hint : L"",
                             o->hint ? L"  \x00b7  " : L"",
                             cur, value_source_text(vs),
                             wl[0] ? L", for " : L"", wl[0] ? wl : L"");
            } else {
                wcsncpy_s(hint, o->hint, _TRUNCATE);
            }
            // SS_ENDELLIPSIS: a hint is one line by construction (LY_HINT), and
            // without this a long one stops mid-word -- which reads as the text
            // being broken rather than merely long.
            HWND hh = CreateWindowExW(0, L"STATIC", hint,
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                      nx, ny, LY_LBLW + 8 + LY_CTLW - 16, LY_HINT,
                                      g_pane, (HMENU)(INT_PTR)(IDC_HINT_FIRST + i), inst, NULL);
            SendMessageW(hh, WM_SETFONT, (WPARAM)g_font_hint, TRUE);
        }
    }

    // THE FILTER BOX. A child of the WINDOW, not the pane, so it cannot scroll away
    // from the rows it is filtering -- the same reason Save is not in the pane.
    //
    // A REAL LABEL, not a cue banner. EM_SETCUEBANNER was tried first and did
    // nothing: it is a comctl32 v6 message, this process ships no such manifest, and
    // an unrecognised message is simply ignored -- so the dialog opened with an
    // unexplained empty box across its top. Seen in a screenshot; no assertion here
    // could have caught it, which is the argument for taking the screenshot.
    const int flbl = 46;
    HWND fl = CreateWindowExW(0, L"STATIC", L"Search", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              LY_PAD, 8, flbl, LY_ROW - 6, h, NULL, inst, NULL);
    SendMessageW(fl, WM_SETFONT, (WPARAM)g_font, TRUE);
    HWND fb = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              LY_PAD + flbl + 6, 4, cw - 2 * LY_PAD - flbl - 6,
                              LY_FILTERBAR - 8, h, (HMENU)IDC_FILTER, inst, NULL);
    SendMessageW(fb, WM_SETFONT, (WPARAM)g_font, TRUE);

    // The action rows sit under the TALLER column, which is what ch was sized for.
    int by = LY_FILTERBAR + ch - LY_CHROME + 12;

    // "Repair games now" gets its own row: it is an ACTION, not a setting, and it must
    // not read as something Save applies.
    HWND br = CreateWindowExW(0, L"BUTTON", L"Repair game registration...",
                              WS_CHILD | WS_VISIBLE, LY_PAD, by, 185, 26, h, (HMENU)IDC_REPAIR, inst, NULL);
    SendMessageW(br, WM_SETFONT, (WPARAM)g_font, TRUE);
    // THE ONE-CLICK ANSWER TO "the games are broken and I do not know which knob it
    // was". Beside the registration repair because the two are the same gesture from
    // the user's side -- something is wrong with a game, put it right -- and because
    // an install carrying an old default is exactly as invisible as a missing
    // registry key was. iniheal.cpp's inimigrate() does this automatically, once;
    // this is the button for when it has already run and something is still off.
    HWND bf = CreateWindowExW(0, L"BUTTON", L"Put the game fixes back...",
                              WS_CHILD | WS_VISIBLE, LY_PAD + 193, by, 185, 26, h, (HMENU)IDC_RESETFIX, inst, NULL);
    SendMessageW(bf, WM_SETFONT, (WPARAM)g_font, TRUE);
    // The controller mapper beside it. This used to be a read-only "A/B diagnostics"
    // dump, which told you what was wrong and left you to fix it by hand in an ini --
    // the reason the A/B map took three rounds to get right. It is now the editor.
    // "Controller mapping...", "Game settings...", "Save server as...",
    // "Forget server" and "FFXI add-ons..." are NOT here any more. They are
    // OPT_BUTTON rows inside the categories they belong to, exactly as
    // "Import FFXI character..." was moved out of this strip before them: an
    // action that only makes sense inside one category is easier to find there
    // than in a bar of general tools, and the bar was three rows deep.

    // The build number is the whole point of this line: it increments on every publish,
    // so it answers "did my update land?" at a glance.
    wchar_t ver[160];
    swprintf_s(ver, L"v%hs  build %d  (%hs)", POLSHIM_VERSION, POLSHIM_BUILD, POLSHIM_BUILD_DATE);


    // NO SECOND ACTION ROW. The saved-server buttons live in "Which server this
    // copy talks to" now, beside the address field they act on, and the Ashita
    // manager under "Final Fantasy XI add-ons" with the rest of its group.
    //
    // Build number, right-aligned on the remaining row: it is the "did my update
    // land?" readout, so it has to stay always visible.
    HWND vs = CreateWindowExW(0, L"STATIC", ver, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                              cw - LY_PAD - 240, by + 6, 240, 18, h, NULL, inst, NULL);
    SendMessageW(vs, WM_SETFONT, (WPARAM)g_font, TRUE);

    // NO THIRD ACTION ROW. "Import FFXI character..." used to sit here on its own,
    // which put an FFXI-only action in the strip of general Viewer tools and as far
    // as this window can get from the add-on rows it belongs with. It is an
    // OPT_BUTTON row under "Final Fantasy XI add-ons" now, beside its export half.
    by += 34;
    // WAS "Changes apply next time the Viewer starts." -- untrue since Save began
    // applying live, and the kind of stale reassurance that teaches people to
    // restart for nothing. It is the SAVE STATUS line now (IDC_STATUS): Save writes
    // its result here instead of into a modal box nobody can act on.
    HWND note = CreateWindowExW(0, L"STATIC", L"",
                                WS_CHILD | WS_VISIBLE, LY_PAD, by + 8, LY_LBLW + 260, 18,
                                h, (HMENU)IDC_STATUS, inst, NULL);
    SendMessageW(note, WM_SETFONT, (WPARAM)g_font, TRUE);
    HWND bs = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              cw - LY_PAD - 170, by + 4, 80, 26, h, (HMENU)IDC_SAVE, inst, NULL);
    // "Close", not "Cancel": Save no longer closes, so this button no longer
    // means "discard" -- it means "I am done". Calling it Cancel next to a Save
    // that leaves the window open reads as "undo what I just saved".
    HWND bc = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                              cw - LY_PAD - 84, by + 4, 80, 26, h, (HMENU)IDC_CANCEL, inst, NULL);
    SendMessageW(bs, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(bc, WM_SETFONT, (WPARAM)g_font, TRUE);

    // POSITION AND HIDE EVERYTHING BEFORE THE WINDOW IS SEEN.
    //
    // The creation pass above builds a control for EVERY drawable row, because Save
    // walks the whole table and apply_filter moves controls rather than rebuilding
    // them -- a row outside the open category still has to exist. It creates them at
    // the coordinates layout_compute gave, and for a row the category excludes that
    // is y = -1, so they all landed in a heap over the first real row: the dialog
    // opened with two categories' text drawn on top of each other. (Invisible until
    // categories existed, since before them every row was always drawn.)
    //
    // One call to the function that already knows the rules fixes it, rather than a
    // second copy of them in the creation loop -- which is the drift that made
    // row_origin() one function in the first place.
    apply_filter(h, show_dev);
    // ...and grey out whichever mode-specific actions do not apply, BEFORE the
    // window is shown -- a button that starts enabled and disables itself a frame
    // later is a flicker nobody can explain.
    apply_mode_gates();

    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    logf("[settings] opened (%d options) -- if you cannot SEE it, the shell is holding "
         "the display exclusively; try [dx] d3d_windowed=1", nvis);

    MSG m;
    HWND lastfocus = NULL;
    while (IsWindow(h) && GetMessageW(&m, NULL, 0, 0) > 0) {
        // ESC cancels the settings dialog too -- same reason as the report window: a
        // plain window gets no free ESC handling, and on a pad the title-bar X is not
        // a realistic target.
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { DestroyWindow(h); break; }
        // THE WHEEL GOES TO THE FOCUSED CONTROL, which is normally a checkbox that
        // does nothing with it and does not pass it on. Redirect it to the thing
        // that scrolls, or the wheel does nothing over most of the dialog.
        if (m.message == WM_MOUSEWHEEL && g_pane) {
            SendMessageW(g_pane, WM_MOUSEWHEEL, m.wParam, m.lParam);
        } else if (!IsDialogMessageW(h, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        HWND f = GetFocus();
        if (f != lastfocus) { lastfocus = f; pane_reveal(f); }
    }
    g_pane = NULL;                       // destroyed with its parent
    logf("[settings] closed");
}

// REFUSE to open over a title that is holding the display EXCLUSIVELY.
//
// Measured on a real Steam Deck, 2026-08-15: pressing the chord inside FFXI
// CRASHED the game. FFXI holds an exclusive fullscreen Direct3D device; creating
// and activating a top-level window takes the display away from it, the device is
// lost, and the title dies. The dialog did what it was told and killed the game
// doing it.
//
// The KNOWN LIMIT note at the top of this file had the symptom too small. It said
// the dialog might be INVISIBLE over an exclusive surface. It is worse than that:
// it takes the title down with it.
//
// This is a GUARD, not a fix, and it is deliberately not dressed up as one. There
// is no way to draw a Win32 dialog over an exclusive fullscreen device, so the
// honest behaviour is to do nothing and say so. Note that d3d_windowed=1 is NOT
// the workaround here the way the old note implies: for FFXI specifically it
// black-screens the title (measured 2026-08-15). Open the dialog from
// the Viewer shell instead, which is windowed.
//
// [settings] fullscreen_ok=1 restores the old behaviour for anyone who wants to
// take the risk on a title that survives it.
static bool exclusive_display_blocks_us()
{
    if (g_fullscreen_ok) return false;
    if (!d3d_exclusive_fullscreen()) return false;
    logf("[settings] chord IGNORED -- a title is holding the display EXCLUSIVELY "
         "(fullscreen device). Opening a window now takes the display from it and "
         "the title crashes (measured with FFXI on a Steam Deck, 2026-08-15). "
         "Close the title and use the chord in the Viewer, or set "
         "[settings] fullscreen_ok=1 to try anyway.");
    return true;
}

void polsettings_open()
{
    if (InterlockedCompareExchange(&g_open, 1, 0) != 0) return;   // already up
    // Checked INSIDE the once-guard, so a refusal cannot be logged while a dialog
    // is already up (polctl can call this too, not just the edge-triggered chord).
    if (exclusive_display_blocks_us()) { InterlockedExchange(&g_open, 0); return; }
    HANDLE dpi_prev = ui_dpi_fit_begin();   // see the HiDPI-fit banner above padmap
    build_and_pump();
    ui_dpi_fit_end(dpi_prev);
    InterlockedExchange(&g_open, 0);
}

// ---------------------------------------------------------------- watcher

// ANY armed chord in the list fires -- they are alternatives, not a sequence. Shared by
// the settings chord and the mapper's, so a second chord cannot drift from the first.
static bool kbd_chord_list_down(const UINT* mods, const UINT* vks, int count)
{
    for (int i = 0; i < count; i++) {
        if ((mods[i] & 1) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) continue;
        if ((mods[i] & 2) && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) continue;
        if ((mods[i] & 4) && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) continue;
        if (GetAsyncKeyState((int)vks[i]) & 0x8000) return true;
    }
    return false;
}

// GetAsyncKeyState is SYSTEM-WIDE: without this, Ctrl+Shift+R pressed in a web
// browser while the Viewer runs behind it would open a report box and screenshot
// the browser. The report key only counts while one of OUR windows is in front.
static bool foreground_is_ours()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool kbd_chord_down()
{
    return kbd_chord_list_down(g_hk_mods, g_hk_vk, g_hk_count);
}

// Render a button mask as the same names pad_chord= accepts, so the debug log can be
// pasted straight back into the ini.
static void pad_names(WORD m, char* out, size_t cch)
{
    out[0] = 0;
    if (!m) { strncpy_s(out, cch, "(none)", _TRUNCATE); return; }
    for (int i = 0; i < _countof(g_padnames); i++) {
        if (!wcscmp(g_padnames[i].name, L"select")) continue;   // alias of back; do not print twice
        if ((m & g_padnames[i].bit) == 0) continue;
        char n[16]; size_t got = 0;
        wcstombs_s(&got, n, sizeof(n), g_padnames[i].name, _TRUNCATE);
        if (out[0]) strncat_s(out, cch, "+", _TRUNCATE);
        strncat_s(out, cch, n, _TRUNCATE);
    }
    if (!out[0]) strncpy_s(out, cch, "(unnamed bits)", _TRUNCATE);
}

// Two chords ride this one XInput poll: the settings dialog's, and the on-surface
// controller mapper's. `mask` picks which. XInput is used for BOTH deliberately -- its
// button names are fixed (`back`, `start`, `lb`), so a chord means the same thing on
// every pad even while the DirectInput indices underneath are the very thing being
// remapped. A chord defined in the index space the mapper edits could be edited away.
static bool pad_mask_down(WORD mask)
{
    if (!g_xiget || !mask) return false;
    bool hit = false;
    for (DWORD i = 0; i < 4; i++) {
        XI_STATE st; ZeroMemory(&st, sizeof(st));
        if (g_xiget(i, &st) != 0) continue;
        if ((st.Gamepad.wButtons & mask) == mask) hit = true;
    }
    return hit;
}

static bool pad_chord_down()
{
    if (!g_xiget) return false;
    bool connected = false, hit = false;
    for (DWORD i = 0; i < 4; i++) {
        XI_STATE st; ZeroMemory(&st, sizeof(st));
        if (g_xiget(i, &st) != 0) continue;              // 0 == ERROR_SUCCESS
        connected = true;
        // pad_debug: report what we actually SEE, whenever it changes. This is the
        // difference between "you pressed the wrong buttons" and "Steam Input is not
        // giving this process an XInput pad at all", which look identical otherwise.
        if (g_pad_debug) {
            static WORD last[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
            if (st.Gamepad.wButtons != last[i]) {
                last[i] = st.Gamepad.wButtons;
                char n[128]; pad_names(st.Gamepad.wButtons, n, sizeof(n));
                logf("[settings] pad%lu buttons=0x%04X %s", i, st.Gamepad.wButtons, n);
            }
        }
        if (g_pad_mask && (st.Gamepad.wButtons & g_pad_mask) == g_pad_mask) hit = true;
    }
    if (g_pad_debug && !connected) {
        static int said = 0;
        if (!said) { said = 1; logf("[settings] pad_debug: XInput resolved but NO controller "
                                    "on slots 0-3 -- Steam Input is probably giving this game a "
                                    "keyboard/mouse layout instead of an emulated pad"); }
    }
    return hit;
}

static DWORD WINAPI watcher(LPVOID)
{
    xinput_resolve();
    bool was = false, was_ui = false, was_rp = false, was_dp = false;
    for (;;) {
        // The MAPPER chord is tested first and, when it fires, consumes the tick. The
        // two chords share the `back` button, so a settings chord pressed a frame later
        // than the mapper's would otherwise open a dialog behind an overlay that is
        // already swallowing the pad -- a state with no way out from a controller.
        bool ui = pad_mask_down(g_padui_mask) || kbd_chord_list_down(g_ui_hk_mods, g_ui_hk_vk,
                                                                    g_ui_hk_count);
        if (ui && !was_ui) padoverlay_toggle();
        was_ui = ui;

        // THE REPORT KEY. It consumes the tick like the mapper does: `back+rb` shares
        // `back` with the settings chord's `back+start`, and a player rolling a thumb
        // across both would otherwise get the settings dialog stacked behind the
        // report box. Tested before settings because it is the one pressed in a hurry.
        bool rp = !ui && foreground_is_ours() && polreport_armed() &&
                  (kbd_chord_list_down(g_rp_hk_mods, g_rp_hk_vk, g_rp_hk_count) ||
                   pad_mask_down(g_rp_pad_mask));
        if (rp && !was_rp && !padoverlay_active()) polreport_open();   // edge-triggered
        was_rp = rp;

        // THE DISPLAY SWITCH, edge-triggered like the others. Alt+Enter shares
        // nothing with the chords above, so it does not consume the tick.
        bool dp = g_dp_hk_count > 0 &&
                  kbd_chord_list_down(g_dp_hk_mods, g_dp_hk_vk, g_dp_hk_count);
        if (dp && !was_dp && !ui && !rp && !padoverlay_active()) {
            char why[200] = "";
            if (d3d_display_toggle(why, sizeof(why)))
                logf("[display] hotkey: %s", why);
            else
                logf("[display] hotkey: not switched -- %s", why);
        }
        was_dp = dp;

        bool now = !ui && !rp && (kbd_chord_down() || pad_chord_down());
        // Never open the dialog on top of the overlay: the overlay owns the pad, so the
        // dialog behind it could be neither seen nor dismissed.
        if (now && !was && !padoverlay_active()) polsettings_open();   // edge-triggered
        was = now;
        Sleep(60);
    }
}

// ---------------------------------------------------------------- self-test
//
// Chord parsing and the ini round-trip are the two things that can silently arm a
// chord nobody can press or write a value nobody asked for, and neither needs a live
// Viewer to check. polsettingstest.exe drives this; --show adds the visual pass.

static int st_fail = 0;
static void st_check(const char* what, bool ok)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) st_fail++;
}

int polsettings_selftest()
{
    UINT m = 0, vk = 0;
    st_check("ctrl+shift+s parses",        parse_hotkey(L"ctrl+shift+s", &m, &vk) && m == 3 && vk == 'S');
    st_check("case/space insensitive",     parse_hotkey(L"  CTRL + Shift+S ", &m, &vk) && m == 3 && vk == 'S');
    st_check("alt+f12 parses (named vk)",  parse_hotkey(L"alt+f12", &m, &vk) && m == 4 && vk == VK_F12);
    // Non-alphanumeric names: before these were in the table the spec was REJECTED and
    // the default armed instead, so the ini said one chord and the shim watched another.
    st_check("shift+tab parses",           parse_hotkey(L"shift+tab", &m, &vk) && m == 2 && vk == VK_TAB);
    st_check("ctrl+space parses",          parse_hotkey(L"ctrl+space", &m, &vk) && m == 1 && vk == VK_SPACE);

    // The hotkey LIST. The property that matters is that one bad entry costs you only
    // that entry -- the whole point of shipping two defaults is that a user editing one
    // of them cannot lock themselves out of the dialog.
    {
        UINT lm[HK_MAX], lv[HK_MAX]; wchar_t bad[128];
        int n = parse_hotkey_list(POLSHIM_DEFAULT_HOTKEY, lm, lv, HK_MAX, bad, _countof(bad));
        st_check("default arms two chords",   n == 2 && !bad[0]);
        st_check("  #1 is bare home",         lm[0] == 0 && lv[0] == VK_HOME);
        st_check("  #2 is ctrl+shift+s",      lm[1] == 3 && lv[1] == 'S');

        n = parse_hotkey_list(L"home , ctrl+notakey ,shift+tab", lm, lv, HK_MAX, bad, _countof(bad));
        st_check("bad entry dropped, rest kept", n == 2 && lv[0] == VK_HOME && lv[1] == VK_TAB);
        st_check("bad entry is reported",     wcsstr(bad, L"ctrl+notakey") != NULL);

        n = parse_hotkey_list(L"  ", lm, lv, HK_MAX, bad, _countof(bad));
        st_check("empty list arms nothing",   n == 0);
        n = parse_hotkey_list(L"ctrl+shift+s", lm, lv, HK_MAX, bad, _countof(bad));
        st_check("single chord still works",  n == 1 && lm[0] == 3 && lv[0] == 'S');
    }
    st_check("bare key, no modifier",      parse_hotkey(L"pause", &m, &vk) && m == 0 && vk == VK_PAUSE);
    st_check("digit key",                  parse_hotkey(L"ctrl+7", &m, &vk) && m == 1 && vk == '7');
    st_check("modifiers only -> reject",   !parse_hotkey(L"ctrl+shift", &m, &vk));
    st_check("garbage -> reject",          !parse_hotkey(L"ctrl+notakey", &m, &vk));
    st_check("empty -> reject",            !parse_hotkey(L"", &m, &vk));

    // The mix-up that cost a Deck session: `back` is the View/Select button (0x0020),
    // NOT the B face button (0x2000). Pin both so they can never be conflated again.
    st_check("back is View/Select 0x0020, not B", parse_pad(L"back") == 0x0020);
    st_check("b is the face button 0x2000",       parse_pad(L"b") == 0x2000);
    st_check("b+start != back+start",             parse_pad(L"b+start") != parse_pad(L"back+start"));

    char nm[128];
    pad_names(0x0030, nm, sizeof(nm));
    st_check("pad_names(0x0030) round-trips to back+start", !strcmp(nm, "back+start"));
    pad_names(0x2010, nm, sizeof(nm));
    st_check("pad_names(0x2010) reports start+b",           !strcmp(nm, "start+b"));
    pad_names(0, nm, sizeof(nm));
    st_check("pad_names(0) is (none)",                      !strcmp(nm, "(none)"));

    st_check("back+start -> 0x0030",       parse_pad(L"back+start") == 0x0030);
    st_check("select aliases back",        parse_pad(L"select+start") == 0x0030);
    st_check("l3+r3 -> 0x00C0",            parse_pad(L"l3+r3") == 0x00C0);
    st_check("unknown token ignored",      parse_pad(L"back+nope+start") == 0x0030);
    st_check("empty -> 0 (chord disabled)", parse_pad(L"") == 0);

    // Round-trip: every option must read back what the table says by default, and a
    // written value must win over it. This is the contract the dialog relies on.
    wchar_t tmp[MAX_PATH], dir[MAX_PATH];
    GetTempPathW(_countof(dir), dir);
    swprintf_s(tmp, L"%spolshim-selftest-%lu.ini", dir, GetCurrentProcessId());
    DeleteFileW(tmp);
    wcsncpy_s(g_ini, tmp, _TRUNCATE);

    int n = 0; const ShimOption* opts = shim_options(&n);
    st_check("option table is non-empty", n > 0);

    // KEYED rows are the ones with a section and a key. Headings and fix-packs have
    // neither, so every loop below has to say which kind it means -- which is why
    // this is written out rather than folded together.
    #define ST_KEYED(o) ((o).sec && (o).key && (o).key[0])

    bool keyshape_ok = true;
    for (int i = 0; i < n; i++) {
        const ShimOption& o = opts[i];
        if (o.type == OPT_GROUP || o.type == OPT_PACK || o.type == OPT_BUTTON) {
            // Must own NO key: iniheal skips them on exactly that test, so a key
            // here would be written into the ini as a phantom option.
            if (o.key && o.key[0]) keyshape_ok = false;
            if (!o.label || !o.label[0]) keyshape_ok = false;
        } else if (!ST_KEYED(o)) keyshape_ok = false;
    }
    st_check("headings and packs own no key; every other row does", keyshape_ok);

    // The control array must cover the whole table, or build_and_pump clamps `n` and
    // the last rows are neither drawn nor saved -- silently. This actually happened:
    // the table outgrew a 64-slot array at 81 rows.
    printf("  [table] %d rows, %d control slots, layout capacity %d\n",
           n, (int)_countof(g_ctl), (int)LY_MAXROWS);
    st_check("every table row has a control slot", n <= (int)_countof(g_ctl));
    st_check("every table row has a layout slot",  n <= (int)LY_MAXROWS);

    // THE DIALOG MUST FIT THE SCREEN IT IS USED ON. A Steam Deck is 1280x800 and
    // this window does not scroll, so a table that outgrows it pushes Save off the
    // bottom on the one device with no keyboard to tab there with. 760 leaves room
    // for the title bar and Game Mode's own furniture.
    {
        int cw = 0, chh = 0, cwd = 0, chd = 0;
        settings_dialog_size(false, &cw, &chh);
        settings_dialog_size(true,  &cwd, &chd);
        printf("  [layout] %dx%d normally, %dx%d with developer options\n", cw, chh, cwd, chd);

        // WIDTH still has to fit outright: there is no horizontal scrolling, and a
        // second column pushed off the right edge would be unreachable.
        st_check("dialog fits a Steam Deck's WIDTH (<=1280)", cw <= 1280 && cwd <= 1280);

        // HEIGHT IS NO LONGER A BUDGET. The old check asserted the table fit 760px
        // and FAILED at 792 -- and the failure was real: build_and_pump clamped the
        // window and the rows past the cut were drawn off-screen, unreachable. On the
        // machine that reported it there were only 672px of work area, so that was
        // most of a column. The options scroll now, so the two things worth asserting
        // are that the window fits and that every row can be reached.
        const struct { const char* name; int avail; } screens[] = {
            { "Steam Deck (800-48)",   752 },
            { "the 672px report",      672 },
            { "1080p (1080-48)",      1032 },
        };
        bool fits = true, reachable = true;
        for (int s = 0; s < (int)_countof(screens); s++) {
            for (int dev = 0; dev < 2; dev++) {
                PaneGeom g = pane_geometry(dev ? chd : chh, screens[s].avail);
                if (g.win_h > screens[s].avail)          fits = false;
                if (g.pane_h + g.scroll_max < g.content_h) reachable = false;
                printf("  [pane] %-20s dev=%d  window %4d  pane %4d  content %4d  scroll %4d\n",
                       screens[s].name, dev, g.win_h, g.pane_h, g.content_h, g.scroll_max);
            }
        }
        st_check("the window fits every screen we ship to",    fits);
        st_check("every option row is reachable by scrolling", reachable);
        // Save and Cancel are children of the WINDOW, below the pane. This is the
        // arithmetic that says they are on screen: the chrome always gets its full
        // height, and the pane takes what is left rather than the other way round.
        PaneGeom tiny = pane_geometry(chd, 320);
        st_check("the chrome keeps its full height on a 320px screen",
                 tiny.win_h - tiny.pane_h == LY_CHROME);
    }

    // THE FILTER. Three properties, and the third is the one that makes it usable:
    // a filter that leaves six empty group headings behind has not narrowed anything.
    {
        st_check("filter: empty matches everything",
                 row_matches(opts[0], L"") && row_matches(opts[0], NULL));

        // Find the FFXI add-on rows by their section, then prove the filter reaches
        // them by the three names a user might type: the label, the ini key, and the
        // section. (Typing an ini key comes straight from reading a log or a note.)
        const ShimOption* addons = NULL;
        for (int i = 0; i < n; i++)
            if (ST_KEYED(opts[i]) && !wcscmp(opts[i].sec, L"ffxi")
                                  && !wcscmp(opts[i].key, L"addons")) addons = &opts[i];
        st_check("the FFXI add-on row exists", addons != NULL);
        if (addons) {
            st_check("filter matches a row by its label",   row_matches(*addons, L"ashita"));
            st_check("filter matches a row by its ini key", row_matches(*addons, L"addons"));
            st_check("filter matches a row by its section", row_matches(*addons, L"ffxi"));
            st_check("filter terms are ANDed",              row_matches(*addons, L"ffxi ashita"));
            st_check("a term that misses excludes the row", !row_matches(*addons, L"ffxi zzznope"));
            st_check("filter is case-insensitive",          row_matches(*addons, L"ASHITA"));
        }

        // And the layout under a filter: fewer rows, no orphaned headings, and every
        // row the layout skipped marked as skipped rather than left at a stale y.
        static LyRow all[LY_MAXROWS], few[LY_MAXROWS];
        int na = layout_compute(false, NULL,      CAT_ALL, all, (int)_countof(all), NULL, NULL);
        int nf = layout_compute(false, L"ashita", CAT_ALL, few, (int)_countof(few), NULL, NULL);
        int drawn_all = 0, drawn_few = 0, orphan = 0;
        for (int i = 0; i < na; i++) if (all[i].y >= 0) drawn_all++;
        for (int i = 0; i < nf; i++) {
            if (few[i].y < 0) continue;
            drawn_few++;
            if (opts[i].type != OPT_GROUP) continue;
            bool any = false;
            for (int j = i + 1; j < nf && opts[j].type != OPT_GROUP; j++)
                if (few[j].y >= 0) any = true;
            if (!any) orphan++;
        }
        // COUNT THE HEADINGS, not just the rows. The bug that made this necessary
        // dropped every heading and showed up only as a suspiciously narrow window.
        int hdr_all = 0, hdr_few = 0, groups = 0;
        for (int i = 0; i < na; i++) {
            if (opts[i].type != OPT_GROUP || opts[i].dev) continue;
            groups++;
            if (all[i].y >= 0) hdr_all++;
        }
        for (int i = 0; i < nf; i++)
            if (opts[i].type == OPT_GROUP && few[i].y >= 0) hdr_few++;
        printf("  [filter] %d rows drawn unfiltered, %d for 'ashita'\n", drawn_all, drawn_few);
        printf("  [filter] %d headings drawn unfiltered (%d in the table), %d for 'ashita'\n",
               hdr_all, groups, hdr_few);
        st_check("every non-dev heading is drawn when nothing is filtered", hdr_all == groups && groups > 1);
        st_check("a filter keeps the heading of what it matched",           hdr_few >= 1 && hdr_few < hdr_all);
        // The window is sized to the tallest CATEGORY now, so the old "it must use
        // both columns" assertion is gone with the columns. What replaces it is the
        // property that actually matters: no category may be taller than the window,
        // because a category that needs a scrollbar is a category that should have
        // been split -- and the sidebar exists precisely so nothing has to scroll.
        int cw2 = 0, ch2 = 0;
        settings_dialog_size(false, &cw2, &ch2);
        int worst = 0, worst_cat = -1, ncat = 0;
        for (int i = 0; i < n; i++) {
            if (opts[i].type != OPT_GROUP || opts[i].dev) continue;
            ncat++;
            int w = 0, hh = 0;
            layout_compute(false, NULL, i, all, (int)_countof(all), &w, &hh);
            if (hh > worst) { worst = hh; worst_cat = i; }
        }
        printf("  [layout] window %dx%d, %d categories, tallest is '%ls' at %dpx\n",
               cw2, ch2, ncat, worst_cat >= 0 ? cat_name(opts[worst_cat]) : L"?", worst);
        st_check("more than one category to choose from", ncat > 1);
        st_check("the window fits its tallest category",  ch2 >= worst);
        // The whole point of the makeover: half the width the two-column window was.
        st_check("the window is one column wide",         cw2 <= LY_SIDEW + LY_PAD + LY_COLW + LY_PAD);
        // A picked category draws its own rows and NOTHING from its neighbours.
        if (worst_cat >= 0) {
            int nc = layout_compute(false, NULL, worst_cat, few, (int)_countof(few), NULL, NULL);
            int leaked = 0, drawn_c = 0;
            for (int i = 0; i < nc; i++) {
                if (few[i].y < 0) continue;
                drawn_c++;
                if (cat_of_row(opts, i) != worst_cat) leaked++;
            }
            st_check("a category draws only its own rows", drawn_c > 0 && leaked == 0);
        }
        st_check("a filter draws strictly fewer rows",       drawn_few > 0 && drawn_few < drawn_all);
        st_check("no heading is left with nothing under it", orphan == 0);

        // A filter that matches nothing must draw NOTHING -- not one lonely heading.
        int nz = layout_compute(false, L"zzz-no-such-option", CAT_ALL, few, (int)_countof(few), NULL, NULL);
        int drawn_z = 0;
        for (int i = 0; i < nz; i++) if (few[i].y >= 0) drawn_z++;
        st_check("a filter that matches nothing draws nothing", drawn_z == 0);
    }

    bool defaults_ok = true, dup_ok = true;
    for (int i = 0; i < n; i++) {
        if (!ST_KEYED(opts[i])) continue;
        wchar_t v[256];
        if (wcscmp(opt_value(&opts[i], v, _countof(v)), opts[i].def) != 0) defaults_ok = false;
        if (opts[i].type == OPT_ENUM && !opts[i].choices) defaults_ok = false;
        for (int j = i + 1; j < n; j++) {
            if (!ST_KEYED(opts[j])) continue;
            if (!wcscmp(opts[i].sec, opts[j].sec) && !wcscmp(opts[i].key, opts[j].key)) dup_ok = false;
        }
    }
    st_check("absent ini -> every option reads its default", defaults_ok);

    // THE DROPDOWN AND THE PROFILE TABLE ARE THE SAME FACT, WRITTEN TWICE.
    // The dropdown's choices name the titles that can be put back to exclusive
    // fullscreen, and profiles.cpp decides which titles those are. Flag a title
    // there and forget this row and it is simply missing from the dialog -- with no
    // symptom, because a dropdown that does not offer a choice looks exactly like a
    // dropdown that has no more choices to offer. This is the check that turns that
    // silence into a failure.
    //
    // IMPORTANT: INVERTED 2026-08-24, with the setting it guards. It used to pin
    // d3d_windowed_force ("ignore this title's fullscreen profile") to the
    // PW_FULLSCREEN rows. Windowed is the default now and NO row is PW_FULLSCREEN, so
    // that dropdown has nothing to offer and is hidden; the live control is its
    // opposite, d3d_windowed_except ("leave this title fullscreen"), and the rows it
    // must offer are the ones flagged fs_fallback -- titles with a recorded history of
    // needing an exclusive device. Same invariant, both ends moved.
    {
        // 2026-09-08: the "Titles left fullscreen" dropdown is hidden; the way back
        // to an exclusive device is now the per-title Display row, so THAT is what
        // must exist, be per-title, and offer "fullscreen" -- checked right after
        // this block. The except-list check below is kept against the hidden row.
        const ShimOption* row = NULL;
        for (int i = 0; i < n; i++)
            if (opts[i].key && !wcscmp(opts[i].key, L"d3d_windowed_except")) { row = &opts[i]; break; }
        // WARNING: Compare whole VALUES, not substrings of the choices blob. The first
        // version of this check used wcsstr, and a sabotage run proved it could
        // not fail: deleting the "Final Fantasy XI" entry outright still passed,
        // because "FFXiMain.dll" survives inside the combined
        // "FrontMissionOnline.dll,FFXiMain.dll" entry. A title has to be
        // selectable ON ITS OWN, and only splitting the fields can say that.
        bool covered = row && row->choices;
        int expect = 0;
        for (int i = 0; covered && i < profiles_count(); i++) {
            const TitleProfile* p = profiles_at(i);
            if (!p || !p->fs_fallback) continue;
            expect++;
            wchar_t want[128];
            MultiByteToWideChar(CP_ACP, 0, p->module, -1, want, _countof(want));
            wchar_t ch[512]; wcsncpy_s(ch, row->choices, _TRUNCATE);
            wchar_t* ctx = NULL; bool own_entry = false;
            for (wchar_t* t = wcstok_s(ch, L"|", &ctx); t && !own_entry;
                 t = wcstok_s(NULL, L"|", &ctx)) {
                wchar_t cv[128], cl[192];
                enum_split(t, cv, _countof(cv), cl, _countof(cl));
                if (!wcscmp(cv, want)) own_entry = true;
            }
            if (!own_entry) covered = false;
        }
        // ...and nothing the table does NOT profile, or the dialog offers a name
        // that can only ever be a no-op.
        if (covered && expect == 0) covered = false;
        st_check("d3d_windowed_except offers every fs_fallback title (and the "
                 "profile table has some)", covered);
    }
    {
        const ShimOption* row = NULL;
        for (int i = 0; i < n; i++)
            if (opts[i].key && !wcscmp(opts[i].key, L"display")) { row = &opts[i]; break; }
        bool ok = row && row->per_title && row->type == OPT_ENUM && row->choices;
        bool has_w = false, has_b = false, has_f = false;
        if (ok) {
            wchar_t ch[512]; wcsncpy_s(ch, row->choices, _TRUNCATE);
            wchar_t* ctx = NULL;
            for (wchar_t* t = wcstok_s(ch, L"|", &ctx); t; t = wcstok_s(NULL, L"|", &ctx)) {
                wchar_t cv[128], cl[192];
                enum_split(t, cv, _countof(cv), cl, _countof(cl));
                if (!wcscmp(cv, L"windowed"))   has_w = true;
                if (!wcscmp(cv, L"borderless")) has_b = true;
                if (!wcscmp(cv, L"fullscreen")) has_f = true;
            }
        }
        st_check("the Display row exists, is per-title, and offers windowed / borderless / "
                 "fullscreen", ok && has_w && has_b && has_f);
    }

    // NO ROW MAY GO BACK TO PW_FULLSCREEN WITHOUT ALSO BEING OFFERED A WAY OUT.
    //
    // PW_FULLSCREEN outranks the global d3d_windowed -- that is the whole "profile as
    // authority" design, and it is how "Run games in a window" silently did nothing
    // for FMO and FFXI for three days. Windowed is the default now, so a future row
    // that sets PW_FULLSCREEN is re-creating exactly that trap. It is a legitimate
    // thing to do, but only if the same row is fs_fallback, i.e. reachable from the
    // dropdown; otherwise the title is pinned fullscreen with no supported way back.
    {
        bool ok = true;
        for (int i = 0; i < profiles_count(); i++) {
            const TitleProfile* p = profiles_at(i);
            if (p && p->window == PW_FULLSCREEN && !p->fs_fallback) ok = false;
        }
        st_check("a PW_FULLSCREEN profile is always fs_fallback too (there is a way "
                 "back to windowed)", ok);
    }
    st_check("no duplicate (section,key) rows",              dup_ok);

    // An OPT_ENUM default must actually be one of its own choices, or the combo opens
    // on a value the ini never held and Save silently rewrites it. Choices may now
    // read "value=Label", so this compares the VALUE half -- which also catches a
    // mistyped label separator.
    bool enum_ok = true;
    for (int i = 0; i < n; i++) {
        if (opts[i].type != OPT_ENUM) continue;
        wchar_t c[512]; wcsncpy_s(c, opts[i].choices, _TRUNCATE);
        wchar_t* ctx = NULL; bool hit = false;
        for (wchar_t* t = wcstok_s(c, L"|", &ctx); t; t = wcstok_s(NULL, L"|", &ctx)) {
            wchar_t cv[64], cl[192];
            enum_split(t, cv, _countof(cv), cl, _countof(cl));
            if (!wcscmp(cv, opts[i].def)) hit = true;
        }
        if (!hit) enum_ok = false;
    }
    st_check("every enum default is one of its choices", enum_ok);

    {
        wchar_t v[64], l[192];
        enum_split(L"2=Crisp", v, _countof(v), l, _countof(l));
        st_check("enum_split reads value=Label", !wcscmp(v, L"2") && !wcscmp(l, L"Crisp"));
        enum_split(L"off", v, _countof(v), l, _countof(l));
        st_check("enum_split passes a bare choice through", !wcscmp(v, L"off") && !wcscmp(l, L"off"));
    }

    // FIX PACKS. The failure this guards is the whole reason packs exist: a pack
    // whose member is not also a row of its own would never be HEALED, so on a stale
    // install the box would tick, write nothing the ini kept, and the fix would
    // silently not apply -- precisely the bug packs were added to stop.
    bool pack_ok = true, pack_any = false;
    for (int i = 0; i < n; i++) {
        if (opts[i].type != OPT_PACK) continue;
        pack_any = true;
        if (!opts[i].choices) { pack_ok = false; continue; }
        const wchar_t* p = opts[i].choices;
        PackMember mem;
        int members = 0;
        while (pack_member(&p, &mem)) {
            members++;
            bool found = false;
            for (int j = 0; j < n; j++)
                if (ST_KEYED(opts[j]) && !wcscmp(opts[j].sec, mem.sec)
                                      && !wcscmp(opts[j].key, mem.key)) found = true;
            if (!found) {
                pack_ok = false;
                printf("  [pack] %ls: member [%ls] %ls has no row of its own -- "
                       "iniheal will never write it\n", opts[i].label, mem.sec, mem.key);
            }
            if (!mem.on[0] || !mem.off[0]) pack_ok = false;
        }
        if (members < 2) pack_ok = false;    // one key needs no pack
    }
    st_check("at least one fix pack is defined", pack_any);
    st_check("every pack member is also a healed row of its own", pack_ok);

    // Round-trip a pack against a real ini: off, on, partial, off.
    {
        const ShimOption* pk = NULL;
        for (int i = 0; i < n; i++) if (opts[i].type == OPT_PACK) { pk = &opts[i]; break; }
        if (pk) {
            DeleteFileW(tmp);
            st_check("pack reads OFF against an empty ini", !pack_is_on(pk));
            pack_write(pk, true);
            st_check("pack reads ON after writing it on",   pack_is_on(pk));
            const wchar_t* p = pk->choices; PackMember one;
            if (pack_member(&p, &one)) {
                WritePrivateProfileStringW(one.sec, one.key, one.off, tmp);
                st_check("one member off -> the pack reads OFF", !pack_is_on(pk));
            }
            pack_write(pk, false);
            st_check("pack reads OFF after writing it off",  !pack_is_on(pk));
            DeleteFileW(tmp);
        }
    }

    // A keyed row, for the written-value-wins check -- opts[0] is a heading now.
    const ShimOption* first_keyed = NULL;
    for (int i = 0; i < n; i++) if (ST_KEYED(opts[i])) { first_keyed = &opts[i]; break; }
    st_check("the table has at least one keyed row", first_keyed != NULL);
    if (first_keyed) {
        WritePrivateProfileStringW(first_keyed->sec, first_keyed->key, L"7", tmp);
        wchar_t back[256];
        st_check("written value beats the default",
                 !wcscmp(opt_value(first_keyed, back, _countof(back)), L"7"));
    }
    DeleteFileW(tmp);

    // TRAILING COMMENTS. The bug this guards cost a user every documented setting
    // they had: the Win32 profile API returns a "; ..." comment as part of the value,
    // so the dialog read `d3d_windowed=1  ; FMO is d3d9` as neither "1" nor "0",
    // drew the box unticked, and wrote 0 on Save. GetPrivateProfileInt hid it by
    // stopping at the space, which is why the shim RAN fine off the same ini.
    // Every shipped template documents options exactly this way, so this is the
    // normal case, not an edge one.
    {
        WritePrivateProfileStringW(L"dx", L"st_bool",  L"1       ; a trailing comment", tmp);
        WritePrivateProfileStringW(L"dx", L"st_enum",  L"2          ; open at 2x", tmp);
        WritePrivateProfileStringW(L"dx", L"st_text",  L"off             ; leave to registry", tmp);
        WritePrivateProfileStringW(L"dx", L"st_tabs",  L"1\t\t; after tabs", tmp);
        WritePrivateProfileStringW(L"dx", L"st_lead",  L"; the whole value is a comment", tmp);
        WritePrivateProfileStringW(L"dx", L"st_inner", L"a;b", tmp);
        WritePrivateProfileStringW(L"dx", L"st_plain", L"1", tmp);
        wchar_t v[256];
        #define ST_INI(k) (ini_str(L"dx", k, L"", v, _countof(v), tmp), v)
        st_check("commented bool reads as its value",  !wcscmp(ST_INI(L"st_bool"), L"1"));
        st_check("commented enum reads as its value",  !wcscmp(ST_INI(L"st_enum"), L"2"));
        st_check("commented text reads as its value",  !wcscmp(ST_INI(L"st_text"), L"off"));
        st_check("a tab before the ';' opens a comment too", !wcscmp(ST_INI(L"st_tabs"), L"1"));
        st_check("a value that is only a comment reads empty", !wcscmp(ST_INI(L"st_lead"), L""));
        // The reason the rule needs the whitespace: this is a VALUE, not a comment.
        st_check("';' with no space before it stays in the value",
                 !wcscmp(ST_INI(L"st_inner"), L"a;b"));
        st_check("an uncommented value is untouched",  !wcscmp(ST_INI(L"st_plain"), L"1"));
        #undef ST_INI
        DeleteFileW(tmp);
    }

    // ...and the same thing through a PACK, which is how the Tetra Master mouse fix
    // was turned off: pack_is_on compares each member's text against its on-value.
    {
        const ShimOption* pk = NULL;
        for (int i = 0; i < n; i++) if (opts[i].type == OPT_PACK) { pk = &opts[i]; break; }
        if (pk) {
            DeleteFileW(tmp);
            const wchar_t* p = pk->choices;
            PackMember mem;
            while (pack_member(&p, &mem)) {
                wchar_t withc[192];
                swprintf_s(withc, L"%ls     ; why this is on", mem.on);
                WritePrivateProfileStringW(mem.sec, mem.key, withc, tmp);
            }
            st_check("a pack whose members carry comments still reads ON", pack_is_on(pk));
            DeleteFileW(tmp);
        }
    }
    #undef ST_KEYED

    // --- MIGRATION: an install left on an OLD DEFAULT ------------------------
    //
    // The whole point of inimigrate() is that it reaches a machine nobody is
    // sitting at, so its failure mode is silence. These build the exact ini a
    // second computer was found carrying and assert each property separately --
    // repaired, left alone, deleted, stamped, and idempotent -- because "it
    // rewrote the file" is not the same claim as "it rewrote the right things".
    {
        wchar_t mi[MAX_PATH];
        swprintf_s(mi, L"%spolshim-migrate-%lu.ini", dir, GetCurrentProcessId());
        DeleteFileW(mi);

        // Values this project really shipped, before the fixes that replaced them.
        WritePrivateProfileStringW(L"dx", L"dinput_mouseabs",     L"1", mi);
        WritePrivateProfileStringW(L"dx", L"d3d_unmask",          L"1", mi);
        // rev 7: the pair that makes windowed the default on an install that already
        // exists. Both of these are values every shipped template physically carries,
        // which is the whole reason a default change could not do this job.
        WritePrivateProfileStringW(L"dx", L"d3d_windowed",        L"0", mi);
        WritePrivateProfileStringW(L"dx", L"d3d_windowed_except", L"FFXiMain.dll", mi);
        // rev 8: a diagnostic that was unsafe ON, and a guard that had earned its
        // promotion. Both shipped as explicit rows, so both need a migration.
        WritePrivateProfileStringW(L"auth", L"log",               L"1", mi);
        WritePrivateProfileStringW(L"dx", L"fe_teardown_guard",   L"0", mi);
        WritePrivateProfileStringW(L"dx", L"d3d_fitwindow",       L"0", mi);   // retired
        WritePrivateProfileStringW(L"regfix", L"enable",          L"0", mi);
        WritePrivateProfileStringW(L"settings", L"hotkey", L"ctrl+shift+s", mi);
        // ...and two the user chose, which must survive untouched. d3d_scale is not
        // in the migration table at all; dinput_mousescale IS a fix key, but 40 is
        // not a value we ever shipped, so the table must not claim it.
        WritePrivateProfileStringW(L"dx", L"d3d_scale",         L"2",  mi);
        WritePrivateProfileStringW(L"dx", L"dinput_mousescale", L"40", mi);
        // A trailing comment is part of the value to the Win32 profile API -- the
        // shipped templates are full of them, so a migration that cannot see past
        // one would skip exactly the installs that took their values from a template.
        WritePrivateProfileStringW(L"dx", L"dinput_autofind", L"1   ; find the cursor", mi);

        inimigrate(mi);

        wchar_t v[256];
        #define MG(sec, key) (ini_str(L##sec, L##key, L"(absent)", v, _countof(v), mi), v)
        st_check("mouseabs 1 -> 2 (Tetra Master's cursor)",  !wcscmp(MG("dx", "dinput_mouseabs"), L"2"));
        st_check("unmask 1 -> 2 (the mask is aligned)",      !wcscmp(MG("dx", "d3d_unmask"), L"2"));
        st_check("fullscreen 0 -> 1 (windowed is the default now)",
                                                             !wcscmp(MG("dx", "d3d_windowed"), L"1"));
        st_check("FFXiMain.dll -> empty except list (nothing held fullscreen)",
                                                             !wcscmp(MG("dx", "d3d_windowed_except"), L""));
        st_check("auth log 1 -> 0 (the session key stops reaching the log)",
                                                             !wcscmp(MG("auth", "log"), L"0"));
        st_check("fe_teardown_guard 0 -> 1 (proven live 08-20, Wine-gated)",
                                                             !wcscmp(MG("dx", "fe_teardown_guard"), L"1"));
        st_check("regfix off -> on",                         !wcscmp(MG("regfix", "enable"), L"1"));
        st_check("single chord -> the two-chord default",    !wcscmp(MG("settings", "hotkey"), POLSHIM_DEFAULT_HOTKEY));
        st_check("a commented value is still matched",       !wcscmp(MG("dx", "dinput_autofind"), L"0"));
        st_check("a RETIRED key is removed outright",        !wcscmp(MG("dx", "d3d_fitwindow"), L"(absent)"));
        // The promise. If either of these fails the feature is worse than nothing:
        // it is the shim changing settings the user chose, which is the complaint it
        // exists to answer, one level down.
        st_check("a value we never shipped is left alone",   !wcscmp(MG("dx", "dinput_mousescale"), L"40"));
        st_check("a key outside the table is left alone",    !wcscmp(MG("dx", "d3d_scale"), L"2"));
        st_check("config_rev is stamped",
                 GetPrivateProfileIntW(L"polshim", L"config_rev", 0, mi) > 0);

        // IDEMPOTENCE, and it is the one that matters for trust: setting a migrated
        // key back by hand must STICK. A second pass that re-migrated it would mean
        // the user can never keep the old value, which is a different bug wearing
        // the same clothes.
        WritePrivateProfileStringW(L"dx", L"dinput_mouseabs", L"1", mi);
        inimigrate(mi);
        st_check("a second pass does not re-migrate a hand-set value",
                 !wcscmp(MG("dx", "dinput_mouseabs"), L"1"));

        // The BUTTON, which ignores config_rev on purpose -- it is pressed by
        // somebody whose games are still wrong after the automatic pass.
        static char rep[8192];
        int fixed = shim_reset_fixes(mi, rep, sizeof(rep));
        st_check("reset puts a hand-set fix key back",  !wcscmp(MG("dx", "dinput_mouseabs"), L"2"));
        st_check("reset reports what it changed",       fixed > 0 && strlen(rep) > 0);
        // ...and stays inside the fix group. The window size and the server are not
        // fixes, and a reset that took them with it would be a reset nobody dares press.
        st_check("reset leaves settings outside the group alone",
                 !wcscmp(MG("dx", "d3d_scale"), L"2"));
        // Run twice: the second time there is nothing left to do.
        st_check("reset is idempotent", shim_reset_fixes(mi, NULL, 0) == 0);
        printf("\n--- reset_fixes said ---\n%s---\n", rep);
        #undef MG
        DeleteFileW(mi);
    }

    // --- A BRAND-NEW INSTALL, IN THE REAL ORDER ------------------------------
    //
    // IMPORTANT: THE TRAP THIS PINS, found 2026-08-24 while making windowed the default.
    //
    // inject.cpp runs iniheal() and THEN inimigrate(). On a fresh install iniheal
    // writes the CURRENT defaults and [polshim] config_rev is absent -- which reads
    // as 0, so inimigrate walks EVERY migration ever written, including rev 1, over
    // a file iniheal filled in seconds earlier. Any migration whose `was` happens to
    // equal a key's PRESENT-DAY default therefore fires on a machine built today and
    // silently un-does that default, for ever, on every new install.
    //
    // That is not hypothetical: rev 1 carried d3d_windowed_except "" -> FFXiMain.dll.
    // It was harmless for as long as the healed default was FFXiMain.dll (the `was`
    // could never match a fresh file). The moment the default became empty, rev 1
    // matched it -- so shipping "windowed by default" would have put FFXI straight
    // back to fullscreen on precisely the installs it was meant to reach, with the
    // migration log line as the only trace. Rev 1's row is deleted; this is the check
    // that catches the next one.
    //
    // The invariant, stated once: AFTER HEAL+MIGRATE ON AN EMPTY FILE, EVERY OPTION
    // MUST STILL READ ITS OWN TABLE DEFAULT. Whatever the migration table grows to,
    // it must never contradict the defaults a new install was just given.
    {
        wchar_t fi[MAX_PATH];
        swprintf_s(fi, L"%spolshim-fresh-%lu.ini", dir, GetCurrentProcessId());
        DeleteFileW(fi);

        iniheal(fi);        // exactly the order inject.cpp uses -- heal, then migrate
        inimigrate(fi);

        int n2 = 0;
        const ShimOption* o2 = shim_options(&n2);
        bool fresh_ok = true;
        for (int i = 0; i < n2; i++) {
            if (!o2[i].sec || !o2[i].key || !o2[i].def) continue;
            wchar_t v[512];
            ini_str(o2[i].sec, o2[i].key, L"\x01?\x01", v, _countof(v), fi);
            if (wcscmp(v, o2[i].def) != 0) {
                fresh_ok = false;
                printf("    !! fresh install: [%ls] %ls = '%ls', default is '%ls'"
                       " -- a migration is un-doing a current default\n",
                       o2[i].sec, o2[i].key, v, o2[i].def);
            }
        }
        st_check("a NEW install survives the migration pass with its defaults intact",
                 fresh_ok);
        // ...and the two that matter most here, named outright so a failure says WHICH
        // setting regressed rather than only that something did.
        wchar_t w[64], e[256];
        ini_str(L"dx", L"d3d_windowed", L"?", w, _countof(w), fi);
        ini_str(L"dx", L"d3d_windowed_except", L"?", e, _countof(e), fi);
        st_check("a NEW install is WINDOWED",                     !wcscmp(w, L"1"));
        st_check("a NEW install holds no title fullscreen",       !wcscmp(e, L""));
        DeleteFileW(fi);
    }

    // The Repair button's whole value is that it EXPLAINS itself. A scan that reports
    // nothing is the bug we are fixing, so assert it always produces text -- on this
    // dev box it will usually find a real hive, but the no-hive path must talk too.
    static char report[16384];
    int rc = regfix_scan(report, sizeof(report), 0);      // apply=0: never writes here
    st_check("regfix_scan produces a report", strlen(report) > 0);
    st_check("regfix_scan does not overflow",  strlen(report) < sizeof(report));
    bool explains = (rc < 0) ? (strstr(report, "NO registered Viewer") != NULL)
                             : (strstr(report, "hive ") != NULL && strstr(report, "root ") != NULL);
    st_check("report states hive+root, or why it cannot", explains);
    printf("\n--- regfix_scan(apply=0) said ---\n%s---\n", report);

    printf("\n%s (%d failure%s)\n", st_fail ? "FAILED" : "all checks passed",
           st_fail, st_fail == 1 ? "" : "s");
    return st_fail;
}

// Open the dialog against a given ini -- the visual pass, driven by --show.
// WHAT EACH SIDEBAR SECTION ACTUALLY CONTAINS, as text.
//
// Exists because this window was once shipped with a section nobody had looked at.
// It walks the same layout_compute the dialog draws with, so it cannot answer a
// different question than the screen does -- which is the whole reason it is not a
// hand-written summary.
void polsettings_dump_cats_for_test(const wchar_t* ini)
{
    if (ini) wcsncpy_s(g_ini, ini, _TRUNCATE);
    servers_load();

    int n = 0; const ShimOption* opts = shim_options(&n);
    static LyRow pos[LY_MAXROWS];

    struct Cat { int id; const wchar_t* name; };
    static Cat cats[64]; int ncat = 0;
    cats[ncat++] = { CAT_ALL, L"All settings" };
    for (int i = 0; i < n && ncat < 64; i++)
        if (opts[i].type == OPT_GROUP && !opts[i].dev && !group_title_leaf(opts, i))
            cats[ncat++] = { i, cat_name(opts[i]) };
    for (int g = 0; g < profiles_count() && ncat < 64; g++)
        if (cat_game_is_first(g)) {
            const TitleProfile* p = profiles_at(g);
            wchar_t* w = (wchar_t*)calloc(96, sizeof(wchar_t));
            MultiByteToWideChar(CP_ACP, 0, p->title, -1, w, 95);
            cats[ncat++] = { CAT_GAME_BASE - g, w };
        }

    for (int c = 1; c < ncat; c++) {     // skip All settings: it is every row
        int cw = 0, ch = 0;
        int m = layout_compute(false, L"", cats[c].id, pos, (int)_countof(pos), &cw, &ch);
        wprintf(L"\n== %ls\n", cats[c].name);
        int shown = 0;
        for (int i = 0; i < m; i++) {
            if (pos[i].y < 0) continue;
            const ShimOption& o = opts[i];
            if (o.type == OPT_GROUP) { wprintf(L"   -- %ls\n", o.label); continue; }
            static const wchar_t* tn[] = { L"check", L"drop", L"text", L"server",
                                           L"hidden", L"group", L"pack", L"BUTTON" };
            wchar_t val[256] = L"";
            if (o.type != OPT_BUTTON) opt_value(&o, val, _countof(val));
            wprintf(L"   %-7ls %-46ls %ls\n", tn[o.type], o.label,
                    o.type == OPT_BUTTON ? L"" : val);
            shown++;
        }
        wprintf(L"   (%d row%ls)\n", shown, shown == 1 ? L"" : L"s");
    }
}

void polsettings_show_for_test(const wchar_t* ini)
{
    wcsncpy_s(g_ini, ini, _TRUNCATE);
    polsettings_open();
}

// Open JUST the report window, without the settings dialog -- so the scrolling text
// window can be eyeballed directly instead of by synthesising a click on the Repair
// button, which on a machine with a live Viewer risks clicking into somebody's session.
void polsettings_show_report_for_test()
{
    static char report[16384];
    int rc = regfix_scan(report, sizeof(report), 0);   // apply=0: report only, never writes
    static char full[16640];
    _snprintf_s(full, sizeof(full), _TRUNCATE,
                "%s\n%s", rc < 0 ? "Registration repair could not run."
                                 : "Dry run (apply=0) -- nothing was written.", report);
    show_text(NULL, L"Game registration repair", full);
}

// Parse and arm the four chord specs. polsettings_start and the Save path share
// this, so a chord edited in the dialog is armed the moment it is saved -- the
// watcher polls these globals every tick, which is why arming is a plain
// re-parse and needs no thread work. The echo logging lives here too: every
// re-arm should say what is now live, not only the first one.
static void settings_load_chords(const wchar_t* ini)
{
    wchar_t spec[256], badspec[256];
    ini_str(L"settings", L"hotkey", POLSHIM_DEFAULT_HOTKEY, spec, _countof(spec), ini);
    g_hk_count = parse_hotkey_list(spec, g_hk_mods, g_hk_vk, HK_MAX, badspec, _countof(badspec));
    if (badspec[0])
        logf("[settings] hotkey: ignored unparsable chord(s) '%ls'", badspec);
    if (g_hk_count == 0) {
        logf("[settings] hotkey '%ls' armed nothing -- falling back to '%ls'",
             spec, POLSHIM_DEFAULT_HOTKEY);
        wcscpy_s(spec, POLSHIM_DEFAULT_HOTKEY);   // so the armed line below reports what is LIVE
        g_hk_count = parse_hotkey_list(spec, g_hk_mods, g_hk_vk, HK_MAX, NULL, 0);
    }
    wchar_t padspec[128];
    ini_str(L"settings", L"pad_chord", L"back+start", padspec, _countof(padspec), ini);
    g_pad_mask  = parse_pad(padspec);
    g_pad_debug = GetPrivateProfileIntW(L"settings", L"pad_debug", 0, ini);
    g_fullscreen_ok = GetPrivateProfileIntW(L"settings", L"fullscreen_ok", 0, ini);

    // The mapper's own chord. Default `back+lb`: it shares `back` with the dialog chord
    // (one small button is the only one a Deck reliably has spare) but adds a SHOULDER,
    // which no face-button layout can collide with. Both are armed at once and the
    // watcher gives the mapper priority, so pressing back+start+lb opens the mapper, not
    // both.
    wchar_t uispec[128];
    ini_str(L"settings", L"padui_chord", L"back+lb", uispec, _countof(uispec), ini);
    g_padui_mask = parse_pad(uispec);
    wchar_t uikey[128];
    ini_str(L"settings", L"padui_hotkey", L"ctrl+shift+p", uikey, _countof(uikey), ini);
    g_ui_hk_count = parse_hotkey_list(uikey, g_ui_hk_mods, g_ui_hk_vk, HK_MAX, NULL, 0);

    // THE DISPLAY SWITCH. Alt+Enter, because that is what every other windowed
    // game uses; the watcher only acts on it while the game is in front
    // (d3d_display_toggle checks), so it cannot fire from another program.
    wchar_t dpkey[128];
    ini_str(L"settings", L"display_hotkey", L"alt+enter", dpkey, _countof(dpkey), ini);
    g_dp_hk_count = parse_hotkey_list(dpkey, g_dp_hk_mods, g_dp_hk_vk, HK_MAX, NULL, 0);
    logf("[display] hotkey '%ls' armed (%d chord(s)) -- windowed <-> borderless for the "
         "running game", dpkey, g_dp_hk_count);

    // THE REPORT KEY.
    wchar_t rpkey[128];
    ini_str(L"report", L"hotkey", POLREPORT_DEFAULT_HOTKEY, rpkey, _countof(rpkey), ini);
    wchar_t rpbad[128] = L"";
    g_rp_hk_count = parse_hotkey_list(rpkey, g_rp_hk_mods, g_rp_hk_vk, HK_MAX,
                                      rpbad, _countof(rpbad));
    if (rpbad[0])
        logf("[report] hotkey: ignored unparsable chord(s) '%ls'", rpbad);
    wchar_t rppad[128];
    ini_str(L"report", L"pad_chord", L"back+rb", rppad, _countof(rppad), ini);
    g_rp_pad_mask = parse_pad(rppad);

    // Echo the chord back in the SAME vocabulary the ini uses. The old line printed raw
    // masks, which is useless for the one question people actually have -- "is the
    // button I am pressing the button it is waiting for". Note `back` is the small
    // View/Select button, NOT the B face button; that mix-up is the first thing to rule out.
    char padn[128]; pad_names(g_pad_mask, padn, sizeof(padn));
    logf("[settings] armed -- keyboard '%ls', pad '%s' (mask 0x%04X)%s",
         spec, padn, g_pad_mask,
         g_pad_debug ? " [pad_debug ON: every button change is logged]" : "");
    if (!g_pad_mask)
        logf("[settings] pad chord is EMPTY -- '%ls' named no known button; keyboard only", padspec);

    // The mapper chord is logged in the SAME vocabulary, on its own line, because "which
    // buttons open the thing that shows me which buttons do what" is the one question a
    // controller mapper must never make you guess at.
    char uin[128]; pad_names(g_padui_mask, uin, sizeof(uin));
    logf("[settings] controller mapper armed -- pad '%s' (mask 0x%04X), keyboard '%ls'%s",
         uin, g_padui_mask, uikey,
         padoverlay_enabled() ? "" : "  [DISABLED: [inputmode] pad_overlay=0]");
    if (!g_padui_mask && g_ui_hk_count == 0)
        logf("[settings] the controller mapper has NO way to open -- both padui_chord "
             "'%ls' and padui_hotkey '%ls' armed nothing", uispec, uikey);

    // The report key, in the same vocabulary, so "which keys file a report" can be
    // checked in the log without a working game.
    {
        char rpn[128]; pad_names(g_rp_pad_mask, rpn, sizeof(rpn));
        if (polreport_armed())
            logf("[report] armed -- keyboard '%ls', pad '%s' (mask 0x%04X)",
                 rpkey, rpn, g_rp_pad_mask);
        else
            logf("[report] off ([report] enable=0) -- the report key does nothing");
        if (polreport_armed() && !g_rp_pad_mask && g_rp_hk_count == 0)
            logf("[report] there is NO way to file a report -- both hotkey '%ls' "
                 "and pad_chord '%ls' armed nothing", rpkey, rppad);
    }
}

void polsettings_start(const wchar_t* ini)
{
    g_enabled = GetPrivateProfileIntW(L"settings", L"enable", 1, ini);
    if (!g_enabled) return;
    wcsncpy_s(g_ini, ini, _TRUNCATE);

    settings_load_chords(ini);

    g_thread = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    if (!g_thread) { logf("[settings] watcher thread failed (%lu)", GetLastError()); return; }
}
