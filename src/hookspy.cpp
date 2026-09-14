// hookspy.cpp -- watch (and optionally correct) the Windows hook chain that
// carries mouse input to a PlayOnline title.
//
// WHY THIS IS THE REMAINING CANDIDATE. In a running game's process the modules
// are pol.exe + PolHook.dll + polcore.dll + the title, and their user32 imports
// converge on exactly one mechanism:
//
//     polcore.dll   SetWindowsHookExA + CallNextHookEx
//     PolHook.dll   SetWindowsHookExA + CallNextHookEx + GetProcAddress
//     TM.dll        CallNextHookEx        <-- and NOTHING else from user32
//
// TM.dll cannot create a window, cannot read the cursor, cannot call
// SetCursorPos. Its only user32 import is the one you use to pass a hook along.
// Meanwhile Ghidra showed pol.exe does `SetCursor(NULL)` on WM_SETCURSOR over
// HTCLIENT -- it HIDES the OS pointer, so the pointer drawn in-game comes from
// whatever the title reads. The hook chain is what is left.
//
// A WH_MOUSE hook receives MOUSEHOOKSTRUCT::pt in **screen** coordinates. At a
// real 640x480 fullscreen mode -- 2003 -- screen coordinates ARE game
// coordinates. On a 1280x720 desktop they span the whole monitor, which is the
// reported symptom exactly, and explains why fullscreen was equally wrong.
//
// DELIBERATELY LOG-FIRST. hook_translate defaults to 0: this build only reports
// which hooks are installed and what coordinates flow through them. Five
// theories have already died from acting before measuring; the transform gets
// chosen from the numbers in this log, not from a sixth guess.

// ---------------------------------------------------------------------------
// THE KEYBOARD HALF -- added 2026-08-26, out of this project's OWN logs
//
// Reported symptom: "most POL games don't respond to a Windows key press or
// alt+tab, which makes it even more frustrating trying to get out of them."
//
// The mechanism was already written down in polshim.*.log, by this very file,
// and nobody had read it back:
//
//   [hk] SetWindowsHookExA(WH_KEYBOARD_LL=13, proc=... in FFXiMain.dll, hmod=pol.exe)
//   [hk] SetWindowsHookExA(WH_KEYBOARD_LL=13, proc=... in FrontMissionOnline.dll, hmod=pol.exe)
//
// A WH_KEYBOARD_LL proc that returns non-zero SWALLOWS the keystroke before any
// application -- the shell included -- ever sees it. That is how a 2003 game
// "disables" the Windows key and Alt+Tab, and being a system-wide hook it holds
// while the game is windowed and even while it is not the foreground window.
// No amount of window styling can undo it, which is why making windowed mode
// the default did not make these titles any easier to leave.
//
// So the slot machinery that already wraps the mouse hooks now wraps the
// keyboard ones too, and for a short list of ESCAPE keys the title's proc is
// not called at all -- the event goes straight on down the chain to the OS.
// Every other key is forwarded untouched, so nothing the game binds changes.
//
// WHY NOT SIMPLY REFUSE THE HOOK: these titles use the same hook for their own
// key handling, so refusing the install would break in-game input. WHY NOT EAT
// ONLY THE DOWN EDGE: a shell that sees Win-down but never Win-up leaves the
// modifier stuck. The pair passes through whole.
//
// This is one of TWO mechanisms, and either alone would leave the symptom.
// dinputhook.cpp has the other -- DISCL_NOWINKEY on a DirectInput cooperative
// level, which the same logs show the shim itself faithfully forwarding.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "polshim.h"

static int g_hook_enable    = 1;
static int g_hook_trace     = 1;
static int g_hook_translate = 0;   // 0 = observe only
static int g_key_escape     = 1;   // let Win / Alt+Tab / Alt+Esc / Ctrl+Esc out
static LONG g_n_key_msgs = 0, g_n_key_let = 0;

typedef HHOOK (WINAPI *PFN_SetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD);
typedef HHOOK (WINAPI *PFN_SetWindowsHookExW)(int, HOOKPROC, HINSTANCE, DWORD);
static PFN_SetWindowsHookExA real_SetWindowsHookExA = NULL;
static PFN_SetWindowsHookExW real_SetWindowsHookExW = NULL;

static LONG g_n_installed = 0, g_n_mouse_msgs = 0;

void hookspy_configure(const wchar_t* ini)
{
    g_hook_enable    = GetPrivateProfileIntW(L"dx", L"hook_enable",    1, ini);
    g_hook_trace     = GetPrivateProfileIntW(L"dx", L"hook_trace",     trace_at(2), ini);
    g_hook_translate = GetPrivateProfileIntW(L"dx", L"hook_translate", 0, ini);
    g_key_escape     = GetPrivateProfileIntW(L"dx", L"key_escape",     1, ini);
}

// Live re-read for the settings dialog. hook_enable is NOT re-read: it gated
// whether SetWindowsHookEx* were patched at install time, and that patch is in
// (or not) for the life of the process.
void hookspy_reload(const wchar_t* ini)
{
    g_hook_trace     = GetPrivateProfileIntW(L"dx", L"hook_trace",     trace_at(2), ini);
    g_hook_translate = GetPrivateProfileIntW(L"dx", L"hook_translate", 0, ini);
    // key_escape IS re-read. The wrapper is installed either way and the test
    // happens per keystroke, so a change takes effect on the next key pressed --
    // no relaunch, which matters for a setting whose whole job is escaping a
    // title you are currently stuck inside.
    g_key_escape     = GetPrivateProfileIntW(L"dx", L"key_escape",     1, ini);
    logf("[reload] hookspy: trace=%d translate=%d key_escape=%d",
         g_hook_trace, g_hook_translate, g_key_escape);
}
int hookspy_enabled() { return g_hook_enable; }

static const char* hook_name(int id)
{
    switch (id) {
        case WH_MSGFILTER:       return "WH_MSGFILTER";
        case WH_JOURNALRECORD:   return "WH_JOURNALRECORD";
        case WH_JOURNALPLAYBACK: return "WH_JOURNALPLAYBACK";
        case WH_KEYBOARD:        return "WH_KEYBOARD";
        case WH_GETMESSAGE:      return "WH_GETMESSAGE";
        case WH_CALLWNDPROC:     return "WH_CALLWNDPROC";
        case WH_CBT:             return "WH_CBT";
        case WH_SYSMSGFILTER:    return "WH_SYSMSGFILTER";
        case WH_MOUSE:           return "WH_MOUSE";
        case WH_DEBUG:           return "WH_DEBUG";
        case WH_SHELL:           return "WH_SHELL";
        case WH_FOREGROUNDIDLE:  return "WH_FOREGROUNDIDLE";
        case WH_CALLWNDPROCRET:  return "WH_CALLWNDPROCRET";
        case WH_KEYBOARD_LL:     return "WH_KEYBOARD_LL";
        case WH_MOUSE_LL:        return "WH_MOUSE_LL";
        default:                 return "?";
    }
}

// ---------------------------------------------------------------------------
// THE ESCAPE SET
//
// Deliberately short. Every key here is one whose ONLY job is to leave the
// application -- none of them is bound to anything inside a POL title, so
// letting them past costs the game nothing. Anything broader (function keys,
// PrintScreen, a media key) would be a judgement about the game's own bindings
// that this project has not measured, and a keyboard hook is the wrong place to
// guess.
//
//   Win (either)   opens the shell. The single most reported one.
//   Alt+Tab        the task switcher.
//   Alt+Esc        cycle windows.
//   Ctrl+Esc       Start menu, i.e. the Win key for a keyboard without one.
//
// Ctrl+Alt+Del is NOT here: it is a secure attention sequence the OS handles
// below any hook, so it was never blocked and needs no help.
//
// Alt is read from the event, not from GetAsyncKeyState, wherever the event
// carries it -- a low-level hook sees the ALT state at the moment the key was
// pressed, and asking the OS afterwards races the user releasing it.
static bool key_is_escape(UINT vk, bool alt)
{
    if (vk == VK_LWIN || vk == VK_RWIN) return true;
    if (vk == VK_TAB && alt) return true;
    if (vk == VK_ESCAPE) {
        if (alt) return true;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) return true;   // Ctrl+Esc
    }
    return false;
}

// One slot per wrapped hook: the system does not tell a hook proc which HHOOK it
// belongs to, so each wrapped hook needs its own thunk to know what to chain to.
// gate_prev is PER SLOT: two wrapped mouse hooks are two independent
// downstream procs, and each needs its own release. A shared flag would give
// the button-ups to whichever one happened to be called first on the edge.
// `global` is thread==0 -- the only keyboard hooks that can fire while another
// application is in front. `kdown` is what this proc has been told is held, so
// the release can be exact rather than a blast of 256 key-ups.
struct Slot { HOOKPROC orig; int id; LONG seen; bool gate_prev;
              bool global; bool kgate_prev; BYTE kdown[256]; };
// 24, not 4: each wrapped hook needs an index-bound thunk (below), and the table only
// grows -- a title that installs/removes a WH_MOUSE hook on each screen transition
// exhausted 4 slots within a session and then wrapped nothing, so tracing went blind.
// Raised 16 -> 24 on 2026-08-26 when the keyboard hooks started sharing this table:
// one pol.exe runs many titles in a row and each brings its own pair, so the
// headroom that was ample for mice alone no longer is. Running out is not a crash --
// wrap() returns the proc unwrapped -- but it silently means the Windows key stops
// escaping partway through a session, so the exhaustion is now LOGGED.
// (A true reclaim would hook UnhookWindowsHookEx to free the slot whose thunk was
// returned; still not worth that machinery here.)
static Slot g_slot[24];
static LONG g_nslot = 0;

static LRESULT dispatch(int idx, int nCode, WPARAM wp, LPARAM lp)
{
    Slot* s = &g_slot[idx];

    // KEYBOARD: let the escape keys past the title's proc entirely.
    //
    // nCode < 0 is the documented "pass it on without inspecting" case and is
    // handled by the tail forward, as it must be for both hook types.
    if (nCode >= 0 && (s->id == WH_KEYBOARD_LL || s->id == WH_KEYBOARD)) {
        UINT vk  = 0;
        bool alt = false;
        if (s->id == WH_KEYBOARD_LL) {
            // WH_KEYBOARD_LL -> KBDLLHOOKSTRUCT. LLKHF_ALTDOWN is the ALT state
            // as of this keystroke.
            const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
            if (!k) return s->orig ? s->orig(nCode, wp, lp)
                                   : CallNextHookEx(NULL, nCode, wp, lp);
            vk  = (UINT)k->vkCode;
            alt = (k->flags & LLKHF_ALTDOWN) != 0;
        } else {
            // WH_KEYBOARD -> vk in wParam, and lParam bit 29 is the context code,
            // which is ALT.
            vk  = (UINT)wp;
            alt = (lp & (1 << 29)) != 0;
        }
        InterlockedIncrement(&g_n_key_msgs);
        if (g_key_escape && key_is_escape(vk, alt)) {
            LONG n = InterlockedIncrement(&g_n_key_let);
            // First few only. This fires on key DOWN and key UP, and a held Win
            // key repeats -- an unsampled line here would be its own bug report.
            if (n <= 8)
                logf("[hk] %s vk=0x%02X%s LET THROUGH -- the title's hook proc %p "
                     "was NOT offered it, so the shell gets it "
                     "([dx] key_escape=0 to stop)",
                     hook_name(s->id), vk, alt ? " (ALT down)" : "",
                     (void*)s->orig);
            return CallNextHookEx(NULL, nCode, wp, lp);
        }

        // SEAM 6 of the focus gate (inputgate.cpp).
        //
        // GLOBAL hooks ONLY, and that restriction is the whole design: a THREAD
        // hook fires only for messages that thread retrieves, and a thread whose
        // window is not in front is not retrieving keystrokes -- gating it could
        // only ever take away input the title was legitimately owed. A hook
        // installed with thread==0, though, is handed every keystroke on the
        // desktop, which is exactly the leak this seam exists for.
        //
        // KEY: THE RELEASE, again. A proc cut off between a key's down and its
        // up believes the key is still held. So on the way out it is handed the
        // ups for whatever we have actually told it is down -- reusing the
        // structure the OS just gave us and changing only the transition bits,
        // so the coordinates, the scan code and the struct KIND stay whatever
        // this hook type really expects. Down/up is read from the message the
        // same way for both kinds: WM_KEYUP/WM_SYSKEYUP in wParam for LL,
        // lParam bit 31 for WH_KEYBOARD.
        if (s->global) {
            const bool is_up = (s->id == WH_KEYBOARD_LL)
                                   ? (wp == WM_KEYUP || wp == WM_SYSKEYUP)
                                   : ((lp & (1u << 31)) != 0);
            bool blocked = false;
            if (inputgate_key_edge(&s->kgate_prev, &blocked) && s->orig) {
                int sent = 0;
                for (int v = 0; v < 256; v++) {
                    if (!s->kdown[v]) continue;
                    s->kdown[v] = 0;
                    if (s->id == WH_KEYBOARD_LL) {
                        KBDLLHOOKSTRUCT k = *(const KBDLLHOOKSTRUCT*)lp;
                        k.vkCode = (DWORD)v;
                        k.flags |= LLKHF_UP;
                        s->orig(nCode, (WPARAM)WM_KEYUP, (LPARAM)&k);
                    } else {
                        // bit 31 = transition (1 = being released), bit 30 = the
                        // previous state (1 = was down), which together are what
                        // a key-up looks like to a WH_KEYBOARD proc.
                        s->orig(nCode, (WPARAM)v, lp | (1u << 31) | (1u << 30));
                    }
                    sent++;
                }
                logf("[gate] %s (GLOBAL): another application is in front -- "
                     "released %d held key(s) to the title's hook proc %p and "
                     "stopped forwarding ([dx] key_focus_gate=0 to stop)",
                     hook_name(s->id), sent, (void*)s->orig);
            }
            if (blocked) {
                inputgate_note_key_blocked(6, 0);
                return CallNextHookEx(NULL, nCode, wp, lp);
            }
            // Delivering: remember what the proc is being told, so the release
            // above can be exact. Tracked only for the hooks that are gated --
            // a thread hook cannot be cut off mid-keystroke by this gate, so it
            // has nothing to release and needs no bookkeeping.
            {
                UINT v = (s->id == WH_KEYBOARD_LL)
                             ? ((const KBDLLHOOKSTRUCT*)lp)->vkCode : (UINT)wp;
                if (v < 256) s->kdown[v] = is_up ? 0 : 1;
            }
        }
        return s->orig ? s->orig(nCode, wp, lp)
                       : CallNextHookEx(NULL, nCode, wp, lp);
    }

    if (nCode >= 0 && lp && (s->id == WH_MOUSE || s->id == WH_MOUSE_LL)) {
        // SEAM 2 of the focus gate (inputgate.cpp).
        //
        // This chain is SE's, and our log says it is SYSTEM-WIDE:
        //   SetWindowsHookExA(WH_MOUSE=7, proc=... in PolHook_orig.DLL,
        //                     hmod=PolHook_orig.DLL, thread=0)
        // thread=0 means every mouse message on the desktop, so a title
        // downstream of it keeps seeing the mouse while you work in another
        // application. Withholding = simply not calling the title's proc and
        // passing the message on down the chain instead.
        //
        // THE RELEASE. FMO and FE mouse-look on a HELD BUTTON. A proc that is
        // cut off mid-drag never sees the button go up and keeps turning for
        // ever -- strictly worse than the bug being fixed. So on the way out we
        // hand it the three button-ups first, reusing the structure the OS just
        // gave us and changing only the message id, so the coordinates and the
        // struct kind are whatever this hook type really expects.
        {
            bool blocked = false;
            if (inputgate_edge(&s->gate_prev, &blocked) && s->orig) {
                static const UINT UPS[] = { 0x0202 /*WM_LBUTTONUP*/,
                                            0x0205 /*WM_RBUTTONUP*/,
                                            0x0208 /*WM_MBUTTONUP*/ };
                for (int u = 0; u < 3; u++) s->orig(nCode, (WPARAM)UPS[u], lp);
                logf("[gate] %s: another application took the foreground -- "
                     "released the mouse buttons to %s's hook proc %p and "
                     "stopped forwarding", hook_name(s->id), "the title",
                     (void*)s->orig);
            }
            if (blocked) {
                inputgate_note_blocked(1);
                return CallNextHookEx(NULL, nCode, wp, lp);
            }
        }
        // WH_MOUSE -> MOUSEHOOKSTRUCT, WH_MOUSE_LL -> MSLLHOOKSTRUCT.
        // Both begin with a POINT in SCREEN coordinates, which is all we touch.
        POINT* pt = (POINT*)lp;
        LONG n = InterlockedIncrement(&s->seen);
        InterlockedIncrement(&g_n_mouse_msgs);

        // BUTTONS ARE ALWAYS LOGGED, never sampled.
        //
        // The 1-in-600 sample exists because WM_MOUSEMOVE floods; but a click is
        // rare and is the whole question. Measured on Fantasy Earth: the game
        // window receives SETCURSOR and WM_MOUSEMOVE and *zero* button messages,
        // so the open question is whether buttons travel this hook chain instead
        // -- and a sampled log can never answer it, because every sampled event
        // so far has been a move (512), wheel (522/526) or NC-move (160).
        // Cheap: a handful of lines per session, versus thousands for moves.
        const unsigned m = (unsigned)wp;
        const bool is_button =
            (m >= 0x201 && m <= 0x209) ||     // L/R/M down, up, dblclk, wheel
            (m >= 0x2A1 && m <= 0x2A3) ||     // NC hover/leave neighbours
            (m >= 0xA1  && m <= 0xA9);        // WM_NCLBUTTONDOWN .. NCMBUTTONDBLCLK
        if (is_button)
            logf("[hk] %s BUTTON msg=0x%03X pt=(%ld,%ld)  <-- a click DID reach "
                 "the %s chain", hook_name(s->id), m, pt->x, pt->y,
                 hook_name(s->id));

        if (g_hook_trace && (n <= 16 || (n % 600) == 0 || is_button)) {
            POINT cur; cur.x = cur.y = -1;
            GetCursorPos(&cur);
            RECT gr; ZeroMemory(&gr, sizeof(gr));
            POINT org; org.x = 0; org.y = 0;
            HWND gw = d3d8_game_window();
            if (gw && IsWindow(gw)) { GetClientRect(gw, &gr); ClientToScreen(gw, &org); }
            logf("[hk] %s #%ld msg=%u pt=(%ld,%ld) cursor=(%ld,%ld) "
                 "gameclient=(%ld,%ld %ldx%ld) -> client-relative would be (%ld,%ld)",
                 hook_name(s->id), n, (unsigned)wp, pt->x, pt->y, cur.x, cur.y,
                 org.x, org.y, gr.right, gr.bottom, pt->x - org.x, pt->y - org.y);
        }

        // THE TRANSFORM.
        //
        // Measured: pt arrives as a raw screen position spanning the whole
        // desktop -- (881,609), (931,658) on a 1280x720 screen -- while the title
        // works in 640x480. In 2003 the display really was 640x480, so pt was
        // already a game coordinate. That equivalence is what modern Windows
        // breaks, and it breaks identically in fullscreen, which is why the
        // symptom never depended on windowing.
        //
        // Subtracting the client origin alone is NOT sufficient: with the window
        // at (0,0) that is a no-op and pt still ranges 0..1279 in a 640 field.
        // So:
        //   1 = client-relative + CLAMP to the client size. The pointer tracks
        //       the real mouse 1:1 over the game and stops at the edges, which
        //       is what a windowed game should do.
        //   2 = SCALE the whole desktop onto the game field, i.e. emulate the
        //       old "screen IS 640x480" equivalence. Use if the title insists on
        //       reaching every coordinate.
        if (g_hook_translate) {
            HWND gw = d3d8_game_window();
            if (gw && IsWindow(gw)) {
                RECT c;
                POINT org;
                org.x = 0; org.y = 0;
                if (GetClientRect(gw, &c) && ClientToScreen(gw, &org) &&
                    c.right > 0 && c.bottom > 0) {
                    LONG ox = pt->x, oy = pt->y;
                    if (g_hook_translate >= 2) {
                        int sw = GetSystemMetrics(SM_CXSCREEN);
                        int sh = GetSystemMetrics(SM_CYSCREEN);
                        if (sw > 0 && sh > 0) {
                            pt->x = (LONG)((__int64)pt->x * c.right  / sw);
                            pt->y = (LONG)((__int64)pt->y * c.bottom / sh);
                        }
                    } else {
                        pt->x -= org.x;
                        pt->y -= org.y;
                        if (pt->x < 0) pt->x = 0;
                        if (pt->y < 0) pt->y = 0;
                        if (pt->x > c.right  - 1) pt->x = c.right  - 1;
                        if (pt->y > c.bottom - 1) pt->y = c.bottom - 1;
                    }
                    if (g_hook_trace && (n <= 16 || (n % 600) == 0))
                        logf("[hk]   translated (%ld,%ld) -> (%ld,%ld)  [mode %d]",
                             ox, oy, pt->x, pt->y, g_hook_translate);
                }
            }
        }
    }
    return s->orig ? s->orig(nCode, wp, lp) : CallNextHookEx(NULL, nCode, wp, lp);
}

// One index-bound thunk per slot: the system never tells a hook proc which HHOOK it
// is, so each slot needs its own entry point that hard-codes its index into dispatch().
#define HOOKSPY_THUNK(n) static LRESULT CALLBACK proc##n(int c, WPARAM w, LPARAM l) { return dispatch(n, c, w, l); }
HOOKSPY_THUNK(0)  HOOKSPY_THUNK(1)  HOOKSPY_THUNK(2)  HOOKSPY_THUNK(3)
HOOKSPY_THUNK(4)  HOOKSPY_THUNK(5)  HOOKSPY_THUNK(6)  HOOKSPY_THUNK(7)
HOOKSPY_THUNK(8)  HOOKSPY_THUNK(9)  HOOKSPY_THUNK(10) HOOKSPY_THUNK(11)
HOOKSPY_THUNK(12) HOOKSPY_THUNK(13) HOOKSPY_THUNK(14) HOOKSPY_THUNK(15)
HOOKSPY_THUNK(16) HOOKSPY_THUNK(17) HOOKSPY_THUNK(18) HOOKSPY_THUNK(19)
HOOKSPY_THUNK(20) HOOKSPY_THUNK(21) HOOKSPY_THUNK(22) HOOKSPY_THUNK(23)
static HOOKPROC const THUNK[24] = {
    proc0,  proc1,  proc2,  proc3,  proc4,  proc5,  proc6,  proc7,
    proc8,  proc9,  proc10, proc11, proc12, proc13, proc14, proc15,
    proc16, proc17, proc18, proc19, proc20, proc21, proc22, proc23
};

static HOOKPROC wrap(int id, HOOKPROC fn, DWORD tid)
{
    // The mouse hooks (coordinates) and the keyboard hooks (the escape set).
    // Nothing else is wrapped: a wrapper on a hook we have no business in is
    // pure risk, and it would spend slots this table has already run out of once.
    const bool mouse = (id == WH_MOUSE || id == WH_MOUSE_LL);
    const bool key   = (id == WH_KEYBOARD || id == WH_KEYBOARD_LL);
    if (!mouse && !key) return fn;
    // A keyboard wrapper with the escape off would be a pure passthrough that
    // still burns a slot. key_escape is re-read live, though, so the wrapper has
    // to exist for a later flip to mean anything -- install it, and let the
    // per-keystroke test decide. Only a session that starts with it off and never
    // turns it on pays, and that session pays one slot.
    LONG i = InterlockedIncrement(&g_nslot) - 1;
    if (i >= (LONG)_countof(g_slot)) {
        // Once per session, not per hook: a title that churns hooks would
        // otherwise fill the log with this one line.
        if (i == (LONG)_countof(g_slot))
            logf("[hk] slot table full (%u) -- further %s hooks are left UNWRAPPED. "
                 "From here the mouse trace goes blind and the Windows key stops "
                 "escaping this title.", (unsigned)_countof(g_slot), hook_name(id));
        return fn;
    }
    g_slot[i].orig = fn;
    g_slot[i].id   = id;
    g_slot[i].seen = 0;
    g_slot[i].gate_prev = false;
    g_slot[i].global    = (tid == 0);
    g_slot[i].kgate_prev = false;
    ZeroMemory(g_slot[i].kdown, sizeof(g_slot[i].kdown));
    logf("[hk] wrapping %s proc %p as slot %ld%s", hook_name(id), fn, i,
         (tid == 0 && (id == WH_KEYBOARD || id == WH_KEYBOARD_LL))
             ? "  <-- GLOBAL keyboard hook: it sees every keystroke on the "
               "desktop, so the focus gate applies to it (seam 6)"
             : "");
    return THUNK[i];
}

static void log_install(const char* which, int id, HOOKPROC fn, HINSTANCE mod, DWORD tid)
{
    char leaf[MAX_PATH] = "?";
    if (mod) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA((HMODULE)mod, path, MAX_PATH);
        const char* s = strrchr(path, '\\');
        strcpy_s(leaf, sizeof(leaf), s ? s + 1 : path);
    }
    // Name the module the PROC lives in -- that is who really owns the hook.
    char owner[MAX_PATH] = "?";
    HMODULE om = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)fn, &om) && om) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(om, path, MAX_PATH);
        const char* s = strrchr(path, '\\');
        strcpy_s(owner, sizeof(owner), s ? s + 1 : path);
    }
    logf("[hk] %s(%s=%d, proc=%p in %s, hmod=%s, thread=%lu)",
         which, hook_name(id), id, fn, owner, leaf, tid);
}

// WHY THE RESULT IS LOGGED, and it matters more than it looks.
//
// wrap() substitutes a proc that lives in OUR module while `mod` still names
// the caller's (PolHook_orig.DLL). For a LOW-LEVEL hook that is fine and is
// why the keyboard escape works: WH_KEYBOARD_LL / WH_MOUSE_LL procs are called
// back in the INSTALLING process, and need not be in a DLL at all.
//
// For a GLOBAL (tid == 0) WH_MOUSE hook it is NOT obviously fine: Windows
// injects `mod` into every process and calls the proc at its address THERE,
// and our thunk is not in that module. This project has been substituting a
// proc into SE's `thread=0` WH_MOUSE install for months without ever recording
// whether the call SUCCEEDED. A NULL here is the answer, and it changes which
// seam of the focus gate is the one doing the work for FMO.
static HHOOK log_result(const char* which, int id, DWORD tid, HHOOK h)
{
    if (!h)
        logf("[hk] %s(%s) FAILED (err=%lu)%s", which, hook_name(id),
             GetLastError(),
             tid == 0 ? "  <-- a GLOBAL hook: if this started failing, the proc "
                        "we substituted is not inside the module that was named"
                      : "");
    else if (tid == 0)
        logf("[hk] %s(%s, thread=0 GLOBAL) -> %p", which, hook_name(id), h);
    return h;
}

static HHOOK WINAPI hook_SetWindowsHookExA(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid)
{
    InterlockedIncrement(&g_n_installed);
    log_install("SetWindowsHookExA", id, fn, mod, tid);
    return log_result("SetWindowsHookExA", id, tid,
                      real_SetWindowsHookExA(id, wrap(id, fn, tid), mod, tid));
}

static HHOOK WINAPI hook_SetWindowsHookExW(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid)
{
    InterlockedIncrement(&g_n_installed);
    log_install("SetWindowsHookExW", id, fn, mod, tid);
    return log_result("SetWindowsHookExW", id, tid,
                      real_SetWindowsHookExW(id, wrap(id, fn, tid), mod, tid));
}

void* hookspy_real_SetWindowsHookExA() { return (void*)real_SetWindowsHookExA; }
void* hookspy_hook_SetWindowsHookExA() { return (void*)hook_SetWindowsHookExA; }
void* hookspy_real_SetWindowsHookExW() { return (void*)real_SetWindowsHookExW; }
void* hookspy_hook_SetWindowsHookExW() { return (void*)hook_SetWindowsHookExW; }

void hookspy_resolve()
{
    if (!g_hook_enable) return;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    real_SetWindowsHookExA = (PFN_SetWindowsHookExA)GetProcAddress(u, "SetWindowsHookExA");
    real_SetWindowsHookExW = (PFN_SetWindowsHookExW)GetProcAddress(u, "SetWindowsHookExW");
    // EAT, like every other hook here: polcore and PolHook are POL1-packed and
    // re-resolve their imports, so an IAT patch on them is a race we lose.
    if (real_SetWindowsHookExA)
        dx_eat_patch_export(u, "SetWindowsHookExA", (void*)hook_SetWindowsHookExA,
                            "user32!SetWindowsHookExA");
    if (real_SetWindowsHookExW)
        dx_eat_patch_export(u, "SetWindowsHookExW", (void*)hook_SetWindowsHookExW,
                            "user32!SetWindowsHookExW");
    logf("[hk] enable=1 trace=%d translate=%d (0 = observe only)",
         g_hook_trace, g_hook_translate);
}

void hookspy_summary()
{
    if (!g_hook_enable) return;
    logf("[hk] summary: keyboard messages seen=%ld, escape keys let through=%ld "
         "(%s)", g_n_key_msgs, g_n_key_let,
         !g_key_escape                     ? "OFF by [dx] key_escape=0" :
         (g_n_key_msgs && !g_n_key_let)    ? "a title's keyboard hook WAS wrapped and "
                                             "no escape key was ever pressed" :
         g_n_key_let                       ? "the Windows key and Alt+Tab reached the "
                                             "shell past this title" :
                                             "no title installed a keyboard hook this "
                                             "session -- if the Windows key still does "
                                             "nothing, look at [din] NOWINKEY instead");
    logf("[hk] summary: hooks installed=%ld, mouse messages seen=%ld, wrapped=%ld",
         g_n_installed, g_n_mouse_msgs, g_nslot);
    if (!g_n_mouse_msgs)
        logf("[hk] NOTE: no WH_MOUSE/WH_MOUSE_LL traffic. The title reads the "
             "pointer some other way -- check WH_GETMESSAGE/WH_CALLWNDPROC in the "
             "install list above, which carry whole messages rather than a POINT.");
}
