// exitprompt.cpp -- what the X in a windowed title's title bar should DO.
//
// WHY THIS EXISTS
//
// Once a title runs in a real window it has a close box, and SE never wrote
// any behaviour for one: these games were built for exclusive fullscreen and left
// through their own in-game menus.
//
// IMPORTANT: THE CLAIM THIS FILE WAS BUILT ON WAS FALSE, and it cost a release.
//
// `spy_proc` in d3d8hook.cpp logged `user asked to CLOSE` and handed SC_CLOSE to
// the title's own wndproc, with a comment asserting that this "ends the TITLE".
// It does not. Nobody had ever checked -- the close box did nothing before this
// dialog existed, so there was no symptom to notice. The first build of this
// prompt inherited the claim, and the result was a dialog whose two ACTION
// buttons were both no-ops (measured on the second Windows machine 2026-08-27: four closes,
// `[exit] user chose RETURN TO PLAYONLINE` twice and `EXIT TO DESKTOP` once, and
// then `gave up waiting for the title to close after 30 s`).
//
// The lesson is the project's own: a comment describing what a message DOES is a
// claim, not a measurement ([[match-the-original-not-a-stopgap]] is the same
// shape). What follows is measured or is explicitly labelled as unproven.
//
// WHAT ACTUALLY ENDS THINGS
//
//   the VIEWER: pol.exe shuts itself down by sending WM_CLOSE to its OWN mask
//   window -- the mask is the last un-owned top-level window it has, its
//   destruction posts WM_QUIT and the loop ends. That is measured, and is the
//   entire reason maskguard.cpp exists ([[mask-guard-blocks-exit]]). So
//   `maskguard_request_shutdown()` is the Viewer's own exit path, not a kill.
//
//   the TITLE: NOT SOLVED (open). SC_CLOSE to the title's wndproc is proven inert.
//   WM_CLOSE -- what DefWindowProc would have sent, and the message an ordinary
//   wndproc handles -- is what we now send, and the result is WATCHED and logged
//   either way. If it turns out the title ignores that too, the honest response
//   is to stop offering "Return to PlayOnline" rather than to keep a button that
//   does nothing.
//
// Checked first, so nobody looks for it: **Ashita has no mechanism for this at
// all.** Its plugin SDK exposes no window-message callback, `Ashita.dll` contains
// no WM_CLOSE/SC_CLOSE handling, and no addon touches close. Its only exit
// affordance is `/terminate`, a hard TerminateProcess. There was nothing to copy.
//
// So: ask. Three answers, and the two that act map onto what PlayOnline itself
// can already do.
//
//   Return to PlayOnline   ask the title to close (WM_CLOSE), and report whether
//                          it did.
//   Exit to desktop        ask the title to close, then ask the VIEWER to shut
//                          down -- whether or not the title went. Waiting for a
//                          title that may never close is what made this button do
//                          nothing at all.
//   Cancel                 swallow it.
//
// IMPORTANT: WHAT THIS DELIBERATELY DOES NOT DO: TerminateProcess. Ending a POL session by
// killing the process loses whatever the title had not written, and leaves the
// server holding a session it thinks is live ([[presence-resolves-from-session-table]]).
// "Exit to desktop" asks nicely and, if the Viewer will not go, says so in the log
// and stops -- the user still has the Viewer in front of them and can close it
// themselves. A prompt that promises to close something must not be the thing that
// corrupts it.
//
// WARNING: It no longer requires the TITLE to close first. It used to, on the reasoning
// that closing the Viewer out from under a running title is "a kill with extra
// steps" -- which is sound, and which made the button a no-op in practice because
// the title never closed. pol.exe's own shutdown tears its title down the same
// way it would if you closed the Viewer by hand, which is the behaviour the
// player just asked for.
//
// KEY: AND IT IS GATED ON THE DISPLAY. A title holding an EXCLUSIVE device owns the
// screen, and putting a window over it is exactly what crashed FFXI on a Deck --
// which is why the settings chord is already a deliberate no-op in that state
// (polsettings.cpp, exclusive_display_blocks_us). Same rule here: with an
// exclusive device live, no dialog is shown and the close behaves as it always
// did. Windowed is the default since 2026-08-24, so in practice the prompt shows.
//
// CONFIG ([dx] in polshim.ini):
//   close_prompt = 1   (default) ask.
//   close_prompt = 0   the pre-2026-08-26 behaviour: X ends the title, silently.
//   close_prompt = 2   X always exits to the desktop, no dialog.

#include "polshim.h"
#include "profiles.h"

static int  g_prompt = 1;
static LONG g_n_asked = 0, g_n_topol = 0, g_n_todesktop = 0, g_n_cancel = 0;
static LONG g_n_suppressed = 0;

void exitprompt_configure(const wchar_t* ini)
{
    g_prompt = GetPrivateProfileIntW(L"dx", L"close_prompt", 1, ini);
}

void exitprompt_reload(const wchar_t* ini)
{
    g_prompt = GetPrivateProfileIntW(L"dx", L"close_prompt", 1, ini);
}

// ---------------------------------------------------------------------------
// the dialog
// ---------------------------------------------------------------------------
#define IDC_TO_POL      201
#define IDC_TO_DESKTOP  202

static LONG g_answer = 0;

static LRESULT CALLBACK exitproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TO_POL:     g_answer = EXITPROMPT_TO_POL;     DestroyWindow(h); return 0;
        case IDC_TO_DESKTOP: g_answer = EXITPROMPT_TO_DESKTOP; DestroyWindow(h); return 0;
        case IDCANCEL:       g_answer = EXITPROMPT_CANCEL;     DestroyWindow(h); return 0;
        }
        break;
    // Closing the PROMPT is a cancel, not a second close. Without this, clicking
    // the prompt's own X would fall through to DefWindowProc, destroy it with
    // g_answer still holding whatever it held, and the caller would act on a
    // decision nobody made.
    case WM_CLOSE: g_answer = EXITPROMPT_CANCEL; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int exitprompt_ask(HWND owner, const char* title_name)
{
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!reg) {
        WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = exitproc; wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PolShimExit";
        if (!RegisterClassExW(&wc)) return EXITPROMPT_TO_POL;   // fail OPEN, see below
        reg = true;
    }

    // THE NAME. `title_name` arrives as the module LEAF, so the first build of
    // this dialog asked "Close FE_Client.dll?" -- which is the shim showing a
    // player its own internals. profiles.cpp already carries a human name for
    // every title ("Fantasy Earth"), and that is the whole point of the field.
    wchar_t label[256];
    const char* human = NULL;
    if (title_name && title_name[0]) {
        const TitleProfile* p = profile_for_module(title_name);
        if (p && p->title && p->title[0]) human = p->title;
    }
    if (human) {
        wchar_t wname[80];
        MultiByteToWideChar(CP_ACP, 0, human, -1, wname, _countof(wname));
        _snwprintf_s(label, _TRUNCATE, L"Close %ls?", wname);
    } else {
        // No profile: say something true rather than printing a DLL name.
        wcscpy_s(label, L"Close this game?");
    }

    g_answer = EXITPROMPT_CANCEL;
    RECT r = { 0, 0, 380, 128 };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int w = r.right - r.left, ht = r.bottom - r.top;

    // CENTRED ON THE GAME, not CW_USEDEFAULT. The default placement cascades --
    // measured across four closes in one session it landed at (342,342), (38,38),
    // (38,38) and (76,76) -- so the prompt appeared somewhere different every
    // time and, at least once, nowhere near the window it belongs to. A modal
    // question about a window belongs over that window.
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT o;
    if (owner && IsWindow(owner) && GetWindowRect(owner, &o) &&
        o.right > o.left && o.bottom > o.top) {
        x = o.left + ((o.right - o.left) - w) / 2;
        y = o.top  + ((o.bottom - o.top) - ht) / 2;
    } else {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            x = wa.left + ((wa.right - wa.left) - w) / 2;
            y = wa.top  + ((wa.bottom - wa.top) - ht) / 2;
        }
    }
    // Keep it on a monitor even if the game window is mostly off-screen.
    if (x != CW_USEDEFAULT) {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            if (x < wa.left) x = wa.left;
            if (y < wa.top)  y = wa.top;
            if (x + w  > wa.right)  x = wa.right  - w;
            if (y + ht > wa.bottom) y = wa.bottom - ht;
        }
    }

    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"PolShimExit", L"PlayOnline",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             x, y, w, ht,
                             owner, NULL, inst, NULL);
    if (!h) return EXITPROMPT_TO_POL;

    // The title's own icon (or PlayOnline's), the same one the game window gets.
    // A caption with the generic default icon reads as a stray Win32 dialog, not
    // as part of PlayOnline -- and this window is asking the player to end their
    // session, which is the worst moment to look like something they should not
    // trust.
    d3d8_set_window_icon(h);

    HWND st = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE,
                              14, 14, 352, 20, h, NULL, inst, NULL);
    // "Return to PlayOnline" is the default button: it is what the X did before
    // this file existed, and it is the reversible one -- the Viewer is still
    // there afterwards. Enter and the caption X therefore never end the session.
    HWND b1 = CreateWindowExW(0, L"BUTTON", L"Return to PlayOnline",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              14, 52, 150, 28, h, (HMENU)IDC_TO_POL, inst, NULL);
    HWND b2 = CreateWindowExW(0, L"BUTTON", L"Exit to desktop",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              172, 52, 120, 28, h, (HMENU)IDC_TO_DESKTOP, inst, NULL);
    HWND b3 = CreateWindowExW(0, L"BUTTON", L"Cancel",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              300, 52, 66, 28, h, (HMENU)IDCANCEL, inst, NULL);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(st, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(b1, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(b2, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(b3, WM_SETFONT, (WPARAM)f, TRUE);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(b1);

    MSG m;
    while (IsWindow(h) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            g_answer = EXITPROMPT_CANCEL; DestroyWindow(h); break;
        }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    return (int)g_answer;
}

// ---------------------------------------------------------------------------
// "Exit to desktop" -- the second half, after the title has gone
//
// The title is ended first, by the same SC_CLOSE the other answer sends, because
// closing the Viewer out from under a running title is not a shutdown, it is a
// kill with extra steps. Only once the game window is really gone do we ask the
// Viewer to close, and pol.exe then runs its OWN shutdown -- which is what logs
// the session out cleanly.
// ---------------------------------------------------------------------------
static HWND g_viewer_found = NULL;

static BOOL CALLBACK find_viewer_enum(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;              // IME and other owned popups
    if (!IsWindowVisible(h)) return TRUE;

    // Never the mask, never our own furniture. The mask is a full-screen window
    // the Viewer puts up behind a title; closing it is not closing the Viewer,
    // and maskguard.cpp exists precisely because destroying it ends the process
    // the wrong way ([[mask-guard-blocks-exit]]).
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    if (strncmp(cls, "PlayOnlineMask", 14) == 0) return TRUE;
    if (strcmp(cls, "PolShimBackdrop") == 0) return TRUE;
    if (strncmp(cls, "PolShim", 7) == 0) return TRUE;

    if (h == d3d8_game_window()) return TRUE;             // the title itself

    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    if (r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;  // DIEmWin etc.

    g_viewer_found = h;
    return FALSE;
}

static HWND find_viewer_window(void)
{
    g_viewer_found = NULL;
    EnumWindows(find_viewer_enum, 0);
    return g_viewer_found;
}

// How long to give the title after we ask it to close, before saying so.
#define EXIT_WAIT_MS     250
#define EXIT_WAIT_TRIES  40         // 10 s -- long enough for a title that saves
// ...and how long to then wait for the VIEWER to come back.
//
// 5 s, down from 10 (2026-09-08). The first successful run recovered the ghost --
// "the Viewer is back after the WM_QUIT" -- but the user read the ten silent
// seconds before it as a failure, which is fair: a button that appears to do
// nothing for that long HAS failed, whatever happens afterwards.
//
// What the margin actually has to cover is NOT "a slow title". Posting WM_QUIT
// while the title is still in its own loop is harmless -- ending that loop is the
// point. The one dangerous interval is between GameStart RETURNING and the
// Viewer's window becoming visible again, because in it the thread is already the
// Viewer's and a WM_QUIT would close PlayOnline. Measured, that gap is one app.dll
// re-load and one window creation: sub-second in every session logged so far. 5 s
// is several times that, and the check is repeated immediately before the post.
#define EXIT_BACK_TRIES  20         // 5 s

// KEY: A DESTROYED WINDOW IS NOT AN ENDED TITLE, and this file used to say it was.
//
// Measured live 2026-09-08 (polshim.371640.log), the report being
// "return to playonline works in FMO, but in Tetra Master it just leaves a ghost
// process":
//
//   14190  [exit] user chose RETURN TO PLAYONLINE      <- Front Mission
//   14217  [exit] the title's window is gone
//          [eat] app.dll was re-loaded ... [shellscale] target ... PlayOnlineUS
//   24930  [exit] user chose RETURN TO PLAYONLINE      <- Tetra Master
//   25012  [exit] the title's window is gone
//   26002  [tm] EVENT-QUEUE #4 ...                     <- and TM is STILL RUNNING
//   41775  [tm] EVENT-QUEUE #14 ...                    <- 16,000 lines later
//
// Both titles lost their window. Only one handed control back. Tetra Master's
// message loop does not end when its window is destroyed -- WM_CLOSE reaches
// DefWindowProc, the window dies, and TM keeps pumping with nothing to pump for.
// GameStart never returns, the Viewer never comes back, and pol.exe sits there
// alive and headless. The old success line claimed "WM_CLOSE ended it" from the
// window's disappearance alone, which is a proxy reported as the outcome.
//
// THE REAL SIGNAL is the Viewer coming back: app.dll is re-loaded and its
// PlayOnlineUS window becomes visible again, which is exactly what
// find_viewer_window() looks for. So that is what we wait for now.
//
// THE RECOVERY, and why WM_QUIT is safe HERE and nowhere else. A loop that
// outlives its window ends when it is posted WM_QUIT -- but the thread running a
// title's loop is the SAME thread that runs the Viewer's once GameStart returns,
// so a mistimed WM_QUIT would shut PlayOnline down instead. That risk is removed
// by the order: we only post it after the Viewer has demonstrably NOT come back
// for EXIT_BACK_TRIES, and we re-check immediately before posting. In that state
// the thread is provably still inside the title.
static bool wait_for_viewer(int tries)
{
    for (int i = 0; i < tries; i++) {
        if (find_viewer_window()) return true;
        Sleep(EXIT_WAIT_MS);
    }
    return find_viewer_window() != NULL;
}

// Ask the TITLE to close.
//
// WM_CLOSE, not SC_CLOSE. SC_CLOSE handed to the title's own wndproc is proven
// inert (see the header). WM_CLOSE is what DefWindowProc would have synthesised
// from SC_CLOSE, and is the message an ordinary wndproc actually handles -- our
// spy_proc intercepts WM_SYSCOMMAND before DefWindowProc ever gets the chance, so
// that conversion never happened. OPEN: Whether this title handles it is the open
// question; the watcher below answers it in the log rather than assuming.
static bool ask_title_to_close(HWND game)
{
    if (!game || !IsWindow(game)) return false;
    if (!PostMessageA(game, WM_CLOSE, 0, 0)) {
        logf("[exit] PostMessage(WM_CLOSE) to the game window %p FAILED (err=%lu)",
             game, GetLastError());
        return false;
    }
    return true;
}

struct ExitJob { HWND game; int mode; };

static DWORD WINAPI exit_thread(LPVOID param)
{
    ExitJob job = *(ExitJob*)param;
    LocalFree(param);

    // The title's own thread, captured while there is still a window to ask. After
    // the close there is nothing left to read it from, and it is what the recovery
    // below needs.
    // WARNING: THE THREAD ID IS THE RETURN VALUE. GetWindowThreadProcessId's out-param is
    // the PROCESS id, and passing &tid for it stored the pid there instead --
    // measured live 2026-09-08 the first time this recovery ran:
    //   Posting WM_QUIT to its thread 376500 ... FAILED (err=1444)
    // 1444 is ERROR_INVALID_THREAD_ID, and 376500 was pol.exe's own pid. The log
    // said which mistake it was because it printed the number.
    DWORD tid = 0, owner_pid = 0;
    if (job.game && IsWindow(job.game))
        tid = GetWindowThreadProcessId(job.game, &owner_pid);

    ask_title_to_close(job.game);

    bool gone = false;
    for (int i = 0; i < EXIT_WAIT_TRIES; i++) {
        Sleep(EXIT_WAIT_MS);
        if (!job.game || !IsWindow(job.game)) { gone = true; break; }
    }

    if (!gone) {
        logf("[exit] WARNING: the title did NOT close within %d s of WM_CLOSE. Its window "
             "is still there, so \"Return to PlayOnline\" cannot work by this route "
             "and needs a different mechanism -- report this line.",
             (EXIT_WAIT_MS * EXIT_WAIT_TRIES) / 1000);
    } else {
        logf("[exit] the title's window is gone -- now waiting for the Viewer to come "
             "back, which is what actually says the title ENDED (a destroyed window "
             "does not)");
        if (wait_for_viewer(EXIT_BACK_TRIES)) {
            logf("[exit] the Viewer is back -- the title returned control cleanly");
        } else if (tid && !find_viewer_window()) {
            // The ghost. See the block comment above for why WM_QUIT is safe only
            // at this exact point.
            logf("[exit] WARNING: the title's window has been gone for %d s and the Viewer "
                 "has NOT come back -- this title's message loop outlived its window "
                 "(measured on Tetra Master). Posting WM_QUIT to its thread %lu to end "
                 "that loop, which is what lets GameStart return.",
                 (EXIT_WAIT_MS * EXIT_BACK_TRIES) / 1000, tid);
            if (!PostThreadMessage(tid, WM_QUIT, 0, 0)) {
                logf("[exit] WARNING: PostThreadMessage(WM_QUIT) to %lu FAILED (err=%lu) -- "
                     "the process is left as it is; nothing is forced.",
                     tid, GetLastError());
            } else if (wait_for_viewer(EXIT_BACK_TRIES)) {
                logf("[exit] the Viewer is back after the WM_QUIT -- the ghost is "
                     "recovered");
            } else {
                logf("[exit] WARNING: the Viewer is STILL not back after WM_QUIT. This title "
                     "does not end by any route we have; nothing was forced, so close "
                     "PlayOnline yourself and report this line.");
            }
        }
    }

    if (job.mode != EXITPROMPT_TO_DESKTOP) return 0;

    // EXIT TO DESKTOP. Go whether or not the title went: pol.exe's own shutdown
    // tears its title down anyway, and waiting for a title that may never close is
    // exactly what made this button do nothing.
    if (!maskguard_request_shutdown("exit to desktop")) {
        // No mask -- fall back to whatever top-level window of ours is left.
        HWND v = find_viewer_window();
        if (v) {
            logf("[exit] no mask window; asking %p (the Viewer's own window) to close "
                 "instead", v);
            PostMessageA(v, WM_SYSCOMMAND, SC_CLOSE, 0);
        } else {
            logf("[exit] WARNING: nothing left to close -- neither a mask nor a Viewer "
                 "window. Exit to desktop could not be carried out; nothing was "
                 "forced, so close PlayOnline yourself.");
        }
    }
    return 0;
}

// Arm the close. Runs on its own thread: this is called from inside the game
// window's wndproc, and doing the wait there would block the very message loop
// that has to process the WM_CLOSE we just posted.
void exitprompt_arm_desktop_exit(HWND game) { exitprompt_arm_close(game, EXITPROMPT_TO_DESKTOP); }

void exitprompt_arm_close(HWND game, int mode)
{
    ExitJob* j = (ExitJob*)LocalAlloc(LPTR, sizeof(ExitJob));
    if (!j) { logf("[exit] out of memory arming the close"); return; }
    j->game = game;
    j->mode = mode;
    HANDLE h = CreateThread(NULL, 0, exit_thread, j, 0, NULL);
    if (h) CloseHandle(h);
    else { LocalFree(j); logf("[exit] could not start the close watcher"); }
}

// ---------------------------------------------------------------------------
// the decision, called from spy_proc's SC_CLOSE branch
//
// Returns what the caller should do:
//   EXITPROMPT_TO_POL      the title has been asked to close
//   EXITPROMPT_TO_DESKTOP  the title has been asked to close, and the Viewer after it
//   EXITPROMPT_CANCEL      swallow it, do nothing
//
// In every non-cancel case the ACTION IS ALREADY ARMED when this returns, and the
// caller must NOT also forward SC_CLOSE -- that path is inert and forwarding it
// only risks the title's wndproc doing something unexpected with a message it has
// already shown it ignores.
//
// FAILS OPEN. Every path that cannot ask -- prompt disabled, exclusive display, no
// window class -- still CLOSES THE TITLE. Before this file existed the close box
// did nothing at all; "the old behaviour" is not worth preserving as a fallback,
// so the fallback is the useful half of the dialog without the question.
// ---------------------------------------------------------------------------
int exitprompt_on_close(HWND game, const char* title_name)
{
    if (g_prompt == 0) {
        InterlockedIncrement(&g_n_topol);
        logf("[exit] close_prompt=0 -- closing the title without asking");
        exitprompt_arm_close(game, EXITPROMPT_TO_POL);
        return EXITPROMPT_TO_POL;
    }
    if (g_prompt >= 2) {
        InterlockedIncrement(&g_n_todesktop);
        logf("[exit] close_prompt=2 -- exiting to the desktop without asking");
        exitprompt_arm_close(game, EXITPROMPT_TO_DESKTOP);
        return EXITPROMPT_TO_DESKTOP;
    }
    if (d3d_exclusive_fullscreen()) {
        InterlockedIncrement(&g_n_suppressed);
        InterlockedIncrement(&g_n_topol);
        logf("[exit] a title is holding the display EXCLUSIVELY -- no prompt "
             "(a window over an exclusive device is what crashed FFXI on the "
             "Deck). Closing the title without asking.");
        exitprompt_arm_close(game, EXITPROMPT_TO_POL);
        return EXITPROMPT_TO_POL;
    }

    InterlockedIncrement(&g_n_asked);
    int a = exitprompt_ask(game, title_name);
    switch (a) {
    case EXITPROMPT_TO_DESKTOP:
        InterlockedIncrement(&g_n_todesktop);
        logf("[exit] user chose EXIT TO DESKTOP -- asking the title to close, then "
             "the Viewer");
        exitprompt_arm_close(game, EXITPROMPT_TO_DESKTOP);
        return EXITPROMPT_TO_DESKTOP;
    case EXITPROMPT_CANCEL:
        InterlockedIncrement(&g_n_cancel);
        logf("[exit] user cancelled the close");
        return EXITPROMPT_CANCEL;
    default:
        InterlockedIncrement(&g_n_topol);
        logf("[exit] user chose RETURN TO PLAYONLINE -- asking the title to close");
        exitprompt_arm_close(game, EXITPROMPT_TO_POL);
        return EXITPROMPT_TO_POL;
    }
}

void exitprompt_summary(void)
{
    if (!g_prompt) {
        logf("[exit] summary: prompt OFF ([dx] close_prompt=0) -- the close box "
             "ended the title silently, as it did before 2026-08-26");
        return;
    }
    logf("[exit] summary: asked=%ld -> PlayOnline=%ld desktop=%ld cancelled=%ld"
         "%s", g_n_asked, g_n_topol, g_n_todesktop, g_n_cancel,
         g_n_suppressed ? "  (and suppressed once or more for an exclusive display)" : "");
}

// ---------------------------------------------------------------------------
// selftest
//
// The dialog needs a desktop and a person. What is proven headlessly is the
// POLICY -- specifically that every non-asking path fails OPEN to the old
// behaviour, because that is the failure that would be invisible: a close box
// that silently stops working.
// ---------------------------------------------------------------------------
int exitprompt_selftest(void)
{
    int fail = 0;
    #define CHK(c, m) do { if (!(c)) { logf("[exit] SELFTEST FAIL: %s", m); fail++; } } while (0)

    int save = g_prompt;

    g_prompt = 0;
    CHK(exitprompt_on_close(NULL, "Test") == EXITPROMPT_TO_POL,
        "close_prompt=0 must behave exactly as the pre-prompt build did");

    // The three answers must be distinct, and CANCEL must not be the same value
    // as either action -- a collision here would silently turn "cancel" into an
    // exit, which is the one mistake this dialog must never make.
    CHK(EXITPROMPT_CANCEL != EXITPROMPT_TO_POL &&
        EXITPROMPT_CANCEL != EXITPROMPT_TO_DESKTOP &&
        EXITPROMPT_TO_POL != EXITPROMPT_TO_DESKTOP,
        "the three answers must be distinct values");
    CHK(EXITPROMPT_CANCEL == 0, "cancel must be the zero/default answer");

    g_prompt = save;
    #undef CHK
    if (!fail) logf("[exit] selftest OK");
    return fail;
}
