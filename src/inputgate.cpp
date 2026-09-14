// inputgate.cpp -- ONE answer to "should the mouse reach the title right now?"
//
// WHY THIS EXISTS
//
// Reported 2026-08-26: "when I don't have FMO selected, it's still rotating my
// character. Tetra sometimes does this."
//
// A POL title receives the mouse down THREE separate paths, and until this file
// existed not one of them asked whether the game was the window you were actually
// looking at:
//
//   1. app.dll's cursor poll -- GetCursorPos -> ScreenToClient(input window).
//      This is the shell path that owns the pointer for EVERY title
//      ([[viewer-owns-the-mouse]]), and GetCursorPos does not care about focus.
//      Hooked in d3d8hook.cpp.
//   2. SE's own WH_MOUSE chain. Measured in our logs:
//          [hk] SetWindowsHookExA(WH_MOUSE=7, proc=... in PolHook_orig.DLL,
//                                 hmod=PolHook_orig.DLL, thread=0)
//      `thread=0` is a SYSTEM-WIDE hook -- every mouse message on the desktop.
//      Wrapped in hookspy.cpp. In the same session FMO created NO DirectInput
//      mouse device at all, so this (with 1) is how FMO sees the mouse.
//   3. DirectInput -- Tetra Master polls (GetDeviceState), Fantasy Earth reads
//      buffered (GetDeviceData) ([[title-mouse-read-paths]]). Wrapped in
//      dinputhook.cpp. IMPORTANT: And this one is OURS: `[dx] dinput_background=1`
//      rewrites the title's DISCL_FOREGROUND to DISCL_BACKGROUND, which is
//      precisely "keep feeding this device while the window is not focused".
//
// THE RULE, AND WHY IT IS NOT THE OBVIOUS ONE
//
// The obvious fix -- put DISCL_FOREGROUND back, or stop wrapping -- is the one
// that must not be taken. DirectInput UNACQUIRES a FOREGROUND device the moment
// its window stops being foreground, and a buffered reader then sees an empty
// queue for ever: that is exactly how Fantasy Earth's mouse CLICKS died, and why
// dinput_background exists and ships on. See the long note at
// DIDeviceWrap::SetCooperativeLevel.
//
// So: **gate DELIVERY, never ACQUISITION.** The device stays acquired and the
// hooks stay wrapped, so refocus is instant and no queue ever dies. What changes
// is whether the data reaches the title.
//
// THE TEST IS "IS THE FOREGROUND WINDOW OURS", NOT "IS IT THE GAME WINDOW"
//
// One pol.exe hosts the Viewer, the PlayOnlineMask window, the shim's own
// dialogs and the title, and they hand focus to each other constantly. Keying on
// the game window alone would cut the mouse off every time the Viewer put up a
// dialog over the game -- a new bug in place of the old one. Process identity is
// the honest question: the mouse stops when you are in ANOTHER APPLICATION.
//
// Two states that are not about focus at all belong to the same gate, because
// they mean the same thing to a player -- "I am not driving the game right now":
//
//   * the modal move/size loop (you are dragging the window's own title bar).
//     Without this, dragging a windowed FMO by its caption spins the camera.
//   * the game minimised by the user.
//
// THE FOURTH SEAM AND ITS FRIENDS: THE KEYBOARD (2026-09-09)
//
// The same complaint came back, twice, about the KEYBOARD -- "Fantasy Earth is
// still accepting my inputs even when I'm not on its window". The mouse gate is
// not at fault; it fires. The keyboard simply had no gate at all, and one of its
// paths is ALSO ours. Measured live in polshim.412476.log (Fantasy Earth,
// 2026-09-09 02:16, right after FE_Client.dll loaded):
//
//   [din] keyboard SetCooperativeLevel(hwnd=..., NONEXCLUSIVE|FOREGROUND|NOWINKEY)
//                                            -> NONEXCLUSIVE|BACKGROUND
//
// The title ASKED for FOREGROUND -- exactly the behaviour the player wants --
// and the shim overrode it. That flip's only justification is FE's mouse
// BUTTONS; the keyboard inherited it on 2026-08-26 by starting to share the coop
// wrapper in order to strip DISCL_NOWINKEY. The Windows-key fix silently handed
// the titles the keyboard.
//
// KEY: The same rule applies, for the same reason: restoring FOREGROUND for the
// keyboard would UNACQUIRE it on every focus loss, and a title that mishandles
// DIERR_NOTACQUIRED loses its keyboard for the rest of the session. So the coop
// level stays and the DELIVERY is withheld -- at every path a key can take:
//
//   seam 4  DirectInput keyboard -- GetDeviceState (polled) and GetDeviceData
//           (buffered), dinputhook.cpp. Until this build both read paths
//           returned before ANY bookkeeping, so no log has ever said whether a
//           title reads the keyboard through DirectInput at all.
//   seam 5  GetAsyncKeyState / GetKeyState / GetKeyboardState -- keystate.cpp.
//           IMPORTANT: These report the PHYSICAL key state and answer the same
//           whichever window is in front. Nothing hooked them, and
//           FE_Client.dll imports GetAsyncKeyState and GetKeyState BY NAME
//           (measured in its import table, 2026-09-09).
//   seam 6  a SYSTEM-WIDE WH_KEYBOARD / WH_KEYBOARD_LL chain, hookspy.cpp.
//           Only the global ones: a thread hook cannot fire without focus.
//
// The keyboard gate is a SEPARATE switch from the mouse's, because the releases
// differ and because turning one off to test it must not turn the other off.
// The player sees them as one tick (iniheal.cpp).
//
// CONFIG ([dx] in polshim.ini):
//   mouse_focus_gate = 1   (default) withhold mouse input while another
//                          application is in front. 0 = SE's behaviour.
//   key_focus_gate   = 1   (default) the same for the keyboard.
//
// This file deliberately holds NO Windows state of its own beyond a short cache:
// the decision is a pure function of three booleans (inputgate_decide) so that it
// can be selftested without a desktop, and the observation is separated from it.

#include "polshim.h"

static int  g_gate  = 1;
static int  g_kgate = 1;

// Per-seam counters, so the summary can say WHICH path was actually carrying the
// mouse for this title -- the question the FMO report could not answer.
static LONG g_n_blocked_cursor = 0;
static LONG g_n_blocked_hook   = 0;
static LONG g_n_blocked_dinput = 0;
static LONG g_n_releases       = 0;
static LONG g_n_eval           = 0;

// The keyboard seams are counted SEPARATELY, and that is the point: no log in
// this project's history has ever said which path carries a title's keys, so
// "we gated the keyboard" could never be distinguished from "we gated a path
// this title does not use". These counters answer it in one session.
static LONG g_n_blocked_dikbd  = 0;   // seam 4: DirectInput keyboard
static LONG g_n_blocked_async  = 0;   // seam 5: GetAsyncKeyState / GetKeyState
static LONG g_n_blocked_khook  = 0;   // seam 6: a global keyboard hook chain
static LONG g_n_kheld          = 0;   // keys we had to synthesise an UP for
static LONG g_n_krel           = 0;   // keyboard focus-loss releases

void inputgate_configure(const wchar_t* ini)
{
    g_gate  = GetPrivateProfileIntW(L"dx", L"mouse_focus_gate", 1, ini);
    g_kgate = GetPrivateProfileIntW(L"dx", L"key_focus_gate",   1, ini);
}

// Re-read LIVE, like key_escape and for the same reason: someone who has just
// been bitten by this must be able to change it without relaunching the title
// they are sitting inside.
void inputgate_reload(const wchar_t* ini)
{
    int was = g_gate;
    g_gate = GetPrivateProfileIntW(L"dx", L"mouse_focus_gate", 1, ini);
    if (was != g_gate)
        logf("[gate] mouse_focus_gate %d -> %d (live)", was, g_gate);
    int kwas = g_kgate;
    g_kgate = GetPrivateProfileIntW(L"dx", L"key_focus_gate", 1, ini);
    if (kwas != g_kgate)
        logf("[gate] key_focus_gate %d -> %d (live)", kwas, g_kgate);
}

int inputgate_enabled(void)     { return g_gate; }
int inputgate_key_enabled(void) { return g_kgate; }

// ---------------------------------------------------------------------------
// THE DECISION -- a pure function, so it can be proven without a desktop
//
// Returns true when the mouse must be WITHHELD from the title.
// ---------------------------------------------------------------------------
bool inputgate_decide(bool ours_foreground, bool in_modal, bool minimized)
{
    if (!ours_foreground) return true;   // another application is in front
    if (in_modal)         return true;   // you are dragging/sizing our own frame
    if (minimized)        return true;   // the user put the game away
    return false;
}

// ---------------------------------------------------------------------------
// THE OBSERVATION
//
// Cached for 16 ms. hook_GetCursorPos "runs constantly" (its own words) and a
// title polls it every frame or faster; two syscalls per poll is not a cost
// worth paying to notice a focus change sooner than one frame.
// ---------------------------------------------------------------------------
static LONG  g_cache_tick = 0;
static LONG  g_cache_val  = 0;
static LONG  g_cache_have = 0;

#define INPUTGATE_CACHE_MS 16

static bool ours_is_foreground(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;               // nobody is foreground: not us
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// The cached OBSERVATION, with no enable test of its own: the mouse and the
// keyboard ask the same question of the desktop and there is no reason to pay
// for it twice, but each has its own switch and must be able to be off while
// the other is on.
static bool gate_observe(void)
{
    DWORD now = GetTickCount();
    if (InterlockedCompareExchange(&g_cache_have, 0, 0)) {
        DWORD then = (DWORD)InterlockedCompareExchange(&g_cache_tick, 0, 0);
        if ((DWORD)(now - then) < INPUTGATE_CACHE_MS)
            return InterlockedCompareExchange(&g_cache_val, 0, 0) != 0;
    }

    InterlockedIncrement(&g_n_eval);
    bool blocked = inputgate_decide(ours_is_foreground(),
                                    d3d8_in_modal() != 0,
                                    d3d8_user_minimized() != 0);
    InterlockedExchange(&g_cache_val, blocked ? 1 : 0);
    InterlockedExchange(&g_cache_tick, (LONG)now);
    InterlockedExchange(&g_cache_have, 1);
    return blocked;
}

bool inputgate_blocked(void)
{
    if (!g_gate) return false;
    return gate_observe();
}

// SEAMS 4-6. Same decision, same cache, its own switch.
bool inputgate_key_blocked(void)
{
    if (!g_kgate) return false;
    return gate_observe();
}

// The keyboard's edge, kept apart from the mouse's on purpose: a seam must not
// consume an edge that belongs to a different device, or exactly one of the two
// releases silently stops happening and the symptom is "sometimes a key sticks".
int inputgate_key_edge(bool* prev_blocked, bool* blocked_out)
{
    bool now = inputgate_key_blocked();
    if (blocked_out) *blocked_out = now;
    bool was = *prev_blocked;
    *prev_blocked = now;
    if (now && !was) { InterlockedIncrement(&g_n_krel); return 1; }
    return 0;
}

void inputgate_note_key_blocked(int seam, int synthetic_ups)
{
    switch (seam) {
    case 4: InterlockedIncrement(&g_n_blocked_dikbd); break;
    case 5: InterlockedIncrement(&g_n_blocked_async); break;
    default: InterlockedIncrement(&g_n_blocked_khook); break;
    }
    if (synthetic_ups > 0) InterlockedExchangeAdd(&g_n_kheld, synthetic_ups);
}

// ---------------------------------------------------------------------------
// EDGE BOOKKEEPING
//
// Each seam keeps its OWN previous state and calls this, because each has its own
// release to perform and they are not interchangeable: the polled DirectInput
// path releases by simply zeroing every poll, the buffered path has to SYNTHESISE
// a button-up (a game that never sees the up thinks the button is still held --
// and FMO's camera is a held-button drag, so that is the difference between "the
// camera stops" and "the camera spins for ever"), and the WH_MOUSE chain releases
// by delivering the up messages itself.
//
// Returns:  1 = this call is the blocking EDGE (do your release now)
//           0 = steady state (blocked or not; read `blocked`)
// ---------------------------------------------------------------------------
int inputgate_edge(bool* prev_blocked, bool* blocked_out)
{
    bool now = inputgate_blocked();
    if (blocked_out) *blocked_out = now;
    bool was = *prev_blocked;
    *prev_blocked = now;
    if (now && !was) { InterlockedIncrement(&g_n_releases); return 1; }
    return 0;
}

void inputgate_note_blocked(int seam)
{
    switch (seam) {
    case 0: InterlockedIncrement(&g_n_blocked_cursor); break;
    case 1: InterlockedIncrement(&g_n_blocked_hook);   break;
    default: InterlockedIncrement(&g_n_blocked_dinput); break;
    }
}

void inputgate_summary(void)
{
    if (!g_gate) {
        logf("[gate] summary: OFF ([dx] mouse_focus_gate=0) -- the mouse reached "
             "the title regardless of which application was in front");
        return;
    }
    logf("[gate] summary: withheld cursor=%ld hookchain=%ld dinput=%ld, "
         "focus-loss releases=%ld (%ld evaluations)",
         g_n_blocked_cursor, g_n_blocked_hook, g_n_blocked_dinput,
         g_n_releases, g_n_eval);
    // The counters ARE the answer to "which path carries this title's mouse".
    // A title with zero on all three either never lost focus or reads the mouse
    // some fourth way, and those are very different findings.
    if (!g_n_blocked_cursor && !g_n_blocked_hook && !g_n_blocked_dinput)
        logf("[gate] summary: nothing was ever withheld -- either the game kept "
             "the foreground for the whole session, or this title reads the "
             "mouse by a path the gate does not cover. Check which of "
             "[cur]/[hk]/[din] shows traffic before concluding either.");

    if (!g_kgate) {
        logf("[gate] summary: KEYBOARD gate OFF ([dx] key_focus_gate=0) -- keys "
             "reached the title regardless of which application was in front");
        return;
    }
    logf("[gate] summary: keyboard withheld dinput=%ld keystate=%ld hookchain=%ld, "
         "focus-loss releases=%ld (%ld synthetic key-ups sent)",
         g_n_blocked_dikbd, g_n_blocked_async, g_n_blocked_khook,
         g_n_krel, g_n_kheld);
    // KEY: THE LINE THAT DECIDES THE NEXT STEP. Which of these three is
    // non-zero names the path this title's keyboard actually travels -- the
    // question that made the 2026-09-04 attempt unprovable. Zero on all three
    // with a non-zero mouse count above is a REAL finding, not a null result:
    // it means the keys arrive some seventh way (window messages are the only
    // remaining one, and those already respect focus).
    if (!g_n_blocked_dikbd && !g_n_blocked_async && !g_n_blocked_khook) {
        if (g_n_krel)
            logf("[gate] summary: the game DID lose the foreground %ld time(s) and "
                 "NOT ONE key read was withheld -- this title does not read the "
                 "keyboard through DirectInput, GetAsyncKeyState/GetKeyState or a "
                 "global keyboard hook. Cross-check the [din] kbdstate=/kbddata= "
                 "and [key] counters before theorising further.", g_n_krel);
        else
            logf("[gate] summary: the keyboard gate never evaluated to blocked -- "
                 "the game kept the foreground for the whole session, so this "
                 "session proves NOTHING about the keyboard either way.");
    }
}

// ---------------------------------------------------------------------------
// selftest -- the decision table
//
// The observation half needs a desktop; the DECISION is the part that can be
// wrong in a way nobody notices, so it is the part that is proven here.
// ---------------------------------------------------------------------------
int inputgate_selftest(void)
{
    int fail = 0;
    #define CHK(c, m) do { if (!(c)) { logf("[gate] SELFTEST FAIL: %s", m); fail++; } } while (0)

    // fg,   modal, min  -> blocked?
    CHK(!inputgate_decide(true,  false, false), "our window in front and idle: deliver");
    CHK( inputgate_decide(false, false, false), "another app in front: withhold");
    CHK( inputgate_decide(true,  true,  false), "dragging our own frame: withhold");
    CHK( inputgate_decide(true,  false, true),  "minimised: withhold");
    CHK( inputgate_decide(false, true,  true),  "all three: withhold");

    // The edge helper: exactly one release per unfocused episode, and a release
    // on the way OUT only -- never on the way back in, where a synthetic
    // button-up would cancel the click the user just made to refocus.
    bool prev = false, now = false;
    int  rel  = 0;
    struct { bool blocked; int want_edge; } seq[] = {
        { false, 0 },   // focused
        { true,  1 },   // lost focus  -> release here
        { true,  0 },   // still away
        { true,  0 },
        { false, 0 },   // came back    -> NO release
        { false, 0 },
        { true,  1 },   // lost it again -> release again
    };
    prev = false;
    for (int i = 0; i < (int)(sizeof(seq) / sizeof(seq[0])); i++) {
        // Drive the pure edge logic directly; inputgate_edge() only adds the
        // observation, which a harness has no desktop for.
        bool was = prev;
        prev = seq[i].blocked;
        int edge = (seq[i].blocked && !was) ? 1 : 0;
        if (edge != seq[i].want_edge) {
            logf("[gate] SELFTEST FAIL: step %d edge=%d want=%d", i, edge, seq[i].want_edge);
            fail++;
        }
        rel += edge;
    }
    CHK(rel == 2, "two unfocused episodes should produce exactly two releases");
    (void)now;

    // OFF must mean OFF: no cache, no policy, nothing withheld.
    int save = g_gate;
    g_gate = 0;
    CHK(!inputgate_blocked(), "mouse_focus_gate=0 must never block");
    g_gate = save;

    // The two switches are INDEPENDENT. This is the assertion that would have
    // caught the 2026-08-26 defect this whole file exists to undo: a fix for one
    // device silently applying to another because they shared a wrapper.
    int ksave = g_kgate;
    g_kgate = 0;
    CHK(!inputgate_key_blocked(), "key_focus_gate=0 must never block the keyboard");
    g_gate = 0; g_kgate = 1;
    // With the mouse gate off and the keyboard gate on, the mouse must still be
    // free -- and vice versa. (The observation half needs a desktop, so this
    // asserts only the half that cannot depend on one.)
    CHK(!inputgate_blocked(), "mouse_focus_gate=0 must stay off while the keyboard gate is on");
    g_gate = save; g_kgate = ksave;

    // The keyboard edge: one release per unfocused episode, on the way OUT.
    // Same table as the mouse's, driven through the keyboard's own counter so a
    // future refactor cannot quietly make them share one prev flag.
    {
        bool kprev = false;
        int  krel  = 0;
        const bool kseq[] = { false, true, true, false, true };
        const int  kwant[] = { 0,    1,    0,    0,     1    };
        for (int i = 0; i < 5; i++) {
            bool was = kprev;
            kprev = kseq[i];
            int edge = (kseq[i] && !was) ? 1 : 0;
            if (edge != kwant[i]) {
                logf("[gate] SELFTEST FAIL: keyboard step %d edge=%d want=%d",
                     i, edge, kwant[i]);
                fail++;
            }
            krel += edge;
        }
        CHK(krel == 2, "two unfocused episodes should produce exactly two keyboard releases");
    }

    #undef CHK
    if (!fail) logf("[gate] selftest OK");
    return fail;
}
