#include "polshim.h"
#include <stdarg.h>
#include <share.h>

FILE* g_logfile = NULL;
static CRITICAL_SECTION g_lock;
static bool g_lock_ready = false;
static LONG g_lines = 0;

// Set once, from DllMain(DLL_PROCESS_DETACH) during process termination. See
// log_begin_teardown() below for why every lock in this file goes away when it is
// set -- in one sentence: ExitProcess has already killed the threads the locks
// existed to synchronise, and a lock held by a dead thread is held forever.
static volatile LONG g_teardown = 0;

// Optional tee: when set (polctl.cpp does this), every formatted line is also
// delivered here as a NUL-terminated copy for the automation log ring. NULL in
// every build that does not link polctl, so those pay nothing.
void (*g_log_sink)(const char*) = NULL;

bool log_open(const wchar_t* path)
{
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
    // _wfopen_s opens EXCLUSIVELY (_SH_DENYRW), which makes the log unreadable
    // until the target exits -- fatal when the whole point is watching a live
    // session. _wfsopen with _SH_DENYWR keeps other writers out but lets us
    // tail/copy the file while the client is still running.
    g_logfile = _wfsopen(path, L"w", _SH_DENYWR);
    if (!g_logfile)
        return false;
    setvbuf(g_logfile, NULL, _IOFBF, 1 << 16);
    return true;
}

// --- [polshim] trace: the one level behind every old trace key --------------
// Rationale, the level table and what is deliberately NOT folded in are all in
// polshim.h next to the declarations. Lives here because log.cpp is the one
// file every build variant links, so the small harnesses get it too.
static int g_trace_level = 0;

void polshim_trace_configure(const wchar_t* ini)
{
    g_trace_level = ini ? GetPrivateProfileIntW(L"polshim", L"trace", 0, ini) : 0;
    if (g_trace_level < 0) g_trace_level = 0;
}

int polshim_trace_level() { return g_trace_level; }

int trace_at(int level) { return g_trace_level >= level ? 1 : 0; }

// THE GHOST-PROCESS FIX (2026-08-19). pol.exe would intermittently finish its
// shutdown -- windows destroyed, nothing left in the taskbar -- and then sit in
// Task Manager forever at ~22 MB. Measured across a day of sessions: the mask
// guard was exonerated (its WM_CLOSE was LET THROUGH and the mask was DESTROYED
// in the ghosts too), and every ghost's log stopped PARTWAY THROUGH the
// DLL_PROCESS_DETACH summary block -- one stopped mid-[d3d] with [mask]/[dx]
// already written.
//
// The sequence: ExitProcess terminates all other threads first, THEN runs
// DllMain(DETACH) on the caller. The shim keeps worker threads that log --
// polctl, logship, autoupdate ([polctl]/[autoupdate]/[logship] enable=1 in the
// shipped ini) -- and killing one between EnterCriticalSection and
// LeaveCriticalSection orphans g_lock. Windows NEVER releases a critical section
// whose owner died, so the first logf() in the summary block blocks forever, the
// main thread wedges inside DllMain under the loader lock, and the process can
// only be killed from Task Manager. The same applies to the CRT's per-FILE lock
// taken inside vfprintf/fflush, so the _nolock forms are used below too.
//
// It is intermittent because it needs a worker to be inside logf() at the instant
// ExitProcess terminates it -- which is why this outlived several other fixes.
void log_begin_teardown()
{
    InterlockedExchange(&g_teardown, 1);
}

int log_teardown_active() { return g_teardown ? 1 : 0; }

// Lock-free raw write for code that has its own formatting (proxy_summary). Safe to
// call at any time: outside teardown it just goes through the normal locked path.
void log_write_raw(const char* text)
{
    if (!g_logfile || !text) return;
    if (g_teardown) {
        _fwrite_nolock(text, 1, strlen(text), g_logfile);
        _fflush_nolock(g_logfile);
        return;
    }
    if (g_lock_ready) EnterCriticalSection(&g_lock);
    fputs(text, g_logfile);
    fflush(g_logfile);
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

void log_flush()
{
    if (!g_logfile) return;
    if (g_teardown) { _fflush_nolock(g_logfile); return; }
    if (g_lock_ready) EnterCriticalSection(&g_lock);
    fflush(g_logfile);
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

void log_close()
{
    if (g_logfile) {
        // Ghost fix (2026-08-21): proxy_summary iterates the whole proxy table and
        // formats each entry -- it can ALLOCATE, and on the process-exit path an
        // ExitProcess-killed worker (logship live, autoupdate) may have orphaned the
        // process HEAP lock, so this wedges even though logf itself is lock-free.
        // That was the hole the DllMain summary-skip left, because log_close still ran
        // this. The banner is a diagnostic total; skip it on teardown, like every other
        // summary, so the exit path allocates NOTHING. A clean FreeLibrary still writes it.
        if (!g_teardown) proxy_summary(g_logfile);
        if (g_teardown) {
            // _nolock: the CRT stream lock can be orphaned exactly like g_lock, and
            // fclose() on an orphaned one hangs just as permanently. Flush, then let
            // process teardown reclaim the handle rather than risk fclose().
            _fflush_nolock(g_logfile);
        } else {
            fflush(g_logfile);
            fclose(g_logfile);
        }
        g_logfile = NULL;
    }
    // DeleteCriticalSection on a section a dead thread still "owns" is undefined;
    // during teardown the memory is about to be reclaimed wholesale anyway.
    if (g_lock_ready && !g_teardown) { DeleteCriticalSection(&g_lock); g_lock_ready = false; }
}

// Integer-only formatting path. This runs between a COM method's return and
// the caller receiving it, so it must not touch x87/SSE state -- a float
// return value is still sitting in ST0 at that point. Keep it that way.
void logf(const char* fmt, ...)
{
    if (!g_logfile) return;
    // Teardown: no lock, and no sink either -- polctl's ring belongs to a thread
    // ExitProcess has already terminated, so feeding it is at best pointless.
    if (g_teardown) {
        // Format first, then ONE unlocked write. The UCRT has no _vfprintf_nolock,
        // and plain vfprintf would take the very per-FILE lock this path exists to
        // avoid -- that lock can be orphaned by a worker ExitProcess killed inside
        // logf(), and taking it is the hang itself.
        char tbuf[8192];
        va_list apt;
        va_start(apt, fmt);
        int n = _vsnprintf_s(tbuf, sizeof(tbuf) - 2, _TRUNCATE, fmt, apt);
        va_end(apt);
        if (n < 0) n = (int)strlen(tbuf);   // _TRUNCATE returns -1; the buffer is still valid
        tbuf[n] = (char)10;                 // newline, spelled numerically on purpose
        _fwrite_nolock(tbuf, 1, (size_t)n + 1, g_logfile);
        _fflush_nolock(g_logfile);   // unbuffered from here: a later hang must not eat the tail
        return;
    }
    if (g_lock_ready) EnterCriticalSection(&g_lock);
    va_list ap;
    va_start(ap, fmt);
    // When a sink is attached, format a truncated copy for it via a va_copy so
    // the file still gets the FULL line through vfprintf (capture hex-dumps run
    // to kilobytes and must not be clipped). Integer/string formats only -- no
    // x87 use, so a float return value sitting in ST0 is undisturbed.
    if (g_log_sink) {
        va_list ap2;
        va_copy(ap2, ap);
        char buf[240];
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap2);
        va_end(ap2);
        g_log_sink(buf);
    }
    vfprintf(g_logfile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logfile);
    if ((++g_lines & 0x7F) == 0) fflush(g_logfile);   // flush ~every 128 lines so live peeks work
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

struct KnownIID { const GUID g; const char* name; };

static const KnownIID k_known[] = {
    { { 0x00000000,0x0000,0x0000,{0xC0,0,0,0,0,0,0,0x46} }, "IUnknown"       },
    { { 0x00000001,0x0000,0x0000,{0xC0,0,0,0,0,0,0,0x46} }, "IClassFactory"  },
    { { 0x00000003,0x0000,0x0000,{0xC0,0,0,0,0,0,0,0x46} }, "IMarshal"       },
    { { 0x00000018,0x0000,0x0000,{0xC0,0,0,0,0,0,0,0x46} }, "IRunnableObject"},
    { { 0x00000019,0x0000,0x0000,{0xC0,0,0,0,0,0,0,0x46} }, "IROTData"       },
    { { 0x0000010e,0x0000,0x0000,{0xC0,0,0,0,0,0,0,0x46} }, "IDataObject"    },
    { { 0x00020400,0x0000,0x0000,{0xC0,0,0,0,0,0,0,0x46} }, "IDispatch"      },
    { { 0xB196B283,0xBAB4,0x101A,{0xB6,0x9C,0x00,0xAA,0x00,0x34,0x1D,0x07} }, "IProvideClassInfo" },
};

// Unknown IIDs get printed raw -- those are the interesting ones, since a
// private POL interface is exactly what we're here to discover.
const char* iid_name(const GUID& g, char* buf, size_t cb)
{
    for (int i = 0; i < _countof(k_known); i++)
        if (memcmp(&k_known[i].g, &g, sizeof(GUID)) == 0)
            return k_known[i].name;

    _snprintf_s(buf, cb, _TRUNCATE,
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
        g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}
