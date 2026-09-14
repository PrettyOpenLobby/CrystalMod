// regredir.cpp -- make a COPIED PlayOnline install FOLDER-AWARE.
//
// WHY THIS EXISTS
//
// The Viewer finds its own tree from the REGISTRY, not from where pol.exe sits.
// pol.exe, util\startpol.exe, polcore.dll and app.dll all read
//     HKLM\SOFTWARE\PlayOnlineUS\InstallFolder     (value "1000" = the Viewer,
//     "0001" = FFXI, ...)  via RegQueryValueEx -- A in startpol/polcore, A+W in
// pol.exe/app.dll. That key is MACHINE-GLOBAL and points at the PRIMARY install
// in Program Files. So a SECOND install tree (E:\PolViewerSE, the SE-facing copy
// netredir uses) resolves "the Viewer" back to the primary, and its patch /
// "installation" step writes THERE -- which the client reports as "cannot write
// to the specified destination folder, check your privileges" (the running tree
// and the registered tree disagree), and which would corrupt the primary with
// SE's live patch even if the write had succeeded.
//
// This hook rewrites the InstallFolder read IN THIS PROCESS ONLY: any registry
// value that comes back rooted at the primary Viewer path (`from`) is re-rooted
// at this copy (`to`). No machine registry is touched, so the primary install is
// unaffected and both trees coexist -- exactly as netredir does for DNS. Off
// unless [installdir] enable=1, and the real function pointer stays NULL when
// off, so a normal install never gets this hook in any IAT.
//
// PREFIX-SUBSTITUTION, not value-name matching, is deliberate: it catches every
// read that returns the Viewer path -- the "1000" value, the ...\Settings key, a
// version stamp -- and, because only the Viewer path contains \PlayOnlineViewer,
// it leaves the title trees (0001 = FFXI, ...) pointing at the primary, which is
// correct: we never launch a game during a capture session.
//
// CONFIG ([installdir] in polshim.ini, next to the DLL):
//   enable=1
//   from=   (optional) the primary Viewer path to replace. Blank -> auto: the
//           registered HKLM\SOFTWARE\PlayOnline{US,,EU}\InstallFolder "1000".
//   to=     (optional) this install's own root. Blank -> auto: the directory the
//           running exe lives in (the truly folder-aware default).

#include "polshim.h"

static int      g_on = 0;
static char     g_fromA[MAX_PATH];  static size_t g_fromA_len = 0;
static char     g_toA[MAX_PATH];
static wchar_t  g_fromW[MAX_PATH];   static size_t g_fromW_len = 0;
static wchar_t  g_toW[MAX_PATH];

typedef LONG (WINAPI *PFN_RQVA)(HKEY, LPCSTR,  LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *PFN_RQVW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *PFN_ROKA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *PFN_RCK)(HKEY);

static PFN_RQVA real_RQVA = NULL;
static PFN_RQVW real_RQVW = NULL;

int installdir_enabled() { return g_on; }

// [inputmode] swap_confirm -- swap the Viewer SHELL's Ok/Cancel (the A/B face
// buttons), which are DWORD button-indices under ...\PlayOnlineViewer\Settings\
// Controller. POL uses the PS2/JP layout (cross=cancel, circle=confirm), which feels
// reversed to Western players. Served IN-PROCESS (registry untouched); reverts by
// clearing the flag. Rides the same RegQueryValueEx hooks as installdir, which arm if
// EITHER is on. NOTE: shell only -- each title (FFXI, TM) has its own pad config.
//
// IMPORTANT: THE FAILURE MODE THIS HAS, reported from a Steam Deck 2026-08-14: the swap is
// PARASITIC on the sibling value already existing. If `Settings\Controller` has no
// `Ok`/`Cancel` REG_DWORDs -- which is the normal state of a fresh Proton prefix, for
// exactly the reason UseGameController is absent there (the prefs app is never run) --
// then the sibling read fails, we fall through, and swap_confirm=1 does NOTHING while
// looking correctly configured. Serving a swapped DEFAULT is not possible yet because
// POL's default button indices are unknown, and inventing them would be worse than
// doing nothing. So: measure first. swap_trace=1 logs every Ok/Cancel query and its
// verdict, and configure() dumps the whole Controller key once at startup, which turns
// "the swap does not work" into a specific fact in one launch.
//
// IMPORTANT: AND A SECOND, WORSE ONE, reported 2026-08-14: with the key present the swap fired
// but came out SHIFTED, not swapped (confirm landed on X, cancel on A). The hook keyed
// only on the VALUE NAMES -- any key anywhere holding `Ok`/`Cancel` DWORDs got swapped,
// and the comment below used to justify that with "effectively only Settings\Controller",
// which was an assumption and never a check. If the Viewer keeps more than one such
// mapping (per-context, or a keyboard block beside the pad one) we were rewriting all of
// them, and a partial rewrite across two tables reads exactly like a shift.
//
// So the swap is now SCOPED to keys whose path ends in [inputmode] swap_key (default
// `Controller`), and swap_trace logs the resolved key path, so which table was touched
// is a fact rather than an inference. btn_ok/btn_cancel serve literal indices for
// working out the right values empirically -- POL's own defaults are still unknown.
//
// RESOLVED: SOLVED 2026-08-15, and the swap was never the right tool. Two live observations
// pinned the index space: with the swap on, Ok=2 put confirm on X and Cancel=1 put
// cancel on B. So the indices are DirectInput XBOX order --
//     0 = A, 1 = B, 2 = X, 3 = Y
// while POL's stock values (Menu=0, Ok=1, Cancel=2, Navi=3) are PS2 FACE order
// (triangle, circle, cross, square). On a PS2 pad those line up; on an Xbox-layout pad
// they are rotated, so confirm sits on B and cancel on X and NO swap of two wrong
// indices can fix it -- swapping just moves the wrongness around, which is precisely
// what the user saw twice.
//
// The fix is to WRITE the indices, not exchange them: pad_layout=xbox sets
// Ok=0 (A), Cancel=1 (B), Navi=2 (X), Menu=3 (Y). Each is still individually
// overridable with btn_ok / btn_cancel / btn_navi / btn_menu.
static int g_swap  = 0;
static int g_swapt = 0;

// Per-action index overrides. -1 = leave alone. Order matters only for the log.
struct BtnOvr { const char* nameA; const wchar_t* nameW; const wchar_t* ini_key; int val; };
static BtnOvr g_btns[] = {
    { "Ok",     L"Ok",     L"btn_ok",     -1 },
    { "Cancel", L"Cancel", L"btn_cancel", -1 },
    { "Navi",   L"Navi",   L"btn_navi",   -1 },
    { "Menu",   L"Menu",   L"btn_menu",   -1 },
};
#define BTN_OK 0
#define BTN_CA 1

// Convenience for the report/log; the hook uses the table.
static int g_btn_ok = -1, g_btn_cancel = -1;
static wchar_t g_swapkey[128] = L"Controller,Controler";
int  swapconfirm_enabled() { return g_swap; }
// The hook must arm for an explicit btn_ override too, not just for the swap --
// otherwise btn_ok/btn_cancel are silently inert, which is the exact failure this
// whole section exists to stop repeating.
static bool swap_active()
{
    if (g_swap) return true;
    for (int i = 0; i < _countof(g_btns); i++) if (g_btns[i].val >= 0) return true;
    return false;
}

// Resolve an HKEY to its full path. There is no Win32 call for this; NtQueryKey with
// KeyNameInformation(3) is the standard route, resolved dynamically so a failure here
// degrades to "cannot scope" rather than breaking the hook.
typedef LONG (__stdcall *PFN_NTQK)(HANDLE, int, PVOID, ULONG, PULONG);
static PFN_NTQK g_ntqk = NULL;
static int      g_ntqk_tried = 0;

static bool key_path(HKEY h, wchar_t* out, size_t cch)
{
    if (!g_ntqk_tried) {
        g_ntqk_tried = 1;
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) g_ntqk = (PFN_NTQK)GetProcAddress(nt, "NtQueryKey");
    }
    if (!g_ntqk) return false;
    BYTE buf[1024]; ULONG got = 0;
    if (g_ntqk((HANDLE)h, 3 /*KeyNameInformation*/, buf, sizeof(buf), &got) != 0) return false;
    ULONG len = *(ULONG*)buf;                       // KEY_NAME_INFORMATION.NameLength, bytes
    ULONG chars = len / sizeof(wchar_t);
    if (chars >= cch) chars = (ULONG)cch - 1;
    memcpy(out, buf + sizeof(ULONG), chars * sizeof(wchar_t));
    out[chars] = 0;
    return true;
}

// Is this key's LEAF one of the names swap_key lists? A comma list, because SE shipped
// two spellings of the same key: `Controller` in the US hive and `Controler` -- one `l`
// -- in the JP one. Matching only the first left JP installs silently unswapped.
static bool swap_leaf_in_scope(const wchar_t* leaf)
{
    if (!g_swapkey[0]) return true;                 // swap_key= (empty) = any key
    wchar_t buf[128]; wcsncpy_s(buf, g_swapkey, _TRUNCATE);
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(buf, L",; 	", &ctx); t; t = wcstok_s(NULL, L",; 	", &ctx))
        if (_wcsicmp(leaf, t) == 0) return true;
    return false;
}

// True if this key is one we are allowed to swap in. An unresolvable path is treated as
// NOT in scope: silently swapping an unknown key is what produced the shift.
static bool swap_in_scope(HKEY h, wchar_t* pathout, size_t cch)
{
    pathout[0] = 0;
    if (!key_path(h, pathout, cch)) return false;
    const wchar_t* leaf = wcsrchr(pathout, L'\\');
    return swap_leaf_in_scope(leaf ? leaf + 1 : pathout);
}

// ---------------------------------------------------------------- report
//
// The swap has now failed twice for reasons that were invisible from outside (nothing
// to swap; swapping the wrong table). Both are questions about what is actually in the
// registry, so this answers them directly and puts it in front of the user instead of
// in a log they have to go find. It does NOT assume Settings\Controller is the right
// key -- it walks every subkey under Settings and reports which ones hold Ok/Cancel,
// which is the assumption that was wrong the first time.

static char*  g_srep;
static size_t g_srepleft;

static void srep(const char* fmt, ...)
{
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    logf("[swap] %s", line);
    if (g_srep && g_srepleft > 1) {
        size_t n = strlen(line);
        if (n + 2 > g_srepleft) n = g_srepleft - 2;
        memcpy(g_srep, line, n); g_srep += n; *g_srep++ = '\n'; *g_srep = 0;
        g_srepleft -= (n + 1);
    }
}

// Report every value in one key; returns how many Ok/Cancel DWORDs it holds.
static int report_key_values(HKEY h, const char* indent)
{
    int hits = 0;
    for (DWORD i = 0; ; i++) {
        wchar_t nm[256]; DWORD ncap = _countof(nm), type = 0, data = 0, dcap = sizeof(data);
        LONG r = RegEnumValueW(h, i, nm, &ncap, NULL, &type, (LPBYTE)&data, &dcap);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type == REG_DWORD) {
            srep("%s%-24ls = %lu", indent, nm, data);
            if (!_wcsicmp(nm, L"Ok") || !_wcsicmp(nm, L"Cancel")) hits++;
        } else {
            srep("%s%-24ls (type %lu)", indent, nm, type);
        }
    }
    return hits;
}

// Count Ok/Cancel REG_DWORDs in one key.
static int key_okcancel(HKEY h)
{
    int hits = 0;
    for (DWORD i = 0; ; i++) {
        wchar_t nm[256]; DWORD ncap = _countof(nm), type = 0, data = 0, dcap = sizeof(data);
        LONG r = RegEnumValueW(h, i, nm, &ncap, NULL, &type, (LPBYTE)&data, &dcap);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type == REG_DWORD && (!_wcsicmp(nm, L"Ok") || !_wcsicmp(nm, L"Cancel"))) hits++;
    }
    return hits;
}

// Walk a hive looking for keys that hold Ok/Cancel, instead of assuming where they are.
//
// IMPORTANT: ASSUMING cost this feature two rounds. The real path carries a PUBLISHER segment
// the old code did not know about:
//     PlayOnlineUS\SquareEnix\PlayOnlineViewer\Settings\Controller
// and the JP hive is PlayOnline\Square\...\Settings\Controler -- SE spelled it with ONE
// `l`. Every probe that hardcoded ...\PlayOnlineViewer\Settings\Controller therefore
// reported "no such key" on a machine where the key was sitting right there with
// Ok=1 / Cancel=2 in it. A bounded search finds them wherever they moved to.
static void scan_for_buttons(const wchar_t* path, int depth, int* total)
{
    if (depth > 6) return;
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &h) != ERROR_SUCCESS) return;

    if (key_okcancel(h)) {
        srep("HKLM\\%ls", path);
        report_key_values(h, "    ");
        const wchar_t* leaf = wcsrchr(path, L'\\'); leaf = leaf ? leaf + 1 : path;
        srep("    ^ %s", swap_leaf_in_scope(leaf)
                         ? "IN SCOPE -- this is the table swap_confirm rewrites"
                         : "NOT in scope -- add its name to [inputmode] swap_key to target it");
        srep("");
        (*total)++;
    }
    for (DWORD i = 0; ; i++) {
        wchar_t kn[256]; DWORD kcap = _countof(kn);
        if (RegEnumKeyExW(h, i, kn, &kcap, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wchar_t child[1024];
        _snwprintf_s(child, _countof(child), _TRUNCATE, L"%ls\\%ls", path, kn);
        scan_for_buttons(child, depth + 1, total);
    }
    RegCloseKey(h);
}

int swapconfirm_report(char* out, size_t cch)
{
    g_srep = out; g_srepleft = cch;
    if (out && cch) *out = 0;

    srep("swap_confirm = %d", g_swap);
    srep("swap_key     = '%ls'  (a key qualifies when its path ENDS with this)",
         g_swapkey[0] ? g_swapkey : L"(empty = any key)");
    static const char* FACE[] = { "A", "B", "X", "Y" };
    int anyovr = 0;
    for (int i = 0; i < _countof(g_btns); i++) {
        if (g_btns[i].val < 0) continue;
        anyovr = 1;
        srep("%-10ls = %d  (%s)", g_btns[i].ini_key, g_btns[i].val,
             g_btns[i].val < 4 ? FACE[g_btns[i].val] : "?");
    }
    if (!anyovr) srep("btn_ok / btn_cancel / btn_navi / btn_menu = -1  (no override; see pad_layout)");
    srep("index space: 0=A  1=B  2=X  3=Y  (DirectInput Xbox order, measured live)");
    srep("POL ships PS2 FACE order (Menu=0 Ok=1 Cancel=2 Navi=3), which on an Xbox-layout");
    srep("pad puts confirm on B and cancel on X. pad_layout=xbox rewrites them properly;");
    srep("swap_confirm cannot fix it, because exchanging two wrong indices stays wrong.");
    srep("");

    // SEARCH, do not assume. The button table sits under a PUBLISHER segment that every
    // hardcoded probe before this one missed:
    //     PlayOnlineUS\SquareEnix\PlayOnlineViewer\Settings\Controller     (Ok=1 Cancel=2)
    //     PlayOnline\Square\PlayOnlineViewer\Settings\Controler            (JP, one `l`)
    static const wchar_t* hives[] = { L"SOFTWARE\\PlayOnlineUS", L"SOFTWARE\\PlayOnline", L"SOFTWARE\\PlayOnlineEU" };
    int found_any = 0, total_hits = 0;
    for (int i = 0; i < 3; i++) {
        HKEY probe;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, hives[i], 0, KEY_READ, &probe) != ERROR_SUCCESS) continue;
        RegCloseKey(probe);
        found_any = 1;
        scan_for_buttons(hives[i], 0, &total_hits);
    }

    srep("");
    if (!found_any) {
        srep("No PlayOnline* hive at all -- nothing is installed where we can see it.");
    } else if (!total_hits) {
        srep("NOTHING holds Ok/Cancel values. swap_confirm works by serving the SIBLING");
        srep("value, so with none present it has nothing to swap and does nothing --");
        srep("this is the usual state of a fresh Proton prefix. Use btn_ok / btn_cancel");
        srep("to set the indices outright instead.");
    } else {
        srep("Set swap_trace=1 and relaunch to log every Ok/Cancel read the client makes,");
        srep("including which key it came from and whether we swapped it.");
    }
    g_srep = NULL;
    return total_hits;
}

// ---------------------------------------------------------------- what will the
// client actually READ for an action?
//
// padmap.cpp builds its permutation against the slot the client reads, so it has to ask
// the same two questions this file already answers for itself: is there a live override
// we are serving, and failing that, what does the registry hold. Exposed rather than
// duplicated, because a second implementation of "where is the Controller key" is
// exactly how this feature acquired its hardcoded-path bug the first time.

int polbtn_override_by_name(const wchar_t* nameW)
{
    if (!nameW) return -1;
    for (int i = 0; i < _countof(g_btns); i++)
        if (_wcsicmp(nameW, g_btns[i].nameW) == 0) return g_btns[i].val;
    return -1;
}

// Depth-bounded search for the first IN-SCOPE key holding Ok/Cancel, then the named
// value out of it. Returns -1 when there is none -- which is the normal state of a
// fresh Proton prefix and must be reported as a fact, not as a zero.
static int find_btn(const wchar_t* path, const wchar_t* nameW, int depth,
                    wchar_t* keyout, size_t cch)
{
    if (depth > 6) return -1;
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &h) != ERROR_SUCCESS) return -1;

    int found = -1;
    const wchar_t* leaf = wcsrchr(path, L'\\'); leaf = leaf ? leaf + 1 : path;
    if (key_okcancel(h) && swap_leaf_in_scope(leaf)) {
        DWORD type = 0, data = 0, cb = sizeof(data);
        // Through the SAVED pointer, never the patched export: this must report what is
        // genuinely on disk, and the hook would hand back our own override -- collapsing
        // the two sources padmap needs to tell apart into one.
        LONG rc = real_RQVW ? real_RQVW(h, nameW, NULL, &type, (LPBYTE)&data, &cb)
                            : RegQueryValueExW(h, nameW, NULL, &type, (LPBYTE)&data, &cb);
        if (rc == ERROR_SUCCESS && type == REG_DWORD) {
            found = (int)data;
            if (keyout && cch) _snwprintf_s(keyout, cch, _TRUNCATE, L"HKLM\\%ls\\%ls", path, nameW);
        }
    }
    for (DWORD i = 0; found < 0; i++) {
        wchar_t kn[256]; DWORD kcap = _countof(kn);
        if (RegEnumKeyExW(h, i, kn, &kcap, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wchar_t child[1024];
        _snwprintf_s(child, _countof(child), _TRUNCATE, L"%ls\\%ls", path, kn);
        found = find_btn(child, nameW, depth + 1, keyout, cch);
    }
    RegCloseKey(h);
    return found;
}

int polbtn_registry(const wchar_t* nameW, wchar_t* keyout, size_t cch)
{
    if (keyout && cch) keyout[0] = 0;
    if (!nameW) return -1;
    static const wchar_t* hives[] = { L"SOFTWARE\\PlayOnlineUS", L"SOFTWARE\\PlayOnline",
                                      L"SOFTWARE\\PlayOnlineEU" };
    for (int i = 0; i < _countof(hives); i++) {
        int v = find_btn(hives[i], nameW, 0, keyout, cch);
        if (v >= 0) return v;
    }
    return -1;
}

// Startup probe: the same SEARCH the report does, straight into the log, so a launch
// with swap_confirm=1 always records where the button table is (or that there is none).
// This used to hardcode ...\PlayOnlineViewer\Settings\Controller and therefore reported
// "no such key" on machines where the key exists one publisher segment deeper.
static void swapconfirm_probe()
{
    // NB the backslashes: written with single ones once, which made these
    // "SOFTWAREPlayOnlineUS" and the probe silently scanned nothing at all.
    static const wchar_t* hives[] = { L"SOFTWARE\\PlayOnlineUS", L"SOFTWARE\\PlayOnline", L"SOFTWARE\\PlayOnlineEU" };
    int total = 0;
    g_srep = NULL;                       // log only
    for (int i = 0; i < 3; i++) scan_for_buttons(hives[i], 0, &total);
    if (!total)
        logf("[swap] probe: no key anywhere under the PlayOnline hives holds Ok/Cancel -- "
             "swap_confirm has nothing to swap. Use btn_ok/btn_cancel to set them outright.");
}

// ---------------------------------------------------------------- force_pad
//
// [inputmode] force_pad -- serve a title's own "use the gamepad" registry flag as ON,
// in-process, whatever is on disk. Registry and disk untouched; clearing the flag
// reverts it. Rides the same RegQueryValueEx hooks as installdir and swap_confirm.
//
// WHY THIS EXISTS. On the Steam Deck the pad drives the Viewer shell fine, but Tetra
// Master ignored it completely -- and pad_remap had nothing to do with it, because TM
// never created a joystick device at all. Measured in the live prefix 2026-08-23:
// HKLM\SOFTWARE\WOW6432Node\PlayOnlineUS\SquareEnix\TetraMaster has PAD=0, and PAD is
// TM's master switch:
//
//   TM.dll RVA 0x187093  RegQueryValueExA(hKey,"PAD",...) -> local, default 0 on failure
//   TM.dll RVA 0x1870CE  store into the global at RVA 0x2FF240
//   TM.dll RVA 0x186071  cmp <global>, 0 / je -- and the je SKIPS THE ENTIRE joystick
//                        setup: JOYPADGUIDENABLE, JOYPADGUID, CreateDevice, and the
//                        SetDataFormat(DIJOYSTATE @ RVA 0x20C7AC) that pad_slot maps.
//
// So PAD=0 is not "no buttons bound", it is "no pad exists". It is also the state a
// fresh Proton prefix is left in, which is why this read as a mapping bug twice.
//
// Serving PAD=1 is sufficient on its own -- the GUID pinning downstream is NOT a second
// gate. At RVA 0x1861B3 a zero JOYPADGUIDENABLE jumps straight to the enumerate-and-take-
// the-first-pad loop at RVA 0x186256, and a non-zero one whose CreateDevice on the stored
// GUID fails falls through into that same loop. Both roads reach a device, so the Deck's
// incoherent state (JOYPADGUIDENABLE=1 with JOYPADGUID absent) still works out.
//
// SCOPE. Each row names a SIBLING value that must exist as a REG_DWORD in the same key
// before we serve anything. `PAD` is a three-letter name that could plausibly live in
// somebody else's key; `JOYPADGUIDENABLE` beside it is TM's registry and nothing else.
// Same discipline swap_confirm uses, for the same reason -- an earlier unscoped rewrite
// matched on value NAME alone and hit keys it had no business in.
//
// ONLY TETRA MASTER HAS A ROW, and that is a measurement, not an oversight. FFXI, FMO
// and FE own their pad config through their own shipped config .exes (padsin000 /
// GamePadAssin0 / JBTN_*) and no equivalent master switch has been measured for them.
// A guessed row here would be indistinguishable from a fix that does nothing. Measure
// first: force_pad_trace=1 logs every gate read the client makes, scoped or not.
struct PadGate {
    const char*    nameA;   // the gate value
    const wchar_t* nameW;
    const char*    sibA;    // sibling REG_DWORD that must exist in the SAME key
    const wchar_t* sibW;
    DWORD          on;      // what to serve
    const char*    title;
};
static PadGate g_padgates[] = {
    { "PAD", L"PAD", "JOYPADGUIDENABLE", L"JOYPADGUIDENABLE", 1, "Tetra Master" },
};

static int g_forcepad = 0, g_padtrace = 0;
int forcepad_enabled() { return g_forcepad; }

static const PadGate* padgate_find(const char* nameA, const wchar_t* nameW)
{
    if (!g_forcepad) return NULL;
    for (int i = 0; i < _countof(g_padgates); i++) {
        bool hit = nameA ? (_stricmp(nameA, g_padgates[i].nameA) == 0)
                         : (_wcsicmp(nameW, g_padgates[i].nameW) == 0);
        if (hit) return &g_padgates[i];
    }
    return NULL;
}

// Serve a DWORD through the caller's buffer, honouring RegQueryValueEx's contract for a
// NULL buffer (size query) and a short one (ERROR_MORE_DATA).
static LONG serve_dword(DWORD out, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    if (lpType) *lpType = REG_DWORD;
    if (!lpData) { if (lpcbData) *lpcbData = sizeof(DWORD); return ERROR_SUCCESS; }
    if (!lpcbData) return ERROR_INVALID_PARAMETER;
    DWORD cap = *lpcbData; *lpcbData = sizeof(DWORD);
    if (cap < sizeof(DWORD)) return ERROR_MORE_DATA;
    memcpy(lpData, &out, sizeof(DWORD));
    return ERROR_SUCCESS;
}

// ---------------------------------------------------------------- seed_pad
//
// [inputmode] seed_pad -- serve a WORKING pad configuration for a value the title
// asks for and the registry does not have. Registry and disk untouched.
//
// SEED, NEVER OVERRIDE. A row fires only when the real RegQueryValueEx says the
// value is ABSENT. If the key holds anything at all -- even a factory default the
// user never touched -- we pass through. That is the whole safety argument: a fresh
// Proton prefix gets controls that work, and anybody who has ever opened a config
// app keeps exactly what they set. It also means a wrong row here is a table edit,
// never a setting of somebody's we destroyed.
//
// This is the piece force_pad cannot do on its own. force_pad is scoped by a SIBLING
// value existing in the same key, so on a genuinely fresh prefix -- where the key was
// just created and holds nothing -- it correctly declines to fire. Seeding is what
// makes that case work, which is why `PAD` appears in both tables.
//
// The values are MEASURED, not invented: dumped from a prefix configured by hand
// through SE's own config apps (2026-08-23), which are the only supported writers.
// Fantasy Earth is deliberately absent -- its pad config is NOT in the registry at
// all (no JBTN_* anywhere in the prefix; it lives in FantasyEarth/Settings/), so it
// needs a different mechanism and a guessed row here would do nothing.
//
// SCOPE. Every row names a key-path SUFFIX that the resolved key must end with, so a
// value of the same name in an unrelated key is passed straight through. An
// unresolvable path counts as out of scope -- the same rule swap_confirm learned the
// hard way.
struct PadSeed {
    const wchar_t* keysuffix;   // resolved key path must END with this
    const char*    nameA;
    const wchar_t* nameW;
    DWORD          type;        // REG_DWORD or REG_SZ
    DWORD          dw;          // REG_DWORD payload
    const char*    szA;         // REG_SZ payload
    const wchar_t* szW;
    const char*    title;
};

#define SEED_D(suffix, name, val, title) \
    { suffix, name, L##name, REG_DWORD, val, NULL, NULL, title }
#define SEED_S(suffix, name, val, title) \
    { suffix, name, L##name, REG_SZ, 0, val, L##val, title }

static PadSeed g_padseeds[] = {
    // Tetra Master. PAD is the master switch (see force_pad); the eight B_* slots are
    // the button map, and they are 1-BASED, which is why OK=2 is the pad's button 1.
    SEED_D(L"SquareEnix\\TetraMaster", "PAD",            1,  "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_00OK",         2,  "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_01CANCEL",     3,  "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_02PAGEDOWN",   5,  "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_03PAGEUP",     6,  "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_04CHAT",       9,  "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_05PLAYONLINE", 10, "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_06FRIENDLIST", 4,  "Tetra Master"),
    SEED_D(L"SquareEnix\\TetraMaster", "B_07MENU",       1,  "Tetra Master"),

    // Front Mission Online. 24 comma-separated tokens in the dialog's own action
    // order; the first two are Confirm and Cancel.
    //
    // KEY: The tokens are 1-BASED over DirectInput's 0-based button order, so
    // button1 = A. FMO's reader (RVA 0x80160) strtok's on "," and atoi's each token
    // straight into a byte with NO decrement, and 0xFF means unassigned -- so the
    // off-by-one lives in the numbering, not the code. Measured live: a stored 2 put
    // Confirm on B and a stored 3 put it on X. Hence 1,2,3,4 = A,B,X,Y.
    SEED_S(L"SQUARE ENIX\\FrontMissionOnline", "GamePadAssin0",
           "1,2,3,4,5,7,6,8,11,12,9,10,36,35,34,33,46,45,44,43,49,50,51,52,",
           "Front Mission Online"),
    SEED_S(L"SQUARE ENIX\\FrontMissionOnline", "GamePadFlip", "1,1,1,1",
           "Front Mission Online"),

    // FFXI. padsin000 is a comma-separated STRING, not the 256-byte blob the older
    // notes claimed -- measured off a live prefix. NOTE FFXI may read its pad through
    // SE's shipped xinputdll.dll and never touch DirectInput, so a correct table here
    // is necessary but may not be sufficient; establish which path is live first.
    SEED_S(L"SquareEnix\\FinalFantasyXI", "padmode000", "1,1,0,0,0,1", "FFXI"),
    SEED_S(L"SquareEnix\\FinalFantasyXI", "padsin000",
           "8,9,13,12,10,0,1,3,2,15,-1,-1,14,-33,-33,32,32,-36,-36,35,35,6,7,5,4,11,-1",
           "FFXI"),

    // The Viewer shell. Its four slots are 0-based, unlike Tetra Master's.
    SEED_D(L"PlayOnlineViewer\\Settings\\Controller", "Ok",     1, "Viewer shell"),
    SEED_D(L"PlayOnlineViewer\\Settings\\Controller", "Cancel", 2, "Viewer shell"),
    SEED_D(L"PlayOnlineViewer\\Settings\\Controller", "Navi",   3, "Viewer shell"),
    SEED_D(L"PlayOnlineViewer\\Settings\\Controller", "Menu",   0, "Viewer shell"),
};

static int g_seedpad = 0, g_seedtrace = 0;
int seedpad_enabled() { return g_seedpad; }

// True if `path` ends with `suffix` (case-insensitive).
static bool path_ends_with(const wchar_t* path, const wchar_t* suffix)
{
    size_t lp = wcslen(path), ls = wcslen(suffix);
    return lp >= ls && _wcsicmp(path + (lp - ls), suffix) == 0;
}

static const PadSeed* padseed_find(HKEY hKey, const char* nameA, const wchar_t* nameW)
{
    if (!g_seedpad) return NULL;
    wchar_t kp[512];
    bool resolved = key_path(hKey, kp, _countof(kp));
    for (int i = 0; i < _countof(g_padseeds); i++) {
        const PadSeed& r = g_padseeds[i];
        bool hit = nameA ? (_stricmp(nameA, r.nameA) == 0)
                         : (_wcsicmp(nameW, r.nameW) == 0);
        if (!hit) continue;
        if (!resolved) return NULL;             // cannot scope -> do not seed
        if (path_ends_with(kp, r.keysuffix)) return &r;
    }
    return NULL;
}

// Serve REG_SZ through the caller's buffer. The A and W forms differ in byte count,
// so they cannot share one helper -- getting that wrong hands the client a string
// half the length it asked about.
static LONG serve_szA(const char* v, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    DWORD need = (DWORD)strlen(v) + 1;
    if (lpType) *lpType = REG_SZ;
    if (!lpData) { if (lpcbData) *lpcbData = need; return ERROR_SUCCESS; }
    if (!lpcbData) return ERROR_INVALID_PARAMETER;
    DWORD cap = *lpcbData; *lpcbData = need;
    if (cap < need) return ERROR_MORE_DATA;
    memcpy(lpData, v, need);
    return ERROR_SUCCESS;
}

static LONG serve_szW(const wchar_t* v, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    DWORD need = (DWORD)((wcslen(v) + 1) * sizeof(wchar_t));
    if (lpType) *lpType = REG_SZ;
    if (!lpData) { if (lpcbData) *lpcbData = need; return ERROR_SUCCESS; }
    if (!lpcbData) return ERROR_INVALID_PARAMETER;
    DWORD cap = *lpcbData; *lpcbData = need;
    if (cap < need) return ERROR_MORE_DATA;
    memcpy(lpData, v, need);
    return ERROR_SUCCESS;
}

void swapconfirm_configure(const wchar_t* ini)
{
    g_swap       = GetPrivateProfileIntW(L"inputmode", L"swap_confirm", 0, ini);
    g_swapt      = GetPrivateProfileIntW(L"inputmode", L"swap_trace",   0, ini);
    g_forcepad   = GetPrivateProfileIntW(L"inputmode", L"force_pad",       0, ini);
    g_seedpad    = GetPrivateProfileIntW(L"inputmode", L"seed_pad",        1, ini);
    g_seedtrace  = GetPrivateProfileIntW(L"inputmode", L"seed_pad_trace",  0, ini);
    if (g_seedpad)
        logf("[seedpad] on -- %d measured pad value(s) available to titles whose "
             "registry does not have them (absent ONLY; an existing value always wins)",
             (int)_countof(g_padseeds));
    g_padtrace   = GetPrivateProfileIntW(L"inputmode", L"force_pad_trace", 0, ini);
    if (g_forcepad)
        logf("[forcepad] on -- serving %d title pad gate(s) as ENABLED "
             "(registry untouched; clear force_pad to revert)", (int)_countof(g_padgates));

    // pad_layout is a PRESET of the four indices; an explicit btn_* still wins over it,
    // so the preset is a starting point and never a straitjacket.
    wchar_t layout[32];
    ini_str(L"inputmode", L"pad_layout", L"off", layout, _countof(layout), ini);
    if (!_wcsicmp(layout, L"xbox")) {
        // Measured, not guessed: with the swap on, Ok=2 landed on X and Cancel=1 on B,
        // which fixes the index space as DirectInput Xbox order 0=A 1=B 2=X 3=Y.
        g_btns[BTN_OK].val = 0;   // A = confirm
        g_btns[BTN_CA].val = 1;   // B = cancel
        g_btns[2].val      = 2;   // Navi -> X
        g_btns[3].val      = 3;   // Menu -> Y
    } else if (!_wcsicmp(layout, L"ps")) {
        g_btns[BTN_OK].val = 1; g_btns[BTN_CA].val = 2; g_btns[2].val = 3; g_btns[3].val = 0;
    } else {
        for (int i = 0; i < _countof(g_btns); i++) g_btns[i].val = -1;
    }
    for (int i = 0; i < _countof(g_btns); i++) {
        int v = GetPrivateProfileIntW(L"inputmode", g_btns[i].ini_key, -2, ini);
        if (v != -2) g_btns[i].val = v;        // -1 in the ini explicitly clears the preset
    }
    g_btn_ok     = g_btns[BTN_OK].val;
    g_btn_cancel = g_btns[BTN_CA].val;
    // Default must list BOTH spellings -- the JP hive's key is `Controler`, one `l`.
    // (This default and g_swapkey's initialiser have to agree; they did not, and an ini
    // with no swap_key= silently lost the JP key.)
    ini_str(L"inputmode", L"swap_key", L"Controller,Controler",
            g_swapkey, _countof(g_swapkey), ini);
    if (g_swap || g_btn_ok >= 0 || g_btn_cancel >= 0) {
        logf("[swap] scope='%ls'%s%s", g_swapkey[0] ? g_swapkey : L"(ANY key -- unscoped)",
             g_btn_ok     >= 0 ? " btn_ok set"     : "",
             g_btn_cancel >= 0 ? " btn_cancel set" : "");
        swapconfirm_probe();
    }
}

// Explicit override wins over the swap: serve a literal index for a named action.
// Returns the value to serve, or -1 for "not overridden".
static int btn_override(const char* nameA, const wchar_t* nameW)
{
    for (int i = 0; i < _countof(g_btns); i++) {
        bool hit = nameA ? (_stricmp(nameA, g_btns[i].nameA) == 0)
                         : (_wcsicmp(nameW, g_btns[i].nameW) == 0);
        if (hit) return g_btns[i].val;
    }
    return -1;
}

// Does this value name have a sibling to swap with? Only the confirm/cancel pair does.
static const char*    swap_sibA(const char* n)
{ return _stricmp(n,"Ok")==0 ? "Cancel" : _stricmp(n,"Cancel")==0 ? "Ok" : NULL; }
static const wchar_t* swap_sibW(const wchar_t* n)
{ return _wcsicmp(n,L"Ok")==0 ? L"Cancel" : _wcsicmp(n,L"Cancel")==0 ? L"Ok" : NULL; }

// Is this a value we might rewrite at all? (Any overridable action, or the swap pair.)
static bool btn_interesting(const char* nameA, const wchar_t* nameW)
{
    for (int i = 0; i < _countof(g_btns); i++)
        if (nameA ? (_stricmp(nameA, g_btns[i].nameA) == 0)
                  : (_wcsicmp(nameW, g_btns[i].nameW) == 0)) return true;
    return false;
}

// The value being read is a string ROOTED AT `from` iff it equals `from` or
// continues with a path separator -- so `...\PlayOnlineViewer` matches, and a
// hypothetical `...\PlayOnlineViewerX` sibling does not.
static int rooted_A(const char* s)
{
    if (_strnicmp(s, g_fromA, g_fromA_len) != 0) return 0;
    char c = s[g_fromA_len];
    return c == '\0' || c == '\\' || c == '/';
}
static int rooted_W(const wchar_t* s)
{
    if (_wcsnicmp(s, g_fromW, g_fromW_len) != 0) return 0;
    wchar_t c = s[g_fromW_len];
    return c == L'\0' || c == L'\\' || c == L'/';
}

// --- the hooks. Serve any InstallFolder-rooted string from `to`; everything
// else is a transparent pass-through to the real function with the caller's own
// arguments, so behaviour is byte-identical when we do not substitute.

// ============================================================================
// regserve -- ANSWER a registry read from the ini, instead of WRITING the registry
//
// Learned from Ashita (2026-08-26). `Ashita::Hooks::Registry::Install` hooks the
// Reg* APIs and ntdll's NtQueryKey and answers FFXI's settings reads out of its
// own boot config's `[ffxi.registry]` block; `polplugins/sandbox.dll` takes the
// same idea all the way to a virtual hive. We already had every piece of the
// machinery -- these hooks, and key_path() below them -- serving three narrow
// hard-coded tables. This makes it general.
//
// WHY THIS IS BETTER THAN WRITING THE VALUE, which is what we do today:
//
//   * an HKLM write from an unelevated process is SILENTLY VIRTUALIZED into a
//     per-user shadow, so it appears to work and changes nothing
//     ([[hklm-write-silently-virtualized]]);
//   * the same setting exists in more than one hive with different values, and
//     which one a title reads is a measurement nobody has finished
//     ([[per-game-config-surfaces]]) -- if you answer the READ you do not have to
//     know which hive won;
//   * it needs no elevation, and the POL tree is in Program Files
//     ([[install-tree-needs-elevation]]);
//   * and it is REVERSIBLE. Nothing on the machine changes. Delete the ini
//     section and the install is exactly as it was, which is not true of any
//     value we have ever written into somebody's hive.
//
// FORMAT. One ini section per registry key, named `reg:` plus a SUFFIX of the
// key's path. The suffix is matched against the resolved path from the tail, so
// you write the part you know and WOW6432Node, the hive prefix and the regional
// spelling take care of themselves:
//
//     [reg:SquareEnix\FinalFantasyXI]
//     0017=1
//     0037=dword:00000780
//     0042="C:\Program Files (x86)\SquareEnix\FINAL FANTASY XI"
//
// A bare number is a REG_DWORD (decimal); `dword:` takes hex; anything quoted, or
// anything that is not a number, is a REG_SZ.
//
// IMPORTANT: SCOPE, and it is deliberately narrow. This serves RegQueryValueEx ONLY --
// the read a title makes for a setting. It does not fabricate keys, does not
// answer enumeration (RegEnumValue), and does not touch RegOpenKey: a value can
// only be served under a key that really exists and that the title really
// opened. Serving a whole synthetic hive is what Ashita's sandbox does and is a
// much bigger promise than this one; do not extend this into it by accident.
//
// IMPORTANT: AND IT OVERRIDES. Unlike seed_pad above -- which fires only when a value is
// ABSENT, and whose safety argument is exactly that -- an entry here WINS over
// whatever the hive holds. That is the point (it is how you try a setting), and
// it is why every distinct entry announces itself in the log the first time it
// fires. A setting that silently disagrees with what the config app shows is the
// failure mode this project has paid for repeatedly.
// ============================================================================

#define RS_MAX      128
#define RS_SUFFIX   192
#define RS_NAME      64

struct RegServe {
    wchar_t keysuffix[RS_SUFFIX];    // resolved key path must END with this
    wchar_t nameW[RS_NAME];
    char    nameA[RS_NAME];
    bool    is_dword;
    DWORD   dw;
    wchar_t szW[MAX_PATH];
    char    szA[MAX_PATH];
    LONG    hits;
};

static RegServe g_rs[RS_MAX];
static int      g_rs_n = 0;
static int      g_rs_trace = 0;

int regserve_count(void) { return g_rs_n; }

// "12" -> dword 12. "dword:0000000A" -> dword 10. "\"text\"" or anything else -> sz.
static void rs_parse_value(RegServe* e, const wchar_t* v)
{
    while (*v == L' ' || *v == L'\t') v++;

    if (_wcsnicmp(v, L"dword:", 6) == 0) {
        e->is_dword = true;
        e->dw = (DWORD)wcstoul(v + 6, NULL, 16);
        return;
    }

    // A bare integer is the common case (FFXI's numbered values are mostly
    // small DWORDs), so it is accepted without ceremony -- but ONLY if the whole
    // string is one, or "1920x1080" would silently become 1920.
    const wchar_t* p = v;
    if (*p == L'-' || *p == L'+') p++;
    bool digits = (*p != 0);
    for (const wchar_t* q = p; *q; q++)
        if (*q < L'0' || *q > L'9') { digits = false; break; }
    if (digits) {
        e->is_dword = true;
        e->dw = (DWORD)wcstoul(v, NULL, 10);
        return;
    }

    e->is_dword = false;
    wchar_t tmp[MAX_PATH];
    wcsncpy_s(tmp, v, _TRUNCATE);
    size_t n = wcslen(tmp);
    // Strip one layer of quotes. A path with a trailing backslash inside quotes
    // is why quoting exists at all -- GetPrivateProfileString keeps what is
    // between them verbatim.
    if (n >= 2 && tmp[0] == L'"' && tmp[n - 1] == L'"') {
        tmp[n - 1] = 0;
        wcsncpy_s(e->szW, tmp + 1, _TRUNCATE);
    } else {
        wcsncpy_s(e->szW, tmp, _TRUNCATE);
    }
    WideCharToMultiByte(CP_ACP, 0, e->szW, -1, e->szA, sizeof(e->szA), NULL, NULL);
}

void regserve_configure(const wchar_t* ini)
{
    g_rs_n = 0;
    g_rs_trace = GetPrivateProfileIntW(L"reg", L"trace", 0, ini);

    // Section NAMES first: this is the only way to find sections whose names are
    // not known ahead of time, and the whole design here is that the user names
    // the key.
    static wchar_t names[8192];
    DWORD got = GetPrivateProfileSectionNamesW(names, _countof(names), ini);
    if (!got) return;

    for (const wchar_t* sec = names; *sec; sec += wcslen(sec) + 1) {
        if (_wcsnicmp(sec, L"reg:", 4) != 0) continue;
        const wchar_t* suffix = sec + 4;
        while (*suffix == L' ') suffix++;
        if (!*suffix) continue;

        static wchar_t body[8192];
        DWORD bl = GetPrivateProfileSectionW(sec, body, _countof(body), ini);
        if (!bl) continue;

        for (const wchar_t* kv = body; *kv; kv += wcslen(kv) + 1) {
            const wchar_t* eq = wcschr(kv, L'=');
            if (!eq || eq == kv) continue;
            if (g_rs_n >= RS_MAX) {
                logf("[reg] more than %d served values -- the rest of [%ls] is "
                     "IGNORED. Raise RS_MAX if this is real.", RS_MAX, sec);
                return;
            }
            RegServe* e = &g_rs[g_rs_n];
            ZeroMemory(e, sizeof(*e));
            wcsncpy_s(e->keysuffix, suffix, _TRUNCATE);
            size_t nl = (size_t)(eq - kv);
            if (nl >= RS_NAME) nl = RS_NAME - 1;
            memcpy(e->nameW, kv, nl * sizeof(wchar_t));
            e->nameW[nl] = 0;
            WideCharToMultiByte(CP_ACP, 0, e->nameW, -1, e->nameA, sizeof(e->nameA), NULL, NULL);
            rs_parse_value(e, eq + 1);
            g_rs_n++;
        }
    }

    if (g_rs_n)
        logf("[reg] %d registry value(s) will be ANSWERED FROM THE INI for this "
             "process -- nothing on this machine is modified, and deleting the "
             "[reg:...] sections undoes it completely", g_rs_n);
}

// Does this key's resolved path end with `suffix`? An unresolvable path is OUT of
// scope -- serving a value into a key we cannot name is the mistake swap_confirm
// already made once.
static bool rs_key_matches(const wchar_t* path, const wchar_t* suffix)
{
    size_t pl = wcslen(path), sl = wcslen(suffix);
    if (!sl || sl > pl) return false;
    return _wcsicmp(path + (pl - sl), suffix) == 0;
}

// Shared by the A and W hooks. `nameA` or `nameW` is set, never both.
static const RegServe* rs_find(HKEY hKey, const char* nameA, const wchar_t* nameW)
{
    if (!g_rs_n) return NULL;

    // Compare the NAME first. It is a cheap string test against a short table,
    // and it fails for virtually every read in the process -- which matters,
    // because resolving the key path is a syscall and this hook is on the path
    // of every registry read pol.exe makes.
    bool any = false;
    for (int i = 0; i < g_rs_n; i++) {
        if (nameA ? (_stricmp(g_rs[i].nameA, nameA) == 0)
                  : (_wcsicmp(g_rs[i].nameW, nameW) == 0)) { any = true; break; }
    }
    if (!any) return NULL;

    wchar_t path[512];
    if (!key_path(hKey, path, _countof(path))) return NULL;

    for (int i = 0; i < g_rs_n; i++) {
        RegServe* e = &g_rs[i];
        bool nm = nameA ? (_stricmp(e->nameA, nameA) == 0)
                        : (_wcsicmp(e->nameW, nameW) == 0);
        if (!nm) continue;
        if (!rs_key_matches(path, e->keysuffix)) continue;
        LONG n = InterlockedIncrement(&e->hits);
        // ONCE per entry, loudly. An override that disagrees with what the game's
        // own config app displays must be discoverable from the log alone.
        if (n == 1) {
            if (e->is_dword)
                logf("[reg] SERVING %ls = %lu (0x%08lX) from the ini, under %ls "
                     "-- this OVERRIDES whatever the registry holds",
                     e->nameW, e->dw, e->dw, path);
            else
                logf("[reg] SERVING %ls = \"%ls\" from the ini, under %ls "
                     "-- this OVERRIDES whatever the registry holds",
                     e->nameW, e->szW, path);
        } else if (g_rs_trace) {
            logf("[reg] served %ls (hit #%ld)", e->nameW, n);
        }
        return e;
    }
    return NULL;
}

void regserve_summary(void)
{
    if (!g_rs_n) return;
    int fired = 0;
    for (int i = 0; i < g_rs_n; i++) if (g_rs[i].hits) fired++;
    logf("[reg] summary: %d value(s) configured, %d actually read by a title",
         g_rs_n, fired);
    // The zero case is the useful one: a value nothing ever asked for is either
    // the wrong name, or under a key path that does not match, and both look
    // exactly like "the setting did nothing".
    for (int i = 0; i < g_rs_n; i++)
        if (!g_rs[i].hits)
            logf("[reg] summary: %ls under '%ls' was NEVER READ -- check the name "
                 "and that the key suffix really matches (turn on [reg] trace=1)",
                 g_rs[i].nameW, g_rs[i].keysuffix);
}

int regserve_selftest(void)
{
    int fail = 0;
    #define CHK(c, m) do { if (!(c)) { logf("[reg] SELFTEST FAIL: %s", m); fail++; } } while (0)

    RegServe e;
    ZeroMemory(&e, sizeof(e));
    rs_parse_value(&e, L"6");
    CHK(e.is_dword && e.dw == 6, "a bare integer is a decimal DWORD");
    ZeroMemory(&e, sizeof(e));
    rs_parse_value(&e, L"dword:0000000A");
    CHK(e.is_dword && e.dw == 10, "dword: is hex");
    ZeroMemory(&e, sizeof(e));
    rs_parse_value(&e, L"1920x1080");
    CHK(!e.is_dword && wcscmp(e.szW, L"1920x1080") == 0,
        "a string that merely STARTS with digits must not become a DWORD");
    ZeroMemory(&e, sizeof(e));
    rs_parse_value(&e, L"\"C:\\Program Files\\x\"");
    CHK(!e.is_dword && wcscmp(e.szW, L"C:\\Program Files\\x") == 0,
        "quotes are stripped and the contents kept verbatim");
    ZeroMemory(&e, sizeof(e));
    rs_parse_value(&e, L"  42");
    CHK(e.is_dword && e.dw == 42, "leading spaces are ignored");
    ZeroMemory(&e, sizeof(e));
    rs_parse_value(&e, L"");
    CHK(!e.is_dword && e.szW[0] == 0, "an empty value is an empty string, not 0");

    // The suffix match is what keeps an override off an unrelated key of the
    // same value name, so both directions matter.
    CHK(rs_key_matches(L"\\REGISTRY\\MACHINE\\SOFTWARE\\WOW6432Node\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI",
                       L"SquareEnix\\FinalFantasyXI"),
        "a tail suffix matches through WOW6432Node and the hive prefix");
    CHK(!rs_key_matches(L"\\REGISTRY\\MACHINE\\SOFTWARE\\Something\\Else",
                        L"SquareEnix\\FinalFantasyXI"),
        "an unrelated key must NOT match");
    CHK(!rs_key_matches(L"\\REGISTRY\\MACHINE\\SOFTWARE\\SquareEnix\\FinalFantasyXI",
                        L"\\REGISTRY\\MACHINE\\SOFTWARE\\SquareEnix\\FinalFantasyXI\\Extra"),
        "a suffix longer than the path must not match");
    CHK(rs_key_matches(L"\\REGISTRY\\MACHINE\\SOFTWARE\\PlayOnlineEU\\SquareEnix\\FinalFantasyXI",
                       L"squareenix\\finalfantasyxi"),
        "matching is case-insensitive");

    #undef CHK
    if (!fail) logf("[reg] selftest OK");
    return fail;
}

static LONG WINAPI hook_RQVA(HKEY hKey, LPCSTR name, LPDWORD reserved,
                             LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    // regserve FIRST: an explicit [reg:...] entry is a direct statement of
    // intent from the person running this, and nothing below may overrule it.
    // Same precedence argument as the windowed-mode except list, where a lower
    // rule silently swallowed the higher one for months
    // ([[shim-windowed-by-default]]).
    if (g_rs_n && name) {
        const RegServe* e = rs_find(hKey, name, NULL);
        if (e)
            return e->is_dword ? serve_dword(e->dw, lpType, lpData, lpcbData)
                               : serve_szA(e->szA, lpType, lpData, lpcbData);
    }

    // seed_pad: supply a measured value the title asked for and the registry does
    // not have. Absent ONLY -- anything already stored wins, always.
    if (real_RQVA && name) {
        const PadSeed* r = padseed_find(hKey, name, NULL);
        if (r) {
            DWORD t = 0, cb = 0;
            LONG rc = real_RQVA(hKey, name, NULL, &t, NULL, &cb);
            if (g_seedtrace) {
                wchar_t kp[512]; if (!key_path(hKey, kp, _countof(kp))) wcscpy_s(kp, L"(unresolved)");
                logf("[seedpad] A: '%s' (%s) in %ls  rc=%ld type=%lu -> %s", name, r->title,
                     kp, rc, t, rc == ERROR_SUCCESS ? "present, passed through" : "SEEDED");
            }
            if (rc != ERROR_SUCCESS) {
                logf("[seedpad] %s: '%s' absent -- seeding", r->title, name);
                return r->type == REG_DWORD
                     ? serve_dword(r->dw, lpType, lpData, lpcbData)
                     : serve_szA(r->szA, lpType, lpData, lpcbData);
            }
        }
    }

    // force_pad: a title's own "use the gamepad" flag, served ON. Scoped by a REQUIRED
    // sibling DWORD in the same key, so an unrelated value of the same name is never
    // touched. Independent of installdir and of the A/B swap.
    if (real_RQVA && name) {
        const PadGate* pg = padgate_find(name, NULL);
        if (pg) {
            DWORD st = 0, ssz = sizeof(DWORD), sval = 0;
            bool scoped = real_RQVA(hKey, pg->sibA, NULL, &st, (LPBYTE)&sval, &ssz) == ERROR_SUCCESS
                          && st == REG_DWORD;
            DWORD ct = 0, csz = sizeof(DWORD), cur = 0;
            LONG  cr = real_RQVA(hKey, name, NULL, &ct, (LPBYTE)&cur, &csz);
            if (g_padtrace)
                logf("[forcepad] A: '%%s' (%%s) cur rc=%%ld val=%%lu | sibling '%%s' %%s -> %%s",
                     name, pg->title, cr, cur, pg->sibA, scoped ? "present" : "MISSING",
                     scoped ? "SERVED ON" : "passed through");
            if (scoped) {
                if (cr != ERROR_SUCCESS || cur != pg->on)
                    logf("[forcepad] A: %s gate '%s' was %s -- serving %lu",
                         pg->title, pg->nameA,
                         cr == ERROR_SUCCESS ? "OFF" : "absent", pg->on);
                return serve_dword(pg->on, lpType, lpData, lpcbData);
            }
        }
    }

    // A/B swap: serve the SIBLING's value for Ok/Cancel. Fires only when the sibling
    // exists as a DWORD in this key (effectively only Settings\Controller), so an
    // unrelated Ok/Cancel elsewhere is left alone. Independent of installdir.
    if (swap_active() && real_RQVA && name && btn_interesting(name, NULL)) {
        {
            const char* sib = swap_sibA(name);      // NULL for Navi/Menu: nothing to swap
            wchar_t kp[512];
            bool scoped = swap_in_scope(hKey, kp, _countof(kp));
            int  ovr    = btn_override(name, NULL);
            DWORD st = 0, ssz = sizeof(DWORD), sval = 0;
            LONG sr = sib ? real_RQVA(hKey, sib, NULL, &st, (LPBYTE)&sval, &ssz) : ERROR_FILE_NOT_FOUND;
            bool canswap = g_swap && sib && (sr == ERROR_SUCCESS && st == REG_DWORD);
            const char* verdict = !scoped ? "OUT OF SCOPE -- passed through"
                                : ovr >= 0 ? "OVERRIDDEN (btn_)"
                                : canswap  ? "SWAPPED"
                                           : "unchanged";
            if (g_swapt)
                logf("[swap] A: '%s' in %ls | sibling '%s' rc=%ld val=%lu | override=%d -> %s",
                     name, kp[0] ? kp : L"(path unresolved)", sib ? sib : "(none)", sr, sval, ovr, verdict);
            if (scoped && (ovr >= 0 || canswap)) {
                DWORD out = (ovr >= 0) ? (DWORD)ovr : sval;
                if (lpType) *lpType = REG_DWORD;
                if (!lpData) { if (lpcbData) *lpcbData = sizeof(DWORD); return ERROR_SUCCESS; }
                if (!lpcbData) return ERROR_INVALID_PARAMETER;
                DWORD cap = *lpcbData; *lpcbData = sizeof(DWORD);
                if (cap < sizeof(DWORD)) return ERROR_MORE_DATA;
                memcpy(lpData, &out, sizeof(DWORD));
                return ERROR_SUCCESS;
            }
        }
    }

    if (!g_on || !real_RQVA)
        return real_RQVA ? real_RQVA(hKey, name, reserved, lpType, lpData, lpcbData)
                         : ERROR_INVALID_FUNCTION;

    // Read the value ourselves first (never through the caller's buffer) to
    // decide. Only REG_SZ / REG_EXPAND_SZ values short enough to be a path.
    DWORD type = 0, size = 0;
    LONG r = real_RQVA(hKey, name, NULL, &type, NULL, &size);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)
            || size == 0 || size > 2000)
        return real_RQVA(hKey, name, reserved, lpType, lpData, lpcbData);

    char cur[2048];
    DWORD cursz = sizeof(cur) - 2;
    if (real_RQVA(hKey, name, NULL, &type, (LPBYTE)cur, &cursz) != ERROR_SUCCESS)
        return real_RQVA(hKey, name, reserved, lpType, lpData, lpcbData);
    if (cursz > sizeof(cur) - 2) cursz = sizeof(cur) - 2;
    cur[cursz] = 0; cur[cursz + 1] = 0;             // guarantee NUL-terminated

    if (!rooted_A(cur))
        return real_RQVA(hKey, name, reserved, lpType, lpData, lpcbData);

    char out[2048];
    _snprintf_s(out, sizeof(out), _TRUNCATE, "%s%s", g_toA, cur + g_fromA_len);
    DWORD outbytes = (DWORD)strlen(out) + 1;         // REG_SZ length includes NUL

    if (lpType) *lpType = type;
    if (!lpData) {                                    // size query
        if (lpcbData) *lpcbData = outbytes;
        return ERROR_SUCCESS;
    }
    if (!lpcbData) return ERROR_INVALID_PARAMETER;
    DWORD cap = *lpcbData;
    *lpcbData = outbytes;
    if (cap < outbytes) return ERROR_MORE_DATA;
    memcpy(lpData, out, outbytes);
    logf("[installdir] %s: re-rooted to this install", name ? name : "(default)");
    return ERROR_SUCCESS;
}

static LONG WINAPI hook_RQVW(HKEY hKey, LPCWSTR name, LPDWORD reserved,
                             LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    if (g_rs_n && name) {                       // see hook_RQVA on precedence
        const RegServe* e = rs_find(hKey, NULL, name);
        if (e)
            return e->is_dword ? serve_dword(e->dw, lpType, lpData, lpcbData)
                               : serve_szW(e->szW, lpType, lpData, lpcbData);
    }

    // seed_pad: supply a measured value the title asked for and the registry does
    // not have. Absent ONLY -- anything already stored wins, always.
    if (real_RQVW && name) {
        const PadSeed* r = padseed_find(hKey, NULL, name);
        if (r) {
            DWORD t = 0, cb = 0;
            LONG rc = real_RQVW(hKey, name, NULL, &t, NULL, &cb);
            if (g_seedtrace) {
                wchar_t kp[512]; if (!key_path(hKey, kp, _countof(kp))) wcscpy_s(kp, L"(unresolved)");
                logf("[seedpad] W: '%ls' (%s) in %ls  rc=%ld type=%lu -> %s", name, r->title,
                     kp, rc, t, rc == ERROR_SUCCESS ? "present, passed through" : "SEEDED");
            }
            if (rc != ERROR_SUCCESS) {
                logf("[seedpad] %s: '%ls' absent -- seeding", r->title, name);
                return r->type == REG_DWORD
                     ? serve_dword(r->dw, lpType, lpData, lpcbData)
                     : serve_szW(r->szW, lpType, lpData, lpcbData);
            }
        }
    }

    // force_pad: a title's own "use the gamepad" flag, served ON. Scoped by a REQUIRED
    // sibling DWORD in the same key, so an unrelated value of the same name is never
    // touched. Independent of installdir and of the A/B swap.
    if (real_RQVW && name) {
        const PadGate* pg = padgate_find(NULL, name);
        if (pg) {
            DWORD st = 0, ssz = sizeof(DWORD), sval = 0;
            bool scoped = real_RQVW(hKey, pg->sibW, NULL, &st, (LPBYTE)&sval, &ssz) == ERROR_SUCCESS
                          && st == REG_DWORD;
            DWORD ct = 0, csz = sizeof(DWORD), cur = 0;
            LONG  cr = real_RQVW(hKey, name, NULL, &ct, (LPBYTE)&cur, &csz);
            if (g_padtrace)
                logf("[forcepad] W: '%%ls' (%%s) cur rc=%%ld val=%%lu | sibling '%%ls' %%s -> %%s",
                     name, pg->title, cr, cur, pg->sibW, scoped ? "present" : "MISSING",
                     scoped ? "SERVED ON" : "passed through");
            if (scoped) {
                if (cr != ERROR_SUCCESS || cur != pg->on)
                    logf("[forcepad] W: %s gate '%s' was %s -- serving %lu",
                         pg->title, pg->nameA,
                         cr == ERROR_SUCCESS ? "OFF" : "absent", pg->on);
                return serve_dword(pg->on, lpType, lpData, lpcbData);
            }
        }
    }

    if (swap_active() && real_RQVW && name && btn_interesting(NULL, name)) {
        {
            const wchar_t* sib = swap_sibW(name);   // NULL for Navi/Menu
            wchar_t kp[512];
            bool scoped = swap_in_scope(hKey, kp, _countof(kp));
            int  ovr    = btn_override(NULL, name);
            DWORD st = 0, ssz = sizeof(DWORD), sval = 0;
            LONG sr = sib ? real_RQVW(hKey, sib, NULL, &st, (LPBYTE)&sval, &ssz) : ERROR_FILE_NOT_FOUND;
            bool canswap = g_swap && sib && (sr == ERROR_SUCCESS && st == REG_DWORD);
            const char* verdict = !scoped ? "OUT OF SCOPE -- passed through"
                                : ovr >= 0 ? "OVERRIDDEN (btn_)"
                                : canswap  ? "SWAPPED"
                                           : "unchanged";
            if (g_swapt)
                logf("[swap] W: '%ls' in %ls | sibling '%ls' rc=%ld val=%lu | override=%d -> %s",
                     name, kp[0] ? kp : L"(path unresolved)", sib ? sib : L"(none)", sr, sval, ovr, verdict);
            if (scoped && (ovr >= 0 || canswap)) {
                DWORD out = (ovr >= 0) ? (DWORD)ovr : sval;
                if (lpType) *lpType = REG_DWORD;
                if (!lpData) { if (lpcbData) *lpcbData = sizeof(DWORD); return ERROR_SUCCESS; }
                if (!lpcbData) return ERROR_INVALID_PARAMETER;
                DWORD cap = *lpcbData; *lpcbData = sizeof(DWORD);
                if (cap < sizeof(DWORD)) return ERROR_MORE_DATA;
                memcpy(lpData, &out, sizeof(DWORD));
                return ERROR_SUCCESS;
            }
        }
    }

    if (!g_on || !real_RQVW)
        return real_RQVW ? real_RQVW(hKey, name, reserved, lpType, lpData, lpcbData)
                         : ERROR_INVALID_FUNCTION;

    DWORD type = 0, size = 0;
    LONG r = real_RQVW(hKey, name, NULL, &type, NULL, &size);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)
            || size == 0 || size > 2000)
        return real_RQVW(hKey, name, reserved, lpType, lpData, lpcbData);

    wchar_t cur[1024];
    DWORD cursz = sizeof(cur) - 2 * sizeof(wchar_t);
    if (real_RQVW(hKey, name, NULL, &type, (LPBYTE)cur, &cursz) != ERROR_SUCCESS)
        return real_RQVW(hKey, name, reserved, lpType, lpData, lpcbData);
    DWORD cwords = cursz / sizeof(wchar_t);
    if (cwords > _countof(cur) - 2) cwords = _countof(cur) - 2;
    cur[cwords] = 0; cur[cwords + 1] = 0;

    if (!rooted_W(cur))
        return real_RQVW(hKey, name, reserved, lpType, lpData, lpcbData);

    wchar_t out[1024];
    _snwprintf_s(out, _countof(out), _TRUNCATE, L"%s%s", g_toW, cur + g_fromW_len);
    DWORD outbytes = (DWORD)(wcslen(out) + 1) * sizeof(wchar_t);

    if (lpType) *lpType = type;
    if (!lpData) {
        if (lpcbData) *lpcbData = outbytes;
        return ERROR_SUCCESS;
    }
    if (!lpcbData) return ERROR_INVALID_PARAMETER;
    DWORD cap = *lpcbData;
    *lpcbData = outbytes;
    if (cap < outbytes) return ERROR_MORE_DATA;
    memcpy(lpData, out, outbytes);
    logf("[installdir] (W) re-rooted to this install");
    return ERROR_SUCCESS;
}

void installdir_resolve()
{
    if (!g_on && !swap_active() && !g_forcepad && !g_seedpad) return;      // stay out of every IAT when both off
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    if (!adv) adv = LoadLibraryW(L"advapi32.dll");
    if (adv) {
        real_RQVA = (PFN_RQVA)GetProcAddress(adv, "RegQueryValueExA");
        real_RQVW = (PFN_RQVW)GetProcAddress(adv, "RegQueryValueExW");
    }
    if (!real_RQVA && !real_RQVW) {
        g_on = 0;
        logf("[installdir] could not resolve RegQueryValueEx -- hook disabled");
    }
}

// An IAT patch CANNOT reach polcore.dll, and that is not a bug to hunt -- it is
// the module's shape. `polcore.dll` has **no import directory at all**: its
// `.text` has SizeOfRawData 0 on disk and the payload lives in a `POL1` section
// at entropy 7.52, so the packer stub builds the import table at runtime and
// resolves every API fresh. There is nothing for patch_iat() to rewrite when the
// module loads, and whatever it did rewrite would be discarded on unpack. That
// is exactly why the 2026-08-14 SE-facing launch still failed with "cannot write
// to the specified destination folder" while the log showed the hook armed and
// `[iat] hooked advapi32:RegQueryValueExA in pol.exe` -- pol.exe was hooked;
// polcore, which does the read that drives the write, was not and could not be.
//
// So patch the SOURCE: advapi32's own export table. Every resolution path -- a
// loader bind, GetProcAddress, LdrGetProcedureAddress, a packer stub walking the
// EAT by hand -- reads it, so all of them land on our thunk. Same mechanism and
// same rationale as d3d8's Direct3DCreate8 patch (measured on the equally packed
// TM.dll) and multi's CreateMutexW.
//
// Ordering matters and is enforced by the caller: installdir_resolve() captures
// the genuine pointers BEFORE this rewrites the table they were read from, so
// the pass-through calls reach real code and never recurse.
void installdir_eat_patch()
{
    if (!g_on && !swap_active() && !g_forcepad && !g_seedpad) return;      // stay out of every EAT when both off
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    if (!adv) adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) return;

    if (!real_RQVA) real_RQVA = (PFN_RQVA)GetProcAddress(adv, "RegQueryValueExA");
    if (!real_RQVW) real_RQVW = (PFN_RQVW)GetProcAddress(adv, "RegQueryValueExW");
    if (!real_RQVA && !real_RQVW) {
        logf("[installdir] EAT patch skipped -- RegQueryValueEx did not resolve");
        return;
    }

    if (real_RQVA)
        dx_eat_patch_export(adv, "RegQueryValueExA", (void*)hook_RQVA,
                            "installdir!RegQueryValueExA (beats polcore's POL1 "
                            "packer resolving it at runtime)");
    if (real_RQVW)
        dx_eat_patch_export(adv, "RegQueryValueExW", (void*)hook_RQVW,
                            "installdir!RegQueryValueExW");
}

void* installdir_real_RegQueryValueExA() { return (void*)real_RQVA; }
void* installdir_hook_RegQueryValueExA() { return (g_on || swap_active() || g_forcepad || g_seedpad || g_rs_n) ? (void*)hook_RQVA : NULL; }
void* installdir_real_RegQueryValueExW() { return (void*)real_RQVW; }
void* installdir_hook_RegQueryValueExW() { return (g_on || swap_active() || g_forcepad || g_seedpad || g_rs_n) ? (void*)hook_RQVW : NULL; }

// --- init helpers -----------------------------------------------------------

static void dir_of_self(char* out, size_t cch)
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    wchar_t* slash = wcsrchr(p, L'\\');
    if (slash) *slash = 0;
    WideCharToMultiByte(CP_ACP, 0, p, -1, out, (int)cch, NULL, NULL);
}

// Read the currently-registered Viewer path (InstallFolder value "1000") through
// the REAL advapi32 export, trying the US, neutral and EU hives in turn.
static int registered_viewer_path(char* out, DWORD cch)
{
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    if (!adv) adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) return 0;
    PFN_ROKA rok = (PFN_ROKA)GetProcAddress(adv, "RegOpenKeyExA");
    PFN_RQVA rqv = (PFN_RQVA)GetProcAddress(adv, "RegQueryValueExA");
    PFN_RCK  rck = (PFN_RCK) GetProcAddress(adv, "RegCloseKey");
    if (!rok || !rqv) return 0;
    static const char* keys[] = {
        "SOFTWARE\\PlayOnlineUS\\InstallFolder",
        "SOFTWARE\\PlayOnline\\InstallFolder",
        "SOFTWARE\\PlayOnlineEU\\InstallFolder",
    };
    for (int i = 0; i < 3; i++) {
        HKEY h;
        if (rok(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_READ, &h) != ERROR_SUCCESS)
            continue;
        DWORD type = 0, sz = cch - 1;
        LONG r = rqv(h, "1000", NULL, &type, (LPBYTE)out, &sz);
        if (rck) rck(h);
        if (r == ERROR_SUCCESS && sz > 1 && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            if (sz > cch - 1) sz = cch - 1;
            out[sz] = 0;
            return 1;
        }
    }
    return 0;
}

static void store_from_to(const char* fromA, const char* toA)
{
    strcpy_s(g_fromA, fromA); g_fromA_len = strlen(g_fromA);
    strcpy_s(g_toA, toA);
    MultiByteToWideChar(CP_ACP, 0, g_fromA, -1, g_fromW, MAX_PATH); g_fromW_len = wcslen(g_fromW);
    MultiByteToWideChar(CP_ACP, 0, g_toA, -1, g_toW, MAX_PATH);
}

void installdir_init(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"installdir", L"enable", 0, ini);
    if (!g_on) return;

    // [installdir] game_only=1 -- arm only in a "/game" title child. The VIEWER
    // must not see its own tree re-rooted: it compares its running location
    // against its registration and refuses with "cannot write to the specified
    // destination folder" when they disagree (the exact error this hook was
    // built to cure in the copied-tree case, produced here by the cure). The
    // title child has no such check and is where a packed title reads LOADDIR.
    if (GetPrivateProfileIntW(L"installdir", L"game_only", 0, ini)) {
        const wchar_t* cl = GetCommandLineW();
        if (!cl || !wcsstr(cl, L"/game")) {
            g_on = 0;
            logf("[installdir] game_only=1 and this is not a /game child -- "
                 "hook idle in this process.");
            return;
        }
    }

    char fromA[MAX_PATH] = "", toA[MAX_PATH] = "";
    {
        // Read WIDE through ini_str so a trailing "; comment" is stripped, then narrow.
        // Raw GetPrivateProfileStringA keeps everything right of '=', and these are
        // compared/prefix-matched as PATH data below -- a comment would break the
        // redirect (the last comment-trap straggler in the codebase).
        wchar_t fw[MAX_PATH], tw[MAX_PATH];
        ini_str(L"installdir", L"from", L"", fw, _countof(fw), ini);
        ini_str(L"installdir", L"to",   L"", tw, _countof(tw), ini);
        WideCharToMultiByte(CP_ACP, 0, fw, -1, fromA, sizeof(fromA), NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, tw, -1, toA,   sizeof(toA),   NULL, NULL);
    }

    if (!toA[0])   dir_of_self(toA, sizeof(toA));               // folder-aware default
    if (!fromA[0]) registered_viewer_path(fromA, sizeof(fromA)); // registered Viewer path

    size_t fl = strlen(fromA); if (fl && fromA[fl - 1] == '\\') fromA[--fl] = 0;
    size_t tl = strlen(toA);   if (tl && toA[tl - 1] == '\\')   toA[--tl] = 0;

    if (!fromA[0] || !toA[0]) {
        g_on = 0;
        logf("[installdir] enable=1 but from/to could not be determined "
             "(from='%s' to='%s') -- refusing to arm. Set [installdir] from= and "
             "to= in polshim.ini.", fromA, toA);
        return;
    }
    if (_stricmp(fromA, toA) == 0) {
        g_on = 0;
        logf("[installdir] from == to (%s) -- nothing to redirect; hook idle.", fromA);
        return;
    }
    store_from_to(fromA, toA);
    logf("[installdir] FOLDER-AWARE: registry paths under '%s' are served as '%s' "
         "in THIS process only -- the machine registry and the primary install are "
         "untouched.", g_fromA, g_toA);
}

// Test harness only (regredirtest.cpp): force the config without an ini/registry.
void installdir_test_config(const char* fromA, const char* toA)
{
    g_on = 1;
    store_from_to(fromA, toA);
    installdir_resolve();
}
