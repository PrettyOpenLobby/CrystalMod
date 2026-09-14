// uitrace.cpp -- name the Viewer's error screens instead of inferring them.
//
// WHY THIS EXISTS
//
// 2026-08-18: Fantasy Earth and FMO both ended a launch attempt with an on-screen
// "An error has occurred. Now exiting the PlayOnline Viewer." and the shim log said
// NOTHING about it. Everything the log did say was a negative:
//
//     [com] summary: 34 activation(s), 0 failure(s)
//     [d3d] NOTE: CreateDevice never ran -- no game was launched this session.
//     [pf]  NOTE: START never ran -- no title used this fetch this session.
//
// -- i.e. the title was never even activated, the Viewer simply gave up. Two separate
// diagnoses were then reasoned out from that absence and BOTH were wrong. The problem is
// structural: the shim can see COM, DirectX, sockets and the registry, and the one thing
// it cannot see is the Viewer's own opinion, which is exactly what an error screen is.
//
// `[mx]` (msgxlate.cpp) already hooks MessageBoxA/W and caught nothing, because these
// screens are NOT MessageBoxes -- they are drawn by the Viewer's own UI off its string
// table. So capture them at the source.
//
// HOW -- app.dll quotes StringTable ids as plain immediates
//
// app.dll never embeds screen text. It calls a one-argument thunk with the string id and
// gets a pointer to the text back ([[viewer-string-id-xref]]). Measured in the runtime
// dump (app.dll, BASE 0x04850000):
//
//     04ab62f9  ff 74 24 04   push dword ptr [esp+4]      <- RVA 0x2662F9, the thunk
//     04ab62fd  e8 22 6c db ff  call 0x486cf24            <- RVA 0x1CF24, the real getter
//     04ab6302  59            pop  ecx
//     04ab6303  c3            ret
//
//     0486cf24  mov edx,[esp+4] / cmp edx,[0x4cddd88] / jle ...
//               not-found -> mov eax, 0x4b6da84 (the fallback string)
//               found     -> eax = record+4
//
// The table's records are `[u16 len][u16 id][UTF-16LE text]`,
// so the getter's return value is a `const wchar_t*` and we can log the TEXT, not just a
// number somebody then has to look up.
//
// IMPORTANT: HOOK THE GETTER, NOT THE THUNK -- MEASURED, after the thunk version logged NOTHING.
// The obvious cheap hook is to rewrite the 4-byte rel32 operand of the thunk's `call`:
// no trampoline, no prologue copy, no length decoding. It armed correctly against the
// real installed app.dll and then recorded ZERO lookups in a whole session, because
// app.dll's screen code overwhelmingly calls the getter DIRECTLY -- the thunk is one
// caller of it, not the funnel. So we detour 0x1CF24 itself and the thunk comes along
// for free, since it too ends up there.
//
// WARNING: BYTES 4..9 CARRY A RELOCATED ABSOLUTE ADDRESS. The prologue is
//     8B 54 24 04           mov edx,[esp+4]        (4 bytes, base-independent)
//     3B 15 <abs32>         cmp edx,[0x4cddd88]    (6 bytes, the operand is RELOCATED)
// so a byte-for-byte compare against the dump would FAIL on any install whose app.dll
// did not land at 0x04850000. Fingerprint only the base-independent bytes (the mov, and
// the cmp's opcode+modrm) and copy the RUNTIME bytes into the trampoline -- never the
// dump's. The relocated absolute stays correct when moved: x86-32 has no RIP-relative
// addressing, so an absolute operand means the same thing at any address.
//
// 10 bytes is two whole instructions and >= the 5 a jmp needs, and the function's own
// branches (+0x0C, +0x12) both land PAST them, so nothing jumps into the middle of what
// we overwrite.
//
// COST. The hook keeps a RING and does no I/O, so it is free to leave on: the answer to
// "what did the Viewer say before it quit" is the last few entries, and those are dumped
// once, at exit, next to the other summaries. `[uitrace] live=1` additionally logs every
// lookup as it happens -- for the case where the Viewer does NOT exit cleanly and the
// summary never runs.

#include "polshim.h"
#include <intrin.h>     // _ReturnAddress -- names the module+RVA that decided to exit
#include <tlhelp32.h>   // thread snapshot -- freeze peers across the getter detour write

static int  g_on   = 1;
static int  g_live = 0;
static int  g_hooked = 0;

// The build these offsets were measured against. Checked before anything is written --
// a different app.dll must be left strictly alone rather than patched on faith.
static const DWORD RVA_GETTER = 0x1CF24;
static const int   STEAL      = 10;         // mov edx,[esp+4] (4) + cmp edx,[abs32] (6)

typedef const wchar_t* (__cdecl *PFN_GETSTR)(int id);
static PFN_GETSTR g_orig;                   // -> the trampoline, not app.dll

// A ring, because the interesting part is always "the last few before it stopped".
// 64 x ~120B is nothing, and it costs no log volume until something asks for it.
#define UITRACE_RING 64
#define UITRACE_TEXT 100
struct Entry { int id; wchar_t text[UITRACE_TEXT]; };
static Entry        g_ring[UITRACE_RING];
static volatile LONG g_seq;                  // total lookups; index = (seq-1) % RING

// SEH, and not defensiveness for its own sake: `eax` on the not-found path is a fallback
// constant inside app.dll, and on a build whose offsets we guessed wrong it could be
// anything at all. Reading it must never be able to take the Viewer down.
static void copy_text(const wchar_t* src, wchar_t* dst, size_t cch)
{
    dst[0] = 0;
    __try {
        size_t i = 0;
        for (; i < cch - 1 && src[i]; i++) {
            wchar_t c = src[i];
            // Newlines would break one entry across log lines and make the ring
            // unreadable; the Viewer's messages are full of \r\n.
            dst[i] = (c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c;
        }
        dst[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wcscpy_s(dst, cch, L"<unreadable>");
    }
}

static const wchar_t* __cdecl hook_getstr(int id)
{
    const wchar_t* s = g_orig(id);

    LONG n = InterlockedIncrement(&g_seq);
    Entry* e = &g_ring[(n - 1) % UITRACE_RING];
    e->id = id;
    copy_text(s, e->text, UITRACE_TEXT);

    if (g_live) logf("[ui] %d  \"%S\"", id, e->text);
    return s;
}

// Dump newest LAST, so the final line is the last thing the Viewer said -- which is the
// line somebody reading a failure report actually wants, and putting it at the bottom
// means they do not have to know the ring's direction to find it.
void uitrace_dump(const char* why)
{
    if (!g_hooked) return;
    LONG n = g_seq;
    if (n <= 0) { logf("[ui] %s: no string lookups seen", why); return; }

    int have = (int)(n < UITRACE_RING ? n : UITRACE_RING);
    logf("[ui] %s -- the last %d string(s) the Viewer resolved, oldest first. The screen "
         "it was showing when it stopped is at the BOTTOM:", why, have);
    for (int i = have; i >= 1; i--) {
        Entry* e = &g_ring[(n - i) % UITRACE_RING];
        logf("[ui]   %5d  \"%S\"", e->id, e->text);
    }
    logf("[ui] (%ld lookup(s) this session)", n);
}

// --- the exit path ----------------------------------------------------------------
//
// A clean exit runs DLL_PROCESS_DETACH and the summary dumps the ring there. A Viewer
// that terminates itself does not, and that is precisely the case where the log has been
// stopping mid-sentence -- so hook both exits, say WHO called them, and dump before
// letting them through. `_ReturnAddress` names the module+RVA, which is the difference
// between "it exited" and "app.dll+0x27xxxx decided to exit".
static void name_addr(void* ra, char* mod, size_t modsz, DWORD* rva)
{
    strcpy_s(mod, modsz, "?");
    *rva = 0;
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ra, &m) && m) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(m, full, sizeof(full))) {
            const char* leaf = strrchr(full, '\\');
            strcpy_s(mod, modsz, leaf ? leaf + 1 : full);
        }
        *rva = (DWORD)((BYTE*)ra - (BYTE*)m);
    }
}

// A title that STATICALLY LINKS the MSVC CRT (FE_Client.dll is exactly this) funnels
// abort() / _exit() / _invalid_parameter() / terminate() / __report_gsfailure through one
// __crtExitProcess, so the immediate return address of ExitProcess/TerminateProcess is that
// CRT stub, NOT the code that decided to die (measured 2026-08-19: FE_Client's only
// TerminateProcess/ExitProcess call sites are __crtExitProcess at FE_Client+0x2CB86B /
// +0x2CB8E6). Walk a few frames up the raw stack and name every return address that lands
// inside a loaded module; the caller chain past the CRT is the signal. There is no unwind
// info in an FPO CRT, so this is a heuristic scan: a stack dword is a probable return
// address when the bytes just before it decode as a `call` (E8 rel32, or FF /2 in its
// common encodings). SEH-guarded throughout because it reads unqualified stack and code.
static bool looks_like_call_before(BYTE* p)
{
    __try {
        if (p[-5] == 0xE8) return true;                                 // call rel32
        if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return true;      // call [mem]/[r+disp32]
        if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return true;      // call [r+disp8]
        if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return true;      // call r / call [r]
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void log_exit_backtrace(void* start_esp)
{
    void** sp = (void**)start_esp;
    int shown = 0;
    logf("[ui]   caller chain (probable return addresses, nearest first -- the frame ABOVE "
         "the CRT's __crtExitProcess is the one that decided to quit):");
    for (int i = 0; i < 1024 && shown < 24; i++) {
        void* ra;
        __try { ra = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        HMODULE m = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)ra, &m) || !m)
            continue;
        if (!looks_like_call_before((BYTE*)ra)) continue;
        char mod[MAX_PATH]; DWORD rva;
        name_addr(ra, mod, sizeof(mod), &rva);
        logf("[ui]     %s+0x%05lX", mod, rva);
        shown++;
    }
    if (!shown) logf("[ui]     (no plausible frames recovered)");
}

static void report_exit(const char* api, UINT code, void* ra)
{
    char mod[MAX_PATH]; DWORD rva;
    name_addr(ra, mod, sizeof(mod), &rva);
    logf("[ui] %s(code=%u) called from %s+0x%05lX", api, code, mod, rva);
    log_exit_backtrace(_AddressOfReturnAddress());
    uitrace_dump("the Viewer is terminating");
    log_flush();
}

typedef void (WINAPI *PFN_EXITPROCESS)(UINT);
typedef BOOL (WINAPI *PFN_TERMPROCESS)(HANDLE, UINT);
typedef void (WINAPI *PFN_FATALEXIT)(UINT, LPCSTR);
static PFN_EXITPROCESS g_orig_exit;
static PFN_TERMPROCESS g_orig_term;
static PFN_FATALEXIT   g_orig_fatalexit;

static void WINAPI hook_ExitProcess(UINT code)
{
    report_exit("ExitProcess", code, _ReturnAddress());
    g_orig_exit(code);
}

static BOOL WINAPI hook_TerminateProcess(HANDLE h, UINT code)
{
    // Only OUR process: the Viewer legitimately terminates children.
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId())
        report_exit("TerminateProcess", code, _ReturnAddress());
    return g_orig_term(h, code);
}

static void WINAPI hook_FatalAppExitA(UINT uAction, LPCSTR msg)
{
    logf("[ui] FatalAppExitA(\"%s\")", msg ? msg : "(null)");
    report_exit("FatalAppExitA", 0, _ReturnAddress());
    if (g_orig_fatalexit) g_orig_fatalexit(uAction, msg);
}

void uitrace_config(const wchar_t* ini)
{
    g_on   = GetPrivateProfileIntW(L"uitrace", L"enable", 1, ini);
    // DELIBERATELY ON, and this is a considered default rather than a leftover debug
    // switch. The ring dumped at exit covers a clean shutdown, and the
    // ExitProcess/TerminateProcess hooks cover a self-terminating one -- but a Viewer
    // killed from OUTSIDE runs neither, and every round trip to the machine that shows
    // this failure costs a person a relaunch. ~500 lines a session (measured on a login
    // screen) against a 2 MB logship cap is a trade worth making until the launch failure
    // these were written for is named. Turn it off with [uitrace] live=0.
    g_live = GetPrivateProfileIntW(L"uitrace", L"live", 1, ini);
}

// Called from the same place patches_apply_module is, i.e. once app.dll is present.
void uitrace_apply_module(void* module_base, const char* module)
{
    if (!g_on || g_hooked || !module_base || !module) return;
    if (_stricmp(module, "app.dll") != 0) return;

    BYTE* base = (BYTE*)module_base;
    BYTE* fn   = base + RVA_GETTER;

    // Fingerprint ONLY the base-independent bytes: the whole `mov edx,[esp+4]`, and the
    // `cmp edx,[abs32]` opcode+modrm. The 4 address bytes after them are relocated per
    // load and comparing them would reject every install that is not based at 0x04850000.
    if (!(fn[0] == 0x8B && fn[1] == 0x54 && fn[2] == 0x24 && fn[3] == 0x04 &&
          fn[4] == 0x3B && fn[5] == 0x15)) {
        char got[3 * 6 + 1] = "";
        for (int k = 0; k < 6; k++) sprintf(got + strlen(got), "%02X ", fn[k]);
        logf("[ui] app.dll+0x%05lX is [%s], expected [8B 54 24 04 3B 15] -- NOT hooking "
             "(different build?). Error screens will not be named.", RVA_GETTER, got);
        return;
    }

    // Trampoline: the RUNTIME prologue bytes (relocations already applied) plus a jump
    // back past what we are about to overwrite.
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { logf("[ui] trampoline alloc failed err=%lu", GetLastError()); return; }
    memcpy(tr, fn, STEAL);
    tr[STEAL] = 0xE9;
    *(LONG*)(tr + STEAL + 1) = (LONG)((fn + STEAL) - (tr + STEAL + 5));
    FlushInstructionCache(GetCurrentProcess(), tr, 32);
    g_orig = (PFN_GETSTR)tr;

    DWORD old;
    if (!VirtualProtect(fn, STEAL, PAGE_EXECUTE_READWRITE, &old)) {
        logf("[ui] VirtualProtect failed at %p err=%lu", fn, GetLastError());
        VirtualFree(tr, 0, MEM_RELEASE);
        return;
    }

    // Close the code-patch race. The getter is a hot UI function, so another thread
    // can be executing inside [fn, fn+STEAL) at the instant we rewrite it, and a
    // partial write (the E9 opcode landing before the rel32/NOP tail) is a jump to a
    // garbage address -- an access violation in pol.exe. This runs from app.dll's
    // first DllGetClassObject on a Viewer COM thread, NOT under the loader lock, so it
    // is safe to freeze every other thread across the ~10-byte write. If any frozen
    // thread's EIP is inside the region we abort rather than resume it onto half-written
    // bytes. NOTHING that takes a lock (logf, VirtualProtect) runs while peers are
    // frozen -- a suspended thread could hold that lock.
    HANDLE frozen[256];
    int    nfrozen = 0;
    bool   busy    = false;
    {
        DWORD  me   = GetCurrentThreadId();
        DWORD  pid  = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te = { sizeof(te) };
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                    if (nfrozen >= (int)_countof(frozen)) break;
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                          FALSE, te.th32ThreadID);
                    if (!h) continue;
                    if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
                    frozen[nfrozen++] = h;
                    CONTEXT cx; cx.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(h, &cx) &&
                        cx.Eip >= (DWORD)(UINT_PTR)fn &&
                        cx.Eip <  (DWORD)(UINT_PTR)(fn + STEAL))
                        busy = true;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
    }

    if (busy) {
        // A thread is mid-getter; do not rewrite under its feet. Resume, undo the
        // page-protection change, and try again on a later module pass.
        for (int i = 0; i < nfrozen; i++) { ResumeThread(frozen[i]); CloseHandle(frozen[i]); }
        VirtualProtect(fn, STEAL, old, &old);
        VirtualFree(tr, 0, MEM_RELEASE);
        logf("[ui] getter busy (a thread was mid-lookup) -- not hooking this attempt");
        return;
    }

    fn[0] = 0xE9;
    *(LONG*)(fn + 1) = (LONG)((BYTE*)hook_getstr - (fn + 5));
    memset(fn + 5, 0x90, STEAL - 5);        // pad the rest of the stolen bytes
    FlushInstructionCache(GetCurrentProcess(), fn, STEAL);

    // Peers see the fully written detour only now, on resume. Restore the page
    // protection AFTER resuming so VirtualProtect never runs inside the frozen window.
    for (int i = 0; i < nfrozen; i++) { ResumeThread(frozen[i]); CloseHandle(frozen[i]); }
    VirtualProtect(fn, STEAL, old, &old);

    if (fn[0] != 0xE9) {
        logf("[ui] detour write did not stick at %p -- not hooked", fn);
        VirtualFree(tr, 0, MEM_RELEASE);
        return;
    }
    g_hooked = 1;
    logf("[ui] string trace armed (detoured app.dll+0x%05lX, trampoline %p); every Viewer "
         "screen's text is now recorded, and the last %d are dumped at exit%s",
         RVA_GETTER, tr, UITRACE_RING,
         g_live ? " (live=1: also logged as they happen)" : "");

    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (k) {
        g_orig_exit      = (PFN_EXITPROCESS)GetProcAddress(k, "ExitProcess");
        g_orig_term      = (PFN_TERMPROCESS)GetProcAddress(k, "TerminateProcess");
        g_orig_fatalexit = (PFN_FATALEXIT)  GetProcAddress(k, "FatalAppExitA");
        if (g_orig_exit)
            dx_eat_patch_export(k, "ExitProcess", (void*)hook_ExitProcess, "kernel32!ExitProcess");
        if (g_orig_term)
            dx_eat_patch_export(k, "TerminateProcess", (void*)hook_TerminateProcess,
                                "kernel32!TerminateProcess");
        if (g_orig_fatalexit)
            dx_eat_patch_export(k, "FatalAppExitA", (void*)hook_FatalAppExitA,
                                "kernel32!FatalAppExitA");
    }
    log_flush();
}

// Exposed so inject.cpp's patch_iat can swap a TITLE's own import slots by value. The EAT
// patch above only redirects modules that resolve these via GetProcAddress AFTER we patch;
// a title whose IAT was bound at load calls the real function directly. FE_Client.dll does
// exactly that -- which is why the ExitProcess/TerminateProcess hooks never fired for Fantasy
// Earth's GameStart crash (2026-08-19) and the log stopped mid-sentence with no crash dump.
// `real` returns NULL until uitrace has armed (app.dll present) or when [uitrace] enable=0,
// and patch_iat treats a NULL `from` as "no hook", so nothing is swapped in those cases.
void* uitrace_real_ExitProcess()      { return g_on ? (void*)g_orig_exit      : NULL; }
void* uitrace_hook_ExitProcess()      { return (g_on && g_orig_exit)      ? (void*)hook_ExitProcess      : NULL; }
void* uitrace_real_TerminateProcess() { return g_on ? (void*)g_orig_term      : NULL; }
void* uitrace_hook_TerminateProcess() { return (g_on && g_orig_term)      ? (void*)hook_TerminateProcess : NULL; }
void* uitrace_real_FatalAppExitA()    { return g_on ? (void*)g_orig_fatalexit : NULL; }
void* uitrace_hook_FatalAppExitA()    { return (g_on && g_orig_fatalexit) ? (void*)hook_FatalAppExitA    : NULL; }

void uitrace_summary()
{
    if (!g_on) return;
    if (!g_hooked) { logf("[ui] summary: never armed (app.dll not seen, or a different build)"); return; }
    uitrace_dump("session end");
}
