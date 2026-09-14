// reslock.cpp -- make GlobalLock/GlobalUnlock treat a pointer into a MAPPED
// IMAGE the way Windows does: hand it back untouched.
//
// THE BUG (measured live on the Steam Deck, 2026-08-23 -- the FMO game-start
// crash). FMO's shader-manager constructor
// (FrontMissionOnline.dll+0x20258b) loads its D3DX effect source from an
// embedded Win32 resource: FindResourceA -> LoadResource -> GlobalLock ->
// copy+XOR-decrypt -> D3DXCreateEffect, storing the ID3DXEffect* at
// [singleton+5]. LoadResource returns the resource's real VA, which in this
// (packed, rebuilt) DLL is UNALIGNED -- low bits 10. Windows' GlobalLock hands
// any non-handle pointer straight back, so the chain works there. Wine's
// GlobalLock (kernelbase LocalLock) returns identity only for 4-ALIGNED
// pointers; anything else is presumed a moveable-heap handle, and it tries to
// bump the lock count JUST BELOW the pointer -- a WRITE into the read-only
// .rsrc page (the [crash1st] #1 swallowed WRITE to 0x613D8A68 at ret
// FrontMissionOnline.dll+0x202666). Wine's own __EXCEPT eats the fault and
// returns NULL; FMO's NULL-check takes a SILENT bail path (no MessageBox --
// that is reserved for effect-compile errors), [singleton+5] stays NULL, and
// nine seconds later the first effect call at game entry virtual-calls the
// NULL pointer: the primary AV at +0x2024BE that FMO's force-quit handler then
// buries under the +0x202DF9 teardown fault.
//
// THE FIX. Hook GlobalLock/GlobalUnlock in the titles' IATs (patch_iat, by
// value, like every other hook). If the argument points into a mapped IMAGE
// (VirtualQuery Type == MEM_IMAGE) it is a resource pointer, never a heap
// handle: return it unchanged / succeed without touching memory -- exactly
// what Windows does. Real GlobalAlloc blocks and moveable handles live in
// MEM_PRIVATE heap memory and always fall through to the real function, so
// nothing else changes behaviour. Correct on Windows too by construction
// (identity is what Windows already returns), so the default is ON.
//
// [polshim] reslock=0 turns it off.

#include "polshim.h"
#include <windows.h>

static int  g_on = 1;
static long g_diverts = 0;          // logged for the first few, counted after

typedef LPVOID (WINAPI* PFN_GlobalLock)(HGLOBAL);
typedef BOOL   (WINAPI* PFN_GlobalUnlock)(HGLOBAL);
static PFN_GlobalLock   g_real_lock   = NULL;
static PFN_GlobalUnlock g_real_unlock = NULL;

// A resource pointer is a VA inside some module's mapped image. A real
// HGLOBAL (fixed or moveable) is heap memory -- MEM_PRIVATE -- and a
// moveable HANDLE is a small heap structure; neither can be MEM_IMAGE.
static bool image_ptr(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return false;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    return mbi.Type == MEM_IMAGE;
}

static void log_divert(const char* api, const void* p)
{
    long n = InterlockedIncrement(&g_diverts);
    if (n > 4) return;              // first few carry the finding; then quiet
    char mod[MAX_PATH] = "?";
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char*)p, &m) && m) {
        GetModuleFileNameA(m, mod, sizeof(mod));
        const char* leaf = strrchr(mod, '\\');
        if (leaf) memmove(mod, leaf + 1, strlen(leaf));
    }
    logf("[reslock] %s(%p) points into %s+0x%X (a resource, not a heap handle) "
         "-- returned as-is, as Windows does",
         api, p, mod, m ? (unsigned)((const BYTE*)p - (const BYTE*)m) : 0);
}

static LPVOID WINAPI hook_GlobalLock(HGLOBAL h)
{
    if (g_on && image_ptr(h)) { log_divert("GlobalLock", h); return (LPVOID)h; }
    return g_real_lock(h);
}

static BOOL WINAPI hook_GlobalUnlock(HGLOBAL h)
{
    if (g_on && image_ptr(h)) { log_divert("GlobalUnlock", h); return TRUE; }
    return g_real_unlock(h);
}

void reslock_init(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"polshim", L"reslock", 1, ini) != 0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        g_real_lock   = (PFN_GlobalLock)  GetProcAddress(k32, "GlobalLock");
        g_real_unlock = (PFN_GlobalUnlock)GetProcAddress(k32, "GlobalUnlock");
    }
    if (!g_real_lock || !g_real_unlock) g_on = 0;   // nothing to divert to
    if (!g_on) logf("[reslock] off (reslock=0%s)",
                    (!g_real_lock || !g_real_unlock) ? ", or kernel32 unresolved" : "");
}

// Exposed for inject.cpp's patch_iat by-value swap map. NULL `from` never
// matches, so reslock=0 leaves every IAT untouched.
void* reslock_real_GlobalLock()    { return g_on ? (void*)g_real_lock   : NULL; }
void* reslock_hook_GlobalLock()    { return g_on ? (void*)hook_GlobalLock   : NULL; }
void* reslock_real_GlobalUnlock()  { return g_on ? (void*)g_real_unlock : NULL; }
void* reslock_hook_GlobalUnlock()  { return g_on ? (void*)hook_GlobalUnlock : NULL; }
