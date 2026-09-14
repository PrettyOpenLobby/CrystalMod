// polfetch.cpp -- break Front Mission Online's startup LIVELOCK, and name the
// fetch that causes it.
//
// THE BUG (measured 2026-08-14)
//
// FrontMissionOnline.dll+0x1B91E0 busy-waits on an asynchronous polcore fetch
// with no Sleep, no yield and no message pump:
//
//     call [ecx+0x11bc]     ; polcore START  -> handle in esi
//     test esi, esi
//     jl   +0x1B922D        ; start FAILED   -> give up      (handled!)
//   L:
//     push esi
//     call [edx+0x11b8]     ; polcore POLL   -> status
//     test eax, eax
//     je   L                ; status 0 = "in progress" -> SPIN, immediately
//     jle  +0x1B922D        ; status < 0     -> give up      (handled!)
//
// Against Square Enix's servers that fetch completed in milliseconds. Against
// ours it never completes at all -- it never even opens a socket -- so the slot's
// status stays 0 and FMO spins a full core forever (measured: 4937 ms of CPU in
// 5000 ms). Every visible symptom follows from that one loop: the window stops
// pumping so Windows paints a blank GHOST over it, the intro music keeps playing
// because DirectShow's threads are independent, not one frame is ever presented,
// and the FMV cannot be skipped because FMO reads the keyboard through a
// system-wide WH_KEYBOARD_LL hook -- which is serviced on the spinning thread.
//
// THE FIX, AND WHY IT IS THIS ONE
//
// FMO ALREADY HANDLES a negative poll result: `jle -> give up` is its own code.
// So we do not need to patch FMO, bypass its FMV, or fake a success it would
// then act on. We only have to stop lying to it that the fetch is still running.
// After `timeout_ms` with status 0 this returns POLFETCH_TIMEOUT (a negative
// polcore-style error) and FMO takes the give-up branch it was always going to
// take had the fetch failed outright.
//
// This is deliberately a TIMEOUT and not a "return failure immediately": a fetch
// that legitimately takes a moment must still be allowed to succeed, and the
// Viewer's own code uses these same two slots.
//
// HOW IT ATTACHES
//
// Not a code patch. polcore publishes a flat dispatch table (the "common function
// table") at polcore+0x6FBE8, and both app.dll and FMO call through it -- FMO
// caches only the TABLE POINTER and re-reads the slot every iteration, so
// swapping two table entries is enough. Slot 1134 (byte offset 0x11b8) is the
// poll, slot 1135 (0x11bc) the start. Both are VERIFIED to hold the addresses we
// expect before anything is written; a polcore build that does not match is left
// completely alone.
//
// IT ALSO ANSWERS THE NEXT QUESTION
//
// The timeout makes FMO usable but does not make the fetch work. So the start
// hook logs its arguments, and a timeout dumps the polcore request record --
// which is where the target host lands. polcore+0x497D0 -> +0x49780 reads config
// id 0x21 into a 0x200 buffer and defaults the port to 0xC800 (51200, the band
// base) before calling the allocator, so the record is the place to read what it
// actually tried to reach.
#include "polshim.h"
#include <intrin.h>          // _ReturnAddress -- identifies the calling title

// --- polcore RVAs -----------------------------------------------------------
// Read off the unpacked image (a polcore.dll memory image, load base 0x037C0000)
// and cross-checked against the client's CFT table. Every one is VERIFIED at
// runtime before use; none is trusted.
#define PF_CFT_RVA        0x6FBE8      // the common function table itself
#define PF_SLOT_POLL      0x11B8       // byte offset in the table (index 1134)
#define PF_SLOT_START     0x11BC       // byte offset in the table (index 1135)
#define PF_FN_POLL_RVA    0x497F0      // what slot 1134 must currently hold
#define PF_FN_START_RVA   0x497D0      // what slot 1135 must currently hold
#define PF_RECORDS_RVA    0xBE0F8      // request table: <=4 records, 552 bytes each
#define PF_RECORD_STRIDE  552
#define PF_RECORD_MAX     4

// polcore's own error space: the getter returns 0xFFFFD800 for a bad index and
// 0xFFFFD7FE when the subsystem is not initialised. Stay inside it so FMO -- and
// anything else reading this -- sees a value shaped like a real polcore failure.
#define POLFETCH_TIMEOUT  ((int)0xFFFFD801)

typedef int (__cdecl *PFN_pf_poll)(int handle);
typedef int (__cdecl *PFN_pf_start)(int a0, int* pport);

static int   g_enable     = 1;
static int   g_timeout_ms = 20000;
static int   g_trace      = 1;
static int   g_dump       = 1;
static int   g_fmo_only   = 1;   // only time out fetches started by the title

static PFN_pf_poll  orig_poll  = NULL;
static PFN_pf_start orig_start = NULL;
static BYTE*        g_polcore  = NULL;
static bool         g_hooked   = false;

// Per-slot state. The table is 4 records; 8 is slack in case a build differs.
static DWORD g_started_ms[8];
static LONG  g_polls[8];
static LONG  g_timed_out[8];

static LONG  g_n_start = 0, g_n_timeout = 0;

void polfetch_configure(const wchar_t* ini)
{
    if (!ini) return;
    g_enable     = GetPrivateProfileIntW(L"polfetch", L"enable",     1, ini);
    g_timeout_ms = GetPrivateProfileIntW(L"polfetch", L"timeout_ms", 20000, ini);
    g_trace      = GetPrivateProfileIntW(L"polfetch", L"trace",      trace_at(1), ini);
    g_dump       = GetPrivateProfileIntW(L"polfetch", L"dump_record", 1, ini);
    g_fmo_only   = GetPrivateProfileIntW(L"polfetch", L"fmo_only",   1, ini);
}

// Live re-read for the in-game settings dialog: timeout_ms is consulted on
// every poll, so a new value applies at once to a fetch already in flight. The
// CFT slot swap itself happens at polcore load (polfetch_apply_module) and is
// gated on enable there -- so enable=1 here cannot install it after the fact,
// and enable=0 does not unhook: the installed hooks stay and simply keep using
// the new timeout. trace/dump_record/fmo_only are left as startup values.
void polfetch_reload(const wchar_t* ini)
{
    if (!ini) return;
    g_enable     = GetPrivateProfileIntW(L"polfetch", L"enable",     1, ini);
    g_timeout_ms = GetPrivateProfileIntW(L"polfetch", L"timeout_ms", 20000, ini);
    logf("[reload] polfetch: enable=%d timeout_ms=%d (hooked=%d; the CFT swap "
         "is load-time only)", g_enable, g_timeout_ms, g_hooked ? 1 : 0);
}

int polfetch_enabled() { return g_enable; }

// Dump a request record. This is the payload for the SECOND half of the job:
// the record is where the host/port the fetch aimed at ends up, so a timeout
// leaves behind the information needed to make the fetch actually work.
static void dump_record(int h)
{
    if (!g_dump || !g_polcore || h < 0 || h >= PF_RECORD_MAX) return;
    BYTE* rec = g_polcore + PF_RECORDS_RVA + (size_t)h * PF_RECORD_STRIDE;
    if (IsBadReadPtr(rec, PF_RECORD_STRIDE)) {
        logf("[pf]   record %d at %p unreadable", h, rec);
        return;
    }
    DWORD* w = (DWORD*)rec;
    logf("[pf]   record %d @%p: status[+0x10]=%08lX out[+0x14]=%08lX "
         "out[+0x18]=%08lX out[+0x1c]=%08lX flag[+0x226]=%02X",
         h, rec, w[4], w[5], w[6], w[7], rec[0x226]);
    // Hex + ASCII of the head: the target host is a plain string in here, and
    // reading it is the whole point of dumping.
    for (int off = 0; off < 0x120; off += 16) {
        char line[128];
        int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "[pf]   %04X  ", off);
        for (int i = 0; i < 16; i++)
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%02x ", rec[off + i]);
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " ");
        for (int i = 0; i < 16; i++) {
            BYTE c = rec[off + i];
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%c",
                             (c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        logf("%s", line);
    }
}

// Is the caller the title rather than the Viewer's own code? Only consulted on
// the timeout path, never in the spin loop -- VirtualQuery per poll would be its
// own performance bug.
static bool caller_is_title(void* retaddr)
{
    if (!g_fmo_only) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(retaddr, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return false;
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) return false;
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;
    return _stricmp(leaf, "FrontMissionOnline.dll") == 0;
}

static int __cdecl hook_start(int a0, int* pport)
{
    int h = orig_start(a0, pport);
    InterlockedIncrement(&g_n_start);
    int port = -1;
    if (pport && !IsBadReadPtr(pport, sizeof(int))) port = *pport;
    logf("[pf] START(a0=%d, pport=%p%s) -> handle %d", a0, pport,
         port >= 0 ? "" : " (null/unreadable)", h);
    if (port >= 0) logf("[pf]   requested port %d", port);
    if (h >= 0 && h < 8) {
        g_started_ms[h] = GetTickCount();
        g_polls[h] = 0;
        g_timed_out[h] = 0;
    }
    return h;
}

static int __cdecl hook_poll(int h)
{
    int r = orig_poll(h);
    if (r != 0 || h < 0 || h >= 8) return r;      // finished/failed: nothing to do

    // HOT PATH. FMO calls this millions of times a second, so do as close to
    // nothing as possible: one counter, and a clock read every 256th call.
    LONG n = InterlockedIncrement(&g_polls[h]);
    if ((n & 0xFF) != 0) return r;

    if (g_timed_out[h]) return POLFETCH_TIMEOUT;  // already decided; stay decided

    DWORD started = g_started_ms[h];
    if (!started) return r;                        // we never saw its START
    if ((DWORD)(GetTickCount() - started) < (DWORD)g_timeout_ms) return r;

    if (!caller_is_title(_ReturnAddress())) return r;

    InterlockedExchange(&g_timed_out[h], 1);
    InterlockedIncrement(&g_n_timeout);
    logf("[pf] *** TIMEOUT on handle %d after %d ms and %ld polls -- the fetch "
         "never completed. Returning 0x%08X so the caller takes its own "
         "give-up branch instead of spinning forever.",
         h, g_timeout_ms, n, (unsigned)POLFETCH_TIMEOUT);
    logf("[pf]   (this UNWEDGES the client; it does NOT make the fetch work -- "
         "the record below says what it was trying to reach)");
    dump_record(h);
    return POLFETCH_TIMEOUT;
}

static bool patch_slot(DWORD* slot, void* hook, void** out_orig, const char* what)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old)) {
        logf("[pf] VirtualProtect failed on the %s slot -- not hooked", what);
        return false;
    }
    if (out_orig) *out_orig = (void*)(*slot);
    *slot = (DWORD)(ULONG_PTR)hook;
    VirtualProtect(slot, sizeof(DWORD), old, &old);
    return true;
}

void polfetch_apply_module(void* module_base, const char* module)
{
    if (!g_enable || g_hooked || !module_base || !module) return;
    if (_stricmp(module, "polcore.dll") != 0) return;

    BYTE* base = (BYTE*)module_base;
    DWORD* table = (DWORD*)(base + PF_CFT_RVA);
    if (IsBadReadPtr(table, PF_SLOT_START + sizeof(DWORD))) {
        logf("[pf] polcore+0x%X is not readable -- common function table not found, "
             "nothing hooked", PF_CFT_RVA);
        return;
    }

    DWORD* slot_poll  = (DWORD*)((BYTE*)table + PF_SLOT_POLL);
    DWORD* slot_start = (DWORD*)((BYTE*)table + PF_SLOT_START);
    DWORD want_poll  = (DWORD)(ULONG_PTR)(base + PF_FN_POLL_RVA);
    DWORD want_start = (DWORD)(ULONG_PTR)(base + PF_FN_START_RVA);

    // Verify before writing, exactly like patches.cpp: a polcore build whose
    // table does not hold what we expect gets left alone rather than corrupted.
    if (*slot_poll != want_poll || *slot_start != want_start) {
        logf("[pf] common function table does NOT hold what this build expects -- "
             "slot 1134 = %08lX (expected %08lX), slot 1135 = %08lX (expected %08lX). "
             "Nothing hooked; the FMO livelock fix is INACTIVE.",
             *slot_poll, want_poll, *slot_start, want_start);
        // Two very different causes, and the fix differs, so name both rather
        // than leaving "it didn't attach" to be re-diagnosed from scratch.
        // tabletrace swaps app.dll's POINTER to a thunk array rather than
        // rewriting polcore's own table, so it should NOT collide -- but if the
        // addresses above look like thunks rather than polcore code, it did.
        logf("[pf]   either this is a different polcore build (compare the RVAs "
             "against the CFT table), or something else already "
             "swapped these slots -- check [polshim] tabletrace.");
        return;
    }

    g_polcore = base;
    ZeroMemory(g_started_ms, sizeof(g_started_ms));
    bool a = patch_slot(slot_poll,  (void*)hook_poll,  (void**)&orig_poll,  "poll");
    bool b = patch_slot(slot_start, (void*)hook_start, (void**)&orig_start, "start");
    if (!a || !b) return;
    g_hooked = true;
    logf("[pf] hooked polcore common-function-table slots 1134 (poll, +0x%X) and "
         "1135 (start, +0x%X); timeout=%d ms fmo_only=%d -- FMO's startup fetch "
         "can no longer spin forever",
         PF_SLOT_POLL, PF_SLOT_START, g_timeout_ms, g_fmo_only);
}

void polfetch_summary()
{
    if (!g_enable) return;
    logf("[pf] summary: hooked=%d starts=%ld timeouts=%ld",
         g_hooked ? 1 : 0, g_n_start, g_n_timeout);
    for (int i = 0; i < PF_RECORD_MAX; i++)
        if (g_polls[i])
            logf("[pf]   handle %d: %ld polls%s", i, g_polls[i],
                 g_timed_out[i] ? "  (TIMED OUT)" : "");
    if (g_hooked && !g_n_start)
        logf("[pf] NOTE: the hook is installed but START never ran -- no title used "
             "this fetch this session.");
}
