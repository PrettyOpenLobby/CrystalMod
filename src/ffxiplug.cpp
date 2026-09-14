// ffxiplug.cpp -- FINAL FANTASY XI add-ons: coexisting with Ashita and Windower,
// and loading plugin DLLs into the title.
//
// WHY THIS IS SMALL, AND WHY THAT IS THE WHOLE POINT
//
// Ashita and Windower are not launchers that own the game -- they are IN-PROCESS
// hooks of **pol.exe**, the same process this shim already lives in. Ashita
// "launches and directly injects itself (Ashita.dll) into the game's main process,
// generally pol.exe"; Windower's launcher injects `hook.dll` into the pol.exe it
// starts. FFXiMain arriving mid-process-life through polcore's COM GameStart call
// is the RETAIL flow both were written against, so none of the PlayOnline-specific
// work falls on us.
//
// And our delivery makes them free to live with: PolHook.dll is a PROXY that
// pol.exe STATICALLY IMPORTS, so the shim loads no matter who starts
// the process. Point Ashita's or Windower's launcher at the same pol.exe and both
// cores are in the process, ours first. There is nothing to integrate.
//
// So "support Ashita/Windower plugins" is NOT: reimplementing AshitaCore's manager
// interfaces, or Windower's Lua runtime and addon API, against a signature database
// we do not own and could not keep current. It is two much smaller things:
//
//   1. GET OUT OF THE WAY. Our Direct3D 8 and probe layers were written for titles
//      that have no add-on core (Tetra Master, Fantasy Earth). Run them under one
//      and two things collide -- see the two consumers below.
//   2. LOAD WHAT THE USER ASKS FOR. A plain DLL loader for modules that want to be
//      in the FFXI process, ours or anyone's.
//
// WARNING: STATUS: BUILT, NOT YET PROVEN LIVE. Nothing here has run against a real Ashita
// or Windower install -- the machine this was written on has neither. The detector
// matches on module identity and the stand-down is mechanical, but "the two cores
// coexist through a whole FFXI session" is an EXPERIMENT, not a
// measurement. Do not write it up as working until that experiment has run.
//
// THE TWO CONSUMERS OF ffxiplug_compat_active()
//
//   d3d8hook.cpp   caller_excepted() -- FFXI joins d3d_windowed_except while a core
//                  is present, which is the ONE switch that keeps our hands off both
//                  CreateDevice and Reset: every intrusive thing we do (the windowed
//                  override, fit_window, the mask realign, the window icon) lives
//                  inside the branch that switch guards. An add-on core does its own
//                  device wrapping and its own windowing; two of us doing it is how
//                  you get a lost device.
//
//   inject.cpp     the FFXI probe arming. probes.cpp PATCHES BYTES at fixed RVAs in
//                  FFXiMain (the connection state machine, the -net parse, the world
//                  context allocator). Ashita and Windower FIND their own hook sites
//                  by scanning FFXiMain's memory for byte signatures. Our patch is
//                  upstream of their scan, so a probe can silently move a signature
//                  out from under a core that was about to hook it -- and the failure
//                  would present as "the add-on is broken", nowhere near us. Probes
//                  are a dev instrument; a core in the process wins.
//
// WHAT IS DELIBERATELY NOT HERE
//
//   * We do not START anything. Ashita's and Windower's launchers do that, and a
//     shim that spawns other people's processes is a support burden with no upside.
//   * Windower's hook.dll expects ITS launcher to be running (it takes settings and
//     addon state over the launcher's channel). Listing it in `plugins=` loads the
//     DLL and gets you nothing useful. Ashita's core is likelier to survive a bare
//     LoadLibrary, but it has not been tried -- either way the SUPPORTED route is
//     "let their launcher start pol.exe", and `plugins=` is for modules that were
//     written to be loaded this way.

#include "polshim.h"

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

enum { COMPAT_OFF = 0, COMPAT_AUTO = 1, COMPAT_ON = 2 };

static int      g_mode      = COMPAT_AUTO;
static int      g_trace     = 0;
static int      g_at_start  = 0;              // load_at=startup rather than ffximain
static wchar_t  g_list[1024] = L"";           // [ffxi] plugins=
static wchar_t  g_dir[MAX_PATH] = L"";        // resolved plugin folder
static char     g_core[32]  = "";             // "Ashita" / "Windower", once found
static char     g_core_path[MAX_PATH] = "";
static LONG     g_loaded    = 0;              // one-shot guard on the plugin list
static int      g_nloaded   = 0, g_nfailed = 0;

// Deferred-load plumbing. LoadLibrary of a plugin must never run under the loader
// lock -- both trigger sites (the LDR notification callback for FFXiMain, and the
// startup path) hold it, and a plugin's own DllMain re-entering the loader would
// deadlock pol.exe (the ghost-process class). The trigger only signals; a dedicated
// worker thread does the actual load off-lock. See ffxiplug_start/ffxiplug_worker.
static HANDLE   g_load_event  = NULL;         // auto-reset; signalled by a trigger
static char     g_pending_why[32] = "";       // why the load was requested (for the log)
static void     ffxiplug_request_load(const char* why);

// The folder a bare name in `plugins=` is resolved against, and the folder the ini
// itself lives in. Both come from the ini path so a dev build in build\ and an
// install in the game tree each keep their own plugins -- the same rule the log and
// every other shim file already follow ([[shim-two-locations]]).
static void resolve_dirs(const wchar_t* ini)
{
    wchar_t base[MAX_PATH]; wcsncpy_s(base, ini, _TRUNCATE);
    wchar_t* slash = wcsrchr(base, L'\\');
    if (slash) *slash = 0; else base[0] = 0;

    wchar_t sub[128];
    ini_str(L"ffxi", L"plugins_dir", L"plugins", sub, _countof(sub), ini);
    if (!sub[0]) wcscpy_s(sub, L"plugins");

    // An absolute plugins_dir is honoured as given; anything else hangs off the ini.
    if (sub[1] == L':' || (sub[0] == L'\\' && sub[1] == L'\\'))
        wcsncpy_s(g_dir, sub, _TRUNCATE);
    else
        swprintf_s(g_dir, L"%ls\\%ls", base, sub);
}

void ffxiplug_configure(const wchar_t* ini)
{
    wchar_t mode[32];
    ini_str(L"ffxi", L"addons", L"auto", mode, _countof(mode), ini);
    if      (!_wcsicmp(mode, L"off") || !wcscmp(mode, L"0")) g_mode = COMPAT_OFF;
    else if (!_wcsicmp(mode, L"on")  || !wcscmp(mode, L"1")) g_mode = COMPAT_ON;
    else                                                     g_mode = COMPAT_AUTO;

    g_trace = GetPrivateProfileIntW(L"ffxi", L"trace", trace_at(2), ini);
    ini_str(L"ffxi", L"plugins", L"", g_list, _countof(g_list), ini);

    wchar_t when[32];
    ini_str(L"ffxi", L"load_at", L"ffximain", when, _countof(when), ini);
    g_at_start = (_wcsicmp(when, L"startup") == 0);

    resolve_dirs(ini);

    if (g_mode == COMPAT_ON)
        logf("[ffxi] add-on coexistence FORCED ON -- FFXI's device and probes are "
             "left alone whether or not a core is detected");
    if (g_list[0])
        logf("[ffxi] plugins=%ls (load_at=%ls, folder %ls)", g_list,
             g_at_start ? L"startup" : L"FFXiMain", g_dir);
}

// Live-reload for the in-game settings dialog: VALUES ONLY. The addons mode is
// consulted on every ffxiplug_compat_active() call, so it applies at once;
// plugins / plugins_dir only matter to a load that has not happened yet.
// Deliberately NOT touched:
//   * load_at -- it picks which one-shot trigger fires, and both triggers are
//     already armed (or not) for this process.
//   * the worker / load event -- ffxiplug_start is a one-shot. If plugins= was
//     empty at startup, no worker or event exists, and this creates neither, so
//     a list written mid-session stays restart-bound.
//   * and if the one-shot load already fired (g_loaded), the new list is inert
//     until restart -- fine: plugins load once per process by design.
void ffxiplug_reload(const wchar_t* ini)
{
    wchar_t mode[32];
    ini_str(L"ffxi", L"addons", L"auto", mode, _countof(mode), ini);
    if      (!_wcsicmp(mode, L"off") || !wcscmp(mode, L"0")) g_mode = COMPAT_OFF;
    else if (!_wcsicmp(mode, L"on")  || !wcscmp(mode, L"1")) g_mode = COMPAT_ON;
    else                                                     g_mode = COMPAT_AUTO;

    ini_str(L"ffxi", L"plugins", L"", g_list, _countof(g_list), ini);
    resolve_dirs(ini);

    logf("[reload] ffxi: addons=%ls plugins=%ls dir=%ls%s", mode, g_list, g_dir,
         g_loaded ? " (plugin list already loaded -- new values inert until restart)"
                  : "");
}

// ---------------------------------------------------------------------------
// detection
// ---------------------------------------------------------------------------
//
// MATCH ON THE INSTALL PATH, NOT THE NAME ALONE -- the same rule d3d8hook already
// follows for title modules, and here it is load-bearing: Windower's core is called
// `hook.dll`, which is a name half the software in the world could have used. A
// module only counts as Windower's if it LIVES in a Windower install. Ashita's is
// distinctive enough to match either way, and both spellings are accepted because
// v3 and v4 differ.

static void lower_ascii(char* s)
{
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static const char* core_of(const char* path_lower, const char* leaf_lower)
{
    if (strstr(path_lower, "\\ashita") || !strcmp(leaf_lower, "ashita.dll") ||
        !strcmp(leaf_lower, "ashitacore.dll"))
        return "Ashita";
    if (strstr(path_lower, "\\windower"))
        return "Windower";
    return NULL;
}

// Called for EVERY module load (inject.cpp's handle_module) and for every module
// already present at startup (its sweep). Two jobs: notice a core arriving, and
// notice FFXiMain arriving so the plugin list can be loaded at the right moment.
void ffxiplug_on_module(void* base)
{
    if (!base) return;
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA((HMODULE)base, path, MAX_PATH)) return;
    const char* leafp = strrchr(path, '\\');
    char leaf[64]; strncpy_s(leaf, leafp ? leafp + 1 : path, _TRUNCATE);

    char pl[MAX_PATH]; strncpy_s(pl, path, _TRUNCATE); lower_ascii(pl);
    char ll[64];       strncpy_s(ll, leaf, _TRUNCATE); lower_ascii(ll);

    if (!g_core[0]) {
        const char* which = core_of(pl, ll);
        if (which) {
            strncpy_s(g_core, which, _TRUNCATE);
            strncpy_s(g_core_path, path, _TRUNCATE);
            // LOUD, because this changes what the shim does to FFXI and the log is
            // the only place that answers "was the shim in the way" afterwards.
            logf("[ffxi] %s add-on core detected in this process: %s", g_core, path);
            if (g_mode == COMPAT_AUTO)
                logf("[ffxi] coexistence ACTIVE -- FFXI's Direct3D device is passed "
                     "through untouched and the FFXiMain probes stay unarmed "
                     "([ffxi] addons=off to keep the shim's own handling instead)");
            else if (g_mode == COMPAT_OFF)
                logf("[ffxi] coexistence is OFF by setting -- the shim keeps its own "
                     "device handling. If the add-on misbehaves, this is the first "
                     "thing to change ([ffxi] addons=auto)");
        }
    }

    if (!g_at_start && _stricmp(leaf, "FFXiMain.dll") == 0)
        ffxiplug_request_load("FFXiMain");
}

int ffxiplug_compat_active(void)
{
    if (g_mode == COMPAT_ON)  return 1;
    if (g_mode == COMPAT_OFF) return 0;
    return g_core[0] ? 1 : 0;
}

const char* ffxiplug_core(void) { return g_core[0] ? g_core : NULL; }

// Is this module one of an add-on core's own? MEASURED LIVE 2026-08-18:
// Ashita interposes the D3D8 layer, so FFXI's CreateDevice arrives with the
// RETURN ADDRESS inside Ashita.dll, not FFXiMain.dll -- caller_excepted()'s
// FFXiMain test never fires and the whole D3D stand-down silently misses. The
// caller-module test therefore needs a second arm: a device call made BY the core
// is FFXI's device arriving through the core's interposer (the cores exist only
// for FFXI), and it gets the same hands-off treatment. Reuses core_of(), the one
// detector the selftest pins -- no second notion of "what is Ashita" to drift.
int ffxiplug_module_is_core(const char* path, const char* leaf)
{
    if (!path || !leaf) return 0;
    char pl[MAX_PATH]; strncpy_s(pl, path, _TRUNCATE); lower_ascii(pl);
    char ll[64];       strncpy_s(ll, leaf, _TRUNCATE); lower_ascii(ll);
    return core_of(pl, ll) != NULL;
}

// ---------------------------------------------------------------------------
// the plugin loader
// ---------------------------------------------------------------------------
//
// LOAD_WITH_ALTERED_SEARCH_PATH is not decoration: a plugin with satellite DLLs
// beside it (which is the normal shape) resolves them from ITS OWN folder only with
// that flag -- otherwise the search runs from pol.exe's directory and the load fails
// with 126 for a file that is plainly there, which reads as "the shim did not load
// my plugin".

static void try_load_one(const wchar_t* spec)
{
    wchar_t cand[MAX_PATH * 2];
    const wchar_t* tried[3]; int ntried = 0;
    static wchar_t buf[3][MAX_PATH * 2];

    // Two cases. An ABSOLUTE spec is tried as given (one candidate). A RELATIVE spec is
    // tried under the plugins folder first, then beside the ini (two candidates) -- so
    // `foo\bar.dll` relative to plugins\ is a reasonable thing to write. Only this single
    // worker thread calls try_load_one (see ffxiplug_start), so the static buf is fine.
    if (spec[1] == L':' || (spec[0] == L'\\' && spec[1] == L'\\')) {
        wcsncpy_s(buf[0], spec, _TRUNCATE); tried[ntried++] = buf[0];
    } else {
        swprintf_s(buf[0], L"%ls\\%ls", g_dir, spec); tried[ntried++] = buf[0];
        wchar_t base[MAX_PATH]; wcsncpy_s(base, g_dir, _TRUNCATE);
        wchar_t* s = wcsrchr(base, L'\\'); if (s) *s = 0;
        swprintf_s(buf[1], L"%ls\\%ls", base, spec); tried[ntried++] = buf[1];
    }

    for (int i = 0; i < ntried; i++) {
        wcsncpy_s(cand, tried[i], _TRUNCATE);
        if (GetFileAttributesW(cand) == INVALID_FILE_ATTRIBUTES) {
            if (g_trace) logf("[ffxi] plugin: no file at %ls", cand);
            continue;
        }
        HMODULE m = LoadLibraryExW(cand, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            logf("[ffxi] plugin FAILED to load: %ls (error %lu)", cand, GetLastError());
            g_nfailed++;
            return;
        }
        logf("[ffxi] plugin loaded: %ls (%p)", cand, m);
        g_nloaded++;

        // An OPTIONAL entry point, and deliberately a trivial one: this is a loader,
        // not a plugin ABI. A module that wants to know it is in the FFXI process
        // exports this and does its own work; everything else just gets its DllMain.
        // Guarded with SEH because a plugin fault here would otherwise be OUR crash,
        // in a thread the user cannot attribute -- crashlog.cpp would name PolHook.
        typedef void (__cdecl *PFN_INIT)(void);
        PFN_INIT init = (PFN_INIT)GetProcAddress(m, "polshim_plugin_init");
        if (init) {
            __try { init(); logf("[ffxi]   polshim_plugin_init() returned"); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                logf("[ffxi]   polshim_plugin_init() FAULTED (0x%08lX) -- the plugin "
                     "is loaded but its init did not finish", GetExceptionCode());
            }
        }
        return;
    }
    logf("[ffxi] plugin NOT FOUND: '%ls' (looked in %ls)", spec, g_dir);
    g_nfailed++;
}

void ffxiplug_load_now(const char* why)
{
    if (!g_list[0]) return;
    if (InterlockedCompareExchange(&g_loaded, 1, 0) != 0) return;   // one shot per process

    logf("[ffxi] loading plugins (%s)", why ? why : "?");
    wchar_t work[1024]; wcsncpy_s(work, g_list, _TRUNCATE);
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(work, L",", &ctx); t; t = wcstok_s(NULL, L",", &ctx)) {
        while (*t == L' ' || *t == L'\t') t++;
        size_t n = wcslen(t);
        while (n && (t[n - 1] == L' ' || t[n - 1] == L'\t')) t[--n] = 0;
        if (!t[0]) continue;
        try_load_one(t);
    }
    logf("[ffxi] plugins: %d loaded, %d failed", g_nloaded, g_nfailed);
}

// The deferred-load worker: blocks off the loader lock until a trigger fires, then
// does the one and only plugin load. One-shot -- it loads once and exits (the plugin
// list is loaded a single time per process, guarded again inside ffxiplug_load_now).
static DWORD WINAPI ffxiplug_worker(void*)
{
    if (WaitForSingleObject(g_load_event, INFINITE) != WAIT_OBJECT_0) return 0;
    char why[32];
    strncpy_s(why, g_pending_why[0] ? g_pending_why : "?", _TRUNCATE);
    ffxiplug_load_now(why);     // the actual LoadLibrary work -- now off the loader lock
    return 0;
}

// Signal the worker to load. Safe to call from under the loader lock (the LDR
// notification callback, sweep_loaded, or startup): it only stamps a reason and
// sets an event -- no LoadLibrary, no blocking.
static void ffxiplug_request_load(const char* why)
{
    if (!g_list[0]) return;                 // nothing configured; no worker exists
    strncpy_s(g_pending_why, why ? why : "?", _TRUNCATE);
    if (g_load_event) SetEvent(g_load_event);
}

// Called from the init sequence once the log is open. Creates the off-lock worker
// (only if plugins are configured) and, for load_at=startup, kicks it immediately.
// Starting a thread from startup() is the same pattern polctl/titletag/maskguard use:
// the loader lock releases before the new thread runs its body.
void ffxiplug_start(void)
{
    if (!g_list[0]) return;                 // no plugins => no worker, no deferral needed
    g_load_event = CreateEventW(NULL, FALSE /*auto-reset*/, FALSE, NULL);
    if (g_load_event) {
        // Fire-and-forget daemon: never joined (joining a worker from DllMain's
        // detach path would deadlock on the loader lock), so close the handle now.
        HANDLE t = CreateThread(NULL, 0, ffxiplug_worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else {
        logf("[ffxi] WARN: could not create load event (%lu); plugins will not load",
             GetLastError());
    }
    if (g_at_start) ffxiplug_request_load("startup");
}

void ffxiplug_summary(void)
{
    if (!g_core[0] && !g_nloaded && !g_nfailed && g_mode != COMPAT_ON) return;
    logf("[ffxi] summary: core=%s coexistence=%s plugins=%d loaded/%d failed",
         g_core[0] ? g_core : "(none)",
         ffxiplug_compat_active() ? "active" : "inactive",
         g_nloaded, g_nfailed);
}

// ---------------------------------------------------------------------------
// self-test -- runs in polsettingstest.exe, no Viewer and no add-on required
// ---------------------------------------------------------------------------
//
// The detector is the part that can be wrong QUIETLY: a false positive stands the
// shim's own FFXI handling down on a machine with no add-on at all (and the user
// then reports "windowed mode stopped working"), while a false negative leaves two
// cores fighting over one device. Neither shows up in a log you would think to read.

int ffxiplug_selftest(void)
{
    struct Case { const char* path; const char* leaf; const char* want; };
    static const Case cases[] = {
        { "c:\\ashita v4\\ashita.dll",                    "ashita.dll",  "Ashita"   },
        { "d:\\games\\ashita\\plugins\\addons.dll",       "addons.dll",  "Ashita"   },
        { "c:\\program files (x86)\\windower\\hook.dll",  "hook.dll",    "Windower" },
        // The reason path matching exists: `hook.dll` on its own is nobody's tell.
        { "c:\\some app\\hook.dll",                       "hook.dll",    NULL       },
        // And the ones that must NEVER match, or we stand down on a normal install.
        { "c:\\program files (x86)\\playonline\\squareenix\\playonlineviewer\\pol.exe",
          "pol.exe", NULL },
        { "c:\\program files (x86)\\playonline\\squareenix\\final fantasy xi\\ffximain.dll",
          "ffximain.dll", NULL },
        { "c:\\windows\\system32\\d3d8.dll",              "d3d8.dll",    NULL       },
    };
    int fail = 0;
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        char pl[MAX_PATH]; strncpy_s(pl, cases[i].path, _TRUNCATE); lower_ascii(pl);
        char ll[64];       strncpy_s(ll, cases[i].leaf, _TRUNCATE); lower_ascii(ll);
        const char* got = core_of(pl, ll);
        bool ok = (!got && !cases[i].want) ||
                  (got && cases[i].want && !strcmp(got, cases[i].want));
        printf("  %-52s %s\n", cases[i].path, ok ? "ok" : "FAIL");
        if (!ok) fail++;
    }
    // ffxiplug_module_is_core must agree with the detector on every case above --
    // it is the same core_of() behind a caller-attribution door (the arm added
    // after the 2026-08-18 live run), and a disagreement would mean two notions
    // of "what is a core" have drifted apart.
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        int got = ffxiplug_module_is_core(cases[i].path, cases[i].leaf);
        bool ok = (got != 0) == (cases[i].want != NULL);
        printf("  module_is_core(%-38s %s\n", cases[i].leaf, ok ? ")  ok" : ")  FAIL");
        if (!ok) fail++;
    }

    // The mode gate itself: OFF must stay off even with a core present, ON must be
    // on without one. Restored afterwards so the harness leaves no state behind.
    const int save_mode = g_mode; char save_core[32]; strncpy_s(save_core, g_core, _TRUNCATE);
    g_core[0] = 0; g_mode = COMPAT_ON;
    bool ok_on = ffxiplug_compat_active() != 0;
    strncpy_s(g_core, "Ashita", _TRUNCATE); g_mode = COMPAT_OFF;
    bool ok_off = ffxiplug_compat_active() == 0;
    g_mode = COMPAT_AUTO;
    bool ok_auto = ffxiplug_compat_active() != 0;
    g_core[0] = 0;
    bool ok_auto0 = ffxiplug_compat_active() == 0;
    printf("  %-52s %s\n", "addons=on is active with no core",    ok_on   ? "ok" : "FAIL");
    printf("  %-52s %s\n", "addons=off is inactive with a core",  ok_off  ? "ok" : "FAIL");
    printf("  %-52s %s\n", "addons=auto follows the core",        (ok_auto && ok_auto0) ? "ok" : "FAIL");
    fail += (!ok_on) + (!ok_off) + (!(ok_auto && ok_auto0));
    g_mode = save_mode; strncpy_s(g_core, save_core, _TRUNCATE);
    return fail;
}
