// sessionwatch.cpp -- notice when the SESSION socket dies.
//
// WHAT DIES, AND WHY IT COSTS A PASSWORD
//
// After the login accept the Viewer keeps its auth-band connection (51241-51250)
// open for the whole session: chat, presence and a launched title's world
// traffic all ride it. When that socket dies the client reports POL-0008 and
// returns to the login screen -- and an SE account has no saved credential to
// autofill, so the user retypes a password for something that was not their
// fault.
//
// The SERVER-side fix landed first and is the better one: services/authrelay.py
// terminates the client's TCP connection in a process that never restarts, so a
// server deploy no longer breaks the socket at all. This is the fallback for
// everything that fix cannot reach -- the relay itself being down, a host
// reboot, a Wi-Fi drop on the Deck, a session orphaned past the relay's
// deadline. Those all still end as a dead socket in this process.
//
// WHAT THIS DOES
//
// This module only OBSERVES and logs: it never modifies a buffer, a return value
// or a Winsock error. The log line always says what it saw.
//
// WHAT IT CANNOT SEE: an OVERLAPPED WSARecv completion. The result lands through
// the completion port, not through our return path, so a client using overlapped
// receives on the session socket would drop silently as far as this is
// concerned. Measured on the Viewer to date: the auth band uses plain
// blocking recv, and the overlapped counter in inject.cpp stays at zero for it.
// If that ever changes, the honest log tell is a session that ends with no
// [session] line at all.
#include "polshim.h"

// From winsock2.h, which polshim.h does not pull in (and inject.cpp deliberately
// keeps out of its own translation unit -- it types sockets as UINT_PTR).
#define POL_WSAECONNRESET    10054
#define POL_WSAECONNABORTED  10053
#define POL_WSAENETRESET     10052
#define POL_WSAETIMEDOUT     10060
#define POL_WSAESHUTDOWN     10058

#define SW_MAX_TRACKED 32

static int      g_enable      = 1;      // observe by default
static unsigned g_band_lo     = 51241;
static unsigned g_band_hi     = 51250;

// The tracked set is tiny and fixed: one session socket, plus one per launched
// title. A ring beats a map here -- no allocation on a Winsock path, and a
// forgotten entry costs an unnoticed drop rather than a wrong action.
struct Tracked { UINT_PTR s; unsigned port; };
static Tracked g_tracked[SW_MAX_TRACKED];
static int     g_next = 0;
static CRITICAL_SECTION g_cs;
static bool    g_cs_ready = false;

static void sw_lock()   { if (g_cs_ready) EnterCriticalSection(&g_cs); }
static void sw_unlock() { if (g_cs_ready) LeaveCriticalSection(&g_cs); }

// SOCKET VALUES ARE RECYCLED. A handle we tagged as a session socket comes back
// as some other connection later, and a stale tag would report that one's close
// as a session drop. So every connect() re-decides: in-band re-tags, out-of-band
// UNtags. That is the whole invalidation story, and it is enough because a
// socket cannot be reused without being connected again.
void sessionwatch_note_connect(UINT_PTR s, unsigned port)
{
    if (!g_enable) return;
    bool in_band = (port >= g_band_lo && port <= g_band_hi);
    sw_lock();
    for (int i = 0; i < SW_MAX_TRACKED; i++) {
        if (g_tracked[i].s == s) {
            if (in_band) { g_tracked[i].port = port; sw_unlock(); return; }
            g_tracked[i].s = 0;                      // reused for something else
        }
    }
    if (in_band) {
        g_tracked[g_next].s = s;
        g_tracked[g_next].port = port;
        g_next = (g_next + 1) % SW_MAX_TRACKED;
        logf("[session] tracking the session socket %p (:%u)", (void*)s, port);
    }
    sw_unlock();
}

void sessionwatch_note_recv(UINT_PTR s, int n, DWORD werr)
{
    if (!g_enable || n > 0) return;
    // n == 0 is a clean FIN, n < 0 with one of these is the abrupt version. A
    // WSAEWOULDBLOCK or a timeout is NOT a drop -- the socket is still there,
    // and treating a quiet moment as a drop is exactly the false positive that
    // would make this feature worse than nothing.
    bool dead = (n == 0) || (werr == POL_WSAECONNRESET) ||
                (werr == POL_WSAECONNABORTED) || (werr == POL_WSAENETRESET) ||
                (werr == POL_WSAESHUTDOWN) || (werr == POL_WSAETIMEDOUT);
    if (!dead) return;
    unsigned port = 0;
    sw_lock();
    for (int i = 0; i < SW_MAX_TRACKED; i++) {
        if (g_tracked[i].s == s && g_tracked[i].s) { port = g_tracked[i].port; g_tracked[i].s = 0; break; }
    }
    sw_unlock();
    if (!port) return;                              // not the session socket
    logf("[session] SESSION SOCKET LOST on :%u (%s) -- this is what the client "
         "reports as POL-0008", port,
         n == 0 ? "clean close by the peer" : "reset");
    log_flush();
}

// ---------------------------------------------------------------- wake recovery
//
// THE OTHER HALF OF A RESUME, AND THE HALF NOBODY SUPPLIED. wakerecover.cpp
// rebuilds the Direct3D device after a suspend; NOTHING did anything about the
// network, and the sockets do not survive a suspend either. What that costs was
// measured live 2026-09-06: a PC Viewer idle ~45 minutes answered its last
// keepalive at 21:28:26 and then went silent -- the server logged no close at
// all -- and on resume the client tried to reconnect on top of a session it
// still believed in and raised **POL-0006**, which is 「TCPのポートが重複してい
// ます」, *the TCP port is duplicated*. A bind collision, in the client's own
// stack. The server was provably clean at that moment: one socket on the auth
// ring, no stale binds, and a clean login the instant the Viewer was restarted.
// The user had to fully exit to recover. See
// [[pol0006-after-idle-is-client-stale-state]].
//
// SHUTDOWN, NOT CLOSESOCKET, AND THE DIFFERENCE MATTERS.
//
// `closesocket` would free the local port outright -- which is precisely what a
// duplicate-bind wants -- but it invalidates a handle the client still owns, and
// Winsock recycles handle values. A client that later touches that number is
// then operating on whatever connection inherited it, which is a far worse bug
// than the one being fixed and would appear as cross-talk between bands.
// `shutdown(SD_BOTH)` keeps the handle valid and does the one thing needed: it
// turns a socket that is SILENTLY dead into one that is DEMONSTRABLY dead, so
// the client's very next recv returns 0 and its own teardown path runs -- the
// path it already has, the one that closes the socket properly and releases the
// port. We convert an invisible failure into the visible one the client knows
// how to handle, and let it do its own cleanup.
//
// That also means this feeds the existing machinery for free: the recv of 0
// arrives at `sessionwatch_note_recv` as a clean close and is logged like any
// other drop.
//
// WHY IT IS ON BY DEFAULT. The sockets are already dead when this runs -- the
// suspend killed them, and the only question is when the client finds out.
// Shutting down a dead socket loses nothing; leaving it costs a POL-0006 and a
// full exit. A wake is also a rare, explicit event rather than a hot path.
// `[session] wake_netdrop=0` turns it off.
static int g_wake_netdrop = 1;   // [session] wake_netdrop

typedef int (WINAPI *PFN_SHUTDOWN)(UINT_PTR, int);
#define POL_SD_BOTH 2

void sessionwatch_note_wake(unsigned long gap_ms)
{
    if (!g_enable || !g_wake_netdrop) return;

    // Snapshot under the lock, act outside it: shutdown() can block briefly and
    // the connect() hook must never wait on a resume sweep.
    UINT_PTR socks[SW_MAX_TRACKED];
    unsigned ports[SW_MAX_TRACKED];
    int n = 0;
    sw_lock();
    for (int i = 0; i < SW_MAX_TRACKED; i++) {
        if (g_tracked[i].s) {
            socks[n] = g_tracked[i].s;
            ports[n] = g_tracked[i].port;
            n++;
        }
    }
    sw_unlock();

    if (!n) {
        logf("[session] resume after %lu.%lus: no auth-band socket tracked -- "
             "nothing to drop (not logged in, or it was closed already)",
             gap_ms / 1000, (gap_ms % 1000) / 100);
        return;
    }

    // Resolved here rather than at startup: ws2_32 is certainly loaded by the
    // time a session socket exists, and this runs once per resume.
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    PFN_SHUTDOWN sd = ws2 ? (PFN_SHUTDOWN)GetProcAddress(ws2, "shutdown") : NULL;
    if (!sd) {
        logf("[session] resume: ws2_32!shutdown not resolvable -- leaving the "
             "%d tracked socket(s) alone", n);
        return;
    }

    logf("[session] *** RESUME: about %lu.%lus unaccounted for. The auth band "
         "does not survive a suspend, and a session the client still believes "
         "in is what raises POL-0006 on the next dial. Shutting down %d tracked "
         "socket(s) so the client sees the drop and reconnects.",
         gap_ms / 1000, (gap_ms % 1000) / 100, n);
    for (int i = 0; i < n; i++) {
        int r = sd(socks[i], POL_SD_BOTH);
        logf("[session]   shutdown sock=%p (:%u) -> %d", (void*)socks[i],
             ports[i], r);
    }
    log_flush();
}

void sessionwatch_configure(const wchar_t* ini)
{
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    g_enable   = GetPrivateProfileIntW(L"session", L"watch", 1, ini);
    g_band_lo  = (unsigned)GetPrivateProfileIntW(L"session", L"band_lo", 51241, ini);
    g_band_hi  = (unsigned)GetPrivateProfileIntW(L"session", L"band_hi", 51250, ini);
    g_wake_netdrop = GetPrivateProfileIntW(L"session", L"wake_netdrop", 1, ini);
    if (!g_enable) return;
    logf("[session] watching the auth band %u-%u; a drop is logged",
         g_band_lo, g_band_hi);
}

// Live re-read for the in-game settings dialog: the watch flag is consulted per
// socket event, so a new value applies to the very next drop -- nothing to
// install and nothing to wake up.
//
// Deliberately NOT re-read: band_lo/band_hi -- the tracked set was tagged
// against the startup band, and a socket only re-decides its tag at its next
// connect(); moving the edges mid-session would leave the set split across two
// definitions.
void sessionwatch_reload(const wchar_t* ini)
{
    g_enable = GetPrivateProfileIntW(L"session", L"watch", 1, ini);
    g_wake_netdrop = GetPrivateProfileIntW(L"session", L"wake_netdrop", 1, ini);
    logf("[reload] sessionwatch: watch=%d wake_netdrop=%d (the band is a startup "
         "value)", g_enable, g_wake_netdrop);
}
