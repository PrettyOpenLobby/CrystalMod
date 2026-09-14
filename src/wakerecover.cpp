// wakerecover.cpp -- get a Direct3D title back on screen after the machine sleeps.
//
// THE REPORT. Suspend the machine (the Steam Deck's power button is the usual
// way) with a POL title running, wake it up, and the game window is black from
// then on. The process is alive, the Viewer is alive, sound may still play --
// only the picture never comes back. The same shape is expected for FMO and
// Fantasy Earth, because nothing about it is title-specific.
//
// WHY IT IS A REAL PROBLEM AND NOT A WINE BUG TO REPORT UPSTREAM
//
// Direct3D 8 and 9 (the non-Ex ones -- and every POL title is non-Ex) have an
// explicit LOST DEVICE contract. When the display goes away underneath a device,
// the runtime starts failing calls with D3DERR_DEVICELOST, and the APPLICATION is
// required to notice, release its D3DPOOL_DEFAULT resources, poll
// TestCooperativeLevel until it answers D3DERR_DEVICENOTRESET, call Reset, and
// rebuild. That contract is the application's half of the deal, and a 2002-2006
// title written for a desktop PC that never suspended frequently does not
// implement it -- it presents into a dead device for ever, which is exactly the
// black window. The runtime is behaving correctly; the title is not.
//
// So the fix belongs here, in the interposer, and it is genuinely general: it is
// not a workaround for one title's bug, it is supplying the half of a published
// contract the title omits.
//
// WHAT THIS FILE IS AND IS NOT
//
//   * It is API-AGNOSTIC. d3d8hook.cpp and d3d9hook.cpp own the ABI (the present-
//     parameter structs differ, and d3d8.h exists in no SDK), so they register a
//     device here with two callbacks -- "test it" and "reset it with the exact
//     parameters it was last created/reset with" -- and this file owns the policy.
//     That split is deliberate: the last two times a d3d9 feature was written as
//     a thinner copy of the d3d8 one they drifted apart (see the FMV-adoption
//     note in d3d8hook.cpp). One policy, two thin adapters.
//
//   * The RECOVERY RUNS ON THE TITLE'S OWN RENDER THREAD, never on the watchdog.
//     A non-Ex device is not thread-safe unless it was created with
//     D3DCREATE_MULTITHREADED, and none of these titles do. So the watchdog only
//     DETECTS and does user32 work; the D3D work is driven from inside the
//     Present / TestCooperativeLevel hooks, which are by construction the render
//     thread. [dx] wake_offthread exists to break that rule on purpose, and it is
//     a DEBUG LEVER, not a fix -- see its comment.
//
//   * IT DEFERS TO THE TITLE. If the title resets its own device during an
//     episode -- successfully or not -- we stand down for the rest of that
//     episode. A title that implements the contract must never have a second
//     Reset driven into it from outside, and a title whose OWN Reset keeps
//     failing would fail identically for us (same runtime, same outstanding
//     resources), so there is nothing to gain by trying.
//
// WHY A FORCED RESET IS SAFER THAN IT SOUNDS
//
// The obvious objection is that we would be resetting a device whose owner does
// not know it happened, leaving its resources and render state stale. The D3D8/9
// rule that makes that mostly self-limiting: Reset FAILS with D3DERR_INVALIDCALL
// while ANY D3DPOOL_DEFAULT resource is still alive. So a Reset that SUCCEEDS is
// a Reset on a device that had no default-pool resources left to lose -- managed-
// pool content is re-uploaded by the runtime automatically. A Reset that would
// have been destructive is the one the runtime refuses. That is why this can be
// on by default and still fail safe: the bad case is a logged INVALIDCALL, not a
// corrupted title. (Render STATE does come back at its defaults, so a title that
// sets state once at init and never again can come back wrong. That is the
// residual risk, it is recoverable by relaunching the title, and it only applies
// to a title that was already showing a black window.)
//
// WHAT "A WAKE" IS, MEASURED RATHER THAN NOTIFIED
//
// WM_POWERBROADCAST/PBT_APMRESUMEAUTOMATIC is the documented signal and it is not
// dependable here: under Proton the guest never necessarily hears that the HOST
// suspended. What IS dependable is that two clocks disagree afterwards. Linux's
// CLOCK_MONOTONIC (which Wine's tick count follows) does not advance across a
// suspend, while the wall clock does. So:
//
//     skew = (wall-clock delta) - (tick delta)
//
// over a 1-second watchdog tick is ~0 in normal running and ~= the suspend
// duration after a resume. The tick delta ALONE is also checked, because on
// native Windows the tick count is biased and does include sleep time -- there
// the wall/tick skew stays ~0 and the raw tick delta is the signal. Either one
// firing is a wake; the log says which, so the two environments stay tellable
// apart in a transcript.
//
// A hard freeze (a stalled render thread, a swapping machine) also produces a
// large tick delta. That is not a false positive worth engineering away: the
// response to it is the same -- look at the device, look at whether frames are
// advancing -- and the episode log distinguishes them by what it finds.
#include "polshim.h"

// The two HRESULTs this whole file is about. Same numeric values in D3D8 and D3D9
// (they are MAKE_D3DHRESULT(2072) and (2073)); spelled here rather than included
// because d3d8.h exists in no SDK and this file must not depend on d3d9.h either.
#define WAKE_DEVICELOST     ((HRESULT)0x88760868L)
#define WAKE_DEVICENOTRESET ((HRESULT)0x88760869L)
#define WAKE_INVALIDCALL    ((HRESULT)0x8876086CL)

// ---------------------------------------------------------------- configuration

static int g_enable    = 1;   // [dx] wake_enable    -- detect + log (harmless)
static int g_nudge     = 1;   // [dx] wake_nudge     -- user32 kick on the window
static int g_do_reset  = 1;   // [dx] wake_reset     -- drive the D3D Reset ourselves
static int g_grace_ms  = 3000;  // [dx] wake_grace_ms  -- the title's own chance first
static int g_gap_ms    = 15000; // [dx] wake_gap_ms    -- clock gap that counts as a wake
static int g_max_reset = 8;   // [dx] wake_max_resets -- per episode, then give up
static int g_offthread = 0;   // [dx] wake_offthread -- DEBUG LEVER, see below
static int g_cooldown  = 30000; // [dx] wake_cooldown_ms -- quiet after a failed episode
static int g_trace     = 1;   // [dx] wake_trace

// How long an episode may run before we stop expecting anything of it. Not a
// setting: it is a log-noise bound, not a behaviour.
#define WAKE_EPISODE_MAX_MS 120000
// Rate limit on the pump. The render thread calls in ~60x/s; the device state
// changes on a human scale.
#define WAKE_PUMP_EVERY_MS    200
// Frames that must land after the device tests healthy before we call it over.
#define WAKE_FRAMES_RECOVERED  30

// ---------------------------------------------------------------- device slot
//
// One slot, not a table. Both hook files already keep exactly one "the game's
// device" pointer, because a POL session runs one title at a time; a table here
// would be state nothing feeds. Re-registration replaces it (a title that
// recreates its device, or a second title in the same Viewer session).

struct WakeDev {
    void*        dev;
    WakeTestFn   test;
    WakeResetFn  reset;
    const char*  api;      // "d3d8" / "d3d9" -- a literal, never freed
    HWND         wnd;
};
static WakeDev  g_dev = { NULL, NULL, NULL, "?", NULL };
static CRITICAL_SECTION g_dev_cs;
static LONG     g_dev_cs_ready = 0;
// A lock-free mirror of g_dev.dev, for the three notification entry points.
// They sit on the title's per-frame path, and taking a critical section 60
// times a second to answer "is this the device I am watching" is a cost the
// answer does not need: the pointer is written once at device creation and read
// everywhere else.
static void* volatile g_dev_fast = NULL;

static void dev_lock_init(void)
{
    // Racy-safe one-time init without depending on a C++11 runtime the rest of
    // the shim does not use.
    if (InterlockedCompareExchange(&g_dev_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_dev_cs);
        InterlockedExchange(&g_dev_cs_ready, 2);
    }
    while (InterlockedCompareExchange(&g_dev_cs_ready, 2, 2) != 2) Sleep(0);
}

static bool dev_snapshot(WakeDev* out)
{
    if (InterlockedCompareExchange(&g_dev_cs_ready, 2, 2) != 2) return false;
    EnterCriticalSection(&g_dev_cs);
    *out = g_dev;
    LeaveCriticalSection(&g_dev_cs);
    return out->dev != NULL;
}

// ---------------------------------------------------------------- episode state

// Arm reasons, so the episode line says what actually started it.
#define WAKE_R_SUSPEND   1
#define WAKE_R_PRESENT   2
#define WAKE_R_TEST      3
static const char* arm_reason_name(LONG r)
{
    return r == WAKE_R_SUSPEND ? "the machine woke from sleep"
         : r == WAKE_R_PRESENT ? "Present returned D3DERR_DEVICELOST"
         : r == WAKE_R_TEST    ? "TestCooperativeLevel reported a lost device"
                               : "?";
}

static volatile LONG g_armed        = 0;   // an episode is in progress
static volatile LONG g_arm_reason   = 0;
static volatile LONG g_arm_tick     = 0;   // GetTickCount at arm
static volatile LONG g_busy         = 0;   // pump re-entrancy guard
static volatile LONG g_last_pump    = 0;

static volatile LONG g_frames       = 0;   // Presents seen, ever
static volatile LONG g_frames_at_arm = 0;
static volatile LONG g_title_resets = 0;   // Reset calls the TITLE made, ever
static volatile LONG g_title_resets_at_arm = 0;

// Per-episode, reset by arm().
static volatile LONG g_our_resets   = 0;
static volatile LONG g_said_lost    = 0;
static volatile LONG g_said_defer   = 0;
static volatile LONG g_said_noreset = 0;
static volatile LONG g_last_tc      = 0;
static volatile LONG g_saw_lost     = 0;   // this episode ever saw a lost device
// WHEN THE DEVICE FIRST BECAME RESETTABLE, which is where the title's grace
// period has to be measured from -- not from when the episode armed.
//
// Caught by wakerecovertest.exe scenario 2. Measuring the grace from the arm was
// wrong in a way that only shows up on a slow resume: a device can sit at
// D3DERR_DEVICELOST for longer than wake_grace_ms, and it arrives at
// D3DERR_DEVICENOTRESET with the whole grace already spent. The shim would then
// Reset on the FIRST resettable poll -- and D3DERR_DEVICENOTRESET is the very
// moment the title's own recovery becomes possible, so the title it is supposed
// to defer to would never get a single frame in which to act. A grace period
// measured from before the thing it is waiting for could begin is not a grace
// period at all.
static volatile LONG g_resettable_seen = 0;
static volatile LONG g_resettable_tick = 0;

// A device that is lost and stays lost would otherwise re-arm on the very next
// Present after an episode gave up, and every episode logs. The cooldown makes
// a permanently dead device cost one episode's worth of log instead of one per
// frame. A RESUME always arms -- that is a new event, not the same one again.
// Configurable so wakerecovertest.exe can drive several scenarios in one
// process without waiting half a minute between them.
static volatile LONG g_cooldown_until = 0;
static volatile LONG g_said_cooldown  = 0;

// Session totals for the summary.
static LONG g_n_wakes       = 0;
static LONG g_n_episodes    = 0;
static LONG g_n_our_resets  = 0;
static LONG g_n_our_reset_ok = 0;
static LONG g_n_recovered   = 0;
static LONG g_n_gave_up     = 0;
static LONG g_n_title_recovered = 0;
static LONG g_n_nudges      = 0;
static LONG g_n_lost_seen   = 0;

int wake_enabled() { return g_enable; }

// ---------------------------------------------------------------- config

void wake_configure(const wchar_t* ini)
{
    if (!ini) return;
    g_enable    = GetPrivateProfileIntW(L"dx", L"wake_enable",     1, ini);
    g_trace     = GetPrivateProfileIntW(L"dx", L"wake_trace", trace_at(1), ini);
    g_nudge     = GetPrivateProfileIntW(L"dx", L"wake_nudge",      1, ini);
    g_do_reset  = GetPrivateProfileIntW(L"dx", L"wake_reset",      1, ini);
    g_grace_ms  = GetPrivateProfileIntW(L"dx", L"wake_grace_ms",   3000, ini);
    g_gap_ms    = GetPrivateProfileIntW(L"dx", L"wake_gap_ms",     15000, ini);
    g_max_reset = GetPrivateProfileIntW(L"dx", L"wake_max_resets", 8, ini);
    g_offthread = GetPrivateProfileIntW(L"dx", L"wake_offthread",  0, ini);
    g_cooldown  = GetPrivateProfileIntW(L"dx", L"wake_cooldown_ms", 30000, ini);
    if (g_gap_ms   < 2000) g_gap_ms   = 2000;    // below this a scheduling hiccup fires it
    if (g_grace_ms < 0)    g_grace_ms = 0;
    if (g_cooldown < 0)    g_cooldown = 0;
}

void wake_reload(const wchar_t* ini)
{
    if (!ini) return;
    // wake_enable is NOT re-read: it decides whether the watchdog thread was
    // started, which is a startup decision. Everything else is live.
    g_trace     = GetPrivateProfileIntW(L"dx", L"wake_trace", trace_at(1), ini);
    g_nudge     = GetPrivateProfileIntW(L"dx", L"wake_nudge",      1, ini);
    g_do_reset  = GetPrivateProfileIntW(L"dx", L"wake_reset",      1, ini);
    g_grace_ms  = GetPrivateProfileIntW(L"dx", L"wake_grace_ms",   3000, ini);
    g_gap_ms    = GetPrivateProfileIntW(L"dx", L"wake_gap_ms",     15000, ini);
    g_max_reset = GetPrivateProfileIntW(L"dx", L"wake_max_resets", 8, ini);
    g_offthread = GetPrivateProfileIntW(L"dx", L"wake_offthread",  0, ini);
    g_cooldown  = GetPrivateProfileIntW(L"dx", L"wake_cooldown_ms", 30000, ini);
    if (g_gap_ms   < 2000) g_gap_ms   = 2000;
    if (g_grace_ms < 0)    g_grace_ms = 0;
    if (g_cooldown < 0)    g_cooldown = 0;
    logf("[reload] wake: nudge=%d reset=%d grace=%dms gap=%dms max_resets=%d "
         "cooldown=%dms offthread=%d", g_nudge, g_do_reset, g_grace_ms, g_gap_ms,
         g_max_reset, g_cooldown, g_offthread);
}

// ---------------------------------------------------------------- helpers

static const char* hr_name(HRESULT hr)
{
    return hr == S_OK                ? "D3D_OK"
         : hr == WAKE_DEVICELOST     ? "D3DERR_DEVICELOST"
         : hr == WAKE_DEVICENOTRESET ? "D3DERR_DEVICENOTRESET"
         : hr == WAKE_INVALIDCALL    ? "D3DERR_INVALIDCALL"
                                     : "";
}

// One line describing the window, which is the other half of "why is it black".
// Everything here is a read; nothing in it can move a window.
static void log_window_state(HWND w, const char* when)
{
    if (!w || !IsWindow(w)) {
        logf("[wake] %s: the game window handle is %p and is NOT a window -- the "
             "title tore its own window down", when, (void*)w);
        return;
    }
    RECT r = { 0, 0, 0, 0 }, c = { 0, 0, 0, 0 };
    GetWindowRect(w, &r);
    GetClientRect(w, &c);
    HWND fg = GetForegroundWindow();
    logf("[wake] %s: window %p rect=(%ld,%ld)-(%ld,%ld) client=%ldx%ld "
         "visible=%d iconic=%d zoomed=%d enabled=%d foreground=%s",
         when, (void*)w, r.left, r.top, r.right, r.bottom,
         c.right - c.left, c.bottom - c.top,
         IsWindowVisible(w) ? 1 : 0, IsIconic(w) ? 1 : 0, IsZoomed(w) ? 1 : 0,
         IsWindowEnabled(w) ? 1 : 0, (fg == w) ? "yes" : "no (something else)");
}

// The user32 half of the recovery, and the ONLY thing the watchdog thread is
// allowed to do to the title. Deliberately conservative:
//
//   * restore ONLY if the window is genuinely minimised (SW_RESTORE on a
//     maximised window would un-maximise it, which nobody asked for);
//   * NO SetForegroundWindow. The shim already polices focus grabs with
//     [dx] d3d_nosteal, and the whole "the window fights me" class of report
//     came from the shim raising windows on its own initiative. Waking up
//     should not steal focus from whatever the user alt-tabbed to;
//   * SWP_FRAMECHANGED with no move/size/z-order change, which is the standard
//     way to make the window manager re-evaluate a window without altering it.
//     Under Wine this is what re-drives the X11/Wayland surface a compositor may
//     be holding stale after a resume.
static void nudge_window(HWND w)
{
    if (!g_nudge || !w || !IsWindow(w)) return;
    if (IsIconic(w)) {
        logf("[wake] the game window was MINIMISED on resume -- restoring it");
        ShowWindow(w, SW_RESTORE);
    }
    SetWindowPos(w, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
    RedrawWindow(w, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    InterlockedIncrement(&g_n_nudges);
}

// ---------------------------------------------------------------- episode

static void arm(LONG reason)
{
    if (!g_enable) return;
    // g_cooldown is tested as well as g_cooldown_until so that setting
    // wake_cooldown_ms to 0 clears a cooldown ALREADY RUNNING. Without that the
    // knob only affected the next one, which made it useless as a live
    // adjustment: a user who turns it off during a session is asking for the
    // shim to start trying again now, not in half a minute. (Found by
    // wakerecovertest scenarios 6 and 7, which silently went inert inside the
    // cooldown scenario 5 left behind and asserted nothing at all.)
    if (reason != WAKE_R_SUSPEND && g_cooldown > 0 && g_cooldown_until &&
        (DWORD)(GetTickCount() - (DWORD)g_cooldown_until) >= 0x80000000u) {
        if (!InterlockedExchange(&g_said_cooldown, 1))
            logf("[wake] the device is still reporting itself lost, but the last "
                 "episode ended without recovering it -- not re-arming for %d s. "
                 "Nothing has changed since; re-arming per frame would only fill "
                 "the log.", g_cooldown / 1000);
        return;
    }
    if (InterlockedCompareExchange(&g_armed, 1, 0) != 0) return;  // already in one
    InterlockedExchange(&g_arm_reason, reason);
    InterlockedExchange(&g_arm_tick, (LONG)GetTickCount());
    InterlockedExchange(&g_frames_at_arm, g_frames);
    InterlockedExchange(&g_title_resets_at_arm, g_title_resets);
    InterlockedExchange(&g_our_resets, 0);
    InterlockedExchange(&g_said_lost, 0);
    InterlockedExchange(&g_said_defer, 0);
    InterlockedExchange(&g_said_noreset, 0);
    InterlockedExchange(&g_last_tc, 0);
    InterlockedExchange(&g_last_pump, 0);
    InterlockedExchange(&g_saw_lost, 0);
    InterlockedExchange(&g_resettable_seen, 0);
    InterlockedExchange(&g_said_cooldown, 0);
    InterlockedIncrement(&g_n_episodes);
    logf("[wake] --- recovery episode #%ld ARMED: %s. Watching the device from "
         "the title's own render thread (reset=%s, the title gets %d ms to do it "
         "itself first).",
         g_n_episodes, arm_reason_name(reason), g_do_reset ? "on" : "OFF",
         g_grace_ms);
    log_flush();
}

static void disarm(const char* verdict, int recovered)
{
    if (InterlockedCompareExchange(&g_armed, 0, 1) != 1) return;
    if (recovered) InterlockedIncrement(&g_n_recovered);
    else InterlockedExchange(&g_cooldown_until,
                             g_cooldown > 0 ? (LONG)(GetTickCount() + g_cooldown) : 0);
    logf("[wake] --- recovery episode #%ld ENDED after %lums: %s "
         "(our resets this episode: %ld)",
         g_n_episodes, (unsigned long)(GetTickCount() - (DWORD)g_arm_tick),
         verdict, g_our_resets);
    log_flush();
}

// THE PUMP. Runs on the render thread, from the Present and TestCooperativeLevel
// hooks. Everything it does to the device happens here and only here.
static void pump(void* dev)
{
    WakeDev d;
    if (!dev || !dev_snapshot(&d) || d.dev != dev) return;
    if (!InterlockedCompareExchange(&g_armed, 0, 0)) return;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;

    DWORD now = GetTickCount();
    if (g_last_pump && (DWORD)(now - (DWORD)g_last_pump) < WAKE_PUMP_EVERY_MS) {
        InterlockedExchange(&g_busy, 0);
        return;
    }
    InterlockedExchange(&g_last_pump, (LONG)now);
    DWORD age = (DWORD)(now - (DWORD)g_arm_tick);

    HRESULT tc = d.test ? d.test(dev) : S_OK;
    if (tc != (HRESULT)g_last_tc) {
        InterlockedExchange(&g_last_tc, (LONG)tc);
        logf("[wake] device state -> 0x%08lX %s (%lums into the episode)",
             (unsigned long)tc, hr_name(tc), (unsigned long)age);
        log_flush();
    }

    if (tc == S_OK) {
        LONG since = g_frames - g_frames_at_arm;
        if (since >= WAKE_FRAMES_RECOVERED) {
            // The one unambiguous good ending: the device is healthy AND the
            // title has put real frames through it since the episode started.
            // Whether anything was ever WRONG is a different question, and
            // conflating them would let this file claim a fix it did not make.
            disarm(g_saw_lost
                     ? "the device was lost and is now healthy again, with the "
                       "title presenting frames -- the picture is back"
                     : "the device was never lost and the title kept presenting "
                       "-- it survived the sleep on its own, nothing was repaired",
                   g_saw_lost ? 1 : 0);
        } else if (age > 10000) {
            // Healthy device, no frames. That is a genuinely different bug from
            // the one this file fixes, and saying so is the point -- it sends
            // the next session at the title's frame loop, not at D3D.
            char msg[320];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "the device reports D3D_OK but the title has presented only "
                        "%ld frame(s) since -- this is NOT a lost-device problem. "
                        "The title's render loop is not running (blocked, or it "
                        "stopped drawing on deactivation); look there, not at D3D",
                        since);
            disarm(msg, 0);
        }
        InterlockedExchange(&g_busy, 0);
        return;
    }

    InterlockedIncrement(&g_n_lost_seen);
    InterlockedExchange(&g_saw_lost, 1);

    if (tc == WAKE_DEVICELOST) {
        // Nothing anyone can do yet -- not us, not the title. The runtime has to
        // move it to DEVICENOTRESET first.
        if (!InterlockedExchange(&g_said_lost, 1))
            logf("[wake] the device is LOST and not yet resettable. Waiting for "
                 "the runtime to move it to D3DERR_DEVICENOTRESET; nothing can "
                 "Reset it before then.");
        if (age > WAKE_EPISODE_MAX_MS) {
            InterlockedIncrement(&g_n_gave_up);
            disarm("the device never became resettable -- it stayed "
                   "D3DERR_DEVICELOST for the whole episode. That is below the "
                   "D3D layer (the adapter/Vulkan device itself is gone); a "
                   "relaunch of the title is the only recovery", 0);
        }
        InterlockedExchange(&g_busy, 0);
        return;
    }

    if (tc != WAKE_DEVICENOTRESET) {
        // Some other failure. Log it once and keep watching rather than acting on
        // a state we do not have a contract for.
        if (!InterlockedExchange(&g_said_lost, 1))
            logf("[wake] TestCooperativeLevel returned 0x%08lX, which is neither "
                 "OK nor either lost state -- not acting on it", (unsigned long)tc);
        if (age > WAKE_EPISODE_MAX_MS) disarm("unrecognised device state throughout", 0);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    // --- D3DERR_DEVICENOTRESET: the device CAN be reset now. ------------------

    // ...and THIS is the instant the title's own recovery becomes possible, so
    // it is the instant the grace period starts. See g_resettable_seen.
    if (!InterlockedExchange(&g_resettable_seen, 1)) {
        InterlockedExchange(&g_resettable_tick, (LONG)now);
        logf("[wake] the device is now RESETTABLE (%lums after the episode "
             "started). The title has %d ms from here to Reset it itself.",
             (unsigned long)age, g_grace_ms);
    }
    DWORD resettable_for = (DWORD)(now - (DWORD)g_resettable_tick);

    // The title's own attempt wins, whatever came of it. If its Reset failed,
    // ours would fail the same way -- same runtime, same outstanding resources --
    // so there is nothing to add, and driving a second Reset into a title that is
    // mid-recovery is how you break one that was about to work.
    if (g_title_resets != g_title_resets_at_arm) {
        if (!InterlockedExchange(&g_said_defer, 1)) {
            InterlockedIncrement(&g_n_title_recovered);
            logf("[wake] the TITLE called Reset itself (%ld time(s) this episode) "
                 "-- it implements the lost-device contract, so we stand down and "
                 "only watch from here.",
                 g_title_resets - g_title_resets_at_arm);
        }
        if (age > WAKE_EPISODE_MAX_MS)
            disarm("the title was handling it itself; watched to the episode "
                   "limit without the device coming back", 0);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    if (resettable_for < (DWORD)g_grace_ms) {   // its chance, uninterrupted
        InterlockedExchange(&g_busy, 0);
        return;
    }

    if (!g_do_reset) {
        if (!InterlockedExchange(&g_said_noreset, 1))
            logf("[wake] the device is resettable and the title is not resetting "
                 "it, but [dx] wake_reset=0 -- doing nothing. Set wake_reset=1 to "
                 "let the shim rebuild it.");
        InterlockedExchange(&g_busy, 0);
        return;
    }

    if (g_our_resets >= g_max_reset) {
        InterlockedIncrement(&g_n_gave_up);
        disarm("gave up: our Reset was refused wake_max_resets times. The usual "
               "reason is D3DERR_INVALIDCALL -- the title is still holding "
               "D3DPOOL_DEFAULT resources it will not release because it does not "
               "know the device was lost. Nothing outside the title can free "
               "those; the title has to implement the contract", 0);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    if (!d.reset) {
        disarm("no Reset callback was registered for this device -- the API "
               "adapter could not hook Reset, so recovery is not available", 0);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    LONG attempt = InterlockedIncrement(&g_our_resets);
    InterlockedIncrement(&g_n_our_resets);
    logf("[wake] the title did not Reset in the %d ms since the device became "
         "resettable -- driving Reset #%ld ourselves with the parameters the "
         "device was last built with (%s)", g_grace_ms, attempt, d.api);
    log_flush();

    HRESULT hr = d.reset(dev);
    if (SUCCEEDED(hr)) {
        InterlockedIncrement(&g_n_our_reset_ok);
        logf("[wake] Reset SUCCEEDED (0x%08lX). The device is rebuilt; the title's "
             "next frames decide whether the picture is really back -- staying "
             "armed until %d of them land.", (unsigned long)hr,
             WAKE_FRAMES_RECOVERED);
        // Deliberately NOT disarmed here. "Reset returned S_OK" is not "the user
        // can see the game"; the S_OK arm above closes the episode on frames.
    } else {
        logf("[wake] Reset FAILED (0x%08lX %s)%s", (unsigned long)hr, hr_name(hr),
             hr == WAKE_INVALIDCALL
               ? " -- the runtime refused it because D3DPOOL_DEFAULT resources are "
                 "still alive. That is the runtime protecting the title from a "
                 "half-rebuilt device, and it means an EXTERNAL reset cannot work "
                 "for this title: only the title can release those."
               : "");
    }
    log_flush();
    InterlockedExchange(&g_busy, 0);
}

// ---------------------------------------------------------------- notifications
//
// All three are on hot paths, so the fast path is an increment and a test.

void wake_note_present(void* dev, HRESULT hr)
{
    // Only the device being watched. A second device's frames counted here would
    // read as "the title is rendering" while the one on screen is frozen.
    if (!g_enable || !dev || dev != g_dev_fast) return;
    InterlockedIncrement(&g_frames);
    if (hr == WAKE_DEVICELOST || hr == WAKE_DEVICENOTRESET) arm(WAKE_R_PRESENT);
    if (InterlockedCompareExchange(&g_armed, 0, 0)) pump(dev);
}

void wake_note_test(void* dev, HRESULT hr)
{
    if (!g_enable || !dev || dev != g_dev_fast) return;
    if (hr == WAKE_DEVICELOST || hr == WAKE_DEVICENOTRESET) arm(WAKE_R_TEST);
    if (InterlockedCompareExchange(&g_armed, 0, 0)) pump(dev);
}

void wake_note_reset(void* dev, HRESULT hr)
{
    (void)hr;
    if (!g_enable || !dev || dev != g_dev_fast) return;
    InterlockedIncrement(&g_title_resets);
}

// ---------------------------------------------------------------- watchdog

static DWORD wake_now_wall_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (DWORD)(u.QuadPart / 10000ULL);   // 100ns -> ms, wraps like GetTickCount
}

// Follow-up state: after a wake we want to know, a few seconds later, whether
// the title's frame loop is running at all. That answer is what separates "the
// device died" from "the title stopped drawing", and it is the first thing the
// next session will want out of the log.
static DWORD g_followup_at    = 0;
static LONG  g_followup_frames = 0;

static void on_wake(DWORD gap_ms, const char* how)
{
    InterlockedIncrement(&g_n_wakes);
    WakeDev d;
    bool have = dev_snapshot(&d);
    logf("[wake] *** RESUME DETECTED: about %lu.%lus unaccounted for (%s). "
         "A Direct3D device does not survive that on its own -- checking.",
         (unsigned long)(gap_ms / 1000), (unsigned long)((gap_ms % 1000) / 100), how);
    // THE NETWORK HALF, and it runs FIRST and unconditionally. A suspend kills
    // the auth-band sockets too, and until 2026-09-06 nothing anywhere did
    // anything about that -- the client came back believing in a session that
    // was gone and raised POL-0006 on the next dial (a duplicate bind, in its
    // own stack), recoverable only by fully exiting. It is deliberately NOT
    // behind the `have` check below: whether a Direct3D device is registered
    // says nothing about whether a session socket is open, and the common case
    // -- idling in the Viewer with no title running -- has a socket and no
    // device. Ordering it before the D3D work also means the client learns its
    // connection is dead while it is still repainting, not after.
    sessionwatch_note_wake((unsigned long)gap_ms);
    if (!have) {
        logf("[wake] no Direct3D device is registered (no title running, or its "
             "device was never hooked) -- nothing to recover.");
        log_flush();
        return;
    }
    log_window_state(d.wnd, "on resume");
    nudge_window(d.wnd);
    arm(WAKE_R_SUSPEND);
    g_followup_frames = g_frames;
    g_followup_at     = GetTickCount() + 4000;
    log_flush();
}

// [dx] wake_offthread -- A DEBUG LEVER, NOT A FIX.
//
// It drives the pump from the WATCHDOG thread when the render thread has stopped
// calling us, which is the one case the correct design cannot reach: a title
// whose frame loop is wedged never runs Present or TestCooperativeLevel, so the
// on-thread pump never fires. Turning this on calls a non-thread-safe device
// from the wrong thread, and if the render thread is only SLOW rather than
// wedged, that is a data race inside the runtime with a crash as the visible
// outcome. It exists so a live session can answer "would a Reset have worked?"
// when nothing else can ask; a positive result from it is a MEASUREMENT, and the
// fix it points at is still a fix that runs on the render thread.
// See the shim-forcecall-is-a-debug-lever note: same shape, same rule.
static void offthread_pump(void)
{
    if (!g_offthread) return;
    if (!InterlockedCompareExchange(&g_armed, 0, 0)) return;
    WakeDev d;
    if (!dev_snapshot(&d)) return;
    // Only when the render thread is demonstrably not coming: no frame in 3s.
    static LONG last_frames = -1;
    static DWORD quiet_since = 0;
    LONG f = g_frames;
    DWORD now = GetTickCount();
    if (f != last_frames) { last_frames = f; quiet_since = now; return; }
    if ((DWORD)(now - quiet_since) < 3000) return;
    static LONG said = 0;
    if (!InterlockedExchange(&said, 1))
        logf("[wake] wake_offthread=1: the render thread has presented nothing "
             "for 3s, so the pump is being driven from the WATCHDOG thread. This "
             "is a DEBUG LEVER -- it calls a non-thread-safe device off-thread "
             "and can crash the title. Do not leave it on.");
    pump(d.dev);
}

static DWORD WINAPI wake_thread(LPVOID)
{
    DWORD  last_tick = GetTickCount();
    DWORD  last_wall = wake_now_wall_ms();
    for (;;) {
        Sleep(1000);
        DWORD tick = GetTickCount();
        DWORD wall = wake_now_wall_ms();
        DWORD dt   = tick - last_tick;             // unsigned: wrap-safe
        DWORD dw   = wall - last_wall;
        last_tick = tick;
        last_wall = wall;

        // Two independent signals; see the header for why both are needed.
        //   skew : the wall clock ran on while the tick clock did not  (Wine/Linux)
        //   dt   : the tick clock itself jumped                        (Windows)
        // dw < dt means the wall clock stepped BACKWARDS relative to the tick
        // count (an NTP correction). Not a wake, and treating it as one would
        // fire this on every time sync.
        DWORD skew = (dw > dt) ? (dw - dt) : 0;
        if (skew >= (DWORD)g_gap_ms)
            on_wake(skew, "the wall clock advanced while the tick count did not "
                          "-- the host suspended under us");
        else if (dt >= (DWORD)g_gap_ms)
            on_wake(dt, "the tick count itself jumped -- a system sleep, or the "
                        "whole process was stalled that long");

        if (g_followup_at && (DWORD)(GetTickCount() - g_followup_at) < 0x80000000u) {
            LONG since = g_frames - g_followup_frames;
            g_followup_at = 0;
            logf("[wake] 4s after the resume the title has presented %ld frame(s) "
                 "-- %s", since,
                 since > 10 ? "its render loop IS running, so anything black is "
                              "being drawn black or the device is lost under it"
                            : since > 0 ? "its render loop is barely ticking"
                                        : "its render loop is NOT running at all. "
                                          "No D3D recovery can help that; the "
                                          "title is blocked somewhere else");
            WakeDev d;
            if (dev_snapshot(&d)) log_window_state(d.wnd, "4s after the resume");
            log_flush();
        }
        offthread_pump();
    }
}

// ---------------------------------------------------------------- registration

void wake_register_device(const char* api, void* dev, WakeTestFn test,
                          WakeResetFn reset, HWND wnd)
{
    if (!g_enable || !dev) return;
    dev_lock_init();
    EnterCriticalSection(&g_dev_cs);
    bool changed = (g_dev.dev != dev);
    g_dev.dev   = dev;
    g_dev.test  = test;
    g_dev.reset = reset;
    g_dev.api   = api ? api : "?";
    g_dev.wnd   = wnd;
    LeaveCriticalSection(&g_dev_cs);
    // Published LAST, so the per-frame path never sees the pointer before the
    // callbacks it will be used with.
    g_dev_fast = dev;

    if (changed)
        logf("[wake] watching the %s device %p (window %p) for sleep/resume. "
             "test=%s reset=%s -- %s", api, dev, (void*)wnd,
             test ? "yes" : "NO", reset ? "yes" : "NO",
             (test && reset) ? "full recovery available"
                             : "DETECTION ONLY (a callback is missing, so the "
                               "shim can log the loss but not repair it)");

    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        HANDLE h = CreateThread(NULL, 0, wake_thread, NULL, 0, NULL);
        if (h) {
            CloseHandle(h);
            logf("[wake] resume watchdog started (1s tick, gap>=%dms counts as a "
                 "wake). Recovery itself runs on the title's render thread.",
                 g_gap_ms);
        } else {
            logf("[wake] WARN: could not start the resume watchdog -- sleep will "
                 "not be detected. Device loss seen by Present/TestCooperativeLevel "
                 "is still handled.");
        }
    }
    log_flush();
}

void wake_forget_device(void* dev)
{
    if (InterlockedCompareExchange(&g_dev_cs_ready, 2, 2) != 2) return;
    EnterCriticalSection(&g_dev_cs);
    if (g_dev.dev == dev) {
        g_dev.dev = NULL; g_dev.test = NULL; g_dev.reset = NULL;
        g_dev_fast = NULL;
    }
    LeaveCriticalSection(&g_dev_cs);
}

// The window can change after registration (a Reset re-homes the device), and
// the nudge is only worth anything if it lands on the right one.
void wake_note_window(HWND w)
{
    if (InterlockedCompareExchange(&g_dev_cs_ready, 2, 2) != 2) return;
    EnterCriticalSection(&g_dev_cs);
    if (w && IsWindow(w)) g_dev.wnd = w;
    LeaveCriticalSection(&g_dev_cs);
}

// ---------------------------------------------------------------- summary

void wake_summary()
{
    if (!g_enable) return;
    WakeDev d;
    bool have = dev_snapshot(&d);
    logf("[wake] summary: resumes detected=%ld episodes=%ld | our resets=%ld "
         "(succeeded %ld) | recovered=%ld gave-up=%ld | title handled it=%ld | "
         "window nudges=%ld lost-state polls=%ld frames=%ld",
         g_n_wakes, g_n_episodes, g_n_our_resets, g_n_our_reset_ok,
         g_n_recovered, g_n_gave_up, g_n_title_recovered, g_n_nudges,
         g_n_lost_seen, g_frames);
    if (!have)
        logf("[wake] NOTE: no Direct3D device was ever registered this session. "
             "Either no title ran, or [dx] d3d_enable=0 -- the recovery rides on "
             "the same hooks as the windowed override and cannot arm without them.");
    else if (!g_n_wakes && !g_n_episodes)
        logf("[wake] NOTE: armed all session and never fired. That is the expected "
             "reading for a session that was not suspended and never lost its "
             "device -- it is NOT evidence that the recovery works.");
    else if (g_n_wakes && !g_n_episodes)
        logf("[wake] NOTE: %ld resume(s) detected but no episode ran, which cannot "
             "happen unless the device was unregistered at the time. Read the "
             "[wake] lines above rather than this counter.", g_n_wakes);
}
