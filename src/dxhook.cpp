// dxhook.cpp -- DirectDraw / DirectSound interposition for the PlayOnline Viewer.
//
// This is the modernisation half of the port effort, and it is deliberately the
// half that needs NO reverse engineering: the Viewer reaches its presentation
// layer through published Microsoft ABIs, so we can sit on that boundary with
// exact type information and change behaviour without understanding a single
// line of Square Enix code.
//
// What the client actually imports (measured from the unpacked images --
// these are the ONLY DirectX entry points in the client):
//
//     app.dll    DDRAW!DirectDrawCreateEx, DSOUND!#11, DINPUT8!DirectInput8Create
//     pol.exe    DDRAW!DirectDrawCreateEx, DDRAW!DirectDrawEnumerateExA
//     polcore    DINPUT8!DirectInput8Create
//
// DSOUND ordinal #11 is DirectSoundCreate8 (verified against the live
// SysWOW64\dsound.dll export table). So the whole audio and video surface of the
// Viewer is two factory functions, which is why this is tractable.
//
// Delivery is the injector's existing by-value IAT patch (see inject.cpp,
// patch_iat) -- the same mechanism already proven on ws2_32, which the client
// also imports by ordinal. By-value matching does not care about ordinals,
// names, or binding.
//
// Two behaviours are implemented, both off unless [dx] enable=1:
//
//   sound_globalfocus  OR DSBCAPS_GLOBALFOCUS into every secondary sound buffer,
//                      which is what makes audio survive focus loss. Without it
//                      DirectSound mutes a buffer the moment its owning window
//                      goes to the background.
//   sound_coop         Downgrade SetCooperativeLevel(DSSCL_EXCLUSIVE) to
//                      DSSCL_PRIORITY. An exclusive-mode app is muted on focus
//                      loss regardless of GLOBALFOCUS, so the two go together.
//
// DirectDraw is wrapped but NOT altered yet: v1 forwards every method and logs
// the ones that decide windowing (SetCooperativeLevel, SetDisplayMode). We need
// to see what the Viewer actually asks for before changing it -- exclusive
// fullscreen and windowed need different fixes, and guessing wrong here is a
// black screen, not a log line.

#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION  0x0700
#define DIRECTSOUND_VERSION 0x0800
#include <windows.h>
#include <mmreg.h>          // WAVEFORMATEX -- WIN32_LEAN_AND_MEAN keeps mmsystem.h
                            // out, and dsound.h uses the type without declaring it
#include <ddraw.h>
#include <dsound.h>
#include <new>
#include "polshim.h"

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

static int g_dx_enable       = 0;
static int g_dx_trace        = 0;
static int g_dx_globalfocus  = 1;   // only consulted when g_dx_enable
static int g_dx_coop         = 1;
static int g_shell_scale     = 0;   // 0 = auto (largest integer scale that fits the desktop)
static int g_shell_scale_en  = 1;   // enlarge the DirectDraw shell window to an integer multiple
static volatile LONG g_shell_present_tick = 0;   // GetTickCount() of the last shell present

// counters, reported on unload -- "the hook never installed" and "the hook
// installed but the client never called it" must not look the same in the log.
static LONG g_n_dscreate   = 0;
static LONG g_n_buffers    = 0;
static LONG g_n_focus_set  = 0;
static LONG g_n_coop_down  = 0;
static LONG g_n_ddcreate   = 0;
static LONG g_n_retry      = 0;

int dx_enabled() { return g_dx_enable; }

void dx_configure(const wchar_t* ini)
{
    g_dx_enable      = GetPrivateProfileIntW(L"dx", L"enable",            0, ini);
    g_dx_trace       = GetPrivateProfileIntW(L"dx", L"trace",             trace_at(1), ini);
    g_dx_globalfocus = GetPrivateProfileIntW(L"dx", L"sound_globalfocus", 1, ini);
    g_dx_coop        = GetPrivateProfileIntW(L"dx", L"sound_coop",        1, ini);
    g_shell_scale    = GetPrivateProfileIntW(L"dx", L"shell_scale",        0, ini);
    g_shell_scale_en = GetPrivateProfileIntW(L"dx", L"shell_scale_enable", 1, ini);
    dpi_declare(GetPrivateProfileIntW(L"dx", L"dpi_aware", 1, ini));
}

// Live re-read for the settings dialog: only values consulted on every call, so
// a new value takes effect at once. [dx] enable, shell_scale_enable and
// dpi_aware are NOT re-read -- enable decided at startup whether the IAT patch
// installed, shell_scale_enable is a startup decision too, and DPI awareness is
// a one-shot process property (dpi_declare must run before the first window).
void dx_reload(const wchar_t* ini)
{
    g_dx_trace       = GetPrivateProfileIntW(L"dx", L"trace",             trace_at(1), ini);
    g_dx_globalfocus = GetPrivateProfileIntW(L"dx", L"sound_globalfocus", 1, ini);
    g_dx_coop        = GetPrivateProfileIntW(L"dx", L"sound_coop",        1, ini);
    g_shell_scale    = GetPrivateProfileIntW(L"dx", L"shell_scale",        0, ini);
    logf("[reload] dx: trace=%d globalfocus=%d coop=%d shell_scale=%d",
         g_dx_trace, g_dx_globalfocus, g_dx_coop, g_shell_scale);
}

// ---------------------------------------------------------------------------
// HiDPI: tell Windows we mean physical pixels.
// ---------------------------------------------------------------------------
//
// A 2003 binary declares no DPI awareness, so on any display above 100% Windows
// runs it in DPI VIRTUALISATION: the app is told the screen is smaller than it
// is, renders at that size, and Windows BITMAP-STRETCHES the result to the real
// pixels. For a game that is already stretching a 640x480 backbuffer up to its
// window, that is a SECOND resample on top of the first -- the "why is it so
// blurry" half of the HiDPI complaint, and it is pure loss: the first stretch
// already produced the pixels the second one smears.
//
// It is also a cursor bug. Virtualised, GetCursorPos and GetClientRect come back
// in logical units while the pointer the user sees moves in physical ones, so
// the absolute mapping in dinputhook is fitting the pointer into a field of the
// wrong size. Declaring awareness puts both in the same (physical) space.
//
// PER_MONITOR_AWARE_V2 is asked for first so dragging between a 4K and a 1080p
// monitor is handled; the older per-monitor context and finally the ancient
// SetProcessDPIAware are the fallbacks for pre-1703 systems. All three are
// resolved dynamically -- this DLL still has to load on the Windows versions
// that have none of them.
//
// TIMING IS THE WHOLE TRICK: this must run before the process creates its first
// window or caches its first metric. dx_configure is called from the shim's
// init, which runs from PolHook's DllMain at load time, so it does.
// The outcome is RECORDED, not logged: dx_configure runs from the shim's init at
// a point where log_open() has not happened yet, so a logf here goes into a NULL
// FILE* and vanishes. That cost a "the DPI fix did nothing" diagnosis on the
// first live run -- the call had worked perfectly and only its evidence was
// missing. inject.cpp calls dpi_log_result() once the log exists.
static char g_dpi_result[256] = "";

void dpi_log_result()
{
    if (g_dpi_result[0]) logf("%s", g_dpi_result);
}

#define DPI_DONE(...) do { _snprintf_s(g_dpi_result, _TRUNCATE, __VA_ARGS__); return; } while (0)

void dpi_declare(int mode)
{
    if (!mode) DPI_DONE("[dpi] left DPI-UNAWARE by [dx] dpi_aware=0 -- Windows will "
                        "bitmap-stretch the window on any display above 100%%");

    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
        PFN_SPDAC set_ctx = (PFN_SPDAC)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (set_ctx) {
            // (HANDLE)-4 = PER_MONITOR_AWARE_V2, (HANDLE)-3 = PER_MONITOR_AWARE.
            // Spelled numerically because the macros need a recent SDK header and
            // this file is built against whatever is installed.
            if (set_ctx((HANDLE)-4)) DPI_DONE("[dpi] per-monitor v2: the window is now "
                                              "physical pixels, no bitmap stretch");
            if (set_ctx((HANDLE)-3)) DPI_DONE("[dpi] per-monitor v1");
        }
    }
    HMODULE shc = LoadLibraryW(L"shcore.dll");
    if (shc) {
        typedef HRESULT (WINAPI *PFN_SPDA)(int);
        PFN_SPDA spda = (PFN_SPDA)GetProcAddress(shc, "SetProcessDpiAwareness");
        if (spda && SUCCEEDED(spda(2)))     // 2 = PROCESS_PER_MONITOR_DPI_AWARE
            DPI_DONE("[dpi] per-monitor (shcore)");
    }
    typedef BOOL (WINAPI *PFN_SPDA0)(void);
    PFN_SPDA0 legacy = u32 ? (PFN_SPDA0)GetProcAddress(u32, "SetProcessDPIAware") : NULL;
    if (legacy && legacy()) DPI_DONE("[dpi] system-DPI aware (legacy)");
    DPI_DONE("[dpi] could not declare DPI awareness -- the OS will scale the window");
}
#undef DPI_DONE

#define DXLOG(...) do { if (g_dx_trace) logf(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// real entry points
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_DSC8)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_DDCEX)(GUID FAR*, LPVOID*, REFIID, IUnknown FAR*);

static PFN_DSC8  real_DirectSoundCreate8 = NULL;
static PFN_DDCEX real_DirectDrawCreateEx = NULL;

// Exposed so inject.cpp's patch_iat can add them to its by-value swap map.
void* dx_real_DirectSoundCreate8() { return (void*)real_DirectSoundCreate8; }
void* dx_real_DirectDrawCreateEx() { return (void*)real_DirectDrawCreateEx; }

// ---------------------------------------------------------------------------
// IDirectSound8 wrapper
// ---------------------------------------------------------------------------
//
// Single inheritance from the abstract interface gives us the exact COM vtable
// layout for free under MSVC, so there is no hand-built vtable to get wrong --
// the compiler is matching the same header DirectSound itself was built against.

class DSWrap : public IDirectSound8
{
    IDirectSound8* m_real;
    LONG           m_ref;
    DWORD          m_level;         // last cooperative level we let through
public:
    explicit DSWrap(IDirectSound8* real) : m_real(real), m_ref(1), m_level(0) {}

    // --- IUnknown ---
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        // Identity: hand back ourselves for the interfaces we implement, so the
        // client cannot QI its way around the wrapper and reach the raw object.
        if (riid == IID_IUnknown || riid == IID_IDirectSound || riid == IID_IDirectSound8) {
            *ppv = static_cast<IDirectSound8*>(this);
            AddRef();
            return S_OK;
        }
        // Anything else is forwarded UNWRAPPED and logged. Nothing the Viewer is
        // known to use lands here; if something does, the log names the IID.
        HRESULT hr = m_real->QueryInterface(riid, ppv);
        if (SUCCEEDED(hr)) {
            char n[64];
            DXLOG("[dx] DSWrap: unwrapped QI for %s", iid_name(riid, n, sizeof(n)));
        }
        return hr;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) {
            m_real->Release();
            delete this;
            return 0;
        }
        return (ULONG)r;
    }

    // --- IDirectSound ---
    STDMETHODIMP CreateSoundBuffer(LPCDSBUFFERDESC pcDesc,
                                   LPDIRECTSOUNDBUFFER* ppBuf, LPUNKNOWN pUnk)
    {
        InterlockedIncrement(&g_n_buffers);

        if (!pcDesc || !g_dx_globalfocus)
            return m_real->CreateSoundBuffer(pcDesc, ppBuf, pUnk);

        // A primary buffer must not carry GLOBALFOCUS -- the flag is defined for
        // secondary buffers only and DirectSound rejects the desc outright.
        // A descriptor larger than the version we compiled against is passed
        // through untouched rather than truncated into a copy.
        if ((pcDesc->dwFlags & DSBCAPS_PRIMARYBUFFER) ||
            pcDesc->dwSize > sizeof(DSBUFFERDESC))
            return m_real->CreateSoundBuffer(pcDesc, ppBuf, pUnk);

        DSBUFFERDESC d;
        ZeroMemory(&d, sizeof(d));
        memcpy(&d, pcDesc, pcDesc->dwSize);     // keeps d.dwSize == caller's dwSize

        DWORD before = d.dwFlags;
        d.dwFlags |= DSBCAPS_GLOBALFOCUS;
        // STICKYFOCUS is the weaker sibling (audible over non-DirectSound apps
        // only) and the two are mutually exclusive; GLOBALFOCUS supersedes it.
        d.dwFlags &= ~DSBCAPS_STICKYFOCUS;

        HRESULT hr = m_real->CreateSoundBuffer(&d, ppBuf, pUnk);
        if (SUCCEEDED(hr)) {
            if (before != d.dwFlags) InterlockedIncrement(&g_n_focus_set);
            DXLOG("[dx] CreateSoundBuffer %lu bytes flags %08lX -> %08lX (+GLOBALFOCUS)",
                  d.dwBufferBytes, before, d.dwFlags);
            return hr;
        }

        // Never make the client worse than it was. If the driver refuses our
        // modified descriptor, replay the caller's original and let its own
        // error handling see the genuine result.
        InterlockedIncrement(&g_n_retry);
        logf("[dx] CreateSoundBuffer rejected our flags (hr=0x%08lX, flags %08lX); "
             "retrying with the client's original %08lX", hr, d.dwFlags, before);
        return m_real->CreateSoundBuffer(pcDesc, ppBuf, pUnk);
    }

    STDMETHODIMP GetCaps(LPDSCAPS p) { return m_real->GetCaps(p); }

    STDMETHODIMP DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER a, LPDIRECTSOUNDBUFFER* b)
    {
        // A duplicate inherits the original's flags, so a buffer we already
        // marked GLOBALFOCUS stays global. Nothing to do but forward.
        return m_real->DuplicateSoundBuffer(a, b);
    }

    STDMETHODIMP SetCooperativeLevel(HWND hwnd, DWORD level)
    {
        DWORD want = level;
        // DSSCL_WRITEPRIMARY is only legal at DSSCL_EXCLUSIVE, so a client that
        // asks for it is left strictly alone.
        if (g_dx_coop && level == DSSCL_EXCLUSIVE) want = DSSCL_PRIORITY;

        HRESULT hr = m_real->SetCooperativeLevel(hwnd, want);
        if (FAILED(hr) && want != level) {
            InterlockedIncrement(&g_n_retry);
            logf("[dx] SetCooperativeLevel(PRIORITY) failed (hr=0x%08lX); "
                 "restoring the client's EXCLUSIVE request", hr);
            want = level;
            hr = m_real->SetCooperativeLevel(hwnd, level);
        }
        if (SUCCEEDED(hr)) {
            m_level = want;
            if (want != level) InterlockedIncrement(&g_n_coop_down);
        }
        DXLOG("[dx] DS SetCooperativeLevel(hwnd=%p, %lu%s) -> 0x%08lX",
              hwnd, want, (want != level) ? " [was EXCLUSIVE]" : "", hr);
        return hr;
    }

    STDMETHODIMP Compact()                      { return m_real->Compact(); }
    STDMETHODIMP GetSpeakerConfig(LPDWORD p)    { return m_real->GetSpeakerConfig(p); }
    STDMETHODIMP SetSpeakerConfig(DWORD c)      { return m_real->SetSpeakerConfig(c); }
    STDMETHODIMP Initialize(LPCGUID g)          { return m_real->Initialize(g); }

    // --- IDirectSound8 ---
    STDMETHODIMP VerifyCertification(LPDWORD p) { return m_real->VerifyCertification(p); }
};

static HRESULT WINAPI hook_DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8* ppDS8,
                                              LPUNKNOWN pUnk)
{
    InterlockedIncrement(&g_n_dscreate);
    HRESULT hr = real_DirectSoundCreate8(guid, ppDS8, pUnk);
    if (FAILED(hr) || !ppDS8 || !*ppDS8) {
        DXLOG("[dx] DirectSoundCreate8 -> 0x%08lX (not wrapped)", hr);
        return hr;
    }
    DSWrap* w = new (std::nothrow) DSWrap(*ppDS8);
    if (!w) return hr;                      // out of memory: leave the client intact
    *ppDS8 = static_cast<IDirectSound8*>(w);
    logf("[dx] DirectSoundCreate8 wrapped (globalfocus=%d coop=%d)",
         g_dx_globalfocus, g_dx_coop);
    return hr;
}

// ---------------------------------------------------------------------------
// IDirectDraw7 wrapper -- observational in v1
// ---------------------------------------------------------------------------

static const char* ddscl_names(DWORD f, char* buf, size_t cb)
{
    buf[0] = '\0';
    struct { DWORD bit; const char* name; } t[] = {
        { DDSCL_FULLSCREEN,     "FULLSCREEN" },
        { DDSCL_EXCLUSIVE,      "EXCLUSIVE" },
        { DDSCL_NORMAL,         "NORMAL" },
        { DDSCL_ALLOWMODEX,     "ALLOWMODEX" },
        { DDSCL_ALLOWREBOOT,    "ALLOWREBOOT" },
        { DDSCL_NOWINDOWCHANGES,"NOWINDOWCHANGES" },
        { DDSCL_MULTITHREADED,  "MULTITHREADED" },
        { DDSCL_FPUSETUP,       "FPUSETUP" },
        { DDSCL_FPUPRESERVE,    "FPUPRESERVE" },
    };
    for (int i = 0; i < _countof(t); i++) {
        if (!(f & t[i].bit)) continue;
        if (buf[0]) strncat_s(buf, cb, "|", _TRUNCATE);
        strncat_s(buf, cb, t[i].name, _TRUNCATE);
    }
    if (!buf[0]) strncpy_s(buf, cb, "0", _TRUNCATE);
    return buf;
}

// ---------------------------------------------------------------------------
// surface present interception -- vtable hook shared across every surface
// ---------------------------------------------------------------------------
// The shell presents by blitting its 640x480 back buffer to the windowed primary.
// Surfaces are handed back genuine (CreateSurface below), so rather than wrap the
// ~50-method IDirectDrawSurface7 we patch three slots of its vtable once: Blt(5),
// BltFast(7), Flip(11). Every surface from this driver shares the vtable, so a single
// patch covers the primary and all offscreen surfaces. Trace-only for now; the scaling
// edit to the Blt destination lands once the live trace shows the present pattern.
typedef HRESULT (WINAPI* PFN_Blt)(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT (WINAPI* PFN_BltFast)(IDirectDrawSurface7*, DWORD, DWORD, IDirectDrawSurface7*, LPRECT, DWORD);
typedef HRESULT (WINAPI* PFN_Flip)(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);

static PFN_Blt     real_Blt      = NULL;
static PFN_BltFast real_BltFast  = NULL;
static PFN_Flip    real_Flip     = NULL;
static LONG        g_surf_hooked = 0;
static IDirectDrawSurface7* g_primary = NULL;   // the windowed primary = the present target

static void rect_or_zero(LPRECT r, RECT* out)
{ if (r) *out = *r; else { out->left = out->top = out->right = out->bottom = 0; } }

// A blit whose destination is the primary IS the present. Those are the only ones we
// log (the UI's compositing blits to the back buffer would flood), and the only ones
// the scaling edit will touch.
static HRESULT WINAPI Blt_hook(IDirectDrawSurface7* This, LPRECT dst,
                               IDirectDrawSurface7* src, LPRECT srcR, DWORD flags, LPDDBLTFX fx)
{
    if (This == g_primary) g_shell_present_tick = (LONG)GetTickCount();   // shell is presenting
    if (g_dx_trace && This == g_primary) {
        RECT d, s; rect_or_zero(dst, &d); rect_or_zero(srcR, &s);
        DXLOG("[dx] Blt->PRIMARY dst=(%ld,%ld,%ld,%ld) src=%p srcR=(%ld,%ld,%ld,%ld) flags=%08lX",
              d.left, d.top, d.right, d.bottom, src, s.left, s.top, s.right, s.bottom, flags);
    }
    // THE CONTROLLER MAPPER IS DRAWN INTO THE BACK BUFFER, BEFORE THE BLIT.
    //
    // The first cut drew it onto the PRIMARY after real_Blt, and it flickered badly and
    // fought the shell's own UI (reported live from the Deck, 2026-08-19). That is not a
    // tuning problem, it is a race: the shell blits a full 640x480 frame over the primary
    // and we then paint on top of what is ALREADY on screen, so every frame is displayed
    // once without the overlay and once with it, and any partial blit the shell makes in
    // between lands on our pixels.
    //
    // Painting the SOURCE surface instead makes the overlay part of the frame: the shell
    // finishes its picture, we add ours, and the one Blt carries both to the screen
    // atomically. No intermediate state is ever visible and nothing composites over us.
    // The shell redraws its back buffer each frame, so it also erases us for free.
    if (src && This == g_primary && padoverlay_active()) padoverlay_draw(src, NULL);
    HRESULT hr = real_Blt(This, dst, src, srcR, flags, fx);
    // No src to paint into (a colour-fill blit, say) -- fall back to the primary so the
    // mapper is still visible rather than silently absent.
    if (!src && This == g_primary && padoverlay_active()) padoverlay_draw(This, dst);
    return hr;
}

static HRESULT WINAPI BltFast_hook(IDirectDrawSurface7* This, DWORD x, DWORD y,
                                   IDirectDrawSurface7* src, LPRECT srcR, DWORD flags)
{
    if (g_dx_trace && This == g_primary) {
        RECT s; rect_or_zero(srcR, &s);
        DXLOG("[dx] BltFast->PRIMARY at=(%lu,%lu) src=%p srcR=(%ld,%ld,%ld,%ld) flags=%08lX",
              x, y, src, s.left, s.top, s.right, s.bottom, flags);
    }
    // Same rule as Blt: into the source, before the copy, so the overlay arrives as part
    // of the frame rather than racing it.
    if (src && This == g_primary && padoverlay_active()) padoverlay_draw(src, NULL);
    return real_BltFast(This, x, y, src, srcR, flags);
}

static HRESULT WINAPI Flip_hook(IDirectDrawSurface7* This, IDirectDrawSurface7* target, DWORD flags)
{
    if (g_dx_trace) DXLOG("[dx] Flip this=%p target=%p flags=%08lX", This, target, flags);
    HRESULT hr = real_Flip(This, target, flags);
    // The EXCLUSIVE-fullscreen present. A flip chain never blits to the primary, so
    // without this the overlay would be invisible in precisely the configuration it was
    // written for. After the flip `This` is the newly visible front buffer, so the
    // drawing lands on the frame now on screen. No dst rect exists here -- the surface
    // is the whole screen, and padoverlay_draw reads its size instead.
    if (This == g_primary && padoverlay_active()) padoverlay_draw(This, NULL);
    return hr;
}

static void install_surface_hooks(IDirectDrawSurface7* surf)
{
    if (!surf || InterlockedCompareExchange(&g_surf_hooked, 1, 0) != 0) return;
    void** vtbl = *(void***)surf;
    DWORD old;
    if (VirtualProtect(&vtbl[5], sizeof(void*) * 7, PAGE_EXECUTE_READWRITE, &old)) {
        real_Blt     = (PFN_Blt)     vtbl[5];  vtbl[5]  = (void*)Blt_hook;
        real_BltFast = (PFN_BltFast) vtbl[7];  vtbl[7]  = (void*)BltFast_hook;
        real_Flip    = (PFN_Flip)    vtbl[11]; vtbl[11] = (void*)Flip_hook;
        VirtualProtect(&vtbl[5], sizeof(void*) * 7, old, &old);
        logf("[dx] surface vtable hooks installed (Blt=5 BltFast=7 Flip=11)");
        log_flush();
    } else {
        g_surf_hooked = 0;
        logf("[dx] surface vtable VirtualProtect FAILED -- present not hooked");
    }
}

class DDWrap : public IDirectDraw7
{
    IDirectDraw7* m_real;
    LONG          m_ref;
public:
    explicit DDWrap(IDirectDraw7* real) : m_real(real), m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectDraw7) {
            *ppv = static_cast<IDirectDraw7*>(this);
            AddRef();
            return S_OK;
        }
        HRESULT hr = m_real->QueryInterface(riid, ppv);
        if (SUCCEEDED(hr)) {
            char n[64];
            // Worth knowing: a QI to IDirectDraw / IDirectDraw2 / IDirectDraw4
            // escapes the wrapper, and every later call on it is invisible here.
            logf("[dx] DDWrap: unwrapped QI for %s -- calls on it are NOT traced",
                 iid_name(riid, n, sizeof(n)));
        }
        return hr;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { m_real->Release(); delete this; return 0; }
        return (ULONG)r;
    }

    STDMETHODIMP Compact() { return m_real->Compact(); }
    STDMETHODIMP CreateClipper(DWORD a, LPDIRECTDRAWCLIPPER* b, IUnknown* c)
        { return m_real->CreateClipper(a, b, c); }
    STDMETHODIMP CreatePalette(DWORD a, LPPALETTEENTRY b, LPDIRECTDRAWPALETTE* c, IUnknown* d)
        { return m_real->CreatePalette(a, b, c, d); }

    STDMETHODIMP CreateSurface(LPDDSURFACEDESC2 d, LPDIRECTDRAWSURFACE7* s, IUnknown* u)
    {
        HRESULT hr = m_real->CreateSurface(d, s, u);
        // Surfaces are handed back genuine; we patch the shared vtable once (Blt/
        // BltFast/Flip) to see -- and later scale -- the present, instead of wrapping.
        if (SUCCEEDED(hr) && s && *s) {
            if (d && (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
                g_primary = *s;                        // remember the present target
            install_surface_hooks(*s);
        }
        if (d && g_dx_trace) {
            DXLOG("[dx] CreateSurface caps=%08lX %lux%lu -> 0x%08lX",
                  (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0,
                  (d->dwFlags & DDSD_WIDTH)  ? d->dwWidth  : 0,
                  (d->dwFlags & DDSD_HEIGHT) ? d->dwHeight : 0, hr);
        }
        return hr;
    }

    STDMETHODIMP DuplicateSurface(LPDIRECTDRAWSURFACE7 a, LPDIRECTDRAWSURFACE7* b)
        { return m_real->DuplicateSurface(a, b); }
    STDMETHODIMP EnumDisplayModes(DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMMODESCALLBACK2 d)
        { return m_real->EnumDisplayModes(a, b, c, d); }
    STDMETHODIMP EnumSurfaces(DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMSURFACESCALLBACK7 d)
        { return m_real->EnumSurfaces(a, b, c, d); }
    STDMETHODIMP FlipToGDISurface() { return m_real->FlipToGDISurface(); }
    STDMETHODIMP GetCaps(LPDDCAPS a, LPDDCAPS b) { return m_real->GetCaps(a, b); }
    STDMETHODIMP GetDisplayMode(LPDDSURFACEDESC2 a) { return m_real->GetDisplayMode(a); }
    STDMETHODIMP GetFourCCCodes(LPDWORD a, LPDWORD b) { return m_real->GetFourCCCodes(a, b); }
    STDMETHODIMP GetGDISurface(LPDIRECTDRAWSURFACE7* a) { return m_real->GetGDISurface(a); }
    STDMETHODIMP GetMonitorFrequency(LPDWORD a) { return m_real->GetMonitorFrequency(a); }
    STDMETHODIMP GetScanLine(LPDWORD a) { return m_real->GetScanLine(a); }
    STDMETHODIMP GetVerticalBlankStatus(LPBOOL a) { return m_real->GetVerticalBlankStatus(a); }
    STDMETHODIMP Initialize(GUID* a) { return m_real->Initialize(a); }

    STDMETHODIMP RestoreDisplayMode()
    {
        HRESULT hr = m_real->RestoreDisplayMode();
        DXLOG("[dx] DD RestoreDisplayMode -> 0x%08lX", hr);
        return hr;
    }

    STDMETHODIMP SetCooperativeLevel(HWND hwnd, DWORD flags)
    {
        HRESULT hr = m_real->SetCooperativeLevel(hwnd, flags);
        char names[192];
        // The single most informative line this file produces: it settles
        // whether the Viewer is an exclusive-fullscreen app or a windowed one,
        // which decides what a windowing/scaling fix has to look like.
        logf("[dx] DD SetCooperativeLevel(hwnd=%p, %s) -> 0x%08lX",
             hwnd, ddscl_names(flags, names, sizeof(names)), hr);
        return hr;
    }

    STDMETHODIMP SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
    {
        HRESULT hr = m_real->SetDisplayMode(w, h, bpp, refresh, flags);
        logf("[dx] DD SetDisplayMode(%lux%lu %lubpp %luHz flags=%08lX) -> 0x%08lX",
             w, h, bpp, refresh, flags, hr);
        return hr;
    }

    STDMETHODIMP WaitForVerticalBlank(DWORD a, HANDLE b)
        { return m_real->WaitForVerticalBlank(a, b); }
    STDMETHODIMP GetAvailableVidMem(LPDDSCAPS2 a, LPDWORD b, LPDWORD c)
        { return m_real->GetAvailableVidMem(a, b, c); }
    STDMETHODIMP GetSurfaceFromDC(HDC a, LPDIRECTDRAWSURFACE7* b)
        { return m_real->GetSurfaceFromDC(a, b); }
    STDMETHODIMP RestoreAllSurfaces() { return m_real->RestoreAllSurfaces(); }
    STDMETHODIMP TestCooperativeLevel() { return m_real->TestCooperativeLevel(); }
    STDMETHODIMP GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 a, DWORD b)
        { return m_real->GetDeviceIdentifier(a, b); }
    STDMETHODIMP StartModeTest(LPSIZE a, DWORD b, DWORD c)
        { return m_real->StartModeTest(a, b, c); }
    STDMETHODIMP EvaluateMode(DWORD a, DWORD* b) { return m_real->EvaluateMode(a, b); }
};

static HRESULT WINAPI hook_DirectDrawCreateEx(GUID* guid, LPVOID* ppDD,
                                              REFIID iid, IUnknown* pUnk)
{
    InterlockedIncrement(&g_n_ddcreate);
    HRESULT hr = real_DirectDrawCreateEx(guid, ppDD, iid, pUnk);
    if (FAILED(hr) || !ppDD || !*ppDD) {
        DXLOG("[dx] DirectDrawCreateEx -> 0x%08lX (not wrapped)", hr);
        return hr;
    }
    // DirectDrawCreateEx is documented to accept IID_IDirectDraw7 only, but the
    // wrapper's layout is IDirectDraw7's, so anything else must go unwrapped
    // rather than be reinterpreted.
    if (iid != IID_IDirectDraw7) {
        char n[64];
        logf("[dx] DirectDrawCreateEx asked for %s, not IDirectDraw7 -- left unwrapped",
             iid_name(iid, n, sizeof(n)));
        return hr;
    }
    DDWrap* w = new (std::nothrow) DDWrap((IDirectDraw7*)*ppDD);
    if (!w) return hr;
    *ppDD = static_cast<IDirectDraw7*>(w);
    logf("[dx] DirectDrawCreateEx wrapped (trace only -- no behaviour change)");
    return hr;
}

// ---------------------------------------------------------------------------
// wiring
// ---------------------------------------------------------------------------

void* dx_hook_DirectSoundCreate8() { return (void*)hook_DirectSoundCreate8; }
void* dx_hook_DirectDrawCreateEx() { return (void*)hook_DirectDrawCreateEx; }

// --- display-mode trace ------------------------------------------------------
//
// Added 2026-08-12, after Tetra Master first ran and the user reported that the
// monitor changes resolution to play it fullscreen, with the mouse cursor
// misplaced and over-sensitive (one symptom, two faces: the game draws in a
// 640x480 space while the pointer arrives in another).
//
// The live trace RULED OUT the obvious suspect. In the TM session the Viewer
// calls DD SetCooperativeLevel(NORMAL) -- WINDOWED, not exclusive -- creates a
// 640x480 primary, and **never calls SetDisplayMode at all**. So DirectDraw is
// not what switches the monitor, and any fix aimed at the DirectDraw mode path
// would have been aimed at nothing.
//
// That leaves the USER32 display API, which nothing here was watching. These
// hooks are trace-only on purpose: the fix (refuse the switch and scale, versus
// let it switch and remap pointer coordinates) depends on WHICH call fires and
// with what mode, and picking before we know that is how the DirectDraw guess
// above would have gone wrong. Cheap -- a mode switch happens once, not per
// frame.
typedef LONG (WINAPI *PFN_CDSA)(DEVMODEA*, DWORD);
typedef LONG (WINAPI *PFN_CDSW)(DEVMODEW*, DWORD);
typedef LONG (WINAPI *PFN_CDSEA)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *PFN_CDSEW)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);

static PFN_CDSA  real_ChangeDisplaySettingsA   = NULL;
static PFN_CDSW  real_ChangeDisplaySettingsW   = NULL;
static PFN_CDSEA real_ChangeDisplaySettingsExA = NULL;
static PFN_CDSEW real_ChangeDisplaySettingsExW = NULL;

static LONG g_n_modeswitch = 0;

static void log_devmode(const char* api, const DEVMODEA* dm, DWORD flags, LONG r)
{
    InterlockedIncrement(&g_n_modeswitch);
    if (!dm) {
        logf("[dx] %s(NULL) flags=%08lX -> %ld   (NULL = restore the "
             "registry-default mode)", api, flags, r);
    } else {
        logf("[dx] %s %lux%lu %lubpp %luHz fields=%08lX flags=%08lX -> %ld",
             api, dm->dmPelsWidth, dm->dmPelsHeight, dm->dmBitsPerPel,
             dm->dmDisplayFrequency, dm->dmFields, flags, r);
    }
    log_flush();
}

static LONG WINAPI hook_ChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags)
{
    LONG r = real_ChangeDisplaySettingsA(dm, flags);
    log_devmode("ChangeDisplaySettingsA", dm, flags, r);
    return r;
}

static LONG WINAPI hook_ChangeDisplaySettingsW(DEVMODEW* dm, DWORD flags)
{
    LONG r = real_ChangeDisplaySettingsW(dm, flags);
    // DEVMODEW and DEVMODEA differ only in their name fields; every member the
    // logger touches is at the same offset and type in both.
    log_devmode("ChangeDisplaySettingsW", (const DEVMODEA*)dm, flags, r);
    return r;
}

static LONG WINAPI hook_ChangeDisplaySettingsExA(LPCSTR dev, DEVMODEA* dm,
                                                 HWND hwnd, DWORD flags, LPVOID p)
{
    LONG r = real_ChangeDisplaySettingsExA(dev, dm, hwnd, flags, p);
    log_devmode("ChangeDisplaySettingsExA", dm, flags, r);
    return r;
}

static LONG WINAPI hook_ChangeDisplaySettingsExW(LPCWSTR dev, DEVMODEW* dm,
                                                 HWND hwnd, DWORD flags, LPVOID p)
{
    LONG r = real_ChangeDisplaySettingsExW(dev, dm, hwnd, flags, p);
    log_devmode("ChangeDisplaySettingsExW", (const DEVMODEA*)dm, flags, r);
    return r;
}

void* dx_real_ChangeDisplaySettingsA()   { return (void*)real_ChangeDisplaySettingsA; }
void* dx_hook_ChangeDisplaySettingsA()   { return (void*)hook_ChangeDisplaySettingsA; }
void* dx_real_ChangeDisplaySettingsW()   { return (void*)real_ChangeDisplaySettingsW; }
void* dx_hook_ChangeDisplaySettingsW()   { return (void*)hook_ChangeDisplaySettingsW; }
void* dx_real_ChangeDisplaySettingsExA() { return (void*)real_ChangeDisplaySettingsExA; }
void* dx_hook_ChangeDisplaySettingsExA() { return (void*)hook_ChangeDisplaySettingsExA; }
void* dx_real_ChangeDisplaySettingsExW() { return (void*)real_ChangeDisplaySettingsExW; }
void* dx_hook_ChangeDisplaySettingsExW() { return (void*)hook_ChangeDisplaySettingsExW; }

// ---------------------------------------------------------------------------
// shell window scaler
// ---------------------------------------------------------------------------
// The shell stretches its fixed 640x480 back buffer to the window CLIENT rect on every
// present (measured: Blt->PRIMARY dst = client rect, recomputed per frame). So enlarging
// the window scales the render -- no per-frame edit needed. Integer scale keeps a clean
// 2x. Scoped to WHILE THE SHELL IS PRESENTING (a recent g_primary Blt): once a d3d8 game
// takes the window the ddraw present stops and the d3d layer owns sizing, so we stand
// down on its own. The app maps the mouse against the same client rect (it already runs
// at a non-native 588x441), so input scales with it.
static const int SHELL_W = 640, SHELL_H = 480;

static int shell_target_scale()
{
    if (g_shell_scale > 0) return g_shell_scale;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int s = sw / SHELL_W, t = sh / SHELL_H;
    if (t < s) s = t;                          // largest integer multiple that fits
    return s < 1 ? 1 : s;
}

// The framed shell window (WS_CAPTION) -- never the borderless mask window.
static BOOL CALLBACK shell_pick(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (!(GetWindowLongW(h, GWL_STYLE) & WS_CAPTION)) return TRUE;
    wchar_t cls[64] = L""; GetClassNameW(h, cls, 64);
    if (!wcsstr(cls, L"PlayOnline")) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

// Say WHICH window we are talking to, and what shape it is in.
//
// "The window will not take the size" is only half a diagnosis -- the other half
// is which window, and whether it is even a resizable one. A window that is
// MAXIMIZED, or that lacks WS_THICKFRAME, or that a wndproc clamps via
// WM_GETMINMAXINFO, all present identically as "SetWindowPos did nothing", and
// they have completely different fixes. Log enough to tell them apart in one
// run rather than guessing across many.
static void shell_describe(const char* tag, HWND h)
{
    if (!h || !IsWindow(h)) { logf("[shellscale] %s: <no window>", tag); return; }
    wchar_t cls[64] = L"", txt[128] = L"";
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, txt, 128);
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    RECT w, c; ZeroMemory(&w, sizeof(w)); ZeroMemory(&c, sizeof(c));
    GetWindowRect(h, &w); GetClientRect(h, &c);
    WINDOWPLACEMENT wp; ZeroMemory(&wp, sizeof(wp)); wp.length = sizeof(wp);
    GetWindowPlacement(h, &wp);
    // The two style bits that decide whether a resize is even legal, plus the
    // placement state -- a MAXIMIZED window silently ignores a SetWindowPos size.
    char f[96] = "";
    if (st & WS_THICKFRAME) strncat_s(f, sizeof(f), "SIZEBOX ",  _TRUNCATE);
    else                    strncat_s(f, sizeof(f), "no-SIZEBOX ", _TRUNCATE);
    if (st & WS_MAXIMIZE)   strncat_s(f, sizeof(f), "MAXIMIZED ", _TRUNCATE);
    if (st & WS_POPUP)      strncat_s(f, sizeof(f), "POPUP ",     _TRUNCATE);
    logf("[shellscale] %s hwnd=%p class=%S title=\"%S\" style=%08lX ex=%08lX [%s] "
         "showCmd=%u win=(%ld,%ld %ldx%ld) client=%ldx%ld",
         tag, h, cls, txt, st, ex, f, wp.showCmd,
         w.left, w.top, w.right - w.left, w.bottom - w.top, c.right, c.bottom);
}

// Ask the window itself what sizes it will accept.
//
// WM_GETMINMAXINFO is the documented clamp DefWindowProc applies to a sizing
// SetWindowPos on an overlapped/thickframe window. If a wndproc answers with a
// ptMaxTrackSize smaller than what we want, our resize can never land, and no
// amount of retrying changes that. Seed the struct the way USER32 does so a
// wndproc that only ADJUSTS the defaults gives a meaningful answer.
static void shell_probe_tracksize(HWND h)
{
    if (!h || !IsWindow(h)) return;
    MINMAXINFO mmi; ZeroMemory(&mmi, sizeof(mmi));
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    mmi.ptMaxSize.x = sw;  mmi.ptMaxSize.y = sh;
    mmi.ptMaxTrackSize.x = sw + 2 * GetSystemMetrics(SM_CXFRAME);
    mmi.ptMaxTrackSize.y = sh + 2 * GetSystemMetrics(SM_CYFRAME);
    mmi.ptMinTrackSize.x = GetSystemMetrics(SM_CXMINTRACK);
    mmi.ptMinTrackSize.y = GetSystemMetrics(SM_CYMINTRACK);
    LONG before_x = mmi.ptMaxTrackSize.x, before_y = mmi.ptMaxTrackSize.y;
    SendMessageW(h, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
    logf("[shellscale] WM_GETMINMAXINFO: maxTrack %ldx%ld (seeded %ldx%ld)%s "
         "minTrack %ldx%ld maxSize %ldx%ld",
         mmi.ptMaxTrackSize.x, mmi.ptMaxTrackSize.y, before_x, before_y,
         (mmi.ptMaxTrackSize.x != before_x || mmi.ptMaxTrackSize.y != before_y)
             ? "  <-- THE WNDPROC CLAMPS IT" : "  (wndproc left it alone)",
         mmi.ptMinTrackSize.x, mmi.ptMinTrackSize.y,
         mmi.ptMaxSize.x, mmi.ptMaxSize.y);
}

// The poll must NEVER fight the user for the window.
//
// The original loop re-applied position+size on every tick whose client-size
// check failed, and the check can fail forever: if the window refuses to take
// the size we ask for (a WM_GETMINMAXINFO clamp, a fixed-frame shell window, a
// DPI-virtualised rect that never reads back as what we set), the "already
// sized" guard never trips. Measured on a real session: 6,267 re-centres in one
// run at 700 ms apart -- i.e. every time you drag the window somewhere, it
// snaps back to the middle of the screen within a second. That is the bug this
// counter and the NOMOVE below exist to stop.
//
// So: try a bounded number of times per window, and only ever CENTRE on the
// first attempt. After that the position belongs to whoever moved it last,
// which is the user.
static HWND g_shell_scaled_hwnd = NULL;   // window the counter below belongs to
static int  g_shell_scale_tries = 0;
static const int SHELL_SCALE_MAX_TRIES = 3;

static DWORD WINAPI shell_scale_poll(LPVOID)
{
    for (;;) {
        Sleep(700);
        if ((LONG)GetTickCount() - g_shell_present_tick > 2000) continue;  // shell not presenting
        HWND h = NULL; EnumWindows(shell_pick, (LPARAM)&h);
        if (!h) continue;
        if (h != g_shell_scaled_hwnd) {          // a different (or the first) shell window
            g_shell_scaled_hwnd = h;
            g_shell_scale_tries = 0;
            shell_describe("target", h);         // WHICH window, and is it resizable at all
            shell_probe_tracksize(h);            // will it even accept the size we want
        }
        int scale = shell_target_scale();
        int cw = SHELL_W * scale, ch = SHELL_H * scale;
        RECT cr; GetClientRect(h, &cr);
        if ((cr.right - cr.left) == cw && (cr.bottom - cr.top) == ch) continue;  // already sized
        if (g_shell_scale_tries >= SHELL_SCALE_MAX_TRIES) continue;   // it will not take -- stop poking
        RECT want = { 0, 0, cw, ch };
        DWORD style = (DWORD)GetWindowLongW(h, GWL_STYLE);
        DWORD ex    = (DWORD)GetWindowLongW(h, GWL_EXSTYLE);
        AdjustWindowRectEx(&want, style, GetMenu(h) != NULL, ex);
        int ow = want.right - want.left, oh = want.bottom - want.top;
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int x = (sw - ow) / 2; if (x < 0) x = 0;
        int y = (sh - oh) / 2; if (y < 0) y = 0;
        // Centre on the FIRST attempt only; every later one is a resize in place.
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
        int first = (g_shell_scale_tries == 0);
        if (!first) flags |= SWP_NOMOVE;
        g_shell_scale_tries++;
        SetLastError(0);
        BOOL ok = SetWindowPos(h, NULL, x, y, ow, oh, flags);
        DWORD gle = GetLastError();
        // Read the result back IMMEDIATELY: "SetWindowPos returned TRUE" and "the
        // window actually changed size" are different claims, and only the second
        // one matters. A clamped resize succeeds and does nothing.
        RECT after; ZeroMemory(&after, sizeof(after));
        GetClientRect(h, &after);
        logf("[shellscale] shell window -> client %dx%d (%dx)%s [try %d/%d] "
             "SetWindowPos=%d err=%lu, client now %ldx%ld%s",
             cw, ch, scale, first ? ", centred" : ", position left alone",
             g_shell_scale_tries, SHELL_SCALE_MAX_TRIES, ok ? 1 : 0, gle,
             after.right - after.left, after.bottom - after.top,
             (after.right - after.left == cw && after.bottom - after.top == ch)
                 ? "  (took)" : "  <-- REFUSED");
        if (g_shell_scale_tries >= SHELL_SCALE_MAX_TRIES) {
            GetClientRect(h, &cr);
            logf("[shellscale] gave up after %d tries: client is %ldx%ld, wanted %dx%d. "
                 "The window will not take the size; leaving it (and its position) alone.",
                 SHELL_SCALE_MAX_TRIES, cr.right - cr.left, cr.bottom - cr.top, cw, ch);
            shell_describe("gave-up", h);
        }
        log_flush();
    }
    return 0;
}

void dx_resolve()
{
    if (!g_dx_enable) return;

    if (g_shell_scale_en) {
        CloseHandle(CreateThread(NULL, 0, shell_scale_poll, NULL, 0, NULL));
        logf("[dx] shell scaler armed (scale=%s)",
             g_shell_scale > 0 ? "fixed" : "auto");
    }

    // USER32 is always loaded; resolve by name so patch_iat can match by value.
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        real_ChangeDisplaySettingsA   = (PFN_CDSA) GetProcAddress(u32, "ChangeDisplaySettingsA");
        real_ChangeDisplaySettingsW   = (PFN_CDSW) GetProcAddress(u32, "ChangeDisplaySettingsW");
        real_ChangeDisplaySettingsExA = (PFN_CDSEA)GetProcAddress(u32, "ChangeDisplaySettingsExA");
        real_ChangeDisplaySettingsExW = (PFN_CDSEW)GetProcAddress(u32, "ChangeDisplaySettingsExW");
    }

    // Load both explicitly: at inject time the client has not necessarily pulled
    // them in yet, and patch_iat matches by RESOLVED ADDRESS -- so we must own a
    // reference to the same module instance the loader will bind the client to.
    // This is exactly the ws2_32 pattern in inject.cpp, and it is what makes
    // app.dll's by-ordinal DSOUND import (#11 = DirectSoundCreate8) a non-issue.
    HMODULE ds = LoadLibraryW(L"dsound.dll");
    if (ds) real_DirectSoundCreate8 = (PFN_DSC8)GetProcAddress(ds, "DirectSoundCreate8");
    HMODULE dd = LoadLibraryW(L"ddraw.dll");
    if (dd) real_DirectDrawCreateEx = (PFN_DDCEX)GetProcAddress(dd, "DirectDrawCreateEx");

    logf("[dx] enable=1 trace=%d globalfocus=%d coop=%d | DirectSoundCreate8=%p "
         "DirectDrawCreateEx=%p", g_dx_trace, g_dx_globalfocus, g_dx_coop,
         real_DirectSoundCreate8, real_DirectDrawCreateEx);
    if (!real_DirectSoundCreate8)
        logf("[dx] WARN: DirectSoundCreate8 unresolved -- audio hooks are dead");
    if (!real_DirectDrawCreateEx)
        logf("[dx] WARN: DirectDrawCreateEx unresolved -- video hooks are dead");
}

void dx_summary()
{
    if (!g_dx_enable) return;
    logf("[dx] summary: DirectSoundCreate8=%ld DirectDrawCreateEx=%ld "
         "sound buffers=%ld (+GLOBALFOCUS on %ld) coop downgrades=%ld retries=%ld",
         g_n_dscreate, g_n_ddcreate, g_n_buffers, g_n_focus_set,
         g_n_coop_down, g_n_retry);
    // A zero here is the diagnosis, not a missing feature: it means the client
    // never reached the entry point we hooked, so the IAT patch is the suspect.
    if (g_dx_enable && !g_n_dscreate)
        logf("[dx] NOTE: DirectSoundCreate8 was never called -- either audio was "
             "never started this session, or the IAT swap did not land");
    logf("[dx] display-mode switches seen: %ld", g_n_modeswitch);
    if (!g_n_modeswitch)
        logf("[dx] NOTE: no ChangeDisplaySettings* call. If the monitor still "
             "changed mode this session, the switch came from somewhere else "
             "again (a d3d device, or the game's own process) -- widen the "
             "search rather than assuming user32.");
}
