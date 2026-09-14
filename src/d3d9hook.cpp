// d3d9hook.cpp -- the Direct3D 9 twin of d3d8hook.cpp's windowed override.
//
// WHY A SECOND FILE
//
// d3d8hook.cpp fixes "the game steals my monitor" by rewriting the fullscreen
// CreateDevice into a windowed one. It covers TM.dll, FFXiMain.dll and
// FE_Client.dll, which all share the IDirect3D8 vtable. It does NOT cover Front
// Mission Online, and that is not a tuning problem -- it is structural:
//
//     Viewer  pol.exe                DDRAW
//     Tetra Master  TM.dll           d3d8
//     FFXI          FFXiMain.dll     d3d8
//     Fantasy Earth FE_Client.dll    d3d8
//     FMO   FrontMissionOnline.dll   d3d9      <- the only one
//
// (read off each module's PE import table, 2026-08-12). So with
// [dx] d3d_windowed=1 set and working for Tetra Master, FMO still went
// fullscreen and ignored input -- the d3d8 hook was never on its path at all.
// Confirmed in the live log: `Direct3DCreate8` was EAT-patched and then NEVER
// CALLED during the FMO session.
//
// FMO also carries the string `d3d9d.dll` (the DEBUG D3D9 runtime, absent from a
// normal machine), but that is only a string -- the import table names plain
// `d3d9.dll`, which is what it actually loads.
//
// WHAT IS AND IS NOT DUPLICATED
//
// Only the device-creation path is API-specific and needs a twin. The cursor
// translation and focus-stealing hooks in d3d8hook.cpp patch **user32**, not
// d3d8, so they already apply to FMO and are deliberately not repeated here.
//
// ONE REAL ADVANTAGE OVER THE D3D8 SIDE
//
// `d3d9.h` ships in the Windows SDK, where `d3d8.h` does not. d3d8hook.cpp had
// to hand-declare the ABI and prove its slot numbers with a standalone probe
// (src/d3d8probe.cpp) because a wrong slot index is a silent crash. Here the
// header IS the ABI contract, so the structures and calling conventions are the
// SDK's own. The slot numbers are still asserted at runtime below rather than
// trusted, because that check is nearly free and the failure mode is severe.
#include "polshim.h"
#include <d3d9.h>
#include <intrin.h>          // _ReturnAddress -- names the title that called us

// IDirect3D9 vtable: IUnknown 0-2, then the interface in declaration order --
// RegisterSoftwareDevice 3, GetAdapterCount 4, GetAdapterIdentifier 5,
// GetAdapterModeCount 6, EnumAdapterModes 7, GetAdapterDisplayMode 8,
// CheckDeviceType 9, CheckDeviceFormat 10, CheckDeviceMultiSampleType 11,
// CheckDepthStencilMatch 12, CheckDeviceFormatConversion 13, GetDeviceCaps 14,
// GetAdapterMonitor 15, CreateDevice 16.
#define S9_GetAdapterCount   4
#define S9_CreateDevice      16

// IDirect3DDevice9 vtable: IUnknown 0-2, then the interface in declaration order.
// CreateVertexDeclaration is index 86 -- and that is not a guess: FMO's crashing
// call site is `call [edx+0x158]`, and 0x158/4 == 86 exactly.
#define S9D_CreateVertexDeclaration 86
// IDirect3DDevice9: IUnknown 0-2, TestCooperativeLevel 3,
// GetAvailableTextureMem 4, EvictManagedResources 5, GetDirect3D 6,
// GetDeviceCaps 7, GetDisplayMode 8, GetCreationParameters 9,
// SetCursorProperties 10, SetCursorPosition 11, ShowCursor 12,
// CreateAdditionalSwapChain 13, GetSwapChain 14, GetNumberOfSwapChains 15,
// Reset 16.
#define S9D_TestCoopLevel    3
#define S9D_GetDisplayMode   8
#define S9D_Reset            16
// ...Reset 16, Present 17, GetBackBuffer 18.
#define S9D_Present          17
// IDirect3DSwapChain9: IUnknown 0-2, Present 3, GetFrontBufferData 4,
// GetBackBuffer 5, GetRasterStatus 6, GetDisplayMode 7, GetDevice 8,
// GetPresentParameters 9.
//
// Hooking the DEVICE's Present alone was NOT enough, and the way it failed is
// worth remembering: FMO presented every frame through its SWAP CHAIN instead,
// so the counter read a confident zero while the title screen was plainly on
// screen -- and the summary drew exactly the wrong conclusion from it. An
// instrument that can be silently bypassed must say so; both paths are counted
// now, and the "never presented" note only fires when both hooks are live.
#define S9SC_Present         3

typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice9)(IDirect3D9*, UINT, D3DDEVTYPE,
                                                       HWND, DWORD,
                                                       D3DPRESENT_PARAMETERS*,
                                                       IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset9)(IDirect3DDevice9*,
                                                D3DPRESENT_PARAMETERS*);
// NOTE the extra argument. D3D8's IDirect3DDevice8::GetDisplayMode takes only
// the out-parameter; D3D9 added a swap-chain index in front of it. Same slot
// number, different signature -- so this hook genuinely cannot be shared with
// d3d8hook.cpp's, and calling through the d3d8 typedef would corrupt the stack.
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevGetDisplayMode9)(IDirect3DDevice9*, UINT,
                                                            D3DDISPLAYMODE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present9)(IDirect3DDevice9*, const RECT*,
                                                  const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SCPresent9)(IDirect3DSwapChain9*, const RECT*,
                                                    const RECT*, HWND, const RGNDATA*,
                                                    DWORD);
typedef UINT (STDMETHODCALLTYPE *PFN_GetAdapterCount9)(IDirect3D9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_TestCoop9)(IDirect3DDevice9*);

static int g_enable   = 0;
static int g_trace    = 1;
static int g_windowed = 0;
static int g_fitwin   = 1;
static int g_fakemode = 1;   // report the backbuffer as the screen size
static int g_decl_guard = 1;  // [dx] d3d9_decl_guard -- see hook_CreateVertexDecl
static int g_fs_rescue = 1;  // retry a FAILED fullscreen CreateDevice windowed
// [dx] d3d_fmv_yield: while FMO's windowless VMR-9 movie is actively presenting to its
// OWN swap chain over the same window, the GAME yields its present -- otherwise the
// game's (black/empty) frame is drawn on top of the movie and the FMV shows as black.
// Two swap chains over one window: last to present wins, so not covering the movie IS
// showing it. Default off (experimental); resumes the instant the movie stops.
static int  g_fmv_yield = 0;
static volatile LONG g_vmr_last_present = 0;   // GetTickCount of the last VMR present
static LONG g_n_yielded = 0;

static PFN_Direct3DCreate9     real_Direct3DCreate9 = NULL;
static PFN_CreateDevice9       orig_CreateDevice    = NULL;
static PFN_Reset9              orig_Reset           = NULL;
static PFN_DevGetDisplayMode9  orig_GetDisplayMode  = NULL;
static PFN_Present9            orig_Present         = NULL;
static PFN_SCPresent9          orig_SCPresent       = NULL;
static PFN_TestCoop9           orig_TestCoop        = NULL;

// --- sleep/resume recovery (wakerecover.cpp) --------------------------------
// The present parameters the device was LAST successfully created or reset with
// -- not the ones FMO asked for, which are a fullscreen set the windowed
// override rewrote. An external Reset has to reproduce the device it has, not
// the one the title requested. See the block comment in wakerecover.cpp.
static D3DPRESENT_PARAMETERS   g_pp_last;
static bool                    g_pp_have  = false;
static void wake_arm_d3d9(IDirect3DDevice9* dev, HWND wnd);
static IDirect3DSwapChain9*    g_game_swapchain     = NULL;
static bool                    g_sc_hooked          = false;

// The device we forced windowed -- i.e. the TITLE's own renderer. FMO's process
// holds more than one d3d9 device: the game's, and an 8x8 SOFTWARE_VP one that
// belongs to DirectShow's VMR-9 while an FMV plays (measured 2026-08-13). They
// share a vtable, so a Present hook sees both and MUST tell them apart by
// device pointer -- otherwise the movie's frames would be counted as the game's
// and "the game is rendering" would be the wrong answer for the right reason.
static IDirect3DDevice9* g_game_device = NULL;

static bool g_vtbl_hooked = false;
static bool g_dev_hooked  = false;
static HWND g_focus_window = NULL;   // Reset() gets no HWND; remember CreateDevice's
static HWND g_game_window  = NULL;

static LONG g_n_create = 0, g_n_fullscreen = 0, g_n_forced = 0,
            g_n_retry = 0, g_n_rescued = 0, g_n_device = 0, g_n_reset = 0,
            g_n_modefaked = 0, g_n_present_game = 0, g_n_present_other = 0;

int d3d9_enabled() { return g_enable; }

void d3d9_configure(const wchar_t* ini)
{
    if (!ini) return;
    // "windowed" is a USER INTENT, not an API choice: someone who set
    // d3d_windowed=1 to stop Tetra Master grabbing the monitor means the same
    // thing for FMO. So the d3d8 keys are the DEFAULTS here and the d3d9_* keys
    // exist only to override one API without the other.
    //
    // The d3d_windowed default is 1 from 2026-08-24 (d3d8hook.cpp says why), and it
    // has to be 1 in BOTH files: FMO is the only d3d9 title, so a 0 left here would
    // mean "windowed by default" quietly excluded the one title whose fullscreen mode
    // a modern panel is least likely to be able to serve.
    g_enable   = GetPrivateProfileIntW(L"dx", L"d3d_enable",   0, ini);
    g_trace    = GetPrivateProfileIntW(L"dx", L"d3d_trace",    trace_at(1), ini);
    g_windowed = ini_int_title(L"dx", L"d3d_windowed", 1, ini);
    // d3d_fitwindow is RETIRED (d3d8hook.cpp explains why); d3d9_fitwindow
    // survives it only as the per-API escape hatch the rest of this block is.
    g_fitwin   = 1;
    g_fakemode = GetPrivateProfileIntW(L"dx", L"d3d_fakemode",  1, ini);
    g_enable   = GetPrivateProfileIntW(L"dx", L"d3d9_enable",   g_enable,   ini);
    g_trace    = GetPrivateProfileIntW(L"dx", L"d3d9_trace",    g_trace,    ini);
    g_windowed = GetPrivateProfileIntW(L"dx", L"d3d9_windowed", g_windowed, ini);
    d3d_display_windowed(&g_windowed, ini);      // [dx] display outranks all of the above
    g_fitwin   = GetPrivateProfileIntW(L"dx", L"d3d9_fitwindow", g_fitwin,  ini);
    g_fakemode = GetPrivateProfileIntW(L"dx", L"d3d9_fakemode",  g_fakemode, ini);
    g_decl_guard = GetPrivateProfileIntW(L"dx", L"d3d9_decl_guard", 1, ini);
    // d3d9-only on purpose: only FMO is d3d9, and the d3d8 titles have no
    // equivalent failure mode measured. See hook_CreateDevice for what this is.
    g_fs_rescue = GetPrivateProfileIntW(L"dx", L"d3d_fs_rescue", 1, ini);
    g_fmv_yield = GetPrivateProfileIntW(L"dx", L"d3d_fmv_yield", 0, ini);
    // The other [dx] d3d_* keys are NOT read here on purpose -- d3d_unmask,
    // d3d_cursor, d3d_msgspy, d3d_frame, d3d_inputhwnd, d3d_freecursor,
    // d3d_nosteal and d3d_windowed_except all belong to the shared support layer
    // in d3d8hook.cpp, which this file now calls into. Duplicating them here is
    // exactly how the two paths drifted apart the first time.
}

// Live re-read for the settings dialog, keeping configure's chain: the d3d_*
// key is the default, the d3d9_* key overrides one API without the other.
// d3d9_enable (and the d3d_enable it follows) is NOT re-read -- enable is the
// vtable-patch install gate, decided at startup.
void d3d9_reload(const wchar_t* ini)
{
    if (!ini) return;
    g_trace    = GetPrivateProfileIntW(L"dx", L"d3d_trace",    trace_at(1), ini);
    g_windowed = ini_int_title(L"dx", L"d3d_windowed", 1, ini);
    g_fitwin   = 1;
    g_fakemode = GetPrivateProfileIntW(L"dx", L"d3d_fakemode",  1, ini);
    g_trace    = GetPrivateProfileIntW(L"dx", L"d3d9_trace",    g_trace,    ini);
    g_windowed = GetPrivateProfileIntW(L"dx", L"d3d9_windowed", g_windowed, ini);
    d3d_display_windowed(&g_windowed, ini);      // [dx] display outranks all of the above
    g_fitwin   = GetPrivateProfileIntW(L"dx", L"d3d9_fitwindow", g_fitwin,  ini);
    g_fakemode = GetPrivateProfileIntW(L"dx", L"d3d9_fakemode",  g_fakemode, ini);
    g_fs_rescue = GetPrivateProfileIntW(L"dx", L"d3d_fs_rescue", 1, ini);
    g_fmv_yield = GetPrivateProfileIntW(L"dx", L"d3d_fmv_yield", 0, ini);
    logf("[reload] d3d9: trace=%d windowed=%d fitwin=%d fakemode=%d "
         "fs_rescue=%d fmv_yield=%d",
         g_trace, g_windowed, g_fitwin, g_fakemode, g_fs_rescue, g_fmv_yield);
}

static bool patch_slot(void** vtbl, int slot, void* hook, void** out_orig)
{
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (out_orig) *out_orig = vtbl[slot];
    vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return true;
}

// Arm `slot` ONLY if it still holds `expect`. See the twin in d3d8hook.cpp for
// why a re-arm needs the exchange to decide: two threads racing a plain
// read-then-write can leave one hook stored as the other's "original", which is
// a hook that calls itself until the stack runs out.
static bool patch_slot_cas(void** vtbl, int slot, void* hook, void* expect,
                           void** out_orig)
{
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    void* prev = InterlockedCompareExchangePointer((volatile PVOID*)&vtbl[slot],
                                                   hook, expect);
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    if (prev != expect) return false;          // someone else wrote it first
    if (out_orig) *out_orig = expect;
    return true;
}

// Assert the slot numbering against the object itself before writing to it.
// Calls GetAdapterCount BOTH ways -- through the typed interface (which the SDK
// header guarantees) and through the raw slot -- and requires they agree. If the
// numbering were off, the raw call would land on a different method and almost
// certainly disagree or fault. Cheap, and the alternative is a silent crash.
static bool validate_d3d9(IDirect3D9* p)
{
    __try {
        UINT typed = p->GetAdapterCount();
        PFN_GetAdapterCount9 raw =
            (PFN_GetAdapterCount9)(*(void***)p)[S9_GetAdapterCount];
        UINT byslot = raw(p);
        if (typed != byslot || typed == 0) {
            logf("[d3d9] vtable check FAILED: GetAdapterCount typed=%u slot%d=%u "
                 "-- slot numbering is wrong, nothing patched",
                 typed, S9_GetAdapterCount, byslot);
            return false;
        }
        logf("[d3d9] vtable check ok (GetAdapterCount=%u both ways)", typed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("[d3d9] vtable check FAULTED -- slot numbers suspect, nothing patched");
        return false;
    }
}

static void log_pp(const char* tag, const D3DPRESENT_PARAMETERS* p)
{
    if (!g_trace || !p) return;
    logf("[d3d9] %s: %ux%u fmt=%d windowed=%d swap=%d hDevWnd=%p count=%u "
         "refresh=%u interval=%u flags=%08lX",
         tag, p->BackBufferWidth, p->BackBufferHeight, (int)p->BackBufferFormat,
         (int)p->Windowed, (int)p->SwapEffect, p->hDeviceWindow,
         p->BackBufferCount, p->FullScreen_RefreshRateInHz,
         p->PresentationInterval, p->Flags);
}

// The window restyle/resize now comes from d3d8hook.cpp's fit_window, reached
// through d3d_prepare_game_window. This file used to carry its own copy, and it
// had fallen behind in two ways that mattered:
//
//  * it stripped WS_THICKFRAME | WS_MAXIMIZEBOX ("no resize: backbuffer is
//    fixed"), which predates the measurement that D3D stretches the backbuffer
//    to fill the client area. The working Tetra Master recipe is to MAXIMISE the
//    window, and FMO simply could not be maximised.
//  * it centred the window using GetSystemMetrics -- which is one of our own
//    hooks. That was harmless only while fakemode stayed inert for FMO; the
//    moment the backbuffer is recorded (below) it would have centred against an
//    800x600 "screen" that does not exist. d3d8hook's copy calls
//    real_GetSystemMetrics for exactly this reason.
//
// Keeping one implementation is the point: the divergence, not the code, was the
// bug.

// Report the backbuffer as the display mode, so a fullscreen-only game that asks
// "how big is the screen" gets an answer consistent with what it is rendering.
// The GetSystemMetrics half of this is already shared (d3d8hook's user32 hook,
// gated on the backbuffer record that d3d_prepare_game_window now sets for us);
// only the device method needs a d3d9-specific hook, because D3D9 put a
// swap-chain index in front of the out-parameter.
static HRESULT STDMETHODCALLTYPE hook_GetDisplayMode(IDirect3DDevice9* self,
                                                     UINT iSwapChain,
                                                     D3DDISPLAYMODE* m)
{
    HRESULT hr = orig_GetDisplayMode(self, iSwapChain, m);
    if (SUCCEEDED(hr) && m && g_fakemode) {
        int bw = 0, bh = 0;
        d3d8_backbuffer_size(&bw, &bh);
        if (bw > 0 && bh > 0) {
            UINT ow = m->Width, oh = m->Height;
            m->Width  = (UINT)bw;
            m->Height = (UINT)bh;
            if (ow != m->Width || oh != m->Height) {
                InterlockedIncrement(&g_n_modefaked);
                if (g_trace && (g_n_modefaked < 6 || (g_n_modefaked % 600) == 0))
                    logf("[mode] d3d9 device GetDisplayMode %ux%u -> %ux%u "
                         "(the game is told the screen IS its backbuffer)",
                         ow, oh, m->Width, m->Height);
            }
        }
    }
    return hr;
}

// Rewrite fullscreen present params into legal windowed ones.
//
// Unlike D3D8, D3D9 DOES accept D3DFMT_UNKNOWN in windowed mode and inherits the
// desktop format -- that difference is called out in d3d8hook.cpp's make_windowed,
// where passing UNKNOWN earns D3DERR_INVALIDCALL. We still ask the adapter for a
// concrete format when we can, and fall back to UNKNOWN, which is valid here.
static void make_windowed(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* p,
                          HWND focus)
{
    p->Windowed                   = TRUE;
    p->FullScreen_RefreshRateInHz = 0;    // must be 0 windowed
    p->PresentationInterval       = D3DPRESENT_INTERVAL_DEFAULT;

    D3DDISPLAYMODE dm;
    if (d3d && SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &dm)))
        p->BackBufferFormat = dm.Format;
    else
        p->BackBufferFormat = D3DFMT_UNKNOWN;

    // Fullscreen ignores hDeviceWindow, so a fullscreen-only game often leaves
    // it NULL. Windowed mode presents INTO it.
    if (!p->hDeviceWindow && focus) p->hDeviceWindow = focus;

    if (p->SwapEffect == D3DSWAPEFFECT_FLIP)
        p->SwapEffect = D3DSWAPEFFECT_DISCARD;
    if (p->BackBufferCount == 0) p->BackBufferCount = 1;
}

// Does the adapter list ANY display mode of this size? Modern panels commonly
// drop 800x600 from the mode list entirely, and then no fullscreen phrasing of
// that size can succeed -- measured on this box 2026-08-18 (9 X8R8G8B8 modes,
// none 800x600; every fullscreen variant INVALIDCALL, windowed fine).
static bool adapter_has_mode(IDirect3D9* d3d, UINT adapter, UINT w, UINT h)
{
    if (!d3d) return false;
    UINT n = d3d->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8);
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m;
        if (SUCCEEDED(d3d->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, i, &m)) &&
            m.Width == w && m.Height == h)
            return true;
    }
    return false;
}

static HRESULT STDMETHODCALLTYPE hook_Reset(IDirect3DDevice9* self,
                                            D3DPRESENT_PARAMETERS* pp);
static HRESULT STDMETHODCALLTYPE hook_TestCoopLevel(IDirect3DDevice9* self);

// WHICH DEVICE IS THE TITLE'S -- without needing the windowed override to have run.
//
// g_game_device was only ever assigned on the forced-windowed and rescued paths,
// so with [dx] d3d_windowed=0 -- THE DEFAULT -- it stayed NULL for the whole
// session and every one of FMO's own frames was counted as the VMR-9 movie's.
// That was survivable for a frame counter; it is not survivable for the
// sleep/resume recovery, which would then be watching nothing.
//
// The discriminator is the CALLER. Two kinds of code create a d3d9 device in
// this process: the title, and DirectShow's VMR-9 for the FMV. The VMR lives in
// the Windows directory and the title does not, which needs no title name and so
// stays as title-agnostic as the rest of this file.
static bool caller_is_title(void* ra)
{
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)ra, &m) || !m)
        return false;
    char path[MAX_PATH] = "", win[MAX_PATH] = "";
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    if (!GetWindowsDirectoryA(win, MAX_PATH))   return false;
    size_t n = strlen(win);
    return n > 0 && _strnicmp(path, win, n) != 0;
}

// Does the TITLE's renderer ever put a frame on screen?
//
// Nothing in the log could answer that, and it is the question that splits the
// two explanations for a black game window: "the game is drawing black" versus
// "the game never draws at all and what you see is an unpainted window with
// something else on top of it". A counter is enough -- this is a per-frame path,
// so it stays a counter and never logs inside the loop after the first frame.

// True when the GAME should skip THIS present because the movie is playing over the same
// window on its own swap chain. Gated on d3d_fmv_yield, and only within a short window of
// the movie's last frame -- so the moment the movie stops presenting (it ended, or the
// user skipped it with Enter) the game's present resumes on the very next frame.
static bool fmv_should_yield()
{
    if (!g_fmv_yield) return false;
    LONG last = InterlockedCompareExchange(&g_vmr_last_present, 0, 0);
    if (!last) return false;
    if ((DWORD)(GetTickCount() - (DWORD)last) >= 250) return false;   // movie idle -> present
    if (InterlockedIncrement(&g_n_yielded) == 1)
        logf("[d3d9] d3d_fmv_yield: the game is YIELDING its present while the VMR-9 "
             "movie is on screen, so its black frame does not cover the movie");
    return true;
}

static HRESULT STDMETHODCALLTYPE hook_Present(IDirect3DDevice9* self,
                                              const RECT* src, const RECT* dst,
                                              HWND override, const RGNDATA* dirty)
{
    bool is_game = (self == g_game_device);
    if (is_game) {
        if (InterlockedIncrement(&g_n_present_game) == 1)
            logf("[d3d9] FIRST Present on the GAME device -- its render loop is "
                 "running (anything black on screen after this is being DRAWN "
                 "black, not left unpainted)");
        if (fmv_should_yield()) return D3D_OK;   // let the movie's frame stand
    } else {
        InterlockedExchange(&g_vmr_last_present, (LONG)GetTickCount());
        if (InterlockedIncrement(&g_n_present_other) == 1)
            logf("[d3d9] FIRST Present on a NON-game device (%p) -- this is the "
                 "VMR-9 movie renderer, not the title", self);
    }
    HRESULT hr = orig_Present(self, src, dst, override, dirty);
    // Only the GAME's frames: the VMR-9 movie has its own device, and counting
    // its presents as the title's would make the recovery think the render loop
    // is alive while the game is frozen.
    if (is_game) wake_note_present(self, hr);
    return hr;
}

// The path FMO actually uses. Same accounting, keyed on the swap chain we took
// from the game's device at creation -- the VMR-9 movie has its own.
static HRESULT STDMETHODCALLTYPE hook_SCPresent(IDirect3DSwapChain9* self,
                                                const RECT* src, const RECT* dst,
                                                HWND override, const RGNDATA* dirty,
                                                DWORD flags)
{
    bool is_game = (self == g_game_swapchain);
    if (is_game) {
        if (InterlockedIncrement(&g_n_present_game) == 1)
            logf("[d3d9] FIRST Present on the GAME swap chain -- its render loop "
                 "is running (this is the path FMO uses, not the device method)");
        if (fmv_should_yield()) return D3D_OK;   // let the movie's frame stand
    } else {
        InterlockedExchange(&g_vmr_last_present, (LONG)GetTickCount());
        if (InterlockedIncrement(&g_n_present_other) == 1)
            logf("[d3d9] FIRST Present on a NON-game swap chain (%p) -- the VMR-9 "
                 "movie renderer, not the title", self);
    }
    HRESULT hr = orig_SCPresent(self, src, dst, override, dirty, flags);
    // The path FMO really uses, so the recovery has to be driven from it too --
    // and the pump is keyed on the DEVICE, not on the swap chain that presented.
    if (is_game) wake_note_present(g_game_device, hr);
    return hr;
}

// Pass-through TestCooperativeLevel. It exists for the sleep/resume recovery:
// this is where a title tells us, on its own render thread, that the device is
// lost -- including for a title that stops calling Present while it is. The
// state is logged on CHANGE only, because a lost device is polled every frame.
static HRESULT STDMETHODCALLTYPE hook_TestCoopLevel(IDirect3DDevice9* self)
{
    HRESULT hr = orig_TestCoop(self);
    static HRESULT last = (HRESULT)0xDEADBEEF;
    if (hr != last) {
        last = hr;
        logf("[d3d9] TestCooperativeLevel -> 0x%08lX%s", (unsigned long)hr,
             hr == D3D_OK                ? " (D3D_OK)" :
             hr == D3DERR_DEVICELOST     ? " (D3DERR_DEVICELOST)" :
             hr == D3DERR_DEVICENOTRESET ? " (D3DERR_DEVICENOTRESET)" : "");
        log_flush();
    }
    // Unconditional: wake_note_test filters on the device it was REGISTERED
    // with, which is the same question asked once, in one place.
    wake_note_test(self, hr);
    return hr;
}

// ---------------------------------------------------------------- decl guard
//
// WHY THIS EXISTS. Front Mission Online dies entering the game on the Steam Deck,
// 100% reproducibly and at a byte-identical address:
//
//   Unhandled page fault on read access to 0x38031575 at 0x61202df9
//   frontmissiononline+0x202df9:  mov ecx, [eax]        (EAX = 0x38031575)
//
// That is a TEARDOWN. The function at +0x202DC0 walks a global object and Releases
// a chain of interfaces, NULL-checking each one first -- so a MISSING pointer is
// handled fine and only a non-NULL junk value is fatal. Two of those slots
// (+0x1D1, +0x1D5) are filled by `call [edx+0x158]` = CreateVertexDeclaration,
// with the slot's address passed as the out-parameter and D3DVERTEXELEMENT9 arrays
// at 0x6139E398 / 0x6139E3D0 as input.
//
// So: the creation does not write the out-param, the slot keeps uninitialised heap
// bytes (repeatable, because Wine's allocator hands back the same block), and the
// teardown Releases that. FMO never initialises the slots to NULL itself.
//
// The guard is to honour the out-param contract on FMO's behalf: when creation
// FAILS, write NULL. The title's own NULL check then does the rest and the teardown
// walks past instead of dying. This cannot mask a working path -- on success we
// touch nothing -- and it turns an unrecoverable page fault into, at worst, a clean
// NULL deref that says where it happened.
//
// It also LOGS every failure with the HRESULT, which is the thing nobody had: "it
// is not writing them" becomes "it returned 0x8876xxxx because ...".
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVertexDecl9)(
    IDirect3DDevice9*, const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**);
static PFN_CreateVertexDecl9 orig_CreateVertexDecl = NULL;
static LONG g_decl_fail  = 0;
static bool g_decl_hooked = false;

static HRESULT STDMETHODCALLTYPE hook_CreateVertexDecl(
    IDirect3DDevice9* self, const D3DVERTEXELEMENT9* elems,
    IDirect3DVertexDeclaration9** ppDecl)
{
    if (!orig_CreateVertexDecl)
        return E_FAIL;
    HRESULT hr = orig_CreateVertexDecl(self, elems, ppDecl);

    if (FAILED(hr)) {
        LONG n = InterlockedIncrement(&g_decl_fail);
        // Count the elements for the report -- the terminator is D3DDECL_END().
        int ne = 0;
        if (elems) while (ne < 64 && elems[ne].Stream != 0xFF) ne++;
        logf("[d3d9] CreateVertexDeclaration FAILED hr=0x%08lX (%d element(s), "
             "failure #%ld)%s", hr, ne, n,
             g_decl_guard ? " -- out-param set to NULL so the title's teardown "
                            "skips it instead of Releasing uninitialised memory"
                          : " -- decl guard OFF, out-param left as the driver left it");
        if (elems && ne > 0 && n <= 3)
            for (int i = 0; i < ne; i++)
                logf("[d3d9]   elem[%d] stream=%u offset=%u type=%u method=%u "
                     "usage=%u index=%u", i, elems[i].Stream, elems[i].Offset,
                     elems[i].Type, elems[i].Method, elems[i].Usage,
                     elems[i].UsageIndex);
        if (g_decl_guard && ppDecl) *ppDecl = NULL;
    } else if (g_trace) {
        logf("[d3d9] CreateVertexDeclaration -> %p", ppDecl ? (void*)*ppDecl : NULL);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateDevice(IDirect3D9* self, UINT adapter,
                                                   D3DDEVTYPE devtype, HWND focus,
                                                   DWORD behavior,
                                                   D3DPRESENT_PARAMETERS* pp,
                                                   IDirect3DDevice9** ppDev)
{
    InterlockedIncrement(&g_n_create);
    if (pp && !pp->Windowed) InterlockedIncrement(&g_n_fullscreen);
    if (focus) g_focus_window = focus;

    if (g_trace) {
        logf("[d3d9] CreateDevice(adapter=%u devtype=%d focus=%p behavior=%08lX)",
             adapter, (int)devtype, focus, behavior);
        log_pp("  requested", pp);
        if (pp && !pp->Windowed)
            logf("[d3d9]   ^ FULLSCREEN -- this is the call that takes the monitor");
    }

    // Per-title opt-out, honoured exactly as on the d3d8 side. Logged rather
    // than silent: "the override did not fire" and "the override fired and did
    // not help" are different findings.
    const char* exc_src = d3d_caller_except_source(_ReturnAddress());
    bool excepted = exc_src != NULL;
    if (excepted && g_windowed && pp && !pp->Windowed)
        logf("[d3d9]   caller is EXCEPTED from the windowed override by %s -- "
             "leaving it FULLSCREEN", exc_src);

    HRESULT hr;
    if (g_windowed && !excepted && pp && !pp->Windowed) {
        D3DPRESENT_PARAMETERS mod = *pp;
        make_windowed(self, adapter, &mod, focus);
        g_game_window = mod.hDeviceWindow ? mod.hDeviceWindow : focus;
        // Record the backbuffer, note the calling title and restyle/resize the
        // window -- all BEFORE the device is built against it. This is the same
        // call the d3d8 path makes, so fakemode, the icon and the absolute mouse
        // mapping now apply to FMO too.
        d3d_prepare_game_window(_ReturnAddress(), g_game_window,
                                mod.BackBufferWidth, mod.BackBufferHeight,
                                g_fitwin);
        hr = orig_CreateDevice(self, adapter, devtype, focus, behavior, &mod, ppDev);
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_n_forced);
            log_pp("  FORCED WINDOWED", &mod);
            // The whole windowed-mode support layer: message spy, title icon,
            // app.dll's input HWND, the delayed window audit, and the mask
            // handling. Without the mask work the Viewer's full-screen
            // PlayOnlineMask stays over the game AND keeps the input, because
            // polcore's CreateInput is bound to it.
            if (ppDev) g_game_device = *ppDev;   // before the Present hook counts
            g_pp_last = mod; g_pp_have = true;   // the device's REAL parameters
            d3d_after_device_created(g_game_window);
            // Shared with the d3d8 path: "is the display held exclusively" is a
            // question about the screen, not about which API took it.
            d3d_note_device_mode(1);
        } else {
            // Same rule as the d3d8 and DirectSound paths: never leave the client
            // worse off than we found it. Replay its own request verbatim.
            InterlockedIncrement(&g_n_retry);
            logf("[d3d9] windowed override rejected (hr=0x%08lX) -- replaying the "
                 "client's own fullscreen request", hr);
            d3d_forget_backbuffer_size();
            hr = orig_CreateDevice(self, adapter, devtype, focus, behavior, pp, ppDev);
            if (SUCCEEDED(hr) && pp) { d3d_note_device_mode(pp->Windowed);
                                       g_pp_last = *pp; g_pp_have = true; }
        }
    } else {
        hr = orig_CreateDevice(self, adapter, devtype, focus, behavior, pp, ppDev);
        if (SUCCEEDED(hr) && pp) { d3d_note_device_mode(pp->Windowed);
                                   g_pp_last = *pp; g_pp_have = true; }

        // A fullscreen request passed through VERBATIM can still fail -- and FMO
        // answers ANY device-init failure by printing "dx" and writing to address
        // zero (0x6100b366 in the unpacked image: a deliberate crash), which
        // surfaces as the CRT abort box and ExitProcess(3). Until 2026-08-18 this
        // branch returned the failure SILENTLY, so the log showed a healthy
        // CreateDevice entry and then a dead client.
        //
        // "Never leave the client worse off" cuts both ways: when the title's own
        // call cannot succeed on this machine, a windowed device is a RESCUE, not
        // an override. A profile that excepts a title from d3d_windowed still gets
        // fullscreen wherever fullscreen is possible (the Deck) -- this path only
        // runs after the real request has already failed.
        if (FAILED(hr) && pp && !pp->Windowed) {
            bool has_mode = adapter_has_mode(self, adapter, pp->BackBufferWidth,
                                             pp->BackBufferHeight);
            logf("[d3d9] the title's own FULLSCREEN CreateDevice FAILED "
                 "(hr=0x%08lX)%s", hr, has_mode ? "" :
                 " -- and the adapter lists NO display mode of that size, so no "
                 "fullscreen phrasing can succeed on this machine");
            if (g_fs_rescue) {
                D3DPRESENT_PARAMETERS mod = *pp;
                make_windowed(self, adapter, &mod, focus);
                g_game_window = mod.hDeviceWindow ? mod.hDeviceWindow : focus;
                d3d_prepare_game_window(_ReturnAddress(), g_game_window,
                                        mod.BackBufferWidth, mod.BackBufferHeight,
                                        g_fitwin);
                hr = orig_CreateDevice(self, adapter, devtype, focus, behavior,
                                       &mod, ppDev);
                if (SUCCEEDED(hr)) {
                    InterlockedIncrement(&g_n_rescued);
                    log_pp("  RESCUED WINDOWED (the fullscreen device was "
                           "unservable)", &mod);
                    if (ppDev) g_game_device = *ppDev;
                    g_pp_last = mod; g_pp_have = true;
                    d3d_after_device_created(g_game_window);
                    d3d_note_device_mode(1);
                } else {
                    logf("[d3d9] windowed rescue ALSO failed (hr=0x%08lX) -- the "
                         "title gets the failure and will act on it", hr);
                    d3d_forget_backbuffer_size();
                }
            } else
                logf("[d3d9] d3d_fs_rescue=0 -- returning the failure verbatim");
        }
    }

    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        InterlockedIncrement(&g_n_device);
        if (!g_dev_hooked) {
            g_dev_hooked = true;               // one attempt, whatever happens
            void** vtbl = *(void***)*ppDev;
            if (patch_slot(vtbl, S9D_Reset, (void*)hook_Reset,
                           (void**)&orig_Reset))
                logf("[d3d9] Reset hooked at device vtable slot %d (orig=%p)",
                     S9D_Reset, orig_Reset);
            else
                orig_Reset = NULL;
            if (g_fakemode &&
                patch_slot(vtbl, S9D_GetDisplayMode, (void*)hook_GetDisplayMode,
                           (void**)&orig_GetDisplayMode))
                logf("[mode] d3d9 GetDisplayMode hooked at device slot %d -- the "
                     "game will be told the screen is its backbuffer",
                     S9D_GetDisplayMode);
            else if (g_fakemode)
                orig_GetDisplayMode = NULL;
            // FMO polls this while lost, and for a title that skips Present
            // during device loss it is the ONLY place the recovery can be driven
            // from the render thread. d3d8hook.cpp has hooked it since the TM
            // zero-draw work; this is the d3d9 twin.
            if (patch_slot(vtbl, S9D_TestCoopLevel, (void*)hook_TestCoopLevel,
                           (void**)&orig_TestCoop))
                logf("[d3d9] TestCooperativeLevel hooked at device vtable slot %d "
                     "(orig=%p) -- the sleep/resume recovery reads the device "
                     "state through it", S9D_TestCoopLevel, orig_TestCoop);
            else
                orig_TestCoop = NULL;
            if (patch_slot(vtbl, S9D_Present, (void*)hook_Present,
                           (void**)&orig_Present))
                logf("[d3d9] Present hooked at device vtable slot %d -- counts the "
                     "GAME's frames separately from the VMR-9 movie's",
                     S9D_Present);
            else
                orig_Present = NULL;
            // And the swap chain, which is the path FMO really uses. Take slot 0
            // from the game's own device so the pointer identifies it later, then
            // Release -- the vtable patch is shared and outlives our reference.
            // The device vtable is shared by every device this process makes, so
            // patch it once. FMO is the only d3d9 title, but the guard is written
            // to be title-agnostic: it only acts on a FAILED creation.
            if (!g_decl_hooked) {
                g_decl_hooked = true;
                if (patch_slot(*(void***)*ppDev, S9D_CreateVertexDeclaration,
                               (void*)hook_CreateVertexDecl,
                               (void**)&orig_CreateVertexDecl))
                    logf("[d3d9] CreateVertexDeclaration hooked at device vtable "
                         "slot %d (orig=%p) -- decl guard %s",
                         S9D_CreateVertexDeclaration, orig_CreateVertexDecl,
                         g_decl_guard ? "ON" : "off");
                else {
                    orig_CreateVertexDecl = NULL;
                    logf("[d3d9] could not hook CreateVertexDeclaration -- the FMO "
                         "teardown crash will not be guarded");
                }
            }

            IDirect3DSwapChain9* sc = NULL;
            if (SUCCEEDED((*ppDev)->GetSwapChain(0, &sc)) && sc) {
                g_game_swapchain = sc;
                if (!g_sc_hooked) {
                    g_sc_hooked = true;
                    if (patch_slot(*(void***)sc, S9SC_Present,
                                   (void*)hook_SCPresent, (void**)&orig_SCPresent))
                        logf("[d3d9] Present hooked at SWAP CHAIN vtable slot %d "
                             "(orig=%p) -- the device hook alone read zero while "
                             "the game was visibly rendering", S9SC_Present,
                             orig_SCPresent);
                    else
                        orig_SCPresent = NULL;
                }
                sc->Release();
            } else {
                logf("[d3d9] GetSwapChain(0) failed -- frame counting will only "
                     "see the device path, so a zero count is NOT evidence");
            }
            if (d3d_renderspy_requested())
                logf("[d3d9] NOTE: d3d_renderspy is set but the render diagnostic "
                     "is d3d8-only (the draw-path slots differ) -- no [rs] lines "
                     "will appear for this d3d9 title");
        }
        // Name the title's device even when no override ran -- see caller_is_title.
        if (caller_is_title(_ReturnAddress()) && g_game_device != *ppDev) {
            g_game_device = *ppDev;
            logf("[d3d9] the GAME's device is %p (created by the title, not by "
                 "the VMR-9 movie renderer) -- frame counting and the "
                 "sleep/resume recovery key on it", (void*)*ppDev);
        }
        // Outside the !g_dev_hooked guard: the vtable is patched once, but the
        // DEVICE POINTER changes when a title recreates its device (FMO creates
        // two and Resets them), and the recovery has to follow the live one.
        // Only ever the title's -- watching the movie's device would have the
        // recovery reading a healthy renderer while the game is frozen.
        if (*ppDev == g_game_device) wake_arm_d3d9(*ppDev, g_game_window);
    }
    return hr;
}

// A successful Reset DESTROYS and recreates the device's swap chain, so the
// g_game_swapchain pointer captured at CreateDevice is now stale (it was already
// Released, and the address may have been reused). Left stale, the game's own frames
// arrive on the NEW swap chain, fail the `self == g_game_swapchain` test in
// hook_SCPresent, and get counted as the VMR-9 movie's -- which also makes the
// summary's "the game presented NO frame" note fire falsely and can mislead the
// fmv_yield decision. Re-capture the current one here. (The DEVICE pointer is stable
// across a Reset -- Reset is a method on it -- so only the swap chain needs this.)
// Same take-pointer-then-release pattern as CreateDevice: we keep only the identity
// value; the device owns its implicit swap chain, so the pointer stays valid.
static void d3d9_recapture_swapchain_after_reset(IDirect3DDevice9* dev)
{
    if (!dev || dev != g_game_device) return;      // only the game's own device
    IDirect3DSwapChain9* sc = NULL;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        g_game_swapchain = sc;
        sc->Release();
    }
}

// Reset is how a device changes mode after creation (alt-tab, an in-game
// resolution change). Without this the game can put itself back to fullscreen
// after we made it windowed.
static HRESULT STDMETHODCALLTYPE hook_Reset(IDirect3DDevice9* self,
                                            D3DPRESENT_PARAMETERS* pp)
{
    InterlockedIncrement(&g_n_reset);
    // The title honouring the lost-device contract itself. Noted BEFORE the call
    // so a Reset that hangs or aborts still counts as an attempt -- the recovery
    // stands down on the attempt, not on its outcome.
    wake_note_reset(self, S_OK);
    if (g_trace) log_pp("Reset requested", pp);

    // Same opt-out as CreateDevice. A title excluded there must be excluded here
    // too, or its first Reset would silently drag it back into windowed mode.
    if (g_windowed && !d3d_caller_excepted(_ReturnAddress()) && pp && !pp->Windowed) {
        D3DPRESENT_PARAMETERS mod = *pp;
        IDirect3D9* d3d = NULL;
        if (FAILED(self->GetDirect3D(&d3d))) d3d = NULL;
        make_windowed(d3d, 0, &mod, g_focus_window);
        if (d3d) d3d->Release();
        HWND w = mod.hDeviceWindow ? mod.hDeviceWindow : g_focus_window;
        if (w) g_game_window = w;
        // Re-record the backbuffer as well as re-fitting: a Reset is how a game
        // changes resolution, and a stale record would leave fakemode and the
        // absolute mouse mapping describing the PREVIOUS field.
        d3d_prepare_game_window(NULL, g_game_window, mod.BackBufferWidth,
                                mod.BackBufferHeight, g_fitwin);
        HRESULT hr = orig_Reset(self, &mod);
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_n_forced);
            log_pp("  Reset FORCED WINDOWED", &mod);
            g_pp_last = mod; g_pp_have = true;   // the device's real parameters now
            // A Reset is exactly when the Viewer tends to put the mask back up,
            // so re-align rather than assuming CreateDevice's pass still holds.
            if (g_game_window) d3d_set_game_window(g_game_window);
            d3d_note_device_mode(1);
            d3d9_recapture_swapchain_after_reset(self);
            return hr;
        }
        InterlockedIncrement(&g_n_retry);
        logf("[d3d9] windowed Reset rejected (hr=0x%08lX) -- replaying original", hr);
        d3d_forget_backbuffer_size();
        hr = orig_Reset(self, pp);
        if (SUCCEEDED(hr)) { if (pp) { d3d_note_device_mode(pp->Windowed);
                                       g_pp_last = *pp; g_pp_have = true; }
                             d3d9_recapture_swapchain_after_reset(self); }
        return hr;
    }
    {
        HRESULT hr = orig_Reset(self, pp);
        if (SUCCEEDED(hr)) { if (pp) { d3d_note_device_mode(pp->Windowed);
                                       g_pp_last = *pp; g_pp_have = true; }
                             d3d9_recapture_swapchain_after_reset(self); }
        return hr;
    }
}

// --- sleep/resume recovery: the d3d9 half of wakerecover.cpp -----------------
//
// The API-specific half only. The policy -- how long the title gets to recover
// itself, when to give up, what an INVALIDCALL means -- lives in wakerecover.cpp
// so this and the d3d8 path cannot drift, which is the failure the FMV-adoption
// note in d3d8hook.cpp records happening twice already.
//
// Both callbacks go through orig_*, never the live vtable slot: the slot holds
// OUR hook, and re-entering it would re-apply the windowed rewrite and re-notify
// the pump from inside itself.
static HRESULT d3d9_wake_test(void* dev)
{
    if (!orig_TestCoop) return E_NOTIMPL;
    return orig_TestCoop((IDirect3DDevice9*)dev);
}

static HRESULT d3d9_wake_reset(void* dev)
{
    if (!orig_Reset || !g_pp_have) return E_NOTIMPL;
    // A COPY: Reset may write back into the structure, and g_pp_last has to keep
    // describing the device for the next attempt.
    D3DPRESENT_PARAMETERS pp = g_pp_last;
    HRESULT hr = orig_Reset((IDirect3DDevice9*)dev, &pp);
    if (SUCCEEDED(hr)) {
        d3d_note_device_mode(pp.Windowed);
        // A successful Reset destroys and recreates the implicit swap chain, so
        // the captured pointer is stale -- and a stale one makes FMO's own frames
        // count as the movie's, which is exactly what would tell the recovery the
        // title had come back when it had not.
        d3d9_recapture_swapchain_after_reset((IDirect3DDevice9*)dev);
        if (g_game_window) d3d_set_game_window(g_game_window);
    }
    return hr;
}

static void wake_arm_d3d9(IDirect3DDevice9* dev, HWND wnd)
{
    if (!dev || !wake_enabled()) return;
    wake_register_device("d3d9", dev,
                         orig_TestCoop ? d3d9_wake_test : NULL,
                         (orig_Reset && g_pp_have) ? d3d9_wake_reset : NULL, wnd);
}

// The factory. Patch the vtable of the first IDirect3D9 handed out; every device
// in the process comes through this one object's CreateDevice.
static IDirect3D9* WINAPI hook_Direct3DCreate9(UINT sdk)
{
    IDirect3D9* p = real_Direct3DCreate9(sdk);
    logf("[d3d9] Direct3DCreate9(SDK=%u) -> %p", sdk, p);
    if (!p) return p;

    // VERIFY the slot, do not merely remember having armed it -- the same fault
    // measured on the d3d8 side on 2026-09-09, where the second launch of a title
    // in one Viewer session found the slot taken and never re-armed, so the
    // forced-windowed rewrite silently stopped and the title failed to start.
    // See the long note in d3d8hook.cpp's hook_Direct3DCreate8. FMO is the only
    // d3d9 title and it does need the rewrite, so it is exposed the same way;
    // WARNING: unlike the d3d8 case this one is REASONED FROM SHARED CODE SHAPE, not
    // measured -- no captured log holds an FMO relaunch.
    void** vt  = *(void***)p;
    void*  cur = vt[S9_CreateDevice];
    if (cur == (void*)hook_CreateDevice) return p;         // still ours

    // g_vtbl_hooked with no original means the one validation attempt already
    // failed; keep that verdict rather than re-testing on every create.
    if (g_vtbl_hooked && !orig_CreateDevice) return p;

    const bool rearm = (orig_CreateDevice != NULL);
    if (rearm) {
        char who[200];
        dx_describe_code(cur, who, sizeof(who));
        logf("[d3d9] CreateDevice slot %d is NOT ours any more: it holds %s "
             "(we wrote hook=%p over orig=%p). RE-ARMING over what is there now.",
             S9_CreateDevice, who, (void*)hook_CreateDevice, orig_CreateDevice);
    }
    g_vtbl_hooked = true;                       // one attempt, whatever happens
    if (!validate_d3d9(p)) return p;
    if (patch_slot_cas(vt, S9_CreateDevice, (void*)hook_CreateDevice, cur,
                       (void**)&orig_CreateDevice))
        logf("[d3d9] CreateDevice %s at vtable slot %d (orig=%p) -- this is "
             "FMO's renderer; the d3d8 hook never sees it",
             rearm ? "RE-ARMED" : "hooked", S9_CreateDevice, orig_CreateDevice);
    else
        // orig_CreateDevice is untouched on failure, so a lost re-arm race keeps
        // the original it already had rather than disarming itself.
        logf("[d3d9] could not arm the d3d9 CreateDevice slot (VirtualProtect "
             "failed, or another thread wrote it first) -- %s",
             rearm ? "the previous arming stands" : "not hooked");
    return p;
}

void* d3d9_real_Direct3DCreate9() { return (void*)real_Direct3DCreate9; }
void* d3d9_hook_Direct3DCreate9() { return (void*)hook_Direct3DCreate9; }

void d3d9_resolve()
{
    if (!g_enable) return;
    HMODULE m = LoadLibraryW(L"d3d9.dll");
    if (m) real_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(m, "Direct3DCreate9");
    logf("[d3d9] enable=1 trace=%d windowed=%d fitwindow=%d fakemode=%d | "
         "Direct3DCreate9=%p (the rest of the [dx] d3d_* settings are applied by "
         "the shared layer in d3d8hook.cpp)",
         g_trace, g_windowed, g_fitwin, g_fakemode, real_Direct3DCreate9);
    if (!real_Direct3DCreate9) {
        logf("[d3d9] WARN: Direct3DCreate9 unresolved -- the FMO hook is dead");
        return;
    }
    // EAT patch for the same reason as d3d8: FrontMissionOnline.dll is
    // POL1-packed and re-resolves its imports after unpacking, which an IAT
    // patch alone loses to. Resolve FIRST, then patch, or the hook calls itself.
    dx_eat_patch_export(m, "Direct3DCreate9", (void*)hook_Direct3DCreate9,
                        "d3d9!Direct3DCreate9 (FMO is the only d3d9 title)");
}

void d3d9_summary()
{
    if (!g_enable) return;
    logf("[d3d9] summary: CreateDevice=%ld (fullscreen requests=%ld) devices=%ld "
         "Reset=%ld forced-windowed=%ld retries=%ld rescued=%ld mode-faked=%ld",
         g_n_create, g_n_fullscreen, g_n_device, g_n_reset, g_n_forced, g_n_retry,
         g_n_rescued, g_n_modefaked);
    logf("[d3d9] frames: GAME presented %ld, movie/other %ld (device hook=%s, "
         "swap-chain hook=%s)",
         g_n_present_game, g_n_present_other,
         orig_Present ? "on" : "OFF", orig_SCPresent ? "on" : "OFF");
    // Only a conclusion when BOTH paths were actually watched. The device hook
    // alone once reported a confident zero for a game that was rendering fine,
    // because FMO presents through its swap chain -- so an unwatched path must
    // downgrade this from a finding to "cannot tell".
    if (g_n_device && !g_n_present_game) {
        if (orig_Present && orig_SCPresent)
            logf("[d3d9] NOTE: the game presented NO frame on either the device or "
                 "the swap-chain path. A black or unpainted window is then "
                 "expected -- the title is blocked before its render loop.");
        else
            logf("[d3d9] NOTE: zero game frames counted, but one of the two present "
                 "paths was NOT hooked -- this is 'cannot tell', NOT 'it never "
                 "rendered'.");
    }
    if (!g_n_create)
        logf("[d3d9] NOTE: CreateDevice never ran. Either no d3d9 title launched "
             "(only FMO is d3d9 -- TM, FFXI and Fantasy Earth are d3d8), or the "
             "EAT patch on d3d9.dll was bypassed.");
    else if (g_n_fullscreen && !g_n_forced)
        logf("[d3d9] NOTE: %ld fullscreen request(s) seen and NONE overridden -- "
             "set [dx] d3d_windowed=1 (or d3d9_windowed=1).", g_n_fullscreen);
}
