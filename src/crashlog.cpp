// crashlog.cpp -- when pol.exe dies, say WHERE, in our own log.
//
// WHY THIS EXISTS. Six pol.exe crashes landed in the Windows event log over three
// days in Aug 2026 and every one of them had to be read out of WER, which gives
// exactly this much:
//
//     Faulting module name: FE_Client.dll_unloaded ... Fault offset: 0x0007be30
//
// One address, no stack, no idea whether the shim was involved -- and two of those
// six named PolHook.dll as the faulting module, so "was it us" is not a rhetorical
// question. A shim that hooks this much of a process owes the person debugging it
// a straight answer, and the log is where they are already looking.
//
// WHAT IT DOES NOT DO. It does not handle the exception, swallow it, or try to
// continue: it logs and hands straight back to whatever was there before. A crash
// stays a crash. `[polshim] crashlog=0` removes it entirely.
//
// TWO DESIGN POINTS THAT MATTER IN A CRASH HANDLER:
//
// * NO ALLOCATION, no CRT locks we do not already hold, no toolhelp snapshot. The
//   heap may be what is broken. Everything here is a static buffer and three
//   kernel32 calls that read already-mapped memory.
// * AN UNLOADED MODULE IS THE INTERESTING CASE, not an error to skip.
//   `GetModuleHandleEx` failing on a code address means the DLL was unmapped and
//   something called into it anyway -- a use-after-unload, which is exactly what
//   WER was reporting as `_unloaded` and exactly what a hook chain holding a raw
//   function pointer produces. So a lookup miss is REPORTED, loudly, not dropped.
//
// The marker file is the other half: logship reads it at the next startup so a
// crashed session's log is the one that gets sent. See `[logship] when=crash`.
#include "polshim.h"
#include <tlhelp32.h>   // CreateToolhelp32Snapshot -- live thread dump (not the crash path)

void logf(const char* fmt, ...);
// WARNING: REQUIRED, not tidiness. logf buffers, and a crashing process is terminated by
// the OS without running the CRT's flush-at-exit -- so without this every crash
// report was written and then thrown away, which crashlogtest caught on its first
// run and which would have been indistinguishable in the field from having no
// crash handler at all. Flushed twice: once after the header, so a fault inside
// our own stack walk still leaves the part that matters most.
void log_flush();

static bool     g_cl_on = false;
static bool     g_cl_armed = false;   // the filter capture happened -- see crashlog_arm
static wchar_t  g_cl_marker[MAX_PATH];
static LPTOP_LEVEL_EXCEPTION_FILTER g_cl_prev = NULL;

// ---------------------------------------------------------------- address -> name
//
// Base and name of the module containing `p`, or false if no module does. The
// UNCHANGED_REFCOUNT flag is required: taking a reference from inside a crash
// handler could keep a module alive and change the very lifetime we are reporting.
static bool cl_module(const void* p, char* name, size_t cb, size_t* rva)
{
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &m) || !m)
        return false;
    char full[MAX_PATH];
    if (!GetModuleFileNameA(m, full, sizeof(full))) return false;
    const char* base = strrchr(full, '\\');
    strncpy_s(name, cb, base ? base + 1 : full, _TRUNCATE);
    *rva = (size_t)((const unsigned char*)p - (const unsigned char*)m);
    return true;
}

// Is this OUR module? The one question a reader of a shim crash asks first, so it
// gets answered per frame instead of left as an exercise in reading base addresses.
static bool cl_is_ours(const char* name)
{
    return _stricmp(name, "PolHook.dll")  == 0 ||
           _stricmp(name, "polinject.dll") == 0 ||
           _stricmp(name, "polshim.dll")  == 0;
}

static void cl_describe(const void* p, char* out, size_t cb)
{
    char name[64]; size_t rva = 0;
    if (cl_module(p, name, sizeof(name), &rva))
        _snprintf_s(out, cb, _TRUNCATE, "%s+0x%IX%s", name, rva,
                    cl_is_ours(name) ? "  <-- THE SHIM" : "");
    else
        // The loud case. Not "unknown": an address that is in no module at all,
        // on a stack of return addresses, means someone called into memory that
        // has been unmapped.
        _snprintf_s(out, cb, _TRUNCATE, "%p  <-- NOT IN ANY LOADED MODULE "
                    "(unmapped -- use-after-unload, or a wild call)", p);
}

static const char* cl_code_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
    case EXCEPTION_GUARD_PAGE:            return "GUARD_PAGE";
    case 0xC0000409:                      return "STACK_BUFFER_OVERRUN / __fastfail";
    case 0xC000041D:                      return "UNHANDLED EXCEPTION IN A CALLBACK";
    case 0xE06D7363:                      return "C++ exception (unhandled throw)";
    default:                              return "";
    }
}

// Readable-and-committed test. VirtualQuery rather than a __try around the read,
// because on a stack walk a bad guess is the common case, not the exception, and
// a fault per frame inside a crash handler is how a crash handler becomes the
// crash.
static bool cl_readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const unsigned char* end = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
    return (const unsigned char*)p + n <= end;
}

// ---------------------------------------------------------------- the walk
//
// EBP chain, not StackWalk64. dbghelp would need symbols we do not have for any
// of these binaries, and it is not safe to load a DLL from a crash handler. Every
// module in this process is 2000s-era x86 built with frame pointers, which is
// exactly the case the EBP chain gets right -- and where a frame is omitted the
// walk simply stops, which is honest. The raw stack scan below covers that.
static void cl_walk_ebp(const CONTEXT* c)
{
    const void** fp = (const void**)(size_t)c->Ebp;
    char d[256];
    for (int i = 0; i < 32; i++) {
        if (!cl_readable(fp, sizeof(void*) * 2)) {
            logf("[crash]   frame %-2d  chain ends (ebp %p not readable)", i, fp);
            return;
        }
        const void* ret  = fp[1];
        const void** next = (const void**)fp[0];
        if (!ret) return;
        cl_describe(ret, d, sizeof(d));
        logf("[crash]   frame %-2d  ret %p  %s", i, ret, d);
        // Forward progress only: a corrupt frame can point back at itself and
        // this would otherwise log 32 identical lines and tell us nothing.
        if (next <= fp) { logf("[crash]   frame %-2d  chain stops (ebp not ascending)", i); return; }
        fp = next;
    }
}

// The fallback that actually solved things by hand: every stack slot that looks
// like a code address, named. Noisier than the chain and immune to a missing
// frame pointer -- which is the case that matters when the crash is inside a
// hook thunk, since a thunk has no frame at all.
static void cl_scan_stack(const CONTEXT* c)
{
    const void** sp = (const void**)(size_t)c->Esp;
    int shown = 0;
    logf("[crash]   -- raw stack scan (code-looking slots; a missing frame "
         "pointer hides frames from the chain above) --");
    for (int i = 0; i < 512 && shown < 24; i++) {
        const void** slot = sp + i;
        if (!cl_readable(slot, sizeof(void*))) break;
        const void* v = *slot;
        char name[64]; size_t rva = 0;
        if (!v || !cl_module(v, name, sizeof(name), &rva)) continue;
        // Only executable pages: a data pointer into a module's .rdata is not a
        // return address and there are hundreds of them.
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(v, &mbi, sizeof(mbi))) continue;
        if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        logf("[crash]   esp+0x%03X  %p  %s+0x%IX%s", i * 4, v, name, rva,
             cl_is_ours(name) ? "  <-- THE SHIM" : "");
        shown++;
    }
}

// On-demand backtrace of the CURRENT thread, for a caller that is running ON the
// wedged thread (e.g. a WaitForSingleObject wrapper). This dodges the Wine
// cross-thread enumeration gap that hides a thread from toolhelp: we already ARE
// that thread, so RtlCaptureContext + the raw stack scan names the caller chain.
void crashlog_scan_current_stack(const char* why)
{
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    logf("[crash] current-thread stack scan (%s) tid=%lu eip=%p esp=%p:",
         why ? why : "", GetCurrentThreadId(), (void*)(size_t)ctx.Eip, (void*)(size_t)ctx.Esp);
    cl_scan_stack(&ctx);
    log_flush();
}

// ---------------------------------------------------------------- live thread dump
//
// NOT a crash path: this is an on-demand "what is every thread doing right now",
// for a title that RUNS but wedges (Fantasy Earth reaches its lobby, loads the
// in-game Friend List, then never presents a frame -- its main thread is waiting on
// something). Because the shim is IN the process it can walk the title's own threads
// with the same EBP chain the crash handler uses -- no external debugger, which
// matters on the Steam Deck where the Proton pressure-vessel hides the process from
// host gdb. It suspends each thread only long enough to snapshot its context.
//
// Unlike the crash handler this MAY allocate / snapshot (the heap is healthy here),
// so it uses toolhelp and resolves a set of known blocking calls so a stuck thread
// reads as "WAITING in NtWaitForSingleObject" instead of a bare ntdll+RVA.
static const char* cl_wait_name(const void* p)
{
    static const struct { const wchar_t* mod; const char* fn; } K[] = {
        { L"ntdll.dll",     "NtWaitForSingleObject" },
        { L"ntdll.dll",     "NtWaitForMultipleObjects" },
        { L"ntdll.dll",     "NtWaitForAlertByThreadId" },
        { L"ntdll.dll",     "NtDelayExecution" },
        { L"ntdll.dll",     "NtRemoveIoCompletion" },
        { L"win32u.dll",    "NtUserMsgWaitForMultipleObjectsEx" },
        { L"KERNELBASE.dll","WaitForSingleObjectEx" },
        { L"KERNELBASE.dll","WaitForMultipleObjectsEx" },
        { L"user32.dll",    "MsgWaitForMultipleObjectsEx" },
        { L"user32.dll",    "GetMessageW" },
        { L"user32.dll",    "GetMessageA" },
        { L"ole32.dll",     "CoWaitForMultipleHandles" },
        { L"combase.dll",   "CoWaitForMultipleHandles" },
        { L"ws2_32.dll",    "recv" },
        { L"ws2_32.dll",    "select" },
        { L"ws2_32.dll",    "WSARecv" },
    };
    for (int i = 0; i < (int)(sizeof(K)/sizeof(K[0])); i++) {
        HMODULE m = GetModuleHandleW(K[i].mod);
        if (!m) continue;
        FARPROC f = GetProcAddress(m, K[i].fn);
        if (!f) continue;
        if ((size_t)p >= (size_t)f && (size_t)p < (size_t)f + 0x200) return K[i].fn;
    }
    return NULL;
}

void crashlog_dump_all_threads(const char* why)
{
    logf("[tdump] ===== all-thread stack dump: %s =====", why ? why : "");
    DWORD me  = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { logf("[tdump] snapshot failed err=%lu", GetLastError()); return; }
    THREADENTRY32 te; te.dwSize = sizeof(te);
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD)) continue;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        SuspendThread(h);
        CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c)) {
            char d[256]; cl_describe((const void*)(size_t)c.Eip, d, sizeof(d));
            const char* w = cl_wait_name((const void*)(size_t)c.Eip);
            logf("[tdump] --- thread %lu  eip=%p  %s%s%s ---",
                 te.th32ThreadID, (void*)(size_t)c.Eip, d,
                 w ? "   *** WAITING in " : "", w ? w : "");
            if (w) logf("[tdump]     (blocked call: %s)", w);
            cl_walk_ebp(&c);
            // The EBP chain stops at Wine's syscall boundary and rarely reaches the
            // TITLE's own frames, so also raw-scan the stack for code-looking slots
            // -- that is what surfaces the FE_Client / FriendList return addresses
            // that name WHICH of the title's message loops this thread is stuck in.
            cl_scan_stack(&c);
        } else {
            logf("[tdump] --- thread %lu  GetThreadContext failed err=%lu ---",
                 te.th32ThreadID, GetLastError());
        }
        ResumeThread(h);
        CloseHandle(h);
        n++;
    }
    CloseHandle(snap);
    logf("[tdump] ===== %d thread(s) dumped =====", n);
    log_flush();
}

// ---------------------------------------------------------------- the marker
//
// One line, so the next launch can say "the previous session crashed" without
// parsing a log. logship reads and deletes it; nothing else depends on the format.
static void cl_write_marker(const char* summary)
{
    if (!g_cl_marker[0]) return;
    HANDLE f = CreateFileW(g_cl_marker, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, summary, (DWORD)strlen(summary), &wrote, NULL);
    CloseHandle(f);
}

static LONG WINAPI cl_filter(EXCEPTION_POINTERS* ep)
{
    // Reentrancy: if reporting the crash crashes, do not do it again -- hand
    // straight to the previous filter and let the process die normally.
    static LONG busy = 0;
    if (InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : NULL;
    const CONTEXT*          cx = ep ? ep->ContextRecord   : NULL;
    if (!er || !cx) {
        // Reset busy on THIS return too, not just the main path below: if a prior
        // filter ever resumed execution, leaving busy=1 would suppress every later
        // crash report for the process.
        InterlockedExchange(&busy, 0);
        return g_cl_prev ? g_cl_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    char at[256];
    cl_describe(er->ExceptionAddress, at, sizeof(at));

    logf("[crash] ============================================================");
    logf("[crash] pid %lu thread %lu  code 0x%08lX %s",
         (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(),
         (unsigned long)er->ExceptionCode, cl_code_name(er->ExceptionCode));
    logf("[crash] at %p  %s", er->ExceptionAddress, at);
    // For an AV, WHICH ACCESS and WHERE is most of the diagnosis: a read of a
    // small address is a null-deref, a read of a plausible one is a freed object.
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        static const char* kind[] = { "READ from", "WRITE to", "EXECUTE at" };
        ULONG_PTR op = er->ExceptionInformation[0];
        logf("[crash] %s %p", op <= 8 ? kind[op == 8 ? 2 : (op ? 1 : 0)] : "ACCESS",
             (void*)er->ExceptionInformation[1]);
    }
    logf("[crash] eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX",
         cx->Eax, cx->Ebx, cx->Ecx, cx->Edx);
    logf("[crash] esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX",
         cx->Esi, cx->Edi, cx->Ebp, cx->Esp, cx->Eip);
    log_flush();                 // the head is on disk before we walk anything
    cl_walk_ebp(cx);
    cl_scan_stack(cx);
    logf("[crash] ============================================================");
    log_flush();

    char summary[320];
    _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                "code=0x%08lX %s at=%s pid=%lu\n",
                (unsigned long)er->ExceptionCode, cl_code_name(er->ExceptionCode),
                at, (unsigned long)GetCurrentProcessId());
    cl_write_marker(summary);

    // NEVER handle it. A shim that turns a crash into a silent continue is worse
    // than the crash: the process limps on with corrupt state and the next
    // failure is somewhere unrelated.
    InterlockedExchange(&busy, 0);
    return g_cl_prev ? g_cl_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- first chance
//
// WHY. FRONT MISSION ONLINE on the Deck (2026-08-23): the only crash Wine ever
// reports is a SECONDARY fault inside FMO's own "Force quitting" handler. The
// title catches the PRIMARY exception with its own SEH filter
// (FrontMissionOnline+0x1600 copies the CONTEXT to +0x3AE388 and returns
// EXCEPTION_EXECUTE_HANDLER), then its master teardown AVs on subsystems that
// were never initialised -- and THAT is the only fault anything external sees.
// The primary, the abort REASON, is consumed and printed nowhere: the title
// builds its report but only logs it AFTER the teardown that dies.
//
// So: a vectored handler, first in the chain, which sees every exception
// before any SEH filter can eat it. It is an OBSERVER -- it always returns
// EXCEPTION_CONTINUE_SEARCH, never handles, never raises, so whoever was going
// to see the exception still sees it. One mask test drops everything below
// error severity: breakpoints, single-steps and guard pages (probes.cpp OWNS
// those) and OutputDebugString's DBG_PRINTEXCEPTION are all status/warning
// class. Caps keep a title that throws C++ exceptions as control flow from
// flooding the log: hardware faults get full register+stack detail for the
// first few, C++ throws one line each, and both go quiet at their cap.
static PVOID g_cl1_veh = NULL;
static bool  g_cl1_on  = false;
static LONG  g_cl1_hw  = 0;     // error-severity, non-C++ -- the interesting ones
static LONG  g_cl1_cxx = 0;     // 0xE06D7363 MSVC throws, often routine

#define CL1_HW_CAP   24
#define CL1_HW_FULL  4          // full register+stack detail for the first N
#define CL1_CXX_CAP  8

static LONG CALLBACK cl1_veh(EXCEPTION_POINTERS* ep)
{
    if (!g_cl1_on || !ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Error severity only (both top bits set): AV, illegal instruction, div0,
    // __fastfail, C++ throw. Everything the probes VEH consumes falls out here.
    if ((code & 0xC0000000) != 0xC0000000)
        return EXCEPTION_CONTINUE_SEARCH;

    // One reporter at a time, and a fault inside our own logging must not
    // recurse through us. Losing a concurrent second thread's line is fine.
    static LONG busy = 0;
    if (InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT*          cx = ep->ContextRecord;
    char at[256];

    if (code == 0xE06D7363) {
        LONG n = InterlockedIncrement(&g_cl1_cxx);
        if (n <= CL1_CXX_CAP) {
            cl_describe(er->ExceptionAddress, at, sizeof(at));
            logf("[crash1st] C++ throw #%ld at %p  %s%s", n, er->ExceptionAddress,
                 at, n == CL1_CXX_CAP ? "  (cap -- further throws not logged)" : "");
        }
    } else {
        // IMPORTANT: PER-SITE THROTTLE BEFORE THE GLOBAL CAP (2026-08-26). One benign,
        // SEH-handled AV that fires in a loop -- FMO's polcore fetch retries hit
        // KERNEL32+0x1CA4E 24+ times during startup -- used to consume the entire
        // CL1_HW_CAP, so the ONE interesting fault later in the session (the
        // minimise crash behind FMO's "Force quitting" box) went unlogged. A
        // repeating address logs 3 times, then stops counting against the cap;
        // a NEW address always has cap room left.
        static void* s_site[8]; static LONG s_site_n[8];
        void* eip = er->ExceptionAddress;
        int slot = -1;
        for (int i = 0; i < 8; i++) { if (s_site[i] == eip) { slot = i; break; } }
        if (slot < 0) for (int i = 0; i < 8; i++) {
            if (!s_site[i] && !InterlockedCompareExchangePointer(&((PVOID&)s_site[i]), eip, NULL)) { slot = i; break; }
        }
        if (slot >= 0 && InterlockedIncrement(&s_site_n[slot]) > 3) {
            InterlockedExchange(&busy, 0);
            return EXCEPTION_CONTINUE_SEARCH;   // seen 3x from this address -- quiet
        }
        LONG n = InterlockedIncrement(&g_cl1_hw);
        if (n <= CL1_HW_CAP) {
            cl_describe(er->ExceptionAddress, at, sizeof(at));
            logf("[crash1st] #%ld first-chance code 0x%08lX %s at %p  %s%s",
                 n, (unsigned long)code, cl_code_name(code), er->ExceptionAddress,
                 at, n == CL1_HW_CAP ? "  (cap -- further faults not logged)" : "");
            if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
                static const char* kind[] = { "READ from", "WRITE to", "EXECUTE at" };
                ULONG_PTR op = er->ExceptionInformation[0];
                logf("[crash1st]   %s %p",
                     op <= 8 ? kind[op == 8 ? 2 : (op ? 1 : 0)] : "ACCESS",
                     (void*)er->ExceptionInformation[1]);
            }
            if (n <= CL1_HW_FULL) {
                logf("[crash1st]   eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX",
                     cx->Eax, cx->Ebx, cx->Ecx, cx->Edx);
                logf("[crash1st]   esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX",
                     cx->Esi, cx->Edi, cx->Ebp, cx->Esp, cx->Eip);
                // FMO formats its error dialog ("%s\n[%s%05d]\n%s" -- message,
                // [FMO#####] code, detail) into the fixed global +0x3C0AE0
                // BEFORE the renderer measures it -- and on the Deck the
                // measure itself faulted on a not-yet-created font, eating the
                // code (2026-08-23). If the buffer holds text at fault time,
                // it names the error the dialog never got to show.
                {
                    HMODULE fmo = GetModuleHandleA("FrontMissionOnline.dll");
                    const char* eb = fmo ? (const char*)fmo + 0x3C0AE0 : NULL;
                    if (eb && cl_readable(eb, 512) && eb[0]) {
                        char line[512];
                        size_t o = 0;
                        for (size_t i = 0; i < 500 && eb[i]; i++)
                            line[o++] = (eb[i] == '\n' || eb[i] == '\r') ? '|'
                                      : (unsigned char)eb[i] < 0x20 ? '.' : eb[i];
                        line[o] = 0;
                        logf("[crash1st]   FMO error-dialog buffer: \"%s\"", line);
                    }
                }
                // Head on disk first: the title's own handler may take the
                // process down before we return.
                log_flush();
                cl_walk_ebp(cx);
                cl_scan_stack(cx);
            }
            log_flush();
        }
    }

    InterlockedExchange(&busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// `ini` is the ini path; the marker goes beside it, which is the install tree for
// the PolHook proxy and the build dir for the injector -- the same "beside its own
// DLL" rule every other file here follows.
void crashlog_arm(const wchar_t* ini)
{
    // Arm ONCE. A second SetUnhandledExceptionFilter here would read back our
    // own cl_filter as g_cl_prev, and the first real crash would then recurse
    // through itself instead of reaching whatever filter was there before us.
    if (g_cl_armed) return;

    g_cl_on = GetPrivateProfileIntW(L"polshim", L"crashlog", 1, ini) != 0;
    if (!g_cl_on) { logf("[crash] crashlog=0 -- no crash handler installed"); return; }

    wcsncpy_s(g_cl_marker, ini, _TRUNCATE);
    wchar_t* slash = wcsrchr(g_cl_marker, L'\\');
    if (slash) slash[1] = 0; else g_cl_marker[0] = 0;
    wcsncat_s(g_cl_marker, L"polshim-crash.txt", _TRUNCATE);

    g_cl_prev = SetUnhandledExceptionFilter(cl_filter);
    g_cl_armed = true;
    logf("[crash] armed -- a crash will be reported here, with module+RVA per "
         "frame; marker %ls", g_cl_marker);

    // The first-chance observer rides the same master switch; crashlog_first=0
    // turns just this half off.
    g_cl1_on = GetPrivateProfileIntW(L"polshim", L"crashlog_first", 1, ini) != 0;
    if (g_cl1_on) {
        g_cl1_veh = AddVectoredExceptionHandler(1, cl1_veh);
        logf("[crash] first-chance observer %s",
             g_cl1_veh ? "armed" : "FAILED to install");
        if (!g_cl1_veh) g_cl1_on = false;
    }
}

// Live re-read for the in-game settings dialog. The one deferred install a
// reload can honestly serve: crashlog=0 at startup meant crashlog_arm declined,
// so a flip to 1 arms for real here -- SetUnhandledExceptionFilter works at any
// time, not just at load. A flip to 0 leaves the filter IN PLACE: there is no
// unarm path, because handing g_cl_prev back would stomp any filter installed
// after ours, and an armed reporter that only logs a crash costs nothing.
void crashlog_reload(const wchar_t* ini)
{
    g_cl_on = GetPrivateProfileIntW(L"polshim", L"crashlog", 1, ini) != 0;
    if (g_cl_on && !g_cl_armed) crashlog_arm(ini);
    // First-chance flip. On->off just mutes the handler (g_cl1_on gates it at
    // entry); off->on installs it if crashlog_arm didn't -- but only when the
    // master filter is armed, matching the startup behavior.
    if (g_cl_armed) {
        g_cl1_on = GetPrivateProfileIntW(L"polshim", L"crashlog_first", 1, ini) != 0;
        if (g_cl1_on && !g_cl1_veh) {
            g_cl1_veh = AddVectoredExceptionHandler(1, cl1_veh);
            if (!g_cl1_veh) g_cl1_on = false;
        }
    }
    logf("[reload] crashlog: on=%d armed=%d first=%d%s", g_cl_on ? 1 : 0,
         g_cl_armed ? 1 : 0, g_cl1_on ? 1 : 0,
         (!g_cl_on && g_cl_armed) ? " (filter stays -- no unarm path)" : "");
}

// Did the PREVIOUS session crash? Reads the marker without deleting it, so the
// caller decides (logship deletes it once the log is away).
bool crashlog_prev_crash(const wchar_t* ini, char* summary, size_t cb)
{
    wchar_t path[MAX_PATH];
    wcsncpy_s(path, ini, _TRUNCATE);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0; else path[0] = 0;
    wcsncat_s(path, L"polshim-crash.txt", _TRUNCATE);

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    if (summary && cb) {
        ReadFile(f, summary, (DWORD)cb - 1, &got, NULL);
        summary[got] = 0;
        for (DWORD i = 0; i < got; i++)
            if (summary[i] == '\r' || summary[i] == '\n') { summary[i] = 0; break; }
    }
    CloseHandle(f);
    return true;
}

void crashlog_clear_marker(const wchar_t* ini)
{
    wchar_t path[MAX_PATH];
    wcsncpy_s(path, ini, _TRUNCATE);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0; else path[0] = 0;
    wcsncat_s(path, L"polshim-crash.txt", _TRUNCATE);
    DeleteFileW(path);
}
