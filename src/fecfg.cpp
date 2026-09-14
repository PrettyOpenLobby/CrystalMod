// fecfg.cpp -- reconcile Fantasy Earth's GLOBAL.INI with its UAC VirtualStore shadow.
//
// THE BUG THIS EXISTS FOR, measured live on the reference Windows install 2026-08-25.
//
// FantasyEarthConfig.exe writes ...\FantasyEarth\Settings\GLOBAL.INI. Program Files
// is not writable unelevated, so a 32-bit legacy binary's write is REDIRECTED by UAC
// to %LOCALAPPDATA%\VirtualStore\Program Files (x86)\...\GLOBAL.INI. The utility
// reports success and the file it wrote is real -- it is simply not the file the
// game reads, because pol.exe runs elevated (a RUNASADMIN compat flag), and an
// ELEVATED process gets no redirection: it reads the real path in Program Files.
//
// Writer and reader end up on opposite sides of the redirect, and both halves look
// like they worked. Six settings had silently diverged before anyone noticed:
//
//     setting             install tree      VirtualStore (what the util wrote)
//     SCREEN_WIDTH        800               1600
//     SCREEN_HEIGHT       600               1200
//     REFLECT_ENABLE      0                 1
//     REFLECT_LEVEL       0                 2
//     MOUSE_SENSITIVITY   14                50
//     OPENING_MOVIE       1                 0
//
// The user had disabled the opening movie and it kept playing; the resolution
// row is the proof of which copy FE reads (it ran at 800x600, the install value).
//
// WHY NOT JUST ELEVATE THE CONFIG TOOLS. Because the fault is not "the tool was
// unelevated", it is "the two sides disagree". Shipping a RUNASADMIN compat flag for
// FantasyEarthConfig.exe is only correct where pol.exe is ALSO elevated -- otherwise
// it produces this same bug mirrored, with the tool writing the real file and the
// game reading a stale shadow. It also adds a UAC prompt, and it does nothing at all
// under Proton, where there is no UAC and no redirection. Reconciling covers every
// combination and is a natural no-op where no shadow exists.
//
// WHAT THIS DOES. If a shadow exists, the NEWER of the two files wins, its [GLOBAL]
// values are written into the path FE actually reads, and the shadow is deleted so
// there is one source of truth again. Every field moved is logged by name and value:
// a silent repair of someone else's config file would be worse than the bug.
//
// WARNING: OFF BY DEFAULT ([fecfg] enable=0). This writes a TITLE's configuration, not
// ours, and it ships to everyone. The last migration that turned a knob on for every
// install broke Fantasy Earth on Windows (fmv_skip=4, 2026-08-23). One release
// opt-in, then reconsider the default.
#include "polshim.h"
#include <stdio.h>
#include <stdlib.h>   // atoi, for the OPENING_MOVIE value

static int g_enable = 0;
static int g_skipmovie = 1;                // [fecfg] skip_opening_movie
static wchar_t g_path[MAX_PATH] = L"";     // [fecfg] path -- override FE's folder

void fecfg_configure(const wchar_t* ini)
{
    g_enable = GetPrivateProfileIntW(L"fecfg", L"enable", 0, ini);
    g_skipmovie = GetPrivateProfileIntW(L"fecfg", L"skip_opening_movie", 1, ini);
    ini_str(L"fecfg", L"path", L"", g_path, _countof(g_path), ini);
}

// FE's folder, from [fecfg] path or the same InstallFolder\0011 value the Viewer's
// own menu launches the title from (see regfix.cpp -- 0011 is Fantasy Earth).
static bool fe_folder(wchar_t* out, size_t n)
{
    if (g_path[0]) { wcscpy_s(out, n, g_path); return true; }
    static const wchar_t* hives[] = {
        L"SOFTWARE\\PlayOnlineUS", L"SOFTWARE\\PlayOnline", L"SOFTWARE\\PlayOnlineEU"
    };
    for (int i = 0; i < 3; i++) {
        wchar_t sub[256];
        _snwprintf_s(sub, _countof(sub), _TRUNCATE, L"%s\\InstallFolder", hives[i]);
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &h) != ERROR_SUCCESS)
            continue;
        wchar_t val[MAX_PATH]; DWORD type = 0, sz = sizeof(val) - sizeof(wchar_t);
        LONG r = RegQueryValueExW(h, L"0011", NULL, &type, (LPBYTE)val, &sz);
        RegCloseKey(h);
        if (r != ERROR_SUCCESS || sz < 2) continue;
        val[sz / sizeof(wchar_t)] = 0;
        size_t l = wcslen(val);
        while (l && (val[l-1] == L'\\' || val[l-1] == L'/')) val[--l] = 0;
        if (!l) continue;
        wcscpy_s(out, n, val);
        return true;
    }
    return false;
}

// %LOCALAPPDATA%\VirtualStore\<real path minus its "X:\" prefix>. That is exactly
// how UAC composes it, so this finds the file the config utility actually wrote.
static bool shadow_of(const wchar_t* real, wchar_t* out, size_t n)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, _countof(base)) || !base[0])
        return false;
    const wchar_t* tail = real;
    if (tail[0] && tail[1] == L':' && (tail[2] == L'\\' || tail[2] == L'/')) tail += 3;
    _snwprintf_s(out, n, _TRUNCATE, L"%s\\VirtualStore\\%s", base, tail);
    return true;
}

static bool file_time(const wchar_t* p, FILETIME* ft)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return false;
    *ft = d.ftLastWriteTime;
    return true;
}

// Read a whole file as bytes. GLOBAL.INI is a few hundred bytes of ASCII.
static char* slurp(const wchar_t* p, DWORD* len)
{
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    if (sz == INVALID_FILE_SIZE || sz > 1u << 20) { CloseHandle(h); return NULL; }
    char* buf = (char*)LocalAlloc(LPTR, sz + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    ReadFile(h, buf, sz, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

// Value for KEY= in an ASCII ini body. FE parses this file with its OWN reader, not
// GetPrivateProfileString, so we match on plain lines rather than going through the
// profile API -- and, more importantly, we EDIT IN PLACE below for the same reason:
// rewriting the file through WritePrivateProfileString would reformat a file whose
// only consumer is a hand-rolled parser.
static bool ini_value(const char* body, const char* key, char* out, size_t n)
{
    size_t kl = strlen(key);
    for (const char* p = body; *p; ) {
        const char* eol = strpbrk(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > kl && _strnicmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vl = ll - kl - 1;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return true;
        }
        if (!eol) break;
        p = eol + (eol[0] == '\r' && eol[1] == '\n' ? 2 : 1);
    }
    return false;
}

// ============================================================================
// Fantasy Earth's opening movie, off by default -- using SE'S OWN SWITCH
//
// IMPORTANT: FIRST, THE CORRECTION THIS KEEPS HAVING TO MAKE: **there is no registry
// option for this.** Fantasy Earth keeps NOTHING in the registry -- its key
// exists and is empty -- and every one of its 18 settings lives in
// `<install>\Settings\GLOBAL.INI` under `[GLOBAL]`. The movie is
// `OPENING_MOVIE`, 1 = play. Anything that claims to cover "each game's
// settings" by reading HKLM has silently omitted this whole title
// ([[per-game-config-surfaces]]).
//
// WHY SE'S SWITCH AND NOT OURS. The shim has `[dx] fmv_skip`, which interferes
// with the DirectShow filter graph. It was turned on for every install once
// (rev 6, 2026-08-23) and BROKE Fantasy Earth on Windows, because level 4 makes
// the title take a no-movie path that leaves an object NULL. It is also Wine-
// gated, so it did nothing on Windows for months while the dialog said it was
// on ([[shim-fmv-skip-was-wine-gated-process-wide]]). The standing rule from
// that episode is exactly this: **prefer the switch SE already shipped; ours is
// the last resort** ([[match-the-original-not-a-stopgap]]).
//
// KEY: SEED ONCE, NEVER OVERRIDE. This writes somebody else's config file, so it
// fires at most once per install and records that it did, in our own ini. A
// player who turns the movie back on afterwards keeps it on for ever -- the
// same argument seed_pad makes in regredir.cpp, and the reason that one is
// safe. "Default" here means "what a fresh install gets", not "what we enforce".
//
// ON THE UAC REDIRECT. FE runs INSIDE this same pol.exe (FE_Client.dll), so if
// our write is virtualised into the VirtualStore, FE's read is virtualised to
// the very same file -- the setting still takes effect. It matters only if the
// process's elevation later changes, which is the divergence fecfg_run below
// exists to repair, so that case is detected and named rather than assumed away.
// (Reading the file back would NOT detect it: a redirected read confirms a
// redirected write -- [[round-trip-is-not-a-measurement]].)
// ============================================================================
static void fe_skip_opening_movie(const wchar_t* ini)
{
    if (!g_skipmovie) return;
    if (GetPrivateProfileIntW(L"fecfg", L"movie_seeded", 0, ini)) return;

    wchar_t dir[MAX_PATH];
    if (!fe_folder(dir, _countof(dir))) return;   // FE is not installed here
    wchar_t real[MAX_PATH];
    _snwprintf_s(real, _countof(real), _TRUNCATE, L"%s\\Settings\\GLOBAL.INI", dir);

    DWORD nr = 0;
    char* body = slurp(real, &nr);
    if (!body) {
        logf("[fecfg] Fantasy Earth's GLOBAL.INI is not readable at %ls -- the "
             "opening movie is left alone (this is normal if FE has never been "
             "run; its config app writes the file).", real);
        return;
    }

    char cur[64] = "";
    bool have = ini_value(body, "OPENING_MOVIE", cur, sizeof(cur));
    if (have && atoi(cur) == 0) {
        // Already what we want. Record it, so we never look again -- and so a
        // later change by the player is never second-guessed.
        logf("[fecfg] Fantasy Earth's opening movie is already off "
             "(OPENING_MOVIE=%s in %ls)", cur, real);
        WritePrivateProfileStringW(L"fecfg", L"movie_seeded", L"1", ini);
        LocalFree(body);
        return;
    }
    if (!have) {
        logf("[fecfg] GLOBAL.INI has no OPENING_MOVIE line -- not adding one. "
             "FE's own reader owns this file's shape; its config app writes "
             "the key.");
        LocalFree(body);
        return;
    }

    // Rewrite that ONE line in place. Same reasoning as the reconcile below:
    // FE parses this file with its own reader, so ordering, spacing and line
    // endings stay exactly as FE's own writer produced them.
    char* out = (char*)LocalAlloc(LPTR, nr + 64);
    if (!out) { LocalFree(body); return; }
    size_t olen = 0; bool done = false;
    for (const char* p = body; *p; ) {
        const char* eol = strpbrk(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        size_t adv = ll + (eol ? ((eol[0] == '\r' && eol[1] == '\n') ? 2 : 1) : 0);
        if (!done && ll > 14 && _strnicmp(p, "OPENING_MOVIE=", 14) == 0) {
            olen += sprintf(out + olen, "OPENING_MOVIE=0%s",
                            (eol && eol[0] == '\r') ? "\r\n" : (eol ? "\n" : ""));
            done = true;
        } else {
            memcpy(out + olen, p, adv); olen += adv;
        }
        if (!eol) break;
        p = eol + (adv - ll);
    }

    // Was a VirtualStore shadow there before we wrote? If one appears (or its
    // timestamp moves) our write was redirected.
    wchar_t shadow[MAX_PATH]; FILETIME sb; bool had_shadow = false;
    bool have_shadow_path = shadow_of(real, shadow, _countof(shadow));
    if (have_shadow_path) had_shadow = file_time(shadow, &sb);

    HANDLE h = CreateFileW(real, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        logf("[fecfg] cannot write %ls (error %lu) -- Fantasy Earth's opening "
             "movie is LEFT ON. Nothing was changed.", real, GetLastError());
        LocalFree(out); LocalFree(body);
        return;
    }
    DWORD wrote = 0;
    WriteFile(h, out, (DWORD)olen, &wrote, NULL);
    CloseHandle(h);

    FILETIME sa;
    bool now_shadow = have_shadow_path && file_time(shadow, &sa);
    bool redirected = now_shadow && (!had_shadow || CompareFileTime(&sa, &sb) != 0);

    logf("[fecfg] Fantasy Earth's opening movie turned OFF using SE's own "
         "setting: OPENING_MOVIE %s -> 0 in %ls%s", cur, real,
         redirected ? "  (our write was VIRTUALISED into the VirtualStore -- FE "
                      "runs in this same process so it reads that same copy and "
                      "the setting DOES take effect, but the install tree still "
                      "says otherwise; set [fecfg] enable=1 to reconcile them)"
                    : "");
    logf("[fecfg] this is done ONCE per install. Turn the movie back on in "
         "Fantasy Earth's own config app and it stays on -- we will not look "
         "again. ([fecfg] skip_opening_movie=0 to skip this entirely.)");
    WritePrivateProfileStringW(L"fecfg", L"movie_seeded", L"1", ini);

    LocalFree(out); LocalFree(body);
}

void fecfg_run(const wchar_t* ini)
{
    // Independent of [fecfg] enable. That gate gates the RECONCILE, which moves
    // six of somebody's settings between two files; this writes one value we
    // ship a default for, once, and says so.
    fe_skip_opening_movie(ini);

    if (!g_enable) return;

    wchar_t dir[MAX_PATH];
    if (!fe_folder(dir, _countof(dir))) {
        logf("[fecfg] Fantasy Earth's folder is not in the registry and [fecfg] path= "
             "is empty -- nothing to reconcile.");
        return;
    }
    wchar_t real[MAX_PATH], shadow[MAX_PATH];
    _snwprintf_s(real, _countof(real), _TRUNCATE, L"%s\\Settings\\GLOBAL.INI", dir);
    if (!shadow_of(real, shadow, _countof(shadow))) return;

    FILETIME tr, ts;
    bool has_real = file_time(real, &tr), has_shadow = file_time(shadow, &ts);
    if (!has_shadow) return;                 // the normal case, and under Wine always
    if (!has_real) {
        logf("[fecfg] a VirtualStore copy of GLOBAL.INI exists but the real file does "
             "not (%ls) -- leaving both alone.", real);
        return;
    }

    // Which is authoritative: whichever was written last. The config utility's write
    // is the user's latest intent regardless of which side it landed on.
    bool shadow_wins = CompareFileTime(&ts, &tr) > 0;
    DWORD nr = 0, ns = 0;
    char* body_real = slurp(real, &nr);
    char* body_shadow = slurp(shadow, &ns);
    if (!body_real || !body_shadow) {
        if (body_real) LocalFree(body_real);
        if (body_shadow) LocalFree(body_shadow);
        logf("[fecfg] could not read one of the two GLOBAL.INI copies -- leaving both alone.");
        return;
    }

    if (!shadow_wins) {
        logf("[fecfg] a stale VirtualStore copy of GLOBAL.INI shadows the real file. "
             "The real file is NEWER, so it wins; the shadow is removed so an "
             "unelevated read cannot resurrect it. (%ls)", shadow);
        LocalFree(body_real); LocalFree(body_shadow);
        if (!DeleteFileW(shadow))
            logf("[fecfg] could not delete the shadow (error %lu) -- it will keep "
                 "shadowing any unelevated reader.", GetLastError());
        return;
    }

    // The shadow is newer: the utility wrote there and FE never saw it. Move every
    // differing [GLOBAL] key into the real file, IN PLACE, and name each one.
    logf("[fecfg] FantasyEarthConfig wrote to the UAC VirtualStore and Fantasy Earth "
         "reads the install tree -- the two had diverged. Moving the newer values "
         "into %ls:", real);

    // Rebuild the real file line by line so ordering, spacing and line endings are
    // whatever FE's own writer produced. Only the VALUE of a differing key changes.
    char* out = (char*)LocalAlloc(LPTR, nr + ns + 64);
    if (!out) { LocalFree(body_real); LocalFree(body_shadow); return; }
    size_t olen = 0; int moved = 0;
    for (const char* p = body_real; *p; ) {
        const char* eol = strpbrk(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        size_t adv = ll + (eol ? ((eol[0] == '\r' && eol[1] == '\n') ? 2 : 1) : 0);
        const char* eq = (const char*)memchr(p, '=', ll);
        bool rewrote = false;
        if (eq && eq > p) {
            char key[128] = "", oldv[128] = "", newv[128] = "";
            size_t kl = (size_t)(eq - p);
            if (kl < sizeof(key)) {
                memcpy(key, p, kl); key[kl] = 0;
                size_t vl = ll - kl - 1;
                if (vl < sizeof(oldv)) { memcpy(oldv, eq + 1, vl); oldv[vl] = 0; }
                if (ini_value(body_shadow, key, newv, sizeof(newv)) &&
                    strcmp(oldv, newv) != 0) {
                    olen += sprintf(out + olen, "%s=%s%s", key, newv,
                                    (eol && eol[0] == '\r') ? "\r\n" : "\n");
                    logf("[fecfg]   %-20s %s -> %s", key, oldv, newv);
                    moved++;
                    rewrote = true;
                }
            }
        }
        if (!rewrote) { memcpy(out + olen, p, adv); olen += adv; }
        if (!eol) break;
        p = eol + (adv - ll);
    }

    if (!moved) {
        logf("[fecfg]   (no differing values -- the shadow is simply newer)");
    }

    // WARNING: WRITE, THEN PROVE THE WRITE LANDED WHERE WE MEANT IT. If the shim itself is
    // not elevated, OUR write gets virtualised too -- into the very shadow we are
    // trying to retire -- and reading it back would confirm a write that changed
    // nothing for FE. So: write, then check the shadow's timestamp. If it moved, we
    // were redirected; say so and keep the shadow rather than deleting the only copy
    // that has the user's values.
    FILETIME before = ts;
    HANDLE h = CreateFileW(real, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        logf("[fecfg] cannot write %ls (error %lu) -- the shim is not elevated for "
             "that tree, so the values stay in the VirtualStore and Fantasy Earth "
             "keeps reading the old ones. Nothing was changed or deleted.",
             real, GetLastError());
    } else {
        DWORD wrote = 0;
        WriteFile(h, out, (DWORD)olen, &wrote, NULL);
        CloseHandle(h);
        FILETIME after;
        bool moved_shadow = file_time(shadow, &after) &&
                            CompareFileTime(&after, &before) != 0;
        if (moved_shadow) {
            logf("[fecfg] our own write was REDIRECTED into the VirtualStore (the "
                 "shadow's timestamp changed) -- the shim is not elevated for that "
                 "tree. Fantasy Earth still reads the old values; shadow kept.");
        } else if (DeleteFileW(shadow)) {
            logf("[fecfg] %d value(s) moved; VirtualStore copy deleted. One source of "
                 "truth again: %ls", moved, real);
        } else {
            logf("[fecfg] %d value(s) moved, but the VirtualStore copy could not be "
                 "deleted (error %lu) -- it will shadow any UNELEVATED reader.",
                 moved, GetLastError());
        }
    }
    LocalFree(out); LocalFree(body_real); LocalFree(body_shadow);
}
