// titletag.cpp -- append the shim's version to the Viewer window title.
//
// So a user can see at a glance WHICH shim build is running, and therefore whether
// the latest update has reached them over the patch channel. Deliberately
// unobtrusive: it only APPENDS " [PoL-Shim vX.Y.Z]" to the existing caption, never
// replaces it, and touches nothing else on screen.
//
// [polshim] titletag=1 (default on) enables it. A tiny worker thread finds this
// process's main top-level window and keeps the tag present; if the app rewrites
// its own caption (the Viewer does, per screen), the next pass re-appends. Cost is
// one EnumWindows + a substring compare every ~2s, and it no-ops once tagged.
//
// Starting a thread from startup() is safe for the same reason polctl does it: the
// loader lock has been released by the time the thread runs, and thread lib-calls
// are disabled (DisableThreadLibraryCalls in DllMain).
#include "polshim.h"
#include <winsock2.h>          // after polshim.h, exactly as netredir.cpp does it

static int    g_titletag = 1;
static int    g_show_server = 1;              // [polshim] titletag_server
static char   g_probe[64] = "ci000.pol.com";  // [polshim] titletag_probe
static wchar_t g_names[512] = L"";            // [polshim] titletag_names
static wchar_t g_server[80] = L"";            // resolved label, empty until known
static DWORD  g_server_at = 0;                // GetTickCount of that resolve
static char   g_tag[64]  = "";        // " [PoL-Shim vX.Y.Z]" (ASCII; widened on use)
static HANDLE g_thread   = NULL;
static volatile LONG g_stop = 0;

static void build_tag()
{
    // Carry the BUILD number too: the version string only moves on features, so it
    // cannot answer "did my update actually land?" -- which is the question the tag is
    // there to answer in the first place.
    if (!g_tag[0])
        _snprintf_s(g_tag, sizeof(g_tag), _TRUNCATE, " [PoL-Shim v%s b%d]",
                    POLSHIM_VERSION, POLSHIM_BUILD);
}

void titletag_configure(const wchar_t* ini)
{
    g_titletag = GetPrivateProfileIntW(L"polshim", L"titletag", 1, ini);
    g_show_server = GetPrivateProfileIntW(L"polshim", L"titletag_server", 1, ini);

    wchar_t w[64] = L"";
    ini_str(L"polshim", L"titletag_probe", L"ci000.pol.com", w, _countof(w), ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_probe, sizeof(g_probe), NULL, NULL);

    ini_str(L"polshim", L"titletag_names", L"", g_names, _countof(g_names), ini);
    build_tag();
}

// Live-reload for the in-game settings dialog. Everything here is state the
// worker reads per pass, so new values land on its next ~2 s tick. The resolved
// server label is dropped so a changed probe/names/server setting refreshes on
// that same pass instead of riding out the 30 s re-resolve cache.
//
// The worker thread itself is deliberately NOT started or stopped: titletag_start
// is a one-shot, so titletag=0 at STARTUP means no worker exists and turning the
// tag on via reload stays restart-bound. Going 0 with a LIVE worker does take
// effect -- apply_once strips the tag when the flag is off.
void titletag_reload(const wchar_t* ini)
{
    g_titletag = GetPrivateProfileIntW(L"polshim", L"titletag", 1, ini);
    g_show_server = GetPrivateProfileIntW(L"polshim", L"titletag_server", 1, ini);

    wchar_t w[64] = L"";
    ini_str(L"polshim", L"titletag_probe", L"ci000.pol.com", w, _countof(w), ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_probe, sizeof(g_probe), NULL, NULL);

    ini_str(L"polshim", L"titletag_names", L"", g_names, _countof(g_names), ini);

    // Invalidate the cached resolve, not just its clock: an empty g_server is
    // the one state resolve_server always re-answers.
    g_server[0] = 0;
    g_server_at = 0;
    logf("[reload] titletag: titletag=%d titletag_server=%d titletag_probe=%s "
         "(server label re-resolves on the next worker pass)",
         g_titletag, g_show_server, g_probe);
}

// --- WHICH SERVER AM I ACTUALLY ON? ---------------------------------------
//
// Added 2026-08-19, and it is a bug-prevention feature rather than a nicety. A
// test laptop believed to be pointed at PROD was in fact talking to DEV. Both
// serve the same screens, so nothing on screen contradicted the belief, and the
// divergence was only caught by reading the SERVER's logs and noticing that the
// machine had never once connected. Hours went into diagnosing a "prod bug"
// that was a client talking to a different host.
//
// WE RESOLVE, WE DO NOT READ THE CONFIG. The question is NOT "what is this
// client configured to do" -- that is precisely what was believed and was
// wrong -- it is "where will it actually go". `ci000.pol.com` is the login
// directory and it is a HARDCODED template inside `polcore.dll`, movable only
// by the hosts file or by us, so whatever it resolves to right now IS the server this Viewer will
// log in to. Reading `[redirect] server` or an env.dat instead would reproduce
// the original fault: it would report the intent, not the outcome.
//
// Re-resolved every 30 s rather than cached once: a hosts edit lands under a
// running client, and a label that is confidently WRONG is worse than no label
// at all -- being wrong in a believable way is the whole failure mode here.
//
// GetProcAddress, not an import: netredir patches the TARGET's IAT for
// gethostbyname, and going through ws2_32's export directly keeps this reading
// the system resolver no matter what has been done to anyone's import table.
static void label_from(const wchar_t* ip);

static void resolve_server()
{
    if (!g_show_server) return;
    DWORD now = GetTickCount();
    if (g_server[0] && (now - g_server_at) < 30000) return;

    // ASK THE REDIRECTOR FIRST, and this order is the whole correctness of the
    // label. An earlier version went straight to the system resolver on the
    // reasoning that "config lies, the resolver does not" -- but when netredir
    // is armed IT is the resolver the Viewer sees (its IAT is patched), so the
    // system answer is merely the hosts file. On 2026-08-19 that put the DEV
    // box in the title bar of a client correctly aimed at PROD: precisely the
    // wrong answer, from the feature built to stop wrong answers.
    unsigned long redir = netredir_effective_addr(g_probe);
    if (redir) {
        unsigned char* r = (unsigned char*)&redir;   // network order
        wchar_t rip[24];
        _snwprintf_s(rip, _countof(rip), _TRUNCATE, L"%u.%u.%u.%u", r[0], r[1], r[2], r[3]);
        label_from(rip);
        g_server_at = now;
        return;
    }

    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryW(L"ws2_32.dll");
    if (!ws2) return;

    typedef struct hostent* (WINAPI *PFN_GHBN)(const char*);
    typedef int (WINAPI *PFN_WSAS)(WORD, LPWSADATA);
    PFN_GHBN ghbn = (PFN_GHBN)GetProcAddress(ws2, "gethostbyname");
    if (!ghbn) return;

    struct hostent* he = ghbn(g_probe);
    if (!he) {
        // The Viewer may not have called WSAStartup yet -- we can run before it
        // touches the network at all. One startup of our own (never cleaned up;
        // this DLL lives as long as the process) and the next pass resolves.
        static int tried = 0;
        if (!tried) {
            tried = 1;
            PFN_WSAS wsas = (PFN_WSAS)GetProcAddress(ws2, "WSAStartup");
            WSADATA wd;
            if (wsas) wsas(MAKEWORD(2, 2), &wd);
        }
        return;
    }
    if (he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0]) return;

    unsigned char* a = (unsigned char*)he->h_addr_list[0];
    wchar_t ip[24];
    _snwprintf_s(ip, _countof(ip), _TRUNCATE, L"%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    label_from(ip);
    g_server_at = now;
}

// `ip` -> the label shown in the caption. Split out so the redirector path and
// the system-resolver path can never format the answer differently.
static void label_from(const wchar_t* ip)
{
    // A friendly name for this address, if the user gave one:
    //   [polshim] titletag_names=198.51.100.10=HOME,198.51.100.20=LAN
    // An address with no label still shows as the raw IP -- an unknown server
    // must never render as NO server, which would read as "not connected".
    wchar_t label[48] = L"";
    if (g_names[0]) {
        const wchar_t* q = g_names;
        while (*q) {
            while (*q == L' ' || *q == L',') q++;
            if (!*q) break;
            const wchar_t* eq  = wcschr(q, L'=');
            const wchar_t* end = wcschr(q, L',');
            if (!end) end = q + wcslen(q);
            if (eq && eq < end) {
                size_t iplen = (size_t)(eq - q);
                if (iplen == wcslen(ip) && _wcsnicmp(q, ip, iplen) == 0) {
                    size_t n = (size_t)(end - eq - 1);
                    if (n >= _countof(label)) n = _countof(label) - 1;
                    wcsncpy_s(label, _countof(label), eq + 1, n);
                    break;
                }
            }
            q = (*end) ? end + 1 : end;
        }
    }

    // [redirect] enable=1 points every pol.com name at REAL Square Enix. Say so
    // in words: an SE address that merely LOOKS unfamiliar is not warning
    // enough for the one mode where a mistake reaches somebody else's servers.
    //
    // netredir_se_bypass(), NOT netredir_enabled(). A private-server override
    // ARMS the redirector with enable still 0, so netredir_enabled() means "the
    // redirector is active" -- and reading it as "going to SE" put a false
    // "SE!" in front of a private-server address on 2026-08-19, on a machine
    // whose own settings dialog correctly showed the SE box UNTICKED.
    const wchar_t* se = netredir_se_bypass() ? L"SE! " : L"";

    if (label[0])
        _snwprintf_s(g_server, _countof(g_server), _TRUNCATE, L"%s%s %s", se, label, ip);
    else
        _snwprintf_s(g_server, _countof(g_server), _TRUNCATE, L"%s%s", se, ip);
}

// Pick this process's largest visible, un-owned (top-level) window that has a
// caption. The caption filter skips the borderless PlayOnlineMask overlay (no
// title); the class filter is belt-and-suspenders in case a build gives it one.
struct FindWnd { DWORD pid; HWND best; long long bestArea; };

static BOOL CALLBACK pick(HWND h, LPARAM lp)
{
    FindWnd* f = (FindWnd*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid)            return TRUE;
    if (!IsWindowVisible(h))      return TRUE;
    if (GetWindow(h, GW_OWNER))   return TRUE;     // top-level only
    if (GetWindowTextLengthW(h) <= 0) return TRUE; // needs a caption to tag

    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, _countof(cls));
    if (wcsstr(cls, L"Mask"))     return TRUE;     // never the black transition overlay

    RECT r; if (!GetWindowRect(h, &r)) return TRUE;
    long long area = (long long)(r.right - r.left) * (r.bottom - r.top);
    if (area <= f->bestArea)      return TRUE;
    f->best = h; f->bestArea = area;
    return TRUE;
}

static HWND main_window()
{
    FindWnd f = { GetCurrentProcessId(), NULL, 0 };
    EnumWindows(pick, (LPARAM)&f);
    return f.best;
}

// The tag we WANT right now. Recomputed per pass rather than cached, because
// autoupdate can stage a new DLL mid-session and the caption is how the user is
// told to restart -- a tag fixed at startup could never say so.
static void desired_tag(wchar_t* out, size_t cch)
{
    resolve_server();

    // " | HOME 198.51.100.10", or nothing at all while the name has not resolved
    // yet. Deliberately ABSENT rather than guessed: "no answer yet" and "the
    // dev box" must not look the same on screen.
    wchar_t srv[96] = L"";
    if (g_show_server && g_server[0])
        _snwprintf_s(srv, _countof(srv), _TRUNCATE, L" | %s", g_server);

    int pend = autoupdate_pending_build();
    if (pend > 0)
        _snwprintf_s(out, cch, _TRUNCATE, L" [PoL-Shim v%S b%d -> b%d: RESTART to apply%s]",
                     POLSHIM_VERSION, POLSHIM_BUILD, pend, srv);
    else if (pend < 0)      // updated, but the new DLL carries no build marker
        _snwprintf_s(out, cch, _TRUNCATE, L" [PoL-Shim v%S b%d -- update ready: RESTART%s]",
                     POLSHIM_VERSION, POLSHIM_BUILD, srv);
    else
        _snwprintf_s(out, cch, _TRUNCATE, L" [PoL-Shim v%S b%d%s]",
                     POLSHIM_VERSION, POLSHIM_BUILD, srv);
}

static void apply_once()
{
    HWND h = main_window();
    if (!h) return;

    wchar_t cur[512];
    int n = GetWindowTextW(h, cur, _countof(cur));
    if (n <= 0) return;

    // titletag=0 arriving via live-reload: fall back to the UNTAGGED title. The
    // original caption is retained -- the tag is only ever APPENDED -- so
    // stripping our own marker restores it exactly. Cheap when already clean.
    if (!g_titletag) {
        wchar_t* mine = wcsstr(cur, L" [PoL-Shim ");
        if (mine) { *mine = 0; SetWindowTextW(h, cur); }
        return;
    }

    wchar_t wtag[224];          // version + pending-build notice + server label
    desired_tag(wtag, _countof(wtag));
    if (wcsstr(cur, wtag)) return;                 // already carries THIS tag

    // Strip any earlier tag of ours before appending, or a caption that was
    // tagged before an update would end up carrying both. The marker is the
    // opening bracket of our own tag, which the Viewer's own captions never use.
    wchar_t* old = wcsstr(cur, L" [PoL-Shim ");
    if (old) *old = 0;

    wchar_t next[760];
    _snwprintf_s(next, _countof(next), _TRUNCATE, L"%s%s", cur, wtag);
    SetWindowTextW(h, next);
}

static DWORD WINAPI worker(LPVOID)
{
    while (!g_stop) {
        apply_once();
        for (int i = 0; i < 20 && !g_stop; i++) Sleep(100);   // ~2s, stop-responsive
    }
    return 0;
}

void titletag_start()
{
    if (!g_titletag) return;
    build_tag();
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (g_thread) logf("[titletag] window title tag active: '%s'", g_tag);
    else          logf("[titletag] CreateThread failed (%lu) -- no title tag", GetLastError());
}

void titletag_stop()
{
    InterlockedExchange(&g_stop, 1);
    // Daemon thread: process teardown reaps it. No join on detach (loader lock).
}
