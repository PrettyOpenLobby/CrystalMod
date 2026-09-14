// maskguard.cpp -- keep the Viewer's PlayOnlineMask* window ALIVE from process
// start, and optionally keep it out of a one-window compositor's way.
//
// WHY THIS FILE EXISTS. Reported on a real Steam Deck in Game Mode, 2026-08-15:
// launching Fantasy Earth or Front Mission Online under gamescope KILLS THE MASK
// WINDOW, and the Viewer then raises an error dialog and exits. FFXI, which does
// not go through the same window juggling, launches fine on the same machine.
//
// The mechanism that fits every fact currently in hand -- and this file's log
// lines are what will CONFIRM OR KILL it, because nothing below is measured yet:
//
//   * pol.exe registers the mask class at 0x0041C455 and its wndproc 0x0041BEC4
//     is a bare `jmp [DefWindowProcA]` thunk (measured). So a close request from a window manager is DESTRUCTIVE by
//     default: DefWindowProc turns WM_CLOSE into DestroyWindow, and there is no
//     application code anywhere in the path to refuse it.
//   * polcore is handed that HWND -- IPOLCoreCom::SetMaskWindowHandle (slot 22)
//     -- and IPOLCoreCom::CreateInput (slot 12) is bound to it, verified
//     identical across four sessions. Once the window is destroyed, polcore holds
//     a dangling handle and the next Show/Hide/IsVisibleMaskWindow has nothing to
//     act on. "Mask dies -> error -> exit" is exactly that shape.
//   * gamescope displays ONE window. A borderless screen-sized overlay at (0,0),
//     owned by a process that also owns a game window, is precisely the case a
//     one-window compositor has to arbitrate -- and the Viewer is a shell that
//     swaps between three such windows per title launch.
//
// WHY IT IS NOT PART OF d3d8hook.cpp's MASK HANDLING. `tame_mask_window()` is
// only reachable from a successful windowed CreateDevice/Reset or the d3d9 path.
// A launch that dies BEFORE device creation therefore gets no mask handling at
// all -- and leaves no `[d3d] mask window` line either, so the log cannot even
// say what happened. That gap was flagged earlier and it is exactly
// the window in which FE and FMO are dying. This file arms from process start
// instead, so the mask is guarded from the moment it exists.
//
// The two coexist by design: THIS file owns the mask's SURVIVAL, d3d8hook owns
// its GEOMETRY once a game window exists. Subclasses chain -- we go on first and
// `install_msgspy` goes on over us, calling back through -- so neither has to
// move and d3d_unmask keeps working unchanged.
//
// [dx] mask_guard
//   0  off -- the pre-build-24 behaviour exactly.
//   1  DEFAULT. Watch and protect: subclass the mask as soon as it appears, log
//      its whole lifecycle, refuse WM_CLOSE / SC_CLOSE under a compositor -- AND,
//      since 2026-09-07, HOLD THE INVARIANT below: the mask is never on top,
//      never opaque, never in Alt+Tab and never the foreground window, for the
//      whole life of the process, whatever the Viewer or a title does to it.
//   2  = 1 plus shrink it to 1x1 off-screen at birth, so a one-window compositor
//      (gamescope) cannot pick it even as an invisible window. Deck A/B setting.
//
// THE INVARIANT, AND WHY IT LIVES HERE (2026-09-07). "The POL mask takes over my
// whole screen when I launch Fantasy Earth" came back for the Nth time. Every
// earlier fix sat on a TITLE-DEPENDENT path: d3d8hook's tame_mask_window() runs
// from a successful CreateDevice, on a branch chosen by the caller's return
// address and by whether the title is "excepted" or "native-windowed". For FE
// that chain is structurally dead -- it asks for a windowed device itself, so it
// takes the pass-through branch; that branch's mask arm is gated on `excepted`,
// which FE is not; and with Ashita in the process the return address names
// Ashita.dll anyway. The one attempt to un-gate it (build 161) crashed FE and was
// never committed. Meanwhile the mask has ALWAYS been born as a 1920x1080
// WS_EX_TOPMOST popup, and at mask_guard=1 nothing touched that style: the Viewer
// shows it at title launch (SetWindowPos SHOWWINDOW|NOZORDER|NOACTIVATE, measured)
// and a black topmost screen-sized window is the result. Alt+Tab "does not work"
// because whatever you switch to lands UNDER a topmost window.
//
// So the rule moved to the one place that sees the mask from its first message
// to its last, for every title, on every path: its own window procedure.
//
//   * At arm(): WS_EX_LAYERED alpha 0 (paints nothing), WS_EX_TRANSPARENT
//     (click-through), WS_EX_TOOLWINDOW minus WS_EX_APPWINDOW (no taskbar button,
//     no Alt+Tab entry), WS_EX_NOACTIVATE (never picked as the next foreground
//     window when a title's window closes -- the measured way it became the
//     foreground window mid-FE), and WS_EX_TOPMOST stripped.
//   * WM_WINDOWPOSCHANGING: an HWND_TOPMOST insert is rewritten to
//     HWND_NOTOPMOST and SWP_NOACTIVATE is added. Geometry is NOT touched: the
//     Viewer may size it to the screen all it likes, an alpha-0 click-through
//     tool window that size is harmless, and d3d8hook's alignment (which exists
//     for polcore's cursor mapping on Tetra Master, not for the screen) still
//     owns the rect when it runs.
//   * WM_STYLECHANGING (GWL_EXSTYLE): the bits above are re-imposed on whatever
//     anyone writes, and WM_STYLECHANGED re-asserts alpha 0 (a freshly-set
//     WS_EX_LAYERED starts opaque).
//   * WM_MOUSEACTIVATE -> MA_NOACTIVATE, and d3d8hook's [foc] hooks refuse
//     SetForegroundWindow / BringWindowToTop / SetActiveWindow aimed at it
//     (maskguard_refuse_activation).
//
// This is exactly the style d3d_unmask=2 has applied AFTER CreateDevice on TM
// and FMO for weeks, so its safety is measured, not argued; the only change is
// WHEN (from birth) and WHO (this file, unconditionally). It does not hide the
// window: SW_HIDE black-screened the display on 2026-08-18 and the Viewer asks
// polcore IsVisibleMaskWindow. An alpha-0 window is still IsWindowVisible.
//
// WHY SWALLOWING WM_CLOSE UNCONDITIONALLY WAS WRONG -- measured 2026-08-15, and
// the reason the deferred-honour machinery below exists.
//
// This file used to argue: DestroyWindow does NOT send WM_CLOSE -- it sends
// WM_DESTROY -- so the Viewer's own teardown of its own mask never travels
// through the message we eat, and a WM_CLOSE here can only have come from
// OUTSIDE the application. The first half is true of DestroyWindow. The
// conclusion is false: **pol.exe's own shutdown closes its mask with WM_CLOSE.**
//
// Refusing it hung the Viewer on every ordinary exit. The mask is the LAST
// un-owned top-level window the process owns, so its destruction is what ends
// the message loop; with it refused, no WM_QUIT is ever posted, GetMessage never
// returns 0, ExitProcess is never reached, and pol.exe sits in the background
// with ~23 threads and one invisible window -- still holding the `PlayOnlineUS`
// single-instance mutex, so the NEXT launch dies a few calls in (a
// self-inflicted wound). Five armed sessions,
// perfect correlation:
//
//     [mask] WM_CLOSE ... REFUSED   reached [mask] summary (i.e. exited)
//     0  (pids 390928/446576/448956)     yes, 3/3
//     1  (pids 414736/363388)            no,  0/2
//
// An EXTERNAL WM_CLOSE was never credible for those two either: the mask is
// created `visible=0`, and nothing outside the process can click or Alt+F4 an
// invisible window.
//
// THE FIX IS NOT "STOP REFUSING". On the Deck the refusal is the whole point, and
// there is no message-level way to tell a compositor's WM_CLOSE from the Viewer's
// own -- both arrive at this wndproc as a bare WM_CLOSE, and InSendMessage()
// cannot separate them (winex11 delivers a WM close request from inside the
// process too). So the discriminator is not WHO SENT IT but WHAT ELSE IS LEFT:
//
//   * The Viewer is shutting down  -> the mask is the only un-owned top-level
//     window the process still has. Measured on the hung pid 363388: EnumWindows
//     returned PlayOnlineMaskUS and a `Default IME` owned BY it, nothing else.
//   * A compositor is killing the mask mid-session -> the shell window or the
//     game window is still there beside it. This holds during a title launch too,
//     which is exactly when the Deck bug bites, and it does not depend on that
//     window being VISIBLE -- EnumWindows returns hidden top-level windows, so a
//     shell that hides itself behind a game still counts as alive.
//
// So a refused close is not final, it is PENDING: refuse it now, and let the
// watcher honour it later if the mask is still the last window standing after
// [dx] mask_close_grace ms. Honouring is a PostMessage of the same WM_CLOSE with
// a bypass flag set -- posted, not DestroyWindow'd, because DestroyWindow is only
// legal on the thread that created the window, and the mask belongs to the
// Viewer's UI thread. That thread is still pumping (the hung process reports
// Responding=True; the hang IS its message loop waiting for a WM_QUIT that never
// comes), so the post lands. Since the mask class's wndproc is a bare
// DefWindowProcA thunk, letting the WM_CLOSE through is exactly a DestroyWindow
// -- the delay is the only difference from never having refused it.
//
// Two safeties, both because a false honour re-opens the Deck bug:
//   * The grace clock RESETS the moment any sibling top-level window reappears,
//     so a shell that swaps windows during a title launch can never mature it.
//   * A close is never honoured until a sibling has been seen at least once, so
//     an early-startup transient (mask up, main window not yet created) cannot
//     mature it either.
//
// [dx] mask_close_grace = 0 restores the old refuse-forever behaviour exactly.
//
// WHY MODE 2 SHRINKS RATHER THAN HIDES: the Viewer asks polcore
// IsVisibleMaskWindow, and a window put down with ShowWindow(SW_HIDE) could
// answer that differently. A 1x1 window off-screen is still visible, still
// hit-tests, still has a valid HWND -- it is only unpaintable and unpickable.
// The cosmetic cost, on every platform, is that the mask no longer blacks out the
// desktop during a transition. Since 2026-09-07 that cost is paid in every mode
// >= 1 (the invariant above), so mode 2's only remaining difference is the shrink.

#include "polshim.h"

// 0 off, 1 protect, 2 protect + hide from the compositor.
static int    g_mode   = 1;
// ms the mask must be the process's LAST un-owned top-level window before a
// refused close is honoured. 0 = never honour (the original refuse-forever).
static int    g_grace  = 2000;
static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;

// The window we are guarding and the wndproc we displaced. Written by the watcher
// thread, read by the Viewer's UI thread inside guard_proc.
static HWND    volatile g_mask = NULL;
static WNDPROC volatile g_orig = NULL;
static char             g_cls[32] = "";

static LONG g_n_seen = 0;      // masks armed (a Viewer session can create several)
static LONG g_n_close = 0;     // close requests refused
static LONG g_n_gone = 0;      // masks that died anyway
static LONG g_n_honoured = 0;  // refusals later honoured as a shutdown

// The invariant's own counters (see the block comment at the top of the file).
static LONG g_n_zorder   = 0;  // SetWindowPos calls rewritten (HWND_TOPMOST / activating)
static LONG g_n_style    = 0;  // ex-style writes that had to be corrected
static LONG g_n_shown    = 0;  // times the Viewer showed the mask
static LONG g_n_activate = 0;  // activation attempts refused (wndproc + [foc] hooks)

// What the mask must always carry, and must never carry. WS_EX_TOPMOST cannot be
// set through SetWindowLong on a live window (only SetWindowPos does that), but
// it is listed so a style READ-modify-write by the Viewer cannot carry it back.
static const LONG MASK_EX_ADD  = WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                                 WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
static const LONG MASK_EX_DROP = WS_EX_APPWINDOW | WS_EX_TOPMOST;

// Impose the invariant on a mask from ANY thread. Idempotent, and every call is
// cheap, so it is safe to repeat on each style change. SetWindowLong sends
// WM_STYLECHANGING/CHANGED to the window's own thread synchronously; that thread
// is the Viewer's UI thread, which is pumping (mode 2 has done this cross-thread
// since build 24). The z-order fix is ASYNC for the reason d3d8hook learned:
// a synchronous cross-thread SetWindowPos from inside CreateDevice deadlocks.
static void keep_invisible(HWND h)
{
    LONG ex  = GetWindowLongA(h, GWL_EXSTYLE);
    LONG nex = (ex | MASK_EX_ADD) & ~MASK_EX_DROP;
    if (nex != ex) SetWindowLongA(h, GWL_EXSTYLE, nex);
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                 SWP_ASYNCWINDOWPOS);
}

// A refusal is pending until it is either honoured or the mask dies. Written by
// the Viewer's UI thread inside guard_proc, read by the watcher.
static volatile LONG g_pending = 0;
// Set by the watcher immediately before it re-posts the close, so guard_proc lets
// that one through instead of refusing it again.
static volatile LONG g_allow = 0;
// Grace-clock state, watcher thread only.
static int   g_alone_armed  = 0;
static DWORD g_alone_since  = 0;
static int   g_saw_sibling  = 0;   // the app got as far as having a real window

// True only under a one-window compositor -- gamescope / Steam Deck Game Mode, reached
// through Proton (Wine). That is the ONLY environment where the mask is killed from
// outside and needs the WM_CLOSE refusal. On native Windows the mask is never killed by
// a compositor, so refusing its close does nothing but hang pol.exe as a ghost process
// (the reported "vanishes but stays running"): the refusal waits for the mask to become
// the last window, and a lingering sibling -- the shell, an IME window -- means it never
// does. So there, the close is let straight through.
static bool g_compositor = false;

static bool detect_compositor()
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt && GetProcAddress(nt, "wine_get_version")) return true;   // Wine / Proton
    // Belt and suspenders in case a future Wine hides that export.
    if (GetEnvironmentVariableA("GAMESCOPE_WAYLAND_DISPLAY", NULL, 0) > 0) return true;
    if (GetEnvironmentVariableA("SteamDeck", NULL, 0) > 0) return true;
    return false;
}

// [dx] mask_guard_force: override the auto-detect. -1 auto (default), 0 never refuse
// (treat as native Windows), 1 always refuse (treat as a compositor). For A/B on a Deck
// or forcing the safe behaviour on a machine the detector gets wrong.
static int g_force = -1;

void maskguard_configure(const wchar_t* ini)
{
    g_mode  = GetPrivateProfileIntW(L"dx", L"mask_guard",       1,    ini);
    g_grace = GetPrivateProfileIntW(L"dx", L"mask_close_grace", 2000, ini);
    if (g_grace < 0) g_grace = 0;
    g_force = GetPrivateProfileIntW(L"dx", L"mask_guard_force", -1, ini);
    g_compositor = (g_force == 1) ? true
                 : (g_force == 0) ? false
                 : detect_compositor();
    logf("[mask] guard mode=%d grace=%dms -- %s (%s): WM_CLOSE will be %s",
         g_mode, g_grace,
         g_compositor ? "one-window compositor" : "native Windows",
         g_force >= 0 ? "forced" : "auto-detected",
         g_compositor ? "refused and deferred (mask protected from the compositor)"
                      : "let through (no compositor to protect against)");
}

// Live-reload for the in-game settings dialog: re-read the SAFE subset without a
// restart. g_mode and g_grace are plain state the guard consults per message /
// per watcher pass, so new values simply apply from here on -- a mode change
// takes effect at the next arm(), which is fine.
//
// Deliberately NOT re-read:
//   * mask_guard_force / the compositor decision -- g_compositor picks which
//     WM_CLOSE branch an already-armed guard takes, and flipping it mid-session
//     could strand a pending refusal in the wrong semantics. It is an
//     environment fact, decided once at startup.
//   * the watcher thread -- maskguard_start is NOT idempotent (a re-call would
//     spawn a duplicate watcher), so a reload never starts or stops it. That
//     means mask_guard=0 at STARTUP stays restart-bound: there is no watcher to
//     wake up.
void maskguard_reload(const wchar_t* ini)
{
    g_mode  = GetPrivateProfileIntW(L"dx", L"mask_guard",       1,    ini);
    g_grace = GetPrivateProfileIntW(L"dx", L"mask_close_grace", 2000, ini);
    if (g_grace < 0) g_grace = 0;
    logf("[reload] maskguard: mask_guard=%d mask_close_grace=%dms (compositor "
         "decision and watcher thread untouched)", g_mode, g_grace);
}

int maskguard_enabled() { return g_mode; }

// Is this HWND the Viewer's mask? By identity first, then by exact class name so
// the answer is right even in the few milliseconds before the watcher arms it.
bool maskguard_is_mask(HWND h)
{
    if (!h) return false;
    if (h == g_mask) return true;
    char cls[64] = "";
    if (!GetClassNameA(h, cls, sizeof(cls))) return false;
    return strcmp(cls, "PlayOnlineMaskUS") == 0 ||
           strcmp(cls, "PlayOnlineMaskEU") == 0 ||
           strcmp(cls, "PlayOnlineMask")   == 0;
}

// Called by d3d8hook's [foc] hooks with the target of SetForegroundWindow /
// BringWindowToTop / SetActiveWindow. True = that is the mask, do not do it.
// Nothing in a windowed session is served by an invisible, click-through window
// holding the foreground: a title's own focus watchdog reads it as "I lost
// focus" and kills its input (measured: FE, polshim.314548.log:13085), and the
// keyboard goes to a wndproc that is a bare DefWindowProc.
bool maskguard_refuse_activation(HWND h, const char* api)
{
    if (!g_mode || !maskguard_is_mask(h)) return false;
    LONG n = InterlockedIncrement(&g_n_activate);
    if (n <= 5 || (n % 100) == 0)
        logf("[mask] %s(%p) REFUSED -- that is the Viewer's mask window, which never "
             "holds the foreground (#%ld)", api, h, n);
    return true;
}

// ---------------------------------------------------------------- the guard

static LRESULT CALLBACK guard_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // Captured up front: WM_NCDESTROY clears the globals below, and we still have
    // to forward that message to whoever was there before us.
    WNDPROC orig = g_orig;

    switch (msg) {
    case WM_CLOSE:
        // Our own re-post, after the watcher decided this is a shutdown. Fall
        // through to the real wndproc (a DefWindowProcA thunk) = DestroyWindow.
        if (g_allow) {
            InterlockedIncrement(&g_n_honoured);
            logf("[mask] WM_CLOSE on %s (%p) HONOURED -- the mask has been this "
                 "process's last un-owned top-level window for %d ms, so this is "
                 "the Viewer shutting down, not a compositor. Letting it through "
                 "is what posts WM_QUIT and lets pol.exe actually exit.",
                 g_cls, h, g_grace);
            break;
        }
        // Native Windows: nothing kills the mask from outside, so refusing its close
        // only strands pol.exe as a ghost. Let it through to DefWindowProc = DestroyWindow.
        if (!g_compositor) {
            static LONG said = 0;
            if (InterlockedCompareExchange(&said, 1, 0) == 0)
                logf("[mask] WM_CLOSE on %s (%p) LET THROUGH -- native Windows, no "
                     "compositor to protect the mask from; refusing here is what ghosts "
                     "the process. (mask_guard_force=1 to protect it anyway.)", g_cls, h);
            break;
        }
        InterlockedIncrement(&g_n_close);
        InterlockedExchange(&g_pending, 1);
        logf("[mask] WM_CLOSE on %s (%p) refused FOR NOW -- if the Viewer is really "
             "shutting down, the mask is about to be its last window and the watcher "
             "will honour this within %d ms. If a sibling window is still there, this "
             "came from the window manager and the refusal stands.", g_cls, h, g_grace);
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE && g_compositor) {
            InterlockedIncrement(&g_n_close);
            InterlockedExchange(&g_pending, 1);
            logf("[mask] SC_CLOSE on %s (%p) refused FOR NOW (same rule as WM_CLOSE)",
                 g_cls, h);
            return 0;
        }
        break;

    // ---- THE INVARIANT: never on top, never opaque, never activated -------------
    // (see the block comment at the top of the file). Each arm edits the message's
    // own struct and lets DefWindowProc carry on, so nothing here re-enters USER32.

    case WM_WINDOWPOSCHANGING:
        if (g_mode && lp) {
            WINDOWPOS* wpos = (WINDOWPOS*)lp;
            bool fixed = false;
            if (!(wpos->flags & SWP_NOZORDER)) {
                // HWND_TOPMOST is the obvious case. The subtle one: inserting a
                // window after a REAL window that is itself topmost makes it topmost
                // too (USER32 sets the bit implicitly), and that is a plain HWND
                // here, not a sentinel.
                HWND ia = wpos->hwndInsertAfter;
                bool topmost_ins = (ia == HWND_TOPMOST) ||
                    (ia != HWND_NOTOPMOST && ia != HWND_TOP && ia != HWND_BOTTOM &&
                     ia != NULL && IsWindow(ia) &&
                     (GetWindowLongA(ia, GWL_EXSTYLE) & WS_EX_TOPMOST));
                if (topmost_ins) {
                    wpos->hwndInsertAfter = HWND_NOTOPMOST;
                    fixed = true;
                }
            }
            if (!(wpos->flags & SWP_NOACTIVATE)) {
                wpos->flags |= SWP_NOACTIVATE;
                fixed = true;
            }
            if (fixed) {
                LONG n = InterlockedIncrement(&g_n_zorder);
                if (n <= 5 || (n % 100) == 0)
                    logf("[mask] SetWindowPos on %s rewritten: not topmost, not activating "
                         "(pos=(%d,%d %dx%d) flags=%08X, #%ld)", g_cls,
                         wpos->x, wpos->y, wpos->cx, wpos->cy, (unsigned)wpos->flags, n);
            }
        }
        break;

    case WM_WINDOWPOSCHANGED:
        // THE CATCH-ALL. Measured live 2026-09-07 (polshim.324716.log): the mask was
        // shown for FE with ex=..A0 and for FMO with ex=..A8 -- TOPMOST had come
        // back in between with NO rewrite logged above. SWP_NOSENDCHANGING skips
        // WM_WINDOWPOSCHANGING entirely, and an insert-after-a-topmost-window sets
        // the bit implicitly; WM_WINDOWPOSCHANGED is sent for both. So the bit is
        // re-checked AFTER every move, and put back down if it is there. ASYNC:
        // this is the window's own thread, and a synchronous SetWindowPos from
        // inside WINDOWPOSCHANGED re-enters USER32 mid-operation.
        if (g_mode && (GetWindowLongA(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            LONG n = InterlockedIncrement(&g_n_zorder);
            SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            if (n <= 5 || (n % 100) == 0)
                logf("[mask] %s came out of a move TOPMOST (a route WINDOWPOSCHANGING never "
                     "saw) -- putting it back down (#%ld)", g_cls, n);
        }
        if (g_mode && lp && (((WINDOWPOS*)lp)->flags & SWP_SHOWWINDOW)) {
            WINDOWPOS* wpos = (WINDOWPOS*)lp;
            // The Viewer just showed the mask (title launch). A fresh
            // SetLayeredWindowAttributes here costs nothing and guarantees the
            // alpha survived whatever the show did.
            SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
            LONG n = InterlockedIncrement(&g_n_shown);
            RECT r; ZeroMemory(&r, sizeof(r)); GetWindowRect(h, &r);
            if (n <= 5 || (n % 50) == 0)
                logf("[mask] the Viewer SHOWED %s (%ld,%ld %ldx%ld) ex=%08lX -- alpha 0, "
                     "click-through, tool window, not topmost: it covers nothing (#%ld)",
                     g_cls, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     GetWindowLongA(h, GWL_EXSTYLE), n);
        }
        break;

    case WM_STYLECHANGING:
        if (g_mode && (int)wp == GWL_EXSTYLE && lp) {
            STYLESTRUCT* ss = (STYLESTRUCT*)lp;
            LONG want = (ss->styleNew | MASK_EX_ADD) & ~MASK_EX_DROP;
            if (want != (LONG)ss->styleNew) {
                LONG n = InterlockedIncrement(&g_n_style);
                if (n <= 5 || (n % 100) == 0)
                    logf("[mask] ex-style write on %s corrected %08lX -> %08lX (#%ld)",
                         g_cls, (LONG)ss->styleNew, want, n);
                ss->styleNew = (DWORD)want;
            }
        }
        break;

    case WM_STYLECHANGED:
        // Any ex-style rewrite may have re-created the layered state; alpha 0 again.
        if (g_mode && (int)wp == GWL_EXSTYLE) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
        break;

    case WM_MOUSEACTIVATE:
        if (g_mode) {
            InterlockedIncrement(&g_n_activate);
            return MA_NOACTIVATE;
        }
        break;

    case WM_NCDESTROY:
        if (g_allow) {
            // The death we asked for. Not a failure, and deliberately NOT counted
            // in g_n_gone -- that counter drives the "the kill did not come through
            // WM_CLOSE, try mask_guard=2" advice in the summary, which would be
            // actively misleading advice for an ordinary exit.
            logf("[mask] %s (%p) destroyed as intended -- shutdown close honoured",
                 g_cls, h);
        } else {
            // We could not stop it, or the Viewer destroyed it itself on the way
            // out. Say so loudly and let the watcher re-arm on the next one: this
            // line is the single most valuable thing in the log for the Deck
            // diagnosis, since it timestamps the death against everything else the
            // run logged.
            InterlockedIncrement(&g_n_gone);
            logf("[mask] %s (%p) is being DESTROYED -- the guard could not prevent "
                 "this (a direct DestroyWindow does not go through WM_CLOSE). "
                 "Whatever the Viewer logs NEXT is the consequence.", g_cls, h);
        }
        g_mask = NULL;
        g_orig = NULL;
        InterlockedExchange(&g_pending, 0);
        InterlockedExchange(&g_allow, 0);
        break;
    }

    return orig ? CallWindowProcA(orig, h, msg, wp, lp)
                : DefWindowProcA(h, msg, wp, lp);
}

// ---------------------------------------------------------------- find + arm

struct FindMask { DWORD pid; HWND found; char cls[32]; };

static BOOL CALLBACK pick_mask(HWND h, LPARAM lp)
{
    FindMask* f = (FindMask*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid) return TRUE;

    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    // EXACT match on the three region-suffixed literals, never a substring. This
    // file restyles what it matches, and a loose match would restyle the game
    // window -- the failure mode d3d8hook's mask block warns about in capitals.
    if (strcmp(cls, "PlayOnlineMaskUS") != 0 &&
        strcmp(cls, "PlayOnlineMaskEU") != 0 &&
        strcmp(cls, "PlayOnlineMask")   != 0)
        return TRUE;

    f->found = h;
    strncpy_s(f->cls, sizeof(f->cls), cls, _TRUNCATE);
    return FALSE;                       // first match wins; stop enumerating
}

static void arm(HWND h, const char* cls)
{
    RECT r;
    ZeroMemory(&r, sizeof(r));
    GetWindowRect(h, &r);
    LONG st = GetWindowLongA(h, GWL_STYLE), ex = GetWindowLongA(h, GWL_EXSTYLE);
    logf("[mask] %s appeared: hwnd=%p (%ld,%ld %ldx%ld) style=%08lX ex=%08lX visible=%d",
         cls, h, r.left, r.top, r.right - r.left, r.bottom - r.top, st, ex,
         IsWindowVisible(h) ? 1 : 0);

    // Subclass BEFORE touching anything else. A restyle is exactly the kind of
    // change that makes a compositor re-evaluate a window, so the protection has
    // to already be in place when it does.
    //
    // There is a one-message race here -- guard_proc goes live the instant this
    // returns, and g_orig is assigned a moment later -- and it is harmless
    // BECAUSE of what this window is: the class's real wndproc is a bare
    // DefWindowProcA thunk, which is precisely what guard_proc falls back to.
    SetLastError(0);
    WNDPROC prev = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(h, GWLP_WNDPROC,
                                                       (LONG_PTR)guard_proc);
    if (!prev && GetLastError() != 0) {
        logf("[mask] could not subclass %p (err=%lu) -- NOT guarded", h, GetLastError());
        return;
    }
    g_orig = prev;
    g_mask = h;
    strncpy_s(g_cls, sizeof(g_cls), cls, _TRUNCATE);
    // Per-mask state. A Viewer session can create several, and a pending close or
    // a half-run grace clock belongs to the one that is gone, not to this one.
    InterlockedExchange(&g_pending, 0);
    InterlockedExchange(&g_allow, 0);
    g_alone_armed = 0;
    InterlockedIncrement(&g_n_seen);
    logf("[mask] guarded (mask_guard=%d) -- close requests from the window manager "
         "will be refused", g_mode);

    // THE INVARIANT, from birth. The subclass above is already in place, so this
    // restyle runs through our own WM_STYLECHANGING arm and the two agree.
    keep_invisible(h);
    logf("[mask]   -> invariant applied: ex %08lX -> %08lX (LAYERED alpha 0, "
         "TRANSPARENT, TOOLWINDOW, NOACTIVATE; APPWINDOW and TOPMOST dropped), "
         "not topmost. From here on this window cannot cover the screen, appear in "
         "Alt+Tab or hold the foreground, whichever title runs and whether or not "
         "d3d_unmask ever aligns it.", ex, GetWindowLongA(h, GWL_EXSTYLE));

    if (g_mode >= 2) {
        // ASYNCWINDOWPOS *posts* rather than sends: this runs on our watcher
        // thread and the mask belongs to the Viewer's UI thread. d3d8hook learned
        // that the hard way -- a synchronous cross-thread move deadlocked inside
        // CreateDevice and the game never got a device.
        SetWindowPos(h, HWND_BOTTOM, -32000, -32000, 1, 1,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
        logf("[mask]   -> mode 2: also 1x1 off-screen. It is still VISIBLE and still "
             "has a valid HWND, so polcore's view of it is unchanged; it is simply no "
             "longer a window a one-window compositor can pick.");
    }

    // Say the interaction out loud rather than leaving two features to surprise
    // each other in a log: once a title creates a device, [dx] d3d_unmask decides
    // this window's geometry from then on (mode 2 aligns it to the game's client
    // rect and parks it behind at alpha 0). Nothing here fights that -- survival
    // and geometry are different jobs -- but it does mean a mask_guard=2 shrink
    // is a PRE-DEVICE state, not a permanent one.
    logf("[mask] note: after a successful CreateDevice, [dx] d3d_unmask owns this "
         "window's position and size (see the [d3d] mask lines)");
}

// ------------------------------------------------- is anything left besides us

// Does this process still own an un-owned top-level window OTHER than the mask?
// OWNED windows are skipped on purpose: the hung pid 363388 still had a
// `Default IME` window, but it was owned BY the mask, so it is not evidence of a
// live application -- counting it would make the shutdown case indistinguishable
// from the compositor case and the hang would stand.
//
// Visibility is deliberately NOT tested. The Viewer hides its shell behind a
// running title, and a hidden shell is still a live application; requiring
// IsWindowVisible here would mature a pending close during a title launch, which
// is precisely the Deck failure this file exists to prevent.
struct Sibling { DWORD pid; HWND mask; int found; };

static BOOL CALLBACK pick_sibling(HWND h, LPARAM lp)
{
    Sibling* s = (Sibling*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid)          return TRUE;
    if (h == s->mask)           return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;   // owned -> not a peer
    // OUR OWN WINDOWS ARE NOT EVIDENCE OF A LIVE APPLICATION.
    //
    // The letterbox backdrop (d3d8hook.cpp, [dx] d3d_borderless) is an un-owned
    // top-level window in this process, so it counted as a sibling here and the
    // mask's close could never mature -- no WM_QUIT, pol.exe never exited, and it
    // kept the PlayOnlineUS single-instance mutex, which made every SUBSEQUENT
    // launch die instantly at startup. Measured: a zombie pol.exe holding exactly
    // PlayOnlineMaskUS + PolShimBackdrop, and three consecutive launches that ran
    // their hooks and exited with "polshim summary: 0 interfaces".
    //
    // Anything the shim creates must be excluded here. A cosmetic window of ours
    // is not the Viewer still being alive.
    {
        char cls[64] = "";
        GetClassNameA(h, cls, sizeof(cls));
        if (strcmp(cls, "PolShimBackdrop") == 0) return TRUE;
    }
    s->found = 1;
    return FALSE;                              // one is enough; stop enumerating
}

static int has_sibling_window(HWND mask)
{
    Sibling s;
    s.pid = GetCurrentProcessId();
    s.mask = mask;
    s.found = 0;
    EnumWindows(pick_sibling, (LPARAM)&s);
    return s.found;
}

// A refusal is pending: decide whether it was the Viewer shutting down (honour it)
// or a window manager (keep refusing). Watcher thread only.
//
// `sib` is sampled by the CALLER on every pass, not recomputed here. It has to be:
// the Viewer destroys its shell window BEFORE it closes the mask, so by the time a
// close is pending there is nothing left to see, and a g_saw_sibling that is only
// maintained while pending can never have been set. That mistake made the very
// first build of this fix refuse forever exactly like the bug it was fixing.
static void service_pending(HWND mask, int sib)
{
    if (sib) {
        if (g_alone_armed) {
            logf("[mask] a sibling top-level window is back -- the grace clock is "
                 "CANCELLED and the refusal stands. This is the Viewer swapping "
                 "windows, not shutting down.");
            g_alone_armed = 0;
        }
        return;
    }

    if (!g_saw_sibling) {
        // Mask up, nothing else ever seen: too early in startup to call this a
        // shutdown. Never mature here.
        return;
    }

    DWORD now = GetTickCount();
    if (!g_alone_armed) {
        g_alone_armed = 1;
        g_alone_since = now;
        logf("[mask] the mask is now this process's LAST un-owned top-level window "
             "with a close pending -- honouring it in %d ms unless a sibling "
             "reappears", g_grace);
        return;
    }

    // Unsigned subtraction, so a GetTickCount wrap is harmless.
    if ((DWORD)(now - g_alone_since) < (DWORD)g_grace) return;

    InterlockedExchange(&g_allow, 1);
    InterlockedExchange(&g_pending, 0);
    g_alone_armed = 0;
    // POSTED, not DestroyWindow'd: DestroyWindow is only legal on the thread that
    // created the window, and this is not that thread. The Viewer's UI thread is
    // still pumping -- the hang IS its message loop waiting on a WM_QUIT that never
    // arrives -- so the post is delivered.
    if (!PostMessageA(mask, WM_CLOSE, 0, 0)) {
        logf("[mask] PostMessage(WM_CLOSE) to %p FAILED (err=%lu) -- the mask was "
             "not released and pol.exe will linger", mask, GetLastError());
        InterlockedExchange(&g_allow, 0);
    }
}

// THE VIEWER'S OWN SHUTDOWN, on demand.
//
// pol.exe ends by sending WM_CLOSE to its OWN mask: the mask is the last
// un-owned top-level window the process has, its destruction posts WM_QUIT, the
// message loop ends and ExitProcess runs. That is measured -- it is the whole
// reason this file exists, because the guard used to REFUSE that very message
// and leave a 138 MB zombie holding the single-instance mutex
// ([[mask-guard-blocks-exit]]).
//
// So the same message is also the honest way to ASK the Viewer to shut down,
// which is what "Exit to desktop" needs (exitprompt.cpp). It is not a kill: the
// Viewer runs its own teardown, which is what logs the session out. g_allow is
// set first so our own guard does not refuse a close we asked for.
//
// Returns false when there is no mask to close -- which is a real answer, not a
// failure to hide: it means this process has no Viewer to shut down.
bool maskguard_request_shutdown(const char* why)
{
    HWND mask = g_mask;
    if (!mask || !IsWindow(mask)) {
        logf("[mask] shutdown requested (%s) but there is no mask window to "
             "close -- the Viewer cannot be asked to exit this way", why);
        return false;
    }
    InterlockedExchange(&g_allow, 1);
    if (!PostMessageA(mask, WM_CLOSE, 0, 0)) {
        logf("[mask] shutdown requested (%s): PostMessage(WM_CLOSE) to %p FAILED "
             "(err=%lu)", why, mask, GetLastError());
        InterlockedExchange(&g_allow, 0);
        return false;
    }
    logf("[mask] shutdown requested (%s): WM_CLOSE posted to the mask %p -- this "
         "is the Viewer's OWN exit path, not a kill", why, mask);
    return true;
}

// ---------------------------------------------------------------- watcher

static DWORD WINAPI watcher(LPVOID)
{
    for (;;) {
        if (g_stop) break;

        HWND cur = g_mask;
        if (cur && !IsWindow(cur)) {
            // A death that produced no WM_NCDESTROY through our subclass. Worth
            // its own line: it means the window went away by a route that did not
            // run application code at all, which is a different finding.
            InterlockedIncrement(&g_n_gone);
            logf("[mask] guarded window %p vanished WITHOUT reaching our wndproc -- "
                 "re-arming", cur);
            g_mask = NULL;
            g_orig = NULL;
            cur = NULL;
        }

        if (!cur) {
            FindMask f;
            f.pid = GetCurrentProcessId();
            f.found = NULL;
            f.cls[0] = 0;
            // EnumWindows rather than FindWindowEx on purpose: [multi] hooks the
            // FindWindow* family, and a diagnostic that quietly runs through
            // another feature's hook is not a diagnostic.
            EnumWindows(pick_mask, (LPARAM)&f);
            if (f.found) arm(f.found, f.cls);
        }
        else if (g_grace > 0) {
            // Sampled EVERY pass, pending or not. This is the only place
            // g_saw_sibling can be established, because a shutdown has already
            // taken the shell window down by the time the mask's close arrives.
            int sib = has_sibling_window(cur);
            if (sib) g_saw_sibling = 1;
            if (g_pending) service_pending(cur, sib);
        }

        // Fast while there is nothing to guard -- the whole point is to be there
        // BEFORE the compositor is -- and slow once armed, where the poll is only
        // a safety net for a window that dies without a message. A PENDING close
        // goes fast again: the user has asked the Viewer to exit and is watching
        // the window, so the grace period is the only delay they should get, not
        // the grace period plus most of a poll interval.
        int budget = !g_mask ? 5 : (g_pending ? 10 : 50);    // x10 ms
        for (int i = 0; i < budget && !g_stop; i++) Sleep(10);
    }
    return 0;
}

void maskguard_start()
{
    if (!g_mode) { logf("[mask] mask_guard=0 -- the mask is not watched"); return; }
    g_thread = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    if (g_thread)
        logf("[mask] guard armed (mask_guard=%d) -- watching for PlayOnlineMask{,EU,US}",
             g_mode);
    else
        logf("[mask] CreateThread failed (%lu) -- the mask is NOT guarded", GetLastError());
}

void maskguard_stop()
{
    InterlockedExchange(&g_stop, 1);
    // Daemon thread, and the subclass is deliberately LEFT IN PLACE: the shim
    // never unloads (DllCanUnloadNow returns S_FALSE and PolHook lives for the
    // life of the process), and un-subclassing on the way out would race the UI
    // thread for no gain. Same rule as d3d8hook's message spy.
}

void maskguard_summary()
{
    if (!g_mode) return;
    logf("[mask] summary: armed=%ld  close requests refused=%ld  honoured=%ld  "
         "destroyed=%ld  still guarding=%p  (mask_close_grace=%d)",
         g_n_seen, g_n_close, g_n_honoured, g_n_gone, (void*)g_mask, g_grace);
    logf("[mask] invariant: shown by the Viewer=%ld  SetWindowPos rewritten=%ld  "
         "ex-style writes corrected=%ld  activations refused=%ld%s",
         g_n_shown, g_n_zorder, g_n_style, g_n_activate,
         (g_n_zorder || g_n_style || g_n_activate)
             ? "  <-- something DID try to put the mask on top or in front; the "
               "rewrites above are what stopped it"
             : "");
    // An honour happened, so the mask did NOT survive and none of the branches
    // below apply -- they all describe a mask that outlived its close requests.
    // Getting this wrong printed "the mask SURVIVED" over a perfectly ordinary
    // exit in testing, which is exactly the sort of line that sends the next
    // session chasing the wrong bug.
    if (g_n_honoured) {
        if (g_n_close > g_n_honoured)
            logf("[mask] %ld close request(s) were refused and never matured, so they "
                 "came from OUTSIDE the Viewer and the guard did its job; the final "
                 "one was the Viewer's own shutdown and was honoured. Both halves "
                 "working -- nothing here needs attention.",
                 g_n_close - g_n_honoured);
        else
            logf("[mask] the refused close(s) were the Viewer's own shutdown and were "
                 "honoured after the grace period. This is the NORMAL exit path -- "
                 "nothing here needs attention.");
    }
    else if (!g_n_seen)
        logf("[mask] NOTE: no PlayOnlineMask* window ever appeared in this process. "
             "The Viewer never created one -- this is NOT the guard failing to find "
             "it, the watcher starts before pol.exe's own windows do.");
    else if (g_n_close && !g_n_gone)
        logf("[mask] READ THIS: %ld close request(s) were refused and the mask "
             "SURVIVED. Something outside the Viewer was trying to close it -- on "
             "the Deck that is the compositor, and this is the bug this file exists "
             "for.", g_n_close);
    else if (g_n_gone)
        logf("[mask] READ THIS: the mask was destroyed %ld time(s) despite the guard, "
             "so the kill did not come through WM_CLOSE. Try mask_guard=2, and look "
             "at what the Viewer logged immediately after the [mask] DESTROYED line.",
             g_n_gone);
}
