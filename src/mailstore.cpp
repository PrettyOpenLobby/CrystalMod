// mailstore.cpp -- make sure the Viewer's per-profile mail directories exist.
//
// PlayOnline Mail writes every received message into a per-profile store under
// <viewer>\usr\home<NN>\EMAIL. The Viewer's installer lays that out for slot 00
// only; a profile slot created later gets `download\` and `gmtool\` and nothing
// else. The mail code then does NOT create the directory it is about to write
// into -- its file layer has a create-directory helper (app.dll RVA 0x10075,
// which even normalises '/' to '\'), but the receive path never calls it. It
// goes straight to store->open("EMAIL/<name>", 0x3002), which fails.
//
// Failing there is invisible, because SE's code loses the failure three times:
//
//     app.dll+0x11462D   movzx eax, al          ; truncate open()'s result
//     app.dll+0x114632   mov [wnd+0x66D5C], eax ; store THAT as the last error
//     app.dll+0x114638   jge  ...               ; always taken -- and the value
//                                               ; is a HANDLE, not a status, so
//                                               ; this check could never work
//     app.dll+0x1146D1   push 0x21              ; "store not open" -> the error
//                                               ; step, WITHOUT setting a code
//     app.dll+0x4968286  push 6                 ; ...which, 101 ticks later,
//                                               ; displays the never-set field
//
// The user gets POL-0000 -- a code that does not exist in polerr.bin -- for what
// is really a missing directory. Measured 2026-08-12.
//
// So: create them at inject time. This is the only automatic remedy available,
// because the failure return is 0 rather than negative -- no byte patch can make
// SE's check work. Empty directories only; no files are written, nothing is
// deleted, and an existing directory is left exactly as it is.
//
// Note the deployment limit: this only runs when the Viewer is started through
// polshim_launch.exe. For an ordinary launch the fix is
// to create the directory by hand, which does the same thing.
#define WIN32_LEAN_AND_MEAN
#include "polshim.h"
#include <windows.h>

// Slot 00's layout, which is what the installer produces and what the mail code
// expects. Parent-first: each entry is created in order, so no recursive mkdir.
static const wchar_t* const USR_LEAVES[] = {
    L"EMAIL", L"EMAIL\\TMP", L"nf"
};
static const wchar_t* const PUB_LEAVES[] = {
    L"EMAIL",
    L"mail", L"mail\\adr", L"mail\\uidl",
    L"mail\\r", L"mail\\r\\a", L"mail\\r\\b",
    L"mail\\s", L"mail\\s\\a", L"mail\\s\\b",
    L"msg",  L"msg\\r",  L"msg\\r\\a",  L"msg\\r\\b",
    L"msg\\s", L"msg\\s\\a", L"msg\\s\\b",
    // open\ScreenShots -- NOT a mail directory, and it hangs FRONT MISSION ONLINE
    // outright rather than producing an error code.
    //
    // Measured 2026-08-14: FMO asks polcore to enumerate
    //   <viewer>\pub\home<NN>\open\ScreenShots\*
    // through the async request table (common function table slots 1134/1135) and
    // then busy-waits on the result with no Sleep and no message pump. When the
    // directory does not exist the request never reaches a terminal status, so
    // the poll returns "in progress" forever and FMO spins a full core --
    // 3,161,856 polls in 20 seconds, window Not Responding, no frame ever drawn.
    // See [pf] in polfetch.cpp, which is the safety net for the same failure.
    //
    // Same root cause as the mail directories above, which is why it belongs
    // here: the installer lays out slot 00, and a profile created later comes out
    // incomplete. Measured on this install -- home00 and home02 both had
    // open\ScreenShots, home01 (the one in use) did not.
    L"open", L"open\\ScreenShots"
};

static int g_fix = 1;          // create, rather than only report
static int g_created = 0;
static int g_missing = 0;

static bool is_dir(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void ensure(const wchar_t* base, const wchar_t* slot, const wchar_t* leaf,
                   const wchar_t* full)
{
    if (is_dir(full)) return;
    g_missing++;
    char b[16], s[64], l[64];
    WideCharToMultiByte(CP_ACP, 0, base, -1, b, sizeof(b), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, slot, -1, s, sizeof(s), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, leaf, -1, l, sizeof(l), NULL, NULL);
    if (!g_fix) {
        logf("[mailstore] MISSING %s\\%s\\%s -- mail Receive will fail with "
             "POL-0000 (mail_store_fix=0, not creating it)", b, s, l);
        return;
    }
    if (CreateDirectoryW(full, NULL)) {
        g_created++;
        logf("[mailstore] created %s\\%s\\%s", b, s, l);
    } else {
        logf("[mailstore] FAILED to create %s\\%s\\%s (err %lu) -- mail Receive "
             "will fail with POL-0000", b, s, l, GetLastError());
    }
}

// Walk one root (usr or pub) and populate every home<NN> slot it already has.
// Only slots that exist are touched: creating pub\home07 because usr\home07
// exists would invent a profile the Viewer never made.
static void sweep_root(const wchar_t* viewer, const wchar_t* base,
                       const wchar_t* const* leaves, int nleaves)
{
    wchar_t pattern[MAX_PATH];
    // _snwprintf_s(_TRUNCATE), not wsprintfW: wsprintfW ignores the buffer size and a
    // deep install path overruns these MAX_PATH stack buffers. Truncation just fails
    // the match/create; a stack smash at inject time is a crash in pol.exe.
    _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%s\\%s\\home*", viewer, base);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        for (int i = 0; i < nleaves; i++) {
            wchar_t full[MAX_PATH];
            _snwprintf_s(full, _countof(full), _TRUNCATE, L"%s\\%s\\%s\\%s",
                         viewer, base, fd.cFileName, leaves[i]);
            ensure(base, fd.cFileName, leaves[i], full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void mailstore_preflight(const wchar_t* ini)
{
    g_fix = GetPrivateProfileIntW(L"polshim", L"mail_store_fix", 1, ini);

    // The Viewer is our host process, so its directory is exact -- better than
    // the registry, which can point at a different install than the running one.
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *slash = 0;

    // Only act on something that really is a Viewer install.
    wchar_t probe[MAX_PATH];
    _snwprintf_s(probe, _countof(probe), _TRUNCATE, L"%s\\usr", exe);
    if (!is_dir(probe)) return;

    g_created = 0;
    g_missing = 0;
    sweep_root(exe, L"usr", USR_LEAVES, _countof(USR_LEAVES));
    sweep_root(exe, L"pub", PUB_LEAVES, _countof(PUB_LEAVES));

    if (g_missing)
        logf("[mailstore] %d missing mail directory(ies), %d created",
             g_missing, g_created);
    log_flush();
}
