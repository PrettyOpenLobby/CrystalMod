// =============================================================================
// PolShimSetup.exe -- ONE-FILE installer for the self-loading PoL-Shim.
//
// WHY THIS EXISTS (and not the PowerShell script it replaces):
//   Install-PolHookProxy.ps1 works, but a .ps1 downloaded from the web carries
//   the Mark-of-the-Web, and the default RemoteSigned execution policy then
//   refuses it outright -- "the file ... is not digitally signed. You cannot run
//   this script on the current system." That reads as a REFUSAL, not a prompt:
//   there is no "run anyway" button in the message, so users stop there.
//   An .exe is subject to SmartScreen instead, which at least offers
//   "More info -> Run anyway", and goes away entirely once the binary is signed.
//   Signing this exe later needs no source change.
//
// It is also SELF-CONTAINED: PolHook.dll and the polshim.ini template are linked
// in as RCDATA resources (setup.rc), so there is one file to download and no way
// for a user to run the installer against a stale or missing payload. If the
// chosen server is reachable it prefers the server's copy (same
// http://<server>/shim/dist layout install.sh uses on the Deck), hash-verified,
// so an old setup.exe still installs the CURRENT shim.
//
// What installing does, exactly (identical to the script, and reversible):
//   PolHook.dll -> PolHook_orig.dll  (+ a .preproxy.bak belt-and-suspenders copy)
//   our proxy    -> PolHook.dll
//   polshim.ini beside it, with [redirect] server=<what you typed>
// pol.exe statically imports PolHook.dll, so it loads the whole shim by itself --
// no injector, no launcher, and (after this installer exits) no elevation.
//
// Console subsystem on purpose: the output IS the documentation of what changed
// on disk, and a failure has to be readable. Double-clicked, it holds the window
// open at the end; with --silent it never blocks.
// =============================================================================
#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "polshim.h"        // POLSHIM_VERSION / POLSHIM_BUILD -- one source of truth

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

// Resource ids -- must match setup.rc.
#define IDR_POLHOOK_DLL   101
#define IDR_POLSHIM_INI   102

// The server offered at the prompt; Enter accepts it. Overridable at build time
// (/DPOLSHIM_DEFAULT_SERVER=\"a.b.c.d\", or \"\" for a build that offers none and
// refuses to continue without an address), mirroring install.sh's DEFAULT_SERVER.
#ifndef POLSHIM_DEFAULT_SERVER
#define POLSHIM_DEFAULT_SERVER "play.openlobby.fyi"
#endif

// First door tried for the shim bundle. The 5130x band is what the Viewer itself uses
// for every PML fetch, so it is up on any working deployment; :80 is optional (a compose
// profile) and is tried only as a fallback.
#ifndef POLSHIM_BAND_PORT
#define POLSHIM_BAND_PORT 51300
#endif

// -----------------------------------------------------------------------------
// console helpers
// -----------------------------------------------------------------------------
static bool g_silent = false;

static void out(const char* tag, const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s %s\n", tag, buf);
    fflush(stdout);
}
#define ok(...)   out("[+]", __VA_ARGS__)
#define info(...) out("[*]", __VA_ARGS__)
#define warn(...) out("[~]", __VA_ARGS__)
#define bad(...)  out("[!]", __VA_ARGS__)

// Hold the window open when double-clicked from Explorer, so the result is
// readable. Detected by asking whether this console has any other process
// attached: launched from a shell it does, double-clicked it does not.
static void pause_if_own_console()
{
    if (g_silent) return;
    DWORD pids[4];
    if (GetConsoleProcessList(pids, 4) <= 1) {
        printf("\nPress Enter to close . . . ");
        fflush(stdout);
        (void)getchar();
    }
}

static int fail(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    bad("%s", buf);
    pause_if_own_console();
    return 1;
}

// -----------------------------------------------------------------------------
// small filesystem / process helpers
// -----------------------------------------------------------------------------
static bool file_exists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dir_exists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static void join(char* out_, size_t cch, const char* dir, const char* leaf)
{
    size_t n = strlen(dir);
    bool sep = n && (dir[n-1] == '\\' || dir[n-1] == '/');
    _snprintf_s(out_, cch, _TRUNCATE, "%s%s%s", dir, sep ? "" : "\\", leaf);
}
static void rstrip_slash(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\\' || s[n-1] == '/' || s[n-1] == ' ' || s[n-1] == '"')) s[--n] = 0;
}

// An open pol.exe HOLDS PolHook.dll (static import), so the swap cannot happen
// and a half-done install is worse than none. Checked by name across the whole
// process list rather than by mutex: the Viewer spawns children, and any one of
// them holding the DLL is enough to break the rename.
static bool pol_is_running(char* which, size_t cch)
{
    static const char* names[] = { "pol.exe", "polboot.exe", "PolFL.exe" };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32First(snap, &pe)) {
        do {
            for (int i = 0; i < 3 && !found; i++)
                if (_stricmp(pe.szExeFile, names[i]) == 0) {
                    if (which) strncpy_s(which, cch, pe.szExeFile, _TRUNCATE);
                    found = true;
                }
        } while (!found && Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool write_all(const char* path, const void* data, DWORD len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL r = WriteFile(h, data, len, &wrote, NULL);
    CloseHandle(h);
    return r && wrote == len;
}

static BYTE* read_all(const char* path, DWORD* out_len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL);
    BYTE* buf = (BYTE*)malloc(sz ? sz : 1);
    DWORD got = 0;
    if (!buf || !ReadFile(h, buf, sz, &got, NULL) || got != sz) { free(buf); CloseHandle(h); return NULL; }
    CloseHandle(h);
    *out_len = sz;
    return buf;
}

// -----------------------------------------------------------------------------
// embedded payload
// -----------------------------------------------------------------------------
static const BYTE* resource(int id, DWORD* len)
{
    HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(id), RT_RCDATA);
    if (!r) return NULL;
    HGLOBAL g = LoadResource(NULL, r);
    if (!g) return NULL;
    *len = SizeofResource(NULL, r);
    return (const BYTE*)LockResource(g);
}

// -----------------------------------------------------------------------------
// SHA-256 (bcrypt) -- used only to verify a DLL pulled from the server, so a
// truncated or man-in-the-middled plain-HTTP download can never be installed.
// -----------------------------------------------------------------------------
// A dotted quad is returned as it is; anything else is looked up. ws2_32 is
// loaded by hand so this file needs no winsock headers (they fight windows.h
// over include order) and the installer gains no static import.
static bool resolve_ipv4(const char* name, char* out_, size_t cap)
{
    unsigned a, b, c, d; char tail;
    if (sscanf_s(name, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail, 1) == 4 &&
        a < 256 && b < 256 && c < 256 && d < 256) {
        _snprintf_s(out_, cap, _TRUNCATE, "%u.%u.%u.%u", a, b, c, d);
        return true;
    }
    struct HostEnt { char* name; char** aliases; short type; short length; char** addrs; };
    typedef int      (WINAPI* PFN_START)(WORD, void*);
    typedef HostEnt* (WINAPI* PFN_GHBN)(const char*);
    typedef int      (WINAPI* PFN_CLEAN)(void);
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    if (!ws) return false;
    PFN_START start = (PFN_START)GetProcAddress(ws, "WSAStartup");
    PFN_GHBN  ghbn  = (PFN_GHBN) GetProcAddress(ws, "gethostbyname");
    PFN_CLEAN clean = (PFN_CLEAN)GetProcAddress(ws, "WSACleanup");
    bool found = false;
    BYTE wsadata[1024];
    if (start && ghbn && clean && start(0x0202, wsadata) == 0) {
        HostEnt* he = ghbn(name);
        if (he && he->type == 2 /* AF_INET */ && he->length == 4 && he->addrs && he->addrs[0]) {
            const BYTE* ip = (const BYTE*)he->addrs[0];
            _snprintf_s(out_, cap, _TRUNCATE, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            found = true;
        }
        clean();
    }
    FreeLibrary(ws);
    return found;
}

static bool sha256_hex(const BYTE* data, DWORD len, char out_[65])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL;
    BYTE digest[32]; bool okr = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != STATUS_SUCCESS) return false;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == STATUS_SUCCESS) {
        if (BCryptHashData(h, (PUCHAR)data, len, 0) == STATUS_SUCCESS &&
            BCryptFinishHash(h, digest, sizeof(digest), 0) == STATUS_SUCCESS) {
            for (int i = 0; i < 32; i++) _snprintf_s(out_ + i*2, 3, _TRUNCATE, "%02x", digest[i]);
            out_[64] = 0;
            okr = true;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return okr;
}

// -----------------------------------------------------------------------------
// HTTP GET (WinINet). Plain HTTP on purpose -- there is no TLS anywhere in this
// stack (see the POLP/PML protocols); integrity comes from the .sha256 compare,
// not from the transport.
// -----------------------------------------------------------------------------
static BYTE* http_get(const char* url, DWORD* out_len, DWORD cap)
{
    HINTERNET net = InternetOpenA("PolShimSetup/" POLSHIM_VERSION,
                                  INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return NULL;
    // Short timeouts: an unreachable server must cost seconds, not a hung install --
    // and we probe TWO doors (band, then :80), so this is paid twice before we give up
    // and install the embedded shim. 4s each keeps the worst case under ten seconds.
    DWORD tmo = 4000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT,  &tmo, sizeof(tmo));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT,  &tmo, sizeof(tmo));
    InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT,     &tmo, sizeof(tmo));

    HINTERNET req = InternetOpenUrlA(net, url, NULL, 0,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                     INTERNET_FLAG_NO_UI  | INTERNET_FLAG_PRAGMA_NOCACHE, 0);
    if (!req) { InternetCloseHandle(net); return NULL; }

    DWORD status = 0, slen = sizeof(status), idx = 0;
    if (HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &slen, &idx)
        && status != 200) {
        InternetCloseHandle(req); InternetCloseHandle(net);
        return NULL;
    }

    DWORD used = 0, size = 64 * 1024;
    BYTE* buf = (BYTE*)malloc(size);
    if (!buf) { InternetCloseHandle(req); InternetCloseHandle(net); return NULL; }
    for (;;) {
        if (used == size) {
            if (size >= cap) { free(buf); buf = NULL; break; }   // refuse an endless body
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

// -----------------------------------------------------------------------------
// install-folder discovery
//
// Order: what you told us -> what the Viewer itself believes -> the default path.
// The registry is authoritative because that is the same value the client reads
// (HKLM\SOFTWARE\<hive>\InstallFolder\1000, id 1000 = the Viewer); an install
// moved off the default path is found this way and nowhere else. Our exe is
// 32-bit so the WOW6432Node redirect happens for us.
// -----------------------------------------------------------------------------
static bool viewer_dir_from_registry(char* out_, size_t cch)
{
    static const char* hives[] = { "SOFTWARE\\PlayOnlineUS", "SOFTWARE\\PlayOnline", "SOFTWARE\\PlayOnlineEU" };
    for (int i = 0; i < 3; i++) {
        char sub[256];
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\InstallFolder", hives[i]);
        HKEY h;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        char val[MAX_PATH] = ""; DWORD type = 0, sz = sizeof(val) - 1;
        LONG r = RegQueryValueExA(h, "1000", NULL, &type, (LPBYTE)val, &sz);
        RegCloseKey(h);
        if (r != ERROR_SUCCESS || sz <= 1) continue;
        val[sz < sizeof(val) ? sz : sizeof(val) - 1] = 0;
        rstrip_slash(val);
        char pol[MAX_PATH]; join(pol, sizeof(pol), val, "pol.exe");
        if (file_exists(pol)) { strncpy_s(out_, cch, val, _TRUNCATE); return true; }
    }
    return false;
}

static bool viewer_dir_from_guess(char* out_, size_t cch)
{
    static const char* rel[] = {
        "\\PlayOnline\\SquareEnix\\PlayOnlineViewer",
        "\\PlayOnline\\Square\\PlayOnlineViewer",
        "\\Steam\\steamapps\\common\\FINAL FANTASY XI\\PlayOnlineViewer",
        "\\Steam\\steamapps\\common\\Final Fantasy XI\\SquareEnix\\PlayOnlineViewer",
    };
    static const char* roots[] = { "ProgramFiles(x86)", "ProgramFiles", "ProgramW6432" };
    for (int r = 0; r < 3; r++) {
        char base[MAX_PATH];
        if (!GetEnvironmentVariableA(roots[r], base, sizeof(base))) continue;
        for (int i = 0; i < (int)(sizeof(rel)/sizeof(rel[0])); i++) {
            char cand[MAX_PATH];
            _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%s%s", base, rel[i]);
            char pol[MAX_PATH]; join(pol, sizeof(pol), cand, "pol.exe");
            if (file_exists(pol)) { strncpy_s(out_, cch, cand, _TRUNCATE); return true; }
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// ini editing -- section-aware set of one key, matching the PowerShell installer
// and install.sh's awk. Deliberately minimal: the DLL's own iniheal adds every
// other missing key at its default on first run, so the installer never has to
// know the full option set (and cannot drift from it).
// -----------------------------------------------------------------------------
static void ini_set(const char* path, const char* section, const char* key, const char* value)
{
    DWORD len = 0;
    BYTE* raw = read_all(path, &len);
    char* text = (char*)malloc((raw ? len : 0) + 1);
    if (!text) { free(raw); return; }
    if (raw) memcpy(text, raw, len);
    text[raw ? len : 0] = 0;
    free(raw);

    // Build the output line by line. CRLF is preserved by copying each line's
    // original terminator; the file is hand-edited by users on both platforms.
    size_t cap = strlen(text) + strlen(key) + strlen(value) + strlen(section) + 64;
    char* out_ = (char*)malloc(cap);
    if (!out_) { free(text); return; }
    out_[0] = 0;

    bool in_sec = false, done = false;
    char* p = text;
    while (*p || p == text) {
        char* eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) + 1 : strlen(p);
        if (!linelen) break;

        char line[1024];
        size_t copy = linelen < sizeof(line) - 1 ? linelen : sizeof(line) - 1;
        memcpy(line, p, copy); line[copy] = 0;

        // trimmed view of the line, for matching only
        char t[1024]; strncpy_s(t, sizeof(t), line, _TRUNCATE);
        char* s = t; while (*s == ' ' || *s == '\t') s++;
        size_t tl = strlen(s); while (tl && (s[tl-1] == '\r' || s[tl-1] == '\n' || s[tl-1] == ' ')) s[--tl] = 0;

        if (s[0] == '[') {
            // leaving our section without having written the key -> write it now
            if (in_sec && !done) { strcat_s(out_, cap, key); strcat_s(out_, cap, "="); strcat_s(out_, cap, value); strcat_s(out_, cap, "\r\n"); done = true; }
            char* close_ = strchr(s, ']');
            if (close_) *close_ = 0;
            in_sec = (_stricmp(s + 1, section) == 0);
            strcat_s(out_, cap, line);
            p += linelen;
            continue;
        }

        // an existing (possibly commented-out) assignment of this key in this section
        bool is_key = false;
        if (in_sec) {
            char* q = s;
            if (*q == ';' || *q == '#') { q++; while (*q == ' ' || *q == '\t') q++; }
            size_t kl = strlen(key);
            if (_strnicmp(q, key, kl) == 0) {
                char* after = q + kl;
                while (*after == ' ' || *after == '\t') after++;
                is_key = (*after == '=');
            }
        }
        if (is_key) {
            // First hit becomes the new value; any FURTHER assignment of the same
            // key in the same section is dropped, not left behind -- a stale
            // duplicate below the one we wrote would win in some readers.
            if (!done) {
                strcat_s(out_, cap, key); strcat_s(out_, cap, "="); strcat_s(out_, cap, value); strcat_s(out_, cap, "\r\n");
                done = true;
            }
        } else {
            strcat_s(out_, cap, line);
        }
        p += linelen;
        if (!*p) break;
    }
    if (!done) {
        size_t n = strlen(out_);
        if (n && out_[n-1] != '\n') strcat_s(out_, cap, "\r\n");
        if (!in_sec) { strcat_s(out_, cap, "["); strcat_s(out_, cap, section); strcat_s(out_, cap, "]\r\n"); }
        strcat_s(out_, cap, key); strcat_s(out_, cap, "="); strcat_s(out_, cap, value); strcat_s(out_, cap, "\r\n");
    }

    write_all(path, out_, (DWORD)strlen(out_));
    free(out_);
    free(text);
}

// -----------------------------------------------------------------------------
// prompts
// -----------------------------------------------------------------------------
static void prompt(const char* question, const char* def, char* out_, size_t cch)
{
    if (g_silent) { strncpy_s(out_, cch, def, _TRUNCATE); return; }
    if (def && def[0]) printf("%s [%s]: ", question, def);
    else               printf("%s: ", question);
    fflush(stdout);
    char line[512];
    if (!fgets(line, sizeof(line), stdin)) { strncpy_s(out_, cch, def, _TRUNCATE); return; }
    size_t n = strlen(line);
    while (n && (line[n-1] == '\r' || line[n-1] == '\n' || line[n-1] == ' ' || line[n-1] == '\t')) line[--n] = 0;
    char* s = line; while (*s == ' ' || *s == '\t' || *s == '"') s++;
    rstrip_slash(s);
    strncpy_s(out_, cch, *s ? s : def, _TRUNCATE);
}

// -----------------------------------------------------------------------------
// revert
// -----------------------------------------------------------------------------
static int do_revert(const char* dir)
{
    char hook[MAX_PATH], orig[MAX_PATH];
    join(hook, sizeof(hook), dir, "PolHook.dll");
    join(orig, sizeof(orig), dir, "PolHook_orig.dll");

    if (!file_exists(orig))
        return fail("No PolHook_orig.dll in \"%s\" -- the proxy is not installed here.", dir);

    DeleteFileA(hook);
    if (!MoveFileA(orig, hook))
        return fail("Could not restore PolHook_orig.dll -> PolHook.dll (error %lu). Is the Viewer running?",
                    GetLastError());

    {
        FileTxtReport ft;
        if (filetxt_restore(dir, &ft))
            ok("Put SE's own file.txt back.");
        else if (ft.err[0])
            info("%s", ft.err);
    }
    ok("Reverted: SE's original PolHook.dll is back in place.");
    info("polshim.ini and polshim.*.log were left alone -- delete them if you want a clean tree.");
    pause_if_own_console();
    return 0;
}

// -----------------------------------------------------------------------------
static void usage(void)
{
    printf(
      "PoL-Shim setup " POLSHIM_VERSION " (build %d)\n\n"
      "  PolShimSetup.exe [options]\n\n"
      "  --dir=<path>      PlayOnlineViewer folder (default: auto-detect, then ask)\n"
      "  --server=<addr>   server to point the client at, a name or an IP address\n"
      "                    (default: ask, offering play.openlobby.fyi)\n"
      "  --gamepad         force controller mode ([inputmode] mode=force_gamepad)\n"
      "  --no-update       do not fetch a newer shim from the server first\n"
      "  --update-url=<u>  where to fetch it from: a URL, or `server` for\n"
      "                    http://<server>/shim/dist (default: the project's latest\n"
      "                    GitHub release)\n"
      "  --revert          put SE's original PolHook.dll back and exit\n"
      "  --force           install even while a Viewer is running (second installs)\n"
      "  --where           just report which install is found, change nothing\n"
      "  --silent          never prompt: take defaults, never wait on exit\n"
      "  --help\n\n"
      "The Viewer must be closed. Installing is reversible with --revert.\n",
      POLSHIM_BUILD);
}

int main(int argc, char** argv)
{
    char dir[MAX_PATH]        = "";
    char server[256]          = "";
    char update_url[512]      = "";
    bool gamepad = false, revert = false, no_update = false, force = false, where = false;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if      (_strnicmp(a, "--dir=",        6)  == 0) { strncpy_s(dir, a + 6, _TRUNCATE); rstrip_slash(dir); }
        else if (_strnicmp(a, "--server=",     9)  == 0) strncpy_s(server, a + 9, _TRUNCATE);
        else if (_strnicmp(a, "--update-url=", 13) == 0) { strncpy_s(update_url, a + 13, _TRUNCATE); rstrip_slash(update_url); }
        else if (_stricmp(a, "--gamepad")   == 0) gamepad   = true;
        else if (_stricmp(a, "--revert")    == 0) revert    = true;
        else if (_stricmp(a, "--no-update") == 0) no_update = true;
        else if (_stricmp(a, "--force")     == 0) force     = true;
        else if (_stricmp(a, "--where")     == 0) where     = true;
        else if (_stricmp(a, "--silent")    == 0 || _stricmp(a, "--yes") == 0) g_silent = true;
        else if (_stricmp(a, "--help")      == 0 || _stricmp(a, "/?") == 0) { usage(); return 0; }
        else if (a[0] != '-' && !dir[0]) { strncpy_s(dir, a, _TRUNCATE); rstrip_slash(dir); }  // bare path, like install.sh
        else { usage(); return fail("unrecognised option: %s", a); }
    }

    printf("\n=== PoL-Shim setup " POLSHIM_VERSION " (build %d) ===\n\n", POLSHIM_BUILD);

    // --- 1. where is the Viewer -------------------------------------------------
    if (!dir[0] && !viewer_dir_from_registry(dir, sizeof(dir)) && !viewer_dir_from_guess(dir, sizeof(dir))) {
        warn("Could not find a PlayOnline Viewer install automatically.");
        char typed[MAX_PATH];
        prompt("Folder containing pol.exe", "C:\\Program Files (x86)\\PlayOnline\\SquareEnix\\PlayOnlineViewer",
               typed, sizeof(typed));
        strncpy_s(dir, typed, _TRUNCATE);
    }
    rstrip_slash(dir);
    if (!dir_exists(dir)) return fail("Not a folder: \"%s\"", dir);

    char pol[MAX_PATH], hook[MAX_PATH], orig[MAX_PATH], bak[MAX_PATH], ini[MAX_PATH];
    join(pol,  sizeof(pol),  dir, "pol.exe");
    join(hook, sizeof(hook), dir, "PolHook.dll");
    join(orig, sizeof(orig), dir, "PolHook_orig.dll");
    join(bak,  sizeof(bak),  dir, "PolHook.dll.preproxy.bak");
    join(ini,  sizeof(ini),  dir, "polshim.ini");

    if (!file_exists(pol)) return fail("No pol.exe in \"%s\" -- pass the right folder with --dir=", dir);
    ok("PlayOnline Viewer: %s", dir);

    // --where answers "which install would this touch, and is the shim in it?"
    // without touching anything -- the first question in any support exchange, and
    // the only way to exercise auto-detect without committing to an install.
    if (where) {
        info("Shim installed here: %s", file_exists(orig) ? "YES (PolHook_orig.dll present)" : "no");
        {
            FileTxtReport ft;
            bool okft = filetxt_sync(dir, "PolHook.dll", false, &ft);
            info("file.txt agrees:    %s", !okft ? ft.err
                 : ft.no_manifest ? "no file.txt here"
                 : ft.already_ok ? "yes" : "NO -- run the installer again to repair it");
        }
        info("polshim.ini:         %s", file_exists(ini)  ? ini : "(none)");
        pause_if_own_console();
        return 0;
    }

    if (revert) return do_revert(dir);

    if (!file_exists(hook) && !file_exists(orig))
        return fail("No PolHook.dll in \"%s\". This does not look like a Viewer install.", dir);

    // The name check is deliberately global rather than per-folder: a Viewer from ANY
    // install can be holding a PolHook.dll open, and there is no cheap way to tell
    // whose. That is right for a user and wrong for this project's second/dev installs
    // (New-PolDevInstance, the SE-facing clone), where a live session in one tree must
    // not block patching another -- hence --force, which only skips the check. If the
    // file really is locked the rename below fails cleanly and nothing is left half-done.
    char running[MAX_PATH] = "";
    if (pol_is_running(running, sizeof(running))) {
        if (!force)
            return fail("%s is RUNNING. Close PlayOnline completely, then run this again "
                        "-- it holds PolHook.dll open.  (--force skips this check, for a "
                        "SECOND install while another one is live.)", running);
        warn("%s is running -- continuing because --force was given.", running);
    }

    // --- 2. which server --------------------------------------------------------
    if (!server[0]) {
        char env[256];
        if (GetEnvironmentVariableA("POLSHIM_SERVER", env, sizeof(env)) && env[0])
            strncpy_s(server, env, _TRUNCATE);
        else
            prompt("PlayOnline server address (IP or hostname)", POLSHIM_DEFAULT_SERVER, server, sizeof(server));
    }
    // No server, no install. The shim is a router: without an address it has
    // nothing to route to, and a copy installed "for later" is a Viewer that
    // silently connects nowhere.
    if (!server[0])
        return fail("A server address is required. Pass --server=<address>, set "
                    "POLSHIM_SERVER in the environment, or type one at the prompt.");
    ok("Server: %s", server);
    // The shim reads [redirect] server= as a DOTTED IPv4 ADDRESS and nothing else
    // (it parses it while the DLL loads, where a DNS lookup is not safe). This
    // prompt has always said "IP or hostname", so a typed hostname used to give
    // an install that armed nothing and connected nowhere, silently. Resolve it
    // here, once, and write the address.
    char server_ip[64] = "";
    if (!resolve_ipv4(server, server_ip, sizeof(server_ip)))
        return fail("Could not look up the address of \"%s\". Check the name and your "
                    "connection, or give the server's IP address with --server=<ip>.", server);
    if (_stricmp(server, server_ip) != 0) {
        ok("  which is %s", server_ip);
        info("If that server ever moves to a new address, run this installer again.");
    }

    // --- 3. payload: embedded, or newer from the server -------------------------
    DWORD dll_len = 0;
    const BYTE* dll = resource(IDR_POLHOOK_DLL, &dll_len);
    if (!dll || dll_len < 4096) return fail("This installer is corrupt: the embedded PolHook.dll is missing.");
    BYTE* downloaded = NULL;

    if (!no_update) {
        // Try the BAND port first, then :80. The band (5130x) is the door every client
        // already reaches, and it is always up; :80 is an optional service that a real
        // deployment may not run at all (on prod it is behind a compose profile because
        // the NAS UI owns port 80). Preferring :80 is what made "couldn't reach the
        // update server" the normal outcome rather than the exceptional one.
        // Default: the project's latest GitHub release, NOT the game server (see
        // POLSHIM_UPDATE_BASE in polshim.h). `--update-url=server` keeps the old
        // <server>/shim/dist layout for an operator who hosts their own build.
        char tried[2][600]; int ntry = 0;
        if (update_url[0] && _stricmp(update_url, "server") != 0) {
            strncpy_s(tried[ntry++], update_url, _TRUNCATE);
        } else if (!update_url[0]) {
            strncpy_s(tried[ntry++], POLSHIM_UPDATE_BASE, _TRUNCATE);
        } else if (server[0]) {
            _snprintf_s(tried[ntry++], sizeof(tried[0]), _TRUNCATE, "http://%s:%d/shim/dist", server, POLSHIM_BAND_PORT);
            _snprintf_s(tried[ntry++], sizeof(tried[0]), _TRUNCATE, "http://%s/shim/dist", server);
        }
        for (int t = 0; t < ntry; t++) {
            char u[600]; DWORD n = 0;
            _snprintf_s(u, sizeof(u), _TRUNCATE, "%s/PolHook.dll.sha256", tried[t]);
            BYTE* sha = http_get(u, &n, 4096);
            if (!sha && t + 1 < ntry) { free(sha); continue; }   // that door is shut; try the next
            strncpy_s(update_url, tried[t], _TRUNCATE);
            if (sha && n >= 64) {
                char want[65]; memcpy(want, sha, 64); want[64] = 0;
                char have[65]; sha256_hex(dll, dll_len, have);
                if (_stricmp(want, have) == 0) {
                    info("Server has the same shim as this installer -- using the built-in copy.");
                } else {
                    _snprintf_s(u, sizeof(u), _TRUNCATE, "%s/PolHook.dll", update_url);
                    DWORD dn = 0;
                    BYTE* got = http_get(u, &dn, 16 * 1024 * 1024);
                    char gh[65] = "";
                    if (got && dn > 4096 && sha256_hex(got, dn, gh) && _stricmp(gh, want) == 0) {
                        downloaded = got;
                        dll = got; dll_len = dn;
                        // "the server's" rather than "a newer": we compare hashes, not
                        // versions, and a rebuild of the same source changes the PE
                        // timestamp -- so a difference does not prove newness. The
                        // server is authoritative either way.
                        ok("Using the server's shim (%lu bytes, hash verified).", dn);
                    } else {
                        free(got);
                        warn("Server copy did not verify -- installing the built-in shim instead.");
                    }
                }
            } else {
                info("No update source reachable at %s -- installing the built-in shim.", update_url);
            }
            free(sha);
            break;                              // a door answered; do not try the other
        }
    }

    // --- 4. swap ---------------------------------------------------------------
    if (file_exists(orig)) {
        info("Already installed here -- updating the shim, keeping the existing backup.");
    } else {
        if (!CopyFileA(hook, bak, FALSE))
            warn("Could not write %s (error %lu) -- continuing; PolHook_orig.dll is the real backup.",
                 bak, GetLastError());
        if (!MoveFileA(hook, orig)) {
            free(downloaded);
            return fail("Could not rename PolHook.dll -> PolHook_orig.dll (error %lu). "
                        "Close the Viewer and any antivirus scan of this folder.", GetLastError());
        }
        ok("Backed up SE's PolHook.dll -> PolHook_orig.dll");
    }

    if (!write_all(hook, dll, dll_len)) {
        DWORD e = GetLastError();
        // Put the original back rather than leave the install with NO PolHook.dll.
        if (!file_exists(hook)) MoveFileA(orig, hook);
        free(downloaded);
        return fail("Could not write PolHook.dll (error %lu). Nothing was changed.", e);
    }
    ok("Installed the shim as %s (%lu bytes)", hook, dll_len);
    free(downloaded);

    // KEEP file.txt IN STEP, in the same breath as the swap. The manifest still
    // describes SE's PolHook.dll, and an install that disagrees with its own
    // manifest is one Check Files away from having the shim reverted -- and its
    // next Viewer update stops with "a file is missing, reinstall PlayOnline",
    // which is how a public player hit this on 2026-09-21. Never fatal: the shim
    // is installed and working by this point, and a manifest we could not repair
    // is worth a warning, not a failed install.
    {
        FileTxtReport ft;
        if (filetxt_sync(dir, "PolHook.dll", true, &ft)) {
            if (ft.written)
                ok("Updated file.txt so PlayOnline's own check agrees with the shim");
            else if (ft.no_manifest)
                info("No file.txt in this install -- nothing to keep in step.");
        } else {
            warn("%s", ft.err);
            warn("If PlayOnline ever says a file is missing, run this installer again.");
        }
    }

    // --- 5. config -------------------------------------------------------------
    if (!file_exists(ini)) {
        DWORD ini_len = 0;
        const BYTE* tpl = resource(IDR_POLSHIM_INI, &ini_len);
        if (tpl && ini_len && write_all(ini, tpl, ini_len)) ok("Wrote polshim.ini");
        else                                                warn("Could not write polshim.ini -- the shim will use its built-in defaults.");
    } else {
        info("polshim.ini already here -- keeping your settings (the shim adds any new keys itself).");
    }
    if (file_exists(ini)) {
        ini_set(ini, "redirect", "server", server_ip);
        ok("Set [redirect] server=%s", server);
        if (gamepad) { ini_set(ini, "inputmode", "mode", "force_gamepad"); ok("Set [inputmode] mode=force_gamepad"); }
    }

    // --- 6. what to do next -----------------------------------------------------
    printf("\n");
    ok("DONE.");
    printf("  1) Start PlayOnline the NORMAL way (pol.exe / your usual shortcut).\n"
           "     NOT a \"PlayOnline (shim)\" injector shortcut, if you have one -- the shim\n"
           "     now loads itself, and both together would load it twice.\n"
           "  2) The window title ends with \"[PoL-Shim v" POLSHIM_VERSION "]\" when it is active.\n"
           "  3) IN-GAME SETTINGS:  press  Home   (or  Ctrl+Shift+S )\n"
           "     On a controller:   Back + Start  (View + Menu)\n"
           "     Windowed mode, cursor and audio fixes, controller mapping and the\n"
           "     server address all live there -- including the key that opens it, so\n"
           "     you can change it to whatever you will actually remember.\n"
           "  4) Config file, if you prefer editing: %s\n"
           "  5) Undo anytime:  PolShimSetup.exe --revert\n", ini);

    pause_if_own_console();
    return 0;
}
