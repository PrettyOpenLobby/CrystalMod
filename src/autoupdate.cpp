// autoupdate.cpp -- replace our own PolHook.dll from the server, then tell the
// user to restart. Checks shortly after launch and then every
// [autoupdate] interval_min minutes for as long as the Viewer is open.
//
// The interval was added 2026-08-17. A POL session is long -- the Viewer gets
// opened once and left up -- so a launch-only check meant a build published in
// the evening reached nobody until the next restart, which on the machine the
// shim is developed on could be the following day. Each check is one 64-byte GET
// of the .sha256 unless it differs; interval_min=0 restores the single check.
//
// WHY THIS EXISTS, AND WHY IT IS THE SHIM'S JOB
//
// The Viewer's own patch channel CANNOT update this file on Windows, and that is
// structural rather than a bug we can fix server-side. `pol.exe` statically
// imports PolHook.dll -- it is the FIRST entry in its import table -- so the file
// is MAPPED for the whole life of the process. polcore (the patcher) stages a
// `.tmp2` and then tries to MoveFileEx over the live file, which Windows refuses,
// and the Viewer reports "failed to replace polhook.dll" on every launch. That is
// why the PS2 mirror's W2U-1000 `meta.json` has `latest_version` rolled back as a
// brake, and why the publish script refuses to publish into that tree.
//
// The asymmetry we exploit: **Windows forbids OVERWRITING a mapped file but
// ALLOWS RENAMING one.** So we never write over ourselves. We rename the live DLL
// out of the way and move the new one into its place. The running process is
// completely unaffected -- its image is already mapped, and a mapping survives a
// rename of the underlying directory entry -- so this is safe to do while the
// very code doing it is executing. The next launch loads the new file.
//
// This is the same trick `setup.cpp` uses (MoveFileA on PolHook.dll), which is why
// PolShimSetup.exe can update a Windows install and the patch channel cannot.
// Wine/Proton permits replacing a mapped file outright, which is why the Deck was
// never blocked and Windows always was.
//
// ORDER OF OPERATIONS, and why each step is where it is
//
//   1. hash the DLL ON DISK -- not our own build number. If a previous launch
//      already staged an update that the user has not restarted into, the on-disk
//      file already matches the server and we must do nothing. Comparing against
//      the RUNNING build would re-download it on every launch forever.
//   2. fetch <base>/PolHook.dll.sha256, compare. Equal -> done, nothing fetched.
//   3. fetch <base>/PolHook.dll and verify it against the hash we were just told.
//      A body that does not match is a truncated or tampered download and is
//      dropped -- this is the only integrity check in the chain, because the
//      transport is plain HTTP (there is no TLS anywhere in this stack).
//   4. refuse to go BACKWARDS (see the build marker below).
//   5. write PolHook.dll.new beside the live file -- same directory, therefore the
//      same volume, so the rename in step 6 is atomic rather than a copy.
//   6. rename live -> PolHook.dll.b<NN>.old, then .new -> PolHook.dll, rolling the
//      first rename back if the second fails. The window in which the install has
//      no PolHook.dll is two adjacent MoveFileW calls wide, and a failure there
//      restores the original rather than leaving the Viewer unlaunchable.
//
// PROGRESSIVE STAGING (added with the boot guard)
//
// A staged update no longer ends the session's checking. The worker keeps
// running on its interval, and because step 1 hashes the DISK, a build published
// after the stage simply differs again and replaces the staged file -- the swap
// is even safer the second time, since a staged-but-never-booted DLL is not
// mapped by anyone. Two consequences worth naming:
//
//   * the shim tracks whatever the server CURRENTLY advertises, not the first
//     thing it saw. The .sha256 is singular -- the server has exactly one
//     current build -- so "match the server" is the whole contract. That also
//     makes a server-side brake work: republishing an older (but >= running)
//     build un-stages a bad one on every machine that has not restarted yet.
//   * the file being renamed aside may itself be a staged build, so its backup
//     name comes from ITS marker, not from the running build's number, and the
//     boot-guard sentinel keeps pointing at the session's known-good copy.
//
// THE BOOT GUARD (autoupdate_boot_guard / the .staged.ini sentinel)
//
// Updating is a two-phase commit now. install_new() writes a sentinel beside the
// DLL recording what was staged and where the known-good copy went. The NEXT
// launch -- which is the first time the staged code actually runs -- counts its
// boot in the sentinel, and the build must then CONFIRM itself by surviving
// confirm_ms (default 20 s, past the window where hook installs and EAT patches
// run) or by exiting cleanly (a crash never reaches DLL_PROCESS_DETACH --
// cl_filter hands the exception on to WER, which terminates without notifying
// DLLs -- so a clean detach IS evidence of health; a sub-5 s exit proves nothing
// and is ignored, because pol.exe's single-instance stub loads us and exits
// cleanly within a second). Confirmation deletes the sentinel. rollback_after+1
// boots without a confirmation restore the known-good copy and QUARANTINE the
// bad build's hash: the worker refuses to re-download it until the server
// publishes something different.
//
// What the guard cannot cover: a staged DLL the LOADER rejects (corrupt PE,
// missing import). pol.exe then dies before any of our code runs, including the
// guard. The sha256 check upstream means such a file was published broken, not
// corrupted in flight -- that class is the publisher's to catch, not ours.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not restart the Viewer. Relaunching means getting past pol.exe's
// single-instance mutex, which the process we are inside still holds, and the
// mask guard has already cost this project one class of zombie that held exactly
// that mutex. Telling the user is cheap and correct; driving it is not.
//
// TWO THINGS THAT CAN UNDO A SUCCESSFUL UPDATE, both known and neither fixable here:
//   * Check Files / MSI self-repair restores what the SERVED manifest says
//     polhook.dll should be, and that manifest is the braked patch tree.
//   * A user who runs PolShimSetup.exe with an older embedded DLL and --no-update.
//
#include "polshim.h"
// winsock2.h AFTER polshim.h is safe here for the same reason it is in
// netredir.cpp: polshim.h defines WIN32_LEAN_AND_MEAN before windows.h, which
// omits the old winsock.h and so avoids the classic redefinition trap.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <bcrypt.h>
#include <ctype.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

// Stringify, so the compile-time build number can be baked into the marker below.
// Two levels: the inner one expands POLSHIM_BUILD before it is quoted.
#define POLSHIM_STR2(x) #x
#define POLSHIM_STR(x)  POLSHIM_STR2(x)

#ifndef POLSHIM_BAND_PORT
// The portal band door. `responders.py` serves /shim/ here rather than on :80
// because prod's :80 belongs to something else entirely -- so the band port is
// the host-independent one and is tried first. Keep in step with setup.cpp.
#define POLSHIM_BAND_PORT 51300
#endif

// The version marker. `POLSHIM_BUILD` is a compile-time integer, so a DLL we have
// only DOWNLOADED cannot be asked what build it is -- there is no export to call
// and no resource we control. Embedding a findable ASCII string means the new
// image can be interrogated with a substring search before it is installed, which
// is what makes the "never go backwards" rule enforceable.
//
// `volatile` and the extern linkage keep the optimiser from folding the string
// away as unreferenced; it must survive into .rdata to be findable.
extern "C" __declspec(dllexport) const char polshim_build_marker[] =
    "POLSHIM_BUILD_MARKER=" POLSHIM_STR(POLSHIM_BUILD) "=";

static int      g_enable   = 0;
static int      g_delay_ms = 10000;
// [autoupdate] interval_min -- keep checking while the Viewer is open, not just
// once at launch. 0 restores the old single check.
//
// A POL session is long: the Viewer is opened once and left up for an evening,
// so "checked at launch" meant a build published at 21:00 reached nobody until
// they next restarted -- which for the machine the shim is developed on could be
// the following day. The check is one 64-byte HTTP GET of the .sha256 unless it
// differs, so an hour is generous rather than expensive.
static int      g_interval_min = 60;
static int      g_keep     = 3;
static char     g_url[256] = "";
static char     g_prompt[16] = "title";
static HANDLE   g_thread  = NULL;
static volatile LONG g_stop = 0;
static volatile LONG g_pending_build = 0;    // 0 = nothing staged; else the new build
static wchar_t  g_ini[MAX_PATH];
//: Our own module path, handed in by startup(). Passed rather than re-derived
//: with GetModuleFileName so this module cannot disagree with the path the rest
//: of the shim is using -- and inject.cpp's copy is the one that decided where
//: the ini and the log live.
static wchar_t  g_self[MAX_PATH];

// --- boot-guard state (see the header comment). All of it is set by
// autoupdate_boot_guard(), which inject.cpp calls at the TOP of startup() --
// before any hook or patch, and regardless of [autoupdate] enable, because a
// staged build must be guarded even on an install that later turned updating off.
static int      g_confirm_ms     = 20000;  // [autoupdate] confirm_ms
static int      g_rollback_after = 2;      // [autoupdate] rollback_after
static wchar_t  g_sentinel[MAX_PATH];      // <self>.staged.ini
static char     g_boot_hash[65]  = "";     // sha256 of the DLL on disk at boot
static char     g_quarantine[65] = "";     // a rolled-back build's sha; never re-fetch
static volatile LONG g_confirm_due = 0;    // this process IS an unconfirmed staged build
static DWORD    g_boot_tick = 0;           // for the minimum-uptime rule on clean exit
static HANDLE   g_confirm_thread = NULL;

int autoupdate_pending_build()
{
    return (int)InterlockedCompareExchange(&g_pending_build, 0, 0);
}

// ---------------------------------------------------------------------------
// the sentinel -- one [staged] section in <self>.staged.ini. Profile APIs
// rather than a bespoke format: atomic enough for one-key updates, readable in
// notepad when someone is diagnosing an install by hand, and the same parser
// the rest of the shim's config already trusts.
// ---------------------------------------------------------------------------
static void sentinel_get(const wchar_t* key, char out_[], int n)
{
    wchar_t w[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"staged", key, L"", w, MAX_PATH, g_sentinel);
    WideCharToMultiByte(CP_ACP, 0, w, -1, out_, n, NULL, NULL);
}

static bool sentinel_set(const wchar_t* key, const wchar_t* val)
{
    return WritePrivateProfileStringW(L"staged", key, val, g_sentinel) != 0;
}

static bool sentinel_set_int(const wchar_t* key, int v)
{
    wchar_t b[16];
    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d", v);
    return sentinel_set(key, b);
}

// ---------------------------------------------------------------------------
// small helpers -- deliberately duplicated from setup.cpp rather than shared
// with THAT (a standalone EXE with no polshim.h and its own logging). They are
// exported through polshim.h (shim_ prefix) for other modules in this DLL;
// setup.cpp keeps its own copies for the same reason it always did.
// ---------------------------------------------------------------------------
bool shim_sha256_hex(const BYTE* data, DWORD len, char out_[65])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL;
    BYTE digest[32]; bool okr = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != STATUS_SUCCESS)
        return false;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == STATUS_SUCCESS) {
        if (BCryptHashData(h, (PUCHAR)data, len, 0) == STATUS_SUCCESS &&
            BCryptFinishHash(h, digest, sizeof(digest), 0) == STATUS_SUCCESS) {
            for (int i = 0; i < 32; i++)
                _snprintf_s(out_ + i * 2, 3, _TRUNCATE, "%02x", digest[i]);
            out_[64] = 0;
            okr = true;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return okr;
}

static BYTE* read_all_w(const wchar_t* path, DWORD* out_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD hi = 0, sz = GetFileSize(f, &hi);
    if (sz == INVALID_FILE_SIZE || hi) { CloseHandle(f); return NULL; }
    BYTE* b = (BYTE*)malloc(sz ? sz : 1);
    DWORD got = 0;
    if (!b || !ReadFile(f, b, sz, &got, NULL) || got != sz) { free(b); CloseHandle(f); return NULL; }
    CloseHandle(f);
    *out_len = sz;
    return b;
}

static bool write_all_w(const wchar_t* path, const void* data, DWORD len)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool okr = WriteFile(f, data, len, &put, NULL) && put == len;
    // Force it down before the rename: a crash between the write and the swap must
    // not leave a .new that LOOKS complete and hashes as garbage next launch.
    if (okr) FlushFileBuffers(f);
    CloseHandle(f);
    if (!okr) DeleteFileW(path);
    return okr;
}

BYTE* shim_http_get(const char* url, DWORD* out_len, DWORD cap)
{
    HINTERNET net = InternetOpenA("PolShim/" POLSHIM_VERSION,
                                  INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return NULL;
    // Short timeouts. This runs on a background thread so it cannot stall the
    // Viewer, but an unreachable server must still cost seconds, not minutes --
    // we probe two doors and pay this on each.
    DWORD tmo = 4000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT,    &tmo, sizeof(tmo));
    HINTERNET req = InternetOpenUrlA(net, url, NULL, 0,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                     INTERNET_FLAG_NO_UI  | INTERNET_FLAG_PRAGMA_NOCACHE, 0);
    if (!req) { InternetCloseHandle(net); return NULL; }
    DWORD status = 0, slen = sizeof(status), idx = 0;
    // Reject when the status query FAILS as well as when it is non-200. The old `&&`
    // short-circuited on a failed HttpQueryInfoA and then read the body anyway -- so an
    // error page (or a proxy interstitial) whose status we could not read got fetched
    // and only caught later by the hash/length check, if at all.
    if (!HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &status, &slen, &idx) || status != 200) {
        InternetCloseHandle(req); InternetCloseHandle(net);
        return NULL;
    }
    DWORD used = 0, size = 64 * 1024;
    BYTE* buf = (BYTE*)malloc(size);
    if (!buf) { InternetCloseHandle(req); InternetCloseHandle(net); return NULL; }
    for (;;) {
        if (used == size) {
            if (size >= cap) { free(buf); buf = NULL; break; }
            size *= 2;
            BYTE* nb = (BYTE*)realloc(buf, size);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        DWORD got = 0;
        if (!InternetReadFile(req, buf + used, size - used, &got)) { free(buf); buf = NULL; break; }
        if (got == 0) break;
        used += got;
    }
    InternetCloseHandle(req);
    InternetCloseHandle(net);
    if (!buf) return NULL;
    *out_len = used;
    return buf;
}

// POST `body` and read the reply -- the request/response sibling of
// shim_http_get, for endpoints that ANSWER (logship.cpp's http_post predates
// this and stays fire-and-forget). Same UA and connect/send timeouts; the
// receive timeout is longer because an import endpoint does real work before
// replying. Returns true when a status came back at all; *out_status carries
// the HTTP code and resp gets up to respcap-1 reply bytes, NUL-terminated
// (truncation is fine -- callers show it to a human). extra_headers may be
// NULL; each entry must end "\r\n".
bool shim_http_post(const char* url, const char* body, DWORD len,
                    const char* content_type, const char* extra_headers,
                    char* resp, DWORD respcap, DWORD* out_status)
{
    if (resp && respcap) resp[0] = 0;
    if (out_status) *out_status = 0;
    URL_COMPONENTSA uc;
    char hostb[256] = {0}, path[512] = {0};
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = hostb;    uc.dwHostNameLength = sizeof(hostb);
    uc.lpszUrlPath  = path;     uc.dwUrlPathLength  = sizeof(path);
    if (!InternetCrackUrlA(url, 0, 0, &uc)) return false;

    HINTERNET net = InternetOpenA("PolShim/" POLSHIM_VERSION,
                                  INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return false;
    DWORD tmo = 4000, rtmo = 20000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo,  sizeof(tmo));
    InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT,    &tmo,  sizeof(tmo));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &rtmo, sizeof(rtmo));

    HINTERNET conn = InternetConnectA(net, hostb, uc.nPort ? uc.nPort : 80,
                                      NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { InternetCloseHandle(net); return false; }
    HINTERNET req = HttpOpenRequestA(conn, "POST", path, NULL, NULL, NULL,
                                     INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD |
                                     INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!req) { InternetCloseHandle(conn); InternetCloseHandle(net); return false; }

    char hdrs[512];
    _snprintf_s(hdrs, sizeof(hdrs), _TRUNCATE, "Content-Type: %s\r\n%s",
                content_type ? content_type : "application/octet-stream",
                extra_headers ? extra_headers : "");
    BOOL ok = HttpSendRequestA(req, hdrs, (DWORD)strlen(hdrs), (LPVOID)body, len);
    DWORD status = 0, slen = sizeof(status), idx = 0;
    if (ok && HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                             &status, &slen, &idx) && out_status)
        *out_status = status;
    if (ok && resp && respcap > 1) {
        DWORD used = 0, got = 0;
        while (used < respcap - 1 &&
               InternetReadFile(req, resp + used, respcap - 1 - used, &got) && got)
            used += got;
        resp[used] = 0;
    }
    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(net);
    return ok != FALSE;
}

// The build number of a downloaded image, or -1 if it carries no marker.
// -1 is NOT a failure: every build before this feature existed lacks the marker,
// so the first update onto it must still be allowed. Only a marker that parses
// and is <= ours blocks the swap.
static int build_of_image(const BYTE* img, DWORD len)
{
    static const char kPfx[] = "POLSHIM_BUILD_MARKER=";
    const DWORD n = (DWORD)(sizeof(kPfx) - 1);
    if (len < n + 2) return -1;
    for (DWORD i = 0; i + n + 1 < len; i++) {
        if (img[i] != (BYTE)kPfx[0] || memcmp(img + i, kPfx, n) != 0) continue;
        int v = 0; DWORD j = i + n; bool any = false;
        while (j < len && img[j] >= '0' && img[j] <= '9') {
            // Saturate instead of capping at 1000000 and bailing: consuming every digit
            // keeps the trailing '=' check valid, and a saturated value simply reads as
            // "newer than us" (allowed) -- the safe direction. No int overflow.
            if (v > (0x7FFFFFFF - 9) / 10) v = 0x7FFFFFFF;
            else v = v * 10 + (img[j] - '0');
            j++; any = true;
        }
        // The trailing '=' pins the end of the number, so a marker that was
        // truncated by the download does not parse as a plausible build.
        if (any && j < len && img[j] == '=') return v;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// where to fetch from
//
// Order: an explicit [autoupdate] url, then the POLSHIM_SERVER environment
// override, then the [redirect] server the redirector was already told about.
// No server known = no update check.
// ---------------------------------------------------------------------------
// The server host + the two doors, for any /shim/<subpath> consumer. Exported
// so every consumer resolves "the server" the same way and none can disagree.
bool shim_http_bases(char out_[2][256], const char* subpath)
{
    char srv[128] = "";
    char v[128];
    if (GetEnvironmentVariableA("POLSHIM_SERVER", v, sizeof(v)) && v[0]) {
        _snprintf_s(srv, sizeof(srv), _TRUNCATE, "%s", v);
    } else {
        wchar_t wv[128] = L"";
        ini_str(L"redirect", L"server", L"", wv, _countof(wv), g_ini);
        if (wv[0]) WideCharToMultiByte(CP_ACP, 0, wv, -1, srv, sizeof(srv), NULL, NULL);
    }
    if (!srv[0]) return false;
    _snprintf_s(out_[0], 256, _TRUNCATE, "http://%s:%d/%s", srv, POLSHIM_BAND_PORT, subpath);
    _snprintf_s(out_[1], 256, _TRUNCATE, "http://%s/%s", srv, subpath);
    return true;
}

// autoupdate's own bases. [autoupdate] url= decides:
//   (empty)   the project's latest GitHub release (POLSHIM_UPDATE_BASE)
//   server    the game server's own /shim/dist, both doors -- for an operator
//             who hosts a build for their players
//   <a URL>   exactly that
// The default used to be the game server. That handed every player of a server
// whatever build its operator happened to keep in /shim/dist.
static bool base_urls(char out_[2][256])
{
    if (_stricmp(g_url, "server") == 0) return shim_http_bases(out_, "shim/dist");
    _snprintf_s(out_[0], 256, _TRUNCATE, "%s", g_url[0] ? g_url : POLSHIM_UPDATE_BASE);
    out_[1][0] = 0;
    return true;
}

// ---------------------------------------------------------------------------
// keep the newest `g_keep` PolHook.dll.b*.old and delete the rest, so a machine
// that updates often does not accumulate 400 KB per build forever. Same reasoning
// as logprune, and the same conservative bias: on any doubt, keep the file.
// ---------------------------------------------------------------------------
static void prune_old(const wchar_t* dir)
{
    if (g_keep < 0) return;
    // Never prune the boot guard's rollback target: it is the copy a crashing
    // staged build gets restored FROM, and with progressive staging it can be
    // older (by mtime) than every other .old in the directory.
    wchar_t protect[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"staged", L"old", L"", protect, _countof(protect), g_sentinel);
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\PolHook.dll.b*.old", dir);
    struct { wchar_t name[MAX_PATH]; FILETIME t; } found[64];
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (protect[0] && _wcsicmp(fd.cFileName, protect) == 0) continue;
        if (n < (int)(sizeof(found) / sizeof(found[0]))) {
            wcscpy_s(found[n].name, MAX_PATH, fd.cFileName);
            found[n].t = fd.ftLastWriteTime;
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (n <= g_keep) return;
    // newest first
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (CompareFileTime(&found[j].t, &found[i].t) > 0) {
                auto tmp = found[i]; found[i] = found[j]; found[j] = tmp;
            }
    for (int i = g_keep; i < n; i++) {
        wchar_t full[MAX_PATH];
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, found[i].name);
        if (DeleteFileW(full))
            logf("[autoupdate] pruned %S", found[i].name);
    }
}

// ---------------------------------------------------------------------------
// the swap
// ---------------------------------------------------------------------------
static bool install_new(const wchar_t* self, const BYTE* img, DWORD len, int newbuild,
                        const char* sha_hex)
{
    wchar_t dir[MAX_PATH], stage[MAX_PATH], old[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, self);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return false;
    *slash = 0;

    // Name the outgoing file for the build it actually IS. That is the running
    // build only on the FIRST swap of a session -- with progressive staging the
    // file being replaced may itself be a staged build the user never booted,
    // and naming its backup b<POLSHIM_BUILD> would overwrite the genuine backup
    // of the running build. Ask the bytes on disk instead.
    int outbuild = POLSHIM_BUILD;
    {
        DWORD outlen = 0;
        BYTE* out = read_all_w(self, &outlen);
        if (out) {
            int m = build_of_image(out, outlen);
            if (m >= 0) outbuild = m;
            free(out);
        }
    }

    _snwprintf_s(stage, MAX_PATH, _TRUNCATE, L"%s.new", self);
    _snwprintf_s(old,   MAX_PATH, _TRUNCATE, L"%s\\PolHook.dll.b%d.old", dir, outbuild);

    DeleteFileW(stage);                       // a leftover from a crashed attempt
    if (!write_all_w(stage, img, len)) {
        logf("[autoupdate] could not write %S (error %lu) -- nothing changed",
             stage, GetLastError());
        return false;
    }
    // An .old from an earlier update at the SAME build number would block the
    // rename; the newest copy of a given build is the one worth keeping.
    DeleteFileW(old);
    if (!MoveFileW(self, old)) {
        DWORD e = GetLastError();
        DeleteFileW(stage);
        logf("[autoupdate] could not rename the live DLL aside (error %lu) -- "
             "nothing changed. A locked file here is usually antivirus scanning "
             "the folder.", e);
        return false;
    }
    if (!MoveFileW(stage, self)) {
        DWORD e = GetLastError();
        // Put ourselves back. Leaving the install with NO PolHook.dll would make
        // pol.exe unlaunchable -- it imports us statically, so a missing file is a
        // loader error before any of our code runs and before we could repair it.
        if (!MoveFileW(old, self))
            logf("[autoupdate] *** CRITICAL: could not move %S back to %S (error %lu). "
                 "The install has no PolHook.dll -- rename it back by hand or re-run "
                 "PolShimSetup.exe.", old, self, GetLastError());
        else
            logf("[autoupdate] could not move the new DLL into place (error %lu) -- "
                 "rolled back, install unchanged", e);
        DeleteFileW(stage);
        return false;
    }
    logf("[autoupdate] installed build %d (was %d); previous kept as %S. "
         "RESTART the Viewer to run it.", newbuild, POLSHIM_BUILD, old);

    // Two-phase commit, phase one: record what was staged and where the
    // known-good copy went, so the NEXT launch's boot guard can count boots
    // against it and roll back if the staged build keeps dying (see the header).
    {
        _snwprintf_s(g_sentinel, MAX_PATH, _TRUNCATE, L"%s.staged.ini", self);
        // The rollback target is normally the file just renamed aside -- but when
        // THIS stage replaces a previous stage the user never booted (progressive
        // updating), that file is as unproven as the one it replaced. Keep
        // pointing at the session's known-good copy, which the earlier stage's
        // sentinel recorded, as long as it is still on disk.
        wchar_t target[MAX_PATH];
        const wchar_t* oldleaf = wcsrchr(old, L'\\');
        wcscpy_s(target, MAX_PATH, oldleaf ? oldleaf + 1 : old);
        wchar_t prevstate[16] = L"", prevold[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"staged", L"state", L"", prevstate, _countof(prevstate), g_sentinel);
        GetPrivateProfileStringW(L"staged", L"old",   L"", prevold,   _countof(prevold),   g_sentinel);
        if (_wcsicmp(prevstate, L"staged") == 0 && prevold[0]) {
            wchar_t full[MAX_PATH];
            _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, prevold);
            if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES)
                wcscpy_s(target, MAX_PATH, prevold);
        }
        wchar_t wsha[80] = L"";
        if (sha_hex) MultiByteToWideChar(CP_ACP, 0, sha_hex, -1, wsha, _countof(wsha));
        bool okr = sentinel_set(L"state", L"staged");
        okr &= sentinel_set(L"sha256", wsha);
        okr &= sentinel_set(L"old", target);
        okr &= sentinel_set_int(L"build", newbuild);
        okr &= sentinel_set_int(L"boots", 0);
        if (!okr)
            // Not fatal -- the update itself is in place -- but the next launch
            // boots UNGUARDED, which is worth one loud line rather than silence.
            logf("[autoupdate] could not write %S (error %lu) -- the staged build "
                 "will run without the boot guard", g_sentinel, GetLastError());
        else
            logf("[autoupdate] boot guard armed: %S watches the next launch "
                 "(rollback to %S after %d unconfirmed boots)",
                 g_sentinel, target, g_rollback_after + 1);
    }
    prune_old(dir);
    return true;
}

static void notify(int newbuild)
{
    InterlockedExchange(&g_pending_build, (LONG)newbuild);
    if (_stricmp(g_prompt, "dialog") != 0) return;      // "title" and "off" are passive
    // NEVER over an exclusive-fullscreen title. Same measured hazard that gates
    // the settings dialog (polsettings.cpp, exclusive_display_blocks_us): a new
    // top-level window takes the display from an exclusive device, the device is
    // lost and the title dies -- FFXI on a Steam Deck, 2026-08-15.
    //
    // This mattered less when the check ran once, ten seconds into launch, before
    // any title existed. On an interval it can land in the middle of a game, so
    // the guard is now load-bearing rather than theoretical. The update is already
    // staged and g_pending_build is already set, so the passive channel (the
    // window title) still carries the news -- nothing is lost by staying quiet.
    if (d3d_exclusive_fullscreen()) {
        logf("[autoupdate] update staged, but a title is holding the display "
             "EXCLUSIVELY -- not opening a dialog over it. The window title "
             "carries the notice instead.");
        return;
    }
    char who[32];
    if (newbuild > 0) _snprintf_s(who, sizeof(who), _TRUNCATE, "build %d", newbuild);
    else              _snprintf_s(who, sizeof(who), _TRUNCATE, "a newer build");
    char msg[320];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "A newer PlayOnline shim (%s) has been installed.\n\n"
                "You are currently running build %d. Close and reopen PlayOnline "
                "to start using it.\n\nNothing else needs to be done.",
                who, POLSHIM_BUILD);
    // MB_SETFOREGROUND only; no owner window. The Viewer's own windows live on
    // other threads and owning a box across threads is how you deadlock a message
    // pump that is mid-present.
    MessageBoxA(NULL, msg, "PlayOnline shim updated",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
}

// One check. Returns true when there is no point checking again this session --
// the update is staged, or something is wrong that another try cannot fix.
//
// Split out of worker() so the loop below can repeat it. Everything it decides is
// re-derived per call, including the hash of the DLL ON DISK: a check an hour
// from now must see what an earlier check may already have staged, not what this
// process was loaded from.
static bool check_once()
{
    const wchar_t* self = g_self;
    if (!self[0]) { logf("[autoupdate] no module path -- disabled"); return true; }

    DWORD havelen = 0;
    BYTE* have = read_all_w(self, &havelen);
    if (!have) { logf("[autoupdate] cannot read %S -- skipping", self); return false; }
    char havehash[65] = "";
    bool hashed = shim_sha256_hex(have, havelen, havehash);
    free(have);
    if (!hashed) { logf("[autoupdate] sha256 unavailable -- skipping"); return true; }

    char bases[2][256] = { { 0 }, { 0 } };
    if (!base_urls(bases)) {
        logf("[autoupdate] no server known ([autoupdate] url, POLSHIM_SERVER, "
             "or [redirect] server) -- skipping");
        return false;
    }

    for (int b = 0; b < 2 && !g_stop; b++) {
        if (!bases[b][0]) continue;
        char url[320];
        _snprintf_s(url, sizeof(url), _TRUNCATE, "%s/PolHook.dll.sha256", bases[b]);
        DWORD n = 0;
        BYTE* sha = shim_http_get(url, &n, 4096);
        if (!sha) continue;
        char want[65] = "";
        // The file is "<64 hex>  PolHook.dll"; take the hash and ignore the rest.
        DWORD take = n < 64 ? n : 64;
        memcpy(want, sha, take); want[take] = 0;
        free(sha);
        for (char* p = want; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strlen(want) != 64) { logf("[autoupdate] %s: malformed sha256 file", bases[b]); continue; }

        if (_stricmp(want, havehash) == 0) {
            // Once per CHANGE, not once per check: on an interval this line would
            // otherwise be the only thing in a long session's log. The state is
            // the pair we compared, so a republish of the same build still speaks.
            static char said[65] = "";
            if (strcmp(said, havehash) != 0) {
                strcpy_s(said, sizeof(said), havehash);
                int pend = autoupdate_pending_build();
                if (pend)
                    logf("[autoupdate] staged build %d matches the server (sha %.12s) "
                         "-- restart the Viewer to run it%s", pend, havehash,
                         g_interval_min > 0 ? "; re-checking on the interval" : "");
                else
                    logf("[autoupdate] up to date (build %d, sha %.12s)%s",
                         POLSHIM_BUILD, havehash,
                         g_interval_min > 0 ? " -- re-checking on the interval" : "");
            }
            return false;
        }

        // A build this install ROLLED BACK (boot guard) must not come home. The
        // quarantine is one hash, not a list, on purpose: the server has exactly
        // one current build, so the only publish we refuse is "the same bytes
        // that just failed to boot here" -- anything newly published differs.
        if (g_quarantine[0] && _stricmp(want, g_quarantine) == 0) {
            static char saidq[65] = "";
            if (strcmp(saidq, want) != 0) {
                strcpy_s(saidq, sizeof(saidq), want);
                logf("[autoupdate] %s still offers the build this install rolled "
                     "back (sha %.12s) -- quarantined until something different "
                     "is published", bases[b], want);
            }
            return false;
        }

        // Already fetched, parsed and refused this exact publish (the backwards
        // check below). The answer cannot change until the server serves a new
        // hash, so do not re-download the whole DLL every interval to re-refuse.
        static char refused[65] = "";
        if (refused[0] && _stricmp(want, refused) == 0)
            return false;

        logf("[autoupdate] %s advertises %.12s, we have %.12s -- fetching",
             bases[b], want, havehash);
        _snprintf_s(url, sizeof(url), _TRUNCATE, "%s/PolHook.dll", bases[b]);
        DWORD dl = 0;
        BYTE* img = shim_http_get(url, &dl, 16 * 1024 * 1024);
        if (!img) { logf("[autoupdate] download failed"); continue; }

        char got[65] = "";
        if (!shim_sha256_hex(img, dl, got) || _stricmp(got, want) != 0) {
            // Plain HTTP: this compare is the ONLY integrity check in the chain.
            logf("[autoupdate] REJECTED: body hashes %.12s but %.12s was advertised "
                 "(truncated or tampered) -- nothing installed", got, want);
            free(img);
            return false;      // a truncated download is worth one more try later
        }

        int newbuild = build_of_image(img, dl);
        if (newbuild >= 0 && newbuild < POLSHIM_BUILD) {
            // Strictly LOWER than the RUNNING build only. Two deliberate
            // allowances hide in that comparison:
            //   * an EQUAL build number is a lateral republish (a hotfix that
            //     reused the number) and must install -- `<=` refused it forever.
            //   * a build lower than a STAGED one but not lower than us is the
            //     server rolling its current build back -- the publish-side
            //     brake -- and must install too, un-staging the newer file.
            // Refused, but keep checking: on the interval a session can outlive
            // the mistake, and `refused` above keeps the re-checks to one 64-byte
            // GET rather than a re-download per hour.
            logf("[autoupdate] server offers build %d and we are %d -- refusing to "
                 "go backwards. Nothing installed; watching for a newer publish.",
                 newbuild, POLSHIM_BUILD);
            strcpy_s(refused, sizeof(refused), want);
            free(img);
            return false;
        }
        if (newbuild < 0)
            logf("[autoupdate] the offered DLL carries no build marker (pre-b27) -- "
                 "installing on the hash difference alone");

        // -1 rather than 0 when the new DLL carries no marker: an update DID
        // happen and the caption must still say so; 0 is reserved for "nothing
        // staged". Only the build NUMBER is unknown, not the fact of the update.
        bool staged = install_new(self, img, dl, newbuild, got);
        if (staged) {
            notify(newbuild < 0 ? -1 : newbuild);
            // Anything that installs supersedes a quarantine: the server moved on.
            g_quarantine[0] = 0;
        }
        free(img);
        // Staged or failed, keep checking. Failed is a rename losing a race, not
        // a wrong answer; staged is no longer final either -- PROGRESSIVE
        // STAGING (see the header) means a build published later in the session
        // replaces the staged file the same way it replaced ours.
        return false;
    }
    logf("[autoupdate] no server answered -- will try again %s",
         g_interval_min > 0 ? "on the interval" : "next launch");
    return false;
}

static DWORD WINAPI worker(LPVOID)
{
    // Let the Viewer finish launching first. An update is never urgent, and the
    // first seconds are when the client is opening its sockets and its D3D device.
    for (int slept = 0; slept < g_delay_ms && !g_stop; slept += 250)
        Sleep(250);

    for (;;) {
        if (g_stop) return 0;
        if (check_once()) return 0;              // decided; nothing more to do
        if (g_interval_min <= 0) return 0;       // one-shot, the old behaviour
        // [autoupdate] interval_s -- SECONDS, a test hook only. The progressive
        // path (stage, publish again, re-stage) is untestable at minute
        // granularity; the autoupdate test harness sets this to a few seconds. Not
        // healed, not documented in the dialog, wins over interval_min when >0.
        int secs = GetPrivateProfileIntW(L"autoupdate", L"interval_s", 0, g_ini);
        if (secs <= 0) secs = g_interval_min * 60;
        // 250 ms slices for the same reason the startup delay uses them:
        // autoupdate_stop() must not wait an hour for this thread to notice.
        for (int i = 0; i < secs * 4 && !g_stop; i++)
            Sleep(250);
    }
}

// ---------------------------------------------------------------------------
// the boot guard -- phase two of the commit install_new() opened. See the
// header comment for the design; mechanics and edge cases live here.
// ---------------------------------------------------------------------------

// Delete the sentinel -- the staged build has proven itself. `verify` guards the
// timer path: by the time confirm_ms elapses, THIS session's worker may already
// have staged a NEWER build (progressive), and the sentinel then describes that
// build, not us -- deleting it would strip the newcomer's guard. The clean-exit
// path cannot afford a profile read (alloc-free rule on process teardown), so it
// uses autoupdate_pending_build() for the same question: nonzero means a stage
// happened this session and the sentinel is no longer ours to clear.
static void boot_confirm_inner(bool verify, const char* how)
{
    if (!InterlockedCompareExchange(&g_confirm_due, 0, 0)) return;
    if (verify) {
        char sha[65] = "";
        sentinel_get(L"sha256", sha, sizeof(sha));
        if (_stricmp(sha, g_boot_hash) != 0) {
            InterlockedExchange(&g_confirm_due, 0);
            logf("[autoupdate] boot guard: sentinel now describes a newer staged "
                 "build -- leaving it armed (we are confirmed by surviving)");
            return;
        }
    } else if (autoupdate_pending_build() != 0) {
        InterlockedExchange(&g_confirm_due, 0);
        return;
    }
    if (InterlockedExchange(&g_confirm_due, 0)) {
        DeleteFileW(g_sentinel);
        logf("[autoupdate] boot guard: staged build confirmed healthy (%s) -- "
             "sentinel cleared", how);
    }
}

static DWORD WINAPI confirm_worker(LPVOID)
{
    for (int i = 0; i < g_confirm_ms / 250 && !g_stop; i++)
        Sleep(250);
    if (!g_stop) {
        char how[64];
        _snprintf_s(how, sizeof(how), _TRUNCATE, "survived %d ms", g_confirm_ms);
        boot_confirm_inner(true, how);
    }
    return 0;
}

// The clean-exit confirmation, called from BOTH DllMain detach paths. A crash
// never gets here (cl_filter hands to WER, which terminates without
// DLL_PROCESS_DETACH), so a clean detach is real evidence -- with one exception:
// pol.exe's single-instance stub loads us, hits its mutex and ExitProcess()es
// within a second, a clean exit that proves nothing about the build. Hence the
// minimum uptime. Alloc-free on purpose: one flag, one tick read, one delete.
void autoupdate_boot_confirm(void)
{
    if (!InterlockedCompareExchange(&g_confirm_due, 0, 0)) return;
    if (GetTickCount() - g_boot_tick < 5000) {
        // IMPORTANT: A PROVES-NOTHING BOOT MUST NOT COUNT AS A STRIKE EITHER (2026-08-27).
        // The boot was counted at startup, before anyone could know it would be a
        // sub-5s stub. Measured on the reference Windows install: three pol.exe launches in 27 seconds --
        // second instances while the real Viewer was running -- each exited clean
        // in seconds, each kept its +1, and a HEALTHY build 153 was rolled back
        // and quarantined on the publisher's own machine ("one of my clients
        // isn't getting the update"). Undo the count on the way out: neither a
        // confirmation nor a strike, exactly what "proves nothing" means.
        int boots = GetPrivateProfileIntW(L"staged", L"boots", 0, g_sentinel);
        if (boots > 0) {
            sentinel_set_int(L"boots", boots - 1);
            logf("[autoupdate] boot guard: sub-5s clean exit -- this boot proves "
                 "nothing about build health; boot count %d -> %d (not a strike)",
                 boots, boots - 1);
        }
        return;
    }
    boot_confirm_inner(false, "clean exit");
}

// The rollback itself: the same rename asymmetry the updater exploits, in
// reverse. WE may be the very image being renamed aside -- a mapping survives
// it, so the running (bad) build finishes its session while the DISK already
// holds the known-good copy for the next launch.
static bool boot_rollback(const wchar_t* self, const wchar_t* dir,
                          const wchar_t* oldleaf, int badbuild)
{
    wchar_t bad[MAX_PATH], oldfull[MAX_PATH];
    if (badbuild >= 0)
        _snwprintf_s(bad, MAX_PATH, _TRUNCATE, L"%s.b%d.bad", self, badbuild);
    else
        _snwprintf_s(bad, MAX_PATH, _TRUNCATE, L"%s.rolledback.bad", self);
    _snwprintf_s(oldfull, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, oldleaf);

    if (GetFileAttributesW(oldfull) == INVALID_FILE_ATTRIBUTES) {
        logf("[autoupdate] *** boot guard: cannot roll back -- the known-good "
             "copy %S is GONE. Leaving the staged build in place; re-run "
             "PolShimSetup.exe if it keeps crashing.", oldfull);
        return false;
    }
    DeleteFileW(bad);
    if (!MoveFileW(self, bad)) {
        logf("[autoupdate] boot guard: could not rename the staged DLL aside "
             "(error %lu) -- rollback not performed", GetLastError());
        return false;
    }
    if (!MoveFileW(oldfull, self)) {
        DWORD e = GetLastError();
        if (!MoveFileW(bad, self))
            logf("[autoupdate] *** CRITICAL: rollback left the install with no "
                 "PolHook.dll (error %lu undoing error %lu). Rename %S back by "
                 "hand or re-run PolShimSetup.exe.", GetLastError(), e, bad);
        else
            logf("[autoupdate] boot guard: could not restore %S (error %lu) -- "
                 "rolled the rename back, install unchanged", oldfull, e);
        return false;
    }
    return true;
}

// Called at the TOP of startup() -- before any hook install, EAT patch or module
// walk, because those are exactly where a bad staged build crashes -- and
// regardless of [autoupdate] enable, because the build that STAGED the update is
// not necessarily the build deciding whether to check for more.
void autoupdate_boot_guard(const wchar_t* ini, const wchar_t* self_path)
{
    if (!self_path || !self_path[0]) return;
    wcscpy_s(g_self, MAX_PATH, self_path);        // autoupdate_start() re-copies; harmless
    _snwprintf_s(g_sentinel, MAX_PATH, _TRUNCATE, L"%s.staged.ini", self_path);
    g_boot_tick      = GetTickCount();
    g_confirm_ms     = GetPrivateProfileIntW(L"autoupdate", L"confirm_ms",     20000, ini);
    g_rollback_after = GetPrivateProfileIntW(L"autoupdate", L"rollback_after", 2,     ini);

    char state[16] = "", sha[65] = "";
    wchar_t oldleaf[MAX_PATH] = L"";
    sentinel_get(L"state", state, sizeof(state));
    if (!state[0]) return;                        // no sentinel -- the common case
    sentinel_get(L"sha256", sha, sizeof(sha));
    for (char* p = sha; *p; p++) *p = (char)tolower((unsigned char)*p);
    GetPrivateProfileStringW(L"staged", L"old", L"", oldleaf, _countof(oldleaf), g_sentinel);
    int build = GetPrivateProfileIntW(L"staged", L"build", -1, g_sentinel);
    int boots = GetPrivateProfileIntW(L"staged", L"boots",  0, g_sentinel);

    if (_stricmp(state, "rolledback") == 0) {
        // The rollback already happened on an earlier boot; we are (presumably)
        // the restored build. All that survives of the incident is the
        // quarantine, which check_once() consults, and this line.
        strcpy_s(g_quarantine, sizeof(g_quarantine), sha);
        logf("[autoupdate] boot guard: build %d was rolled back on a previous "
             "launch; its sha %.12s is quarantined until the server publishes "
             "a different build", build, sha);
        return;
    }
    if (_stricmp(state, "staged") != 0) {
        logf("[autoupdate] boot guard: sentinel %S has unknown state '%s' -- "
             "clearing it", g_sentinel, state);
        DeleteFileW(g_sentinel);
        return;
    }

    // Whose boot is this? Hash the DISK: at load time the file and our mapping
    // are the same bytes, and the sentinel's hash is the one install_new()
    // verified. A mismatch means something ELSE replaced the DLL since the stage
    // (Check Files / MSI self-repair, PolShimSetup.exe, a hand copy) -- the
    // stage is moot and the sentinel with it.
    DWORD len = 0;
    BYTE* disk = read_all_w(self_path, &len);
    if (!disk) {
        logf("[autoupdate] boot guard: cannot read %S -- leaving the sentinel "
             "for a launch that can", self_path);
        return;
    }
    bool hashed = shim_sha256_hex(disk, len, g_boot_hash);
    int  diskbuild = build_of_image(disk, len);
    free(disk);
    if (!hashed) return;
    if (_stricmp(g_boot_hash, sha) != 0) {
        logf("[autoupdate] boot guard: the DLL on disk (%.12s) is not the staged "
             "build (%.12s) -- externally replaced since the stage; clearing the "
             "sentinel", g_boot_hash, sha);
        DeleteFileW(g_sentinel);
        return;
    }

    boots++;
    if (boots <= g_rollback_after) {
        if (!sentinel_set_int(L"boots", boots))
            logf("[autoupdate] boot guard: could not count this boot (error %lu) "
                 "-- the guard is armed but cannot roll back", GetLastError());
        InterlockedExchange(&g_confirm_due, 1);
        logf("[autoupdate] boot guard: boot %d of staged build %d -- confirming "
             "after %d ms or on a clean exit; %d unconfirmed boots roll back "
             "to %S", boots, build, g_confirm_ms, g_rollback_after + 1, oldleaf);
        // Same thread-from-startup() rule as the worker below. Runs regardless
        // of [autoupdate] enable -- confirmation is the guard's business, not
        // the updater's.
        if (!g_confirm_thread)
            g_confirm_thread = CreateThread(NULL, 0, confirm_worker, NULL, 0, NULL);
        return;
    }

    // Third strike (by default). Every one of those boots loaded fine and died
    // before confirming -- restore the known-good copy for the NEXT launch. THIS
    // session still runs the bad code (it is mapped); nothing to be done about
    // that beyond saying so.
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, self_path);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    if (boot_rollback(self_path, dir, oldleaf, diskbuild >= 0 ? diskbuild : build)) {
        sentinel_set(L"state", L"rolledback");
        sentinel_set_int(L"boots", boots);
        strcpy_s(g_quarantine, sizeof(g_quarantine), sha);
        // The caption channel: titletag shows "restart" whenever a build is
        // pending, which after a rollback is exactly the right nag.
        InterlockedExchange(&g_pending_build, -1);
        logf("[autoupdate] *** boot guard: staged build %d ROLLED BACK after %d "
             "boots without a confirmation. %S is the live DLL again; the bad "
             "build is kept beside it as *.bad and its sha %.12s is quarantined. "
             "RESTART the Viewer -- this session still runs the bad build.",
             build, boots, oldleaf, sha);
    } else {
        // Could not roll back; count the boot so the NEXT launch tries again
        // rather than resetting the strike count.
        sentinel_set_int(L"boots", boots);
    }
}

void autoupdate_start(const wchar_t* ini, const wchar_t* self_path)
{
    wcscpy_s(g_ini,  MAX_PATH, ini ? ini : L"");
    wcscpy_s(g_self, MAX_PATH, self_path ? self_path : L"");
    g_enable = GetPrivateProfileIntW(L"autoupdate", L"enable", 0, g_ini);
    if (!g_enable) return;
    g_delay_ms = GetPrivateProfileIntW(L"autoupdate", L"delay_ms", 10000, g_ini);
    g_interval_min = GetPrivateProfileIntW(L"autoupdate", L"interval_min", 60, g_ini);
    g_keep     = GetPrivateProfileIntW(L"autoupdate", L"keep", 3, g_ini);
    wchar_t w[256];
    ini_str(L"autoupdate", L"url", L"", w, _countof(w), g_ini);
    if (w[0]) WideCharToMultiByte(CP_ACP, 0, w, -1, g_url, sizeof(g_url), NULL, NULL);
    ini_str(L"autoupdate", L"prompt", L"title", w, _countof(w), g_ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_prompt, sizeof(g_prompt), NULL, NULL);

    // Same thread-from-startup() rule as titletag/maskguard: the loader
    // lock is released before this runs and thread library calls are disabled.
    // Double-start guard: autoupdate_reload() can call this again after an
    // enable flip, and two workers would race the rename dance against each
    // other.
    if (g_thread) return;
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (g_interval_min > 0)
        logf("[autoupdate] armed (first check in %d ms, then every %d min, "
             "prompt=%s, keep=%d)", g_delay_ms, g_interval_min, g_prompt, g_keep);
    else
        logf("[autoupdate] armed (one check, in %d ms; interval_min=0, "
             "prompt=%s, keep=%d)", g_delay_ms, g_prompt, g_keep);
}

void autoupdate_stop()
{
    InterlockedExchange(&g_stop, 1);
    // Signal only, never join. autoupdate_stop runs from DllMain's FreeLibrary
    // detach path (under the loader lock) and from autoupdate_reload's enable
    // flip; the worker may be mid-download, and waiting
    // on a thread that needs the loader lock to exit stalls under the lock. The DLL
    // never truly unloads (DllCanUnloadNow == S_FALSE), so process teardown reaps the
    // daemon thread -- same rule as maskguard_stop/titletag_stop.
    if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
    if (g_confirm_thread) { CloseHandle(g_confirm_thread); g_confirm_thread = NULL; }
}

// Live re-read for the in-game settings dialog. prompt/url/interval_min/keep
// are consulted by the worker on every pass (base_urls, the interval sleep,
// prune_old, notify), so new values take effect at its next check with no
// thread restart. enable drives the worker itself: off stops it (signal only,
// same as the detach path), on starts it the same way inject.cpp does at
// startup -- g_self was recorded there, so the path survives the round trip.
//
// delay_ms is deliberately NOT re-read: it is consumed once, before the first
// check, and that moment is gone until the next launch.
void autoupdate_reload(const wchar_t* ini)
{
    g_interval_min = GetPrivateProfileIntW(L"autoupdate", L"interval_min", 60, ini);
    g_keep     = GetPrivateProfileIntW(L"autoupdate", L"keep", 3, ini);
    wchar_t w[256];
    g_url[0] = 0;                     // overwrite wholesale; blank means "resolve"
    ini_str(L"autoupdate", L"url", L"", w, _countof(w), ini);
    if (w[0]) WideCharToMultiByte(CP_ACP, 0, w, -1, g_url, sizeof(g_url), NULL, NULL);
    ini_str(L"autoupdate", L"prompt", L"title", w, _countof(w), ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_prompt, sizeof(g_prompt), NULL, NULL);

    int enable = GetPrivateProfileIntW(L"autoupdate", L"enable", 0, ini);
    if (!enable && g_thread) {
        autoupdate_stop();
    } else if (enable && !g_thread) {
        // A stop above (or an earlier reload's) left g_stop latched; clear it or
        // the fresh worker exits on its first look. A worker signalled away
        // moments ago may take up to one HTTP timeout to notice, so a fast
        // off-then-on toggle risks a brief overlap -- accepted: the check is
        // idempotent and the guard in autoupdate_start keeps g_thread single.
        InterlockedExchange(&g_stop, 0);
        autoupdate_start(ini, g_self);
    }
    g_enable = enable;
    logf("[reload] autoupdate: enable=%d interval_min=%d keep=%d prompt=%s "
         "worker=%s (delay_ms is spent at startup, not re-read)",
         g_enable, g_interval_min, g_keep, g_prompt,
         g_thread ? "running" : "stopped");
}
