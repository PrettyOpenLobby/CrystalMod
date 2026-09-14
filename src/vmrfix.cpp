// vmrfix.cpp -- intercept Front Mission Online's VMR-9 rendering mode.
//
// THE PROBLEM:
// FMO plays its FMV through a Video Mixing Renderer 9 in WINDOWLESS mode. A windowless
// renderer has no window of its own -- it composites straight into the game's HWND. In
// exclusive fullscreen that has a defined draw order and looks correct; forced windowed,
// the game's own D3D Present and the VMR's composite are two independent presenters over
// the same client area with no ordering, so the movie flickers unusably. That is the one
// reason FMO is pinned fullscreen while every other title can be windowed.
//
// THE INTERCEPTION POINT. FMO creates the VMR-9 itself, by CLSID, BEFORE the filter graph
// is connected -- through the same ole32 CoCreateInstance that comtrace.cpp already hooks
// (measured: clsid={51B4ABF3-...} from FrontMissionOnline.dll+0x22AE26). So the filter can
// be reached at birth, and comtrace hands it here. It then QIs IVMRFilterConfig9 and calls
// SetRenderingMode(WINDOWLESS) itself -- and that call is the whole fault. We do NOT race
// it (SetRenderingMode may be called only once; beating FMO to it makes ITS call fail and
// it may bail). We INTERCEPT it: patch the one vtable slot so FMO's own call arrives here.
//
// TWO MODES, one switch ([dx] d3d_vmr_mode):
//   0 = off. Leave FMO's VMR exactly as it asks. FMO stays fullscreen; nothing changes.
//   1 = WINDOWED. Force VMR9Mode_Windowed, so the movie gets its own child HWND instead of
//       sharing the game's surface -- which is precisely Fantasy Earth's proven-working
//       windowed video config. Cheap, and it validates the interception point before any
//       allocator exists. This is the experiment to run FIRST.
//   4 = RENDERLESS. Force VMR9Mode_Renderless and supply our own allocator-presenter so
//       decoded frames arrive as D3D surfaces drawn inside the game's own render loop --
//       one presenter, no race, scales with the window for free. NOT IMPLEMENTED yet; the
//       switch is refused with a log line rather than half-doing it, because forcing
//       renderless without providing an allocator means the movie never draws at all.
//
// Default OFF. FMO works fullscreen today and this must not change that until a windowed
// FMO has been seen to play its FMV correctly on the account holder's machine.
#include "polshim.h"
#include <objbase.h>

// IVMRFilterConfig9 -- {5A804648-4F66-4867-9C43-4F5C822CF1B8}. We only need the IID and the
// vtable index of SetRenderingMode; the interface is not in every SDK, so it is declared by
// hand here the same way d3d8hook.cpp declares the d3d8 ABI.
static const GUID IID_IVMRFilterConfig9 =
    { 0x5a804648, 0x4f66, 0x4867, { 0x9c, 0x43, 0x4f, 0x5c, 0x82, 0x2c, 0xf1, 0xb8 } };
static const GUID CLSID_VideoMixingRenderer9 =
    { 0x51b4abf3, 0x748f, 0x4e3b, { 0xa2, 0x76, 0xc8, 0x28, 0x33, 0x0e, 0x92, 0x6a } };

// IVMRFilterConfig9 vtable (own methods after IUnknown's 0/1/2):
//   3 SetImageCompositor   4 SetNumberOfStreams   5 GetNumberOfStreams
//   6 SetRenderingPrefs    7 GetRenderingPrefs    8 SetRenderingMode   9 GetRenderingMode
#define VMRFC9_SLOT_SetRenderingMode 8

// VMR9Mode
#define VMR9Mode_Windowed    0x00000001
#define VMR9Mode_Windowless  0x00000002
#define VMR9Mode_Renderless  0x00000004

static int    g_mode = 0;            // [dx] d3d_vmr_mode: 0 off, 1 windowed, 4 renderless
static void** g_fc_vtbl = NULL;      // the IVMRFilterConfig9 vtable we patched
static void*  g_fc_orig = NULL;      // its original SetRenderingMode
static LONG   g_said = 0;

typedef HRESULT (__stdcall *PFN_SetRenderingMode)(void* self, DWORD mode);
static PFN_SetRenderingMode g_real_srm = NULL;

static const char* mode_name(DWORD m)
{
    switch (m) {
        case VMR9Mode_Windowed:   return "WINDOWED";
        case VMR9Mode_Windowless: return "WINDOWLESS";
        case VMR9Mode_Renderless: return "RENDERLESS";
        default:                  return "?";
    }
}

// FMO's own SetRenderingMode call arrives here. Force the mode the profile wants.
static HRESULT __stdcall hook_SetRenderingMode(void* self, DWORD mode)
{
    DWORD want = mode;
    if (g_mode == 1)      want = VMR9Mode_Windowed;
    // renderless (4) is refused in vmrfix_configure, so it never reaches here as a force;
    // if it ever does, fall through to the caller's own mode rather than break the movie.

    if (InterlockedCompareExchange(&g_said, 1, 0) == 0)
        logf("[vmr] SetRenderingMode: FMO asked for %s (0x%lX); serving %s (0x%lX)",
             mode_name(mode), mode, mode_name(want), want);

    HRESULT hr = g_real_srm ? g_real_srm(self, want) : E_UNEXPECTED;
    if (FAILED(hr))
        logf("[vmr] SetRenderingMode(%s) FAILED hr=0x%08lX -- the renderer kept its "
             "own mode; the FMV is unchanged", mode_name(want), (unsigned long)hr);
    return hr;
}

void vmrfix_configure(const wchar_t* ini)
{
    if (!ini) return;
    g_mode = GetPrivateProfileIntW(L"dx", L"d3d_vmr_mode", 0, ini);
    if (g_mode == VMR9Mode_Renderless) {
        logf("[vmr] d3d_vmr_mode=4 (renderless) requested, but the allocator-presenter is "
             "NOT implemented yet -- forcing renderless without one would leave the movie "
             "blank. Treating as OFF. Use d3d_vmr_mode=1 (windowed) for now.");
        g_mode = 0;
    }
    if (g_mode == 1)
        logf("[vmr] armed: FMO's VMR-9 will be forced to WINDOWED mode "
             "(d3d_vmr_mode=1) -- its FMV gets its own child window instead of racing "
             "the game's surface");
}

int vmrfix_enabled() { return g_mode != 0; }

// Called by comtrace.cpp for every successful CLSID_VideoMixingRenderer9 activation, with
// the filter's IUnknown. QI IVMRFilterConfig9 and patch SetRenderingMode on its vtable so
// FMO's own call is intercepted. The vtable is shared per class, so patching once covers
// FMO's later QI-and-call; we hold no reference and restore nothing (only FMO's FMV uses a
// VMR-9 in this process, and the patch is inert once d3d_vmr_mode is off).
void vmrfix_note_vmr(void* punk)
{
    if (g_mode == 0 || !punk) return;
    if (g_fc_vtbl) return;                          // already patched this process

    IUnknown* u = (IUnknown*)punk;
    void* fc = NULL;
    if (FAILED(u->QueryInterface(IID_IVMRFilterConfig9, &fc)) || !fc) {
        logf("[vmr] a VMR-9 was created but it has no IVMRFilterConfig9 -- not a VMR9 we "
             "can steer; left alone");
        return;
    }

    void** vtbl = *(void***)fc;
    if (IsBadReadPtr(vtbl, (VMRFC9_SLOT_SetRenderingMode + 1) * sizeof(void*))) {
        ((IUnknown*)fc)->Release();
        return;
    }
    void* slot = vtbl[VMRFC9_SLOT_SetRenderingMode];
    if (!slot || IsBadCodePtr((FARPROC)slot) || slot == (void*)hook_SetRenderingMode) {
        ((IUnknown*)fc)->Release();
        return;
    }

    DWORD old;
    if (!VirtualProtect(&vtbl[VMRFC9_SLOT_SetRenderingMode], sizeof(void*),
                        PAGE_READWRITE, &old)) {
        ((IUnknown*)fc)->Release();
        return;
    }
    g_real_srm = (PFN_SetRenderingMode)slot;
    g_fc_orig  = slot;
    g_fc_vtbl  = vtbl;
    vtbl[VMRFC9_SLOT_SetRenderingMode] = (void*)hook_SetRenderingMode;
    VirtualProtect(&vtbl[VMRFC9_SLOT_SetRenderingMode], sizeof(void*), old, &old);

    ((IUnknown*)fc)->Release();                      // the vtable stays patched regardless
    logf("[vmr] VMR-9 born -- SetRenderingMode intercepted (slot %d, vtable=%p); FMO's "
         "own mode selection will now be steered to %s",
         VMRFC9_SLOT_SetRenderingMode, (void*)vtbl,
         g_mode == 1 ? "WINDOWED" : "its own choice");
}
