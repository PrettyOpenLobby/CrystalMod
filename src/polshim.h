// polshim -- generic COM interposer for the PlayOnline Viewer components.
//
// We register ourselves as the InprocServer32 for a POL CLSID, load the real
// DLL, and hand the host a proxy for every interface pointer that crosses the
// boundary. Each proxy has a synthetic vtable whose slots are 10-byte stubs
// that funnel into one dispatcher, so we can log a call without knowing the
// interface's method signatures -- or even how many methods it has.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLSHIM_MAX_SLOTS   512
#define POLSHIM_MAX_ARGS    8     // stack dwords captured per call
#define POLSHIM_MAX_DEPTH   256   // shadow-stack depth per thread

// Shim version -- shown in the log banner and appended to the Viewer window title
// (titletag.cpp) so a user can see which build is running, and thus whether the
// latest update reached them over the patch channel. Bump on each published shim
// update. Overridable at build time via /DPOLSHIM_VERSION=\"x.y.z\".
// 0.2.0 (2026-08-17): the settings pass. d3d_fitwindow RETIRED, nine trace keys
// collapsed into [polshim] trace=0..3 (default off, where most of them had been
// compiled ON), the FMV letterboxed instead of stretched, the game window's
// remembered size shape-corrected, and [autoupdate] now re-checks on an interval
// instead of only at launch. A minor bump, not a patch one: settings changed
// meaning, and one of them stopped existing.
// WHERE UPDATES COME FROM. A release build updates from the project's own latest
// GitHub release, never from the game server it plays on: a server operator's
// /shim/dist may hold a private or modified build, and players of that server
// should not be handed it just because they pointed the shim there. GitHub's
// `releases/latest/download/<file>` always names the newest published release,
// over HTTPS, and serves exactly the two files the updater wants (PolHook.dll and
// PolHook.dll.sha256). An operator who DOES want to host their own build sets
// [autoupdate] url=server (the old <server>/shim/dist layout) or a full URL.
#ifndef POLSHIM_UPDATE_BASE
#define POLSHIM_UPDATE_BASE "https://github.com/PrettyOpenLobby/CrystalMod/releases/latest/download"
#endif

#ifndef POLSHIM_VERSION
#define POLSHIM_VERSION "0.2.0"
#endif

// POLSHIM_BUILD -- a plain counter Publish-ShimDist.ps1 increments on EVERY dist
// release, so "is my update actually installed?" is answered by reading one number in
// the settings dialog instead of comparing hashes. The version string above moves only
// on features, which makes it useless for that question.
#include "buildnum.h"

// ---------------------------------------------------------------- reading the ini
//
// READ EVERY STRING VALUE THROUGH ini_str(), NOT GetPrivateProfileStringW.
//
// The Win32 profile API honours ';' only at the start of a LINE. A comment after a
// value is not a comment to it -- GetPrivateProfileString hands back everything to the
// right of the '=', so
//
//     d3d_windowed=1       ; FMO is d3d9 -- d3d9hook forces the windowed device
//
// reads as the string "1       ; FMO is d3d9 -- ...". GetPrivateProfileInt survives it
// by accident (wcstol stops at the space), which is why the shim RAN correctly off a
// commented ini for months and hid this completely.
//
// What it did not survive was any read that COMPARES the text. The settings dialog does:
// its checkbox is `value == "1"`, its dropdown matches the value against its choices, and
// a fix-pack is ticked when every member equals its on-value. Every one of those read a
// commented option as unset, drew it unticked, and wrote its OFF value on Save. Since the
// shipped templates document options with exactly these trailing comments, opening the
// dialog once on a documented install silently turned off whatever had been explained in
// place -- reported 2026-08-17: windowed mode and the 2x window size lost, then the Tetra
// Master mouse pack, from one visit to a dialog the user was not even there to change them
// in. The runtime string readers (inputmode mode=, dinput_cursor_xy=, polshim real=, ...)
// carried the same latent bug against any commented value.
//
// A comment starts at a ';' that opens the value or follows whitespace, so a value that
// contains one with no space in front of it -- `a;b` -- is left alone. Trailing whitespace
// goes with it, since it is the same edit.
// Edits in place and returns the new length. Separate from ini_str because the
// [servers] list arrives through GetPrivateProfileSection, which has no per-value
// read to route through.
static inline DWORD ini_decomment(wchar_t* s)
{
    DWORD n = (DWORD)wcslen(s);
    for (DWORD i = 0; i < n; i++) {
        if (s[i] != L';') continue;
        if (i && s[i - 1] != L' ' && s[i - 1] != L'\t') continue;
        n = i;
        break;
    }
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) n--;
    s[n] = 0;
    return n;
}

static inline DWORD ini_str(const wchar_t* sec, const wchar_t* key, const wchar_t* def,
                            wchar_t* out, DWORD cch, const wchar_t* ini)
{
    GetPrivateProfileStringW(sec, key, def, out, cch, ini);
    return ini_decomment(out);
}

// --------------------------------------------------------- PER-TITLE SETTINGS
//
// The design, in one paragraph:
//
// A per-title override lives in a section named `<sec>.<module leaf>` --
//
//     [dx.FE_Client.dll]
//     d3d_freecursor=0
//
// -- and resolution is MOST SPECIFIC WINS:
//
//     [<sec>.<leaf>]  >  [<sec>]  >  TitleProfile row  >  compiled default
//
// Only the two ini layers live here; the profile layer is per-KEY code in
// profiles.cpp, and a caller holding a profile verdict passes it as `def`.
//
// KEY: THE SCOPE IS AMBIENT, ON PURPOSE. Which title is in scope is a thread-local
// set around a reload (title_scope_set), not a parameter, so ADOPTING a key is a
// one-word change at its call site -- GetPrivateProfileIntW -> ini_int_title --
// instead of threading a leaf through 25 `*_reload(ini)` signatures. With no
// scope set these are byte-for-byte the old global read, which is what makes the
// whole layer inert until a title is actually running.
//
// Thread-local because the scope guards only the READING WINDOW. The values a
// reload writes into module globals are deliberately process-wide: while a title
// runs, its overrides ARE the live settings.
void        title_scope_set(const char* leaf);   // NULL/"" = no title in scope
const char* title_scope(void);                   // never NULL; "" when global

// THE DISPLAY CONTROL (2026-09-08). One user-facing row -- [dx] display, per title
// as [dx.<leaf>] display -- for how a title's window is shown. It RESOLVES INTO the
// three older mechanisms (d3d_windowed, d3d_windowed_except, d3d_borderless), which
// stay as the compatibility path for an ini that never had it and are no longer
// drawn. Values are the ini spellings below; anything else reads as DM_UNSET.
enum DisplayMode { DM_UNSET = -1, DM_WINDOWED = 0, DM_BORDERLESS = 1,
                   DM_BORDERLESS_CRISP = 2, DM_FULLSCREEN = 3 };
DisplayMode    display_mode_parse(const wchar_t* v);
const wchar_t* display_mode_name(DisplayMode m);
// [dx.<leaf>] display, then [dx] display, else DM_UNSET. `leaf` may be NULL or "";
// `ini` NULL = the path d3d8_configure was last given.
DisplayMode    display_mode_for(const char* leaf, const wchar_t* ini);
// d3d9hook's copy of the windowed verdict: overwritten from `display` when set.
void           d3d_display_windowed(int* windowed, const wchar_t* ini);
// The live hotkey: switch the RUNNING title between windowed and borderless. Writes
// [dx.<leaf>] display, reloads under the title's scope, refits the window on the
// game's own thread. False + a reason (for the log) when it cannot.
bool           d3d_display_toggle(char* why, size_t cap);

// Build "<sec>.<leaf>". False when no title is in scope, which is the signal to
// take the plain global path.
static inline bool ini_title_section(wchar_t* out, size_t cch, const wchar_t* sec)
{
    const char* leaf = title_scope();
    if (!leaf || !*leaf) return false;
    wchar_t wleaf[80];
    if (MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf, (int)(sizeof(wleaf)/sizeof(wleaf[0]))) <= 0)
        return false;
    return _snwprintf_s(out, cch, _TRUNCATE, L"%s.%s", sec, wleaf) > 0;
}

// PRESENT vs ABSENT, not empty-vs-set. An absent per-title key must fall through
// to the global value, and "" is a legitimate value for a list key, so the two
// cannot be told apart by emptiness -- hence the \x01 sentinel default.
static inline DWORD ini_str_title(const wchar_t* sec, const wchar_t* key,
                                  const wchar_t* def, wchar_t* out, DWORD cch,
                                  const wchar_t* ini)
{
    wchar_t ts[96];
    if (ini_title_section(ts, sizeof(ts)/sizeof(ts[0]), sec)) {
        wchar_t probe[512] = L"";
        GetPrivateProfileStringW(ts, key, L"\x01", probe,
                                 (DWORD)(sizeof(probe)/sizeof(probe[0])), ini);
        if (probe[0] != 1) {                       // sentinel gone == key present
            ini_decomment(probe);
            wcsncpy_s(out, cch, probe, _TRUNCATE);
            return (DWORD)wcslen(out);
        }
    }
    return ini_str(sec, key, def, out, cch, ini);
}

// The global fallback is GetPrivateProfileIntW itself, not a re-parse, so a key
// with no per-title override behaves EXACTLY as it did before adoption -- same
// function, same quirks. That equivalence is the whole safety of phase 2.
static inline int ini_int_title(const wchar_t* sec, const wchar_t* key, int def,
                                const wchar_t* ini)
{
    wchar_t ts[96];
    if (ini_title_section(ts, sizeof(ts)/sizeof(ts[0]), sec)) {
        wchar_t probe[64] = L"";
        GetPrivateProfileStringW(ts, key, L"\x01", probe,
                                 (DWORD)(sizeof(probe)/sizeof(probe[0])), ini);
        if (probe[0] != 1) {
            ini_decomment(probe);
            return (int)wcstol(probe, NULL, 10);   // base 10, as GetPrivateProfileInt
        }
    }
    return GetPrivateProfileIntW(sec, key, def, ini);
}

// UAC registry virtualization: a 32-bit process with no requestedExecutionLevel
// manifest (every legacy PlayOnline binary) has its unelevated HKLM\SOFTWARE writes
// SILENTLY redirected to HKCU\...\VirtualStore and gets ERROR_SUCCESS -- so a registry
// repair that "succeeds" is one the elevated title, reading real HKLM, never sees.
// Any code about to write HKLM must REFUSE when this returns true. Shared by
// gamecfg.cpp and regfix.cpp so the check has ONE definition and cannot drift.
static inline bool polshim_token_virtualized(void)
{
    HANDLE t = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    DWORD on = 0, cb = 0;
    BOOL ok = GetTokenInformation(t, TokenVirtualizationEnabled, &on, sizeof(on), &cb);
    CloseHandle(t);
    return ok && on != 0;
}

enum IfaceKind {
    KIND_UNKNOWN = 0,
    KIND_CLASSFACTORY,            // slot 3 = CreateInstance, out-param needs wrapping
};

struct Proxy {
    void**   fake_vtbl;           // MUST be first: &Proxy is the interface pointer we hand out
    void**   real_vtbl;
    void*    real;                // the genuine interface pointer
    int      nslots;
    int      kind;
    int      id;                  // small integer for log readability
    DWORD    vtbl_rva;            // vtable offset inside its module -- ties back to Ghidra
    char     module[64];
    LONG     calls[POLSHIM_MAX_SLOTS];
    void*    stubs;
    // Typed engine (typed_wrap): the proxy is sized from com_allow.h (exact vtable
    // slots, no heuristic probe), IID-scoped (only allow-listed interfaces are
    // wrapped), and lifetime-tracked (retired when the real Release hits 0). These
    // are zero/false for a legacy proxy_wrap proxy.
    bool     typed;
    GUID     iid;                 // the interface's IID (typed proxies only)
};

// proxy.cpp
bool  proxy_init();
void* proxy_wrap(void* iface, int kind, const char* origin);
// TYPED interposition (proxy.cpp, see com_allow.h): wrap `iface` ONLY if `iid` is
// on the allow-list, sizing its synthetic vtable from the typelib-exact slot count.
// A non-allow-listed IID returns `iface` untouched -- which is what keeps the typed
// engine from ever wrapping the transient system COM that crashed the generic one.
void* typed_wrap(void* iface, REFIID iid);
int   proxy_count();              // live proxy count (for the self-test / summary)
void  proxy_summary(FILE* f);

// log.cpp
bool  log_open(const wchar_t* path);
void  log_close();
// Called from DllMain(DLL_PROCESS_DETACH) when lpReserved != NULL, i.e. the process
// is terminating rather than someone calling FreeLibrary. ExitProcess has ALREADY
// terminated every other thread by then, at an arbitrary instruction -- including,
// sometimes, one that was inside logf() holding g_lock or the CRT's FILE lock. A
// critical section whose owner is dead is never released by Windows, so the very
// next logf() (the summary block below the *_stop calls) blocks forever and pol.exe
// becomes an invisible ghost: no windows left, nothing in the taskbar, only Task
// Manager can end it. After this call the log takes NO locks at all -- correct
// precisely because there is no longer another thread to race with.
void  log_begin_teardown();
int   log_teardown_active();               // 1 once the process is tearing down
void  log_write_raw(const char* text);     // pre-formatted; lock-free during teardown
void  logf(const char* fmt, ...);
void  log_flush();
const char* iid_name(const GUID& g, char* buf, size_t cb);
// Optional log tee -- set by polctl.cpp to mirror lines into its automation
// ring; NULL (and free) in every build that does not link polctl.
extern void (*g_log_sink)(const char*);

// --- [polshim] trace: ONE level for every "log more" switch ------------------
//
// The shim had nine of them (d3d_trace, d3d_msgspy, d3d_dumpcaller,
// d3d_renderspy, dinput_trace, dinput_callsite, hook_trace, [dx] trace,
// [polfetch] trace) and every one was a separate compiled literal. Most were
// compiled ON, none was an iniheal row, and the shipping rule separately said
// they must all be 0 in a shipped bundle -- so the shipped default contradicted
// the shipping rule and no healed install could correct it. One session's log
// in this install reached 11.9 MB.
//
// So: one level, default 0, and every old key kept as an OVERRIDE. Pass
// trace_at(N) where the literal used to be -- GetPrivateProfileInt returns it
// only when the ini does not name the key, so an explicit row still wins and
// nobody's tuned dev ini changes behaviour.
//
//   0  off -- the shipping default. Errors, decisions and summaries only.
//   1  what the layers DID: device creation, window fitting, the fetch hook.
//   2  + per-message spies (window messages, hook chain, OutputDebugString).
//   3  + RE instrumentation: module image dumps, the render spy, call sites.
//
// NOT folded in: [polshim] gamestart, which reads like a trace and is not --
// it drives padmap_note_title, so turning it off changes the pad. And
// [polfetch] dump_record, which only fires on a timeout: a fault is exactly
// when the record is wanted, and it costs 18 lines.
// vidfit.cpp -- place a title's DirectShow FMV through the graph's own
// interfaces (IVideoWindow / IVMRWindowlessControl9) rather than by moving its
// window. void* rather than IFilterGraph* so nothing but vidfit.cpp has to pull
// in dshow.h.
void  vidfit_configure(const wchar_t* ini);
int   vidfit_enabled();
int   vidfit_probe_enabled();   // [dx] d3d_videoprobe -- watch only
int   vidfit_wants_graphs();    // track graphs for placement OR the probe
void  vidfit_note_graph(void* punk);      // comtrace: a CLSID_FilterGraph activation

// fmvskip.cpp -- skip a title's opening/FMV DirectShow movie so Wine's quartz
// never decodes it (FE/FMO die in RenderFile under Proton). Wraps the graph
// comtrace sees at CLSID_FilterGraph. [dx] fmv_skip: 0 off, 1 trace, 2 skip
// (report success), 3 skip (report failure). riid guards the reinterpret_cast.
void    fmvskip_configure(const wchar_t* ini);
int     fmvskip_enabled();
// The level to use for the title that owns `ra` (a return address from the hooked
// CoCreateInstance). Decided at CALL time because a title builds its movie graph
// BEFORE the CreateDevice that would have put it in scope -- see fmvskip.cpp.
int     fmvskip_level_for_caller(void* ra);
HRESULT fmvskip_wrap(REFIID riid, void** ppv, const char* caller, int level);  // level = fmvskip_level_for_caller(ra)
void  vidfit_poll(HWND game);             // d3d8hook: the video poll
void  vidfit_release_all();
void  vidfit_summary();

// vmrfix.cpp -- intercept FMO's VMR-9 rendering mode ([dx] d3d_vmr_mode). void*
// rather than the DirectShow interface so nothing else pulls in vmr9.h.
void  vmrfix_configure(const wchar_t* ini);
int   vmrfix_enabled();
void  vmrfix_note_vmr(void* punk);        // comtrace: a CLSID_VideoMixingRenderer9 activation

void  polshim_trace_configure(const wchar_t* ini);
int   polshim_trace_level();
int   trace_at(int level);

// ---------------------------------------------------------------- live reload
//
// Saving in the settings dialog re-reads the SAFE subset of every module's ini
// keys into the running shim: values that are consumed per call, per poll or per
// title launch. One-shot startup decisions -- hook installs, EAT patches, byte
// patches with no unpatch path, process-wide declarations, thread starts -- are
// deliberately NOT re-read here; polsettings.cpp's restart table is what tells
// the user those still need a relaunch. shim_reload (inject.cpp) re-reads
// inject's own flags first (and re-seeds the [polshim] trace level, which the
// other reloads consume through trace_at defaults), then fans out to the rest.
void  shim_reload(const wchar_t* ini);
// Re-read settings with ONE TITLE in scope, so its [<sec>.<module>] overrides
// win. Called from d3d_title_boundary() at each launch and from Save while a
// title is running.
void  shim_reload_for_title(const wchar_t* ini, const char* leaf);
void  dx_reload(const wchar_t* ini);
void  d3d8_reload(const wchar_t* ini);
void  d3d9_reload(const wchar_t* ini);
void  dinput_reload(const wchar_t* ini);
void  hookspy_reload(const wchar_t* ini);
void  maskguard_reload(const wchar_t* ini);
void  fmvskip_reload(const wchar_t* ini);
void  vidfit_reload(const wchar_t* ini);
void  fmoime_reload(const wchar_t* ini);
void  titletag_reload(const wchar_t* ini);
void  padmap_reload(const wchar_t* ini);
void  padoverlay_reload(const wchar_t* ini);
void  ffxiplug_reload(const wchar_t* ini);
void  netredir_reload(const wchar_t* ini);
void  authkey_reload(const wchar_t* ini);
void  sessionwatch_reload(const wchar_t* ini);
void  autoupdate_reload(const wchar_t* ini);
void  polfetch_reload(const wchar_t* ini);
void  com_reload(const wchar_t* ini);
void  crashlog_reload(const wchar_t* ini);

// polshim.cpp
extern int   g_verbose;
extern int   g_max_slots;
extern FILE* g_logfile;

// patches.cpp (injector only) -- persistent byte patches, applied post-unpack.
// Currently: the pre-login "Check Files" accept list,
// which is a hardcoded per-content-id chain in app.dll (Fantasy Earth, id 11, has
// no case and is dropped). Verified byte-for-byte before writing.
void patches_set_enabled(int on);
int  patches_enabled();
void patches_set_optional(int on);
void patches_apply_module(void* module_base, const char* module);

// fepatch.cpp -- the Fantasy Earth scene-graph teardown guard. Trampolines the
// NULL-child read at FE_Client.dll RVA 0x2AC5C4 so a NULL graph link is treated as
// end-of-list (the sentinel case) instead of faulting under Wine/Proton. Armed
// from the d3d8 CreateDevice hook, the reliable post-unpack moment for a title.
void fepatch_set_enabled(int on);
int  fepatch_enabled();
void fepatch_apply_module(void* module_base, const char* module);

// These patches DECAY, and the "beta test is installed" gate comes back with
// them. Measured 2026-08-17: the cause is not an in-place rewrite of app.dll's
// page but a failed title launch UNLOADING and re-LOADING app.dll -- often at the
// very base it just freed -- so the fix that matters is patches_forget_module,
// called by the injector when it re-arms a module it has hooked before.
// patches_reverify stays as the cheap in-place check (a memcmp per site, nothing
// else unless a rewrite is really needed); it is called from COM activation,
// which recurs constantly AND is how a title is launched.
void patches_reverify(void);
void patches_forget_module(const char* module);   // module re-loaded: sites are unpatched again
void patches_summary(void);

// dxhook.cpp (injector only) -- DirectDraw / DirectSound interposition, the
// modernisation seam. The Viewer's entire presentation surface is two factory
// functions (DDRAW!DirectDrawCreateEx and DSOUND!#11 = DirectSoundCreate8), both
// PUBLIC Microsoft ABIs, so this half of the port needs no reverse engineering.
// Delivered through the same by-value IAT patch as the ws2_32 hooks. Gated on
// [dx] enable=1; the DirectDraw side is trace-only for now.
// mailstore.cpp (injector only) -- create the per-profile mail directories the
// Viewer needs but never creates. A missing usr\home<NN>\EMAIL is reported by
// the client as POL-0000, an error code that does not exist, because the store
// open's failure is truncated away and the give-up path sets no code. Gated on
// [polshim] mail_store_fix (default 1); with it 0 the gap is reported and left.
void  mailstore_preflight(const wchar_t* ini);

// comtrace.cpp (injector only) -- hooks the ole32 activation entry points and
// diagnoses failures against the registry in place. This is the ONLY hook that
// can see a REGDB_E_CLASSNOTREG: the EAT patch of DllGetClassObject fires only
// after a module is found, and proxy.cpp sees only interfaces that were created
// successfully. Gated on [polshim] comtrace (default 1 -- activations are rare).
void  com_configure(const wchar_t* ini);
void  com_resolve();
void  com_set_enabled(int on);
int   comtrace_enabled();
void  com_summary();
void* com_real_CoCreateInstance();
void* com_hook_CoCreateInstance();
void* com_real_CoCreateInstanceEx();
void* com_hook_CoCreateInstanceEx();
void* com_real_CoGetClassObject();
void* com_hook_CoGetClassObject();
void* com_real_CLSIDFromProgID();
void* com_hook_CLSIDFromProgID();

// msgxlate.cpp (injector only) -- translate the OS-level dialogs the titles
// raise. Hooks nothing itself: inject.cpp's existing MessageBoxA/W hooks call
// in. Because the interception is at user32, ONE table serves every title --
// FMO, Fantasy Earth, Tetra Master, FFXI and the Viewer. Unmatched strings are
// appended to a sidecar in the table's own format, so filling it is copy-paste.
// Does NOT see a dialog a game draws itself (FMO's "cannot connect" is one).
void  msgxlate_configure(const wchar_t* ini);
int   msgxlate_enabled();
int   msgxlate_a(const char* src, char* out, size_t cb, const char* where);
int   msgxlate_w(const wchar_t* src, wchar_t* out, size_t cch, const char* where);
void  msgxlate_summary();

// polfetch.cpp (injector only) -- breaks Front Mission Online's startup
// LIVELOCK. FMO busy-waits on an async polcore fetch with no Sleep and no
// message pump (FrontMissionOnline.dll+0x1B9210); the fetch never completes on
// our server, so it spins a full core forever and the window goes Not
// Responding. FMO already handles a NEGATIVE poll result, so after a timeout we
// return one and it takes its own give-up branch. Attaches by swapping two
// entries in polcore's common function table (slots 1134/1135), verified before
// writing. Also logs the START arguments and dumps the request record on
// timeout, which is where the target host lives. Gated on [polfetch] enable.
void  polfetch_configure(const wchar_t* ini);
int   polfetch_enabled();
void  polfetch_apply_module(void* module_base, const char* module);
void  polfetch_summary();

// crashlog.cpp -- report our own crashes into our own log. Every pol.exe crash
// before this had to be read out of WER, which gives one address and no stack;
// two of the six in Aug 2026 named PolHook.dll as the faulting module, so "was it
// us" needs a real answer. Installs a LAST-CHANCE filter and never handles the
// exception -- a crash stays a crash. [polshim] crashlog=0 removes it.
void  crashlog_arm(const wchar_t* ini);
// Live all-thread stack dump for a RUNNING-but-wedged title (FE reaches its lobby
// then never presents). In-process EBP walk -> works under Proton where host gdb
// cannot. Tagged [tdump]; names known blocking calls. Not a crash path.
void  crashlog_dump_all_threads(const char* why);
void  crashlog_scan_current_stack(const char* why);
// Did the PREVIOUS session crash? The marker file beside the ini. Reading does not
// clear it: logship clears it once that session's log has actually been sent.
bool  crashlog_prev_crash(const wchar_t* ini, char* summary, size_t cb);
void  crashlog_clear_marker(const wchar_t* ini);

// tmpathfix.cpp -- fix TM's gW resource-index path parser (first-dot -> last-dot
// scan) so a dotted install path (Steam Deck under ~/.local) parses the gW file
// number correctly. RVA-keyed byte patch, byte-verified, applied post-ASProtect-
// unpack from the injector hot path. [dx] tm_pathfix, default ON (strict no-op
// wherever the path already parses). See tm-gw-index-path-dot.
int   tmpathfix_enabled();
void  tmpathfix_init(const wchar_t* ini);

// reslock.cpp -- GlobalLock/GlobalUnlock on a pointer into a mapped IMAGE
// return it untouched, as Windows does. Wine presumes any unaligned pointer is
// a moveable-heap handle and pokes the read-only .rsrc page just below it --
// which silently NULLed FMO's shader-effect resource load on the Deck (the
// game-start crash). [polshim] reslock, default ON (identity is Windows'
// behaviour, so it is a no-op everywhere else). See fmo-deck-gamestart-crash.
void  reslock_init(const wchar_t* ini);
void* reslock_real_GlobalLock();
void* reslock_hook_GlobalLock();
void* reslock_real_GlobalUnlock();
void* reslock_hook_GlobalUnlock();

// flwindow.cpp -- the Friend List's SetWindowRgn-shaped top-level window shows a
// black surround under Wine/KWin (a non-layered shaped window is not composited).
// Give class "WInFriendListWindow" WS_EX_LAYERED + a black colour key so the
// surround keys out. [dx] fl_window_transparent, default ON, scoped to that one
// class. See fe-proton-gamestart-crash / pol-friendlist-app.
void  flwindow_init(const wchar_t* ini);
void* flwindow_real_ShowWindow();
void* flwindow_hook_ShowWindow();
void* flwindow_real_SetWindowRgn();
void* flwindow_hook_SetWindowRgn();

void  tmpathfix_apply_module(void* module_base, const char* module);
void  tmpathfix_eat_patch();   // EAT-hook kernel32!FindFirstFileA: patch on TM's gW glob (deterministic pre-index timing)


// fmoime.cpp -- FMO text fields open in DIRECT (alphanumeric) input instead of
// Japanese. Neutralises the RESTORE of the per-field remembered IME flag at
// FrontMissionOnline.dll+0x83162 / +0x839D5, both byte-verified. Not a disk
// patch: FMO's .text has SizeOfRawData=0 and POL1 rebuilds it at runtime. Not a
// module callback either -- those never fire for FrontMissionOnline.dll, the
// lesson fmokey.cpp already paid for -- so it polls for the module itself.
void  fmoime_configure(const wchar_t* ini);   // [dx] fmo_ime_direct (default ON)
void  fmoime_stop();                          // signal-only; safe under the loader lock
void  fmoime_summary();

// authkey.cpp -- the login-hang fix. Trampoline hook on polcore's SINGLE auth
// key-setup, polcrypt_init (RVA 0x63EB0). Phase 1 [auth] log=1 (default): logs
// K/IV/caller on every auth keying -- the Wine-safe measurement that probes.cpp's
// INT3/DR probe of the same function cannot do on the Deck. Phase 2 [auth]
// rekey_zero=1 (default off): force Klo=Khi=0 so the NICK goes out under K=0,
// which our server always decrypts. No-op in the healthy case; repair in the
// stale-cached-key case; REFUSED under [redirect] SE-bypass (real SE needs the
// real key). See the authkey.cpp header.
void  authkey_configure(const wchar_t* ini);
int   authkey_enabled();
int   authkey_wants_modules();
void  authkey_apply_module(void* module_base, const char* module);
void  authkey_summary();

// --------------------------------------------------- WHERE DID A VALUE COME FROM
//
// The single most expensive recorded failure in this area is "the setting does
// nothing", and it cost a build every time because nothing on screen or in the log
// said WHO HAD ALREADY DECIDED. profile-overrides-the-windowed-setting puts it
// bluntly: a log line that names the wrong source is worse than silence -- silence
// makes you look, a wrong name makes you look somewhere else and conclude the
// setting is fine.
//
// So every per-title row must be able to say which of the four layers answered.
// Ordered most specific first, matching ini_int_title's resolution.
enum ValueSource {
    VS_TITLE_INI = 0,   // [<sec>.<module>] -- this title, chosen by the user
    VS_GLOBAL_INI,      // [<sec>]          -- every game, chosen by the user
    VS_PROFILE,         // TitleProfile     -- this title, shipped by us
    VS_DEFAULT          // ShimOption.def   -- every game, shipped by us
};
const wchar_t* value_source_text(ValueSource s);   // one short human phrase
void           title_current_set(const char* leaf);  // d3d8hook: the title boundary
const char*    title_current(void);                  // "" when no title is running

// d3d8hook.cpp -- does the title currently running want its DirectShow video
// window adopted onto its game window? (TitleProfile::adopt_fmv, latched at the
// title boundary.) vidfit.cpp asks from its own thread.
int   d3d_title_fmv_adopt(void);

// netredir.cpp (injector only) -- points THIS install's pol.com lookups at real
// Square Enix instead of the private server, so a second install can hold a live
// SE session without touching the machine-global hosts file. Hooks
// gethostbyname (polcore imports it as ws2_32 ordinal 52 and imports no other
// resolver); the redirect must happen at name resolution because our DNS stub
// answers every pol.com name with the same address, so connect() can no longer
// tell the hosts apart. Off unless [redirect] enable=1.
void  netredir_init(const wchar_t* ini);
void  netredir_resolve();
int   netredir_enabled();
int   netredir_se_bypass();                        // raw [redirect] enable: bypassing to REAL SE
unsigned long netredir_effective_addr(const char*); // what the VIEWER resolves; 0 = no opinion
void* netredir_real_gethostbyname();
void* netredir_hook_gethostbyname();

// regredir.cpp (injector only) -- makes a COPIED install FOLDER-AWARE. The Viewer
// resolves its own tree from HKLM\SOFTWARE\PlayOnlineUS\InstallFolder (value 1000
// = the Viewer) via RegQueryValueEx, and that key is machine-global -> it points
// a second install (E:\PolViewerSE) back at the PRIMARY, whose patch/install step
// then writes to Program Files ("cannot write to the specified destination
// folder"). This hook re-roots any registry value under the primary Viewer path
// (`from`) at this copy (`to`) IN THIS PROCESS ONLY, so the machine registry and
// the primary install are untouched -- the netredir story, for install paths.
// Off unless [installdir] enable=1; the real pointer stays NULL when off, so a
// normal install never gets the hook. `from`/`to` are optional in the ini: `to`
// defaults to the running exe's own directory, `from` to the registered path.
void  installdir_init(const wchar_t* ini);
void  installdir_resolve();
void  installdir_eat_patch(); // EAT-patch advapi32!RegQueryValueEx: polcore has NO
                              // import directory, so an IAT patch can never reach it
int   installdir_enabled();
void* installdir_real_RegQueryValueExA();
void* installdir_hook_RegQueryValueExA();
void* installdir_real_RegQueryValueExW();
void* installdir_hook_RegQueryValueExW();
void  installdir_test_config(const char* fromA, const char* toA);  // test harness only

// [inputmode] swap_confirm -- swap the shell's Ok/Cancel (A/B) face buttons in
// Settings\Controller. Rides the installdir RegQueryValueEx hooks (armed if either on).
void  swapconfirm_configure(const wchar_t* ini);
int   swapconfirm_enabled();
int   forcepad_enabled();
int   seedpad_enabled();           // [inputmode] seed_pad -- supply ABSENT pad values          // [inputmode] force_pad -- serve title pad gates ON
// Human-readable state of the A/B swap: the current settings, and every value under
// PlayOnlineViewer\Settings and its subkeys, marking which hold Ok/Cancel and whether
// they are in scope. Deliberately does NOT assume Settings\Controller -- that
// assumption is what made the swap come out shifted. Returns the Ok/Cancel count.
int   swapconfirm_report(char* out, size_t cch);
// What the CLIENT will read for an action, split into its two sources so padmap can tell
// "we are overriding it" from "the registry says so" from "there is nothing there at all"
// -- a distinction the swap could not make, which is why it failed silently twice.
// Both return -1 for "no answer". polbtn_registry fills keyout with the key it read.
int   polbtn_override_by_name(const wchar_t* nameW);   // btn_* / pad_layout, live
int   polbtn_registry(const wchar_t* nameW, wchar_t* keyout, size_t cch);

// padmap.cpp -- LIVE controller button mapping. Permutes the pad's buttons inside the
// DirectInput device wrapper, so a binding takes effect on the client's very next poll
// instead of at the next launch, and works even where the Settings\Controller registry
// key does not exist (a fresh Proton prefix, i.e. every Steam Deck). Drives, and is
// driven by, the mapper in polsettings.cpp. [inputmode] pad_remap / pad_bind.
enum PadAction { PAD_OK = 0, PAD_CANCEL, PAD_MENU, PAD_NAVI, PAD_NACTIONS };
struct TitleProfile;                       // profiles.h -- the per-title pad map lives there
void  padmap_configure(const wchar_t* ini);
void  padmap_save(const wchar_t* ini);
int   padmap_enabled();
void  padmap_set_enabled(int on);
// the live wire, called from dinputhook.cpp's pad wrapper. `owner` is the profile of the
// TITLE that created this pad device (CreateDevice return address), or NULL for a device
// the shell created: a shell device gets the shell permutation (and stands down while a
// title runs), a title device gets ITS OWN profile's pad map -- each title reads the pad
// in its own order, so the shell's permutation would scramble it.
void  padmap_on_state(void* data, unsigned long cb, const TitleProfile* owner);
void  padmap_on_data(void* rgod, unsigned long cbOd, unsigned long n,
                     const TitleProfile* owner);
void  padmap_note_device(const char* product, int nbuttons);
void  padmap_note_title(int running);      // a title owns the pad -> stop remapping
                                           // (the SHELL device's gate; title devices are
                                           // matched by owner, not by this flag)
// bindings (action -> PHYSICAL button index; -1 = unbound)
int   padmap_binding(int action);
void  padmap_bind(int action, int phys);
// A preset carries an action->button map AND that hardware's button names, because they
// are one fact and drift apart if stored separately. Enumerate them rather than hardcode
// the list, so adding a layout adds its dialog button too. "off" clears the bindings.
void  padmap_preset(const char* name);     // "deck" | "xbox" | "ps" | "off"
int   padmap_layout_count();
const char* padmap_layout_key(int i);
const char* padmap_layout_label(int i);
void  padmap_swap(int a, int b);
void  padmap_snapshot_bindings(int out[PAD_NACTIONS], int* enabled);
void  padmap_restore_bindings(const int in[PAD_NACTIONS], int enabled);
void  padmap_format_bind(wchar_t* out, size_t cch);
// resolution + naming, for the dialog
int   padmap_pol_index(int action, char* whence, size_t cch);
// The slot decision as a PURE function of its inputs (measured / btn_*-pad_layout /
// registry), so the rule can be tested without arranging a registry the dev box does not
// have. -1 means "this source has no answer".
int   padmap_resolve_slot(int action, int measured, int ovr, int reg,
                          char* whence, size_t cch);
int   padmap_logical_for_phys(int phys);
int   padmap_action_for_phys(int phys);
const char* padmap_action_name(int action);
const char* padmap_button_name(int phys, char* buf, size_t cch);
// live view: 1 if a pad answered. src is "game" (the client's own device -- ground
// truth), "xinput" (fallback, index space ASSUMED) or "none".
int   padmap_live_buttons(unsigned char out[32], int* nbtn, const char** src);
int   padmap_device_seen(char* product, size_t cch);
int   padmap_last_down();
int   padmap_remapped_count();   // polls actually permuted -- "is it running at all"
// MEASURED slots. Everything else about which rgbButtons slot the client reads for an
// action is inference (an override we serve, the registry, POL's defaults); this is the
// client having been watched doing it, so it wins over all of them. Measure by turning
// the remap OFF and pressing the button that really performs the action today.
int   padmap_slot_observed(int action);
// Two actions resolving to ONE slot: the client cannot tell them apart, so the first
// claim wins and the loser is reported here (the colliding action, or -1). Measured live
// on a Deck, where ok and menu had both been learned as slot 2 and Confirm's binding was
// being silently overwritten -- a map that printed as configured and did nothing.
int   padmap_slot_conflict(int action);
void  padmap_learn_slot(int action, int phys);   // phys < 0 clears
// "press a button" capture. While armed the buttons are also swallowed on the way to
// the client, so binding one does not click whatever is behind the dialog.
void  padmap_capture_begin();
void  padmap_capture_cancel();
int   padmap_capture_poll();     // physical index once, then -1
int   padmap_capture_armed();
int   padmap_report(char* out, size_t cch);
int   padmap_selftest();

// padoverlay.cpp -- the SAME mapper, drawn on the Viewer's own DirectDraw surface and
// driven by the pad alone. It exists because a top-level window is invisible over the
// shell's surface (see polsettings.cpp's KNOWN LIMIT), which is the normal case on a
// Steam Deck -- so the dialog's live view, the whole point of the feature, never
// reaches the machine that needs it. Every edit goes through padmap; this owns pixels.
void  padoverlay_configure(const wchar_t* ini);
int   padoverlay_enabled();
int   padoverlay_active();
void  padoverlay_open();
void  padoverlay_close(int save);
void  padoverlay_toggle();               // the chord, from polsettings' watcher
// Fed from padmap's observer with the RAW pad -- physical buttons, pre-permutation.
void  padoverlay_feed(const unsigned char* btn, int nbtn, unsigned long pov);
int   padoverlay_swallows();             // padmap asks: hide the pad from the client?
// Called from dxhook's present hook. `surface` is an IDirectDrawSurface7*, `dstrect` an
// optional const RECT* -- both void* so polshim.h stays free of ddraw.h.
void  padoverlay_draw(void* surface, const void* dstrect);
int   padoverlay_selftest();

// gamecfg.cpp -- dump and DIFF the per-game registry settings (FFXI's are opaque
// numbered names). Marks what changed since the previous call, so running a title's own
// config app between two views identifies the value it wrote. Returns the change count.
// The key tree is WALKED, not named: see the note at the top of gamecfg.cpp on the five
// keys a hand-written table of publisher spellings could never reach.
int   gamecfg_report(const wchar_t* ini, char* out, size_t cch);

// THE TITLE'S OWN CONFIG APP. Where a game ships a settings UI, that UI is better
// than anything reimplemented here -- it knows the resolutions the game accepts, what
// its quality levels are called, and it is what SE tested. So the dialog offers it
// rather than guessing labels for numbers nobody here has measured.
//
// Fills `path` with the executable and returns false when it is not on this machine.
// `which` picks between a title's several tools: 0 = the main config, 1 = its gamepad
// config (NULL if it has none).
bool        gamecfg_configapp(const char* game, int which, wchar_t* path, size_t cch);
// The same, plus WHICH build answered: true means the repaired copy (FMO only -- built by
// fmocfg_build_en.py), false the stock tool. The warning text belongs to the
// stock tool's obstacles, so a caller that shows it must ask this first.
bool        gamecfg_configapp_ex(const char* game, int which, wchar_t* path, size_t cch,
                                 bool* is_fixed);
// A one-line warning to show beside the button, or NULL. FMO's config app refuses to
// start while the Viewer is open and its labels are unreadable outside a Japanese
// system locale -- a person deserves to know that BEFORE they click.
const char* gamecfg_configapp_warning(const char* game, int which);
// Launch it. Returns the Win32 error, 0 on success.
DWORD       gamecfg_configapp_launch(const char* game, int which);

// polsettings.cpp -- the editor for the rows above, with the diff report behind a
// Details... button (the same shape padmap_open uses for its own registry report).
// The DIFF report, as text, in a scrolling read-only window. Not a settings screen --
// the way to identify one of FFXI's numbered values is to open this, change the setting
// in the title's own config tool, and open it again to see which number moved.
void  gamecfg_show_report(HWND parent, const wchar_t* ini);
// Checks the config-app table against THIS machine: every game we offer a button for
// must resolve to a tool that is actually there, or we are drawing buttons that can
// only fail. Reads only.
int   gamecfg_selftest(void);

// logprune.cpp -- delete all but the newest `keep` of dir\<stem>.*.<ext>. The log is
// per-PID and pol.exe fans out into several processes, so without this an install
// accumulates hundreds. dry_run counts without deleting. Returns files removed.
int   log_prune(const wchar_t* dir, const wchar_t* logname, int keep, int dry_run);

// patchver.cpp -- the 288-byte per-title patch.ver codec, ported from
// the server's tools/patchver.py and validated against a real installed file. Interface
// is an install NONCE, so regfix repairs a title by writing a self-consistent
// (blob, key) pair rather than recovering the original.
unsigned __int64 patchver_key64(const char* keystring);
unsigned __int64 patchver_seed64(int content_id, const char* keystring);
void patchver_encrypt(unsigned char* buf, size_t len, unsigned __int64 seed);
void patchver_decrypt(unsigned char* buf, size_t len, unsigned __int64 seed);
void patchver_make_blob(const char* text, int content_id, const char* keystring, unsigned char out[0x120]);
bool patchver_read_blob(const unsigned char in[0x120], int content_id, const char* keystring,
                        char* text, size_t cch);

// fmoiid.cpp -- the on-disk FrontMissionOnline.dll JP->US interface-GUID patch
// transcribed from the offline patch_fmo_iids.py so a machine that
// installed FMO for itself gets it too. The served bundle is NOT a reliable backstop: the
// patched DLL sits at W20-0004 version 20060823_1 while that tree's latest is 20260813_1,
// so a client already stamped at latest is told it is current and never fetches it.
enum { FMOIID_ERROR = -1, FMOIID_UNPATCHED = 0, FMOIID_PATCHED = 1, FMOIID_MIXED = 2 };
struct FmoIidReport {
    int  state;                 // FMOIID_*, as found BEFORE any edit
    int  jp, us;                // GUID sites present before the edit
    int  substituted;           // sites rewritten this run
    bool dll_written;
    bool filetxt_written;
    char digest_before[23];
    char digest_after[23];
    char filetxt_listed[23];    // what file.txt claimed before we touched it
    char err[200];              // why nothing was done, when nothing was done
};
// True = the tree ends up patched AND file.txt agrees. `apply` false surveys only.
bool fmoiid_repair(const char* installdir, bool apply, bool force, FmoIidReport* r);
// The updater-manifest digest: base64(MD5) over SE's substituted alphabet, 22 chars.
bool pol_digest(const void* data, size_t len, char out[23]);

// uitrace.cpp -- record the TEXT of every screen the Viewer draws, by hooking the
// StringTable-id thunk in app.dll, and dump the last few at exit. Written because two
// launch failures in a row were diagnosed from the ABSENCE of log lines and both
// diagnoses were wrong: the shim could see COM/DirectX/sockets but not the Viewer's own
// error screens, which are not MessageBoxes and so slipped past [mx] entirely.
void  uitrace_config(const wchar_t* ini);
void  uitrace_apply_module(void* module_base, const char* module);
void  uitrace_dump(const char* why);
void  uitrace_summary();
// Self-termination hooks, exposed for patch_iat to swap into a title's OWN IAT (the EAT
// patch misses a statically-CRT-linked title like FE_Client.dll). NULL until armed / off.
void* uitrace_real_ExitProcess();      void* uitrace_hook_ExitProcess();
void* uitrace_real_TerminateProcess(); void* uitrace_hook_TerminateProcess();
void* uitrace_real_FatalAppExitA();    void* uitrace_hook_FatalAppExitA();

// regfix.cpp -- registration repair. Detect installed-but-unregistered titles and
// write the missing HKLM\...\PlayOnlineUS keys so they appear/launch. [regfix] enable=1;
// clean=1 undoes exactly what it added. Runs pre-entry, so writes land before the menu.
void  regfix_run(const wchar_t* ini);
int   regfix_enabled();
// Run the scan on demand and get a human-readable verdict for EVERY title (including
// the ones it rejected, and why) -- what the settings dialog's "Repair games now"
// shows. apply=0 reports without writing. Returns titles registered, or -1 if it could
// not even start. Independent of [regfix] enable.
int   regfix_scan(char* out, size_t cch, int apply);

// sessionwatch.cpp -- notice when the auth-band SESSION socket dies.
// Observation only: it never touches the socket or the return value.
void  sessionwatch_configure(const wchar_t* ini);
void  sessionwatch_note_connect(UINT_PTR s, unsigned port);
void  sessionwatch_note_recv(UINT_PTR s, int n, DWORD werr);
// Called by wakerecover.cpp on a detected resume: the auth band does not
// survive a suspend, and a session the client still believes in is what
// raises POL-0006 on its next dial.
void  sessionwatch_note_wake(unsigned long gap_ms);

// poltoken.cpp -- stamp the PlayOnline session id into FFXI's lobby packets, so
// the FFXI bridge can attribute a launch to a member instead of inferring it
// from the peer address (which every client shares behind NAT). Strictly
// additive: without the stamp the bridge behaves exactly as before.
void  poltoken_configure(const wchar_t* ini);
void  poltoken_note_send(const char* buf, int len);
int   poltoken_tag(const char* buf, int len, char* out, int outcap);
int   poltoken_selftest(void);
int   poltoken_session_hex(char out[17]);   // "u"+these 16 hex = POL session id; 0 = not signed in yet

// ffxiplug.cpp -- FFXI add-ons. Ashita and Windower are in-process hooks of the
// SAME pol.exe this shim proxies into, so supporting them is not an integration:
// it is standing our own FFXI-facing hooks down while one of them is present, plus
// a plain DLL loader. ffxiplug_compat_active() has exactly two consumers -- the
// d3d8 windowed override (caller_excepted) and the FFXiMain probe arming -- and
// both are the places where two hookers of one thing would collide.
void  ffxiplug_configure(const wchar_t* ini);
void  ffxiplug_on_module(void* base);       // every module load + the startup sweep
void  ffxiplug_load_now(const char* why);   // load [ffxi] plugins= (one shot)
void  ffxiplug_start(void);                 // load_at=startup path, after the log opens
int   ffxiplug_compat_active(void);
const char* ffxiplug_core(void);            // "Ashita" / "Windower", or NULL
int   ffxiplug_module_is_core(const char* path, const char* leaf); // caller IS a core module

// ffxicfg.cpp -- print FFXI's own settings table, which carries each setting's
// PlayOnline registry name ("0017") beside its value, type, min, max and default.
// READ-ONLY, and the decode gamecfg.cpp has never had. [ffxi] cfgdump (default 1)
// dumps once per FFXiMain.dll load, from a bounded poller -- the table is built
// some time after the module is mapped.
void  ffxicfg_configure(const wchar_t* ini);
void  ffxicfg_on_module(void* base);        // every module load + the startup sweep
void  ffxicfg_dump(const char* why);        // on demand (the [polctl] channel, tests)
void  ffxicfg_summary(void);
int   ffxicfg_selftest(void);

// autoupdate.cpp -- shared plain-HTTP plumbing: resolve the server the same way
// autoupdate does and GET with the same timeouts. sha256 compare is the ONLY integrity check anywhere in
// this chain -- there is no TLS in the stack -- so every consumer must verify.
bool  shim_http_bases(char out_[2][256], const char* subpath);   // ":51300" door first
BYTE* shim_http_get(const char* url, DWORD* out_len, DWORD cap); // malloc'd; caller frees
bool  shim_http_post(const char* url, const char* body, DWORD len,
                     const char* content_type, const char* extra_headers,
                     char* resp, DWORD respcap, DWORD* out_status);
bool  shim_sha256_hex(const BYTE* data, DWORD len, char out_[65]);
void  ffxiplug_summary(void);
int   ffxiplug_selftest(void);

// iniheal.cpp -- THE curated option table (one source of truth), plus the startup
// heal that adds any missing key with its default, so a new shim option appears on a
// deployed install too. polsettings.cpp draws its dialog from the same table.
// OPT_SERVER is OPT_TEXT plus a dropdown of the saved servers in [servers], so dev/prod
// can be switched without retyping an address on a touchscreen keyboard.
// OPT_HIDDEN is healed into the ini like every other row but is NOT drawn. It is for
// keys that a dedicated editor owns (the controller bindings, which the mapper writes
// live) or that its own editor supersedes: a stale text box beside a live editor is a
// silent overwrite waiting to happen, and the key still has to reach installs that
// update through the Viewer's patch manager, which never touches polshim.ini.
// OPT_GROUP -- a heading, not an option: no sec/key, label is the heading text.
//   iniheal skips it (nothing to write); the dialog draws it as a separator.
// OPT_PACK  -- ONE checkbox that owns SEVERAL keys, because some fixes are
//   several keys and a user should not have to know which. It has no key of its
//   own; `choices` lists the members as "sec.key=on:off|sec.key=on:off". The box
//   reads as ticked when every member is at its ON value, and ticking or
//   unticking writes all of them. The members stay in the table as their own
//   (usually OPT_HIDDEN) rows so iniheal still heals them individually.
// OPT_BUTTON -- an ACTION that belongs beside the settings it relates to, not in
//   the action strip along the bottom of the window. It owns no key (iniheal skips
//   it, Save ignores it); `choices` names the action so the dialog can dispatch on
//   it, and `label` is the button's caption. Added for the two FFXI character rows,
//   which are FFXI features and read as general-purpose buttons when they sit under
//   Save/Cancel with the server tools.
enum ShimOptType { OPT_BOOL, OPT_ENUM, OPT_TEXT, OPT_SERVER, OPT_HIDDEN,
                   OPT_GROUP, OPT_PACK, OPT_BUTTON };

// WHICH SERVER THIS ROW MAKES SENSE AGAINST. Some actions are only coherent in one
// mode: you EXPORT a character out of retail FFXI, and you IMPORT one onto your
// account on the private server. A row whose mode does not match is drawn DISABLED
// with the reason in its hint line -- deliberately not hidden. A setting that is
// simply absent is the failure mode this project has lost the most time to (see
// shim-two-locations, and every "the option does nothing" round): greyed-out with a
// reason can be read over chat, a missing row cannot.
enum ShimOptMode { OPTM_ANY = 0, OPTM_SE, OPTM_PRIVATE };
struct ShimOption {
    const wchar_t* sec;
    const wchar_t* key;
    const wchar_t* def;
    ShimOptType    type;
    const wchar_t* label;     // dialog caption, or the heading for OPT_GROUP
    // OPT_ENUM: "a|b|c", or "value=Label|value=Label" when the stored value is
    //   not what a person should have to read. A field with no '=' is both.
    // OPT_PACK: the member list, "sec.key=on:off|...".
    const wchar_t* choices;
    // DEVELOPER ROW: healed into the ini like any other, but drawn only when
    // [settings] show_dev=1. A separate field rather than another OPT_ type
    // because `type` says what CONTROL to draw and this says WHO SEES IT --
    // a developer row still needs to be a checkbox or a dropdown. Trailing and
    // optional, so every existing row keeps its 6-field initialiser and false.
    bool           dev;
    // One grey line under the row: what it is FOR, in the user's terms -- which
    // title it fixes, what breaks without it. Optional; NULL draws nothing.
    const wchar_t* hint;
    // MAY THIS KEY APPEAR IN A [<sec>.<module>] SECTION? Marks the knobs that
    // describe a TITLE's behaviour rather than the shim's.
    // Trailing and optional, so every
    // existing row keeps its initialiser and false.
    //
    // Setting this is NOT enough on its own: the module must also READ the key
    // through ini_int_title/ini_str_title, or the flag promises a per-title
    // override that silently does nothing. The two are checked against each
    // other by the selftest; do not mark a key here until its read
    // is converted.
    bool           per_title;
    // Which server mode this row applies to; OPTM_ANY (0) is every row that has no
    // opinion. Trailing and optional, so every existing initialiser keeps its shape.
    ShimOptMode    mode;
    // OPT_GROUP ONLY: the module leaf of the game this whole category is about,
    // or NULL. A tagged group is not listed in the sidebar on its own -- its rows
    // appear inside that game's section, so one game has one door.
    //
    // A DEDICATED FIELD, not a reused one. The first attempt put this in `key`,
    // which is NULL on group rows and looked free -- and the option-table selftest
    // rejected it on the spot: "headings and packs own no key; every other row
    // does". That invariant is what stops a heading from looking like a setting,
    // and it was right to keep.
    const wchar_t* title_of;
};
const ShimOption* shim_options(int* count);
void  iniheal(const wchar_t* ini);

// THE HEADING OF THE FIX GROUP, as a macro rather than a literal in two places:
// inimigrate()'s "put the game fixes back" walk finds the group by its label, and
// a heading somebody retitles must not silently empty that walk out.
#define SHIM_GROUP_FIXES  L"Making the games work"

// --- MIGRATION: repairing an install that is sitting on an OLD DEFAULT -------
//
// iniheal() only ever ADDS an absent key. That promise is what makes it safe, and
// it is also its blind spot: an install that received an OLDER shipped template
// carries the old value EXPLICITLY, so every later heal steps over it and the
// machine keeps running a superseded fix for ever. That is the state a second
// computer was found in -- Tetra Master's cursor drifting and Fantasy Earth
// mis-patching, months after both were fixed here -- and no amount of healing
// would ever have reached it.
//
// So a value this project ITSELF once shipped as a default, and has since
// replaced, is repairable. The rule is deliberately narrow: a key is rewritten
// only when its current text is one of the specific values named in the table as
// superseded. Anything else -- including a value the user chose -- is left alone,
// which keeps iniheal's promise intact for everything that is not a known-bad
// default of ours.
//
// [polshim] config_rev in the ini stamps how far an install has been migrated, so
// each step runs AT MOST ONCE. Without it, setting a key back to an old value
// deliberately would have it silently re-migrated on the next launch, which is the
// same "something changed my ini" complaint one level down.
void  inimigrate(const wchar_t* ini);
// The manual half, behind the settings dialog's "Put the game fixes back" button:
// force every row under SHIM_GROUP_FIXES to its table default and clear the
// retired keys, whatever config_rev says. This is the answer to "the games are
// broken and I do not want to go digging through the ini to find out which knob
// it was". Fills a human-readable report; returns the number of keys changed.
int   shim_reset_fixes(const wchar_t* ini, char* report, size_t cap);

// polsettings.cpp -- in-game settings dialog. A watcher thread waits for a keyboard
// chord or a gamepad chord and opens a Win32 dialog built from shim_options(), so
// settings are editable without alt-tabbing out to hand-edit polshim.ini -- which on
// a Steam Deck is most of a session's friction. [settings] enable=1 (default).
void  polsettings_start(const wchar_t* ini);
void  polsettings_open();     // open it now (used by the chord and by polctl)
void  polsettings_open_padmap(const wchar_t* ini);   // just the controller mapper
// The dialog's computed client size. Exposed so the self-test can assert it still
// fits a Steam Deck screen -- the table is not scrollable, so growth is a real risk.
void  settings_dialog_size(bool show_dev, int* cw, int* ch);

// The HiDPI fit both shim dialogs use (polsettings.cpp). The dialogs are laid
// out in 96-DPI constants, so a 200% monitor draws them at half size unless the
// thread is made DPI-unaware while the window lives and the OS scales it --
// reported 2026-09-02 as "incredibly small on a super high res monitor".
// Shared, not copied: a second copy is a second dialog that can regress to
// half-size on its own.
HANDLE ui_dpi_fit_begin(void);
void   ui_dpi_fit_end(HANDLE prev);

void  dx_configure(const wchar_t* ini);
// [dx] dpi_aware. Declares the process DPI-aware so Windows stops bitmap-
// stretching the window (a second resample on top of the game's own 640x480
// stretch) and so cursor/window coordinates are physical rather than logical.
// MUST be called before the first window is created -- dx_configure does.
void  dpi_declare(int mode);
// dx_configure runs BEFORE log_open(), so dpi_declare only records its outcome.
// Call this once the log exists or the evidence is lost.
// Fantasy Earth's GLOBAL.INI vs its UAC VirtualStore shadow -- fecfg.cpp. The
// config utility writes one copy and an elevated FE reads the other, so settings
// silently diverge. Off by default ([fecfg] enable=0): it writes a TITLE's config.
void  fecfg_configure(const wchar_t* ini);
void  fecfg_run(const wchar_t* ini);
void  dpi_log_result();
// Same deferred-logging contract as dpi_log_result: fmvskip_configure runs before
// log_open, so its "the skip is gated off on Windows" explanation is recorded and
// replayed once the log exists. Without this the setting silently does nothing.
void  fmvskip_log_result();
void  dx_resolve();
int   dx_enabled();
void  dx_summary();
void* dx_real_DirectSoundCreate8();
void* dx_hook_DirectSoundCreate8();
void* dx_real_DirectDrawCreateEx();
void* dx_hook_DirectDrawCreateEx();
// Display-mode trace. The TM session proved DirectDraw does NOT switch the
// monitor (coop level NORMAL, SetDisplayMode never called), so the resolution
// change has to come from the user32 API -- which nothing was watching.
void* dx_real_ChangeDisplaySettingsA();
void* dx_hook_ChangeDisplaySettingsA();
void* dx_real_ChangeDisplaySettingsW();
void* dx_hook_ChangeDisplaySettingsW();
void* dx_real_ChangeDisplaySettingsExA();
void* dx_hook_ChangeDisplaySettingsExA();
void* dx_real_ChangeDisplaySettingsExW();
void* dx_hook_ChangeDisplaySettingsExW();

// d3d8hook.cpp (injector only) -- Direct3D 8 interposition. The Viewer SHELL is
// DirectDraw, but every GAME is Direct3D (TM.dll, FFXiMain.dll, FE_Client.dll =
// d3d8; FrontMissionOnline.dll = d3d9), and a fullscreen D3D8 CreateDevice is
// what changes the monitor mode -- which is why the DirectDraw and
// ChangeDisplaySettings traces both came back empty. d3d8.h exists in no current
// SDK, so the ABI is hand-declared and the vtable is VALIDATED against the real
// desktop mode before a single slot is patched. Gated on [dx] d3d_enable=1.
void  d3d8_configure(const wchar_t* ini);
void  d3d8_resolve();
int   d3d8_enabled();
void  d3d8_summary();
void* d3d8_real_Direct3DCreate8();
void* d3d8_hook_Direct3DCreate8();

// Hand the d3d9 path into d3d8hook's windowed-mode support layer (mask
// alignment, cursor translation, focus guard). All of it is HWND/user32 work
// with nothing d3d8-specific about it; it was only ever keyed off a window the
// d3d8 CreateDevice path set, which is why FMO ran windowed with the Viewer's
// mask still covering it and its window taking no input.
void  d3d_set_game_window(HWND h);

// The white-flash fix: a title's own RegisterClass call, with the white system
// background rewritten black before the class exists. See d3d8hook.cpp.
void* d3d8_real_RegisterClassA();
void* d3d8_hook_RegisterClassA();
void* d3d8_real_RegisterClassExA();
void* d3d8_hook_RegisterClassExA();

// Is the LIVE device an EXCLUSIVE fullscreen one? Owned by d3d8hook.cpp and fed
// by both renderer paths (d3d9hook calls the setter), because the question is
// about the display, not about which API took it.
//
// It exists because of a measured crash: opening the shim's settings dialog while
// FFXI held an exclusive fullscreen device took the display away from that device
// and the title died (Steam Deck, 2026-08-15). Anything that is about to create
// or activate a top-level window has to ask this first.
//
// `windowed` is the FINAL value -- what the device was actually created or reset
// with, after any [dx] d3d_windowed override -- not what the title requested.
void  d3d_note_device_mode(int windowed);
int   d3d_exclusive_fullscreen();

// The rest of that support layer, so the d3d9 path runs the SAME sequence as the
// d3d8 one rather than a thinner copy that falls behind it. Split the way the
// d3d8 CreateDevice hook is: prepare BEFORE the real call (the window must be
// its final size before a windowed device is built against it), finish AFTER a
// successful one. See the block comment in d3d8hook.cpp.
int   d3d_caller_excepted(void* retaddr);                 // [dx] d3d_windowed_except
const char* d3d_caller_except_source(void* retaddr);      // NULL, or WHY it is excepted
void  d3d_prepare_game_window(void* retaddr, HWND game, UINT bw, UINT bh, int fit);
void  d3d_forget_backbuffer_size();                       // windowed override rejected
void  d3d_after_device_created(HWND game);
int   d3d_renderspy_requested();                          // [dx] d3d_renderspy

// d3d9hook.cpp -- the SAME windowed override for Direct3D 9. Needed because
// Front Mission Online is the ONLY d3d9 title (TM/FFXI/Fantasy Earth are d3d8,
// the Viewer shell is DirectDraw), so d3d8hook.cpp is structurally blind to it.
// Shares the [dx] d3d_* keys, with d3d9_* available to override one API alone.
void  d3d9_configure(const wchar_t* ini);
void  d3d9_resolve();
int   d3d9_enabled();
void  d3d9_summary();
void* d3d9_real_Direct3DCreate9();
void* d3d9_hook_Direct3DCreate9();

// wakerecover.cpp -- the lost-device half of the Direct3D contract, which the
// titles do not implement. Suspend the machine with a title running and its
// device is lost; a title written in 2003 for a desktop that never slept keeps
// presenting into it for ever, which is the black window after a Steam Deck
// sleep. This supplies the missing half: detect the resume (by clock skew, not
// by WM_POWERBROADCAST, which Proton does not reliably deliver), and drive
// TestCooperativeLevel/Reset from the title's OWN render thread until the
// picture is back -- standing down the moment the title turns out to handle it
// itself. Gated on [dx] wake_enable; rides the same hooks as d3d_enable.
//
// The ABI lives in the two hook files (the present-parameter structs differ and
// d3d8.h exists in no SDK), so they register a device with two callbacks and
// this file owns the policy. `reset` must Reset with the parameters the device
// was LAST successfully created or reset with, and must not re-enter the hook.
typedef HRESULT (*WakeTestFn)(void* dev);
typedef HRESULT (*WakeResetFn)(void* dev);
void  wake_configure(const wchar_t* ini);
void  wake_reload(const wchar_t* ini);
void  wake_summary();
int   wake_enabled();
void  wake_register_device(const char* api, void* dev, WakeTestFn test,
                           WakeResetFn reset, HWND wnd);
void  wake_forget_device(void* dev);
void  wake_note_window(HWND w);
// Hot paths -- an increment and a test unless an episode is running.
void  wake_note_present(void* dev, HRESULT hr);
void  wake_note_test(void* dev, HRESULT hr);
void  wake_note_reset(void* dev, HRESULT hr);

// Cursor translation for a fullscreen-only game running windowed: it positions
// and clips the pointer in raw screen coordinates it believes start at (0,0).
void* d3d8_real_SetCursorPos();
void* d3d8_hook_SetCursorPos();
void* d3d8_real_GetCursorPos();
void* d3d8_hook_GetCursorPos();
void* d3d8_real_ClipCursor();
void* d3d8_hook_ClipCursor();
// The COUNT half of the pointer, instrumentation only: ShowCursor keeps a
// per-queue display count, and while it is negative NO cursor is drawn whatever
// SetCursor says. Fantasy Earth's in-game pointer is the OS cursor on its own
// window class, so an unbalanced ShowCursor(FALSE) erases it outright. These say
// which module drives it negative.
void* d3d8_real_ShowCursor();
void* d3d8_hook_ShowCursor();
// Mouse capture, instrumentation only: a window holding the capture receives no
// non-client hit-testing, which is the measured cause shape of "the frame went
// inert after one drag". These log the caller and pass through.
void* d3d8_real_SetCapture();
void* d3d8_hook_SetCapture();
void* d3d8_real_ReleaseCapture();
void* d3d8_hook_ReleaseCapture();
void* d3d8_real_SetForegroundWindow();
void* d3d8_hook_SetForegroundWindow();
void* d3d8_real_BringWindowToTop();
void* d3d8_hook_BringWindowToTop();
void* d3d8_real_SetActiveWindow();
void* d3d8_hook_SetActiveWindow();
void* d3d8_real_GetSystemMetrics();
void* d3d8_hook_GetSystemMetrics();
void* d3d8_real_GetWindowRect();
void* d3d8_hook_GetWindowRect();
// The window a windowed override presents into -- hookspy needs it to express
// mouse coordinates relative to what the title actually renders.
// Called by gamestart.cpp as each title launches. One Viewer session can run
// several titles, and the d3d layer latches "which title am I" on the first
// CreateDevice caller -- so without this, a second launch keeps the FIRST
// title's module and wears its icon.
void  d3d8_new_title_launch(void);

HWND  d3d8_game_window();
LONG  d3d8_in_modal();        // the user is dragging/sizing our own frame
void  d3d8_set_window_icon(HWND h);   // the title's / PlayOnline's own icon
LONG  d3d8_user_minimized();  // the user minimised the game (not the game itself)

// inputgate.cpp -- ONE answer to "should the mouse reach the title right now?",
// applied at all THREE seams a POL title reads the mouse through (app.dll's
// GetCursorPos poll, SE's WH_MOUSE chain, DirectInput). Gates DELIVERY, never
// ACQUISITION -- putting DISCL_FOREGROUND back is what killed Fantasy Earth's
// clicks. [dx] mouse_focus_gate, default 1.
void  inputgate_configure(const wchar_t* ini);
void  inputgate_reload(const wchar_t* ini);
int   inputgate_enabled(void);
bool  inputgate_decide(bool ours_foreground, bool in_modal, bool minimized);
bool  inputgate_blocked(void);      // cheap (16ms cache); safe from any thread
// Each seam keeps its own `prev` and performs its own release on the edge.
// Returns 1 exactly once per unfocused episode, on the way OUT.
int   inputgate_edge(bool* prev_blocked, bool* blocked_out);
void  inputgate_note_blocked(int seam);   // 0=cursor 1=hookchain 2=dinput
void  inputgate_summary(void);
int   inputgate_selftest(void);

// THE KEYBOARD HALF -- a SEPARATE switch ([dx] key_focus_gate, default 1) over
// the same decision and the same 16 ms cache. Separate because the releases
// differ and because turning one off to test it must not turn the other off:
// the bug this fixes was created by a keyboard silently inheriting a mouse fix.
// Seams: 4 = DirectInput keyboard (dinputhook.cpp), 5 = GetAsyncKeyState /
// GetKeyState / GetKeyboardState (keystate.cpp), 6 = a SYSTEM-WIDE keyboard
// hook chain (hookspy.cpp).
int   inputgate_key_enabled(void);
bool  inputgate_key_blocked(void);
int   inputgate_key_edge(bool* prev_blocked, bool* blocked_out);
void  inputgate_note_key_blocked(int seam, int synthetic_ups);

// keystate.cpp -- seam 5. GetAsyncKeyState/GetKeyState/GetKeyboardState report
// the PHYSICAL keyboard and answer the same whichever window is in front, so a
// title that polls them keeps playing while you type somewhere else. Installed
// through inject.cpp's patch_iat by resolved address, so the shim's own hotkey
// reads (polsettings.cpp, tmevent.cpp) are untouched -- patch_iat skips g_self.
void  keystate_init(const wchar_t* ini);
SHORT keystate_filter(SHORT real, bool blocked);   // pure; the selftest's subject
void* keystate_real_GetAsyncKeyState(void);
void* keystate_hook_GetAsyncKeyState(void);
void* keystate_real_GetKeyState(void);
void* keystate_hook_GetKeyState(void);
void* keystate_real_GetKeyboardState(void);
void* keystate_hook_GetKeyboardState(void);
void  keystate_summary(void);
int   keystate_selftest(void);

// exitprompt.cpp -- what the X in a windowed title's title bar does. Ashita
// has no equivalent (checked three ways); this is ours. [dx] close_prompt:
// 1 ask (default), 0 the old silent "end the title", 2 always to the desktop.
#define EXITPROMPT_CANCEL      0
#define EXITPROMPT_TO_POL      1
#define EXITPROMPT_TO_DESKTOP  2
void  exitprompt_configure(const wchar_t* ini);
void  exitprompt_reload(const wchar_t* ini);
int   exitprompt_on_close(HWND game, const char* title_name);  // spy_proc's SC_CLOSE
int   exitprompt_ask(HWND owner, const char* title_name);      // the dialog alone
void  exitprompt_arm_desktop_exit(HWND game);          // == arm_close(..., TO_DESKTOP)
void  exitprompt_arm_close(HWND game, int mode);       // ask the title to close, watch, report
void  exitprompt_summary(void);
int   exitprompt_selftest(void);

// regserve (in regredir.cpp) -- ANSWER a title's registry read from the ini
// instead of writing the machine's hive. Ashita's trick; our hooks. Sections
// are named `reg:<key path suffix>`, e.g. [reg:SquareEnix\FinalFantasyXI].
// Nothing on the machine is modified and deleting the section undoes it.
void  regserve_configure(const wchar_t* ini);
int   regserve_count(void);
void  regserve_summary(void);
int   regserve_selftest(void);
// The forced-windowed backbuffer size (the game's own coordinate field);
// dinputhook maps the OS pointer into this for resolution-independent tracking.
void  d3d8_backbuffer_size(int* w, int* h);

// hookspy.cpp (injector only) -- the Windows hook chain that carries mouse input
// to a title. In a game's process TM.dll's ONLY user32 import is CallNextHookEx,
// and polcore/PolHook are the ones calling SetWindowsHookExA -- so this is the
// path the title actually reads. A WH_MOUSE hook carries MOUSEHOOKSTRUCT::pt in
// SCREEN coordinates, which at a real 640x480 fullscreen mode WERE the game's
// coordinates. Log-first: [dx] hook_translate defaults to 0.
void  hookspy_configure(const wchar_t* ini);
void  hookspy_resolve();
int   hookspy_enabled();
void  hookspy_summary();
void* hookspy_real_SetWindowsHookExA();
void* hookspy_hook_SetWindowsHookExA();
void* hookspy_real_SetWindowsHookExW();
void* hookspy_hook_SetWindowsHookExW();
// Shared EAT patcher: rewrite one export's RVA to point at our thunk. This is
// the only delivery that survives a POL1-packed module re-resolving its imports,
// which is what defeats IAT patching on TM.dll / app.dll / polcore.
bool  dx_eat_patch_export(HMODULE mod, const char* want, void* hook, const char* tag);
// Name a code address as module+RVA for the log, calling an unmapped address out
// loudly. Used by the d3d8/d3d9 vtable arming to say WHO holds a slot we expected
// to hold our own hook -- the one fact that turns "the hook stopped running" into
// a name. Defined in d3d8hook.cpp.
void  dx_describe_code(const void* p, char* out, size_t cb);

// dinputhook.cpp (injector only) -- DirectInput 8 mouse. Measured 2026-08-12:
// across a whole Tetra Master session the cursor hooks above logged FIVE calls,
// so user32 is NOT where the game's pointer comes from -- DirectInput is. That
// also explains the focus stealing (a DISCL_EXCLUSIVE mouse must be foreground)
// and the over-sensitivity (DirectInput hands out RAW device counts, and a
// modern mouse has ~4x the CPI of a 2003 one). Gated on [dx] dinput_enable=1.
void  dinput_configure(const wchar_t* ini);
void  dinput_resolve();
int   dinput_enabled();
void  dinput_summary();
void* dinput_real_DirectInput8Create();
void* dinput_hook_DirectInput8Create();

// inputmode.cpp (injector only) -- force the Viewer's keyboard/gamepad input
// mode instead of leaving it to the UseGameController registry value the prefs
// app writes. An in-memory 3-byte patch of pol.exe's settings loader (the flag
// lives at config[+0x21], loaded via a `sete dl` at pol.exe+0x7285), verified
// against a build fingerprint before writing; registry and disk are untouched.
// Off unless [inputmode] mode=force_gamepad|force_keyboard. Fixes the Steam
// Deck / Proton case where the controller mode is never set.
void  inputmode_configure(const wchar_t* ini);
void  inputmode_apply();
int   inputmode_enabled();

// shortcut.cpp -- offer a configured title as the Viewer's startup shortcut by
// appending "/game <token>" to pol.exe's command line, unless a real /game (a
// genuine polboot launch) was already passed. [shortcut] game=<content-id|off>,
// default off. Launch-time (restart-bound); see g_restart_keys.
void  shortcut_configure(const wchar_t* ini);

// titletag.cpp -- append " [PoL-Shim vX.Y.Z]" to the Viewer window title so the
// running build is visible at a glance. [polshim] titletag=1 (default on).
void  titletag_configure(const wchar_t* ini);
void  titletag_start();
void  titletag_stop();

// autoupdate.cpp -- fetch a newer PolHook.dll from the server at launch and swap
// it in, then tell the user to restart. The Viewer's own patch channel CANNOT do
// this on Windows: pol.exe imports PolHook.dll statically, so it is mapped for the
// life of the process and polcore's overwrite always fails ("failed to replace
// polhook.dll"). Windows does allow RENAMING a mapped file, so we rename ourselves
// aside and move the new DLL into place; the running image is unaffected and the
// next launch picks it up. [autoupdate] enable=1 arms it (default OFF in code).
// `autoupdate_pending_build()` is 0 until a swap succeeds, then the new build
// number -- titletag reads it to put "restart" in the window caption.
void  autoupdate_start(const wchar_t* ini, const wchar_t* self_path);
void  autoupdate_stop();
int   autoupdate_pending_build();
// The update boot guard -- phase two of the staged-update commit. _boot_guard
// runs at the TOP of startup(), before any hook or patch (that is where a bad
// staged build crashes) and regardless of [autoupdate] enable: it counts this
// boot against the .staged.ini sentinel install_new() wrote and rolls back to
// the kept .old after too many boots that never confirmed. _boot_confirm is the
// cheap half of confirmation, called from BOTH DllMain detach paths -- a clean
// exit proves the staged build survived (crashes never reach DETACH; cl_filter
// hands to WER, which terminates without notifying DLLs), except a sub-5 s exit,
// which is pol.exe's single-instance stub and proves nothing. Alloc-free, so it
// is safe on the process-teardown path.
void  autoupdate_boot_guard(const wchar_t* ini, const wchar_t* self_path);
void  autoupdate_boot_confirm(void);

// maskguard.cpp -- keep pol.exe's PlayOnlineMask* window ALIVE from process start.
// polcore's SetMaskWindowHandle/CreateInput are bound to that HWND, and its class
// wndproc is a bare DefWindowProcA thunk, so a close request from a window manager
// destroys it and the Viewer errors out and exits. Reported on a Steam Deck under
// gamescope, 2026-08-15 (Fantasy Earth, FMO). Arms at startup on purpose:
// d3d8hook's tame_mask_window() only runs after a successful CreateDevice, which
// is too late for a launch that dies before graphics init. [dx] mask_guard: 0 off,
// 1 protect (default), 2 protect + hide the mask from a one-window compositor.
//
// Since 2026-09-07 mode >= 1 also HOLDS THE INVARIANT "the mask never covers the
// screen, never sits on top, never holds the foreground" from the mask's own
// wndproc, for every title on every path -- the title-keyed CreateDevice arms in
// d3d8hook were structurally dead for Fantasy Earth. See maskguard.cpp's header.
//
// A refusal is PENDING, not final. pol.exe closes its own mask with WM_CLOSE on
// the way out, and refusing that hung the process on every exit (measured
// 2026-08-15). The watcher honours a refused close once the mask has been the
// process's last un-owned top-level window for [dx] mask_close_grace ms (default
// 2000; 0 = never honour). See the block comment in maskguard.cpp.
void  maskguard_configure(const wchar_t* ini);
void  maskguard_start();
void  maskguard_stop();
void  maskguard_summary();
int   maskguard_enabled();
// Ask the Viewer to shut down the way it shuts ITSELF down: WM_CLOSE to its own
// mask window. Not a kill -- pol.exe runs its own teardown. False = no mask.
bool  maskguard_request_shutdown(const char* why);
// Is this the Viewer's mask window (by identity, else by exact class name)?
bool  maskguard_is_mask(HWND h);
// [foc] hooks: true = the target is the mask, do not activate/raise it (logged).
bool  maskguard_refuse_activation(HWND h, const char* api);

// protondxvk.cpp -- keep Proton's Direct3D 8 on DXVK, and SAY which path we are on.
//
// Every POL title but FMO is d3d8, and stock Proton serves d3d8 through wined3d
// (OpenGL) unless PROTON_DXVK_D3D8=1 -- d8vk ships in the same Proton, opt-in. The flag
// is PER-PROTON-INSTALL, so a Proton version change silently reverts it; that happened
// 2026-08-27 and cost eleven days of half-speed rendering nobody could see, plus two
// published wrong causes, both since retracted.
//
//   [proton] report=1      read-only: log which d3d8 is loaded. One greppable line.
//   [proton] dxvk_d3d8=1   write the key into the ACTIVE Proton's user_settings.py.
//                          WARNING: Outside the game folder, and it affects EVERY title under
//                          that Proton -- the widest write this shim performs.
//
// WARNING: The heal lands on the NEXT launch: Proton chooses the d3d8.dll before this DLL
// exists. The log says so in those words, because a fix that looks inert gets undone.
void  protondxvk_configure(const wchar_t* ini);
void  protondxvk_heal(void);
// The file decision, split out so protondxvktest.cpp can drive it against a scratch
// tree instead of somebody's Steam install. install.sh's version of this same logic
// shipped two bugs that only running it caught -- it dropped the user's other settings,
// and its "is this file mine?" test matched the wrong quote style and would have DELETED
// a hand-written file. Both are pinned by the self-test here.
enum ProtonDxvkResult {
    PDX_SET = 0,        // key inserted into an existing file, other keys preserved
    PDX_ALREADY,        // already there; nothing written
    PDX_CREATED,        // no file existed; wrote a minimal one
    PDX_NO_PROTON,      // not a Proton directory
    PDX_NO_D8VK,        // Proton older than 9: ships no DXVK d3d8
    PDX_UNRECOGNISED,   // hand-written Python we will not guess at; left untouched
    PDX_WRITE_FAILED
};
int   protondxvk_apply_to(const char* proton_dir);
// Call once d3d8.dll is actually in the process (d3d8hook's Direct3DCreate8 hook).
// KEY: Do NOT test "is wined3d.dll loaded" instead -- it does not discriminate: Wine's
// ddraw.dll is built on wined3d and the Viewer shell draws through DirectDraw, so
// wined3d is loaded on a healthy DXVK session too (measured 2026-09-07).
void  protondxvk_report_d3d8(void);

