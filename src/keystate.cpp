// keystate.cpp -- SEAM 5 of the focus gate: the keyboard APIs that do not care
// which window is in front.
//
// WHY THIS EXISTS
//
// Reported 2026-08-26, again 2026-09-04, and again 2026-09-09 -- three times, in
// almost the same words: "Fantasy Earth is still accepting my keyboard inputs
// even when the window is not active."
//
// The mouse gate (inputgate.cpp) was never at fault; its counters show it
// firing. The keyboard had no gate at all, and it reaches a POL title by more
// than one road. This file owns the road that nothing in this project had ever
// hooked:
//
//   GetAsyncKeyState   asks the OS for the PHYSICAL state of a key. It does not
//   GetKeyState        take a window, it does not consult focus, and it answers
//   GetKeyboardState   a background process exactly as it answers the
//                      foreground one. A game loop that polls these plays on
//                      while you type in another application, and no amount of
//                      window styling, windowed mode or message filtering can
//                      change that -- there is no message involved.
//
// KEY: THIS IS NOT A GUESS ABOUT FANTASY EARTH. Measured 2026-09-09 from the
// import table of the shipped FE_Client.dll:
//
//   == USER32.dll
//      ... GetAsyncKeyState, ... GetKeyState, ... GetKeyboardLayout
//
// Both are imported BY NAME, which is what makes them reachable by an IAT swap
// at all. (FE also creates a DirectInput keyboard -- that is seam 4, in
// dinputhook.cpp. Which of the two actually carries its keys is a question no
// log has ever answered, so BOTH are gated and BOTH are counted; see the [gate]
// and [key] summaries. Gating only the one we guessed at is how this bug got
// declared fixed on 2026-09-04 and came back.)
//
// THE RULE, UNCHANGED: gate DELIVERY, never ACQUISITION. There is nothing to
// acquire here, which makes it the simplest seam of the six: while another
// application is in front, the title is told no key is down. The instant you
// click back, the real state is reported again -- no state of our own to unwind,
// no edge to release, exactly like the polled DirectInput mouse.
//
// KEY: WHY THE REAL FUNCTION IS STILL CALLED WHILE BLOCKED. GetAsyncKeyState's
// low bit means "pressed since the last call", and that bit is CLEARED by
// reading it. If we returned 0 without calling through, every key pressed while
// you were away would still be sitting in that bit, and the title's first poll
// after you clicked back would fire all of them at once. Calling and discarding
// consumes them, which is what a genuinely unfocused application experiences.
// It also gives us the measurement below for free.
//
// WHERE IT IS INSTALLED
//
// inject.cpp's patch_iat, by RESOLVED ADDRESS, over every module in the process
// except the shim itself (mod == g_self returns early). That exemption is
// load-bearing: polsettings.cpp's settings hotkey and tmevent.cpp's debug keys
// are GetAsyncKeyState callers inside PolHook.dll, and they must keep working --
// a hotkey you can only use while the game is in front is not a hotkey.
//
// The hooks are installed UNCONDITIONALLY and consult the gate on each call,
// rather than being installed only when the gate is on. [dx] key_focus_gate is
// re-read live, and a hook that was never installed cannot start working when
// the setting changes -- that would be a switch that does nothing until the next
// launch, which is precisely the class of failure this project keeps hitting.
// With the gate off these are one predicted branch and a tail call.
//
// [dx] key_focus_gate = 0 restores SE's behaviour.

#include "polshim.h"
#include <windows.h>
#include <intrin.h>          // _ReturnAddress -- names the module that polled

typedef SHORT (WINAPI* PFN_GetAsyncKeyState)(int);
typedef SHORT (WINAPI* PFN_GetKeyState)(int);
typedef BOOL  (WINAPI* PFN_GetKeyboardState)(PBYTE);

static PFN_GetAsyncKeyState g_real_async = NULL;
static PFN_GetKeyState      g_real_key   = NULL;
static PFN_GetKeyboardState g_real_state = NULL;

static LONG g_n_calls    = 0;   // every call through the three hooks
static LONG g_n_withheld = 0;   // ...that were answered "nothing is down"
static LONG g_n_real_dn  = 0;   // ...where a key REALLY WAS down. THE EVIDENCE.
static LONG g_n_logged   = 0;

// ---------------------------------------------------------------------------
// THE DECISION -- pure, so the harness can prove it without a desktop.
//
// `blocked` is the whole policy; this is the one line that has to be right, and
// the thing worth asserting is that OFF is a true pass-through (bit for bit,
// including the toggle and "pressed since" bits) and ON reveals nothing at all.
// ---------------------------------------------------------------------------
SHORT keystate_filter(SHORT real, bool blocked)
{
    return blocked ? (SHORT)0 : real;
}

// A key is physically down if the high bit is set. This is what makes the
// difference between "we gated a path" and "we gated a path that was CARRYING
// SOMETHING" -- without it, a session where you never touched a key while away
// and a session where the gate is broken produce identical logs.
static bool really_down(SHORT v) { return (v & (SHORT)0x8000) != 0; }

static void note(const char* api, int vk, bool was_down, void* ra)
{
    InterlockedIncrement(&g_n_withheld);
    inputgate_note_key_blocked(5, 0);
    if (!was_down) return;
    InterlockedIncrement(&g_n_real_dn);
    LONG n = InterlockedIncrement(&g_n_logged);
    if (n > 6) return;             // the first few carry the finding, then quiet
    char mod[MAX_PATH] = "?";
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char*)ra, &m) && m) {
        GetModuleFileNameA(m, mod, sizeof(mod));
        const char* leaf = strrchr(mod, '\\');
        if (leaf) memmove(mod, leaf + 1, strlen(leaf));
    }
    logf("[key] %s(vk=0x%02X) while ANOTHER APPLICATION is in front: the key IS "
         "physically down and the answer was withheld from %s. That module polls "
         "the keyboard directly, so nothing but this gate could have stopped it "
         "([dx] key_focus_gate=0 to stop)", api, vk, mod);
}

static SHORT WINAPI hook_GetAsyncKeyState(int vk)
{
    void* ra = _ReturnAddress();
    InterlockedIncrement(&g_n_calls);
    SHORT r = g_real_async ? g_real_async(vk) : (SHORT)0;
    if (!inputgate_key_blocked()) return r;
    note("GetAsyncKeyState", vk, really_down(r), ra);
    return keystate_filter(r, true);
}

static SHORT WINAPI hook_GetKeyState(int vk)
{
    void* ra = _ReturnAddress();
    InterlockedIncrement(&g_n_calls);
    SHORT r = g_real_key ? g_real_key(vk) : (SHORT)0;
    if (!inputgate_key_blocked()) return r;
    note("GetKeyState", vk, really_down(r), ra);
    return keystate_filter(r, true);
}

// The whole 256-byte array in one call. Zeroed rather than failed: a caller that
// gets FALSE may well fall back to GetAsyncKeyState per key, or treat the read
// as an error and do something louder than nothing.
static BOOL WINAPI hook_GetKeyboardState(PBYTE st)
{
    void* ra = _ReturnAddress();
    InterlockedIncrement(&g_n_calls);
    BOOL ok = g_real_state ? g_real_state(st) : FALSE;
    if (!inputgate_key_blocked() || !ok || !st) return ok;
    bool any = false;
    for (int i = 0; i < 256; i++) if (st[i] & 0x80) { any = true; break; }
    note("GetKeyboardState", 0, any, ra);
    ZeroMemory(st, 256);
    return ok;
}

void keystate_init(const wchar_t* ini)
{
    (void)ini;                     // the switch lives on inputgate (key_focus_gate)
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) u32 = LoadLibraryW(L"user32.dll");
    if (u32) {
        g_real_async = (PFN_GetAsyncKeyState)GetProcAddress(u32, "GetAsyncKeyState");
        g_real_key   = (PFN_GetKeyState)     GetProcAddress(u32, "GetKeyState");
        g_real_state = (PFN_GetKeyboardState)GetProcAddress(u32, "GetKeyboardState");
    }
    if (!g_real_async || !g_real_key || !g_real_state)
        logf("[key] user32 keyboard-state entry points NOT resolved (async=%p "
             "key=%p state=%p) -- seam 5 will install nothing, and a title that "
             "polls the keyboard will keep playing while you are elsewhere",
             (void*)g_real_async, (void*)g_real_key, (void*)g_real_state);
}

// Exposed for inject.cpp's patch_iat by-value swap map. A NULL `from` never
// matches, so an unresolved entry point leaves every IAT untouched instead of
// installing a hook that would call through a NULL pointer.
void* keystate_real_GetAsyncKeyState() { return (void*)g_real_async; }
void* keystate_hook_GetAsyncKeyState() { return g_real_async ? (void*)hook_GetAsyncKeyState : NULL; }
void* keystate_real_GetKeyState()      { return (void*)g_real_key; }
void* keystate_hook_GetKeyState()      { return g_real_key ? (void*)hook_GetKeyState : NULL; }
void* keystate_real_GetKeyboardState() { return (void*)g_real_state; }
void* keystate_hook_GetKeyboardState() { return g_real_state ? (void*)hook_GetKeyboardState : NULL; }

void keystate_summary(void)
{
    if (!g_n_calls) {
        logf("[key] summary: nothing in this process ever called "
             "GetAsyncKeyState/GetKeyState/GetKeyboardState through a patched "
             "IAT. Either the title does not poll the keyboard this way, or the "
             "swap never reached its imports -- check for an "
             "'[iat] hooked u32:GetAsyncKeyState in <title>' line before "
             "concluding the first.");
        return;
    }
    logf("[key] summary: %ld call(s), %ld withheld while another application was "
         "in front, %ld of those with a key GENUINELY DOWN",
         g_n_calls, g_n_withheld, g_n_real_dn);
    if (g_n_withheld && !g_n_real_dn)
        logf("[key] summary: the gate fired but no key was ever actually held "
             "while you were away -- this seam is armed and proven reachable, "
             "but this session does NOT prove it was the one carrying the keys.");
    if (g_n_real_dn)
        logf("[key] summary: %ld read(s) of a REALLY-HELD key were withheld -- "
             "this seam was carrying input to the title, and before this build "
             "nothing was hooking it.", g_n_real_dn);
}

// ---------------------------------------------------------------------------
// selftest -- the filter, which is the only part that can be silently wrong.
// ---------------------------------------------------------------------------
int keystate_selftest(void)
{
    int fail = 0;
    #define CHK(c, m) do { if (!(c)) { logf("[key] SELFTEST FAIL: %s", m); fail++; } } while (0)

    // OFF is a bit-for-bit pass-through -- including the toggle bit (1) and the
    // "pressed since last call" bit, which a naive & 0x8000 filter would eat.
    CHK(keystate_filter((SHORT)0x8001, false) == (SHORT)0x8001, "off: pass through held+pressed");
    CHK(keystate_filter((SHORT)0x0001, false) == (SHORT)0x0001, "off: pass through the toggle bit alone");
    CHK(keystate_filter((SHORT)0x0000, false) == (SHORT)0x0000, "off: pass through nothing-down");

    // ON reveals NOTHING -- not the high bit, not the toggle, not the low bit.
    // A caller must not be able to infer a keypress from any surviving bit.
    CHK(keystate_filter((SHORT)0x8001, true) == 0, "on: withhold held+pressed");
    CHK(keystate_filter((SHORT)0x0001, true) == 0, "on: withhold the toggle bit");
    CHK(keystate_filter((SHORT)0xFFFF, true) == 0, "on: withhold every bit");

    CHK(really_down((SHORT)0x8000), "0x8000 is down");
    CHK(!really_down((SHORT)0x0001), "the toggle bit alone is not down");
    CHK(!really_down((SHORT)0x0000), "nothing is not down");

    #undef CHK
    if (!fail) logf("[key] selftest OK");
    return fail;
}
