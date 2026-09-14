// dinputhook.cpp -- DirectInput 8 mouse interposition.
//
// WHY, measured 2026-08-12. With the cursor hooks in dxhook/d3d8hook live for a
// whole Tetra Master session the log carried FIVE [cur] lines: two
// SetCursorPos(30,30) and two GetCursorPos. A game driving a pointer calls those
// every frame. So user32 is not where TM's pointer comes from -- it parks the OS
// cursor once and then reads the mouse through DirectInput, which TM.dll imports
// as DINPUT8!DirectInput8Create.
//
// That one fact explains both remaining symptoms:
//
//   "it aggressively steals my window"  A mouse acquired with DISCL_EXCLUSIVE
//       must be the foreground window -- Acquire() fails otherwise -- so a game
//       written for exclusive fullscreen fights to stay foreground and grabs it
//       straight back when you click away.
//
//   "small real movements -> large virtual movements"  DirectInput hands the
//       application RAW device counts. A 2003 mouse was ~400 CPI; a modern one
//       is commonly 1600+. The game advances its cursor one unit per count, so
//       the same physical movement now travels ~4x as far, and a cursor clamped
//       to a 640x480 field spends its time pegged at the edges -- which is what
//       "snaps to different points, never anywhere accurate" looks like.
//       Note this is NOT caused by running windowed; it was in the original
//       fullscreen report too.
//
// Both are addressed here, and both are configurable because the right scale
// depends on the physical mouse:
//
//   dinput_nonexclusive   DISCL_EXCLUSIVE -> DISCL_NONEXCLUSIVE on the mouse
//   dinput_mousescale     percent applied to relative X/Y (100 = unchanged)
//
// dinput.h ships in the current Windows SDK, so unlike d3d8 the ABI is real:
// these are compiler-checked interface implementations, not hand-counted vtable
// slots.

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <new>
#include <vector>
#include "polshim.h"
#include "profiles.h"

static int g_di_enable      = 0;
static int g_di_trace       = 1;
static int g_di_nonexcl     = 1;
// DISCL_NOWINKEY is DirectInput's "disable the Windows logo key" flag, and the
// POL titles ask for it. Measured in this project's own logs 2026-08-26:
//     [din] mouse SetCooperativeLevel(hwnd=..., NONEXCLUSIVE|FOREGROUND|NOWINKEY)
//                                  -> NONEXCLUSIVE|BACKGROUND|NOWINKEY
// -- i.e. the shim was already rewriting that coop level and passing the Windows
// key kill straight through, in a build whose whole purpose was making a title
// possible to LEAVE. 1 (default) strips it. See also hookspy.cpp: a title's
// WH_KEYBOARD_LL hook can eat the same keys, and that is the other half.
static int g_di_winkey      = 1;
static int g_di_scale       = 100;   // percent
static int g_di_abs         = 0;     // absolute-tracking mode (overrides scale)
static int g_di_findcursor  = 0;     // RE tool: locate the game's internal cursor var
static int g_di_callsite    = 0;     // RE tool: report WHERE the game polls the mouse
// The statically-read cursor pair, for dinput_mouseabs=2 (see cursor_xy_resolve).
static wchar_t g_cur_xy_spec[128] = L"";
static void*   g_cur_xy_addr  = 0;
static bool    g_cur_xy_off   = false;   // resolution failed, or a write faulted
static bool    g_cur_xy_tried = false;
static LONG    g_n_direct     = 0;
// Cursor OWNERSHIP (mode 2). See the comment above apply_direct: the mouse only
// owns the cursor while the mouse is actually moving, so pad navigation and the
// game's own anchoring can hold it the rest of the time.
static int  g_di_abs_yield  = 1;     // hand the cursor back when the GAME moves it
static int  g_di_abs_follow = 1;     // park the OS pointer on the cursor while it does
static int  g_di_abs_wake   = 3;     // device counts of real motion that retake it
static int  g_di_abs_keepdelta = 0;  // leave lX/lY live, pre-subtracting the game's add
static LONG g_n_yield = 0, g_n_wake = 0;
static int g_di_abs_home    = 6;     // polls to slam into (0,0) on (re)acquire; 0 = don't home
static int g_di_abs_clip    = 1;     // confine the OS pointer to the game window while focused
// The wide scan is only allowed for an EXPLICIT probe. The automatic one must
// never reach it: it stalls the game inside its own input callback for seconds
// and, unguarded, crashed it outright.
static bool g_fc_wide    = false;
static int g_di_abs_gain    = 100;   // the game's OWN multiplier on our deltas, percent
static int g_di_autofind    = 0;     // run the finder by ITSELF once, to close the loop
static int g_di_autofind_at = 600;   // polls of settled play before the auto probe runs
static wchar_t g_di_ini[MAX_PATH] = L"";   // for writing a found address back

static LONG g_n_create = 0, g_n_mouse = 0, g_n_coop = 0, g_n_downgraded = 0;
static LONG g_n_bg = 0;   // FOREGROUND -> BACKGROUND rewrites
static LONG g_n_winkey = 0;   // NOWINKEY strips
// SEAM 4. Until 2026-09-09 both keyboard read paths returned before ANY
// bookkeeping, so no log this project has ever produced could say whether a
// title reads its keyboard through DirectInput at all. These counters are the
// answer, and they are why the gate below can be reported honestly instead of
// merely asserted.
static LONG g_n_kbdstate = 0;   // keyboard GetDeviceState  (polled)
static LONG g_n_kbddata  = 0;   // keyboard GetDeviceData   (buffered)
static LONG g_n_kbdgated = 0;   // ...reads withheld while another app was in front
static LONG g_n_kbdups   = 0;   // synthetic key-ups written into a caller's buffer
static LONG g_n_state  = 0, g_n_data  = 0, g_n_scaled = 0, g_n_abs = 0;
static LONG g_n_pad = 0, g_n_padstate = 0, g_n_paddata = 0;
// Buffered-object census. Which OBJECT the events in GetDeviceData's batches
// carry has never been logged, and the whole Fantasy Earth click failure turns
// on one binary question: are DIMOFS_BUTTON* events in that stream at all?
// See the census block in GetDeviceData for what the numbers mean.
static LONG g_n_ofs_x = 0, g_n_ofs_y = 0, g_n_ofs_z = 0, g_n_ofs_other = 0;
static LONG g_n_ofs_btn[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// Classify one buffered event by the object it carries: 0=X, 1=Y, 2=Z (wheel),
// 3..10 = BUTTON0..7, 11 = anything else. The index doubles as a bit position,
// so a whole batch collapses to a mask and "this batch carries kinds the last
// one did not" is a single compare -- which is what keeps the trace bounded.
static int didata_kind(DWORD ofs)
{
    if (ofs == (DWORD)DIMOFS_X) return 0;
    if (ofs == (DWORD)DIMOFS_Y) return 1;
    if (ofs == (DWORD)DIMOFS_Z) return 2;
    if (ofs >= (DWORD)DIMOFS_BUTTON0 && ofs <= (DWORD)DIMOFS_BUTTON0 + 7)
        return 3 + (int)(ofs - (DWORD)DIMOFS_BUTTON0);
    return 11;
}

static const char* const didata_kind_name[12] = {
    "X", "Y", "Z", "BTN0", "BTN1", "BTN2", "BTN3",
    "BTN4", "BTN5", "BTN6", "BTN7", "other"
};
static int  g_di_background = 1;   // [dx] dinput_background
static int  g_di_pad        = 1;   // [dx] dinput_pad -- wrap game controllers (padmap.cpp)

static void cursor_addr_parse(const wchar_t* spec);   // closed-loop cursor, below

void dinput_configure(const wchar_t* ini)
{
    g_di_pad     = GetPrivateProfileIntW(L"dx", L"dinput_pad",          1, ini);
    g_di_enable  = GetPrivateProfileIntW(L"dx", L"dinput_enable",       0, ini);
    g_di_trace   = GetPrivateProfileIntW(L"dx", L"dinput_trace",        trace_at(1), ini);
    g_di_nonexcl = GetPrivateProfileIntW(L"dx", L"dinput_nonexclusive", 1, ini);
    g_di_winkey  = GetPrivateProfileIntW(L"dx", L"dinput_winkey",       1, ini);
    g_di_background = GetPrivateProfileIntW(L"dx", L"dinput_background", 1, ini);
    g_di_scale   = ini_int_title(L"dx", L"dinput_mousescale", 25, ini);
    g_di_abs      = ini_int_title(L"dx", L"dinput_mouseabs",     0, ini);
    g_di_abs_home = GetPrivateProfileIntW(L"dx", L"dinput_abs_home",     6, ini);
    g_di_abs_clip = ini_int_title(L"dx", L"dinput_abs_clip",     0, ini);
    g_di_abs_yield  = GetPrivateProfileIntW(L"dx", L"dinput_abs_yield",  1, ini);
    g_di_abs_follow = GetPrivateProfileIntW(L"dx", L"dinput_abs_follow", 1, ini);
    g_di_abs_wake   = GetPrivateProfileIntW(L"dx", L"dinput_abs_wake",   3, ini);
    g_di_abs_keepdelta = GetPrivateProfileIntW(L"dx", L"dinput_abs_keepdelta", 0, ini);
    if (g_di_abs_wake < 1)   g_di_abs_wake = 1;
    if (g_di_abs_wake > 200) g_di_abs_wake = 200;
    g_di_callsite = GetPrivateProfileIntW(L"dx", L"dinput_callsite",     trace_at(3), ini);
    g_di_findcursor = GetPrivateProfileIntW(L"dx", L"dinput_findcursor", 0, ini);
    g_fc_wide       = (g_di_findcursor != 0);   // asked for by hand => may search all of memory
    g_di_autofind    = GetPrivateProfileIntW(L"dx", L"dinput_autofind",    0, ini);
    g_di_abs_gain    = GetPrivateProfileIntW(L"dx", L"dinput_abs_gain",   100, ini);
    if (g_di_abs_gain < 10)   g_di_abs_gain = 10;
    if (g_di_abs_gain > 1000) g_di_abs_gain = 1000;
    g_di_autofind_at = GetPrivateProfileIntW(L"dx", L"dinput_autofind_at", 600, ini);
    wcscpy_s(g_di_ini, ini);
    {
        // The address of the game's own cursor variable, as "MODULE+0xRVA" (or a
        // bare hex VA). Written back here by the finder, so the ~2 s probe happens
        // once per install and never again.
        wchar_t buf[128] = L"";
        ini_str(L"dx", L"dinput_cursor_addr", L"", buf, 128, ini);
        cursor_addr_parse(buf);
    }
    // The statically-read cursor pair (dinput_mouseabs=2). Defaulted, not hunted.
    ini_str(L"dx", L"dinput_cursor_xy", L"TM.dll+0x2FF59C",
            g_cur_xy_spec, _countof(g_cur_xy_spec), ini);
    if (g_di_scale < 5)    g_di_scale = 5;
    if (g_di_scale > 1000) g_di_scale = 1000;
    if (g_di_abs_home < 0)   g_di_abs_home = 0;
    if (g_di_abs_home > 240) g_di_abs_home = 240;
}

// Live re-read for the settings dialog: everything here is consulted per poll
// through the wrappers, so a new value takes effect on the next one.
// dinput_cursor_xy / dinput_cursor_addr are NOT re-read -- their resolution is
// latched (cursor_addr_parse / cursor_xy_resolve cache an address), so a new
// spec needs cursor_reset_resolution() first; left for a later pass.
void dinput_reload(const wchar_t* ini)
{
    g_di_pad     = GetPrivateProfileIntW(L"dx", L"dinput_pad",          1, ini);
    g_di_enable  = GetPrivateProfileIntW(L"dx", L"dinput_enable",       0, ini);
    g_di_nonexcl = GetPrivateProfileIntW(L"dx", L"dinput_nonexclusive", 1, ini);
    g_di_winkey  = GetPrivateProfileIntW(L"dx", L"dinput_winkey",       1, ini);
    g_di_background = GetPrivateProfileIntW(L"dx", L"dinput_background", 1, ini);
    g_di_scale   = ini_int_title(L"dx", L"dinput_mousescale", 25, ini);
    g_di_abs      = ini_int_title(L"dx", L"dinput_mouseabs",     0, ini);
    g_di_abs_home = GetPrivateProfileIntW(L"dx", L"dinput_abs_home",     6, ini);
    g_di_abs_clip = ini_int_title(L"dx", L"dinput_abs_clip",     0, ini);
    g_di_abs_yield  = GetPrivateProfileIntW(L"dx", L"dinput_abs_yield",  1, ini);
    g_di_abs_follow = GetPrivateProfileIntW(L"dx", L"dinput_abs_follow", 1, ini);
    g_di_abs_wake   = GetPrivateProfileIntW(L"dx", L"dinput_abs_wake",   3, ini);
    g_di_abs_keepdelta = GetPrivateProfileIntW(L"dx", L"dinput_abs_keepdelta", 0, ini);
    if (g_di_abs_wake < 1)   g_di_abs_wake = 1;
    if (g_di_abs_wake > 200) g_di_abs_wake = 200;
    g_di_autofind    = GetPrivateProfileIntW(L"dx", L"dinput_autofind",    0, ini);
    g_di_abs_gain    = GetPrivateProfileIntW(L"dx", L"dinput_abs_gain",   100, ini);
    if (g_di_abs_gain < 10)   g_di_abs_gain = 10;
    if (g_di_abs_gain > 1000) g_di_abs_gain = 1000;
    g_di_autofind_at = GetPrivateProfileIntW(L"dx", L"dinput_autofind_at", 600, ini);
    if (g_di_scale < 5)    g_di_scale = 5;
    if (g_di_scale > 1000) g_di_scale = 1000;
    if (g_di_abs_home < 0)   g_di_abs_home = 0;
    if (g_di_abs_home > 240) g_di_abs_home = 240;
    logf("[reload] dinput: enable=%d pad=%d nonexcl=%d bg=%d scale=%d abs=%d "
         "gain=%d yield=%d follow=%d wake=%d home=%d clip=%d keepdelta=%d "
         "autofind=%d at=%d",
         g_di_enable, g_di_pad, g_di_nonexcl, g_di_background, g_di_scale,
         g_di_abs, g_di_abs_gain, g_di_abs_yield, g_di_abs_follow,
         g_di_abs_wake, g_di_abs_home, g_di_abs_clip, g_di_abs_keepdelta,
         g_di_autofind, g_di_autofind_at);
}
int dinput_enabled() { return g_di_enable; }

static const char* coop_names(DWORD f, char* buf, size_t cb)
{
    buf[0] = '\0';
    struct { DWORD bit; const char* n; } t[] = {
        { DISCL_EXCLUSIVE,    "EXCLUSIVE" },
        { DISCL_NONEXCLUSIVE, "NONEXCLUSIVE" },
        { DISCL_FOREGROUND,   "FOREGROUND" },
        { DISCL_BACKGROUND,   "BACKGROUND" },
        { DISCL_NOWINKEY,     "NOWINKEY" },
    };
    for (int i = 0; i < _countof(t); i++) {
        if (!(f & t[i].bit)) continue;
        if (buf[0]) strncat_s(buf, cb, "|", _TRUNCATE);
        strncat_s(buf, cb, t[i].n, _TRUNCATE);
    }
    if (!buf[0]) strncpy_s(buf, cb, "0", _TRUNCATE);
    return buf;
}

// Scale a relative axis, keeping sub-unit movement alive: without the carry a
// scale below 100% turns every 1-count twitch into 0 and the pointer feels
// stuck rather than slower.
static LONG scale_axis(LONG v, LONG* carry)
{
    if (g_di_scale == 100 || v == 0) return v;
    LONG num = v * g_di_scale + *carry;
    LONG out = num / 100;
    *carry = num - out * 100;
    return out;
}

// ===========================================================================
// THE GAIN, and tuning it live.
//
// Absolute tracking computes a delta that WOULD put the game's cursor under the
// pointer if the game moved its cursor one unit per count we hand it. Measured
// behaviour says it does not: the game applies its own multiplier k, so asking
// for D moves the cursor by k*D. Every poll then overshoots (or undershoots) by
// the same ratio, the error accumulates as you move, and the only place it comes
// right is a screen edge -- where the game's own clamp forces the two back into
// agreement. That is exactly the reported symptom: "I could only get it to align
// by running it against the edge."
//
// The correction is one number. To move the cursor by D we must ask for D/k, and
// the model still lands on the target because the game still moves k*(D/k) = D.
//
// k is a property of the title and cannot be read from anywhere -- TetraMaster's
// registry has no sensitivity setting -- so it is MEASURED: hold Ctrl+Alt and
// press [ or ] in game to walk the value and watch the cursor converge. The
// value is logged and saved, so the tuning survives the session. This is a much
// cheaper instrument than hunting the cursor variable in memory, and if NO value
// makes the cursor track, that is itself the answer: the game's integration is
// not a constant factor and only the closed loop can fix it.
// ===========================================================================

// Divide by the gain, keeping a carry so sub-unit motion is not rounded away to
// a dead pointer (the same reason scale_axis carries).
static LONG gain_axis(LONG v, LONG* carry)
{
    if (g_di_abs_gain == 100 || v == 0) return v;
    LONG num = v * 100 + *carry;
    LONG out = num / g_di_abs_gain;
    *carry = num - out * g_di_abs_gain;
    return out;
}

// Ctrl+Alt+[ / Ctrl+Alt+] walk the gain while the game runs. Polled from the
// mouse read, so it costs no thread and no hook: we are already called every
// frame. Edge-triggered, so holding the key steps once.
static void gain_hotkeys(const wchar_t* ini)
{
    static bool prev_dn = false, prev_up = false;
    if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return;
    if (!(GetAsyncKeyState(VK_MENU)    & 0x8000)) return;
    bool dn = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;   // [
    bool up = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;   // ]
    int before = g_di_abs_gain;
    if (dn && !prev_dn) g_di_abs_gain -= 5;
    if (up && !prev_up) g_di_abs_gain += 5;
    prev_dn = dn; prev_up = up;
    if (g_di_abs_gain < 10)   g_di_abs_gain = 10;
    if (g_di_abs_gain > 1000) g_di_abs_gain = 1000;
    if (g_di_abs_gain == before) return;
    logf("[cur*] gain now %d%% (was %d) -- the game moves its cursor %d.%02dx what "
         "we ask; lower it if the cursor overshoots your pointer, raise it if it "
         "lags behind", g_di_abs_gain, before,
         g_di_abs_gain / 100, g_di_abs_gain % 100);
    if (ini && ini[0]) {
        wchar_t v[16];
        _snwprintf_s(v, _TRUNCATE, L"%d", g_di_abs_gain);
        WritePrivateProfileStringW(L"dx", L"dinput_abs_gain", v, ini);
    }
}

// ===========================================================================
// CURSOR-VARIABLE FINDER (dinput_findcursor) -- RE tool.
//
// Tetra Master keeps its own cursor in memory and integrates DirectInput deltas
// into it (it imports no cursor-position API at all). To drive it flawlessly we
// must READ that variable. This finds it: we steer the game cursor to a series
// of KNOWN points and, after each, scan the game's writable memory for an
// adjacent (x,y) int32 pair equal to that point, keeping only addresses that
// match at EVERY point. What survives is the cursor. Deterministic, open-loop
// (home to (0,0), then feed an exact delta), so it needs no OS-pointer input.
// ===========================================================================

static void fc_scan_region(unsigned char* s, unsigned char* rend, LONG px, LONG py);

static bool fc_module_range(const char* name, unsigned char** base, size_t* size)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    bool found = false;
    if (Module32First(snap, &me)) do {
        if (_stricmp(me.szModule, name) == 0) {
            *base = me.modBaseAddr; *size = me.modBaseSize; found = true; break;
        }
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    return found;
}

// Resolve an address back to "module+0xRVA" for the log.
static void fc_describe(unsigned char* a, char* out, size_t cb)
{
    const char* mods[] = { "TM.dll", "polcore.dll", "pol.exe" };
    for (int i = 0; i < 3; i++) {
        unsigned char* b; size_t s;
        if (fc_module_range(mods[i], &b, &s) && a >= b && a < b + s) {
            _snprintf_s(out, cb, _TRUNCATE, "%s+0x%X", mods[i], (unsigned)(a - b));
            return;
        }
    }
    _snprintf_s(out, cb, _TRUNCATE, "%p (heap/other)", a);
}

// ===========================================================================
// CALL-SITE INSTRUMENT (`[dx] dinput_callsite`) -- WHERE the game polls the
// mouse, and therefore where it integrates the deltas.
//
// WHY THIS, AND WHY NOW. Everything before it steers the game's cursor from
// OUTSIDE: feed a delta, hope the game's own multiplier k is what we assumed,
// correct at the edges. That is an open loop around code we have never read,
// and it has cost this project weeks -- the gain walk (`dinput_abs_gain`), the
// heap-wide cursor finder that crashed the game, the stack false positive. The
// integrator is a piece of x86 in TM.dll; k is an immediate or a constant in
// it. Read it and the guessing ends.
//
// A static hunt for the site is blocked: TM.dll is ASProtect-packed, its import
// directory is a stub (one function per DLL) and the real IAT is rebuilt at run
// time OUTSIDE the module, so `call [slot]` cannot be tied to a name from the
// image alone. The process knows what the image does not -- so ask it.
//
// The RETURN ADDRESS of our own hook is exact: it is the instruction after the
// game's `call [GetDeviceState]`, i.e. the poll site itself. The frames above
// it are a heuristic -- the module ships FPO'd, so no EBP chain can be trusted
// -- and are recovered by scanning this thread's stack for values that land in
// a known module's code. Over-reporting is fine here: a handful of candidate
// RVAs to disassemble beats none, and every one is checked against the image
// afterwards. Read-only, bounded by the TIB's own stack limits, first few calls
// only; it cannot perturb what it measures.
// ===========================================================================

// ===========================================================================
// THE CURSOR VARIABLE, as an ADDRESS rather than a search.
//
// `dinput_cursor_addr` (above) is the FINDER's output: a pair the probe hunted
// for at run time. This is different and it should not be conflated -- it is
// the pair read out of the game's own code, statically, so there is nothing to
// hunt. It is spelled MODULE+0xRVA because the load base moves every run.
//
// Default `TM.dll+0x2FF59C` (measured). Resolved lazily and once: the
// module is not loaded when the ini is read (the shim is in pol.exe long before
// a title is), so resolution has to happen on the first poll that wants it.
// ===========================================================================
static void* cursor_xy_resolve()
{
    if (g_cur_xy_off) return 0;
    if (g_cur_xy_addr) return g_cur_xy_addr;
    if (g_cur_xy_tried && !g_cur_xy_addr) {
        // Not fatal and not permanent: the title may simply not be loaded yet.
        // Retry, but only every so often -- this runs on the mouse poll.
        static LONG spin = 0;
        if ((InterlockedIncrement(&spin) % 240) != 0) return 0;
    }
    g_cur_xy_tried = true;

    char spec[128];
    WideCharToMultiByte(CP_ACP, 0, g_cur_xy_spec, -1, spec, sizeof(spec), 0, 0);
    char* plus = strchr(spec, '+');
    if (!plus) { g_cur_xy_off = true; return 0; }
    *plus = '\0';
    unsigned rva = (unsigned)strtoul(plus + 1, 0, 0);
    if (!rva) { g_cur_xy_off = true; return 0; }

    unsigned char* base; size_t size;
    if (!fc_module_range(spec, &base, &size)) return 0;    // title not up yet
    if (rva + 4 > size) {
        logf("[cur2] %s+0x%X is past the end of the module (size 0x%X) -- off",
             spec, rva, (unsigned)size);
        g_cur_xy_off = true;
        return 0;
    }
    g_cur_xy_addr = base + rva;
    logf("[cur2] cursor variable at %s+0x%X = %p (direct write armed)",
         spec, rva, g_cur_xy_addr);
    return g_cur_xy_addr;
}

// `fc_describe` takes a toolhelp snapshot per call, which is fine for the
// finder's handful of hits and ruinous for a walk that tests every stack slot.
// Snapshot the ranges once instead.
struct CsMod { const char* name; unsigned char* base; size_t size; };
static CsMod g_cs_mod[4];
static int   g_cs_nmod = 0;

static void cs_load_modules()
{
    if (g_cs_nmod) return;
    static const char* want[] = { "TM.dll", "polcore.dll", "pol.exe", "app.dll" };
    for (int i = 0; i < _countof(want); i++) {
        unsigned char* b; size_t s;
        if (fc_module_range(want[i], &b, &s)) {
            g_cs_mod[g_cs_nmod].name = want[i];
            g_cs_mod[g_cs_nmod].base = b;
            g_cs_mod[g_cs_nmod].size = s;
            g_cs_nmod++;
        }
    }
}

static const CsMod* cs_owner(unsigned char* a)
{
    for (int i = 0; i < g_cs_nmod; i++)
        if (a >= g_cs_mod[i].base && a < g_cs_mod[i].base + g_cs_mod[i].size)
            return &g_cs_mod[i];
    return 0;
}

static void di_report_callsite(const char* what, void* ret)
{
    if (!g_di_callsite) return;
    cs_load_modules();

    const CsMod* m = cs_owner((unsigned char*)ret);
    if (m) logf("[cs] %s called from %s+0x%X", what, m->name,
                (unsigned)((unsigned char*)ret - m->base));
    else   logf("[cs] %s called from %p (outside every tracked module)", what, ret);

    // Stack bounds from the TIB, so the walk can never run off the end:
    // fs:[0x04] = StackBase (high address, exclusive), fs:[0x08] = StackLimit.
    unsigned char* base = (unsigned char*)__readfsdword(0x04);
    unsigned char* lim  = (unsigned char*)__readfsdword(0x08);
    unsigned char* sp   = (unsigned char*)&base;
    if (sp < lim || sp >= base) return;

    // Candidate frames: stack slots holding an address inside a module we care
    // about. The walk is bounded twice over -- by the TIB's own stack top and by
    // a slot budget -- so a deep stack cannot turn one poll into a long stall.
    int shown = 0;
    unsigned char* prev = 0;
    unsigned char** stop = (unsigned char**)base;
    unsigned char** p    = (unsigned char**)sp;
    for (int budget = 0; p < stop && budget < 4096 && shown < 12; p++, budget++) {
        unsigned char* v = *p;
        const CsMod* om = cs_owner(v);
        if (!om || v == prev) continue;
        prev = v;
        logf("[cs]     frame %d: %s+0x%X", shown, om->name, (unsigned)(v - om->base));
        shown++;
    }
}

// A candidate is an address plus HOW the pair is stored there. Tetra Master was
// not found as an int32 pair in any module, so a float pair is the next most
// likely shape and is scanned for in the same pass rather than in a second
// session.
struct FcCand { unsigned char* a; bool isfloat; };
static std::vector<FcCand> g_fc_cand;
static bool g_fc_started = false;
static bool g_fc_done    = false;
static int  g_fc_step    = 0;

// The scan runs ON the game's mouse-polling thread, from inside its
// GetDeviceState call -- so that thread's STACK is holding the very coordinates
// we are searching for (our own DIMOUSESTATE, the game's locals, the arguments
// in flight). Those slots match at every probe point and survive all five
// filters, which is exactly the false positive the first live run produced:
// 001AF2F0, reading x=1895446138 y=698709432 one poll later, i.e. reused stack.
// Stacks are excluded from the scan for that reason.
static bool fc_in_stack(unsigned char* p)
{
    int local;
    MEMORY_BASIC_INFORMATION here, there;
    if (VirtualQuery(&local, &here, sizeof(here)) != sizeof(here)) return false;
    if (VirtualQuery(p, &there, sizeof(there)) != sizeof(there)) return false;
    return here.AllocationBase == there.AllocationBase;
}

// Read a candidate as whatever it is. Floats are rounded, so the closed loop
// works in whole pixels either way.
static bool fc_load(const FcCand& c, LONG* x, LONG* y)
{
    __try {
        if (c.isfloat) {
            float fx = *(float*)c.a, fy = *(float*)(c.a + 4);
            *x = (LONG)(fx + (fx < 0 ? -0.5f : 0.5f));
            *y = (LONG)(fy + (fy < 0 ? -0.5f : 0.5f));
        } else {
            *x = *(LONG*)c.a; *y = *(LONG*)(c.a + 4);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Does this address hold (px,py) right now, in either encoding? Floats are
// compared with a half-unit tolerance -- a cursor integrated in floating point
// will not land on an exact integer.
static bool fc_match(unsigned char* a, LONG px, LONG py, bool isfloat)
{
    if (!isfloat) return *(LONG*)a == px && *(LONG*)(a + 4) == py;
    float fx = *(float*)a, fy = *(float*)(a + 4);
    return fx > px - 0.5f && fx < px + 0.5f && fy > py - 0.5f && fy < py + 0.5f;
}

// ===========================================================================
// CLOSED-LOOP CURSOR.
//
// The open-loop model below (m_vx/m_vy) assumes the game's cursor only ever
// moves because WE moved it. That is false: TM places the cursor itself on a
// dialog's default button, and the joypad-anchor path moves it too. Every such
// move desyncs the model permanently -- it is only ever re-synced by a re-home
// or by the pointer hitting a window edge -- and THAT is the residual drift.
//
// Once the finder above has located the game's own cursor variable we stop
// modelling and start READING it: delta = target - actual, recomputed every
// poll. Drift is then impossible by construction, and a game-side warp is
// corrected on the very next poll instead of persisting.
// ===========================================================================

static char  g_cur_mod[64] = "";     // module the cursor variable lives in ("" = bare VA)
static DWORD g_cur_rva     = 0;      // RVA within it, or the VA when g_cur_mod is empty
static bool  g_cur_have    = false;  // an address is configured
static LONG* g_cur_ptr     = NULL;   // resolved this session

// Does a mouse CreateDevice caller belong to the title the cursor fixes are for? The
// target title is named in dinput_cursor_xy (g_cur_xy_spec, e.g. "TM.dll+0x2FF59C") --
// NOT g_cur_mod, which is the separate, usually-empty dinput_cursor_addr. Compare the
// creating module to the module named there. If the spec has no module (a bare VA, or
// empty), keep the old global behaviour -- apply to every mouse.
static bool mouse_fix_title(void* ret)
{
    char want[64] = "";
    {
        char spec[128] = "";
        WideCharToMultiByte(CP_ACP, 0, g_cur_xy_spec, -1, spec, sizeof(spec), 0, 0);
        char* plus = strchr(spec, '+');
        if (plus && plus != spec) { *plus = '\0'; strncpy_s(want, spec, _TRUNCATE); }
    }
    if (!want[0]) return true;               // no module-scoped target -> global behaviour
    if (!ret) return false;
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)ret, &m) || !m)
        return false;
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;
    return _stricmp(leaf, want) == 0;
}
static bool  g_cur_float   = false;  // the pair is float, not int32
static int   g_cur_bad     = 0;      // consecutive polls the variable ignored our delta
static bool  g_cur_off     = false;  // gave up; fall back to the open-loop model

// Re-resolve the cursor variable from scratch on the next poll. TM.dll UNLOADS when the
// game exits and RELOADS at a DIFFERENT base on a second launch in the same Viewer
// session -- so the address cached from the first load (g_cur_xy_addr / g_cur_ptr) points
// into freed or reused memory. Writing to it faults, the mode disables itself, and the
// card cursor stops landing where you point on the SECOND launch (reported 2026-08-18:
// "small and the cursor wasn't aligned anymore"). Called when the cursor-fix title
// (re)creates its mouse device, which is exactly a fresh module load; a same-session
// re-resolve just finds the same base, so it is always safe.
static void cursor_reset_resolution()
{
    g_cur_xy_addr = 0; g_cur_xy_tried = false; g_cur_xy_off = false;   // dinput_mouseabs=2 path
    g_cur_ptr = NULL; g_cur_off = false; g_cur_bad = 0;                // the older steering path
    g_cur_float = false;
}

// "TM.dll+0x1234", "polcore.dll+0x1234" or a bare "0x04F9ABCD".
static void cursor_addr_parse(const wchar_t* spec)
{
    g_cur_have = false; g_cur_mod[0] = '\0'; g_cur_rva = 0;
    if (!spec || !spec[0]) return;
    char a[128];
    WideCharToMultiByte(CP_ACP, 0, spec, -1, a, sizeof(a), NULL, NULL);
    char* plus = strchr(a, '+');
    if (plus) {
        *plus = '\0';
        strncpy_s(g_cur_mod, a, _TRUNCATE);
        g_cur_rva = (DWORD)strtoul(plus + 1, NULL, 0);
    } else {
        g_cur_rva = (DWORD)strtoul(a, NULL, 0);
    }
    g_cur_have = (g_cur_rva != 0);
}

// Resolve once per session -- the module base moves between runs, the RVA does not.
static LONG* cursor_resolve()
{
    if (g_cur_ptr || !g_cur_have || g_cur_off) return g_cur_ptr;
    if (!g_cur_mod[0]) { g_cur_ptr = (LONG*)(uintptr_t)g_cur_rva; return g_cur_ptr; }
    unsigned char* base; size_t size;
    if (!fc_module_range(g_cur_mod, &base, &size)) return NULL;   // not loaded YET
    if (g_cur_rva + 8 > size) {
        logf("[cur*] configured address %s+0x%X is past the end of the module "
             "(%u bytes) -- ignoring it", g_cur_mod, g_cur_rva, (unsigned)size);
        g_cur_off = true;
        return NULL;
    }
    g_cur_ptr = (LONG*)(base + g_cur_rva);
    logf("[cur*] closed-loop cursor armed at %s+0x%X (%p)",
         g_cur_mod, g_cur_rva, g_cur_ptr);
    return g_cur_ptr;
}

// The variable is in our own address space, but a stale address from the ini
// could point anywhere, so the read is guarded rather than trusted.
static bool cursor_read(LONG* p, LONG* x, LONG* y)
{
    __try {
        if (g_cur_float) {
            float fx = ((float*)p)[0], fy = ((float*)p)[1];
            *x = (LONG)(fx + (fx < 0 ? -0.5f : 0.5f));
            *y = (LONG)(fy + (fy < 0 ? -0.5f : 0.5f));
        } else { *x = p[0]; *y = p[1]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("[cur*] FAULT reading the cursor variable at %p -- closed loop off, "
             "falling back to the open-loop model", p);
        g_cur_off = true;
        return false;
    }
}

// Persist a found address so the probe only ever runs once per install.
static void cursor_addr_save(const char* mod, DWORD rva, bool isfloat)
{
    if (!g_di_ini[0]) return;
    wchar_t v[128];
    _snwprintf_s(v, _TRUNCATE, L"%S+0x%X%s", mod, rva, isfloat ? L":float" : L"");
    if (WritePrivateProfileStringW(L"dx", L"dinput_cursor_addr", v, g_di_ini))
        logf("[cur*] saved dinput_cursor_addr=%s+0x%X%s -- the probe will not run "
             "again", mod, rva, isfloat ? ":float" : "");
    else
        logf("[cur*] could NOT write dinput_cursor_addr to the ini (err=%lu); set it "
             "by hand to skip the probe next time", GetLastError());
}

// Scan one module's committed R/W pages for an adjacent (px,py) int32 pair.
static void fc_scan_fresh(const char* mod, LONG px, LONG py)
{
    unsigned char* base; size_t size;
    if (!fc_module_range(mod, &base, &size)) return;
    unsigned char* p = base, *end = base + size;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        unsigned char* rend = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & w) && !(mbi.Protect & PAGE_GUARD)) {
            unsigned char* s = (unsigned char*)mbi.BaseAddress;
            if (s < base) s = base;
            unsigned char* e = (rend > end) ? end : rend;
            fc_scan_region(s, e, px, py);
        }
        p = rend;
    }
}

// Scan one region's worth of addresses. SEPARATE FUNCTION because it needs SEH,
// and a function using __try may not also need C++ unwinding -- and because the
// guard is the entire point: we are walking ~100 MB of a LIVE process's heap
// while its other threads keep allocating, freeing and re-protecting. A region
// can be handed back to the OS between the VirtualQuery that approved it and the
// read that touches it, and then the read is an access violation that kills the
// game. It killed it: pol.exe faulted at fc_scan_all+0x158, 0xC0000005, on the
// second live run. An earlier run survived the same code by luck.
static void fc_scan_region(unsigned char* s, unsigned char* rend, LONG px, LONG py)
{
    __try {
        for (unsigned char* a = s; a + 8 <= rend; a += 4) {
            if (fc_match(a, px, py, false)) { FcCand c = { a, false }; g_fc_cand.push_back(c); }
            else if (fc_match(a, px, py, true)) { FcCand c = { a, true }; g_fc_cand.push_back(c); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The region went away underneath us. It cannot have been the cursor --
        // the cursor is still being drawn -- so dropping it costs nothing.
    }
}

// Scan EVERY committed R/W page in the process, not just a named module. The
// cursor is as likely to sit in a heap allocation as in a module's .data, and
// the module-only scan is what came back empty on the first live run. Thread
// stacks are skipped -- see fc_in_stack for why they are guaranteed false
// positives here, not merely likely ones.
//
// EXPENSIVE AND INTRUSIVE: seconds of stall inside the game's own input
// callback. Only ever reached from an EXPLICIT probe (dinput_findcursor=1),
// never from the automatic one.
static void fc_scan_all(LONG px, LONG py)
{
    unsigned char* p = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    size_t bytes = 0, skipped = 0;
    while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        unsigned char* rend = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & w) && !(mbi.Protect & PAGE_GUARD)) {
            unsigned char* s = (unsigned char*)mbi.BaseAddress;
            if (fc_in_stack(s)) {
                skipped += mbi.RegionSize;
            } else {
                fc_scan_region(s, rend, px, py);
                bytes += mbi.RegionSize;
            }
        }
        if (rend <= p) break;            // wrapped / no progress
        p = rend;
    }
    logf("[fc] wide scan covered %u MB of committed R/W memory (%u KB of thread "
         "stack skipped -- it holds our own probe values)",
         (unsigned)(bytes / (1024 * 1024)), (unsigned)(skipped / 1024));
}

// One probe: collect on the first, intersect (filter) on the rest.
static void fc_pass(LONG px, LONG py, bool first)
{
    if (first) {
        g_fc_cand.clear();
        fc_scan_fresh("TM.dll", px, py);
        fc_scan_fresh("polcore.dll", px, py);
        if (g_fc_cand.empty()) {
            if (g_fc_wide) {
                logf("[fc] no candidate in TM.dll/polcore.dll -- widening to all "
                     "committed memory (slower; expect a hitch)");
                fc_scan_all(px, py);
            } else {
                logf("[fc] no candidate in TM.dll/polcore.dll. NOT widening: the "
                     "wide scan stalls the game for seconds inside its own input "
                     "callback. Set [dx] dinput_findcursor=1 for an explicit probe "
                     "that does search all of memory.");
            }
        }
        int nf = 0;
        for (size_t i = 0; i < g_fc_cand.size(); i++) if (g_fc_cand[i].isfloat) nf++;
        logf("[fc] probe (%ld,%ld): %u fresh candidates (%d int32, %d float)",
             px, py, (unsigned)g_fc_cand.size(), (int)g_fc_cand.size() - nf, nf);
    } else {
        std::vector<FcCand> keep;
        for (size_t i = 0; i < g_fc_cand.size(); i++)
            if (fc_match(g_fc_cand[i].a, px, py, g_fc_cand[i].isfloat))
                keep.push_back(g_fc_cand[i]);
        g_fc_cand.swap(keep);
        logf("[fc] probe (%ld,%ld): %u candidates remain",
             px, py, (unsigned)g_fc_cand.size());
    }
}

static void fc_finalize()
{
    int bw = 0, bh = 0;
    d3d8_backbuffer_size(&bw, &bh);
    if (bw <= 0 || bh <= 0) { bw = 640; bh = 480; }

    logf("[fc] DONE. %u surviving cursor-variable candidate(s):",
         (unsigned)g_fc_cand.size());
    for (size_t i = 0; i < g_fc_cand.size() && i < 32; i++) {
        char d[128];
        LONG x = 0, y = 0;
        fc_describe(g_fc_cand[i].a, d, sizeof(d));
        fc_load(g_fc_cand[i], &x, &y);
        logf("[fc]   candidate #%u: %s %s (x=%ld y=%ld now)", (unsigned)i, d,
             g_fc_cand[i].isfloat ? "float" : "int32", x, y);
    }
    if (g_fc_cand.empty()) {
        logf("[fc] none survived. The x,y pair may not be ADJACENT, or not stored "
             "as a plain pair at all. Next: scan the two axes independently and "
             "intersect the addresses.");
        return;
    }

    // PLAUSIBILITY GATE. A survivor must still hold a position inside the field
    // one poll later. The first live run armed on 001AF2F0, which read
    // x=1895446138 y=698709432 at this very point -- a stack slot that had already
    // been reused. Anything that is not a coordinate NOW was never the cursor,
    // and arming on it would have thrown the pointer across the screen for the
    // several polls the run-time validator needs to notice.
    {
        std::vector<FcCand> live;
        for (size_t i = 0; i < g_fc_cand.size(); i++) {
            LONG x, y;
            if (!fc_load(g_fc_cand[i], &x, &y)) continue;
            if (x >= 0 && x < bw && y >= 0 && y < bh) live.push_back(g_fc_cand[i]);
        }
        if (live.size() != g_fc_cand.size())
            logf("[fc] %u candidate(s) dropped: their values are no longer inside "
                 "the %dx%d field, so they were transient storage, not the cursor",
                 (unsigned)(g_fc_cand.size() - live.size()), bw, bh);
        g_fc_cand.swap(live);
    }
    if (g_fc_cand.empty()) {
        logf("[fc] nothing plausible left -- closed loop NOT armed, open-loop "
             "tracking continues unchanged");
        return;
    }

    // LATCH THE WINNER. Prefer one inside a module: a module+RVA is stable across
    // runs and can be written to the ini, where a bare VA is good for this session
    // only. The closed loop validates whichever we pick and drops it if it turns
    // out to be a decoy, so picking the first is safe.
    FcCand pick = g_fc_cand[0];
    char mod[64] = ""; DWORD rva = 0;
    for (size_t i = 0; i < g_fc_cand.size() && !mod[0]; i++) {
        const char* mods[] = { "TM.dll", "polcore.dll", "pol.exe" };
        for (int k = 0; k < 3; k++) {
            unsigned char* b; size_t sz;
            if (fc_module_range(mods[k], &b, &sz) &&
                g_fc_cand[i].a >= b && g_fc_cand[i].a < b + sz) {
                pick = g_fc_cand[i];
                strncpy_s(mod, mods[k], _TRUNCATE);
                rva = (DWORD)(pick.a - b);
                break;
            }
        }
    }

    g_cur_ptr    = (LONG*)pick.a;
    g_cur_float  = pick.isfloat;
    g_cur_have   = true;
    g_cur_off    = false;
    g_cur_bad    = 0;
    if (mod[0]) {
        strncpy_s(g_cur_mod, mod, _TRUNCATE);
        g_cur_rva = rva;
        logf("[fc] closed loop ARMED on %s+0x%X (%s)", mod, rva,
             pick.isfloat ? "float" : "int32");
        cursor_addr_save(mod, rva, pick.isfloat);
    } else {
        g_cur_mod[0] = '\0';
        g_cur_rva = 0;
        logf("[fc] closed loop ARMED on %p (%s; not in a module, so NOT saved to "
             "the ini -- a bare address is not stable across runs and the probe "
             "will run again next session)", pick.a,
             pick.isfloat ? "float" : "int32");
    }
}

// Start the probe BY ITSELF, once, when absolute mode is running without a known
// cursor address. Waiting a few hundred polls matters: the probe is meaningless
// before the game is up, focused and actually drawing a cursor, and it costs a
// visible ~2 s of the pointer flying to five corners. The address it finds is
// written to the ini, so this happens once per install and never again.
static void maybe_autofind()
{
    static LONG settled = 0;
    static bool fired = false;
    if (fired || !g_di_autofind || !g_di_abs) return;
    if (g_di_findcursor || g_fc_done) return;        // manual probe, or already run
    if (g_cur_have && !g_cur_off) return;            // address already known
    HWND gw = d3d8_game_window();
    if (!gw || !IsWindow(gw) || GetForegroundWindow() != gw) { settled = 0; return; }
    if (InterlockedIncrement(&settled) < g_di_autofind_at) return;
    fired = true;
    g_di_findcursor = 1;
    logf("[cur*] no cursor address known -- starting the finder automatically "
         "(the pointer will sweep the corners for ~2 s; this happens once)");
}

// Drive the probe from GetDeviceState by writing lX/lY. Returns nothing; it owns
// the mouse deltas while running. Point layout is spread across the field so a
// false-positive pair cannot track all five.
static void fc_run(DIMOUSESTATE* m)
{
    static const struct { LONG x, y; } PT[] = {
        {120, 90}, {520, 90}, {520, 390}, {120, 390}, {300, 240}
    };
    const int NPT = 5, HOME = 8, HOLD = 12, PER = HOME + 1 + HOLD;

    if (g_fc_done) { m->lX = 0; m->lY = 0; return; }
    if (!g_fc_started) { g_fc_started = true; g_fc_step = 0;
        logf("[fc] cursor-variable finder started (%d probe points)", NPT); }

    int pt = g_fc_step / PER, within = g_fc_step % PER;
    if (pt >= NPT) { fc_finalize(); g_fc_done = true; m->lX = 0; m->lY = 0; return; }

    LONG px = PT[pt].x, py = PT[pt].y;
    if (within < HOME) { m->lX = -30000; m->lY = -30000; }   // slam to (0,0)
    else if (within == HOME) { m->lX = px; m->lY = py; }     // jump to the point
    else {                                                    // hold, scan at end
        m->lX = 0; m->lY = 0;
        if (within == PER - 1) fc_pass(px, py, pt == 0);
    }
    g_fc_step++;
}

// ---------------------------------------------------------------------------
// device wrapper (mouse only)
// ---------------------------------------------------------------------------

class DIDeviceWrap : public IDirectInputDevice8A
{
    IDirectInputDevice8A* m_real;
    LONG m_ref;
    // THE SYSTEM KEYBOARD. A third kind, added 2026-08-26 for one reason only:
    // the coop level. A title's keyboard SetCooperativeLevel is where
    // DISCL_NOWINKEY and DISCL_EXCLUSIVE actually decide whether the Windows key
    // works, and until this device was wrapped that call was invisible -- the log
    // recorded `CreateDevice(GUID_SysKeyboard)` and then nothing, for every
    // session in this project's history. Sharing the wrapper is deliberate and is
    // the same argument the mouse/pad note makes -- a second passthrough class is
    // 30 more chances to forward one slot wrong.
    //
    // 2026-09-09: it is no longer the coop level ALONE. Both keyboard read paths
    // now go through kbd_state()/kbd_data() -- seam 4 of the focus gate, plus the
    // first counters this project has ever had on them. See those two functions.
    // (The transferable lesson from the bug they fix: a wrapper shared for one
    // method opts the device into every rewrite that wrapper performs. That is
    // how the NOWINKEY strip silently handed the titles the keyboard.)
    bool m_kbd;
    // WHAT WE HAVE TOLD THIS DEVICE'S READER IS HELD.
    //
    // The held-button trap, in keyboard form. A buffered reader learns a key was
    // released only by being TOLD; cut it off mid-keystroke and it believes the
    // key is still down for ever, which for a movement key means the character
    // walks into a wall until you come back -- strictly worse than the bug being
    // fixed. So we remember what we handed it and drain the ups on the way out.
    //
    // Indexed by DIK scan code (dwOfs). uAppData is remembered alongside,
    // because a title using DirectInput action mapping keys its handler off
    // uAppData and would ignore a synthetic up that carried the wrong one.
    BYTE      m_kbd_held[256];
    UINT_PTR  m_kbd_app[256];
    bool      m_kbd_gate_prev;
    bool m_pad;               // GAME CONTROLLER rather than the mouse: the two share this
                              // wrapper because everything except GetDeviceState /
                              // GetDeviceData is an identical forward, and a second
                              // 30-method passthrough class would be 30 more chances to
                              // forward one slot wrong.
    LONG m_cx, m_cy;          // scaling carry, per axis
    // Absolute-tracking state. m_vx/m_vy is our model of the game's internal
    // cursor in backbuffer pixels; m_vinit says it is valid; m_home counts down
    // the "slam to (0,0)" polls that establish that model against the game's own
    // clamp, since we cannot read the game's cursor directly.
    LONG m_vx, m_vy;
    bool m_vinit;
    int  m_home;
    // Closed-loop bookkeeping: what we read and what we commanded last poll, so
    // the next read can confirm the variable is really the cursor.
    LONG m_cl_px, m_cl_py, m_cl_dx, m_cl_dy;
    bool m_cl_prev;
    LONG m_gx, m_gy;          // gain-divider carry, per axis
    // Ownership bookkeeping for the direct mode. m_own_mouse says the pointer is
    // currently driving; m_wrote_* is the value we last stored, so a later read
    // that disagrees with it is the GAME having moved its own cursor; m_last_p is
    // the OS pointer as of the previous poll, which is how "the user actually
    // moved the mouse" is told apart from "the mouse is sitting still".
    bool  m_own_mouse;
    bool  m_wrote_valid;
    LONG  m_wrote_x, m_wrote_y;
    POINT m_last_p;
    bool  m_last_p_valid;
    // Has absolute tracking ever actually STEERED this device? Only the polled
    // path (GetDeviceState) can steer, so this stays false for a title that reads
    // the mouse buffered. GetDeviceData consults it to decide whether skipping
    // its scale term is safe -- see the comment there.
    bool  m_abs_steering;
    // Does THIS mouse device belong to the title the cursor fixes are for
    // (dinput_mouseabs / dinput_mousescale = Tetra Master)? Set at creation from the
    // creating module. When false -- Fantasy Earth, FMO, FFXI -- the device is left
    // STOCK: no scale, no cursor write. Those fixes write TM's own cursor variable and
    // scale relative counts, and on a title that reads its mouse buffered (FE) they make
    // "the cursor run away from the pointer" -- the shim's own note at GetDeviceData, and
    // the account holder's report to the letter. Default true so a mouse created before
    // the tag is set behaves as it always did.
    bool  m_mousefix;
    // Focus-gate edge state, per DEVICE. A title can hold more than one mouse
    // device open across a title change, and each needs its own release.
    bool  m_gate_prev;
    // For a PAD device: the profile of the TITLE whose code called CreateDevice, or NULL
    // when the shell (polcore/app.dll) created it. padmap uses it to permute a title's
    // pad against THAT title's own button order -- each title reads the pad in its own
    // order, so the shell's permutation is wrong for every one of them.
    const TitleProfile* m_padowner;
public:
    void set_mousefix(bool on) { m_mousefix = on; }
    void set_padowner(const TitleProfile* p) { m_padowner = p; }
    explicit DIDeviceWrap(IDirectInputDevice8A* r, bool pad = false, bool kbd = false)
        : m_real(r), m_ref(1), m_kbd(kbd), m_pad(pad), m_cx(0), m_cy(0),
          m_vx(0), m_vy(0), m_vinit(false), m_home(0),
          m_cl_px(0), m_cl_py(0), m_cl_dx(0), m_cl_dy(0), m_cl_prev(false),
          m_gx(0), m_gy(0),
          m_own_mouse(false), m_wrote_valid(false), m_wrote_x(0), m_wrote_y(0),
          m_last_p_valid(false), m_abs_steering(false), m_mousefix(true),
          m_gate_prev(false),
          m_padowner(NULL),
          m_clipped(false)
    {
        m_last_p.x = m_last_p.y = 0;
        m_kbd_gate_prev = false;
        ZeroMemory(m_kbd_held, sizeof(m_kbd_held));
        ZeroMemory(m_kbd_app,  sizeof(m_kbd_app));
    }

    // Names this device in the log. Three kinds share one wrapper, so a bare
    // "mouse" on a keyboard line would be a lie in the one place the reader is
    // relying on the log to tell them which device set which flag.
    const char* kind_name() const { return m_kbd ? "keyboard" : (m_pad ? "pad" : "mouse"); }

private:
    // Confirm the address really is the cursor. A false positive that survived
    // all five probe points would steer the pointer wildly, so: whenever we
    // commanded a decisive move on an axis whose value was not already pinned at
    // the edge of the field, that value must have CHANGED by the next poll. It
    // does not have to match exactly -- the game may clamp, round or rate-limit
    // -- it just has to respond. Enough consecutive non-responses and we drop
    // back to the open-loop model rather than fight the user.
    void validate_closed_loop(LONG ax, LONG ay, int bw, int bh)
    {
        if (!m_cl_prev) return;
        bool tested = false, dead = false;
        if (m_cl_dx <= -4 || m_cl_dx >= 4) {
            if (m_cl_px > 2 && m_cl_px < bw - 3) { tested = true; if (ax == m_cl_px) dead = true; }
        }
        if (m_cl_dy <= -4 || m_cl_dy >= 4) {
            if (m_cl_py > 2 && m_cl_py < bh - 3) { tested = true; if (ay == m_cl_py) dead = true; }
        }
        if (!tested) return;
        if (!dead) { g_cur_bad = 0; return; }
        if (++g_cur_bad >= 8) {
            logf("[cur*] the variable at %p did not respond to %d decisive moves "
                 "-- it is NOT the cursor. Closed loop off; reverting to the "
                 "open-loop model. Clear [dx] dinput_cursor_addr to re-probe.",
                 g_cur_ptr, g_cur_bad);
            g_cur_off = true;
        }
    }

    // Replace the mouse's relative lX/lY with a delta that steers the game's
    // cursor to where the OS pointer actually is, mapped into the game window's
    // client area. The game integrates relative counts into its own clamped
    // cursor, so we (1) slam it to (0,0) for the first g_di_abs_home polls after
    // an acquire/refocus -- the game's own clamp makes that a KNOWN reference we
    // never have to read -- then (2) feed exact deltas toward the live target.
    // Fixes the constant offset that plain speed-scaling leaves behind.
    // Returns TRUE when it actually steered. FALSE means it could not (no game
    // window yet, or the game is not foreground) and the CALLER must decide what
    // the untouched raw deltas become -- see GetDeviceState. Measured on a real
    // session: GetDeviceState=23930 but abs=7955, so two polls in three take one
    // of these early exits, and every one of them used to hand the game raw
    // device counts.
    // Where the game's cursor SHOULD be this poll: the OS pointer, mapped
    // through the game window's client area onto the backbuffer field. Split out
    // of apply_absolute so the direct-write mode below can share it -- the two
    // modes differ only in what they do with the answer, and duplicating this
    // mapping is exactly how d3d9hook drifted out of parity with d3d8hook.
    // The whole mapping, in one place, because the direct mode needs it in BOTH
    // directions: pointer -> field to place the cursor, and field -> pointer to
    // park the pointer back on a cursor the game moved itself.
    struct AbsMap {
        HWND  gw;
        RECT  src;        // the game window's client area, in screen pixels
        LONG  sw, sh;
        int   bw, bh;     // the game's own coordinate field (the backbuffer)
        POINT p;          // the true OS pointer, in screen pixels
        LONG  tx, ty;     // p mapped into the field
    };

    bool abs_target(LONG* tx_out, LONG* ty_out, int* bwo, int* bho)
    {
        AbsMap a;
        if (!abs_map(&a)) return false;
        *tx_out = a.tx; *ty_out = a.ty;
        if (bwo) *bwo = a.bw;
        if (bho) *bho = a.bh;
        return true;
    }

    // Map the field back onto the screen: the CENTRE of the cell (gx,gy) occupies,
    // so that re-mapping the result forward lands on (gx,gy) again and parking the
    // pointer cannot drift the cursor by a pixel per poll.
    static void abs_unmap(const AbsMap& a, LONG gx, LONG gy, POINT* out)
    {
        out->x = a.src.left + (LONG)(((__int64)gx * 2 + 1) * a.sw / (2 * a.bw));
        out->y = a.src.top  + (LONG)(((__int64)gy * 2 + 1) * a.sh / (2 * a.bh));
    }

    bool abs_map(AbsMap* a)
    {
        HWND gw = d3d8_game_window();
        if (!gw || !IsWindow(gw) || GetForegroundWindow() != gw) {
            release_clip();
            m_vinit = false;            // re-home when the game regains focus
            m_cl_prev = false;          // and don't validate across the gap
            return false;
        }
        RECT c;
        if (!GetClientRect(gw, &c) || c.right <= 0 || c.bottom <= 0) {
            m_vinit = false;
            m_cl_prev = false;
            return false;
        }

        // The game's own coordinate field is the BACKBUFFER, not the window's
        // client area -- they diverge the moment the window is resized, and D3D
        // stretches the backbuffer to fill the client. Map into the backbuffer
        // so tracking is correct at any window size. Fall back to the client
        // size if the backbuffer is not known yet.
        int bw = 0, bh = 0;
        d3d8_backbuffer_size(&bw, &bh);
        if (bw <= 0 || bh <= 0) { bw = c.right; bh = c.bottom; }

        // The SOURCE region is the game window's CLIENT area (in screen pixels):
        // we stretch it onto the backbuffer field so the in-game cursor sits
        // exactly under the real pointer wherever it is inside the window. clip
        // only decides whether the pointer is CONFINED to the window (penned, so
        // it can never leave and desync) or free (it may leave, and then the
        // game cursor simply pins at the nearest edge until it returns). Make the
        // window bigger to make leaving it rarer.
        if (g_di_abs_clip) clip_to_window(gw); else release_clip();
        RECT src;
        {
            POINT o = { 0, 0 };
            ClientToScreen(gw, &o);
            src.left = o.x; src.top = o.y;
            src.right = o.x + c.right; src.bottom = o.y + c.bottom;
        }
        LONG sw = src.right - src.left, sh = src.bottom - src.top;
        if (sw <= 0 || sh <= 0) return false;

        // True OS pointer, in screen pixels. Use the shim's saved real
        // GetCursorPos so d3d8hook's own GetCursorPos hook can't double-correct;
        // fall back to the plain API when that hook is disabled (unhooked then).
        typedef BOOL (WINAPI *PFN_GCP)(LPPOINT);
        PFN_GCP gcp = (PFN_GCP)d3d8_real_GetCursorPos();
        POINT p;
        if (gcp) { if (!gcp(&p)) return false; }
        else     { if (!GetCursorPos(&p)) return false; }

        // source region -> backbuffer field. This is the TARGET: where the game's
        // cursor should be for it to sit under the real pointer.
        LONG tx = (LONG)((__int64)(p.x - src.left) * bw / sw);
        LONG ty = (LONG)((__int64)(p.y - src.top)  * bh / sh);
        if (tx < 0) tx = 0; else if (tx > bw - 1) tx = bw - 1;
        if (ty < 0) ty = 0; else if (ty > bh - 1) ty = bh - 1;
        a->gw = gw;
        a->src = src; a->sw = sw; a->sh = sh;
        a->bw = bw;   a->bh = bh;
        a->p = p;
        a->tx = tx;   a->ty = ty;
        return true;
    }

    // =======================================================================
    // DIRECT MODE (dinput_mouseabs=2) -- SET the cursor instead of steering it.
    //
    // Everything else in this file feeds the game deltas and hopes its own
    // integration turns them into the position we wanted. It does not have to
    // be a hope. Tetra Master's cursor is a pair of int16s at a fixed RVA, its
    // own code clamps them to 0..639 / 0..479, and the path from device count
    // to that pair is `add` at unity (measured end to end).
    // So: compute where the pointer is in the game's field and store it.
    //
    //     TM.dll+0x2FF59C  int16 cursor x   (game clamps 0..639)
    //     TM.dll+0x2FF59E  int16 cursor y   (game clamps 0..479)
    //
    // The deltas are zeroed in the same breath. The game adds this frame's
    // accumulated movement to the same words a moment later (TM.dll+0x189360),
    // so leaving them live would put our absolute position PLUS one frame of
    // relative motion on screen. Zeroing them makes this the only writer of the
    // position while leaving buttons and wheel entirely alone.
    //
    // Why this beats the closed loop above, which reads the same variable: that
    // one still has to express its correction as a delta, so it inherits the
    // game's spike filter (>100 counts on an axis and the poll is silently
    // dropped, TM.dll+0x185BBA) and any frame in which the game moves its own
    // cursor. A store has neither failure mode. It is also the only mode that
    // needs no gain, no homing and no model.
    //
    // It is title-specific by construction -- the RVA is Tetra Master's -- which
    // is why it is opt-in per install rather than the default, and why the
    // address is a setting rather than a constant.
    //
    // -----------------------------------------------------------------------
    // OWNERSHIP -- why the store is CONDITIONAL (dinput_abs_yield)
    //
    // Storing the position on EVERY poll made the shim the only writer of the
    // cursor, and that is one writer too many. The game moves that same pair
    // itself, for two reasons that both matter:
    //
    //   * joypad navigation. The branch at TM.dll+0x1892FE (taken while the
    //     glide counter at singleton+0x14 is set) interpolates the cursor from
    //     +0x10 toward the anchor of the item the pad selected. Writing the
    //     pointer's position 60 times a second erases every frame of that
    //     glide, so the pad appears to do nothing but snap back to the mouse.
    //   * screens that HOLD the cursor on something -- a dialog's default
    //     button, a selection the game wants kept while you look elsewhere.
    //     Same erasure, same frame.
    //
    // The Viewer never has this problem because pad navigation and the mouse
    // cursor are two separate things there (pol.exe's UseGameController branches
    // ADD pad navigation; they do not take the mouse away -- see
    // inputmode.cpp). Tetra Master instead runs BOTH through this
    // one variable, and arbitrates natively by last-writer-wins: whoever moved
    // most recently is who the cursor is following. An unconditional store is
    // simply not a participant in that scheme -- it always writes last.
    //
    // So participate properly. The mouse owns the cursor only while the mouse is
    // MOVING; the moment the game moves the cursor itself and the mouse is at
    // rest, ownership goes back to the game and we stop writing until real
    // motion arrives. That is TM's own rule, applied to an absolute source.
    //
    // With dinput_abs_follow on, the OS pointer is parked on top of the cursor
    // for as long as the game owns it. That makes the handover free in both
    // directions -- the pad leaves the cursor on a menu item, the pointer is
    // already there, and the next nudge of the mouse continues from that item
    // instead of teleporting across the screen.
    //
    //     while the MOUSE owns it: the game's cursor is glued to the pointer
    //     while the GAME owns it:  the pointer is glued to the game's cursor
    // =======================================================================
    bool apply_direct(DIMOUSESTATE* m)
    {
        short* cur = (short*)cursor_xy_resolve();
        if (!cur) return false;

        // yield=0 is the pre-2026-08-17 contract: the mouse owns the cursor
        // unconditionally and nothing else ever gets a turn. Say so up front so it
        // holds from the very first poll rather than from the first mouse movement.
        if (!g_di_abs_yield) m_own_mouse = true;

        AbsMap a;
        if (!abs_map(&a)) { m_last_p_valid = false; return false; }

        // Where the cursor is right now. Reading it is what makes the game a
        // visible participant: a value that is not the one we stored can only
        // have come from the game.
        LONG ax = 0, ay = 0;
        bool haveact = false;
        __try {
            ax = cur[0]; ay = cur[1];
            haveact = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_cur_xy_off = true;
            logf("[cur2] direct read faulted -- disabling dinput_mouseabs=2");
            return false;
        }

        // Did the USER move the mouse this poll? Two independent witnesses,
        // because either one alone has a blind spot: the OS pointer stops at the
        // edge of the screen while the hand keeps going, and the device counts
        // say nothing about a pointer moved by something other than this mouse.
        // dinput_abs_wake is the noise floor -- a sensor that twitches by a count
        // must not take the cursor off the pad mid-menu.
        LONG dev = (m->lX < 0 ? -m->lX : m->lX) + (m->lY < 0 ? -m->lY : m->lY);
        bool moved = m_last_p_valid &&              // first poll: no reference yet
                     (a.p.x != m_last_p.x || a.p.y != m_last_p.y ||
                      dev >= g_di_abs_wake);

        // Did the GAME move it since our last store?
        bool gamemoved = g_di_abs_yield && m_wrote_valid && haveact &&
                         (ax != m_wrote_x || ay != m_wrote_y);

        if (moved) {
            // Real motion always wins, even in the same poll the game moved the
            // cursor -- the user is pointing at something and that is the whole
            // contract of an absolute mouse.
            if (!m_own_mouse) {
                m_own_mouse = true;
                LONG n = InterlockedIncrement(&g_n_wake);
                if (g_di_trace && (n <= 8 || (n % 200) == 0))
                    logf("[cur2] mouse takes the cursor (dev=%ld, cursor at %ld,%ld)",
                         dev, ax, ay);
            }
        } else if (gamemoved && m_own_mouse) {
            m_own_mouse = false;
            LONG n = InterlockedIncrement(&g_n_yield);
            if (g_di_trace && (n <= 8 || (n % 200) == 0))
                logf("[cur2] game moved the cursor (%ld,%ld)->(%ld,%ld) and the mouse "
                     "is at rest -- yielding", m_wrote_x, m_wrote_y, ax, ay);
        }

        m_last_p = a.p;
        m_last_p_valid = true;

        if (!m_own_mouse) {
            // The game (or the pad through it) is driving. Do not touch the
            // cursor, and do not touch the deltas either: leaving them live means
            // a small hand movement still nudges the cursor through the game's own
            // `add`, exactly as it would with no shim at all -- and the wake test
            // above promotes anything larger to a proper handover.
            m_wrote_valid = false;
            if (g_di_abs_follow && haveact) {
                POINT want;
                abs_unmap(a, ax, ay, &want);
                if (want.x != a.p.x || want.y != a.p.y) {
                    typedef BOOL (WINAPI *PFN_SCP)(int, int);
                    PFN_SCP scp = (PFN_SCP)d3d8_real_SetCursorPos();
                    if (scp) scp(want.x, want.y); else SetCursorPos(want.x, want.y);
                    // Read it BACK rather than assume it landed: a clip rect, a
                    // monitor edge or DPI rounding can all refuse part of the move,
                    // and a reference that is one pixel off the truth would read as
                    // the user moving the mouse on every single poll from here on.
                    typedef BOOL (WINAPI *PFN_GCP)(LPPOINT);
                    PFN_GCP gcp = (PFN_GCP)d3d8_real_GetCursorPos();
                    POINT got = want;
                    if (gcp) gcp(&got); else GetCursorPos(&got);
                    m_last_p = got;    // our own warp is not the user moving the mouse
                }
            }
            return true;               // handled: no scaling fallback on top
        }

        // Zeroing the deltas is how the store stays the ONLY writer of the
        // position: the game adds this frame's accumulated movement to the same
        // words a moment later (0x17FBC0 -> 0x189360), so live deltas would put
        // our absolute position PLUS a frame of relative motion on screen.
        //
        // dinput_abs_keepdelta buys the deltas back for anything that reads MOTION
        // rather than position -- a screen that pans, drags or flicks -- by
        // pre-subtracting exactly what the game is about to add instead of
        // destroying it. Same landing point, delta intact. It is OFF by default
        // because it depends on predicting the game's own arithmetic: the poll
        // silently discards both counts when either axis exceeds 100 (0x185BBA),
        // and any frame where the prediction is wrong is a frame the cursor sits
        // one movement away from the pointer. Compensated here, but only worth the
        // risk once a screen has actually shown it needs the motion.
        LONG kx = 0, ky = 0;
        if (g_di_abs_keepdelta &&
            m->lX <= 100 && m->lX >= -100 && m->lY <= 100 && m->lY >= -100) {
            kx = m->lX; ky = m->lY;
        }
        __try {
            cur[0] = (short)(a.tx - kx);
            cur[1] = (short)(a.ty - ky);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // The address is configured, so a fault means it is WRONG (or the
            // module moved). Never retry -- a bad pointer written every poll is
            // a crash waiting to happen.
            g_cur_xy_off = true;
            logf("[cur2] direct write faulted -- disabling dinput_mouseabs=2");
            return false;
        }
        // What the cursor should READ as next poll, i.e. after the game's own add.
        m_wrote_x = a.tx; m_wrote_y = a.ty;
        m_wrote_valid = true;

        if (!g_di_abs_keepdelta) { m->lX = 0; m->lY = 0; }
        LONG n = InterlockedIncrement(&g_n_direct);
        if (g_di_trace && (n <= 3 || (n % 1800) == 0))
            logf("[cur2] direct: cursor <- (%ld,%ld)  poll #%ld", a.tx, a.ty, n);
        return true;
    }

    bool apply_absolute(DIMOUSESTATE* m)
    {
        LONG tx, ty; int bw, bh;
        if (!abs_target(&tx, &ty, &bw, &bh)) return false;

        // CLOSED LOOP: read where the game's cursor actually is and aim from
        // there. No model to desync, so no homing, no drift, and a warp the game
        // performs itself (dialog default button, joypad anchor) is undone on the
        // next poll rather than offsetting everything from then on.
        LONG* cp = cursor_resolve();
        if (cp && !g_cur_off) {
            LONG ax, ay;
            if (cursor_read(cp, &ax, &ay)) {
                validate_closed_loop(ax, ay, bw, bh);
                if (!g_cur_off) {
                    LONG dx = tx - ax, dy = ty - ay;
                    m_cl_px = ax; m_cl_py = ay;
                    m_cl_dx = dx; m_cl_dy = dy;
                    m_cl_prev = true;
                    m_vinit = false;      // the open-loop model is stale; re-home if we fall back
                    m->lX = gain_axis(dx, &m_gx);
                    m->lY = gain_axis(dy, &m_gy);
                    LONG nc = InterlockedIncrement(&g_n_abs);
                    if (g_di_trace && (nc < 8 || (nc % 900) == 0))
                        logf("[cur*] CLOSED target=(%ld,%ld) actual=(%ld,%ld) "
                             "delta=(%ld,%ld) field=%dx%d", tx, ty, ax, ay, dx, dy, bw, bh);
                    return true;
                }
            }
        }

        // OPEN LOOP (fallback). Homing: for the first few polls after acquire,
        // slam the game cursor toward (0,0); the game clamps and lands there,
        // giving us a known reference we can never read directly. Simple fixed
        // count -- the proven version. (A cleverer "hold until first movement"
        // was tried and made mid-session re-homes disruptive, so it was reverted.)
        if (!m_vinit) { m_vinit = true; m_vx = 0; m_vy = 0; m_home = g_di_abs_home; }
        if (m_home > 0) {
            m_home--;
            m->lX = -30000; m->lY = -30000;
            m_vx = 0; m_vy = 0;
            return true;
        }

        // Ask for delta/k, because the game will move k times what we ask. The
        // MODEL still advances to the target: the game applies k to our reduced
        // request and lands on it. With gain=100 this is the identity.
        LONG dx = tx - m_vx, dy = ty - m_vy;
        m_vx = tx; m_vy = ty;
        m->lX = gain_axis(dx, &m_gx);
        m->lY = gain_axis(dy, &m_gy);
        LONG na = InterlockedIncrement(&g_n_abs);
        if (g_di_trace && (na < 8 || (na % 900) == 0))
            logf("[din] ABS target=(%ld,%ld) delta=(%ld,%ld) sent=(%ld,%ld) "
                 "gain=%d%% field=%dx%d",
                 tx, ty, dx, dy, m->lX, m->lY, g_di_abs_gain, bw, bh);
        return true;
    }

    // Confine / release the OS pointer to the game window rect. We track whether
    // WE applied a clip so we only ever release our own, and re-issue it each
    // poll so it follows the window if it is moved or resized.
    bool m_clipped;
    void clip_to_window(HWND gw)
    {
        if (!g_di_abs_clip) return;
        RECT wr;
        if (!GetWindowRect(gw, &wr)) return;
        typedef BOOL (WINAPI *PFN_CLIP)(const RECT*);
        PFN_CLIP clip = (PFN_CLIP)d3d8_real_ClipCursor();
        if (clip) clip(&wr); else ClipCursor(&wr);
        m_clipped = true;
    }
    void release_clip()
    {
        if (!m_clipped) return;
        m_clipped = false;
        typedef BOOL (WINAPI *PFN_CLIP)(const RECT*);
        PFN_CLIP clip = (PFN_CLIP)d3d8_real_ClipCursor();
        if (clip) clip(NULL); else ClipCursor(NULL);
    }

public:

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInputDevice8A) {
            *ppv = static_cast<IDirectInputDevice8A*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { release_clip(); m_real->Release(); delete this; return 0; }
        return (ULONG)r;
    }

    STDMETHODIMP GetCapabilities(LPDIDEVCAPS p) { return m_real->GetCapabilities(p); }
    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID r, DWORD f)
        { return m_real->EnumObjects(cb, r, f); }
    STDMETHODIMP GetProperty(REFGUID g, LPDIPROPHEADER p) { return m_real->GetProperty(g, p); }
    STDMETHODIMP SetProperty(REFGUID g, LPCDIPROPHEADER p) { return m_real->SetProperty(g, p); }
    STDMETHODIMP Acquire()
    {
        if (!m_pad && !m_kbd) di_report_callsite("mouse Acquire", _ReturnAddress());
        return m_real->Acquire();
    }
    STDMETHODIMP Unacquire() { release_clip(); return m_real->Unacquire(); }

    // -----------------------------------------------------------------
    // SEAM 4 -- THE DIRECTINPUT KEYBOARD
    //
    // IMPORTANT: THIS PATH IS OURS. Measured live on 2026-09-09
    // (polshim.412476.log, immediately after FE_Client.dll loaded):
    //
    //   [din] keyboard SetCooperativeLevel(hwnd=..., NONEXCLUSIVE|FOREGROUND|NOWINKEY)
    //                                            -> NONEXCLUSIVE|BACKGROUND
    //
    // The title asked for FOREGROUND -- "stop feeding me when I am not the
    // window in front", exactly what the player wants -- and dinput_background
    // overrode it. That rewrite exists for FE's mouse BUTTONS; the keyboard
    // inherited it on 2026-08-26 by joining this wrapper to strip NOWINKEY.
    //
    // The coop level is NOT put back. See SetCooperativeLevel: FOREGROUND
    // unacquires on focus loss, and a title that mishandles DIERR_NOTACQUIRED
    // loses its keyboard for the whole session. Gate DELIVERY, never
    // ACQUISITION -- the same rule, for the same reason, as the mouse.
    // -----------------------------------------------------------------
    HRESULT kbd_state(HRESULT hr, DWORD cb, LPVOID data)
    {
        InterlockedIncrement(&g_n_kbdstate);
        bool blocked = false;
        int  edge = inputgate_key_edge(&m_kbd_gate_prev, &blocked);
        if (blocked) {
            InterlockedIncrement(&g_n_kbdgated);
            inputgate_note_key_blocked(4, 0);
            // Zeroing IS the release for a polled reader: no key held, every
            // poll, for as long as you are away. Nothing accumulates, so there
            // is nothing to unwind when you come back.
            if (SUCCEEDED(hr) && data && cb) ZeroMemory(data, cb);
            ZeroMemory(m_kbd_held, sizeof(m_kbd_held));
            if (edge)
                logf("[gate] keyboard (polled): another application is in front "
                     "-- delivering an all-keys-up state to the title until you "
                     "return. The device stays ACQUIRED "
                     "([dx] key_focus_gate=0 to stop)");
            return hr;
        }
        // Not blocked: remember what the title has been told is down, so the
        // buffered half has something to release if the focus goes while a key
        // is held. cb is the caller's buffer size -- a keyboard data format is
        // 256 bytes, but never trust that over what we were handed.
        if (SUCCEEDED(hr) && data && cb) {
            DWORD n = cb < 256 ? cb : 256;
            const BYTE* k = (const BYTE*)data;
            for (DWORD i = 0; i < n; i++) m_kbd_held[i] = (BYTE)((k[i] & 0x80) ? 1 : 0);
        }
        return hr;
    }

    HRESULT kbd_data(HRESULT hr, DWORD cbOd, LPDIDEVICEOBJECTDATA rgod,
                     LPDWORD pdwInOut, DWORD fl)
    {
        InterlockedIncrement(&g_n_kbddata);
        bool blocked = false;
        int  edge = inputgate_key_edge(&m_kbd_gate_prev, &blocked);
        if (!blocked) {
            // Record what this batch tells the title. DIK code in dwOfs, high
            // bit of dwData = down. PEEK batches are recorded too: a peeked
            // event is still one the title will act on.
            if (SUCCEEDED(hr) && rgod && pdwInOut && cbOd >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
                DWORD got = *pdwInOut;
                for (DWORD i = 0; i < got; i++) {
                    const DIDEVICEOBJECTDATA* e =
                        (const DIDEVICEOBJECTDATA*)((const char*)rgod + (size_t)i * cbOd);
                    if (e->dwOfs > 255) continue;
                    m_kbd_held[e->dwOfs] = (BYTE)((e->dwData & 0x80) ? 1 : 0);
                    if (cbOd >= sizeof(DIDEVICEOBJECTDATA))
                        m_kbd_app[e->dwOfs] = e->uAppData;
                }
            }
            return hr;
        }

        InterlockedIncrement(&g_n_kbdgated);
        DWORD room = (pdwInOut ? *pdwInOut : 0);
        DWORD n = 0;

        // THE RELEASE. Drained from the map above rather than blasting all 256
        // codes: a key-up for a key the title never saw go down is at best
        // ignored and at worst a bound action fired on the way out, and 256
        // events would not fit in most callers' buffers anyway. Whatever does
        // not fit stays flagged and goes out on the NEXT blocked call, so a
        // small buffer delays the release by a poll instead of losing it.
        //
        // PEEK is deliberately not special-cased: a peeking reader that is told
        // a key came up will see the same event again on its next real read,
        // and a duplicate up is a no-op for any sane handler.
        if (SUCCEEDED(hr) && rgod && room && cbOd >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
            for (DWORD dik = 0; dik < 256 && n < room; dik++) {
                if (!m_kbd_held[dik]) continue;
                DIDEVICEOBJECTDATA* e =
                    (DIDEVICEOBJECTDATA*)((char*)rgod + (size_t)n * cbOd);
                ZeroMemory(e, cbOd < sizeof(DIDEVICEOBJECTDATA)
                              ? cbOd : sizeof(DIDEVICEOBJECTDATA));
                e->dwOfs  = dik;
                e->dwData = 0;                 // high bit clear = released
                if (cbOd >= sizeof(DIDEVICEOBJECTDATA)) e->uAppData = m_kbd_app[dik];
                m_kbd_held[dik] = 0;
                n++;
            }
        }
        if (n) InterlockedExchangeAdd(&g_n_kbdups, (LONG)n);
        inputgate_note_key_blocked(4, (int)n);
        if (edge)
            logf("[gate] keyboard (buffered): another application is in front -- "
                 "released %lu held key(s) to the title, then stopped delivering "
                 "(buffer room=%lu, cbOd=%lu, hr=0x%08lX)", n, room, cbOd, hr);
        if (pdwInOut) *pdwInOut = n;
        (void)fl;
        return hr;
    }

    STDMETHODIMP GetDeviceState(DWORD cb, LPVOID data)
    {
        HRESULT hr = m_real->GetDeviceState(cb, data);
        if (m_kbd) return kbd_state(hr, cb, data);
        if (m_pad) {
            // The pad's whole interposition is the button permutation, and it must sit
            // AFTER the real read and BEFORE the client sees the buffer -- which is
            // exactly here. padmap also snapshots the raw state from this call, so the
            // settings dialog reads the same device the client does rather than a
            // second, separately-enumerated one that might not agree with it.
            InterlockedIncrement(&g_n_padstate);
            if (SUCCEEDED(hr)) padmap_on_state(data, cb, m_padowner);
            return hr;
        }
        // SEAM 3 of the focus gate (inputgate.cpp), polled half.
        //
        // THIS PATH IS OURS, not SE's: [dx] dinput_background rewrites the
        // title's DISCL_FOREGROUND to DISCL_BACKGROUND, which is exactly "keep
        // feeding this device while the window is not focused" -- and that is
        // why "Tetra sometimes does this" too. The coop level must NOT be put
        // back (see SetCooperativeLevel: FOREGROUND is what killed Fantasy
        // Earth's clicks); the DELIVERY is what gets withheld.
        //
        // Zeroing the whole buffer IS the release for a polled reader: no
        // movement and no buttons held, every poll, for as long as you are
        // away. It also costs nothing on refocus -- these are RELATIVE counts,
        // so withheld motion cannot accumulate into a jump the way a frozen
        // absolute position can.
        {
            bool blocked = false;
            if (inputgate_edge(&m_gate_prev, &blocked))
                logf("[gate] mouse (polled): another application is in front -- "
                     "delivering a zeroed state to the title until you return. "
                     "The device stays ACQUIRED ([dx] mouse_focus_gate=0 to stop)");
            if (blocked) {
                if (SUCCEEDED(hr) && data && cb) ZeroMemory(data, cb);
                inputgate_note_blocked(2);
                return hr;
            }
        }
        LONG ns = InterlockedIncrement(&g_n_state);
        // The poll site, once. `_ReturnAddress()` here IS the instruction after
        // the game's own `call [GetDeviceState]` -- the anchor the whole static
        // pass hangs off. Reported on the 2nd poll, not the 1st: the first read
        // of a freshly acquired device is sometimes made from init code rather
        // than from the per-frame poll we actually want to disassemble.
        if (ns == 2 || ns == 200) di_report_callsite("mouse GetDeviceState", _ReturnAddress());
        // Report the FIRST few reads unconditionally. The old logging only fired
        // when scaling altered a value, so with scale=100 the counters read zero
        // and "DirectInput is not the pointer source" was asserted on no evidence.
        if (g_di_trace && ns <= 5 && SUCCEEDED(hr) && data &&
            (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2))) {
            DIMOUSESTATE* m = (DIMOUSESTATE*)data;
            logf("[din] GetDeviceState #%ld cb=%lu dx=%ld dy=%ld dz=%ld buttons=%02X%02X%02X%02X",
                 ns, cb, m->lX, m->lY, m->lZ,
                 m->rgbButtons[0], m->rgbButtons[1], m->rgbButtons[2], m->rgbButtons[3]);
        }
        // DIMOUSESTATE and DIMOUSESTATE2 both start with lX, lY, lZ.
        //
        // Absolute tracking wins when it can steer -- it rewrites lX/lY outright,
        // so a scale term on top would be meaningless. But it CANNOT always
        // steer: before a game window exists, and whenever the game is not the
        // foreground window, it bails. Those polls used to fall through to here
        // with the deltas untouched, i.e. RAW device counts from a 1600+ CPI
        // mouse going straight into the game's cursor -- and the mouse is now
        // acquired BACKGROUND, so the game keeps reading it while unfocused and
        // quietly shoves its cursor into a corner. On a real session that was two
        // polls in three (GetDeviceState=23930 vs abs=7955). Scaling is the right
        // fallback for exactly those polls, so the chain is: steer if we can,
        // otherwise scale, and only pass raw counts if scaling is off too.
        if (SUCCEEDED(hr) && data &&
            (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2))) {
            DIMOUSESTATE* m = (DIMOUSESTATE*)data;
            maybe_autofind();
            gain_hotkeys(g_di_ini);
            if (g_di_findcursor && !g_fc_done) {
                fc_run(m);               // RE tool: owns the deltas while it probes,
                                         // then falls through to normal tracking
            } else if (!m_mousefix) {
                /* not the cursor-fix title -- leave the mouse STOCK (FE et al.) */
            } else if (g_di_abs == 2 && apply_direct(m)) {
                m_abs_steering = true;   // SET outright -- no gain, no model, no drift
            } else if (g_di_abs && apply_absolute(m)) {
                m_abs_steering = true;   // steered
            } else if (g_di_scale != 100) {
                LONG ox = m->lX, oy = m->lY;
                m->lX = scale_axis(m->lX, &m_cx);
                m->lY = scale_axis(m->lY, &m_cy);
                if (ox != m->lX || oy != m->lY) {
                    InterlockedIncrement(&g_n_scaled);
                    if (g_di_trace && (g_n_scaled < 8 || (g_n_scaled % 900) == 0))
                        logf("[din] state dx %ld->%ld dy %ld->%ld (scale %d%%)",
                             ox, m->lX, oy, m->lY, g_di_scale);
                }
            }
        }
        return hr;
    }

    STDMETHODIMP GetDeviceData(DWORD cbOd, LPDIDEVICEOBJECTDATA rgod,
                               LPDWORD pdwInOut, DWORD fl)
    {
        HRESULT hr = m_real->GetDeviceData(cbOd, rgod, pdwInOut, fl);
        if (m_kbd) return kbd_data(hr, cbOd, rgod, pdwInOut, fl);
        if (m_pad) {
            // PEEK is remapped too: a peeked event is still the one the client will
            // consume, and rewriting the COPY in its buffer leaves the device's own
            // queue untouched. (Skipping PEEK is what made the mouse-scaling experiment
            // inconclusive; the same mistake is not worth repeating here.)
            InterlockedIncrement(&g_n_paddata);
            if (SUCCEEDED(hr) && rgod && pdwInOut)
                padmap_on_data(rgod, cbOd, *pdwInOut, m_padowner);
            return hr;
        }
        // SEAM 3 of the focus gate, buffered half.
        //
        // Returning zero events is what DISCL_FOREGROUND would have done by
        // itself -- we are reproducing its EFFECT without its side effect (an
        // unacquired device whose queue never comes back, i.e. FE's dead
        // clicks).
        //
        // But a buffered reader only learns a button was released by being TOLD.
        // Cut it off mid-drag and it believes the button is still down for ever,
        // which for a held-button camera means it never stops turning -- worse
        // than the bug. So on the edge we SYNTHESISE the ups, into the caller's
        // own buffer, and report them as the batch. A button-up for a button the
        // title thinks is already up is ignored by any sane reader, so sending
        // all four is safe and needs no state of our own.
        {
            bool blocked = false;
            int  edge = inputgate_edge(&m_gate_prev, &blocked);
            if (blocked) {
                inputgate_note_blocked(2);
                DWORD room = (pdwInOut ? *pdwInOut : 0);
                DWORD n = 0;
                if (edge && SUCCEEDED(hr) && rgod && room &&
                    cbOd >= sizeof(DIDEVICEOBJECTDATA)) {
                    static const DWORD BTN[4] = { DIMOFS_BUTTON0, DIMOFS_BUTTON1,
                                                  DIMOFS_BUTTON2, DIMOFS_BUTTON3 };
                    for (DWORD i = 0; i < 4 && n < room; i++) {
                        DIDEVICEOBJECTDATA* e =
                            (DIDEVICEOBJECTDATA*)((char*)rgod + (size_t)n * cbOd);
                        ZeroMemory(e, sizeof(*e));
                        e->dwOfs  = BTN[i];
                        e->dwData = 0;              // 0 = released
                        n++;
                    }
                    logf("[gate] mouse (buffered): another application is in "
                         "front -- sent %lu synthetic button-up event(s) so the "
                         "title does not think a button is still held, then "
                         "stopped delivering", n);
                } else if (edge) {
                    logf("[gate] mouse (buffered): another application is in "
                         "front -- could NOT send the synthetic button-ups "
                         "(hr=0x%08lX buf=%p room=%lu cbOd=%lu); if a held-button "
                         "camera keeps turning after alt-tab, this line is why",
                         hr, rgod, room, cbOd);
                }
                if (pdwInOut) *pdwInOut = n;
                return hr;
            }
        }
        LONG nd = InterlockedIncrement(&g_n_data);
        DWORD got = (pdwInOut ? *pdwInOut : 0);

        // THE OBJECT CENSUS -- the one thing about this stream never measured.
        //
        // Two facts about Fantasy Earth's buffered reads are already settled by
        // an instrumented session (polshim.570204.log): the device stays
        // acquired for the whole run -- zero DIERR_NOTACQUIRED, zero
        // DIERR_INPUTLOST -- and 850 of the 1703 calls DID return events. So
        // "DirectInput delivers nothing" is dead as an explanation, and so is
        // the note still sitting on dinput_background in polshim.ini.
        //
        // What no log has ever said is WHICH OBJECT those events carry, and
        // count=2 -- much the commonest batch -- is exactly the shape of an X/Y
        // pair. The remaining question is binary:
        //
        //   BTN* present   the buttons DO reach DirectInput, so FE is
        //                  discarding or mis-routing them; follow what FE does
        //                  with them next
        //   BTN* absent    the buttons never reach DirectInput at all, and the
        //                  fault is upstream of every module examined so far
        //
        // Tallied unconditionally -- a batch is at most the queue size and this
        // is a handful of compares -- so dinput_summary can answer it even with
        // the per-call trace switched off. NOTE the double count: a DIGDD_PEEK
        // read and the real read that follows report the SAME events twice, so
        // the totals run high. That cannot turn a zero into a non-zero, which
        // is all the verdict rests on.
        DWORD kmask = 0;
        int   kcount[12] = { 0 };
        if (SUCCEEDED(hr) && rgod && got) {
            for (DWORD i = 0; i < got; i++) {
                const DIDEVICEOBJECTDATA* d =
                    (const DIDEVICEOBJECTDATA*)((const BYTE*)rgod + i * cbOd);
                int k = didata_kind(d->dwOfs);
                kmask |= (1u << k);
                kcount[k]++;
                switch (k) {
                case 0:  InterlockedIncrement(&g_n_ofs_x); break;
                case 1:  InterlockedIncrement(&g_n_ofs_y); break;
                case 2:  InterlockedIncrement(&g_n_ofs_z); break;
                case 11: InterlockedIncrement(&g_n_ofs_other); break;
                default: InterlockedIncrement(&g_n_ofs_btn[k - 3]); break;
                }
            }
        }

        // WHY THIS LOGS MORE THAN THE FIRST FIVE CALLS, AND WHY IT LOGS hr.
        //
        // Fantasy Earth is the only title that reads the mouse BUFFERED
        // (title-mouse-read-paths), and its clicks do not work. The old trace --
        // first 5 calls, HRESULT discarded -- cannot tell the two candidate
        // causes apart:
        //
        //   * acquired, queue genuinely empty      hr = DI_OK,  count = 0
        //   * device not acquired / lost           hr = DIERR_NOTACQUIRED
        //                                               or DIERR_INPUTLOST
        //
        // Both print as `count=0` and both stop being visible after call #5, so
        // every reading of this log so far has been a guess. A run against a
        // client whose buttons are dead now says which, in one line.
        //
        // Volume is bounded WITHOUT hiding the interesting cases: the first few
        // calls always, then only TRANSITIONS -- a call that returns events when
        // the last one did not, a call that returns none when the last one did,
        // or any change of HRESULT. An idle mouse therefore costs nothing while a
        // queue that dries up mid-session is reported the moment it happens,
        // which is the exact shape of the failure described in the ini.
        if (g_di_trace) {
            static LONG s_last_got = -1;
            static HRESULT s_last_hr = S_FALSE;
            static DWORD s_last_mask = 0xFFFFFFFF;
            static bool s_seen_btn = false;
            // A batch whose OBJECT MIX differs from the previous one is an edge
            // too. Without that clause the first click of the session -- got>0
            // and the same hr as the X/Y batch before it -- prints NOTHING, and
            // the census this instrument exists for would only ever be readable
            // in the summary. Repeated batches of the same mix, which is what an
            // idle or merely-moving mouse produces, still cost one line in total.
            bool edge = (got > 0) != (s_last_got > 0) || hr != s_last_hr ||
                        kmask != s_last_mask;
            if (nd <= 5 || edge) {
                const char* why = "";
                if (hr == DIERR_NOTACQUIRED)   why = "  <-- NOT ACQUIRED";
                else if (hr == DIERR_INPUTLOST) why = "  <-- INPUT LOST";
                else if (FAILED(hr))            why = "  <-- FAILED";
                else if (got == 0 && nd > 1)    why = "  <-- acquired, queue EMPTY";
                // objs=[X:1 Y:1] / objs=[BTN0:2] -- one term per object kind
                // actually present, so the line stays short and a button is
                // impossible to miss.
                char objs[192];
                objs[0] = 0;
                size_t ol = 0;
                int nk = 0;
                for (int k = 0; k < 12; k++) {
                    if (!kcount[k] || ol + 24 >= sizeof(objs)) continue;
                    int w = _snprintf_s(objs + ol, sizeof(objs) - ol, _TRUNCATE,
                                        "%s%s:%d", nk++ ? " " : "",
                                        didata_kind_name[k], kcount[k]);
                    if (w > 0) ol += (size_t)w;
                }
                const char* first = "";
                if ((kmask & 0x7F8u) && !s_seen_btn) {
                    s_seen_btn = true;
                    first = "  <-- FIRST BUTTON EVENT THIS SESSION";
                }
                logf("[din] GetDeviceData #%ld cbOd=%lu count=%lu flags=%08lX "
                     "hr=0x%08lX objs=[%s]%s%s%s",
                     nd, cbOd, got, fl, hr, objs, why, first,
                     (fl & DIGDD_PEEK) ? "  [PEEK -- scaling used to SKIP these]"
                                       : "");
            }
            s_last_got = (LONG)got;
            s_last_hr = hr;
            s_last_mask = kmask;
        }

        // Buffered mode: one element per axis event, dwData carries the delta.
        // PEEK is no longer excluded: a peeked read is still the value the game
        // consumes, and scaling a COPY in the caller's buffer is safe -- the
        // device's own queue is untouched. Excluding it is what made the earlier
        // "DirectInput is not the source" test inconclusive.
        // NOT while absolute tracking is actually STEERING this device.
        // GetDeviceState rewrites lX/lY outright there, so a scale term is
        // meaningless on top of it -- and applying one HERE while the polled path
        // is absolute makes the two sources of the same motion disagree (the
        // shipped ini has dinput_mouseabs=1 AND dinput_mousescale=10, so this
        // fired every time the game read the mouse buffered).
        //
        // The test is m_abs_steering, NOT g_di_abs. Absolute tracking lives only
        // in the polled path -- there is nothing to steer in a stream of deltas --
        // so for a title that reads the mouse ONLY buffered, g_di_abs is on and
        // absolute never runs at all. Testing the setting therefore disabled
        // scaling with nothing replacing it, and the game got RAW high-CPI counts.
        // Measured 2026-08-17 on Fantasy Earth (polshim.554200.log): GetDeviceState
        // 0, GetDeviceData 5, [cur*] absolute engagements 0, with dinput_mouseabs=2
        // set in the ini for Tetra Master's benefit -- and its buttons stop
        // responding because the cursor runs away from the pointer. Tetra Master
        // is unaffected: it polls, so it steers, so m_abs_steering is true and this
        // block stays skipped exactly as before. Same chain as the polled path:
        // steer if we can, otherwise scale, and only pass raw counts if scaling is
        // off too.
        if (SUCCEEDED(hr) && rgod && pdwInOut && m_mousefix && g_di_scale != 100 &&
            !(g_di_abs && m_abs_steering)) {
            DWORD n = *pdwInOut;
            for (DWORD i = 0; i < n; i++) {
                DIDEVICEOBJECTDATA* d =
                    (DIDEVICEOBJECTDATA*)((BYTE*)rgod + i * cbOd);
                LONG* carry = NULL;
                if (d->dwOfs == DIMOFS_X) carry = &m_cx;
                else if (d->dwOfs == DIMOFS_Y) carry = &m_cy;
                else continue;
                LONG v = (LONG)d->dwData, o = v;
                v = scale_axis(v, carry);
                d->dwData = (DWORD)v;
                if (o != v) InterlockedIncrement(&g_n_scaled);
            }
        }
        return hr;
    }

    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT p) { return m_real->SetDataFormat(p); }
    STDMETHODIMP SetEventNotification(HANDLE h) { return m_real->SetEventNotification(h); }

    STDMETHODIMP SetCooperativeLevel(HWND hwnd, DWORD flags)
    {
        InterlockedIncrement(&g_n_coop);
        char a[128], b[128];
        DWORD want = flags;
        if (g_di_nonexcl && (flags & DISCL_EXCLUSIVE)) {
            want = (flags & ~DISCL_EXCLUSIVE) | DISCL_NONEXCLUSIVE;
            InterlockedIncrement(&g_n_downgraded);
        }
        // FOREGROUND -> BACKGROUND. DirectInput UNACQUIRES a FOREGROUND device
        // the moment its window stops being the foreground window, and a buffered
        // reader then just sees an empty queue -- no error, simply nothing.
        //
        // That is exactly Fantasy Earth's measured symptom: it acquires the mouse
        // NONEXCLUSIVE|FOREGROUND, its first GetDeviceData returns 31 events, and
        // every call after that returns count=0 forever, while its own log keeps
        // reporting the window losing focus. The cursor still moves because
        // Windows draws it and position arrives over the WH_MOUSE chain; only the
        // BUTTONS come through DirectInput, which is why clicks die but the
        // pointer and the keyboard do not.
        //
        // BACKGROUND keeps the device acquired regardless of focus. It cannot
        // steal input from other apps -- that is EXCLUSIVE's doing, and we have
        // already downgraded that above.
        if (g_di_background && (want & DISCL_FOREGROUND)) {
            want = (want & ~DISCL_FOREGROUND) | DISCL_BACKGROUND;
            InterlockedIncrement(&g_n_bg);
        }
        // DISCL_NOWINKEY -- "disable the Windows logo key". Stripped, because a
        // title that owns the whole keyboard is a title you cannot leave, and
        // that is the complaint this whole windowing effort exists to answer.
        // The flag is the game's, not ours; we have simply been forwarding it.
        // ([dx] dinput_winkey=0 to hand it back.)
        if (g_di_winkey && (want & DISCL_NOWINKEY)) {
            want &= ~DISCL_NOWINKEY;
            InterlockedIncrement(&g_n_winkey);
        }
        HRESULT hr = m_real->SetCooperativeLevel(hwnd, want);
        if (FAILED(hr) && want != flags) {
            logf("[din] NONEXCLUSIVE refused (hr=0x%08lX) -- replaying the "
                 "game's own request", hr);
            want = flags;
            hr = m_real->SetCooperativeLevel(hwnd, flags);
        }
        logf("[din] %s SetCooperativeLevel(hwnd=%p, %s) -> %s = 0x%08lX",
             kind_name(),
             hwnd, coop_names(flags, a, sizeof(a)),
             coop_names(want, b, sizeof(b)), hr);
        return hr;
    }

    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA p, DWORD o, DWORD h)
        { return m_real->GetObjectInfo(p, o, h); }
    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA p) { return m_real->GetDeviceInfo(p); }
    STDMETHODIMP RunControlPanel(HWND h, DWORD f) { return m_real->RunControlPanel(h, f); }
    STDMETHODIMP Initialize(HINSTANCE h, DWORD v, REFGUID g)
        { return m_real->Initialize(h, v, g); }
    STDMETHODIMP CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* p, LPUNKNOWN u)
        { return m_real->CreateEffect(g, e, p, u); }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA cb, LPVOID r, DWORD t)
        { return m_real->EnumEffects(cb, r, t); }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA p, REFGUID g) { return m_real->GetEffectInfo(p, g); }
    STDMETHODIMP GetForceFeedbackState(LPDWORD p) { return m_real->GetForceFeedbackState(p); }
    STDMETHODIMP SendForceFeedbackCommand(DWORD f) { return m_real->SendForceFeedbackCommand(f); }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb,
                                          LPVOID r, DWORD f)
        { return m_real->EnumCreatedEffectObjects(cb, r, f); }
    STDMETHODIMP Escape(LPDIEFFESCAPE p) { return m_real->Escape(p); }
    STDMETHODIMP Poll() { return m_real->Poll(); }
    STDMETHODIMP SendDeviceData(DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f)
        { return m_real->SendDeviceData(cb, d, n, f); }
    STDMETHODIMP EnumEffectsInFile(LPCSTR f, LPDIENUMEFFECTSINFILECALLBACK cb,
                                   LPVOID r, DWORD fl)
        { return m_real->EnumEffectsInFile(f, cb, r, fl); }
    STDMETHODIMP WriteEffectToFile(LPCSTR f, DWORD n, LPDIFILEEFFECT e, DWORD fl)
        { return m_real->WriteEffectToFile(f, n, e, fl); }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f)
        { return m_real->BuildActionMap(a, u, f); }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f)
        { return m_real->SetActionMap(a, u, f); }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA p) { return m_real->GetImageInfo(p); }
};

// ---------------------------------------------------------------------------
// factory wrapper
// ---------------------------------------------------------------------------

class DI8Wrap : public IDirectInput8A
{
    IDirectInput8A* m_real;
    LONG m_ref;
public:
    explicit DI8Wrap(IDirectInput8A* r) : m_real(r), m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInput8A) {
            *ppv = static_cast<IDirectInput8A*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { m_real->Release(); delete this; return 0; }
        return (ULONG)r;
    }

    STDMETHODIMP CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN unk)
    {
        HRESULT hr = m_real->CreateDevice(rguid, out, unk);
        InterlockedIncrement(&g_n_create);
        if (FAILED(hr) || !out || !*out) return hr;

        bool is_mouse = (rguid == GUID_SysMouse);
        bool is_kbd   = (rguid == GUID_SysKeyboard);
        // Anything that is neither the system mouse nor the system keyboard is a device
        // instance GUID, i.e. a game controller -- DirectInput has no third system GUID.
        // Confirmed against the device's own capabilities below before it is treated as
        // one, so a hypothetical other device type cannot be silently remapped.
        bool is_pad = !is_mouse && !is_kbd;

        if (is_pad) {
            DIDEVCAPS caps; ZeroMemory(&caps, sizeof(caps)); caps.dwSize = sizeof(caps);
            DIDEVICEINSTANCEA di; ZeroMemory(&di, sizeof(di)); di.dwSize = sizeof(di);
            (*out)->GetCapabilities(&caps);
            (*out)->GetDeviceInfo(&di);
            DWORD type = GET_DIDEVICE_TYPE(caps.dwDevType);
            is_pad = g_di_pad &&
                     (type == DI8DEVTYPE_GAMEPAD || type == DI8DEVTYPE_JOYSTICK ||
                      type == DI8DEVTYPE_1STPERSON || type == DI8DEVTYPE_DRIVING ||
                      type == DI8DEVTYPE_FLIGHT || type == DI8DEVTYPE_SUPPLEMENTAL);
            logf("[din] CreateDevice(device instance) -> 0x%08lX  type=0x%02lX buttons=%lu "
                 "'%s'%s", hr, type, caps.dwButtons, di.tszProductName,
                 is_pad ? "  [wrapping -- button mapping applies to this]" : "");
            if (is_pad) {
                InterlockedIncrement(&g_n_pad);
                padmap_note_device(di.tszProductName, (int)caps.dwButtons);
                DIDeviceWrap* w = new (std::nothrow) DIDeviceWrap(*out, true);
                if (w) {
                    // Who created this pad? A profiled title's device is permuted
                    // against ITS pad map; the shell's (owner NULL) keeps the shell
                    // mapping. Resolved here once -- CreateDevice's return address
                    // lands in the creating module, the same fact the mouse fixes use.
                    const TitleProfile* owner = profile_for_addr(_ReturnAddress());
                    w->set_padowner(owner);
                    if (owner)
                        logf("[din] the pad device was created by %s -- padmap will "
                             "use its OWN button map (see [prof] pad=)", owner->title);
                    *out = static_cast<IDirectInputDevice8A*>(w);
                }
            }
            return hr;
        }

        logf("[din] CreateDevice(%s) -> 0x%08lX%s",
             is_mouse ? "GUID_SysMouse" : "GUID_SysKeyboard",
             hr, ((is_mouse && g_di_enable) || (is_kbd && g_di_winkey)) ? "  [wrapping]" : "");

        // THE KEYBOARD, wrapped for exactly one method: SetCooperativeLevel.
        //
        // Every POL title creates this device and then asks for a coop level we
        // have never seen, because until now the line above was the last thing
        // the log ever said about it. That call is where DISCL_NOWINKEY and
        // DISCL_EXCLUSIVE decide whether the Windows key still works -- and
        // "the games don't respond to a Windows key press" is a live complaint.
        //
        // Gated on dinput_winkey, NOT dinput_enable: dinput_enable means "the
        // mouse fixes", it ships 0, and burying a keyboard fix behind a mouse
        // switch is how [[shim-windowed-by-default]]'s four hidden gates
        // happened. One knob, one meaning.
        if (is_kbd) {
            if (!g_di_winkey) return hr;
            DIDeviceWrap* k = new (std::nothrow) DIDeviceWrap(*out, false, true);
            if (k) *out = static_cast<IDirectInputDevice8A*>(k);
            return hr;
        }

        // With only the pad path armed ([dx] dinput_enable=0, dinput_pad=1) the mouse is
        // left strictly alone -- turning the mouse fixes off must keep meaning that.
        if (!is_mouse || !g_di_enable) return hr;

        InterlockedIncrement(&g_n_mouse);
        DIDeviceWrap* w = new (std::nothrow) DIDeviceWrap(*out);
        if (w) {
            // Apply the TM mouse fixes ONLY to the title they are for -- the one whose
            // cursor variable is configured (dinput_cursor_xy, g_cur_mod = "TM.dll").
            // CreateDevice's return address lands in the creating title, so it names it.
            bool fixes = mouse_fix_title(_ReturnAddress());
            w->set_mousefix(fixes);
            // Fresh mouse device for the cursor-fix title = a fresh TM.dll load. Drop the
            // cached cursor-variable address so it re-resolves against the CURRENT base --
            // without this the second launch writes to the first launch's (freed) address
            // and the cursor stops tracking.
            if (fixes) cursor_reset_resolution();
            logf("[din] mouse device created by %s -- TM cursor fixes %s",
                 fixes ? "the cursor-fix title" : "another title (stock mouse)",
                 fixes ? "APPLIED (cursor address re-resolved for this launch)"
                       : "SKIPPED (no scale, no cursor write)");
            *out = static_cast<IDirectInputDevice8A*>(w);
        }
        return hr;
    }

    STDMETHODIMP EnumDevices(DWORD t, LPDIENUMDEVICESCALLBACKA cb, LPVOID r, DWORD f)
        { return m_real->EnumDevices(t, cb, r, f); }
    STDMETHODIMP GetDeviceStatus(REFGUID g) { return m_real->GetDeviceStatus(g); }
    STDMETHODIMP RunControlPanel(HWND h, DWORD f) { return m_real->RunControlPanel(h, f); }
    STDMETHODIMP Initialize(HINSTANCE h, DWORD v) { return m_real->Initialize(h, v); }
    STDMETHODIMP FindDevice(REFGUID g, LPCSTR n, LPGUID out)
        { return m_real->FindDevice(g, n, out); }
    STDMETHODIMP EnumDevicesBySemantics(LPCSTR u, LPDIACTIONFORMATA a,
                                        LPDIENUMDEVICESBYSEMANTICSCBA cb,
                                        LPVOID r, DWORD f)
        { return m_real->EnumDevicesBySemantics(u, a, cb, r, f); }
    STDMETHODIMP ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb,
                                  LPDICONFIGUREDEVICESPARAMSA p, DWORD f, LPVOID r)
        { return m_real->ConfigureDevices(cb, p, f, r); }
};

// ---------------------------------------------------------------------------
// UNICODE (W) interface wrappers
//
// Tetra Master creates its DirectInput via IID_IDirectInput8W, and the ANSI
// DI8Wrap above cannot touch it (a W vtable is not an A vtable). Left unwrapped,
// none of the coop-level handling reaches TM's pad -- and TM acquires the pad
// EXCLUSIVE|FOREGROUND, which DirectInput refuses when the game window is not the
// foreground window. Under gamescope it usually is not, so Acquire() fails and
// EVERY controller read comes back empty: all input dead (measured symptom on the
// Deck 2026-08-20, tm-deck-joystick-dead). The mouse still worked because TM's
// cursor is a direct write, not a dinput read (tetra-cursor-source).
//
// These wrappers are PAD-ONLY: they apply the same SetCooperativeLevel downgrade
// (EXCLUSIVE->NONEXCLUSIVE, FOREGROUND->BACKGROUND) and the same padmap button
// permutation the ANSI pad path uses, and forward everything else verbatim. W
// mouse/keyboard devices are left unwrapped (TM does not read the mouse through
// dinput, so there is nothing to gain and every reason not to duplicate the ANSI
// mouse-steering machinery on a second interface). Acquire() logs its HRESULT for
// the first few calls, so one launch confirms the mechanism even if the fix is
// wrong: a DIERR from Acquire is the proof the coop level was the wall.

class DIDeviceWrapW : public IDirectInputDevice8W
{
    IDirectInputDevice8W* m_real;
    LONG m_ref;
    const TitleProfile* m_owner;
    LONG m_nacq;
public:
    explicit DIDeviceWrapW(IDirectInputDevice8W* r)
        : m_real(r), m_ref(1), m_owner(NULL), m_nacq(0),
          m_nstate(0), m_lastbtn(0), m_lastpov(0), m_lastxy(0) {}
    void set_padowner(const TitleProfile* o) { m_owner = o; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInputDevice8W) {
            *ppv = static_cast<IDirectInputDevice8W*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { m_real->Release(); delete this; return 0; }
        return (ULONG)r;
    }

    STDMETHODIMP GetCapabilities(LPDIDEVCAPS p) { return m_real->GetCapabilities(p); }
    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW cb, LPVOID r, DWORD f)
        { return m_real->EnumObjects(cb, r, f); }
    STDMETHODIMP GetProperty(REFGUID g, LPDIPROPHEADER p) { return m_real->GetProperty(g, p); }
    STDMETHODIMP SetProperty(REFGUID g, LPCDIPROPHEADER p) { return m_real->SetProperty(g, p); }

    STDMETHODIMP Acquire()
    {
        HRESULT hr = m_real->Acquire();
        LONG n = InterlockedIncrement(&m_nacq);
        if (n <= 6 || FAILED(hr))
            logf("[din] pad(W) Acquire #%ld -> 0x%08lX%s", n, (unsigned long)hr,
                 FAILED(hr) ? "  <-- FAILED (coop level / focus)" : "");
        return hr;
    }
    STDMETHODIMP Unacquire() { return m_real->Unacquire(); }

    LONG m_nstate; DWORD m_lastbtn; DWORD m_lastpov; LONG m_lastxy;
    STDMETHODIMP GetDeviceState(DWORD cb, LPVOID data)
    {
        HRESULT hr = m_real->GetDeviceState(cb, data);
        LONG n = InterlockedIncrement(&g_n_padstate);
        // Diagnostic: show what TM actually RECEIVES from the device -- the axes,
        // the POV hat (an Xbox d-pad usually arrives here, NOT as buttons), and the
        // first 12 buttons. Logged for the first few polls and then only on CHANGE,
        // so a held stick or a pressed button is visible without flooding. This is
        // read from the RAW state, before padmap permutes it.
        if (g_di_trace && SUCCEEDED(hr) && data && cb >= 80) {
            const BYTE* s = (const BYTE*)data;
            LONG  lx  = *(const LONG*)(s + 0);
            LONG  ly  = *(const LONG*)(s + 4);
            DWORD pov = *(const DWORD*)(s + 32);          // rgdwPOV[0]
            const BYTE* btn = s + 48;                      // rgbButtons[0..]
            DWORD bmask = 0;
            for (int i = 0; i < 12; i++) if (btn[i] & 0x80) bmask |= (1u << i);
            LONG xy = (lx & 0xFFFF) | (ly << 16);
            LONG ln = InterlockedIncrement(&m_nstate);
            if (ln <= 4 || bmask != m_lastbtn || pov != m_lastpov || xy != m_lastxy) {
                m_lastbtn = bmask; m_lastpov = pov; m_lastxy = xy;
                logf("[padin] W state #%ld lX=%ld lY=%ld POV=%lu btns=0x%03lX",
                     ln, lx, ly, (unsigned long)pov, (unsigned long)bmask);
            }
        }
        if (SUCCEEDED(hr)) padmap_on_state(data, cb, m_owner);
        return hr;
    }
    STDMETHODIMP GetDeviceData(DWORD cbOd, LPDIDEVICEOBJECTDATA rgod,
                               LPDWORD pdwInOut, DWORD fl)
    {
        HRESULT hr = m_real->GetDeviceData(cbOd, rgod, pdwInOut, fl);
        InterlockedIncrement(&g_n_paddata);
        if (SUCCEEDED(hr) && rgod && pdwInOut)
            padmap_on_data(rgod, cbOd, *pdwInOut, m_owner);
        return hr;
    }

    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT p) { return m_real->SetDataFormat(p); }
    STDMETHODIMP SetEventNotification(HANDLE h) { return m_real->SetEventNotification(h); }

    STDMETHODIMP SetCooperativeLevel(HWND hwnd, DWORD flags)
    {
        InterlockedIncrement(&g_n_coop);
        char a[128], b[128];
        DWORD want = flags;
        if (g_di_nonexcl && (flags & DISCL_EXCLUSIVE)) {
            want = (flags & ~DISCL_EXCLUSIVE) | DISCL_NONEXCLUSIVE;
            InterlockedIncrement(&g_n_downgraded);
        }
        if (g_di_background && (want & DISCL_FOREGROUND)) {
            want = (want & ~DISCL_FOREGROUND) | DISCL_BACKGROUND;
            InterlockedIncrement(&g_n_bg);
        }
        // DISCL_NOWINKEY -- "disable the Windows logo key". Stripped, because a
        // title that owns the whole keyboard is a title you cannot leave, and
        // that is the complaint this whole windowing effort exists to answer.
        // The flag is the game's, not ours; we have simply been forwarding it.
        // ([dx] dinput_winkey=0 to hand it back.)
        if (g_di_winkey && (want & DISCL_NOWINKEY)) {
            want &= ~DISCL_NOWINKEY;
            InterlockedIncrement(&g_n_winkey);
        }
        HRESULT hr = m_real->SetCooperativeLevel(hwnd, want);
        if (FAILED(hr) && want != flags) {
            logf("[din] pad(W) NONEXCLUSIVE/BACKGROUND refused (hr=0x%08lX) -- "
                 "replaying the game's own request", hr);
            want = flags;
            hr = m_real->SetCooperativeLevel(hwnd, flags);
        }
        logf("[din] pad(W) SetCooperativeLevel(hwnd=%p, %s) -> %s = 0x%08lX",
             hwnd, coop_names(flags, a, sizeof(a)),
             coop_names(want, b, sizeof(b)), hr);
        return hr;
    }

    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW p, DWORD o, DWORD h)
        { return m_real->GetObjectInfo(p, o, h); }
    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEW p) { return m_real->GetDeviceInfo(p); }
    STDMETHODIMP RunControlPanel(HWND h, DWORD f) { return m_real->RunControlPanel(h, f); }
    STDMETHODIMP Initialize(HINSTANCE h, DWORD v, REFGUID g)
        { return m_real->Initialize(h, v, g); }
    STDMETHODIMP CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* p, LPUNKNOWN u)
        { return m_real->CreateEffect(g, e, p, u); }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKW cb, LPVOID r, DWORD t)
        { return m_real->EnumEffects(cb, r, t); }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOW p, REFGUID g) { return m_real->GetEffectInfo(p, g); }
    STDMETHODIMP GetForceFeedbackState(LPDWORD p) { return m_real->GetForceFeedbackState(p); }
    STDMETHODIMP SendForceFeedbackCommand(DWORD f) { return m_real->SendForceFeedbackCommand(f); }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb,
                                          LPVOID r, DWORD f)
        { return m_real->EnumCreatedEffectObjects(cb, r, f); }
    STDMETHODIMP Escape(LPDIEFFESCAPE p) { return m_real->Escape(p); }
    STDMETHODIMP Poll() { return m_real->Poll(); }
    STDMETHODIMP SendDeviceData(DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f)
        { return m_real->SendDeviceData(cb, d, n, f); }
    STDMETHODIMP EnumEffectsInFile(LPCWSTR f, LPDIENUMEFFECTSINFILECALLBACK cb,
                                   LPVOID r, DWORD fl)
        { return m_real->EnumEffectsInFile(f, cb, r, fl); }
    STDMETHODIMP WriteEffectToFile(LPCWSTR f, DWORD n, LPDIFILEEFFECT e, DWORD fl)
        { return m_real->WriteEffectToFile(f, n, e, fl); }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATW a, LPCWSTR u, DWORD f)
        { return m_real->BuildActionMap(a, u, f); }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATW a, LPCWSTR u, DWORD f)
        { return m_real->SetActionMap(a, u, f); }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW p) { return m_real->GetImageInfo(p); }
};

class DI8WrapW : public IDirectInput8W
{
    IDirectInput8W* m_real;
    LONG m_ref;
public:
    explicit DI8WrapW(IDirectInput8W* r) : m_real(r), m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInput8W) {
            *ppv = static_cast<IDirectInput8W*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { m_real->Release(); delete this; return 0; }
        return (ULONG)r;
    }

    STDMETHODIMP CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN unk)
    {
        HRESULT hr = m_real->CreateDevice(rguid, out, unk);
        InterlockedIncrement(&g_n_create);
        if (FAILED(hr) || !out || !*out) return hr;

        // Only game controllers get wrapped on the W path (see the class comment).
        bool is_pad = !(rguid == GUID_SysMouse) && !(rguid == GUID_SysKeyboard);
        if (!is_pad) return hr;

        DIDEVCAPS caps; ZeroMemory(&caps, sizeof(caps)); caps.dwSize = sizeof(caps);
        DIDEVICEINSTANCEW di; ZeroMemory(&di, sizeof(di)); di.dwSize = sizeof(di);
        (*out)->GetCapabilities(&caps);
        (*out)->GetDeviceInfo(&di);
        DWORD type = GET_DIDEVICE_TYPE(caps.dwDevType);
        is_pad = g_di_pad &&
                 (type == DI8DEVTYPE_GAMEPAD || type == DI8DEVTYPE_JOYSTICK ||
                  type == DI8DEVTYPE_1STPERSON || type == DI8DEVTYPE_DRIVING ||
                  type == DI8DEVTYPE_FLIGHT || type == DI8DEVTYPE_SUPPLEMENTAL);
        char prod[MAX_PATH] = "";
        WideCharToMultiByte(CP_ACP, 0, di.tszProductName, -1, prod, sizeof(prod), NULL, NULL);
        logf("[din] CreateDevice(W device instance) -> 0x%08lX type=0x%02lX buttons=%lu "
             "'%s'%s", hr, type, caps.dwButtons, prod,
             is_pad ? "  [wrapping -- button mapping + coop downgrade apply]" : "");
        if (!is_pad) return hr;

        InterlockedIncrement(&g_n_pad);
        padmap_note_device(prod, (int)caps.dwButtons);
        DIDeviceWrapW* w = new (std::nothrow) DIDeviceWrapW(*out);
        if (w) {
            const TitleProfile* owner = profile_for_addr(_ReturnAddress());
            w->set_padowner(owner);
            if (owner)
                logf("[din] the W pad device was created by %s -- padmap uses its "
                     "OWN button map", owner->title);
            *out = static_cast<IDirectInputDevice8W*>(w);
        }
        return hr;
    }

    STDMETHODIMP EnumDevices(DWORD t, LPDIENUMDEVICESCALLBACKW cb, LPVOID r, DWORD f)
        { return m_real->EnumDevices(t, cb, r, f); }
    STDMETHODIMP GetDeviceStatus(REFGUID g) { return m_real->GetDeviceStatus(g); }
    STDMETHODIMP RunControlPanel(HWND h, DWORD f) { return m_real->RunControlPanel(h, f); }
    STDMETHODIMP Initialize(HINSTANCE h, DWORD v) { return m_real->Initialize(h, v); }
    STDMETHODIMP FindDevice(REFGUID g, LPCWSTR n, LPGUID out)
        { return m_real->FindDevice(g, n, out); }
    STDMETHODIMP EnumDevicesBySemantics(LPCWSTR u, LPDIACTIONFORMATW a,
                                        LPDIENUMDEVICESBYSEMANTICSCBW cb,
                                        LPVOID r, DWORD f)
        { return m_real->EnumDevicesBySemantics(u, a, cb, r, f); }
    STDMETHODIMP ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb,
                                  LPDICONFIGUREDEVICESPARAMSW p, DWORD f, LPVOID r)
        { return m_real->ConfigureDevices(cb, p, f, r); }
};

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_DI8C)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static PFN_DI8C real_DirectInput8Create = NULL;

static HRESULT WINAPI hook_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid,
                                              LPVOID* out, LPUNKNOWN unk)
{
    HRESULT hr = real_DirectInput8Create(hinst, ver, riid, out, unk);
    if (FAILED(hr) || !out || !*out) return hr;

    if (riid == IID_IDirectInput8W) {
        // The Unicode flavour has its own vtable, so it gets its own wrapper.
        // PAD-ONLY: this is what Tetra Master uses, and leaving it unwrapped left
        // TM's pad acquiring EXCLUSIVE|FOREGROUND with no downgrade -> Acquire()
        // fails off-focus under gamescope -> all controller input dead. Mouse
        // scaling is NOT provided on the W path (TM's cursor is a direct write,
        // not a dinput read), so a W mouse device stays stock.
        DI8WrapW* w = new (std::nothrow) DI8WrapW((IDirectInput8W*)*out);
        if (w) *out = static_cast<IDirectInput8W*>(w);
        logf("[din] DirectInput8Create(W, ver=0x%04lX) wrapped -- pad coop downgrade "
             "(nonexcl=%d bg=%d) + button map apply; mouse left stock",
             ver, g_di_nonexcl, g_di_background);
        return hr;
    }
    if (riid != IID_IDirectInput8A) {
        logf("[din] DirectInput8Create asked for an interface that is neither "
             "IDirectInput8A nor W (riid unhandled) -- left unwrapped");
        return hr;
    }
    DI8Wrap* w = new (std::nothrow) DI8Wrap((IDirectInput8A*)*out);
    if (w) *out = static_cast<IDirectInput8A*>(w);
    logf("[din] DirectInput8Create(ver=0x%04lX) wrapped "
         "(nonexclusive=%d scale=%d%%)", ver, g_di_nonexcl, g_di_scale);
    return hr;
}

void* dinput_real_DirectInput8Create() { return (void*)real_DirectInput8Create; }
void* dinput_hook_DirectInput8Create() { return (void*)hook_DirectInput8Create; }

void dinput_resolve()
{
    // The PAD path arms independently of the mouse path. The controller mapper has to be
    // able to SHOW the live device even with the mapping switched off -- that readout is
    // the whole diagnosis for "the pad does nothing" -- and it must not become unusable
    // because somebody turned the mouse fixes off.
    if (!g_di_enable && !g_di_pad) return;
    HMODULE m = LoadLibraryW(L"dinput8.dll");
    if (m) real_DirectInput8Create = (PFN_DI8C)GetProcAddress(m, "DirectInput8Create");
    logf("[din] mouse=%d nonexclusive=%d scale=%d%% abs=%d(home=%d) pad=%d | "
         "DirectInput8Create=%p",
         g_di_enable, g_di_nonexcl, g_di_scale, g_di_abs, g_di_abs_home, g_di_pad,
         real_DirectInput8Create);
    if (!real_DirectInput8Create) {
        logf("[din] WARN: DirectInput8Create unresolved -- input hooks are dead");
        return;
    }
    // Same reason as d3d8 and user32: TM.dll is POL1-packed and re-resolves its
    // imports, so an IAT patch on it is a race we lose. Patch the export.
    dx_eat_patch_export(m, "DirectInput8Create", (void*)hook_DirectInput8Create,
                        "dinput8!DirectInput8Create");
}

void dinput_summary()
{
    // THE KEYBOARD LINE COMES FIRST, AND BEFORE THE EARLY RETURN.
    //
    // The keyboard device is wrapped on [dx] dinput_winkey, which is a DIFFERENT
    // switch from dinput_enable (the mouse fixes, which ship OFF) and dinput_pad.
    // Reporting it after the return below would mean the one measurement this
    // seam exists to produce is missing on exactly the default configuration --
    // the shape of failure this project has already paid for several times.
    if (g_n_kbdstate || g_n_kbddata) {
        logf("[din] keyboard: kbdstate=%ld kbddata=%ld gated=%ld synthetic-ups=%ld "
             "-- this title DOES read its keyboard through DirectInput",
             g_n_kbdstate, g_n_kbddata, g_n_kbdgated, g_n_kbdups);
        if (g_n_kbdstate && !g_n_kbddata)
            logf("[din] keyboard: POLLED only (GetDeviceState). The release for "
                 "that path is a zeroed state, which cannot leave a key stuck.");
        else if (g_n_kbddata && !g_n_kbdstate)
            logf("[din] keyboard: BUFFERED only (GetDeviceData). This is the path "
                 "where a key can stick if the release fails -- check "
                 "synthetic-ups above against the [gate] keyboard lines.");
    } else if (g_n_winkey || g_n_coop) {
        // KEY: A REAL FINDING, NOT A GAP. A keyboard device was created (that is
        // what put a NOWINKEY strip or a coop call in the counters) and NEVER
        // READ through DirectInput. Then the keys arrive some other way -- and
        // seam 5 (keystate.cpp, the [key] summary) is the first place to look,
        // because GetAsyncKeyState is focus-blind and was hooked by nothing at
        // all until 2026-09-09.
        logf("[din] keyboard: a keyboard device was configured but NEVER READ "
             "through DirectInput (kbdstate=0 kbddata=0) -- if a title is acting "
             "on keys while it is not in front, this is NOT the path; read the "
             "[key] summary next");
    }

    if (!g_di_enable && !g_di_pad) return;
    logf("[din] summary: CreateDevice=%ld mouse=%ld pad=%ld SetCooperativeLevel=%ld "
         "(downgraded %ld, NOWINKEY stripped %ld) GetDeviceState=%ld GetDeviceData=%ld "
         "scaled=%ld abs=%ld padstate=%ld paddata=%ld",
         g_n_create, g_n_mouse, g_n_pad, g_n_coop, g_n_downgraded, g_n_winkey,
         g_n_state, g_n_data, g_n_scaled, g_n_abs, g_n_padstate, g_n_paddata);
    // "NOWINKEY stripped 0" with a keyboard device created is a RESULT, not a
    // gap: it means this title asked for the Windows key to keep working, and
    // whatever is eating it is somewhere else -- start at hookspy.cpp's
    // [hk] key lines, which cover the other mechanism.
    // The buffered-object census, and the verdict it exists to deliver. Read
    // this FIRST for any "the pointer moves but clicks do nothing" report on a
    // title that reads the mouse buffered (Fantasy Earth is the only one --
    // title-mouse-read-paths). Counts are inflated by DIGDD_PEEK double reads;
    // only zero vs non-zero is being claimed.
    if (g_n_data) {
        LONG btn = 0;
        char b[128];
        size_t bl = 0;
        b[0] = 0;
        for (int i = 0; i < 8; i++) {
            btn += g_n_ofs_btn[i];
            if (!g_n_ofs_btn[i] || bl + 24 >= sizeof(b)) continue;
            int w = _snprintf_s(b + bl, sizeof(b) - bl, _TRUNCATE,
                                " BTN%d=%ld", i, g_n_ofs_btn[i]);
            if (w > 0) bl += (size_t)w;
        }
        logf("[din] buffered objects: X=%ld Y=%ld Z=%ld%s other=%ld  (buttons "
             "total %ld)",
             g_n_ofs_x, g_n_ofs_y, g_n_ofs_z, b, g_n_ofs_other, btn);
        if (!btn && (g_n_ofs_x || g_n_ofs_y))
            logf("[din] VERDICT: motion was delivered buffered but NO BUTTON "
                 "EVENT EVER REACHED DIRECTINPUT -- the clicks are lost UPSTREAM "
                 "of the game, so stop looking at what the game does with them");
        else if (btn)
            logf("[din] VERDICT: %ld button event(s) WERE delivered buffered -- "
                 "DirectInput is not swallowing the clicks; the game is "
                 "discarding or mis-routing them", btn);
    }
    // Which of the two tracking modes actually ran is the first thing to read
    // when the pointer still misbehaves -- they fail in completely different ways.
    if (g_di_abs) {
        if (g_cur_ptr && !g_cur_off)
            logf("[cur*] tracking was CLOSED-LOOP on %s+0x%X -- the pointer cannot "
                 "drift; if it still felt wrong the fault is the mapping, not the model",
                 g_cur_mod[0] ? g_cur_mod : "(heap)", g_cur_rva);
        else if (g_cur_off)
            logf("[cur*] closed loop was DROPPED this session -- ran open-loop "
                 "(dead reckoning), so drift after a game-side cursor move is expected");
        else
            logf("[cur*] no cursor address: ran OPEN-LOOP (dead reckoning). Set "
                 "[dx] dinput_autofind=1 to have the finder locate it, or "
                 "dinput_findcursor=1 to probe immediately");
    }
    // Ownership, for mode 2. These two counters answer "is the pad being allowed
    // to drive at all" without a single guess: yields=0 with a controller plugged
    // in means the shim never let go of the cursor, which is the whole of the old
    // bug. wakes >> yields means the wake threshold is picking up sensor noise.
    if (g_di_abs == 2)
        logf("[cur2] direct writes=%ld  handovers: mouse took it %ld time(s), "
             "yielded to the game %ld time(s)  (yield=%d follow=%d wake=%d keepdelta=%d)",
             g_n_direct, g_n_wake, g_n_yield,
             g_di_abs_yield, g_di_abs_follow, g_di_abs_wake, g_di_abs_keepdelta);
    // The one question the controller mapper cannot answer for itself, recorded on every
    // run: was there ever a pad on the client's own input path? "No" means the mapping is
    // not being ignored, it has nothing to act on -- a different problem with a different
    // fix (gamepad mode, or Steam Input's layout), and the two have been confused before.
    if (!g_n_pad)
        logf("[din] NOTE: the client never created a GAME CONTROLLER device. The button "
             "mapping had nothing to remap. Check [inputmode] mode=force_gamepad, and on "
             "a Steam Deck that Steam Input is set to a CONTROLLER layout, not "
             "Keyboard/Mouse.");
    else if (!g_n_padstate && !g_n_paddata)
        logf("[din] NOTE: a pad device exists but the client never READ it -- the buttons "
             "are arriving some other way, and remapping them here cannot work.");
    if (g_di_enable && !g_n_mouse)
        logf("[din] NOTE: no GUID_SysMouse device was ever created -- the game is "
             "not reading the mouse through DirectInput either. Look at WM_ "
             "messages next.");
    else if (g_di_enable && g_n_mouse && !g_n_state && !g_n_data)
        logf("[din] NOTE: a mouse device exists but was never READ. The pointer "
             "must be coming from somewhere else.");
}
