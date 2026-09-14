// comtrace.cpp -- log every COM activation the Viewer performs, and DIAGNOSE
// the failures in place.
//
// Why this exists, and why proxy.cpp/inject.cpp could not already do it:
//
//   Launching Front Mission Online raises "Class not registered"
//   (REGDB_E_CLASSNOTREG, 0x80040154). Its registered ContentsCLSID
//   {94603A98-...} is fine -- it lives in the 32-bit view with a valid
//   InprocServer32, and CoCreateInstance on it from a 32-bit process returns a
//   live object. The inference drawn from that was "so the Viewer is asking for
//   some OTHER class" -- WRONG, and this hook is what disproved it: the Viewer
//   asks for exactly {94603A98-...}, and the answer turned out to be the CLSCTX
//   argument (see ctx_str below). Naming the call was still the necessary step;
//   nothing we had could do it:
//
//     * inject.cpp's EAT patch of DllGetClassObject only fires once a module
//       has been FOUND. A class-not-registered failure dies at the registry,
//       before any module is located, so that hook is structurally blind to it.
//     * proxy.cpp only ever sees interfaces that were successfully created.
//
//   The failing call is therefore only observable at the ole32 entry points.
//
// The diagnosis half is the point. A bare "hr=0x80040154" would just restate
// what the dialog said; on failure we go read the registry for that CLSID --
// both views -- and report whether the key exists, what InprocServer32 names,
// and whether that file is actually on disk. One log line then distinguishes
// the cases that otherwise need a ProcMon capture to tell apart:
//
//     not registered in either view          -> nothing claims the class
//     registered 64-bit only                 -> a 32-bit host cannot see it
//     registered, InprocServer32 file missing -> stale registration
//     registered, file present               -> the DLL refused the CLSID
//                                               (its DllGetClassObject said no)
//
// Delivered through inject.cpp's by-value IAT patch, the same mechanism as the
// ws2_32 and DirectX hooks. That covers static imports of ole32 in every module
// we sweep. It does NOT cover a dynamic GetProcAddress("CoCreateInstance") --
// if a run shows installed=0 hooks or zero calls while the client is plainly
// doing COM, suspect that and say so rather than concluding "no COM happened".
// polshim.h notes the same trap for the app.dll<->polcore flat table.
//
// COVERAGE IS NOT UNIFORM, and the hook reports on itself for that reason -- see
// the counter block in com_summary(). Measured: dxtest.exe gets 19 hooked
// CoCreateInstance entries (DirectSound's internal activations, via dsound.dll's
// IAT), while comtest.exe's own direct call bypasses a slot that provably holds
// our address. Read the `hook entries:` line before concluding anything from
// silence.
#include "polshim.h"
// polshim.h defines WIN32_LEAN_AND_MEAN before <windows.h>, which excludes the
// OLE headers -- so IUnknown, COSERVERINFO, MULTI_QI, LPCOLESTR and LPCLSID are
// all absent by default. Pull them in explicitly rather than dropping the
// LEAN_AND_MEAN define, which the whole project relies on.
#include <objbase.h>
#include <intrin.h>          // _ReturnAddress -- names the module that asked for a class

static int g_comtrace = 1;      // default ON: activations are rare, not a hot path
static int g_iid_alias = 1;     // see the ContentsIID alias block below
static int g_block_ingame_fl = 0;  // [polshim] block_ingame_friendlist
static int g_fl_slot7_stub = 0;    // [polshim] fl_slot7_stub: return S_OK w/o the real init
static int g_fl_slot7_async = 0;   // [polshim] fl_slot7_async: run real init on a worker thread
static int g_fl_pump = 0;          // [polshim] fl_slot7_pump: pump messages during the FL's
                                   // WaitForSingleObject on the STA thread (breaks the STA deadlock)
static volatile LONG g_fl_sta_tid = 0;   // the thread FE calls slot 7 on (its STA/main thread)
static int g_fl_grant_ms = 0;      // [polshim] fl_wfso_grant_ms: if the STA WaitForSingleObject in
                                   // slot 7 is still stuck after this many ms, force-return WAIT_OBJECT_0
                                   // to break the SQ_ENVVAR_LOCK mutex deadlock (0 = never, just pump)

// FriendListCom Class, content id 0014 (FriendList.dll). Fantasy Earth
// instantiates this apartment-threaded (STA) object on ITS OWN main thread during
// startup, which then loads FriendList.dll and creates a 1280x800 WinFriendList
// window -- after which FE's main thread WEDGES (0 Present, all threads waiting) so
// the game never renders (black screen on the Deck). This lets us fail that one
// activation to test whether the in-game Friend List is the block. See regfix.cpp.
static const GUID kFriendListCom =
    { 0x7E2702CC, 0x2338, 0x4087, { 0x83, 0x55, 0xFA, 0x98, 0x82, 0xB5, 0xBD, 0xA8 } };

// Slot-7 entry/exit probe. FE calls vtable[7] (FriendList.dll+0x8D80) right after
// creating the FL and blocks; this wrapper logs ENTER and EXIT so we know whether
// that method returns (FE blocks LATER) or never returns (it IS the wedge, in
// FriendList.dll sub_1005f0d0's init). __stdcall: FE pushes (this, arg).
typedef HRESULT (__stdcall *PFN_FLSlot7)(void* self, void* arg);
static PFN_FLSlot7 g_fl_slot7_real = NULL;
static volatile LONG g_fl_slot7_inside = 0;   // 1 while stuck in the real method

// Watchdog: if slot 7 is still running 8s after entry, dump every thread's stack so
// the frame thread 332 is wedged in (the exact blocking call inside sub_1005f0d0)
// is named. Fires once; the wedge is permanent so re-dumps add nothing.
static DWORD WINAPI fl_slot7_watchdog(void*)
{
    Sleep(8000);
    if (InterlockedCompareExchange(&g_fl_slot7_inside, 0, 0) != 0) {
        logf("[fl] slot7 STILL INSIDE after 8s -- dumping all threads to find the wedge");
        log_flush();
        crashlog_dump_all_threads("fl-slot7-wedge");
    }
    return 0;
}
// After the stub lets FE render, FE_Client wedges at its in-game "initializing
// network" screen without ever contacting the FE lobby balancer (:54848). Dump all
// threads ~20s in to see what its world-init is waiting on (the FL we skipped? a
// local event? a socket?). Fires once. Gated by fl_slot7_stub so it only runs when
// we're actually driving the stub path.
static DWORD WINAPI fl_poststub_watchdog(void*)
{
    Sleep(20000);
    logf("[fl] post-stub +20s: dumping all threads to find the 'initializing network' stall");
    log_flush();
    crashlog_dump_all_threads("fe-netinit-stall-poststub");
    return 0;
}
// Async mode: run the REAL slot 7 on a worker thread so it can't block FE's main
// thread. The real init creates the FL singleton (global 0x1020c340) and spawns its
// session worker; FE's per-frame slot-8 poll then observes the singleton and advances
// if the session comes up. Tests whether the FL session can establish off the main
// thread under Wine, without us having to fabricate the "ready" out-param.
struct FLSlot7Args { void* self; void* arg; };
static DWORD WINAPI fl_slot7_async_run(void* p)
{
    FLSlot7Args* a = (FLSlot7Args*)p;
    logf("[fl] slot7 ASYNC worker running real init on tid=%lu", GetCurrentThreadId());
    log_flush();
    HRESULT hr = g_fl_slot7_real ? g_fl_slot7_real(a->self, a->arg) : E_FAIL;
    logf("[fl] slot7 ASYNC real init RETURNED hr=0x%08lX (tid=%lu)", (unsigned long)hr, GetCurrentThreadId());
    log_flush();
    free(a);
    return 0;
}
// ---- STA-deadlock breaker: pump messages during the FL's WaitForSingleObject ----
// The in-game FL's slot-7 init (FriendList.dll sub_1005f0d0) builds its window,
// installs a WH_GETMESSAGE hook, then blocks on WaitForSingleObject on FE's OWN
// (STA/main) thread -- waiting on an event that only gets set once a message reaches
// that hook (or a worker's SendMessage to WInFriendListWindow returns). But FE's
// message pump is frozen INSIDE this synchronous slot-7 call, so nothing is
// dispatched and the wait never completes. The standalone Friend List app works
// because IT pumps. FriendList.dll imports WaitForSingleObject but NO
// GetMessage/MsgWait/CoWait, so it never pumps itself. Fix: when the FL calls
// WaitForSingleObject on the STA thread while we are inside slot 7, dispatch pending
// messages while waiting (MsgWaitForMultipleObjects loop), exactly as a correct STA
// blocking wait would. Only on that one thread, only during slot 7 -- worker-thread
// waits pass straight through. FriendList.dll is POL1-packed and calls imports via a
// runtime function table (not the PE IAT), so we cannot patch_iat it; instead we scan
// its writable data for the resolved WaitForSingleObject pointer and swap every slot.
typedef DWORD (WINAPI *PFN_WFSO)(HANDLE, DWORD);
static PFN_WFSO real_wfso = NULL;

// SetEvent watch: while inside slot 7, log every SetEvent the FL makes so we can see
// whether the handle the STA thread waits on is ever signalled (and by whom).
typedef BOOL (WINAPI *PFN_SETEVENT)(HANDLE);
static PFN_SETEVENT real_setevent = NULL;
static BOOL WINAPI hook_setevent(HANDLE h)
{
    if (g_fl_pump && InterlockedCompareExchange(&g_fl_slot7_inside, 0, 0) == 1) {
        logf("[fl] SetEvent(%p) by tid=%lu (during slot 7)", h, GetCurrentThreadId());
        log_flush();
    }
    return real_setevent ? real_setevent(h) : SetEvent(h);
}

static void caller_desc(void* ra, char* buf, size_t n);   // defined later; names ra's module+RVA

static DWORD WINAPI pump_wfso(HANDLE h, DWORD ms)
{
    void* ra = _ReturnAddress();   // the instruction that called our patched WFSO slot
    if (g_fl_pump && InterlockedCompareExchange(&g_fl_slot7_inside, 0, 0) == 1 &&
        GetCurrentThreadId() == (DWORD)g_fl_sta_tid) {
        static volatile LONG logged = 0;
        if (InterlockedExchange(&logged, 1) == 0) {
            // Classify the handle: GetThreadId != 0 => it is a THREAD handle, so this
            // WaitForSingleObject is a worker-thread JOIN (the worker never exits under
            // Wine). == 0 => it is an event/mutex/other sync object (something must
            // SetEvent it). This single fact picks the durable fix's direction.
            DWORD wtid = GetThreadId(h);
            logf("[fl] WFSO PUMP engaged on STA tid=%lu (handle=%p timeout=%lu) "
                 "GetThreadId=%lu -> %s", GetCurrentThreadId(), h, ms, wtid,
                 wtid ? "THREAD handle: this is a worker JOIN"
                      : "NOT a thread: event/mutex (needs SetEvent)");
            log_flush();
            // DEFINITIVE caller: pump_wfso was reached through the FL's patched WFSO
            // slot, so our own return address IS the exact instruction that called
            // WaitForSingleObject -- names the FL function that owns the wait, with no
            // stack-scan ambiguity.
            char who[192]; caller_desc(ra, who, sizeof(who));
            logf("[fl] WFSO CALLER%s  <-- the code that called WaitForSingleObject", who);
            log_flush();
            // Secondary: raw stack scan for context (note: some slots are winsock
            // function-POINTER tables, not return addresses -- trust WFSO CALLER above).
            crashlog_scan_current_stack("fl-wfso-wait");
        }
        // FORCE-GRANT path (fl_wfso_grant_ms>0): a PLAIN timed wait, no message pump.
        // Pumping here re-enters DispatchMessage and blocks (the msg handler wedges),
        // so the pump loop below never gets to check the timeout. A plain real wait with
        // a finite timeout returns cleanly; on timeout we grant to break the deadlock.
        if (g_fl_grant_ms > 0) {
            DWORD r = real_wfso ? real_wfso(h, (DWORD)g_fl_grant_ms)
                                : WaitForSingleObject(h, (DWORD)g_fl_grant_ms);
            if (r == WAIT_TIMEOUT) {
                logf("[fl] WFSO FORCE-GRANT after %dms (handle=%p, plain wait) -> WAIT_OBJECT_0 to "
                     "break the SQ_ENVVAR_LOCK deadlock; watch for slot7 EXIT + FE advancing",
                     g_fl_grant_ms, h);
                log_flush();
                return WAIT_OBJECT_0;
            }
            return r;   // it actually signalled within the window
        }
        DWORD start = GetTickCount();
        DWORD lastlog = start;
        for (;;) {
            DWORD rem = 1000;   // wake at least every 1s so we can heartbeat-log
            if (ms != INFINITE) {
                DWORD el = GetTickCount() - start;
                if (el >= ms) return WAIT_TIMEOUT;
                DWORD left = ms - el;
                if (left < rem) rem = left;
            }
            DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, rem, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0)  return WAIT_OBJECT_0;   // the handle signalled
            if (r == WAIT_OBJECT_0 + 1) {                    // messages pending
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            } else if (r != WAIT_TIMEOUT) {
                return r;                                    // WAIT_FAILED / abandoned
            }
            if (ms == INFINITE && (GetTickCount() - lastlog) >= 5000) {
                lastlog = GetTickCount();
                logf("[fl] WFSO PUMP still waiting on %p after %lus (pumping)",
                     h, (GetTickCount() - start) / 1000);
                log_flush();
            }
            // Force-grant the stuck acquire (SQ_ENVVAR_LOCK) to test whether the mutex
            // deadlock is the whole wedge. Only the genuinely-stuck wait reaches this;
            // legit short waits signal long before g_fl_grant_ms.
            if (g_fl_grant_ms > 0 && (GetTickCount() - start) >= (DWORD)g_fl_grant_ms) {
                logf("[fl] WFSO FORCE-GRANT after %dms (handle=%p) -> returning WAIT_OBJECT_0 to "
                     "break the mutex deadlock; watch for slot7 EXIT and FE advancing", g_fl_grant_ms, h);
                log_flush();
                return WAIT_OBJECT_0;
            }
            if (ms != INFINITE && (GetTickCount() - start) >= ms) return WAIT_TIMEOUT;
        }
    }
    return real_wfso ? real_wfso(h, ms) : WaitForSingleObject(h, ms);
}

// Scan FriendList.dll's writable committed pages for the resolved
// WaitForSingleObject pointer and swap each occurrence to pump_wfso. Returns the
// number of slots patched. Runs once, during the FL's CoCreateInstance (its runtime
// import table is resolved by then, before FE calls slot 7).
static void fl_install_wfso_pump(HMODULE flm)
{
    if (!flm || !g_fl_pump) return;
    logf("[fl] WFSO pump config: g_fl_pump=%d g_fl_grant_ms=%d", g_fl_pump, g_fl_grant_ms);
    if (!real_wfso) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        real_wfso = k32 ? (PFN_WFSO)GetProcAddress(k32, "WaitForSingleObject") : NULL;
    }
    if (!real_wfso) { logf("[fl] WFSO pump: could not resolve real WaitForSingleObject"); return; }

    // Also collect alternate resolutions: under Wine WaitForSingleObject may live in
    // kernelbase/kernel32 as a forwarder, and the FL's bound slot may hold either.
    void* cands[3]; int ncand = 0;
    cands[ncand++] = (void*)real_wfso;
    { HMODULE kb = GetModuleHandleA("kernelbase.dll");
      if (kb) { void* a = (void*)GetProcAddress(kb, "WaitForSingleObject");
                if (a && a != cands[0]) cands[ncand++] = a; } }
    { HMODULE k32 = GetModuleHandleA("kernel32.dll");
      if (k32) { void* a = (void*)GetProcAddress(k32, "WaitForSingleObject");
                 if (a && a != cands[0] && (ncand<2 || a != cands[1])) cands[ncand++] = a; } }

    void* repl = (void*)&pump_wfso;
    int patched = 0, seen_ro = 0, seen_rw = 0, seen_x = 0;
    MEMORY_BASIC_INFORMATION mbi;
    BYTE* p = (BYTE*)flm;
    // Bound the scan to FriendList.dll's own mapped image.
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)flm;
    IMAGE_NT_HEADERS* nt = (dos->e_magic == IMAGE_DOS_SIGNATURE)
        ? (IMAGE_NT_HEADERS*)((BYTE*)flm + dos->e_lfanew) : NULL;
    BYTE* end = nt ? ((BYTE*)flm + nt->OptionalHeader.SizeOfImage) : ((BYTE*)flm + 0x1000000);
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        BYTE* base = (BYTE*)mbi.BaseAddress;
        SIZE_T rgsz = mbi.RegionSize;
        // Scan DATA pages (RO/RW/WC) -- a bound IAT is often read-only. Skip
        // executable pages so a code immediate equal to the address is never patched.
        BOOL exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        BOOL data = (mbi.State == MEM_COMMIT) && !exec &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY));
        if (data && base >= (BYTE*)flm && base < end) {
            SIZE_T span = rgsz;
            if (base + span > end) span = end - base;
            for (SIZE_T o = 0; o + sizeof(void*) <= span; o += sizeof(void*)) {
                void** slot = (void**)(base + o);
                void* v = *slot;
                int hit = 0;
                for (int c = 0; c < ncand; c++) if (v == cands[c]) { hit = 1; break; }
                if (hit) {
                    DWORD oldp = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldp)) {
                        *slot = repl;
                        VirtualProtect(slot, sizeof(void*), oldp, &oldp);
                        patched++;
                        if (mbi.Protect & PAGE_READONLY) seen_ro++;
                        else if (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) seen_rw++;
                    }
                }
            }
        }
        p = base + rgsz;
        if (rgsz == 0) break;
    }
    // Diagnostic pass: if we patched nothing, report whether the pointer exists at
    // all in the image and under what protection, so the miss is not a mystery.
    if (patched == 0) {
        p = (BYTE*)flm;
        while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            BYTE* base = (BYTE*)mbi.BaseAddress;
            SIZE_T rgsz = mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && base >= (BYTE*)flm && base < end) {
                SIZE_T span = rgsz; if (base + span > end) span = end - base;
                for (SIZE_T o = 0; o + sizeof(void*) <= span; o += sizeof(void*)) {
                    void* v = *(void**)(base + o);
                    for (int c = 0; c < ncand; c++) if (v == cands[c]) {
                        BOOL x = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
                        if (x) seen_x++;
                        else if (mbi.Protect & PAGE_READONLY) seen_ro++;
                        else seen_rw++;
                        break;
                    }
                }
            }
            p = base + rgsz; if (rgsz == 0) break;
        }
        logf("[fl] WFSO pump: 0 patched. cands=%d (k32/kbase) found in image: ro=%d rw=%d exec=%d "
             "(real=%p) -- if all 0, the FL resolves WFSO lazily or via another module",
             ncand, seen_ro, seen_rw, seen_x, cands[0]);
    } else {
        logf("[fl] WFSO pump: patched %d WaitForSingleObject slot(s) in FriendList.dll "
             "(ro=%d rw=%d, real=%p -> pump=%p)", patched, seen_ro, seen_rw, cands[0], repl);
    }
    log_flush();
}

// Swap FriendList.dll's resolved SetEvent pointer(s) to hook_setevent (same
// read-only-IAT-aware data scan as the WFSO pump). No-op unless fl_slot7_pump=1.
static void fl_install_setevent_watch(HMODULE flm)
{
    if (!flm || !g_fl_pump) return;
    void* cands[2]; int ncand = 0;
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (kb)  { void* a = (void*)GetProcAddress(kb, "SetEvent");  if (a) cands[ncand++] = a; }
    if (k32) { void* a = (void*)GetProcAddress(k32, "SetEvent");
               if (a && (ncand == 0 || a != cands[0])) cands[ncand++] = a; }
    if (!ncand) return;
    real_setevent = (PFN_SETEVENT)cands[0];
    void* repl = (void*)&hook_setevent;
    int patched = 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)flm;
    IMAGE_NT_HEADERS* nt = (dos->e_magic == IMAGE_DOS_SIGNATURE)
        ? (IMAGE_NT_HEADERS*)((BYTE*)flm + dos->e_lfanew) : NULL;
    BYTE* end = nt ? ((BYTE*)flm + nt->OptionalHeader.SizeOfImage) : ((BYTE*)flm + 0x1000000);
    MEMORY_BASIC_INFORMATION mbi; BYTE* p = (BYTE*)flm;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        BYTE* base = (BYTE*)mbi.BaseAddress; SIZE_T rgsz = mbi.RegionSize;
        BOOL exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        BOOL data = (mbi.State == MEM_COMMIT) && !exec &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY));
        if (data && base >= (BYTE*)flm && base < end) {
            SIZE_T span = rgsz; if (base + span > end) span = end - base;
            for (SIZE_T o = 0; o + sizeof(void*) <= span; o += sizeof(void*)) {
                void** slot = (void**)(base + o); void* v = *slot;
                int hit = 0; for (int c = 0; c < ncand; c++) if (v == cands[c]) { hit = 1; break; }
                if (hit) { DWORD oldp = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldp)) {
                        *slot = repl; VirtualProtect(slot, sizeof(void*), oldp, &oldp); patched++; } }
            }
        }
        p = base + rgsz; if (rgsz == 0) break;
    }
    logf("[fl] SetEvent watch: patched %d slot(s) (real=%p)", patched, cands[0]);
    log_flush();
}

// Generic: swap every FriendList.dll data slot holding `target` to `repl`. Returns
// count. Scans RO+RW non-exec pages (the FL's runtime import table can be read-only).
static int fl_swap_ptr(HMODULE flm, void* target, void* repl)
{
    if (!flm || !target || !repl) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)flm;
    IMAGE_NT_HEADERS* nt = (dos->e_magic == IMAGE_DOS_SIGNATURE)
        ? (IMAGE_NT_HEADERS*)((BYTE*)flm + dos->e_lfanew) : NULL;
    BYTE* end = nt ? ((BYTE*)flm + nt->OptionalHeader.SizeOfImage) : ((BYTE*)flm + 0x1000000);
    int n = 0; MEMORY_BASIC_INFORMATION mbi; BYTE* p = (BYTE*)flm;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        BYTE* base = (BYTE*)mbi.BaseAddress; SIZE_T rgsz = mbi.RegionSize;
        BOOL exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        BOOL data = (mbi.State == MEM_COMMIT) && !exec &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY));
        if (data && base >= (BYTE*)flm && base < end) {
            SIZE_T span = rgsz; if (base + span > end) span = end - base;
            for (SIZE_T o = 0; o + sizeof(void*) <= span; o += sizeof(void*)) {
                void** slot = (void**)(base + o);
                if (*slot == target) { DWORD op = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op)) {
                        *slot = repl; VirtualProtect(slot, sizeof(void*), op, &op); n++; } }
            }
        }
        p = base + rgsz; if (rgsz == 0) break;
    }
    return n;
}

// ---- Winsock endpoint probe: confirm 0x400 is a WSA network event and read the
// endpoint the in-game FL dials. FriendList.dll calls winsock through its runtime
// function table, invisible to the shim's ws2_32 IAT hooks, so we swap the FL's own
// resolved slots. Minimal ABI decls (polshim.h uses WIN32_LEAN_AND_MEAN). ----
typedef UINT_PTR SOCKET_T;
struct sockaddr_in_min { short sin_family; unsigned short sin_port; unsigned long sin_addr; unsigned char pad[8]; };
typedef int    (WINAPI *PFN_CONNECT)(SOCKET_T, const void*, int);
typedef void*  (WINAPI *PFN_GHBN)(const char*);
typedef HANDLE (WINAPI *PFN_WSACE)(void);
typedef int    (WINAPI *PFN_WSAES)(SOCKET_T, HANDLE, long);
static PFN_CONNECT real_connect = NULL;
static PFN_GHBN    real_ghbn    = NULL;
static PFN_WSACE   real_wsace   = NULL;
static PFN_WSAES   real_wsaes   = NULL;

static int WINAPI hook_connect(SOCKET_T s, const void* name, int namelen)
{
    if (name && namelen >= 8) {
        const sockaddr_in_min* a = (const sockaddr_in_min*)name;
        if (a->sin_family == 2) {  // AF_INET
            unsigned long ip = a->sin_addr;
            unsigned short port = (unsigned short)((a->sin_port >> 8) | (a->sin_port << 8));
            logf("[fl] connect sock=%Iu -> %lu.%lu.%lu.%lu:%u (tid=%lu)", s,
                 ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff,
                 port, GetCurrentThreadId());
            log_flush();
        }
    }
    return real_connect(s, name, namelen);
}
static void* WINAPI hook_ghbn(const char* n)
{
    logf("[fl] gethostbyname(%s) (tid=%lu)", n ? n : "(null)", GetCurrentThreadId());
    log_flush();
    return real_ghbn(n);
}
static HANDLE WINAPI hook_wsace(void)
{
    HANDLE h = real_wsace();
    logf("[fl] WSACreateEvent -> %p (tid=%lu)", h, GetCurrentThreadId());
    log_flush();
    return h;
}
static int WINAPI hook_wsaes(SOCKET_T s, HANDLE ev, long events)
{
    logf("[fl] WSAEventSelect sock=%Iu event=%p mask=0x%lx (tid=%lu) "
         "<-- if event==0x400, the wedge IS this socket", s, ev, events, GetCurrentThreadId());
    log_flush();
    return real_wsaes(s, ev, events);
}
static void fl_install_winsock_probe(HMODULE flm)
{
    if (!flm || !g_fl_pump) return;
    HMODULE ws = GetModuleHandleA("ws2_32.dll");
    if (!ws) { logf("[fl] winsock probe: ws2_32 not loaded"); return; }
    real_connect = (PFN_CONNECT)GetProcAddress(ws, "connect");
    real_ghbn    = (PFN_GHBN)   GetProcAddress(ws, "gethostbyname");
    real_wsace   = (PFN_WSACE)  GetProcAddress(ws, "WSACreateEvent");
    real_wsaes   = (PFN_WSAES)  GetProcAddress(ws, "WSAEventSelect");
    int c1 = real_connect ? fl_swap_ptr(flm, (void*)real_connect, (void*)&hook_connect) : 0;
    int c2 = real_ghbn    ? fl_swap_ptr(flm, (void*)real_ghbn,    (void*)&hook_ghbn)    : 0;
    int c3 = real_wsace   ? fl_swap_ptr(flm, (void*)real_wsace,   (void*)&hook_wsace)   : 0;
    int c4 = real_wsaes   ? fl_swap_ptr(flm, (void*)real_wsaes,   (void*)&hook_wsaes)   : 0;
    logf("[fl] winsock probe: patched connect=%d gethostbyname=%d WSACreateEvent=%d WSAEventSelect=%d",
         c1, c2, c3, c4);
    log_flush();
}

static HRESULT __stdcall hook_fl_slot7(void* self, void* arg)
{
    InterlockedExchange(&g_fl_sta_tid, (LONG)GetCurrentThreadId());
    logf("[fl] slot7 ENTER self=%p arg=%p (tid=%lu)", self, arg, GetCurrentThreadId());
    log_flush();
    if (g_fl_slot7_async) {
        FLSlot7Args* a = (FLSlot7Args*)malloc(sizeof(FLSlot7Args));
        a->self = self; a->arg = arg;
        HANDLE t = CreateThread(NULL, 0, fl_slot7_async_run, a, 0, NULL);
        if (t) CloseHandle(t);
        logf("[fl] slot7 ASYNC -> returning S_OK to FE now; real init runs in background");
        log_flush();
        return S_OK;
    }
    // FIX EXPERIMENT: skip the FL's blocking init entirely and tell FE it succeeded.
    // FE checks hr>=0 and continues to its render path (FE_Client+0x1D8AD1). If the
    // game appears, the in-game FL modal init WAS the whole black screen; the in-game
    // friend list is then inert but the game is playable. If FE later faults using the
    // half-live object, the init is load-bearing and we need the real fix.
    if (g_fl_slot7_stub) {
        logf("[fl] slot7 STUBBED -> S_OK (fl_slot7_stub=1); real init skipped");
        log_flush();
        HANDLE wd = CreateThread(NULL, 0, fl_poststub_watchdog, NULL, 0, NULL);
        if (wd) CloseHandle(wd);
        return S_OK;
    }
    InterlockedExchange(&g_fl_slot7_inside, 1);
    HANDLE wd = CreateThread(NULL, 0, fl_slot7_watchdog, NULL, 0, NULL);
    if (wd) CloseHandle(wd);
    HRESULT hr = g_fl_slot7_real ? g_fl_slot7_real(self, arg) : E_FAIL;
    InterlockedExchange(&g_fl_slot7_inside, 0);
    logf("[fl] slot7 EXIT hr=0x%08lX (tid=%lu) -- so it DID return; FE blocks LATER",
         (unsigned long)hr, GetCurrentThreadId());
    log_flush();
    return hr;
}

// {6D365D27-4999-4BC5-AADF-513EF0E7B438} -- IPolContentsCom, the ONE entry
// interface pol.exe knows how to ask for. Hardcoded at pol.exe .rdata 0x4421cc
// and pushed as riid at 0x414220.
static const GUID IID_IPolContentsCom =
    {0x6D365D27,0x4999,0x4BC5,{0xAA,0xDF,0x51,0x3E,0xF0,0xE7,0xB4,0x38}};

typedef HRESULT (WINAPI *PFN_CCI)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef HRESULT (WINAPI *PFN_CCIEX)(REFCLSID, LPUNKNOWN, DWORD, COSERVERINFO*,
                                    DWORD, MULTI_QI*);
typedef HRESULT (WINAPI *PFN_CGCO)(REFCLSID, DWORD, COSERVERINFO*, REFIID, LPVOID*);
typedef HRESULT (WINAPI *PFN_CFPI)(LPCOLESTR, LPCLSID);

static PFN_CCI   real_CoCreateInstance   = NULL;
static PFN_CCIEX real_CoCreateInstanceEx = NULL;
static PFN_CGCO  real_CoGetClassObject   = NULL;
static PFN_CFPI  real_CLSIDFromProgID    = NULL;

static LONG g_calls = 0;
static LONG g_fails = 0;
// Raw hook-entry counters, incremented BEFORE anything else can filter them.
// Separates "the hook was never called" from "the hook ran but logged nothing".
static LONG g_entered_cci = 0;
static LONG g_entered_cgco = 0;

void com_set_enabled(int on) { g_comtrace = on ? 1 : 0; }
int  comtrace_enabled()      { return g_comtrace; }

// --- registry diagnosis ------------------------------------------------------

static void guid_str(const GUID& g, char* out, size_t cb)
{
    _snprintf_s(out, cb, _TRUNCATE,
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
        g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// Read InprocServer32's default value for `clsid` from one registry view.
// `flag` is KEY_WOW64_32KEY or KEY_WOW64_64KEY -- we ask for BOTH explicitly
// rather than letting WOW64 redirection pick, because "registered in the view
// this process cannot see" is one of the failure modes we need to name.
static bool inproc_for(const char* clsid, REGSAM flag, char* out, size_t cb,
                       bool* key_exists)
{
    char path[256];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "CLSID\\%s", clsid);
    HKEY k;
    *key_exists = false;
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, path, 0, KEY_READ | flag, &k) != ERROR_SUCCESS)
        return false;
    *key_exists = true;

    _snprintf_s(path, sizeof(path), _TRUNCATE, "CLSID\\%s\\InprocServer32", clsid);
    HKEY s;
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, path, 0, KEY_READ | flag, &s) != ERROR_SUCCESS) {
        RegCloseKey(k);
        return false;
    }
    DWORD cb2 = (DWORD)cb, type = 0;
    LONG r = RegQueryValueExA(s, NULL, NULL, &type, (LPBYTE)out, &cb2);
    RegCloseKey(s);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return false;
    out[(cb2 < cb) ? cb2 : cb - 1] = 0;
    return true;
}

// Render a CLSCTX bitmask. This parameter is logged because it turned out to be
// the whole story for FMO (2026-08-12): fmoprobe.exe swept CLSID x IID x CLSCTX
// x apartment against the real component and REGDB_E_CLASSNOTREG appeared in
// exactly two cells -- CLSCTX_INPROC_HANDLER and CLSCTX_LOCAL_SERVER. With
// CLSCTX_INPROC_SERVER the very same class activates. So for a class that has
// only an InprocServer32, "class not registered" can mean "you asked for a
// flavour of it that isn't registered", and the HRESULT alone cannot say which.
// Never log an activation failure without its ctx again.
static void ctx_str(DWORD ctx, char* out, size_t cb)
{
    static const struct { DWORD bit; const char* nm; } K[] = {
        { 0x1,  "INPROC_SERVER"  }, { 0x2,  "INPROC_HANDLER" },
        { 0x4,  "LOCAL_SERVER"   }, { 0x10, "REMOTE_SERVER"  },
        { 0x8,  "INPROC_SERVER16"}, { 0x20, "INPROC_HANDLER16" },
    };
    out[0] = 0;
    DWORD left = ctx;
    for (int i = 0; i < (int)(sizeof(K)/sizeof(K[0])); i++) {
        if (ctx & K[i].bit) {
            if (out[0]) strncat_s(out, cb, "|", _TRUNCATE);
            strncat_s(out, cb, K[i].nm, _TRUNCATE);
            left &= ~K[i].bit;
        }
    }
    if (left || !out[0]) {
        char x[32];
        _snprintf_s(x, sizeof(x), _TRUNCATE, "%s0x%lX",
                    out[0] ? "|" : "", (unsigned long)left);
        strncat_s(out, cb, x, _TRUNCATE);
    }
}

// Emit the one line that turns a bare HRESULT into an actionable statement.
static void diagnose(const GUID& clsid, DWORD ctx)
{
    char cs[64];
    guid_str(clsid, cs, sizeof(cs));

    char p32[MAX_PATH] = {0}, p64[MAX_PATH] = {0};
    bool k32 = false, k64 = false;
    bool has32 = inproc_for(cs, KEY_WOW64_32KEY, p32, sizeof(p32), &k32);
    bool has64 = inproc_for(cs, KEY_WOW64_64KEY, p64, sizeof(p64), &k64);

    if (!k32 && !k64) {
        logf("[com]   DIAGNOSIS: %s is registered in NEITHER view -- "
             "nothing on this machine claims that class", cs);
        return;
    }
    if (!k32 && k64) {
        logf("[com]   DIAGNOSIS: %s is registered ONLY in the 64-bit view "
             "(InprocServer32=%s). This process is 32-bit, so it cannot see it.",
             cs, has64 ? p64 : "(none)");
        return;
    }
    if (has32) {
        DWORD attr = GetFileAttributesA(p32);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            logf("[com]   DIAGNOSIS: %s registered (32-bit) but its server is "
                 "MISSING ON DISK: %s", cs, p32);
            return;
        }
        // The InprocServer32 exists, but the caller may not have asked for an
        // inproc server. That combination is REGDB_E_CLASSNOTREG with nothing
        // wrong anywhere -- checked here BEFORE blaming the DLL, because the
        // older wording ("the DLL itself refused this CLSID") sent this
        // investigation down a dead end for a day.
        if (!(ctx & 0x1)) {
            char xs[96];
            ctx_str(ctx, xs, sizeof(xs));
            logf("[com]   DIAGNOSIS: %s has an InprocServer32 (%s) that EXISTS, "
                 "but the caller passed ctx=%s, which does NOT include "
                 "CLSCTX_INPROC_SERVER. COM therefore looked for a flavour of "
                 "the class that is not registered. The class is fine; the "
                 "CONTEXT is wrong.", cs, p32, xs);
            return;
        }
        logf("[com]   DIAGNOSIS: %s registered (32-bit), server EXISTS (%s), and "
             "the caller did ask for CLSCTX_INPROC_SERVER -- so the DLL itself "
             "refused this CLSID; its DllGetClassObject returned the failure.",
             cs, p32);
        return;
    }
    logf("[com]   DIAGNOSIS: %s has a CLSID key in the 32-bit view but NO "
         "InprocServer32 value -- registration is incomplete.", cs);
}

// WHO asked for this class.
//
// "A second PlayOnline Viewer appears while Fantasy Earth is on its character
// screen" was traced to a CoCreateInstance of app.dll's {40555AAE-...}, and then
// stalled: the log said WHAT was activated but never WHICH MODULE asked, so there
// was nothing to disassemble. Every hook here sits directly on the caller's
// stack, so its return address lands inside that module -- exactly the trick
// d3d8hook's caller_excepted() already uses to tell TM from FFXI from FE when
// they share one vtable.
//
// Reported as leaf+RVA, because the RVA is what a disassembler wants: a packed
// title's runtime base moves every launch (FE_Client.dll was at 0x04F90000 one
// session and 0x0F800000 the next), so an absolute address is useless a day
// later while `FE_Client.dll+0x1234` stays meaningful against a dump.
static void caller_desc(void* ra, char* buf, size_t n)
{
    buf[0] = '\0';
    if (!ra) return;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ra, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        _snprintf_s(buf, n, _TRUNCATE, " from=%p (no module)", ra);
        return;
    }
    char path[MAX_PATH] = "";
    const char* leaf = "?";
    if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) {
        const char* s = strrchr(path, '\\');
        leaf = s ? s + 1 : path;
    }
    _snprintf_s(buf, n, _TRUNCATE, " from=%s+0x%lX (base=%p)", leaf,
                (unsigned long)((char*)ra - (char*)mbi.AllocationBase),
                mbi.AllocationBase);
}

static void record(const char* api, const GUID& clsid, const GUID* iid,
                   DWORD ctx, HRESULT hr, void* ra)
{
    if (!g_comtrace) return;
    InterlockedIncrement(&g_calls);

    char cs[64], cn[64], in[64], xs[96], who[MAX_PATH + 64];
    guid_str(clsid, cs, sizeof(cs));
    iid_name(clsid, cn, sizeof(cn));
    ctx_str(ctx, xs, sizeof(xs));
    caller_desc(ra, who, sizeof(who));

    if (SUCCEEDED(hr)) {
        logf("[com] %s clsid=%s (%s) iid=%s ctx=%s -> OK%s", api, cs, cn,
             iid ? iid_name(*iid, in, sizeof(in)) : "-", xs, who);
        return;
    }

    InterlockedIncrement(&g_fails);
    const char* what = "";
    switch ((unsigned long)hr) {
        case 0x80040154UL: what = " REGDB_E_CLASSNOTREG";     break;
        case 0x80040155UL: what = " REGDB_E_IIDNOTREG";       break;
        case 0x80004002UL: what = " E_NOINTERFACE";           break;
        case 0x80040111UL: what = " CLASS_E_CLASSNOTAVAILABLE"; break;
        case 0x8007007EUL: what = " ERROR_MOD_NOT_FOUND";     break;
        case 0x800401F3UL: what = " CO_E_CLASSSTRING (bad ProgID)"; break;
        default: break;
    }
    logf("[com] %s clsid=%s (%s) iid=%s ctx=%s -> FAILED hr=0x%08lX%s%s",
         api, cs, cn, iid ? iid_name(*iid, in, sizeof(in)) : "-", xs,
         (unsigned long)hr, what, who);
    diagnose(clsid, ctx);
    log_flush();            // a failure is the line we are here for -- never lose it
}

// --- the ContentsIID alias ---------------------------------------------------
//
// THE FMO BUG, and why the fix belongs here rather than in the registry.
//
// Every COM-content title defines its OWN entry interface, and the installer
// records it in HKLM\SOFTWARE\<hive>\ContentsIID\<%04d content id>:
//
//     0004  FMO           IFMOEntry         {2031D0EF-97FA-48AE-A3FD-8260C380029A}
//     0011  Fantasy Earth IFantasyEarthCom  {6560D1E0-BA0D-4877-BFB6-A438529E9701}
//           SE's sample   IPolContentsCom   {6D365D27-4999-4BC5-AADF-513EF0E7B438}
//
// pol.exe reads ContentsCLSID to get the class (0x40f048) -- but it does NOT
// read ContentsIID to get the interface. It hardcodes IPolContentsCom:
//
//     0x414220  push 0x4421cc      ; riid = IID_IPolContentsCom
//     0x414225  push 0x17          ; CLSCTX_ALL
//     0x414230  call ole32!CoCreateInstance
//
// It touches ContentsIID exactly once, at 0x4142d4 -- inside the branch taken
// when GameStart FAILS, i.e. error reporting, long after the interface has been
// chosen. So activating FMO asks a component that implements IFMOEntry for
// IPolContentsCom, and the QI fails.
//
// The user-visible error is a red herring twice over. Nothing is unregistered:
// with CLSCTX_ALL, COM tries the remaining contexts after the inproc QI fails,
// none of which are registered, and the caller gets REGDB_E_CLASSNOTREG instead
// of the E_NOINTERFACE that actually happened. (Measured: fmoprobe.exe `cold`
// reproduces 0x80040154 exactly; the same call on a WARM class object returns
// 0x80004002, which is why the matrix disagreed with the client until the probe
// was made single-shot.)
//
// The alias is safe because the interfaces are ABI-IDENTICAL, not merely
// similar. Both typelibs declare exactly one method at vtable slot 3:
//
//     HRESULT GameStart([in] IUnknown* pPolCore, [out] IUnknown** pMessage)
//
// and slot 3 is precisely what pol.exe calls at 0x4142bb. After that call it
// only ever Releases the pointer (0x41431b), so handing back the native
// IFMOEntry needs no wrapper and no thunk -- there is no later QI to satisfy.
//
// Driven off the registry rather than a hardcoded GUID pair, so Fantasy Earth
// and any other COM content are covered by the same code.
static bool guid_from_reg_sz(const char* sz, GUID* out)
{
    wchar_t w[64];
    // The registry stores these bare; IIDFromString demands the braces.
    _snwprintf_s(w, 64, _TRUNCATE, L"{%S}", sz);
    return SUCCEEDED(IIDFromString(w, out));
}

// Find the ContentsIID registered alongside whichever ContentsCLSID entry
// equals `clsid`, in any of the three regional hives.
static bool contents_iid_for(const GUID& clsid, GUID* out, char* idname, size_t cb)
{
    char want[64];
    guid_str(clsid, want, sizeof(want));       // "{....}"
    // compare against the registry's bare form
    char bare[64];
    strncpy_s(bare, sizeof(bare), want + 1, _TRUNCATE);
    size_t n = strlen(bare);
    if (n) bare[n - 1] = 0;

    static const char* HIVES[] = { "SOFTWARE\\PlayOnlineUS",
                                   "SOFTWARE\\PlayOnline",
                                   "SOFTWARE\\PlayOnlineEU" };
    for (int h = 0; h < 3; h++) {
        char path[256];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\ContentsCLSID", HIVES[h]);
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0,
                          KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0; ; i++) {
            char name[64], val[128];
            DWORD cn = sizeof(name), cv = sizeof(val), type = 0;
            LONG r = RegEnumValueA(k, i, name, &cn, NULL, &type,
                                   (LPBYTE)val, &cv);
            if (r != ERROR_SUCCESS) break;
            if (type != REG_SZ) continue;
            val[(cv < sizeof(val)) ? cv : sizeof(val) - 1] = 0;
            if (_stricmp(val, bare) != 0) continue;

            // Same value NAME (the %04d content id) under ContentsIID.
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\ContentsIID", HIVES[h]);
            HKEY k2;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0,
                              KEY_READ | KEY_WOW64_32KEY, &k2) == ERROR_SUCCESS) {
                char iidv[128];
                DWORD ci = sizeof(iidv), t2 = 0;
                LONG r2 = RegQueryValueExA(k2, name, NULL, &t2, (LPBYTE)iidv, &ci);
                RegCloseKey(k2);
                if (r2 == ERROR_SUCCESS && t2 == REG_SZ) {
                    iidv[(ci < sizeof(iidv)) ? ci : sizeof(iidv) - 1] = 0;
                    if (guid_from_reg_sz(iidv, out)) {
                        _snprintf_s(idname, cb, _TRUNCATE, "%s\\ContentsIID\\%s",
                                    HIVES[h], name);
                        RegCloseKey(k);
                        return true;
                    }
                }
            }
        }
        RegCloseKey(k);
    }
    return false;
}

// --- the hooks ---------------------------------------------------------------

// A COM activation is the EARLIEST point at which a packed title is reliably
// unpacked and running, so it is also the earliest safe moment to arm the
// shim's late title hooks. See arm_title_hooks_late() in inject.cpp.
extern "C" void shim_arm_title_late(void);

static HRESULT WINAPI hook_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN outer,
                                            DWORD ctx, REFIID riid, LPVOID* out)
{
    shim_arm_title_late();
    InterlockedIncrement(&g_entered_cci);

    // EXPERIMENT: refuse the in-game Friend List. FE creates FriendListCom (STA) on
    // its main thread and then wedges before rendering. Failing the activation tests
    // whether skipping the in-game FL lets FE reach its render loop. Return
    // REGDB_E_CLASSNOTREG so a caller that checks the HRESULT can degrade gracefully.
    if (g_block_ingame_fl && IsEqualGUID(rclsid, kFriendListCom)) {
        if (out) *out = NULL;
        char who[160]; caller_desc(_ReturnAddress(), who, sizeof(who));
        logf("[com] BLOCKED FriendListCom activation (block_ingame_friendlist=1)%s "
             "-> REGDB_E_CLASSNOTREG", who);
        log_flush();
        return REGDB_E_CLASSNOTREG;
    }

    // BEFORE the activation, not after: launching a content title runs through
    // here, and app.dll's Check-Files/beta-flag patch has to be in place by the
    // time pol.exe evaluates the gate. See patches_reverify -- the startup
    // one-shot decays, which is why the beta error returns later in a session.
    patches_reverify();

    HRESULT hr = real_CoCreateInstance(rclsid, outer, ctx, riid, out);

    // FRIEND LIST WEDGE PROBE. FE (FE_Client+0x1D8A03) calls this object's vtable
    // slot 7 right after creating it and BLOCKS there -- that method runs a message
    // loop that never returns, so FE never renders. Log the vtable slots as
    // FriendList.dll+RVA so slot 7 can be disassembled in the memimg. Read-only.
    if (IsEqualGUID(rclsid, kFriendListCom) && SUCCEEDED(hr) && out && *out) {
        void** vt = *(void***)*out;
        HMODULE flm = GetModuleHandleA("FriendList.dll");
        logf("[fl] FriendListCom created %p, vtable %p, FriendList.dll base %p", *out, (void*)vt, (void*)flm);
        for (int s = 0; s <= 10; s++) {
            void* fn = (!IsBadReadPtr(vt, (s + 1) * sizeof(void*))) ? vt[s] : NULL;
            if (fn && flm && (BYTE*)fn >= (BYTE*)flm)
                logf("[fl]   vtable[%d] = %p = FriendList.dll+0x%IX%s", s, fn,
                     (SIZE_T)((BYTE*)fn - (BYTE*)flm), s == 7 ? "  <-- FE BLOCKS HERE" : "");
            else
                logf("[fl]   vtable[%d] = %p (not in FriendList.dll)", s, fn);
        }
        // Hook slot 7 so we log ENTER *and* EXIT of the method FE blocks in. If we
        // only ever see ENTER, sub_1005f0d0 never returns (it IS the wedge). If we
        // see EXIT too, slot 7 returns and FE blocks somewhere after -- redirects
        // the hunt out of FriendList.dll entirely. Patch once (shared vtable).
        if (!g_fl_slot7_real && !IsBadReadPtr(vt, 8 * sizeof(void*)) && vt[7]) {
            DWORD oldp = 0;
            if (VirtualProtect(&vt[7], sizeof(void*), PAGE_READWRITE, &oldp)) {
                g_fl_slot7_real = (PFN_FLSlot7)vt[7];
                vt[7] = (void*)&hook_fl_slot7;
                VirtualProtect(&vt[7], sizeof(void*), oldp, &oldp);
                logf("[fl] slot7 HOOKED: real=%p -> wrapper=%p", (void*)g_fl_slot7_real, (void*)&hook_fl_slot7);
            } else {
                logf("[fl] slot7 hook FAILED: VirtualProtect err=%lu", GetLastError());
            }
        }
        // Install the STA-deadlock breaker before FE calls slot 7 (the FL's runtime
        // import table is resolved by now). No-op unless [polshim] fl_slot7_pump=1.
        if (g_fl_pump && flm) { fl_install_wfso_pump(flm); fl_install_setevent_watch(flm); fl_install_winsock_probe(flm); }
        log_flush();
    }

    // Only ever on the failure path, and only for the one interface pol.exe
    // hardcodes -- a successful activation is never touched.
    if (g_iid_alias && FAILED(hr) && out &&
        IsEqualGUID(riid, IID_IPolContentsCom)) {
        GUID alt;
        char where[160];
        if (contents_iid_for(rclsid, &alt, where, sizeof(where)) &&
            !IsEqualGUID(alt, riid)) {
            HRESULT hr2 = real_CoCreateInstance(rclsid, outer, ctx, alt, out);
            char cs[64], as[64];
            guid_str(rclsid, cs, sizeof(cs));
            guid_str(alt, as, sizeof(as));
            if (SUCCEEDED(hr2)) {
                // Do NOT assert the vtables match -- this fires for any title in
                // ContentsIID, and the claim has only been VERIFIED for FMO
                // (IFMOEntry, typelib-confirmed: one method, GameStart, slot 3).
                // Fantasy Earth's typelib declares cbSizeVft=12, i.e. IUnknown
                // and NO methods at all, so on the typelib alone slot 3 -- the
                // slot pol.exe calls at 0x4142bb -- would be off the end of the
                // vtable. It survives in practice (the client calls it and stays
                // alive), so that typelib is merely incomplete; but an untested
                // third title could genuinely differ, and this log line must not
                // tell a future reader the compatibility was checked when it was
                // not.
                logf("[com] ALIAS %s: asked for IPolContentsCom (hr=0x%08lX), "
                     "retried with %s from %s -> OK. ASSUMES that interface puts "
                     "GameStart at vtable slot 3, which is what the caller will "
                     "now call; verified for FMO, empirical for others.",
                     cs, (unsigned long)hr, as, where);
                log_flush();
                return hr2;
            }
            logf("[com] ALIAS %s: retry with %s from %s ALSO failed "
                 "hr=0x%08lX -- alias not applied", cs, as, where,
                 (unsigned long)hr2);
        }
    }

    // CLSID_FilterGraph: hand the graph to vidfit, which places the title's FMV
    // through the graph's own interfaces. This is the ONLY way to reach a
    // windowless renderer (no window exists to enumerate) and the only exact
    // source for the movie's true dimensions. Measured: FE_Client.dll+0x33CE7
    // and FFXiMain.dll+0x1C3F28 both arrive here asking for IID_IGraphBuilder.
    if (SUCCEEDED(hr) && out && *out && vidfit_wants_graphs()) {
        static const GUID kFilterGraph =
            { 0xE436EBB3, 0x524F, 0x11CE, { 0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };
        if (IsEqualGUID(rclsid, kFilterGraph)) vidfit_note_graph(*out);
    }

    // CLSID_FilterGraph, the SKIP side: if [dx] fmv_skip is on, replace the graph
    // the title receives with a wrapper that skips (or traces) its movie
    // RenderFile -- FE and FMO die in Wine's DirectShow decode of their opening
    // .avi. This runs AFTER vidfit_note_graph above, so vidfit keeps the REAL
    // graph and only the TITLE gets the wrapper. See fmvskip.cpp.
    // WARNING: THE GATE IS PER CALLER, AND IT IS TESTED INSIDE THE CLSID CHECK.
    //
    // It used to be `fmvskip_enabled()` out here -- the process-wide level, which on
    // Windows is still the no-title 0 at the moment a title builds its opening-movie
    // graph, because that happens BEFORE the CreateDevice that sets the per-title
    // scope. So the wrap never armed and the movie played. Asking per caller answers
    // for the title that is actually building the graph, whenever it does it.
    if (SUCCEEDED(hr) && out && *out) {
        static const GUID kFilterGraphSkip =
            { 0xE436EBB3, 0x524F, 0x11CE, { 0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };
        int fmv_level = IsEqualGUID(rclsid, kFilterGraphSkip)
                      ? fmvskip_level_for_caller(_ReturnAddress()) : 0;
        if (fmv_level > 0) {
            char who[128]; caller_desc(_ReturnAddress(), who, sizeof(who));
            // IMPORTANT: Pass the level in. fmvskip_wrap used to re-read the process-wide
            // g_level, which is 0 on Windows at this moment (no title boundary yet),
            // so the per-caller answer computed one line above was never applied.
            HRESULT fhr = fmvskip_wrap(riid, out, who, fmv_level);
            if (FAILED(fhr)) {
                // Level 4 refused the graph: propagate the failure to the title so
                // it takes its own no-movie path. record() logs the call, then we
                // return the substituted HRESULT instead of the real S_OK.
                record("CoCreateInstance", rclsid, &riid, ctx, fhr, _ReturnAddress());
                return fhr;
            }
        }
    }

    // CLSID_VideoMixingRenderer9: FMO creates its FMV's renderer here, by CLSID, before
    // the graph is connected -- the one moment its IVMRFilterConfig9 can be reached before
    // FMO's own SetRenderingMode(WINDOWLESS) call. Hand it to vmrfix, which patches that
    // one call so the movie stops racing the game's surface. See vmrfix.cpp.
    if (SUCCEEDED(hr) && out && *out && vmrfix_enabled()) {
        static const GUID kVMR9 =
            { 0x51B4ABF3, 0x748F, 0x4E3B, { 0xA2, 0x76, 0xC8, 0x28, 0x33, 0x0E, 0x92, 0x6A } };
        if (IsEqualGUID(rclsid, kVMR9)) vmrfix_note_vmr(*out);
    }

    record("CoCreateInstance", rclsid, &riid, ctx, hr, _ReturnAddress());
    return hr;
}

static HRESULT WINAPI hook_CoCreateInstanceEx(REFCLSID rclsid, LPUNKNOWN outer,
                                              DWORD ctx, COSERVERINFO* si,
                                              DWORD cmq, MULTI_QI* mqi)
{
    HRESULT hr = real_CoCreateInstanceEx(rclsid, outer, ctx, si, cmq, mqi);
    record("CoCreateInstanceEx", rclsid,
           (cmq && mqi) ? mqi[0].pIID : NULL, ctx, hr, _ReturnAddress());
    return hr;
}

static HRESULT WINAPI hook_CoGetClassObject(REFCLSID rclsid, DWORD ctx,
                                            COSERVERINFO* si, REFIID riid,
                                            LPVOID* out)
{
    InterlockedIncrement(&g_entered_cgco);
    HRESULT hr = real_CoGetClassObject(rclsid, ctx, si, riid, out);
    record("CoGetClassObject", rclsid, &riid, ctx, hr, _ReturnAddress());
    return hr;
}

// Worth hooking even though it returns no object: CLSIDFromProgID is the OTHER
// producer of REGDB_E_CLASSNOTREG, and it fails on a STRING rather than a GUID.
// If FMO's failure comes through here, the log names the ProgID -- which no
// CLSID-based trace could ever recover.
static HRESULT WINAPI hook_CLSIDFromProgID(LPCOLESTR progid, LPCLSID out)
{
    HRESULT hr = real_CLSIDFromProgID(progid, out);
    if (g_comtrace && FAILED(hr)) {
        InterlockedIncrement(&g_fails);
        char nm[128] = {0};
        if (progid)
            WideCharToMultiByte(CP_ACP, 0, progid, -1, nm, sizeof(nm) - 1, NULL, NULL);
        logf("[com] CLSIDFromProgID(\"%s\") -> FAILED hr=0x%08lX%s",
             progid ? nm : "(null)", (unsigned long)hr,
             ((unsigned long)hr == 0x80040154UL) ? " REGDB_E_CLASSNOTREG" : "");
        log_flush();
    }
    return hr;
}

// --- wiring ------------------------------------------------------------------

void com_configure(const wchar_t* ini)
{
    if (!ini) return;
    g_comtrace = GetPrivateProfileIntW(L"polshim", L"comtrace", 1, ini);
    g_iid_alias = GetPrivateProfileIntW(L"polshim", L"contentiid_alias", 1, ini);
    g_block_ingame_fl = GetPrivateProfileIntW(L"polshim", L"block_ingame_friendlist", 0, ini);
    // KEY: THE STUB IS NOT A FIX AND MUST NOT BE A DEFAULT. fl_slot7_stub=1 hands FE
    // S_OK for a Friend List that was never created, so FE waits forever on a
    // singleton that does not exist -- that is the "initializing network" hang, and
    // it was the shipped state on the Deck until 2026-08-27. 0 is the only sane
    // default; the stub stays available purely as an experiment lever.
    g_fl_slot7_stub = GetPrivateProfileIntW(L"polshim", L"fl_slot7_stub", 0, ini);
    g_fl_slot7_async = GetPrivateProfileIntW(L"polshim", L"fl_slot7_async", 0, ini);

    // KEY: THE PUMP DEFAULTS **ON UNDER WINE**, because that is where the deadlock is.
    //
    // FriendList.dll's slot-7 init blocks on WaitForSingleObject on FE's own STA
    // thread, waiting on an event that only arrives once a message reaches the
    // WH_GETMESSAGE hook it just installed -- but FE's pump is frozen inside that
    // very call. It imports WaitForSingleObject and NO GetMessage/MsgWait/CoWait, so
    // it never pumps itself; the standalone Friend List app works precisely because
    // IT pumps. Leaving this off means the in-game FL wedges on every Wine install,
    // which is why a stub got switched on to paper over it and produced a different
    // hang instead. Pumping during an STA blocking wait is what a correct STA wait
    // does, so this is the right behaviour, not a workaround.
    //
    // Off by default on Windows: the FL works there, and this replaces a plain wait
    // with a MsgWaitForMultipleObjects loop -- a real behaviour change that Windows
    // has no reason to take. An explicit ini value still wins on either platform.
    //
    // (Wine probe duplicated rather than shared through polshim.h -- same reasoning
    // as fmvskip.cpp: a two-line probe is not worth a shared-header conflict.)
    HMODULE nt_probe = GetModuleHandleW(L"ntdll.dll");
    const bool under_wine = nt_probe && GetProcAddress(nt_probe, "wine_get_version") != NULL;
    g_fl_pump = GetPrivateProfileIntW(L"polshim", L"fl_slot7_pump", under_wine ? 1 : 0, ini);
    g_fl_grant_ms = GetPrivateProfileIntW(L"polshim", L"fl_wfso_grant_ms", 0, ini);
}

// Live re-read for the in-game settings dialog -- ONLY the two stable trace
// flags, both consulted per call by hooks that are already in place; the other
// [polshim] keys configure reads are startup/experiment gates and are left
// alone. Turning comtrace ON is restart-bound the other way: if it was 0 when
// com_resolve ran, the real_* pointers stayed NULL, the ole32 IAT was never
// patched, and there is nothing for g_comtrace=1 to switch on. OFF works live.
void com_reload(const wchar_t* ini)
{
    if (!ini) return;
    g_comtrace = GetPrivateProfileIntW(L"polshim", L"comtrace", 1, ini);
    g_iid_alias = GetPrivateProfileIntW(L"polshim", L"contentiid_alias", 1, ini);
    logf("[reload] comtrace: comtrace=%d contentiid_alias=%d",
         g_comtrace, g_iid_alias);
}

void com_resolve()
{
    if (!g_comtrace) return;     // NULL `from` never matches in patch_iat
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    if (!ole) { logf("[com] ole32.dll would not load -- comtrace disabled");
                g_comtrace = 0; return; }
    real_CoCreateInstance   = (PFN_CCI)  GetProcAddress(ole, "CoCreateInstance");
    real_CoCreateInstanceEx = (PFN_CCIEX)GetProcAddress(ole, "CoCreateInstanceEx");
    real_CoGetClassObject   = (PFN_CGCO) GetProcAddress(ole, "CoGetClassObject");
    real_CLSIDFromProgID    = (PFN_CFPI) GetProcAddress(ole, "CLSIDFromProgID");
}

void* com_real_CoCreateInstance()   { return (void*)real_CoCreateInstance; }
void* com_hook_CoCreateInstance()   { return (void*)hook_CoCreateInstance; }
void* com_real_CoCreateInstanceEx() { return (void*)real_CoCreateInstanceEx; }
void* com_hook_CoCreateInstanceEx() { return (void*)hook_CoCreateInstanceEx; }
void* com_real_CoGetClassObject()   { return (void*)real_CoGetClassObject; }
void* com_hook_CoGetClassObject()   { return (void*)hook_CoGetClassObject; }
void* com_real_CLSIDFromProgID()    { return (void*)real_CLSIDFromProgID; }
void* com_hook_CLSIDFromProgID()    { return (void*)hook_CLSIDFromProgID; }

void com_summary()
{
    if (!g_comtrace) return;
    // Says plainly whether the byte patches decayed during the session -- this
    // hook is where they are kept alive, so the count belongs next to it.
    patches_summary();
    logf("[com] summary: %ld activation(s), %ld failure(s); hook entries: "
         "CoCreateInstance=%ld CoGetClassObject=%ld",
         g_calls, g_fails, g_entered_cci, g_entered_cgco);
    if (g_calls == 0)
        logf("[com] NOTE: zero activations seen. Either the client did no COM "
             "in this run, or it resolves ole32 dynamically and the IAT patch "
             "missed it -- do NOT read this as 'no COM happened'.");

    // Why these counters exist (2026-08-12). The CoCreateInstance hook WORKS:
    // dxtest.exe records 19 entries, every activation DirectSound makes
    // internally, hooked through dsound.dll's IAT. But in comtest.exe the same
    // hook is never entered even though its IAT slot demonstrably holds our
    // address (read back at runtime), while CoGetClassObject in that very module
    // is entered fine. So a module calling ole32 through its OWN import can
    // apparently bypass the swapped slot, and which modules do that is not
    // established.
    //
    // The consequence for reading a log: absence of [com] CoCreateInstance lines
    // is NOT evidence the client made no such call. These counters are the only
    // way to tell "not called" from "not covered", so check them before drawing
    // any conclusion, and fall back to ProcMon (RegOpenKey + path contains CLSID
    // + result NAME NOT FOUND) when coverage is zero.
    if (g_entered_cci == 0 && g_entered_cgco > 0)
        logf("[com] WARNING: CoGetClassObject was entered but CoCreateInstance "
             "never was, so CoCreateInstance activations may be UNCOVERED this "
             "run. Absence of its lines is not evidence it was not called.");
}
