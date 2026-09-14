// fmoime.cpp -- open Front Mission Online's text fields in DIRECT (alphanumeric)
// input instead of Japanese, so typing produces English without touching the IME
// first. Requested 2026-08-21.
//
// WHAT THE CLIENT ACTUALLY DOES. There is NO software keyboard on the PC build.
// The kana grids in .data at 0x6138F42C / 0x61392014 (`あかさたなはまやら`, the
// gojuon rows) have ZERO code references anywhere in the image -- they are dead
// PS2-era data. The PC port drives the Windows IME instead: eight IMM32 imports,
// of which ImmSetOpenStatus is the one that decides whether a field is in
// Japanese or direct input.
//
// Both text-field entry points do the same two-step, e.g. at 0x61083140:
//
//   61083154  mov  ecx,[0x613B79B8]      ; the HIMC global
//   6108315A  push 1
//   6108315D  call ImmSetOpenStatus      ; force the IME OPEN...
//   61083162  movzx edx, byte [esi+0x14] ; ...then restore the field's REMEMBERED flag
//   6108316D  call ImmSetOpenStatus
//
// and the same pair again at 0x610839C2 / 0x610839E1. `byte [esi+0x14]` is a
// per-field saved "the IME was open" flag, written at 0x61083941 on the path
// guarded by ImmGetOpenStatus.
//
// WARNING: SO THIS IS NOT "CHANGING A DEFAULT". There is no initialiser constant to
// flip -- the byte is *remembered state*. What this patch does is neutralise the
// RESTORE, so every field opens with the IME closed. The consequence, stated
// plainly because it is a real trade and not a free win: **the IME can still be
// toggled by hand while a field is open, but that choice is no longer carried
// to the next field.** Someone who types Japanese constantly will find that
// worse, which is exactly why this is a switch and not a hardcoded edit.
//
// THE SITES (RVA, byte-verified before every write):
//   0x83162  0F B6 56 14  movzx edx,byte [esi+14]  ->  33 D2 90 90  xor edx,edx
//   0x839D5  0F B6 46 14  movzx eax,byte [esi+14]  ->  33 C0 90 90  xor eax,eax
// Both are consumed immediately as the BOOL argument to ImmSetOpenStatus, and no
// flag set by `xor` is read before the following `call` -- the next instruction
// at each site is a `mov` from the HIMC global.
//
// WHY THE SHIM AND NOT A DISK PATCH. FrontMissionOnline.dll's `.text` has
// **SizeOfRawData = 0**: the code section is not in the file at all, and POL1
// rebuilds it at runtime. The A4 IID patch works on disk only because GUIDs live
// in .rdata/.rsrc, which ARE present. There is nothing on disk to edit here.
//
// WHY A WATCHER AND NOT apply_module. fmokey.cpp already paid for this lesson:
// the module callbacks come off the DllGetClassObject interposer, which fires
// for polcore.dll and app.dll and **never** for FrontMissionOnline.dll. So this
// finds the module itself, and keeps retrying until the bytes appear -- which
// doubles as the wait for POL1 to finish unpacking, since a still-packed page
// fails the byte-verify rather than getting written.
#include "polshim.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

// The two restore sites and their replacements.
struct ImeSite {
    DWORD         rva;
    unsigned char orig[4];
    unsigned char repl[4];
    const char*   what;
};

static ImeSite g_sites[] = {
    { 0x83162, { 0x0F, 0xB6, 0x56, 0x14 }, { 0x33, 0xD2, 0x90, 0x90 },
      "movzx edx,byte [esi+0x14] -> xor edx,edx" },
    { 0x839D5, { 0x0F, 0xB6, 0x46, 0x14 }, { 0x33, 0xC0, 0x90, 0x90 },
      "movzx eax,byte [esi+0x14] -> xor eax,eax" },
};
static const int  N_SITES = (int)(sizeof(g_sites) / sizeof(g_sites[0]));

static int    g_on          = 0;
static HANDLE g_thread      = NULL;
static LONG   g_stop        = 0;
static int    g_applied     = 0;      // how many sites are ours
static int    g_said_packed = 0;
static int    g_said_odd    = 0;
static BYTE*  g_base        = NULL;

// Patch one site. Returns 1 if the site now holds our bytes.
static int apply_site(BYTE* base, ImeSite* s)
{
    unsigned char* a = base + s->rva;
    if (IsBadReadPtr(a, sizeof(s->orig))) return 0;

    if (memcmp(a, s->repl, sizeof(s->repl)) == 0) return 1;   // already ours

    if (memcmp(a, s->orig, sizeof(s->orig)) != 0) {
        // Tell "still packed" apart from "a build we do not know". An all-zero
        // page is POL1 not having unpacked yet and is worth retrying; anything
        // else is a different build and must never be written over.
        int allzero = 1;
        for (size_t i = 0; i < sizeof(s->orig); i++) if (a[i]) { allzero = 0; break; }
        if (allzero) {
            if (!g_said_packed++)
                logf("[fmoime] FrontMissionOnline.dll+0x%05X still blank (POL1 "
                     "packed) -- waiting for the unpack", s->rva);
        } else if (!g_said_odd++) {
            logf("[fmoime] FrontMissionOnline.dll+0x%05X holds %02X %02X %02X %02X, "
                 "not the known ImmSetOpenStatus restore -- NOT patching (other build?)",
                 s->rva, a[0], a[1], a[2], a[3]);
        }
        return 0;
    }

    DWORD old = 0;
    if (!VirtualProtect(a, sizeof(s->repl), PAGE_EXECUTE_READWRITE, &old)) {
        logf("[fmoime] VirtualProtect failed at FrontMissionOnline.dll+0x%05X (err %lu)",
             s->rva, GetLastError());
        return 0;
    }
    memcpy(a, s->repl, sizeof(s->repl));
    DWORD junk = 0;
    VirtualProtect(a, sizeof(s->repl), old, &junk);
    FlushInstructionCache(GetCurrentProcess(), a, sizeof(s->repl));
    logf("[fmoime] patched FrontMissionOnline.dll+0x%05X: %s", s->rva, s->what);
    return 1;
}

static DWORD WINAPI ime_watch(LPVOID)
{
    // Find FMO ourselves -- see the header comment on why a callback cannot.
    while (!g_base) {
        if (InterlockedCompareExchange(&g_stop, 0, 0)) return 0;
        HMODULE h = GetModuleHandleA("FrontMissionOnline.dll");
        if (h) { g_base = (BYTE*)h; break; }
        Sleep(1000);
    }
    logf("[fmoime] FrontMissionOnline.dll at %p -- waiting for the text-field "
         "IME sites to unpack", g_base);

    // Retry until both sites are ours. The byte-verify IS the unpack wait.
    for (;;) {
        if (InterlockedCompareExchange(&g_stop, 0, 0)) return 0;

        // A re-load at a different base means a fresh image: re-arm.
        HMODULE h = GetModuleHandleA("FrontMissionOnline.dll");
        if (h && (BYTE*)h != g_base) {
            logf("[fmoime] re-arming: FrontMissionOnline.dll re-loaded at a new base");
            g_base = (BYTE*)h;
            g_applied = g_said_packed = g_said_odd = 0;
        }

        int done = 0;
        for (int i = 0; i < N_SITES; i++)
            done += apply_site(g_base, &g_sites[i]);

        if (done != g_applied) {
            g_applied = done;
            if (g_applied == N_SITES) {
                logf("[fmoime] text fields now open in DIRECT input -- typing "
                     "produces English without touching the IME. The IME hotkey "
                     "still works inside a field; that choice is no longer "
                     "remembered for the next one. Set [dx] fmo_ime_direct=0 to "
                     "restore SE's behaviour.");
                log_flush();
            }
        }
        Sleep(1000);
    }
}

void fmoime_configure(const wchar_t* ini)
{
    // WARNING: TWO DEFAULTS PER SETTING: this literal AND the iniheal row must agree,
    // or a stale install heals into behaviour a fresh one never has
    // (the ini-defaults check gates exactly that). The row is in
    // iniheal.cpp under [dx], same default.
    g_on = GetPrivateProfileIntW(L"dx", L"fmo_ime_direct", 1, ini);
    if (!g_on) return;
    if (!g_thread)
        g_thread = CreateThread(NULL, 0, ime_watch, NULL, 0, NULL);
}

// Live-reload for the in-game settings dialog. fmoime_configure is already
// re-entrant: it re-reads g_on, and the `if (!g_thread)` guard means a second
// call can never spawn a duplicate watcher -- so OFF -> ON live is exactly a
// re-call (the one place a reload is allowed to create a thread, because the
// configure path it delegates to already owns that thread's one-shot guard).
// ON -> OFF stays RESTART-BOUND on purpose: the watcher never re-reads g_on,
// and the bytes already written into FrontMissionOnline.dll stay patched --
// flipping the switch off mid-session cannot un-patch anything.
void fmoime_reload(const wchar_t* ini)
{
    int was = g_on;
    fmoime_configure(ini);
    logf("[reload] fmoime: fmo_ime_direct=%d%s", g_on,
         (was && !g_on) ? " (was on -- the patch stays until restart)" : "");
}

// Signal-only, no join: called from the detach path with the loader lock held,
// the same rule as fmokey_stop and maskguard_stop.
void fmoime_stop(void) { InterlockedExchange(&g_stop, 1); }

void fmoime_summary(void)
{
    if (!g_on) { logf("[fmoime] off ([dx] fmo_ime_direct=0)"); return; }
    if (!g_base)
        logf("[fmoime] armed, but FrontMissionOnline.dll never loaded this session");
    else
        logf("[fmoime] %d/%d IME restore sites patched", g_applied, N_SITES);
}
