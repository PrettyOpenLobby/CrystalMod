// netredir.cpp -- per-install redirection of pol.com names to REAL Square Enix.
//
// WHY THIS EXISTS
//
// Pointing a client at the private server is a hosts-file edit,
// and the hosts file is MACHINE-GLOBAL.
// That is fine while there is one client, but the portal work needs a session
// against genuine SE -- the x-MD5-pol digest secret is minted by SE and dies in
// ~15 minutes -- and flipping the hosts file back and forth costs a full
// stack/DNS/relaunch cycle every time.
//
// The shim is delivered per-install (install.ps1 renames the component and
// drops polshim.dll beside it), and polshim.ini is read from next to the DLL.
// So a SECOND install directory already has an independent config: give that
// one `[redirect] enable=1` and it talks to Square Enix while the primary
// install keeps using the hosts file and our own server. No switching.
//
// WHY gethostbyname AND NOT connect
//
// Our DNS stub answers EVERY pol.com name with the same private address, so by
// the time connect() sees a sockaddr the hostname is gone and wh000 is
// indistinguishable from ci000. The redirect therefore has to happen at name
// resolution, before that information is lost. polcore imports winsock BY
// ORDINAL (52 = gethostbyname, the only resolver it imports -- there is no
// getaddrinfo in its import table), but the IAT patch in inject.cpp matches by
// resolved ADDRESS, so a by-name GetProcAddress here reaches the same pointer.
//
// The address table is data, not code: it lives in polshim.ini so it can be
// refreshed without a rebuild when SE moves a host.

#include "polshim.h"
// inet_addr is deprecated in favour of inet_pton, but this is a 32-bit shim
// living inside a 2002 client: the table values are dotted quads from an ini and
// the parse is not on any hot path.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>

static int  g_redirect = 0;
// The RAW [redirect] enable value -- "bypass to REAL Square Enix". Kept apart
// from g_redirect on purpose: a private-server override ARMS g_redirect with
// enable still 0 (see netredir_init), so g_redirect means "the redirector is
// active", NOT "pointing at SE". Conflating the two put a false "SE!" in the
// title bar on 2026-08-19 for a client aimed at the private server.
static int  g_se_bypass = 0;
static int  g_strict   = 0;        // unmapped pol.com name -> fail instead of leak

struct MapEnt { char host[64]; unsigned long addr; };   // addr in network order
static MapEnt g_map[64];
static int    g_nmap = 0;

// Single-IP catch-all target: when set, EVERY SE-domain name with no explicit
// [redirect] entry resolves here. Set by `server=<ip>` in the ini, or overridden
// at LAUNCH by the POLSHIM_SERVER env var or a `--polserver=<ip>` command-line arg
// -- so a Steam Deck user changes the server from the game's launch options with
// no editing of the ini inside the Proton prefix. 0 = unset (network byte order).
static unsigned long g_server = 0;

typedef struct hostent* (WINAPI *PFN_GHBN)(const char*);
static PFN_GHBN real_gethostbyname = NULL;

// gethostbyname's return value is a per-thread static in ws2_32, and callers
// may hold it until their next lookup, so ours is thread-local too. One address
// per name is all the Viewer ever uses.
static __declspec(thread) struct hostent  t_he;
static __declspec(thread) unsigned long   t_addr;
static __declspec(thread) unsigned long*  t_addr_list[2];
static __declspec(thread) char            t_name[64];
// h_aliases must be a valid NULL-terminated array, never NULL: the real
// gethostbyname always returns a (possibly empty) list, and a caller that walks it
// the canonical way -- for (char** a = he->h_aliases; *a; a++) -- dereferences NULL
// and crashes inside pol.exe on every redirected resolve. Point at this empty list.
static __declspec(thread) char*           t_aliases[1];   // { NULL }

int  netredir_enabled() { return g_redirect; }
int  netredir_se_bypass() { return g_se_bypass; }

static int host_eq(const char* a, const char* b)
{
    return _stricmp(a, b) == 0;
}

// True for names we consider SE's, i.e. the ones the private server would
// otherwise capture. Used only by strict mode's refusal.
static int is_pol_name(const char* n)
{
    size_t l = strlen(n);
    return (l > 8  && host_eq(n + l - 8,  ".pol.com"))
        || (l > 15 && host_eq(n + l - 15, ".playonline.com"))
        || (l > 9  && host_eq(n + l - 9,  ".sqex.net"));
}

// WHERE WILL THE VIEWER ACTUALLY GO FOR `name`? -- the SAME decision
// hook_gethostbyname makes, minus the logging and the strict-mode refusal.
//
// Added 2026-08-19 for the title-bar server label, and the bug it fixes is worth
// keeping in mind for any future caller: the label first asked the SYSTEM
// resolver directly (GetProcAddress -> ws2_32!gethostbyname) on the reasoning
// that config lies and the resolver does not. But when this redirector is armed
// it IS the resolver as far as the Viewer is concerned -- the Viewer's IAT is
// patched -- so the system answer is the hosts file, which may point somewhere
// else entirely. It reported the dev box for a client correctly aimed at prod.
//
// Returns 0 when the redirector has no opinion, and the caller should then fall
// back to the system resolver -- that is the un-armed case, where the hosts file
// really is the answer.
unsigned long netredir_effective_addr(const char* name)
{
    if (!g_redirect || !name) return 0;
    for (int i = 0; i < g_nmap; i++)
        if (host_eq(name, g_map[i].host)) return g_map[i].addr;
    if (g_server && is_pol_name(name)) return g_server;
    return 0;
}

static struct hostent* WINAPI hook_gethostbyname(const char* name)
{
    if (!g_redirect || !name)
        return real_gethostbyname(name);

    for (int i = 0; i < g_nmap; i++) {
        if (!host_eq(name, g_map[i].host))
            continue;
        t_addr = g_map[i].addr;
        t_addr_list[0] = &t_addr;
        t_addr_list[1] = NULL;
        strncpy_s(t_name, sizeof(t_name), name, _TRUNCATE);
        t_he.h_name      = t_name;
        t_aliases[0]     = NULL;
        t_he.h_aliases   = t_aliases;
        t_he.h_addrtype  = AF_INET;
        t_he.h_length    = 4;
        t_he.h_addr_list = (char**)t_addr_list;
        const unsigned char* b = (const unsigned char*)&t_addr;
        logf("[dns] %s -> %u.%u.%u.%u (SE, redirected)",
             name, b[0], b[1], b[2], b[3]);
        log_flush();
        return &t_he;
    }

    // Single-server catch-all: any SE name without an explicit entry above goes
    // to the configured server IP (server= / POLSHIM_SERVER / --polserver). The
    // per-host entries above win; this is the fallback the override installs.
    if (g_server && is_pol_name(name)) {
        t_addr = g_server;
        t_addr_list[0] = &t_addr;
        t_addr_list[1] = NULL;
        strncpy_s(t_name, sizeof(t_name), name, _TRUNCATE);
        t_he.h_name      = t_name;
        t_aliases[0]     = NULL;
        t_he.h_aliases   = t_aliases;
        t_he.h_addrtype  = AF_INET;
        t_he.h_length    = 4;
        t_he.h_addr_list = (char**)t_addr_list;
        const unsigned char* b = (const unsigned char*)&t_addr;
        logf("[dns] %s -> %u.%u.%u.%u (server override)", name, b[0], b[1], b[2], b[3]);
        log_flush();
        return &t_he;
    }

    // A pol.com name we have no address for is the dangerous case: falling
    // through sends it to the system resolver, which our own hosts file/DNS
    // stub answers -- so the "SE-facing" client would silently talk to the
    // private server for that one host and the capture would be a quiet lie.
    // Say so loudly, and in strict mode refuse rather than leak.
    if (is_pol_name(name)) {
        logf("[dns] %s -> UNMAPPED%s -- add it to [redirect] in polshim.ini",
             name, g_strict ? " (refused: strict=1)" : " (FALLING THROUGH to the "
             "system resolver, which may be the private server)");
        log_flush();
        if (g_strict) {
            WSASetLastError(WSAHOST_NOT_FOUND);
            return NULL;
        }
    }
    return real_gethostbyname(name);
}

void netredir_resolve()
{
    if (!g_redirect) return;                 // stay out of every IAT when off
    HMODULE ws2 = LoadLibraryW(L"ws2_32.dll");
    if (ws2) real_gethostbyname = (PFN_GHBN)GetProcAddress(ws2, "gethostbyname");
}

void* netredir_real_gethostbyname() { return (void*)real_gethostbyname; }
void* netredir_hook_gethostbyname() { return g_redirect ? (void*)hook_gethostbyname : NULL; }

// Launch-time server override sources (Steam Deck sets these in the game's launch
// options). Both return the IP in network byte order, or INADDR_NONE if absent.
static unsigned long server_from_env()
{
    char v[64];
    DWORD n = GetEnvironmentVariableA("POLSHIM_SERVER", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return INADDR_NONE;
    return inet_addr(v);
}

static unsigned long server_from_cmdline()
{
    const char* cl = GetCommandLineA();
    if (!cl) return INADDR_NONE;
    static const char* keys[] = { "--polserver=", "-polserver=", "/polserver=" };
    for (int k = 0; k < 3; k++) {
        size_t kl = strlen(keys[k]);
        for (const char* p = strstr(cl, keys[k]); p; p = strstr(p + kl, keys[k])) {
            // The key must sit at a TOKEN boundary: start of the command line, or
            // after whitespace/quote. Without this, any path or argument that merely
            // CONTAINS "--polserver=" would arm the redirect -- and this override arms
            // it even when [redirect] enable=0, so a stray occurrence could route
            // login/patch traffic to an attacker-chosen IP. The boundary check also
            // stops "-polserver=" from matching inside "--polserver=".
            if (p != cl && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '"') continue;
            const char* v = p + kl;
            if (*v == '"') v++;                        // tolerate a quoted value
            char buf[64]; int i = 0;
            while (*v && *v != ' ' && *v != '\t' && *v != '"' && i < 63) buf[i++] = *v++;
            buf[i] = 0;
            unsigned long a = inet_addr(buf);
            if (a != INADDR_NONE) return a;
        }
    }
    return INADDR_NONE;
}

// COMPILED-IN Square Enix address table -- the fallback that makes the
// "Connect to REAL Square Enix" settings toggle actually work.
//
// The redirect map is built from `host=a.b.c.d` rows in the [redirect] section
// (below). Those rows were only ever hand-authored in the capture install's ini
// (build-se), so ticking `enable=1` through the settings UI -- which writes only
// the `enable` key (iniheal.cpp) -- armed an EMPTY map and silently did nothing:
// the option looked broken. Baking the known SE addresses in here lets `enable=1`
// alone reach Square Enix; an ini that carries its own [redirect] rows still
// wins (this table is used ONLY when the parsed map came back empty).
//
// WARNING: These are SE's public-DNS addresses as of 2026-08-12 (see the shipped
// build-se ini they were lifted from). If SE ever renumbers, the fix is an ini
// override (`host=a.b.c.d` in [redirect]) or a rebuild -- not a silent failure.
struct SeDefault { const char* host; const char* ip; };
static const SeDefault SE_DEFAULTS[] = {
    { "pp000.pol.com", "202.67.54.51" },
    { "ci000.pol.com", "202.67.54.52" },
    { "ma000.pol.com", "202.67.54.53" }, { "po000.pol.com", "202.67.54.53" },
    { "gm000.pol.com", "202.67.54.54" }, { "gm001.pol.com", "202.67.54.54" },
    { "wh000.pol.com", "202.67.54.55" },
    { "gd000.pol.com", "202.67.54.98" }, { "gd001.pol.com", "202.67.54.98" },
    { "ucs.pol.com",   "219.117.150.8" },
    { "info.playonline.com", "202.67.54.66" }, { "info.sqex.net", "202.67.54.66" },
    { "pt002.pol.com", "124.150.156.107" }, { "pt004.pol.com", "124.150.156.107" },
    { "pt007.pol.com", "124.150.156.107" }, { "pt008.pol.com", "124.150.156.107" },
    { "pt011.pol.com", "124.150.156.107" }, { "pt012.pol.com", "124.150.156.107" },
    { "pc001w20.pol.com", "124.150.156.107" }, { "pc001w2u.pol.com", "124.150.156.107" },
    { "pc002p2u.pol.com", "124.150.156.107" }, { "pc002ps2.pol.com", "124.150.156.107" },
    { "pc002w20.pol.com", "124.150.156.107" }, { "pc002w2u.pol.com", "124.150.156.107" },
    { "pc003ps2.pol.com", "124.150.156.107" }, { "pc004w20.pol.com", "124.150.156.107" },
    { "pc004w2u.pol.com", "124.150.156.107" }, { "pc011w20.pol.com", "124.150.156.107" },
    { "pc011w2u.pol.com", "124.150.156.107" }, { "pc014w20.pol.com", "124.150.156.107" },
    { "pc014w2u.pol.com", "124.150.156.107" },
};

// MERGE, not replace: add every SE default the ini did not already supply.
// A PARTIAL ini table (even one stray `host=` row) used to block the defaults
// entirely -- the old guard only seeded when the table was completely empty --
// so a session armed the toggle, mapped that ONE host, and left the other 30 SE
// names unresolved (strict=1 -> they fail; the client reaches login at best and
// never the lobby). Returns how many were added.
static int netredir_seed_se_defaults()
{
    int added = 0;
    for (size_t i = 0; i < _countof(SE_DEFAULTS) && g_nmap < _countof(g_map); i++) {
        bool have = false;
        for (int j = 0; j < g_nmap; j++)
            if (host_eq(g_map[j].host, SE_DEFAULTS[i].host)) { have = true; break; }
        if (have) continue;                 // ini already maps this host -- keep it
        unsigned long a = inet_addr(SE_DEFAULTS[i].ip);
        if (a == INADDR_NONE) continue;
        strncpy_s(g_map[g_nmap].host, sizeof(g_map[0].host),
                  SE_DEFAULTS[i].host, _TRUNCATE);
        g_map[g_nmap].addr = a;
        g_nmap++;
        added++;
    }
    return added;
}

// [redirect] enable=1 / strict=1, then one `host=a.b.c.d` line per name.
// Keys that are not dotted-quad addresses are skipped with a log line rather
// than silently ignored.
void netredir_init(const wchar_t* ini)
{
    g_redirect = GetPrivateProfileIntW(L"redirect", L"enable", 0, ini);
    g_se_bypass = g_redirect;          // before any server= override arms it
    g_strict   = GetPrivateProfileIntW(L"redirect", L"strict", 1, ini);

    // Single-server override. Precedence: --polserver=<ip> (command line) > env
    // POLSHIM_SERVER > [redirect] server=<ip> in the ini. Any of them ARMS the
    // redirect even if enable=0, so a Steam Deck user just adds POLSHIM_SERVER to
    // the game's launch options -- no editing the ini inside the Proton prefix.
    const char* src = NULL;
    unsigned long srv = server_from_cmdline();
    if (srv != INADDR_NONE) src = "--polserver (command line)";
    if (srv == INADDR_NONE) { srv = server_from_env(); if (srv != INADDR_NONE) src = "POLSHIM_SERVER (env)"; }
    if (srv == INADDR_NONE) {
        wchar_t wv[64] = L"";
        ini_str(L"redirect", L"server", L"", wv, _countof(wv), ini);
        if (wv[0]) {
            char v[64]; WideCharToMultiByte(CP_ACP, 0, wv, -1, v, sizeof(v), NULL, NULL);
            // enable=1 means "bypass the private server, go to REAL Square Enix".
            // The ini `server=` is labelled "the private server address, WHEN NOT
            // BYPASSING" -- so when the user ticks "Connect to real SE" it must be
            // IGNORED, not obeyed. Without this it silently WON: `server=` armed
            // g_server and routed every pol.com name back to the private server,
            // so the toggle did nothing and the redirect log even said
            // `source: [redirect] server (ini)` while it happened. A command-line
            // / env server override is deliberate and still wins (handled above,
            // before this block), so a self-hoster is unaffected.
            if (g_redirect) {
                logf("[dns] [redirect] enable=1 (bypass to SE): IGNORING "
                     "[redirect] server=%s -- that is the private-server address "
                     "for when NOT bypassing. Untick 'Connect to real SE' to use "
                     "it; leaving it ON reaches Square Enix.", v);
            } else {
                srv = inet_addr(v);
                if (srv != INADDR_NONE) src = "[redirect] server (ini)";
                else logf("[dns] ini: [redirect] server=%s is not a dotted-quad, ignored", v);
            }
        }
    }
    if (srv != INADDR_NONE) { g_server = srv; g_redirect = 1; }

    if (!g_redirect) return;

    static char sect[8192];
    // The ANSI section read needs the ANSI ini PATH. Passing NULL here made Windows
    // read win.ini (not polshim.ini) -- it "worked" only because win.ini has no
    // [redirect] section, so the whole first read was a wasted lookup at the wrong
    // file (and would have picked up a stray win.ini [redirect] if one existed).
    // Convert the path up front and read the correct file once.
    char inia[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, ini, -1, inia, sizeof(inia), NULL, NULL);
    DWORD n = GetPrivateProfileSectionA("redirect", sect, sizeof(sect) - 2, inia);
    for (char* p = sect; *p && g_nmap < _countof(g_map); p += strlen(p) + 1) {
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = p;
        const char* val = eq + 1;
        if (!host_eq(key, "enable") && !host_eq(key, "strict")) {
            unsigned long a = inet_addr(val);
            if (a == INADDR_NONE) {
                logf("[dns] ini: %s=%s is not a dotted-quad address, skipped",
                     key, val);
            } else {
                strncpy_s(g_map[g_nmap].host, sizeof(g_map[0].host), key, _TRUNCATE);
                g_map[g_nmap].addr = a;
                g_nmap++;
            }
        }
        *eq = '=';
    }
    // enable=1 -> MERGE the compiled-in SE table so the "Connect to REAL Square
    // Enix" toggle reaches ALL of SE, whether the ini carried no rows or a
    // partial/stale few. Whatever SE rows the ini DID supply are kept (the merge
    // skips hosts already present). Runs whenever the toggle is on and there is
    // no `server=` override, so a private-server override still wins -- but a
    // one-host leftover in the ini can no longer under-map SE (the second-machine bug:
    // toggle armed, "1 host pointed at SE", never reached the lobby).
    if (g_redirect && !g_server) {
        int added = netredir_seed_se_defaults();
        if (added)
            logf("[dns] [redirect] enable=1 -- merged %d built-in Square Enix "
                 "default host(s) into the %d from the ini (2026-08-12 IPs; add "
                 "`host=a.b.c.d` rows to override any)", added, g_nmap - added);
    }
    // FAIL SAFE, not fail closed. enable=1 with an empty table is not a
    // redirect -- under strict=1 it is a client that cannot resolve anything,
    // and the symptom (every connection fails) points nowhere near the ini.
    // This is a real delivery hazard rather than a hypothetical: the ini merger
    // keyed on [A-Za-z0-9_]+ and silently dropped every dotted hostname, so the
    // deployed copy arrived with enable/strict and none of the addresses.
    if (g_nmap == 0 && !g_server) {
        g_redirect = 0;
        logf("[dns] redirect enable=1 but there is NO server and an EMPTY table -- "
             "refusing to arm. This install still uses the system resolver (hosts "
             "file / private server). Set `server=a.b.c.d` (or POLSHIM_SERVER / "
             "--polserver=a.b.c.d), or add `host=a.b.c.d` lines to [redirect].");
        return;
    }
    // A `server=` AND a per-host table is a CONTRADICTION, and the dangerous
    // half used to win silently. Every ini we ship carries the SE-facing table
    // (ci000=202.67.54.52, wh000=..., pp000=... -- Square Enix's REAL addresses,
    // there for the capture install), and the per-host lookup ran first. So
    // `Install-PolHookProxy.ps1 -Server <my-lan-ip>` -- the one knob a
    // self-hoster is told to use -- armed the redirect and then sent login,
    // portal and patch to SQUARE ENIX anyway. It fails in the worst possible
    // way: the client works, against the wrong server, and the log said
    // "REDIRECT ACTIVE" while it happened.
    //
    // `server=` is documented as "every SE-domain name resolves here", so when
    // it is set explicitly it WINS, and each shadowed entry is named in the log
    // rather than dropped quietly. The SE-facing install is unaffected: it sets
    // the table and NO server, which is the branch below.
    if (g_server && g_nmap > 0) {
        const unsigned char* s = (const unsigned char*)&g_server;
        logf("[dns] `server=%u.%u.%u.%u` is set AND [redirect] has %d per-host "
             "entry(ies) -- those point at REAL Square Enix in every shipped ini, "
             "so they are IGNORED. Delete them if you meant to talk to SE.",
             s[0], s[1], s[2], s[3], g_nmap);
        for (int i = 0; i < g_nmap; i++) {
            const unsigned char* b = (const unsigned char*)&g_map[i].addr;
            logf("[dns]   ignored: %s=%u.%u.%u.%u", g_map[i].host,
                 b[0], b[1], b[2], b[3]);
        }
        g_nmap = 0;
    }
    if (g_server) {
        const unsigned char* b = (const unsigned char*)&g_server;
        logf("[dns] REDIRECT ACTIVE (single-server): all pol.com/playonline.com/sqex.net"
             " -> %u.%u.%u.%u  [source: %s], strict=%d.",
             b[0], b[1], b[2], b[3], src, g_strict);
    } else {
        logf("[dns] REDIRECT ACTIVE: %d host(s) pointed at real Square Enix, strict=%d."
             " This install is NOT talking to the private server.", g_nmap, g_strict);
    }
}

// Live re-read for the in-game settings dialog. netredir_init() derives EVERY
// piece of this module's state from the ini plus the launch overrides, and the
// overrides (--polserver / POLSHIM_SERVER) cannot change while the process
// runs -- so the honest reload is the same parse run again, not a copy of it
// that would drift. The one thing init cannot do twice on its own is start
// clean: it APPENDS host rows and treats a non-zero g_server as already
// decided, so the derived state is reset here first to keep the reload
// idempotent.
//
// NOT re-done here: the gethostbyname IAT patch. inject.cpp installs the hook
// only when the redirector was armed at startup (netredir_hook_gethostbyname()
// returns NULL otherwise), so if enable was 0 then, flipping it ON here
// re-arms this module's decision logic but resolves nothing until a restart --
// the settings classifier upstream is what tells the user that. Flipping it
// OFF works live: the installed hook re-checks g_redirect on every call and
// falls straight through to the system resolver.
void netredir_reload(const wchar_t* ini)
{
    g_server = 0;
    g_nmap   = 0;
    netredir_init(ini);
    const unsigned char* b = (const unsigned char*)&g_server;
    logf("[reload] netredir: armed=%d se_bypass=%d strict=%d hosts=%d "
         "server=%u.%u.%u.%u", g_redirect, g_se_bypass, g_strict, g_nmap,
         b[0], b[1], b[2], b[3]);
}
