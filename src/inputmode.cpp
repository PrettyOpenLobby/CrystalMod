// inputmode.cpp -- keyboard vs. gamepad input-mode control for the Viewer.
//
// WHY THIS EXISTS
//
// The Viewer has two input modes -- keyboard/mouse and gamepad -- and the only
// shipped way to switch is the external prefs app (polcfg.exe), which writes a
// single REG_DWORD:
//     HKLM\SOFTWARE\WOW6432Node\<hive>\<pub>\PlayOnlineViewer\Settings
//         UseGameController   (0 = keyboard/mouse, 1 = gamepad)
// On a fresh Proton/Steam-Deck install that value is absent (=0), so the Deck's
// controller does nothing and there is no in-Viewer way to fix it -- you have to
// run the prefs app, which the Deck install does not surface.
//
// WHERE THE FLAG LIVES (reversed 2026-08-14, all in pol.exe, image base fixed at
// 0x00400000, no ASLR -- DllCharacteristics has no DYNAMIC_BASE):
//
//   * The settings loader at 0x00407220 reads "UseGameController" (the UTF-16
//     literal at 0x00442128) and stores (regvalue == 1) as a BYTE into the config
//     object at [this+0x21]. Sibling fields load the same way: PlayOpeningMovie
//     -> +0x1d, PlayAudio -> +0x20, FullScreen -> +0x24 (dword).
//   * That byte is then READ LIVE by three UI handlers (0x0040A7F4, 0x0041375A,
//     0x00413A62) -- it is not latched once. The ==1 branches ADD pad navigation
//     (post a window message, set a nav mode via a vtable slot); the else path
//     does not tear the mouse down, so gamepad mode reads as ADDITIVE, not
//     exclusive. (That "additive" reading is a static inference -- confirm it
//     live before trusting force_gamepad on a machine you also mouse on.)
//
// THE PATCH
//
// The loader turns the register read into the stored byte with a `sete dl` at
// 0x00407285 (bytes 0F 94 C2), whose result feeds ONLY the UseGameController
// store (PlayAudio/FullScreen each have their own sete), so overwriting it is
// surgical:
//     force_gamepad   0F 94 C2  ->  B2 01 90   (mov dl,1 ; nop)  -- always store 1
//     force_keyboard  0F 94 C2  ->  B2 00 90   (mov dl,0 ; nop)  -- always store 0
// Applied to pol.exe IN MEMORY only (the registry and the file on disk are never
// touched, so the change is undone by clearing [inputmode] mode). The launcher
// injects before pol.exe's entry point, so the patch is in place before the
// loader ever runs.
//
// Verified byte-for-byte against a build fingerprint before writing -- a
// different pol.exe fails safe (logs and does nothing) rather than corrupting
// code, exactly as patches.cpp does for app.dll.
//
// NOT DONE HERE: true real-time auto-switching (pad press -> gamepad, mouse move
// -> keyboard). That needs the LIVE config-object pointer and a re-apply trigger
// for the current screen, both of which need a live run to test.
// This module is the force/default half, and
// the empirical test of whether gamepad mode is additive (in which case
// force_gamepad alone is the whole fix and auto-switching is unnecessary).

#include "polshim.h"

// --- config ----------------------------------------------------------------
enum { IM_OFF = 0, IM_GAMEPAD = 1, IM_KEYBOARD = 2 };
static int g_mode = IM_OFF;

// --- the site, as RVAs from pol.exe's fixed 0x00400000 base -----------------
#define IM_SETE_RVA   0x00007285      // sete dl
#define IM_STORE_RVA  0x0000728E      // mov [esi+0x21], dl   (fingerprint)
#define IM_STR_RVA    0x00042128      // L"UseGameController" (fingerprint)

static const BYTE SETE_ORIG[3]  = { 0x0F, 0x94, 0xC2 };          // sete dl
static const BYTE STORE_ORIG[3] = { 0x88, 0x56, 0x21 };          // mov [esi+0x21], dl
static const BYTE WANT_GAMEPAD[3]  = { 0xB2, 0x01, 0x90 };       // mov dl,1 ; nop
static const BYTE WANT_KEYBOARD[3] = { 0xB2, 0x00, 0x90 };       // mov dl,0 ; nop

void inputmode_configure(const wchar_t* ini)
{
    wchar_t v[32] = L"";
    ini_str(L"inputmode", L"mode", L"off", v, _countof(v), ini);
    if      (!_wcsicmp(v, L"force_gamepad")  || !_wcsicmp(v, L"gamepad"))  g_mode = IM_GAMEPAD;
    else if (!_wcsicmp(v, L"force_keyboard") || !_wcsicmp(v, L"keyboard")) g_mode = IM_KEYBOARD;
    else                                                                    g_mode = IM_OFF;
}

int inputmode_enabled() { return g_mode != IM_OFF; }

// Confirm this is the pol.exe build we reversed: the sete + the store that
// consumes it, and the "UseGameController" wide string it belongs to. Any
// mismatch means a different build -> do nothing.
static bool fingerprint_ok(const BYTE* base)
{
    // SEH-guarded: these reach base + 0x4214C, and if this pol.exe were ever smaller
    // than that (a different build) the unguarded read faulted and took the process
    // down before its entry point ran. A fault just means "not the build we reversed".
    static const wchar_t* kName = L"UseGameController";
    __try {
        if (memcmp(base + IM_SETE_RVA,  SETE_ORIG,  sizeof(SETE_ORIG))  != 0) return false;
        if (memcmp(base + IM_STORE_RVA, STORE_ORIG, sizeof(STORE_ORIG)) != 0) return false;
        if (memcmp(base + IM_STR_RVA, kName, (wcslen(kName) + 1) * sizeof(wchar_t)) != 0) return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void inputmode_apply()
{
    if (g_mode == IM_OFF) return;

    // The main image is pol.exe; the flag lives there and nowhere else.
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    if (!base) { logf("[inputmode] no main module -- skipped"); return; }

    // A sanity check on the fixed base: if this pol.exe were ever rebased the
    // RVAs would be wrong. It has no DYNAMIC_BASE, so this should always hold.
    if (base != (BYTE*)0x00400000)
        logf("[inputmode] NOTE: pol.exe base is %p, expected 0x00400000 -- "
             "RVAs assume the fixed base; proceeding on fingerprint match only", base);

    if (!fingerprint_ok(base)) {
        logf("[inputmode] mode=%s but pol.exe does not match the reversed build "
             "(sete/store/string fingerprint) -- doing nothing, input mode is left "
             "as the registry sets it", g_mode == IM_GAMEPAD ? "force_gamepad" : "force_keyboard");
        return;
    }

    const BYTE* want = (g_mode == IM_GAMEPAD) ? WANT_GAMEPAD : WANT_KEYBOARD;
    BYTE* at = base + IM_SETE_RVA;
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(SETE_ORIG), PAGE_EXECUTE_READWRITE, &old)) {
        logf("[inputmode] VirtualProtect failed at pol.exe+0x%X -- not applied", IM_SETE_RVA);
        return;
    }
    memcpy(at, want, sizeof(SETE_ORIG));
    VirtualProtect(at, sizeof(SETE_ORIG), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(SETE_ORIG));

    logf("[inputmode] FORCED %s: pol.exe+0x%X sete dl -> %s. The "
         "UseGameController registry value is now ignored for this session "
         "(registry and disk untouched; clear [inputmode] mode to revert).",
         g_mode == IM_GAMEPAD ? "GAMEPAD mode" : "KEYBOARD/MOUSE mode",
         IM_SETE_RVA,
         g_mode == IM_GAMEPAD ? "mov dl,1;nop" : "mov dl,0;nop");
}
