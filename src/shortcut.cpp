// shortcut.cpp -- synthesise the Viewer's "shortcut" (direct game boot) for a
// configured title, UNLESS a real shortcut was actually sent to the Viewer.
//
// WHAT THE VIEWER'S SHORTCUT IS
//
// The PlayOnline Viewer boots straight into a game when it sees `/game <token>`
// on its command line. That is the ONLY thing a title's polboot.exe adds over a
// plain Viewer launch: it spawns `pol.exe /game <token>` (measured live -- the
// TetraMaster shortcut launches `"...\pol.exe" /game DdcMLbB`). pol.exe's argv
// scan (pol.exe+0x14710) matches the wide literal "/game", copies the following
// token verbatim into its shortcut buffer at 0x45cb20, and carries it downstream
// through polcore, which boots the title the token names. pol.exe treats the
// token OPAQUELY -- it never decodes it -- so all we have to do to make the
// Viewer behave as if launched from a title's shortcut is put `/game <token>`
// on the command line its CRT parses.
//
// THE TOKEN TABLE IS BAKED INTO polboot.exe
//
// Every top-level polboot.exe carries, in plaintext .data, a pointer array that
// indexes an 8-byte-slot token table where the array position IS the content id.
// The FFXI / FMO / FFXI-Test polboots hold the full 22-entry table (ids 0-21);
// it is byte-identical across every polboot and across US/EU/JP. Extracted and
// cross-checked against the Viewer's own content-id table (data/doc/sqpolcts.bin
// says 1=FFXI, 2=Tetra Master, 4=Front Mission Online, 11=Fantasy Earth, ...).
// So token = TABLE[content_id]; the constants below ARE that table.
//
// THE GUARD -- "unless one was actually sent"
//
// If the real command line already carries "/game" (a genuine polboot launch),
// we pass it through untouched: the real shortcut always wins. The Viewer's OTHER
// shortcut trigger -- exiting back to a resumable game -- is a within-session
// state that a launch-time command line cannot conflict with, so it needs no
// guard here.
//
// THE SEAM
//
// pol.exe imports kernel32!GetCommandLineW (its CRT builds __wargv from it; the
// argv scan compares WIDE, so it is the W variant that matters). We IAT-patch
// that one import on the main module and return an augmented command line. The
// shim's startup() runs from DllMain(PROCESS_ATTACH) -- for the PolHook static-
// import delivery that is during loader init, BEFORE pol.exe's entry point and
// therefore before the CRT parses argv -- so the hook is in place in time. Same
// timing inputmode_apply() relies on. Nothing on disk or in the registry is
// touched; clearing [shortcut] game reverts it.
//
// LIVENESS: the value is consumed once, at launch, so it is restart-bound
// (g_restart_keys RB_ALWAYS in polsettings.cpp). The settings dialog can save it
// live, but the shortcut only materialises on the next Viewer launch.

#include "polshim.h"

// content_id -> /game token, straight out of polboot.exe's baked table.
// Index is the content id. Empty entries are ids the Viewer never launches as a
// game (mail services, reserved slots) but are kept so the array index stays the
// content id. See pol-content-ids for the title each id names.
static const char* const TOKENS[] = {
    /* 0 undefined      */ "",
    /* 1 FFXI           */ "eAZcFcB",
    /* 2 Tetra Master   */ "DdcMLbB",
    /* 3 Mahjong        */ "TEDUNGC",
    /* 4 FMO            */ "NcEIDL",
    /* 5 mail forward   */ "",
    /* 6 charname addr  */ "",
    /* 7 mail filter    */ "",
    /* 8 (unallocated)  */ "",
    /* 9 (unallocated)  */ "",
    /* 10 Dirge/FFVII   */ "WPMXQWB",
    /* 11 Fantasy Earth */ "EcdCVMB",
    /* 12 TEMP_OS_012   */ "",
    /* 13 EverQuest II  */ "OTVRAFB",
    /* 14 Friend List   */ "ecMBZE",
    /* 15 FFXI Test     */ "SUULOPB",
};
static const int TOKENS_N = (int)(sizeof(TOKENS) / sizeof(TOKENS[0]));

// A few friendly names accepted in place of the numeric content id.
struct Alias { const wchar_t* name; int cid; };
static const Alias ALIASES[] = {
    { L"ff11", 1 }, { L"ffxi", 1 },
    { L"tetra", 2 }, { L"tm", 2 }, { L"tetramaster", 2 },
    { L"mahjong", 3 }, { L"jan", 3 }, { L"janhourou", 3 },
    { L"fmo", 4 }, { L"frontmission", 4 },
    { L"doc", 10 }, { L"dirge", 10 },
    { L"fe", 11 }, { L"fantasyearth", 11 },
    { L"eq2", 13 }, { L"everquest", 13 },
    { L"fl", 14 }, { L"friendlist", 14 },
    { L"ffxitest", 15 }, { L"testclient", 15 },
};

static int   g_cid   = 0;                 // 0 = off
static char  g_token[16] = "";            // resolved token (ASCII)
static wchar_t g_augmented[2048];         // storage the CRT reads its cmdline from

typedef LPWSTR (WINAPI *PFN_GCLW)(void);
static PFN_GCLW real_GetCommandLineW = NULL;
static LONG     g_reported = 0;

// Case-insensitive: does the command line already carry a "/game" switch? Written
// out to avoid a shlwapi link dependency for one call (as gamestart.cpp does).
static bool has_game_switch(const wchar_t* cl)
{
    if (!cl) return false;
    static const wchar_t* NEEDLE = L"/game";
    for (const wchar_t* s = cl; *s; s++) {
        const wchar_t* a = s; const wchar_t* b = NEEDLE;
        while (*a && *b && towlower(*a) == towlower(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static LPWSTR WINAPI hook_GetCommandLineW(void)
{
    LPWSTR orig = real_GetCommandLineW ? real_GetCommandLineW() : GetCommandLineW();

    // The guard: a real shortcut was sent (genuine polboot launch) -> hands off.
    if (has_game_switch(orig)) {
        if (InterlockedCompareExchange(&g_reported, 1, 0) == 0)
            logf("[shortcut] a real /game was already on the command line -- "
                 "leaving it untouched (real shortcut wins)");
        return orig;
    }

    _snwprintf_s(g_augmented, _countof(g_augmented), _TRUNCATE,
                 L"%s /game %S", orig ? orig : L"", g_token);
    if (InterlockedCompareExchange(&g_reported, 1, 0) == 0)
        logf("[shortcut] synthesised /game %S (content %d) -> \"%ls\"",
             g_token, g_cid, g_augmented);
    return g_augmented;
}

// Patch one named import on the main module (pol.exe). Returns the original
// function through old_out. Minimal, self-contained IAT walk.
static bool patch_main_import(const char* fn_want, void* newfn, void** old_out)
{
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_DATA_DIRECTORY imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.VirtualAddress || !imp.Size) return false;

    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp.VirtualAddress);
    for (; desc->Name; desc++) {
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        IMAGE_THUNK_DATA* intt = desc->OriginalFirstThunk
                               ? (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk)
                               : iat;   // no INT: names come through the IAT itself
        for (; intt->u1.AddressOfData; intt++, iat++) {
            if (intt->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;   // imported by ordinal
            IMAGE_IMPORT_BY_NAME* nm = (IMAGE_IMPORT_BY_NAME*)(base + intt->u1.AddressOfData);
            if (strcmp((const char*)nm->Name, fn_want) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            if (old_out) *old_out = (void*)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)newfn;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void shortcut_configure(const wchar_t* ini)
{
    g_cid = 0; g_token[0] = 0;

    wchar_t v[32] = L"";
    ini_str(L"shortcut", L"game", L"", v, _countof(v), ini);

    // Disabled values.
    if (!v[0] || !_wcsicmp(v, L"0") || !_wcsicmp(v, L"off") || !_wcsicmp(v, L"none"))
        return;

    // Numeric content id, or a friendly alias.
    if (iswdigit(v[0])) {
        g_cid = (int)wcstol(v, NULL, 10);
    } else {
        for (const Alias& a : ALIASES)
            if (!_wcsicmp(v, a.name)) { g_cid = a.cid; break; }
    }

    if (g_cid <= 0 || g_cid >= TOKENS_N || !TOKENS[g_cid][0]) {
        logf("[shortcut] game=\"%ls\" is not a launchable content id -- ignored "
             "(known: 1 FFXI, 2 Tetra Master, 3 Mahjong, 4 FMO, 10 Dirge, "
             "11 Fantasy Earth, 13 EQII, 14 Friend List, 15 FFXI Test)", v);
        g_cid = 0;
        return;
    }
    strcpy_s(g_token, sizeof(g_token), TOKENS[g_cid]);

    void* orig = NULL;
    if (patch_main_import("GetCommandLineW", (void*)hook_GetCommandLineW, &orig)) {
        real_GetCommandLineW = (PFN_GCLW)orig;
        logf("[shortcut] armed: content %d (token %s) will be offered as the "
             "startup shortcut unless a real /game is passed", g_cid, g_token);
    } else {
        logf("[shortcut] could NOT patch pol.exe's GetCommandLineW import -- "
             "shortcut not armed (content %d)", g_cid);
        g_cid = 0; g_token[0] = 0;
    }
}
