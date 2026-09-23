// sysdiag.cpp -- the machine facts a bug report carries as diag.txt.
//
// WHY. A "black screen for some players" report is unanswerable from the log
// alone, because the log says what the TITLE did and the fault is usually in
// what the MACHINE did with it: which GPU and driver, what display scaling the
// monitor runs at, a Windows compatibility setting on pol.exe, or a recording
// or FPS overlay that injected itself into the process. None of that is in the
// shim log, and asking a player for it afterwards costs a day per question.
// So the report collects it at the key press, next to the screenshot.
//
// WHAT IT MUST NOT CARRY. Paths under the user's profile are rewritten to
// %USERPROFILE% (a Windows user name is often a real name), and for windows
// that belong to OTHER programs only the class and the program's file name are
// recorded, never the window title, which can be a document or a chat. Nothing
// here reads a file's contents or any credential.
//
// Read-only and bounded: every call is a query. The only state it changes is
// this thread's DPI awareness, which it switches to per-monitor for the
// duration of the monitor walk (so the numbers are the real ones rather than
// the 96-DPI virtualised ones an unaware process is told) and then restores.
#include "polshim.h"

#include <windows.h>
#include <psapi.h>     // K32EnumProcessModules lives in kernel32; this only declares it
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int d3d8_diag_text(char* out, size_t cch, DWORD sample_ms);

struct Out {
    char*  p;
    size_t len, cap;
};

static void out_fmt(Out* o, const char* fmt, ...)
{
    if (!o->p || o->len + 1 >= o->cap) return;
    va_list ap; va_start(ap, fmt);
    int w = _vsnprintf_s(o->p + o->len, o->cap - o->len, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (w > 0) o->len += (size_t)w;
    else       o->len = strlen(o->p);   // truncated: keep what fitted
}

// --- paths -------------------------------------------------------------------
static wchar_t g_profile[MAX_PATH];

static void to_utf8(const wchar_t* w, char* out, size_t cch)
{
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cch, NULL, NULL);
    out[cch - 1] = 0;
}

// A path with the user's profile directory replaced, as UTF-8.
static void anon_path(const wchar_t* path, char* out, size_t cch)
{
    size_t pl = wcslen(g_profile);
    if (pl && !_wcsnicmp(path, g_profile, pl)) {
        wchar_t tmp[MAX_PATH * 2];
        _snwprintf_s(tmp, _countof(tmp), _TRUNCATE, L"%%USERPROFILE%%%s", path + pl);
        to_utf8(tmp, out, cch);
    } else {
        to_utf8(path, out, cch);
    }
}

static const wchar_t* leaf_of(const wchar_t* path)
{
    const wchar_t* s = wcsrchr(path, L'\\');
    return s ? s + 1 : path;
}

// "1.2.3.4 (ProductName)" from a module's version resource, or "" when it has none.
static void file_version(const wchar_t* path, char* out, size_t cch)
{
    out[0] = 0;
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return;
    void* buf = malloc(sz);
    if (!buf) return;
    if (GetFileVersionInfoW(path, 0, sz, buf)) {
        VS_FIXEDFILEINFO* fi = NULL; UINT fl = 0;
        char ver[64] = "";
        if (VerQueryValueW(buf, L"\\", (void**)&fi, &fl) && fi && fl)
            _snprintf_s(ver, sizeof(ver), _TRUNCATE, "%u.%u.%u.%u",
                        HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                        HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        char prod[128] = "";
        struct { WORD lang, cp; }* tr = NULL; UINT tl = 0;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void**)&tr, &tl) && tl >= 4) {
            wchar_t q[80]; wchar_t* v = NULL; UINT vl = 0;
            _snwprintf_s(q, _countof(q), _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\ProductName",
                         tr[0].lang, tr[0].cp);
            if (VerQueryValueW(buf, q, (void**)&v, &vl) && v && vl) to_utf8(v, prod, sizeof(prod));
        }
        _snprintf_s(out, cch, _TRUNCATE, "%s%s%s%s", ver,
                    prod[0] ? " (" : "", prod, prod[0] ? ")" : "");
    }
    free(buf);
}

// --- system ------------------------------------------------------------------
static void sec_system(Out* o)
{
    out_fmt(o, "[system]\n");
    typedef const char* (__cdecl *PFN_wine_ver)(void);
    PFN_wine_ver wv = (PFN_wine_ver)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                   "wine_get_version");
    out_fmt(o, "wine: %s\n", wv ? wv() : "no (native Windows)");
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    out_fmt(o, "cpus: %lu\n", (unsigned long)si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out_fmt(o, "ram: %llu MB total, %llu MB free\n",
                ms.ullTotalPhys >> 20, ms.ullAvailPhys >> 20);
        // pol.exe is a 32-bit process: running out of ADDRESS SPACE, not RAM,
        // is a real way for a title to stop drawing.
        out_fmt(o, "address_space: %llu MB total, %llu MB free\n",
                ms.ullTotalVirtual >> 20, ms.ullAvailVirtual >> 20);
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULONGLONG a = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
        ULONGLONG b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        out_fmt(o, "process_uptime: %llus\n", (b - a) / 10000000ULL);
    }
    out_fmt(o, "\n");
}

// --- GPUs --------------------------------------------------------------------
static void reg_str(HKEY root, const wchar_t* sub, const wchar_t* name, char* out, size_t cch)
{
    out[0] = 0;
    wchar_t w[256]; DWORD cb = sizeof(w), type = 0;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, &type, w, &cb) == ERROR_SUCCESS)
        to_utf8(w, out, cch);
}

static void sec_gpus(Out* o)
{
    out_fmt(o, "[gpus]\n");
    wchar_t seen[8][128]; int nseen = 0;
    for (DWORD i = 0; i < 16; i++) {
        DISPLAY_DEVICEW dd; ZeroMemory(&dd, sizeof(dd)); dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        // One adapter is listed once per OUTPUT; report each adapter once.
        bool dup = false;
        for (int s = 0; s < nseen; s++) if (!wcscmp(seen[s], dd.DeviceString)) dup = true;
        if (dup && !(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        if (!dup && nseen < 8) wcsncpy_s(seen[nseen++], dd.DeviceString, _TRUNCATE);
        if (dup) continue;
        char name[256]; to_utf8(dd.DeviceString, name, sizeof(name));
        char drv[64] = "", date[64] = "";
        // DeviceKey is "\Registry\Machine\System\...\Video\{guid}\0000".
        const wchar_t* pfx = L"\\Registry\\Machine\\";
        if (!_wcsnicmp(dd.DeviceKey, pfx, wcslen(pfx))) {
            const wchar_t* sub = dd.DeviceKey + wcslen(pfx);
            reg_str(HKEY_LOCAL_MACHINE, sub, L"DriverVersion", drv, sizeof(drv));
            reg_str(HKEY_LOCAL_MACHINE, sub, L"DriverDate", date, sizeof(date));
        }
        out_fmt(o, "gpu: %s | driver %s%s%s%s%s\n", name, drv[0] ? drv : "?",
                date[0] ? " (" : "", date, date[0] ? ")" : "",
                (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " | primary" : "");
    }
    out_fmt(o, "\n");
}

// --- monitors and DPI --------------------------------------------------------
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
typedef HRESULT (WINAPI *PFN_GetProcessDpiAwareness)(HANDLE, int*);
typedef HANDLE  (WINAPI *PFN_SetThreadDpiAwarenessContext)(HANDLE);
typedef HANDLE  (WINAPI *PFN_GetThreadDpiAwarenessContext)(void);
typedef int     (WINAPI *PFN_GetAwarenessFromDpiAwarenessContext)(HANDLE);

static PFN_GetDpiForMonitor g_GetDpiForMonitor;

struct MonWalk { Out* o; int n; HMONITOR mons[16]; };

static BOOL CALLBACK mon_proc(HMONITOR m, HDC, LPRECT, LPARAM lp)
{
    MonWalk* w = (MonWalk*)lp;
    MONITORINFOEXW mi; ZeroMemory(&mi, sizeof(mi)); mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, &mi)) return TRUE;
    UINT dx = 0, dy = 0;
    if (g_GetDpiForMonitor) g_GetDpiForMonitor(m, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy);
    DEVMODEW dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
    bool have_mode = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) != 0;
    RECT r = mi.rcMonitor;
    out_fmt(w->o, "monitor %d: %ldx%ld at (%ld,%ld)%s | scaling %u%% (dpi %u)",
            w->n, r.right - r.left, r.bottom - r.top, r.left, r.top,
            (mi.dwFlags & MONITORINFOF_PRIMARY) ? " primary" : "",
            dx ? dx * 100 / 96 : 0, dx);
    if (have_mode)
        out_fmt(w->o, " | mode %lux%lu %luHz %lubpp", dm.dmPelsWidth, dm.dmPelsHeight,
                dm.dmDisplayFrequency, dm.dmBitsPerPel);
    out_fmt(w->o, "\n");
    if (w->n < 16) w->mons[w->n] = m;
    w->n++;
    return TRUE;
}

static const char* awareness_name(int a)
{
    switch (a) {
    case 0: return "unaware";
    case 1: return "system-aware";
    case 2: return "per-monitor";
    default: return "?";
    }
}

static MonWalk g_mw;

static void sec_monitors(Out* o)
{
    out_fmt(o, "[display]\n");
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    g_GetDpiForMonitor = shcore ? (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor") : NULL;
    PFN_GetProcessDpiAwareness gpa = shcore ?
        (PFN_GetProcessDpiAwareness)GetProcAddress(shcore, "GetProcessDpiAwareness") : NULL;
    PFN_SetThreadDpiAwarenessContext stdac = (PFN_SetThreadDpiAwarenessContext)
        GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    PFN_GetThreadDpiAwarenessContext gtdac = (PFN_GetThreadDpiAwarenessContext)
        GetProcAddress(u32, "GetThreadDpiAwarenessContext");
    PFN_GetAwarenessFromDpiAwarenessContext gafc = (PFN_GetAwarenessFromDpiAwarenessContext)
        GetProcAddress(u32, "GetAwarenessFromDpiAwarenessContext");

    int pa = -1;
    if (gpa) gpa(NULL, &pa);
    out_fmt(o, "process_dpi_awareness: %s\n", awareness_name(pa));
    if (gtdac && gafc)
        out_fmt(o, "report_thread_dpi_awareness: %s\n", awareness_name(gafc(gtdac())));

    // Per-monitor for the walk, so the scaling figures are the real ones.
    HANDLE prev = NULL;
    if (stdac) prev = stdac((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    ZeroMemory(&g_mw, sizeof(g_mw)); g_mw.o = o;
    EnumDisplayMonitors(NULL, NULL, mon_proc, (LPARAM)&g_mw);
    if (stdac && prev) stdac(prev);
    out_fmt(o, "virtual_screen: %dx%d\n", GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN));
    out_fmt(o, "\n");
}

// --- compatibility settings on pol.exe -----------------------------------------
// "Run in compatibility mode", "Override high DPI scaling", "Reduced colour
// mode" and friends are stored as a layer string keyed by the exe's full path.
// They change how every title in the process draws, and players set them from
// old forum advice without remembering they did.
static void sec_compat(Out* o)
{
    out_fmt(o, "[compat]\n");
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    char ap[MAX_PATH * 3]; anon_path(exe, ap, sizeof(ap));
    out_fmt(o, "exe: %s\n", ap);
    const wchar_t* sub = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    struct { HKEY root; const char* name; } roots[] = {
        { HKEY_CURRENT_USER, "user" }, { HKEY_LOCAL_MACHINE, "machine" } };
    bool any = false;
    for (int i = 0; i < 2; i++) {
        char v[512] = "";
        reg_str(roots[i].root, sub, exe, v, sizeof(v));
        if (v[0]) { out_fmt(o, "layers_%s: %s\n", roots[i].name, v); any = true; }
    }
    if (!any) out_fmt(o, "layers: none\n");
    char env[256] = "";
    if (GetEnvironmentVariableA("__COMPAT_LAYER", env, sizeof(env)))
        out_fmt(o, "__COMPAT_LAYER: %s\n", env);
    out_fmt(o, "\n");
}

// --- modules -------------------------------------------------------------------
// The graphics DLLs actually loaded (which d3d8.dll: Windows' own, or a DXVK
// copy beside the game), and every module from OUTSIDE the Windows directory,
// which is where overlays live. Known overlays are named, because they are the
// first suspect for a black or frozen game window.
static const struct { const char* leaf; const char* what; } kOverlays[] = {
    { "gameoverlayrenderer.dll", "Steam overlay" },
    { "rtsshooks.dll",           "RivaTuner / MSI Afterburner overlay" },
    { "discordhook.dll",         "Discord overlay" },
    { "discord_overlay.dll",     "Discord overlay" },
    { "nvspcap.dll",             "NVIDIA ShadowPlay / overlay" },
    { "nvcamera32.dll",          "NVIDIA Ansel / Freestyle" },
    { "gfexperiencecore.dll",    "NVIDIA GeForce Experience" },
    { "igo32.dll",               "EA / Origin overlay" },
    { "overlay.dll",             "an overlay (generic name)" },
    { "reshade32.dll",           "ReShade" },
    { "dxgi.dll",                "a DXGI wrapper or overlay (check the path)" },
    { "obs-graphics-hook32.dll", "OBS game capture" },
    { "graphics-hook32.dll",     "OBS game capture" },
    { "bdcamvk32.dll",           "Bandicam" },
    { "fraps32.dll",             "FRAPS" },
};

static void sec_modules(Out* o)
{
    out_fmt(o, "[modules]\n");
    wchar_t windir[MAX_PATH] = L"";
    GetWindowsDirectoryW(windir, MAX_PATH);
    size_t wl = wcslen(windir);

    const wchar_t* gfx[] = { L"d3d8.dll", L"d3d9.dll", L"ddraw.dll", L"dxgi.dll",
                             L"d3d11.dll", L"opengl32.dll", L"dinput8.dll" };
    for (size_t i = 0; i < sizeof(gfx) / sizeof(gfx[0]); i++) {
        HMODULE m = GetModuleHandleW(gfx[i]);
        char leaf[64]; to_utf8(gfx[i], leaf, sizeof(leaf));
        if (!m) { out_fmt(o, "%s: not loaded\n", leaf); continue; }
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(m, path, MAX_PATH);
        char ap[MAX_PATH * 3]; anon_path(path, ap, sizeof(ap));
        char ver[200]; file_version(path, ver, sizeof(ver));
        bool sys = wl && !_wcsnicmp(path, windir, wl);
        out_fmt(o, "%s: %s | %s | %s\n", leaf, ap, ver[0] ? ver : "no version info",
                sys ? "Windows' own" : "NOT the Windows copy (a wrapper such as DXVK?)");
    }

    HMODULE mods[512]; DWORD need = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD cnt = need / sizeof(HMODULE);
        if (cnt > 512) cnt = 512;
        int nonwin = 0;
        for (DWORD i = 0; i < cnt; i++) {
            wchar_t path[MAX_PATH] = L"";
            if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
            if (wl && !_wcsnicmp(path, windir, wl)) continue;
            char ap[MAX_PATH * 3]; anon_path(path, ap, sizeof(ap));
            char leaf[128]; to_utf8(leaf_of(path), leaf, sizeof(leaf));
            _strlwr_s(leaf);
            const char* ov = NULL;
            for (size_t k = 0; k < sizeof(kOverlays) / sizeof(kOverlays[0]); k++)
                if (!strcmp(leaf, kOverlays[k].leaf)) ov = kOverlays[k].what;
            out_fmt(o, "module: %s%s%s\n", ap, ov ? "   <-- " : "", ov ? ov : "");
            nonwin++;
        }
        out_fmt(o, "modules_total: %lu (%d from outside the Windows directory)\n",
                (unsigned long)cnt, nonwin);
    }
    out_fmt(o, "\n");
}

// --- windows -------------------------------------------------------------------
static int monitor_index(HWND h)
{
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
    for (int i = 0; i < g_mw.n && i < 16; i++) if (g_mw.mons[i] == m) return i;
    return -1;
}

static BOOL CALLBACK win_proc(HWND h, LPARAM lp)
{
    Out* o = (Out*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t cls[96] = L"", title[128] = L"";
    GetClassNameW(h, cls, _countof(cls));
    GetWindowTextW(h, title, _countof(title));
    char c8[200], t8[300]; to_utf8(cls, c8, sizeof(c8)); to_utf8(title, t8, sizeof(t8));
    RECT wr = {0}, cr = {0};
    GetWindowRect(h, &wr); GetClientRect(h, &cr);
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    out_fmt(o, "window %p: class=\"%s\" title=\"%s\" %s%s%s%s%s rect=(%ld,%ld %ldx%ld) "
               "client=%ldx%ld style=%08lX ex=%08lX monitor=%d\n",
            (void*)h, c8, t8,
            IsWindowVisible(h) ? "visible" : "hidden",
            IsIconic(h) ? " minimized" : "", IsZoomed(h) ? " maximized" : "",
            (ex & WS_EX_TOPMOST) ? " topmost" : "", (ex & WS_EX_LAYERED) ? " layered" : "",
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
            cr.right, cr.bottom, (unsigned long)st, (unsigned long)ex, monitor_index(h));
    return TRUE;
}

static void sec_windows(Out* o)
{
    out_fmt(o, "[windows]\n");
    EnumWindows(win_proc, (LPARAM)o);
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (!fg) {
        out_fmt(o, "foreground: none\n");
    } else if (pid == GetCurrentProcessId()) {
        out_fmt(o, "foreground: %p (this process)\n", (void*)fg);
    } else {
        // ANOTHER program's window: class and exe name only, never its title.
        wchar_t cls[96] = L""; GetClassNameW(fg, cls, _countof(cls));
        char c8[200]; to_utf8(cls, c8, sizeof(c8));
        char exe8[128] = "?";
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (ph) {
            wchar_t p[MAX_PATH]; DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(ph, 0, p, &n)) to_utf8(leaf_of(p), exe8, sizeof(exe8));
            CloseHandle(ph);
        }
        out_fmt(o, "foreground: ANOTHER program (%s, class \"%s\") -- the game is not in front\n",
                exe8, c8);
    }
    out_fmt(o, "\n");
}

// --- entry -----------------------------------------------------------------------
char* sysdiag_collect(DWORD* out_len)
{
    Out o;
    o.cap = 96 * 1024; o.len = 0;
    o.p = (char*)malloc(o.cap);
    if (!o.p) return NULL;
    o.p[0] = 0;
    if (!GetEnvironmentVariableW(L"USERPROFILE", g_profile, MAX_PATH)) g_profile[0] = 0;

    out_fmt(&o, "CrystalMod diagnostics, collected when the report key was pressed.\n\n");
    sec_system(&o);
    sec_gpus(&o);
    sec_monitors(&o);
    sec_compat(&o);
    sec_windows(&o);
    sec_modules(&o);

    out_fmt(&o, "[d3d8]\n");
    if (o.len + 4096 < o.cap) {
        // One second of frame counting: the difference between "stopped" and
        // "drawing into nothing" (see d3d8_diag_text).
        int w = d3d8_diag_text(o.p + o.len, o.cap - o.len, 1000);
        if (w > 0) o.len += (size_t)w;
    }
    if (out_len) *out_len = (DWORD)o.len;
    return o.p;
}
