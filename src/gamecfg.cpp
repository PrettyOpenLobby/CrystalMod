// gamecfg.cpp -- open each title's OWN settings app, and DIFF what the titles store.
//
// THE RULE THIS FILE EXISTS TO ENFORCE:
//
//     WHERE SE ALREADY SHIPPED A SETTINGS SCREEN, OPEN SE'S SETTINGS SCREEN.
//
// This file briefly did something else. It grew a table of every value each title
// keeps, drew them as ~130 controls, and wrote them back into the titles' registry
// keys and ini files. That was removed on 2026-08-26, and the reason is worth keeping
// because it is the same reason the rule above exists:
//
//   * It was a SECOND settings UI for settings that already had one, so every value
//     had two editors that had to agree about hives, types, UAC virtualization and
//     defaults. Each of those was a real bug found and fixed here -- work that only
//     existed because we had reimplemented somebody else's window.
//   * It could not label what it showed. FE's quality levels are named inside
//     FE_Client.dll and FFXI's settings are stored as 0000..0043, both unreachable
//     statically (POL1-packed, and a "%04d" mapped in the config tool's own code), so
//     the screen showed numbers with no scale and no meaning. Reported, correctly, as
//     "It feels like a developer menu with its language."
//   * SE's own tools have the labels, the valid ranges and the restricted lists,
//     because they were written by the people who wrote the games.
//
// So what is left is a launcher and a diagnostic:
//
//   gamecfg_configapp*  -- find and open a title's own config tool, started in its own
//                          folder (they resolve their files relative to the working
//                          directory) and inheriting the elevated Viewer's token, which
//                          is what keeps their writes out of the per-user VirtualStore.
//   gamecfg_report      -- a read-only DIFF of everything the titles store, for
//                          identifying a value by changing it in the real tool and
//                          seeing which number moved. This is how FFXI's numbered
//                          settings would ever be decoded; it is not a settings screen.
//
// IMPORTANT: THE KEY TREE IS WALKED, NOT NAMED. A hand-written table of publisher spellings
// missed five keys on a live enumeration (2026-08-20): a THIRD spelling
// `PlayOnline\SQUARE ENIX\FrontMissionOnline` (caps and a space), FantasyEarth and
// PlayOnlineFriendList under `SquareEnix`, FinalFantasyXITestClient, and the Viewer's
// own `Settings` / `Settings\Controller` SUBKEYS, which the walk never recursed into.
// Only the three ROOTS are named anywhere in this file.
#include "polshim.h"
#include <stdio.h>
#include <shellapi.h>   // ShellExecuteExW -- open a title's own config app


// SOFTWARE\<root>\<publisher>\<...>. Only the ROOTS are named -- everything below them is
// enumerated. HKLM here is the 32-bit view: the shim is x86 inside pol.exe, so Windows
// redirects these to WOW6432Node itself, and naming that node explicitly would break the
// same code under a 32-bit OS or a Wine prefix.
//
// ORDER IS THE HIVE PRIORITY and it matches regfix.cpp's detect_hive, so "which hive won"
// cannot drift between the two files.
static const wchar_t* g_roots[] = {
    L"SOFTWARE\\PlayOnlineUS",
    L"SOFTWARE\\PlayOnline",
    L"SOFTWARE\\PlayOnlineEU",
};


// Deep enough for the deepest key measured (root\publisher\game\Settings\Controller),
// plus one. A cap at all because this walks a tree somebody else writes.
#define GAMECFG_MAXDEPTH 5

#define GAME_VIEWER "PlayOnline Viewer"
#define GAME_FFXI   "Final Fantasy XI"
#define GAME_TM     "Tetra Master"
#define GAME_FMO    "Front Mission Online"
#define GAME_FE     "Fantasy Earth"
#define GAME_FFXI_TC "Final Fantasy XI Test Client"

// WHICH HIVE IS LIVE. The active one is whichever PlayOnline* root carries
// InstallFolder\1000 -- the same rule and the same US/JP/EU order regfix.cpp's
// detect_hive uses, deliberately: if the two disagreed, this would open a tool from
// one region's install while the Viewer ran the other's.
//
// It matters even for a launcher. On a machine with both a JP and a US registration
// the SAME title can be registered twice with DIFFERENT folders, and FMO is registered
// ONLY in the JP hive on this install -- so trying one root and giving up would hide
// its config app entirely.
static int active_root()
{
    for (int i = 0; i < _countof(g_roots); i++) {
        wchar_t sub[300];
        _snwprintf_s(sub, _countof(sub), _TRUNCATE, L"%ls\\InstallFolder", g_roots[i]);
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        DWORD t = 0, cb = 0;
        LONG r = RegQueryValueExW(h, L"1000", NULL, &t, NULL, &cb);
        RegCloseKey(h);
        if (r == ERROR_SUCCESS && cb > 1) return i;
    }
    return 0;
}

// A title's install folder, from InstallFolder\<cid> in whichever hive has it -- the same
// value the Viewer's own menu launches the title from (regfix.cpp: 0011 = Fantasy Earth).
static bool title_folder(const wchar_t* cid, wchar_t* out, size_t cch)
{
    int first = active_root();
    for (int n = 0; n < _countof(g_roots); n++) {
        const wchar_t* root = g_roots[(first + n) % _countof(g_roots)];
        wchar_t sub[300];
        _snwprintf_s(sub, _countof(sub), _TRUNCATE, L"%ls\\InstallFolder", root);
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        wchar_t val[MAX_PATH]; DWORD type = 0, sz = sizeof(val) - sizeof(wchar_t);
        LONG r = RegQueryValueExW(h, cid, NULL, &type, (LPBYTE)val, &sz);
        RegCloseKey(h);
        if (r != ERROR_SUCCESS || sz < 2) continue;
        val[sz / sizeof(wchar_t)] = 0;
        size_t l = wcslen(val);
        while (l && (val[l-1] == L'\\' || val[l-1] == L'/')) val[--l] = 0;
        if (!l) continue;
        wcsncpy_s(out, cch, val, _TRUNCATE);
        return true;
    }
    return false;
}

// WHY THIS TABLE EXISTS AT ALL.
//
// Because SE's own settings UI is better than a reimplementation of it, and the
// difference is not effort -- it is KNOWLEDGE. FE's config app knows which
// resolutions Fantasy Earth accepts and what its shadow and texture levels are
// CALLED; that mapping lives in FE_Client.dll, whose .text is POL1-packed (raw size
// zero), so it cannot be read out statically. Offering our own dropdown of invented
// labels for SHADOW_LEVEL would be exactly the guess this file refuses to make
// everywhere else -- and offering a bare number box asks the reader "4 out of what?".
//
// So: settings we have MEASURED get proper controls in the game's own section, and
// for the rest the answer is the game's own window, one click away.
//
// `exe` is relative to the title's install folder. `cid` is the content id the folder
// is registered under (regfix.cpp's table), because that is how every other part of
// the shim finds a title's tree.
struct ConfigApp {
    const char*    game;
    const wchar_t* cid;
    const wchar_t* exe;       // the main config
    const wchar_t* padexe;    // its gamepad config, or NULL
    const char*    warn;      // shown beside the button BEFORE it is clicked, or NULL
    const char*    padwarn;
    // A REPAIRED BUILD OF THE SAME TOOL, tried FIRST when it is present.
    //
    // Only FMO has one, and only because its stock tool cannot actually be used: it
    // refuses to run while the Viewer is open and its labels are unreadable outside a
    // Japanese locale. A build script (fmocfg_build_en.py) makes the copy -- English resources
    // plus two .rdata strings that neuter the Viewer check -- and the stock exe is
    // left untouched beside it.
    //
    // When the repaired build is the one we open, `warn` is NOT shown: it describes
    // obstacles that are the whole reason the copy exists.
    const wchar_t* fixed;
    const wchar_t* padfixed;
};

static const ConfigApp g_apps[] = {
    // The VIEWER's own settings screen. Measured on this install at
    // <viewer>\polcfg\polcfg.exe (670 KB, with per-language .chm help beside it).
    // It owns the opening movie, sound, language and the controller bindings -- the
    // values this file used to duplicate as rows.
    //
    // cid 1000 is the Viewer's own InstallFolder entry, the same one regfix and
    // detect_hive use to decide which hive is live.
    { GAME_VIEWER, L"1000", L"polcfg\\polcfg.exe", NULL,
      "Opens the PlayOnline Viewer's own settings -- the opening movie, sound, "
      "language and controller buttons.", NULL, NULL, NULL },
    { GAME_FE,   L"0011", L"FantasyEarthConfig.exe", L"FEiPadConfig.exe",
      "Opens Fantasy Earth's own settings -- resolution, texture and shadow quality, "
      "water reflections. Close it before you launch the game.", NULL, NULL, NULL },
    // WARNING: TWO REAL OBSTACLES, BOTH MEASURED, BOTH WORTH SAYING BEFORE THE CLICK.
    // FrontMissionOnlineConfig.exe does FindWindowA on "PlayOnline"/"PlayOnlineUS" and
    // refuses to start while the Viewer is up; and its resources are Japanese-only, so
    // outside a Japanese system locale every label renders as a literal '?'.
    // WARNING: THE ONE TITLE WHOSE OWN TOOL IS GENUINELY AWKWARD, and the reason this file
    // once grew its own editor. Both obstacles are measured: it does FindWindowA on
    // "PlayOnline"/"PlayOnlineUS" and refuses to start while the Viewer is up, and its
    // resources are Japanese-only, so outside a Japanese system locale every label
    // renders as a literal '?'. Say both BEFORE the click rather than let someone
    // discover them by pressing a button that appears to do nothing.
    { GAME_FMO,  L"0004", L"FrontMissionOnlineConfig.exe", NULL,
      "Front Mission Online's own settings (resolution, refresh rate, gamepad).\n\n"
      "Two things to know first: it will NOT start while the PlayOnline Viewer is "
      "open, so close the Viewer first -- and its labels only render if Windows is "
      "set to Japanese; otherwise they show as '?'. The controls are still in the "
      "same places.", NULL,
      L"FrontMissionOnlineConfig.en.exe", NULL },
    { GAME_TM,   L"0002", L"TetraMasterConfig.exe", NULL,
      "Tetra Master's own settings, including its button assignments.", NULL, NULL, NULL },
    // FFXI ships its tools three times, once per region. ToolsUS first because the
    // test client this project runs is the US Viewer (see test-client-us-viewer);
    // whichever exists is used.
    { GAME_FFXI, L"0001", L"ToolsUS\\FINAL FANTASY XI Config.exe",
                          L"ToolsUS\\FFXiPadConfig.exe",
      "Opens FINAL FANTASY XI Config -- resolution, sound, mip mapping, bump mapping, "
      "the opening movie. FFXI stores these under numbered names, so this is the only "
      "place they have readable labels.", NULL, NULL, NULL },
    { GAME_FFXI_TC, L"0015", L"ToolsUS\\FINAL FANTASY XI Config.exe",
                             L"ToolsUS\\FFXiPadConfig.exe",
      "The Test Client keeps its own copy of every setting, and its own config tool.", NULL, NULL, NULL },
};

static const ConfigApp* app_for(const char* game)
{
    if (!game) return NULL;
    for (int i = 0; i < _countof(g_apps); i++)
        if (strcmp(g_apps[i].game, game) == 0) return &g_apps[i];
    return NULL;
}

// ToolsUS / ToolsEU / Tools -- a US install has all three, an old one may have only
// `Tools`. Try the row's spelling, then the other two, so a button is offered
// whenever SOMETHING is there rather than only for the layout we happened to write.
static bool app_probe(const wchar_t* folder, const wchar_t* rel, wchar_t* out, size_t cch)
{
    static const wchar_t* kTools[] = { L"ToolsUS\\", L"ToolsEU\\", L"Tools\\" };
    _snwprintf_s(out, cch, _TRUNCATE, L"%ls\\%ls", folder, rel);
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;

    const wchar_t* slash = wcschr(rel, L'\\');
    if (!slash) return false;
    for (int i = 0; i < _countof(kTools); i++) {
        _snwprintf_s(out, cch, _TRUNCATE, L"%ls\\%ls%ls", folder, kTools[i], slash + 1);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;
    }
    *out = 0;
    return false;
}

bool gamecfg_configapp(const char* game, int which, wchar_t* path, size_t cch)
{
    return gamecfg_configapp_ex(game, which, path, cch, NULL);
}

// `is_fixed` (optional) comes back true when the tool we resolved is the REPAIRED
// build rather than the stock one. The caller needs it for exactly one thing: the
// stock tool's warning describes obstacles the repaired build does not have, and
// showing it anyway would send someone off to close a Viewer they do not need to
// close.
bool gamecfg_configapp_ex(const char* game, int which, wchar_t* path, size_t cch,
                          bool* is_fixed)
{
    if (path && cch) *path = 0;
    if (is_fixed) *is_fixed = false;
    const ConfigApp* a = app_for(game);
    if (!a || !path) return false;
    const wchar_t* rel = which ? a->padexe : a->exe;
    if (!rel) return false;

    wchar_t folder[MAX_PATH];
    if (!title_folder(a->cid, folder, _countof(folder))) return false;

    const wchar_t* fixed = which ? a->padfixed : a->fixed;
    if (fixed && app_probe(folder, fixed, path, cch)) {
        if (is_fixed) *is_fixed = true;
        return true;
    }
    return app_probe(folder, rel, path, cch);
}

const char* gamecfg_configapp_warning(const char* game, int which)
{
    const ConfigApp* a = app_for(game);
    if (!a) return NULL;
    wchar_t path[MAX_PATH]; bool fixed = false;
    if (gamecfg_configapp_ex(game, which, path, _countof(path), &fixed) && fixed)
        return NULL;      // the repaired build has neither obstacle
    return which ? a->padwarn : a->warn;
}

DWORD gamecfg_configapp_launch(const char* game, int which)
{
    wchar_t path[MAX_PATH]; bool fixed = false;
    if (!gamecfg_configapp_ex(game, which, path, _countof(path), &fixed)) {
        logf("[gamecfg] no config app for %hs (which=%d) on this machine", game ? game : "?", which);
        return ERROR_FILE_NOT_FOUND;
    }

    // KEY: START IT IN ITS OWN FOLDER. These tools resolve their help file, their
    // resources and (FE's) Settings\GLOBAL.INI RELATIVE to the working directory --
    // FE's config app builds "<cwd>\Settings\GLOBAL.INI" -- so launching it with the
    // Viewer's directory inherited would have it read and write the wrong file, which
    // is this project's single most repeated bug wearing a new hat.
    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, path, _TRUNCATE);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0; else dir[0] = 0;

    SHELLEXECUTEINFOW si; ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SEE_MASK_FLAG_NO_UI;
    si.lpVerb = L"open";
    si.lpFile = path;
    si.lpDirectory = dir[0] ? dir : NULL;
    si.nShow  = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si)) {
        DWORD e = GetLastError();
        logf("[gamecfg] could not launch %ls (%lu)", path, e);
        return e;
    }
    // Inherited elevation is a FEATURE here, not an accident: pol.exe is elevated, so
    // the tool it starts is too, and its writes land in the real install tree instead
    // of the per-user VirtualStore. That is precisely the divergence fecfg.cpp exists
    // to repair -- launching the tool from here avoids creating it in the first place.
    logf("[gamecfg] launched %ls%hs (cwd %ls)", path,
         fixed ? "  [the repaired build: English, and no Viewer check]" : "",
         dir[0] ? dir : L"(inherited)");
    return 0;
}

// ===========================================================================
// THE DIFF REPORT -- unchanged in behaviour, and still the way an unnamed value
// gets identified: view, change it in the title's own config app, view again.
// ===========================================================================

static char*  g_out;
static size_t g_left;

static void gout(const char* fmt, ...)
{
    char line[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_out && g_left > 1) {
        size_t n = strlen(line);
        if (n + 2 > g_left) n = g_left - 2;
        memcpy(g_out, line, n); g_out += n; *g_out++ = '\n'; *g_out = 0;
        g_left -= (n + 1);
    }
}

// One value as "key|name=text", the line format the baseline file stores.
static void value_text(HKEY h, const wchar_t* name, char* out, size_t cch)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(h, name, NULL, &type, NULL, &cb) != ERROR_SUCCESS) { strcpy_s(out, cch, "?"); return; }
    if (cb > 4096) { _snprintf_s(out, cch, _TRUNCATE, "<%lu bytes>", cb); return; }
    BYTE buf[4096]; DWORD got = sizeof(buf);
    if (RegQueryValueExW(h, name, NULL, &type, buf, &got) != ERROR_SUCCESS) { strcpy_s(out, cch, "?"); return; }
    if (type == REG_DWORD)      _snprintf_s(out, cch, _TRUNCATE, "%lu", *(DWORD*)buf);
    else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        buf[got < sizeof(buf) - 2 ? got : sizeof(buf) - 2] = 0;
        WideCharToMultiByte(CP_ACP, 0, (wchar_t*)buf, -1, out, (int)cch, NULL, NULL);
    } else {                                    // binary: first bytes, enough to spot a change
        char* p = out; size_t left = cch;
        DWORD show = got < 16 ? got : 16;
        for (DWORD i = 0; i < show && left > 4; i++) {
            _snprintf_s(p, left, _TRUNCATE, "%02x ", buf[i]);
            size_t adv = strlen(p); p += adv; left -= adv;
        }
        if (show < got && left > 4) _snprintf_s(p, left, _TRUNCATE, "...");
    }
}

static void baseline_path(const wchar_t* ini, wchar_t* out, size_t cch)
{
    wcsncpy_s(out, cch, ini, _TRUNCATE);
    wchar_t* s = wcsrchr(out, L'\\'); if (!s) s = wcsrchr(out, L'/');
    if (s) { *(s + 1) = 0; wcsncat_s(out, cch, L"gamecfg-baseline.txt", _TRUNCATE); }
    else   wcsncpy_s(out, cch, L"gamecfg-baseline.txt", _TRUNCATE);
}

// Look a "key|name" line up in the baseline. Returns false when absent (a NEW value).
static bool baseline_find(FILE* f, const char* want, char* val, size_t cch)
{
    if (!f) return false;
    fseek(f, 0, SEEK_SET);
    char line[2048];
    size_t wl = strlen(want);
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (strncmp(line, want, wl) == 0 && line[wl] == '=') {
            strcpy_s(val, cch, line + wl + 1);
            return true;
        }
    }
    return false;
}

// --- the walk ---------------------------------------------------------------
static FILE* g_bf;
static FILE* g_nf;
static int   g_total;
static int   g_changed;

static void walk(const wchar_t* sub, int depth)
{
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) return;

    DWORD nsub = 0, nval = 0;
    RegQueryInfoKeyW(h, NULL, NULL, NULL, &nsub, NULL, NULL, &nval, NULL, NULL, NULL, NULL);

    if (nval) {
        char subA[600];
        WideCharToMultiByte(CP_ACP, 0, sub, -1, subA, sizeof(subA), NULL, NULL);
        gout("HKLM\\%s", subA);
        for (DWORD i = 0; i < nval; i++) {
            wchar_t nm[256]; DWORD ncap = _countof(nm);
            if (RegEnumValueW(h, i, nm, &ncap, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            char nameA[256], valA[1024];
            WideCharToMultiByte(CP_ACP, 0, nm, -1, nameA, sizeof(nameA), NULL, NULL);
            value_text(h, nm, valA, sizeof(valA));
            // An unnamed value is the key's default. It has to READ as something, but
            // its identity in the baseline must stay the empty name it really has.
            const char* shown = nameA[0] ? nameA : "(default)";
            g_total++;

            char id[900]; _snprintf_s(id, sizeof(id), _TRUNCATE, "%s|%s", subA, nameA);
            if (g_nf) fprintf(g_nf, "%s=%s\n", id, valA);

            char was[1024];
            bool had = baseline_find(g_bf, id, was, sizeof(was));
            if (had && strcmp(was, valA) != 0) {
                gout("  * %-24s %s      (was %s)", shown, valA, was);
                g_changed++;
            } else {
                gout("    %-24s %s", shown, valA);
            }
        }
        gout("");
    }

    if (depth < GAMECFG_MAXDEPTH) {
        for (DWORD i = 0; ; i++) {
            wchar_t nm[256]; DWORD ncap = _countof(nm);
            if (RegEnumKeyExW(h, i, nm, &ncap, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            wchar_t child[1024];
            _snwprintf_s(child, _countof(child), _TRUNCATE, L"%ls\\%ls", sub, nm);
            walk(child, depth + 1);
        }
    }
    RegCloseKey(h);
}

int gamecfg_report(const wchar_t* ini, char* out, size_t cch)
{
    g_out = out; g_left = cch;
    if (out && cch) *out = 0;

    wchar_t bpath[MAX_PATH]; baseline_path(ini, bpath, _countof(bpath));
    g_bf = _wfopen(bpath, L"rt");
    g_total = 0; g_changed = 0;

    gout("Per-game settings. Values marked * changed since the last time you opened");
    gout("this window. FFXI stores these under numbered names with no documentation,");
    gout("so the way to find one is to diff: open this, change the setting in the");
    gout("game's own config app, then open this again and read the * lines.");
    gout("");

    // Collect the new baseline as we go, then replace the old one.
    wchar_t tmp[MAX_PATH]; wcsncpy_s(tmp, bpath, _TRUNCATE); wcsncat_s(tmp, L".new", _TRUNCATE);
    g_nf = _wfopen(tmp, L"wt");

    for (int i = 0; i < _countof(g_roots); i++)
        walk(g_roots[i], 1);

    if (g_bf) fclose(g_bf);
    if (g_nf) fclose(g_nf);
    // Replace the baseline only after a successful pass, so a failed read cannot wipe it.
    if (g_nf) { DeleteFileW(bpath); MoveFileW(tmp, bpath); }
    g_bf = NULL; g_nf = NULL;

    if (!g_total) {
        gout("No per-game keys found -- no title has written its settings on this machine.");
    } else if (g_changed) {
        gout("%d value(s) changed. Those are the ones your last action wrote.", g_changed);
    } else {
        gout("Nothing changed since the last view. Baseline refreshed (%d values); change a", g_total);
        gout("setting in the game's config app now, then open this again to see which");
        gout("value it was.");
    }
    g_out = NULL;
    return g_changed;
}

// ===========================================================================
// SELF-TEST
// ===========================================================================
//
// Reads only. It asks the one question this file can get wrong on a machine it
// has never seen: does every game we offer a button for actually resolve to a
// tool, or are we drawing buttons that can only fail?
int gamecfg_selftest(void)
{
    int bad = 0, found = 0;
    for (int i = 0; i < _countof(g_apps); i++) {
        const ConfigApp* a = &g_apps[i];
        if (!a->game || !a->cid || !a->exe) {
            printf("[gamecfg] app row %d is missing game/cid/exe\n", i); bad++;
            continue;
        }
        // A row whose warning is empty rather than NULL would draw a blank prompt.
        if (a->warn && !a->warn[0]) {
            printf("[gamecfg] app row %s has an empty warning\n", a->game); bad++;
        }
        wchar_t path[MAX_PATH];
        if (gamecfg_configapp(a->game, 0, path, _countof(path))) found++;
    }
    printf("[gamecfg] selftest: %d config app(s) known, %d present here, %d problem(s)\n",
           (int)_countof(g_apps), found, bad);
    return bad;
}
