// wndguard.cpp -- neutralise window procedures owned by a DLL that is unloading.
//
// WHAT THIS FIXES. A title unloads while one of its windows is still alive, and the
// window keeps the DLL's WNDPROC. The next message USER32 delivers to that window is
// a call into unmapped memory:
//
//     Unhandled exception: page fault on read access to 0x0544be30
//     EIP:0544be30  EAX:0544be30  ECX:00000200
//
// EIP == EAX == the fault address, and ECX is the MESSAGE ID (0x200 WM_MOUSEMOVE;
// also seen 0x86 WM_NCACTIVATE, 0x06 WM_ACTIVATE, 0x08 WM_KILLFOCUS). 0x0544be30 is
// FE_Client.dll+0x7BE30 -- inside a module the loader had unmapped 25 log lines
// earlier. Nothing is corrupt: the pointer was valid, and then its code went away.
//
// This is NOT a Wine bug and NOT rare. crashlog.cpp has carried the WER signature
// since August:
//
//     Faulting module name: FE_Client.dll_unloaded ... Fault offset: 0x0007be30
//
// -- six of them on desktop Windows in three days. On the Deck it fired every launch
// because Fantasy Earth was exiting early for an unrelated reason (see
// profiles.h fmv_wine_level); fixing that hid this, it did not fix it. Any future
// path where a title exits with a window still up re-arms it, which is exactly what
// makes it worth closing now rather than when it next surfaces.
//
// WHAT IT DOES. On the loader's UNLOADED notification -- which fires BEFORE the
// image is unmapped, the one moment when the old pointer is still readable and the
// new one can be installed -- every window in this process is checked, and any whose
// WNDPROC points inside the departing module is repointed at DefWindowProc. The
// window survives as an inert stub instead of a landmine. That is the whole fix.
//
// WHY DefWindowProc AND NOT "RESTORE THE ORIGINAL". There is no original to restore.
// The window belongs to the title (measured: HWND 0x4009A is FE's own game window,
// the one d3d8hook logs as "left to the title"), so its class WNDPROC lived in the
// same module and is going away too. And a subclass chain does not help either: the
// Viewer's app.dll is itself unloaded and RELOADED AT A DIFFERENT BASE across a title
// launch, so a saved "previous" pointer would be just as stale as the one it replaced.
// DefWindowProc is the only target guaranteed to still exist.
//
// WARNING: NO DestroyWindow, DELIBERATELY. This runs inside a loader-lock callback, where
// anything that SENDS a message can deadlock. Get/SetWindowLongPtr and the Enum
// family do not send messages; DestroyWindow does (WM_DESTROY/WM_NCDESTROY), so
// tidying the window away here would trade a crash for a hang. Leaving an inert
// window costs a few KB until whoever owns it tears it down.
//
// WARNING: AND THE A/W PAIR MUST MATCH. GetWindowLongPtrW on an ANSI window does not return
// the real procedure -- USER32 hands back an internal thunk handle -- so asking with
// the wrong charset compares a value that was never a code address, and the guard
// silently never matches. IsWindowUnicode decides, and the replacement is written
// with the same charset it was read with.
#include "polshim.h"

void logf(const char* fmt, ...);
void log_flush(void);

static int g_wg_on = 1;   // [polshim] wndproc_guard

struct WGScan {
    BYTE*  lo;
    BYTE*  hi;
    const char* mod;
    int    fixed;
};

// Repoint one window if its procedure lives in the departing image. Returns TRUE so
// enumeration always continues -- one stubborn window must not strand the others.
static BOOL wg_check_window(HWND hwnd, LPARAM lp)
{
    WGScan* s = (WGScan*)lp;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;

    const BOOL uni = IsWindowUnicode(hwnd);
    LONG_PTR proc = uni ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
                        : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (proc && (BYTE*)proc >= s->lo && (BYTE*)proc < s->hi) {
        char cls[64] = "";
        GetClassNameA(hwnd, cls, sizeof(cls));
        LONG_PTR repl = uni ? (LONG_PTR)DefWindowProcW : (LONG_PTR)DefWindowProcA;
        LONG_PTR old = uni ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, repl)
                           : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, repl);
        if (old) {
            s->fixed++;
            logf("[wndguard] HWND %p class=%s had its WNDPROC in %s (+0x%IX) -- "
                 "repointed to DefWindowProc%s before the unmap; a message to this "
                 "window would have jumped into freed code",
                 (void*)hwnd, cls[0] ? cls : "?", s->mod,
                 (SIZE_T)((BYTE*)proc - s->lo), uni ? "W" : "A");
        }
    }

    // Children carry their own procedures and are not reached by EnumWindows.
    EnumChildWindows(hwnd, (WNDENUMPROC)wg_check_window, lp);
    return TRUE;
}

void wndguard_configure(const wchar_t* ini)
{
    if (ini) g_wg_on = GetPrivateProfileIntW(L"polshim", L"wndproc_guard", 1, ini);
    if (!g_wg_on)
        logf("[wndguard] disabled ([polshim] wndproc_guard=0) -- a title that unloads "
             "with a live window will crash the process on its next message");
}

// Called from inject.cpp's LdrDllNotification on UNLOADED. `base` is still mapped.
void wndguard_module_unloading(HMODULE base)
{
    if (!g_wg_on || !base) return;

    // SizeOfImage straight from the headers: no psapi, no allocation, and it works
    // for a module the loader is already dismantling.
    SIZE_T span = 0;
    __try {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        span = nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!span) return;

    char path[MAX_PATH] = "", *leaf = path;
    if (GetModuleFileNameA(base, path, sizeof(path))) {
        for (char* p = path; *p; p++)
            if (*p == '\\' || *p == '/') leaf = p + 1;
    }

    WGScan s;
    s.lo = (BYTE*)base;
    s.hi = (BYTE*)base + span;
    s.mod = leaf;
    s.fixed = 0;

    EnumWindows((WNDENUMPROC)wg_check_window, (LPARAM)&s);

    // Silence is the normal case and worth saying nothing about; a hit is a bug in
    // the title we just papered over, and the next person needs to know it happened.
    if (s.fixed)
        logf("[wndguard] %s unloading: %d dangling window procedure(s) neutralised",
             leaf, s.fixed);
    if (s.fixed) log_flush();
}
