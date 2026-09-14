// d3d8hook.cpp -- Direct3D 8 interposition. This is where the games live.
//
// WHY THIS FILE EXISTS. dxhook.cpp measured the Viewer shell and found it to be
// a WINDOWED DirectDraw app at 640x480 that never calls SetDisplayMode, and the
// ChangeDisplaySettings* trace came back empty across ~10 live sessions. Both
// negatives were real; we were simply watching the wrong API. Reading the import
// tables of the CONTENT modules settles it:
//
//     app.dll                  DDRAW!DirectDrawCreateEx      (the Viewer shell)
//     TM.dll                   d3d8!Direct3DCreate8          (Tetra Master)
//     FFXiMain.dll             d3d8!Direct3DCreate8
//     FE_Client.dll            d3d8!Direct3DCreate8          (Fantasy Earth)
//     FrontMissionOnline.dll   d3d9!Direct3DCreate9
//
// The shell is DirectDraw; every actual GAME is Direct3D. D3D8 changes the
// display mode inside CreateDevice when Windowed == FALSE, and it does that
// through the runtime and driver -- never through ChangeDisplaySettings and
// never through DirectDraw. That is exactly the shape of the evidence we had.
//
// It also predicts the second half of the report: a 640x480 fullscreen device on
// a larger desktop puts the rendered image and the OS pointer in two different
// coordinate spaces, which is the "mouse misplaced and over-sensitive" symptom.
// One cause, two faces.
//
// THE ABI PROBLEM, AND HOW THIS FILE HANDLES IT. d3d8.h ships in no current SDK
// and not even in the June 2010 DirectX SDK, so the interfaces below are
// declared by hand and a wrong vtable slot index would be a silent crash -- the
// same failure mode that cost a session on PolContentsCom's stolen slot 3.
// So nothing is patched until the layout is PROVEN at runtime:
// validate_d3d8() calls slot 4 (GetAdapterCount) and slot 8
// (GetAdapterDisplayMode) and requires the adapter count to be sane AND the
// reported display mode to equal the real desktop mode from
// EnumDisplaySettingsW. If the vtable were shifted, slot 8 would not hand back
// the true desktop resolution. The device vtable is checked the same way against
// its own slot 8. A failed check logs and disables the hook; it never guesses.
//
// Delivery is a VTABLE patch, not a wrapper object: we replace exactly one
// function pointer (CreateDevice, and later Reset) in d3d8's own vtable and keep
// the original. The client goes on using the genuine interface pointer. That
// means one correct slot index instead of a 100-method wrapper, and it covers
// TM, FFXI and Fantasy Earth at once because they all share d3d8's vtable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>          // _ReturnAddress -- names the title that called us
#include <tlhelp32.h>       // module enumeration -- the app module's name varies
#include <shlwapi.h>        // StrStrIA -- match modules by INSTALL PATH, not name
#include <shellapi.h>       // ExtractIconA -- the app icon for the game window
#include "polshim.h"
#include "profiles.h"

// ---------------------------------------------------------------------------
// hand-declared D3D8 ABI
// ---------------------------------------------------------------------------

// D3DPRESENT_PARAMETERS as of D3D8 -- NOT the D3D9 layout, which inserts
// MultiSampleQuality and renames the trailing interval field.
struct D3D8_PRESENT_PARAMETERS {
    UINT  BackBufferWidth;
    UINT  BackBufferHeight;
    DWORD BackBufferFormat;              // D3DFORMAT
    UINT  BackBufferCount;
    DWORD MultiSampleType;               // D3DMULTISAMPLE_TYPE
    DWORD SwapEffect;                    // D3DSWAPEFFECT
    HWND  hDeviceWindow;
    BOOL  Windowed;
    BOOL  EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;        // D3DFORMAT
    DWORD Flags;
    UINT  FullScreen_RefreshRateInHz;    // must be 0 in windowed mode
    UINT  FullScreen_PresentationInterval;// must be DEFAULT (0) in windowed mode
};

struct D3D8_DISPLAYMODE { UINT Width; UINT Height; UINT RefreshRate; DWORD Format; };

#define D3D8FMT_UNKNOWN              0
#define D3D8PRESENT_INTERVAL_DEFAULT 0
#define D3D8SWAPEFFECT_DISCARD       1
#define D3D8SWAPEFFECT_FLIP          2

// IDirect3D8 vtable slots (IUnknown 0-2, then the interface in declaration order)
#define S_GetAdapterCount        4
#define S_GetAdapterDisplayMode  8
#define S_CreateDevice          15
// IDirect3DDevice8 vtable slots
#define SD_TestCoopLevel         3
#define SD_GetDisplayMode        8
#define SD_Reset                14
// Render-path slots on IDirect3DDevice8 (standard layout, corroborated by the
// validated Reset=14 / GetDisplayMode=8 above). Used only by the read-only
// render diagnostic (d3d_renderspy) to learn HOW the title draws.
#define SD_Present              15
#define SD_BeginScene           34
#define SD_SetTransform         37
#define SD_SetViewport          40
#define SD_DrawPrimitive        70
#define SD_DrawIndexedPrimitive 71
#define SD_DrawPrimitiveUP      72
#define SD_DrawIndexedPrimitiveUP 73
#define SD_SetVertexShader      76

typedef UINT    (STDMETHODCALLTYPE *PFN_GetAdapterCount)(void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetAdapterDisplayMode)(void*, UINT, D3D8_DISPLAYMODE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(void*, UINT, DWORD, HWND, DWORD,
                                                     D3D8_PRESENT_PARAMETERS*, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevGetDisplayMode)(void*, D3D8_DISPLAYMODE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevReset)(void*, D3D8_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevTestCoop)(void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevBeginScene)(void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DevPresent)(void*, const void*, const void*, HWND, const void*);

// D3DVIEWPORT8 and the draw/transform methods (D3DMATRIX is 16 floats row-major).
struct D3D8_VIEWPORT { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTransform)(void*, DWORD, const float*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(void*, const D3D8_VIEWPORT*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitive)(void*, DWORD, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitive)(void*, DWORD, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitiveUP)(void*, DWORD, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitiveUP)(void*, DWORD, UINT, UINT, UINT,
                                                               const void*, DWORD, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVertexShader)(void*, DWORD);
typedef void*   (WINAPI *PFN_Direct3DCreate8)(UINT);

// ---------------------------------------------------------------------------
// configuration + state
// ---------------------------------------------------------------------------

static int g_d3d_enable    = 0;
static int g_d3d_trace     = 1;
// WINDOWED IS THE DEFAULT (2026-08-24). It shipped as 0 for most of this project's
// life, on the reasoning that a behaviour change should be opt-in. The report that
// overturned that: an exclusive fullscreen device "fights for screen dominance" so
// hard the game cannot be left. That is not a cosmetic preference -- an exclusive
// device owns the display, so alt-tab, the settings chord (polsettings.cpp
// exclusive_display_blocks_us makes it a NO-OP while one is live), any overlay, and
// recovery from sleep (wakerecover.cpp) are all things it takes away. Opt-out, not
// opt-in. Set [dx] d3d_windowed=0 to go back; d3d_windowed_except puts back ONE title.
static int g_d3d_windowed  = 1;

// TM.dll SE-queue unstick (Steam Deck / Proton workaround). TM's gW201.dat
// chunked loader stalls under Proton, leaving an SE-request entry {id,sub} in a
// std::vector that never drains, so TM never builds/draws its home screen (the
// device is healthy and Present runs -- it just issues 0 draws). Clearing that
// vector's contents lets TM proceed (proven live: draws + sound). Off by default;
// enable only for TM on the Deck. The vector RVA + the stuck signature are ini-
// tunable so a different TM.dll build can be retargeted without a rebuild.
static int      g_tm_se_unstick  = 0;
static unsigned g_tm_unstick_rva = 0x2FF294;  // RVA of the vector's begin pointer
static unsigned g_tm_unstick_id  = 201;       // stuck-entry id  (entry+0x04) = gW201.dat
static unsigned g_tm_unstick_sub = 0;         // stuck-entry sub (entry+0x08); 0 = ANY sub
                                              // (the loader stalls on EVERY gW201 resource:
                                              // sub 1000, 1005, ... each sticks in turn)
// [dx] tm_diag -- read-only observable for the gW path-dot hypothesis.
// A LOADDIR change has no log line of its own,
// which made the 08-19 Deck test unfalsifiable: "the fix is wrong" and "the fix
// never ran" were indistinguishable. This logs, once each, (a) the basedir
// string TM actually read from the registry into TM.dll+0x2FF308, and (b) the
// resource-index map's size and min/max key (+0x2FC438). The max key IS the
// verdict: gW keys are fileNumber*10000+sub, so max <= 9999 means fileNumber
// parsed as 0 = every key collapsed = the dotted-path bug is LIVE; max >= 10000
// means the path parsed correctly. Compiled ON (read-only, one-shot, SEH-
// guarded, TM-only) so it reaches the Deck without an ini delivery.
static int      g_tm_diag        = 1;
static unsigned g_tm_basedir_rva = 0x2FF308;  // LOADDIR readback buffer (cap 100)
static unsigned g_tm_map_rva     = 0x2FC438;  // resource-index std::map (VC6 _Tree)

// [dx] tm_resv_diag -- read-only observable for the reservation slot +0x108, the
// value that drives the "Table Members" sidebar AND the Start-Game gate AND the
// owner's own recruiting tile (2026-08-21). The
// external tm_store_probe can only sample slowly and kept catching an empty store
// between the client's ~15s @Pong refreshes; this reads the same slots on EVERY
// Present frame from inside the process and logs the instant any of {+0x108,
// +0x41c arm, a (0x41,0xC) @GameML in the store} changes -- so it captures the
// exact frame the reservation reply lands vs. when the arm is set, without a
// debugger (no debug register, so ASProtect's anti-debug is not tripped -- unlike
// a hardware breakpoint, which freezes this binary). THE FORK it settles: if a
// 0x41/0xC is present AND the arm is set AND +0x108 is still 0, the reservation
// parser is rejecting a delivered reply (escalate to an inline 0xA90A0 hook);
// if +0x108 fills on some frame, the reply just is not resident often enough
// (fix the server send cadence). RVAs are TM.dll dump-base (0x04F90000) RVAs, the
// same the tm_store_probe uses; screenobj is a POINTER at +0x2B6254.
static int      g_tm_resv_diag       = 1;
static unsigned g_tm_resv_obj_rva    = 0x2B6254;  // [0x5246254] lobby screen obj ptr
static unsigned g_tm_resv_store_rva  = 0x298250;  // [0x5228250] message store base
static unsigned g_tm_resv_count_rva  = 0x2B31C4;  // [0x52431C4] store entry count
// store entry stride 0x3D8; ASCII header at entry+0x25 (code=hex[0:2], msgid=hex[4:6])
// RETIRED AS A KNOB, 2026-08-17 -- kept as a variable only so the d3d9 side can
// pass its own resolution through fit_window's `fit` argument.
//
// It was `[dx] d3d_fitwindow`, and there was no configuration in which 0 was
// right. fit_window does TWO things: it sizes the client area to the backbuffer
// (which is also the mouse fix -- the pointer maps against the client rect), and
// it restyles the title's exclusive-fullscreen WS_POPUP into a real framed
// window. Turning it off skips both, so a forced-windowed game becomes an
// unmovable borderless box at whatever size the title happened to pick.
//
// MEASURED both ways, 2026-08-17, on the two titles to hand:
//   FMO  -> an 800x600 borderless box pinned at (0,0) of a 1600x900 screen,
//           no frame, cannot be dragged  (screenshotted by the user)
//   FE   -> the screen-sized 1600x852 popup it built for exclusive fullscreen,
//           i.e. a 1.41x horizontal stretch -- WORSE than the 1.22x fault that
//           d3d_fitwindow=0 was reached for as a diagnostic in the first place
//
// It was offered as a peer of d3d_windowed, so it read like a preference and got
// switched off to test a hypothesis about the stretch. It is not a preference:
// it is the second half of d3d_windowed. An ini that still names it is IGNORED,
// loudly, once -- see d3d8_configure.
static int g_d3d_fitwindow = 1;
static int g_d3d_unmask    = 2;   // 0 off, 1 hide the mask, 2 align it to the game
static int g_d3d_cursor    = 2;   // 0 off, 1 log only, 2 log + translate
static int g_d3d_nosteal   = 1;   // refuse focus grabs from another application
// Refuse the game's attempt to TAKE THE MOUSE CAPTURE BACK. The exact sibling of
// d3d_nosteal, one layer down: nosteal stops a forced-windowed title dragging its
// own window back to the top, this stops it re-grabbing the mouse. See the
// WM_CAPTURECHANGED handling in spy_proc for the RE that produced it.
static int g_d3d_nograb     = 1;
static int g_d3d_msgspy    = 1;   // watch the mouse messages the windows receive
static int g_d3d_fakemode  = 1;   // report the backbuffer as the screen size
static int g_hang_dump_ms   = 0;  // [dx] hang_dump_ms: if the title has not presented
                                  // this many ms after its device is made, dump every
                                  // thread's stack once (for FE's wedged-black state)
static char g_windowed_except[256] = "";  // titles the windowed override skips
static char g_windowed_force[256] = "";   // titles whose PROFILE verdict is cleared

// [dx] d3d_bbsize=WxH -- force the title's backbuffer to this size.
//
// THE POINTER FIX, from the shell side. pol.exe maps the cursor into a FIXED
// 640x480 virtual screen: FUN_00409FF0 scales with `lea esi,[esi+esi*4]; shl 7`
// (x640) and `imul edx,edx,0x1E0` (x480), magic-number divides by the other, and
// an aspect test against the double at 0x00442540 = 1.3333333730697632 (4:3).
// Those numbers are baked into the instruction ENCODINGS, so retargeting pol.exe
// at the real size would mean re-encoding several instructions and their magic
// divisors -- invasive and fragile. The other direction is one field.
//
// Tetra Master and FFXI ask for 640x480, so the mapping is 1:1 and their input
// lands. Fantasy Earth asks for 800x600 -- same 4:3, but 1.25x the scale -- so
// every coordinate it receives is 0.8x of where the user clicked. That is
// exactly the reported symptom: the cursor moves correctly (Windows draws it)
// but clicks miss the buttons, while the keyboard is unaffected because it never
// goes through this path.
//
// 0 = off, leave every title with the size it asked for.
static UINT g_bb_force_w = 0, g_bb_force_h = 0;

// Returns true if it changed anything, so the caller can log it.
static bool force_bbsize(D3D8_PRESENT_PARAMETERS* p)
{
    if (!p || !g_bb_force_w || !g_bb_force_h) return false;
    if (p->BackBufferWidth == g_bb_force_w && p->BackBufferHeight == g_bb_force_h)
        return false;
    p->BackBufferWidth  = g_bb_force_w;
    p->BackBufferHeight = g_bb_force_h;
    return true;
}
static int g_d3d_frame     = 1;   // let the window be dragged, sized and closed
static int g_d3d_inputhwnd = 1;   // repoint app.dll's ScreenToClient window
static int g_d3d_clientrect = 1;  // GetWindowRect(game) -> client rect (THE cursor fix)
static int g_d3d_freecursor = 0;  // SUPPRESS the game's SetCursorPos/ClipCursor (no capture)
static int g_d3d_scale      = 0;  // integer window scale; 0 = auto, 1 = native size
// Remembered window size (d3d_remember_window). A scale factor is a rule about
// how big the window SHOULD be; this is a record of how big the user MADE it,
// and the user wins. See fit_window and remember_window_size.
static int  g_d3d_remember  = 1;   // [dx] d3d_remember_window
static int  g_saved_cw = 0, g_saved_ch = 0;   // remembered CLIENT size, 0 = none
static LONG g_user_sized    = 0;   // the user has resized this session
// The current title windows ITSELF (PW_NATIVE_WINDOWED -- Fantasy Earth), and is being
// forced windowed by the shim only because -windowmode was not delivered. Such a title
// maps the cursor in its OWN backbuffer space and assumes the window IS the backbuffer:
// any other window size desyncs its on-screen cursor from the pointer by the size ratio
// (measured: an 800x600 backbuffer in a 1084x813 window offset the busy cursor by 1.36x).
// So fit_window sizes it to exactly the backbuffer (1x) and ignores the remembered size.
static bool g_title_native_windowed = false;
static wchar_t g_dx_ini[MAX_PATH] = L"";
static int g_d3d_aspect     = 1;  // lock the window to the backbuffer's aspect while sizing
static int g_d3d_borderless = 0;  // 0 off, 1 borderless fullscreen (fill), 2 (integer scale)
static int g_d3d_renderspy  = 0;  // READ-ONLY: log how the title draws (HD-feasibility probe)
static int g_d3d_fitvideo   = 0;  // adopt a misplaced DirectShow video window onto the game
// Window classes that count as that video window. Read here, used by
// is_video_class in the video-window block far below (which explains the two).
static char g_videoclass[128] = "VideoRenderer,FilterGraphWindow";

// Defined with the video-window block far below; d3d_after_device_created calls it.
static void schedule_video_adopt();

// Defined with the message spy further down; used by tame_mask_window above it.
static void install_msgspy(HWND h, const char* tag);
// Drop the previous title's message-spy entries; defined with g_spy further down.
static void spy_reset_for_new_title(void);

// Defined with the input-HWND block below; CreateDevice calls it.
static void inspect_input_hwnd();

// Declared with the display-mode block below; fit_window needs the REAL metrics.
typedef int (WINAPI *PFN_GetSystemMetrics)(int);
static PFN_GetSystemMetrics real_GetSystemMetrics;   // fwd

// The window a windowed override is presenting into -- the cursor translation
// measures its client origin, and the mask/foreground handling raises it.
static HWND g_game_window = NULL;
static int  g_d3d_blackbg = 1;      // [dx] d3d_blackbg -- black class brush on the game window
// Startup-exemption state for d3d_nosteal (see the focus hooks, far below):
static LONG  g_game_ever_fg   = 0;      // the game window has reached the foreground
static DWORD g_game_born_tick = 0;      // when we FIRST recorded a game window
static int   g_nosteal_grace  = 60000;  // ms; [dx] d3d_nosteal_grace, -1 = no limit
static LONG  g_shim_raised    = 0;      // the shim has raised the game window once this title

// Set while the USER has minimised the game window. The game re-foregrounds
// itself every frame (measured: 14+ SetForegroundWindow calls in a row), which
// would pop a minimised window straight back up; while this is set the focus
// hooks hold the game window down until the user restores it.
//
// Declared HERE, well above its old home, because the focus hooks turned out not
// to be the only readers that matter: raise_game_window_once() and fit_window()
// both place or show the game window and both sit above this point, and neither
// asked. A title Resets its device on every loss -- and minimising a windowed
// device IS a loss -- so fit_window ran on the way down and put the window
// straight back up. See the WM_WINDOWPOSCHANGING arm in spy_proc for the other
// half, the title restoring itself with SetWindowPos.
static LONG  g_user_minimized = 0;

// ---------------------------------------------------------------------------
// TITLE GENERATION -- the fact that makes every fix in this file conditional.
// ---------------------------------------------------------------------------
//
// ONE pol.exe process runs MANY titles, one after another. The Viewer stays up
// while you launch Tetra Master, quit back to it, launch Front Mission Online,
// quit, launch it again. Every global in this file is PROCESS-scoped, but the
// thing it describes -- a window, a device, "the user has sized it", "we have
// already raised it" -- dies with the TITLE. So the second title of a session
// inherits the first one's state and runs under a different set of rules, and
// the third under a different set again.
//
// MEASURED, on the account holder's own machine: polshim.962956.log, four
// launches in one Viewer session (TM, FMO, FMO, FMO):
//   launch 1  fit_window RESTORED, taskbar button asserted, msgspy watching,
//             icon resolved to TetraMaster's polboot.exe        -- all correct
//   launch 2  icon falls back to pol.exe          (g_title_module latched)
//   launch 3  "fit_window: SKIPPED -- the user sized this window by hand"
//             (g_user_sized latched back in launch 1): no restyle, no resize
//   launch 4  SKIPPED again, AND no "[msg] watching GAME window" line at all --
//             the 4-slot g_spy table was full of DEAD windows, so install_msgspy
//             returned silently and that window got no spy_proc.
//
// That last one is the user-visible report "I can drag the Tetra Master window,
// and on a later launch suddenly I can't": spy_proc is the only handler of the
// frame messages and the only writer of g_in_modal / g_user_minimized.
//
// g_title_gen counts title sessions. It is what "once per title" means in this
// file -- as opposed to the `static LONG said` one-shots, which mean "once per
// PROCESS" and are precisely why none of this was ever visible in a log: from
// launch 2 onward, the lines that say which rules a title got stop printing.
static volatile LONG g_title_gen = 1;

// The window and module of the title session currently open. Compared, never
// trusted: a destroyed HWND is how we know the previous title has gone.
static HWND    g_session_window = NULL;
static HMODULE g_session_module = NULL;

// Per-title verdicts latched at the boundary, for the files that cannot resolve
// "which title am I in" themselves (vidfit.cpp runs on its own thread).
static int     g_title_adopt_fmv = 0;   // TitleProfile::adopt_fmv
// The active title's module leaf. Cached because the cursor and coordinate hooks
// run on paths that have no caller to resolve, and they must be able to ask
// "was -windowmode delivered to THIS title" rather than "to anyone".
static char    g_title_leaf[80] = "";

// "Say this once for THIS title" -- the per-title replacement for `static LONG
// said`. Pass a function-local static; it stores the generation it last spoke
// in, so an explanation is printed again for the next title instead of staying
// silent for the rest of the process.
static bool say_once_per_title(volatile LONG* slot)
{
    LONG gen = InterlockedCompareExchange(&g_title_gen, 0, 0);
    return InterlockedExchange(slot, gen) != gen;
}

// Declared here, defined at the very end of the file: title_reset_state() clears
// state declared all the way down this file, so it cannot be written until all
// of that state exists.
static void title_reset_state(void);
static void d3d_title_boundary(void* caller_ra, HWND incoming);

// Raise the game window in front of the mask -- but ONLY the first time for a title.
// A forced-windowed title churns its device during startup (FMO creates two and Resets
// them), and d3d_after_device_created runs on each; re-raising every time yanks the
// window back in front after the user has clicked away to something behind it -- the
// "it fights me" report. The first raise is the necessary one (present the game); every
// later one is the shim stealing focus the user did not ask it to. Reset per title in
// d3d8_new_title_launch.
static void raise_game_window_once(HWND game)
{
    if (!game || !IsWindow(game)) return;
    // Not over a window the user has put away. SWP_SHOWWINDOW restores a
    // minimised window, and a device (re)creation is one of the events that can
    // arrive while it is minimised -- so without this the "present the game"
    // raise doubles as an un-minimise the user did not ask for.
    if (IsIconic(game) || InterlockedCompareExchange(&g_user_minimized, 0, 0)) {
        logf("[foc] not raising the game window -- it is minimised; presenting it "
             "now would undo the user's own minimise");
        return;
    }
    if (InterlockedCompareExchange(&g_shim_raised, 1, 0) != 0) {
        logf("[foc] not re-raising the game window on this device (re)creation -- "
             "already presented once; raising again is the 'the window fights me' report");
        return;
    }
    SetWindowPos(game, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(game);
}

// The title module that called CreateDevice (e.g. TM.dll) -- for its own icon.
static HMODULE g_title_module = NULL;

// Forget which title we are in. ONE Viewer session can launch several titles in
// turn, but g_title_module was latched on the first CreateDevice caller and never
// cleared -- so after Front Mission Online had run, launching Fantasy Earth still
// resolved to FMO's directory and the window wore FMO's icon (measured
// 2026-08-14: "set game window icon from ...\FRONT MISSION ONLINE\polboot.exe"
// during an FE launch). gamestart.cpp calls this as each title starts, so the
// next CreateDevice re-resolves against the title that is actually running.
void d3d8_new_title_launch(void)
{
    // Deliberately does NOT reset anything itself any more. It INVALIDATES the
    // open session, so the next CreateDevice declares the boundary and performs
    // the reset -- one code path and one log line, whether or not gamestart is
    // on.
    //
    // This entry point is WHY the per-title reset had never once run on a
    // deployed machine: gamestart.cpp was its only caller, and [polshim]
    // gamestart ships OFF (0 in polshim.ini.production and dist/polshim.ini, and
    // iniheal rev 3 turns it back off after watching FMO's GameStart crashed it).
    // The reset existed, was correct, and was unreachable in production.
    g_session_window = NULL;
    g_session_module = NULL;
}

// A NEW TITLE HAS STARTED -- detected on a call we know always happens.
//
// The signal is the one this file already relies on and has already measured: a
// title asks for a FULLSCREEN device while the Viewer's own device is windowed,
// so a fullscreen CreateDevice from a title module IS "a title is starting".
// That is the same fact the title-module re-resolve in hook_CreateDevice is
// built on, and unlike gamestart.cpp's GameStart hook it fires in every build
// and on every path.
//
// DEBOUNCED, because a title creates more than one device per launch -- FMO
// makes two, the real one and one with focus=NULL (polshim.962956.log lines
// 78696 and 79078). A boundary is declared only when the open session has
// actually ended:
//   * nothing is open yet                     -> the first title
//   * the open session's window is destroyed  -> the previous title quit
//   * a DIFFERENT live window is presented    -> a handoff that overlapped
//   * the calling module changed              -> a different title
// FMO's second, focus=NULL device matches none of these while its real window is
// still alive, so it cannot spuriously reset the shim mid-launch.
static void d3d_title_boundary(void* caller_ra, HWND incoming)
{
    HMODULE m = NULL;
    if (caller_ra)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)caller_ra, &m);

    const char* why = NULL;
    if (!g_session_window && !g_session_module)
        why = "first title of this Viewer session";
    else if (g_session_window && !IsWindow(g_session_window))
        why = "the previous title's window is gone";
    else if (incoming && g_session_window && incoming != g_session_window &&
             IsWindow(incoming))
        why = "a different live window is being presented";
    else if (m && g_session_module && m != g_session_module)
        why = "a different title module is calling";
    if (!why) return;

    LONG gen = InterlockedIncrement(&g_title_gen);

    char nm[MAX_PATH] = "";
    const char* leaf = "(unknown module)";
    if (m && GetModuleFileNameA(m, nm, MAX_PATH)) {
        const char* s = strrchr(nm, '\\');
        leaf = s ? s + 1 : nm;
    }
    const TitleProfile* p = profile_for_module(leaf);
    // ONE line that answers "which rules is this launch running under?" -- the
    // question the per-process `said` latches made unanswerable from the second
    // title onward.
    logf("[title] ======== TITLE #%ld: %s -- %s ======== (%s)", gen, leaf,
         p ? p->title : "no compat profile", why);

    title_reset_state();

    g_session_module = m;
    g_session_window = (incoming && IsWindow(incoming)) ? incoming : NULL;
    // Latch the per-title verdicts that other files have to consult. Resolved HERE,
    // once, against the title that is actually starting -- vidfit.cpp runs on its own
    // thread and must not have to re-derive "which title am I in".
    strncpy_s(g_title_leaf, sizeof(g_title_leaf), leaf, _TRUNCATE);
    title_current_set(leaf);   // for the settings dialog -- see profiles.cpp
    g_title_adopt_fmv = (p && p->adopt_fmv) ? 1 : 0;
    if (g_title_adopt_fmv)
        logf("[title]   profile asks for FMV ADOPTION -- this title's DirectShow video "
             "window is placed onto its game window even with [dx] d3d_fitvideo=0 "
             "(that default is a mitigation for a DIFFERENT title's bug, long fixed).");

    // PER-TITLE SETTINGS. Re-read the settings with THIS title in scope, so any
    // [<sec>.<leaf>] overrides win for the launch that is starting.
    //
    // AFTER title_reset_state() and after the verdicts above, deliberately: the
    // reset restores the globals to their no-title values, and this applies this
    // title's on top. The other order would have the reset wipe the overrides it
    // had just applied -- the same ordering bug, one layer up, that this whole
    // boundary exists to fix.
    //
    // Inert until a key is both marked per_title AND read through ini_int_title,
    // so today this resolves to exactly the global values it already had.
    if (g_dx_ini[0]) shim_reload_for_title(g_dx_ini, leaf);
}

// Does the title currently running want its DirectShow video window adopted?
// vidfit.cpp and schedule_video_adopt consult this; see TitleProfile::adopt_fmv for
// why a per-title answer had to exist at all.
int d3d_title_fmv_adopt(void) { return g_title_adopt_fmv; }

// (g_user_minimized is declared up beside g_shim_raised -- everything that must
// not disturb a minimised window needs it, including raise_game_window_once and
// fit_window, both of which sit above this point.)
// Set while a modal move/size loop is running (WM_ENTERSIZEMOVE..EXITSIZEMOVE).
// The same per-frame SetForegroundWindow spam yanks focus mid-drag and cancels
// the loop -- which is why resizing was flaky and one corner "didn't grab". We
// hold the game's self-refocus off for the duration so the loop can finish.
static LONG g_in_modal = 0;

// The hFocusWindow CreateDevice was given. Reset() has no such argument, so this
// is the only way its windowed path can name a present target.
static HWND g_focus_window = NULL;

// Is the LIVE device exclusive fullscreen? -1 unknown (no device yet), 0 no, 1 yes.
//
// Recorded from the FINAL present parameters -- what the device was actually built
// or reset with, after any windowed override -- because the question this answers
// is about the display, not about what the title asked for. Both renderer paths
// feed it: d3d9hook calls d3d_note_device_mode(), since FMO's device takes the
// screen exactly as a d3d8 one does.
//
// The consumer is polsettings.cpp. Measured on a Steam Deck, 2026-08-15: opening
// the settings dialog inside FFXI CRASHED the title, because creating and
// activating a top-level window over an exclusive fullscreen device takes the
// display from it. Anything about to put a window on screen has to ask first.
static LONG g_dev_fullscreen = -1;

void d3d_note_device_mode(int windowed)
{
    LONG now = windowed ? 0 : 1;
    if (InterlockedExchange(&g_dev_fullscreen, now) == now) return;
    logf("[d3d] live device is now %s -- the settings dialog is %s",
         now ? "EXCLUSIVE FULLSCREEN" : "windowed",
         now ? "held off until this changes" : "safe to open");
}

// The flag alone would be a one-way latch: nothing hooks Release, so after a
// fullscreen title exited and handed the screen back to the Viewer shell it would
// still read "fullscreen" and the settings chord would stay dead for the rest of
// the session. So it is paired with a LIVENESS test on the device's focus window
// -- which CreateDevice always names, on the fullscreen path too, unlike
// g_game_window (only the windowed override sets that). pol.exe destroys the
// title's window when the title ends, and the guard lifts by itself.
int d3d_exclusive_fullscreen()
{
    if (g_dev_fullscreen != 1) return 0;
    if (!g_focus_window || !IsWindow(g_focus_window)) return 0;
    return IsWindowVisible(g_focus_window) ? 1 : 0;
}

static PFN_Direct3DCreate8      real_Direct3DCreate8 = NULL;
static PFN_CreateDevice         orig_CreateDevice    = NULL;
static PFN_DevReset             orig_Reset           = NULL;
static bool                     g_dev_hooked         = false;
static LONG g_n_create = 0, g_n_device = 0, g_n_reset = 0;
static LONG g_n_forced = 0, g_n_retry = 0, g_n_fullscreen_seen = 0;

// --- render diagnostic (d3d_renderspy) state -------------------------------
static PFN_SetTransform          orig_SetTransform    = NULL;
static PFN_SetViewport           orig_SetViewport     = NULL;
static PFN_DrawPrimitive         orig_DrawPrimitive   = NULL;
static PFN_DrawIndexedPrimitive  orig_DrawIndexedPrimitive = NULL;
static PFN_DrawPrimitiveUP       orig_DrawPrimitiveUP = NULL;
static PFN_DrawIndexedPrimitiveUP orig_DrawIndexedPrimitiveUP = NULL;
static PFN_SetVertexShader       orig_SetVertexShader = NULL;
static bool g_rs_hooked = false;
static DWORD g_rs_cur_fvf = 0;                 // last FVF set via SetVertexShader
// draw classification: 2D = the current vertex format is pre-transformed screen
// space (D3DFVF_XYZRHW); 3D = anything with real XYZ that the pipeline transforms.
static LONG g_rs_draw_2d = 0, g_rs_draw_3d = 0, g_rs_draw_other = 0, g_rs_draw_total = 0;
static LONG g_rs_up_calls = 0;                 // *UP draws (vertices inline, easy to rescale)
static LONG g_rs_proj_persp = 0, g_rs_proj_ortho = 0;
static LONG g_rs_vp_logged = 0;
static void install_renderspy(void* dev);   // defined after hook_Reset
static void arm_hang_probe();                // hang_dump_ms: dump stacks if wedged

// Vertex-processing override for CreateDevice's `behavior` flags. Fantasy Earth asks for
// D3DCREATE_HARDWARE_VERTEXPROCESSING (0x40) and Wine's wined3d HARD-ABORTS the process
// inside CreateDevice on the Deck (measured 2026-08-19: forced-windowed 800x600 X8R8G8B8
// behavior=0x40, log stops mid-call, no CRT exit / no VEH). Tetra Master, the same d3d8
// vtable on the same Deck, asks for SOFTWARE VP (0x20) and creates its device fine -- so a
// software/mixed device is a proven-good path here. 0=leave alone, 1=force software,
// 2=force mixed. Clears PUREDEVICE too (0x10 is invalid without hardware VP).
static int g_d3d_vp = 0;

// D3DCREATE_FPU_PRESERVE (0x00000002).
//
// Direct3D sets the x87 control word to single precision and round-to-nearest
// when it creates a device, and LEAVES IT THAT WAY, unless the caller asks it
// not to. A 2003 title that does its own maths in double precision -- physics,
// a timer, a coordinate transform -- then quietly loses the bottom of every
// mantissa for the rest of the process. The classic symptoms are drift and
// non-reproducibility rather than a crash, which is exactly the kind of thing
// that gets misattributed for years.
//
// Ashita exposes this as `[ffxi.direct3d8] behaviorflags.fpu_preserve` and the
// shim had no equivalent at all. OFF by default, deliberately: this changes
// the floating-point mode for the whole process, SE shipped without it, and
// turning it on costs a little speed. It is here so that a suspected precision
// bug can be TESTED in one line instead of theorised about.
static int g_d3d_fpu = 0;

static DWORD apply_vp_override(DWORD behavior)
{
    DWORD b = behavior;
    if (g_d3d_fpu) b |= 0x00000002u;                   // D3DCREATE_FPU_PRESERVE
    if (g_d3d_vp == 0) return b;
    b &= ~0x000000F0u;                                 // clear PURE(0x10)|SW(0x20)|HW(0x40)|MIXED(0x80)
    b |= (g_d3d_vp == 2) ? 0x80u : 0x20u;              // 2=mixed, else software
    return b;
}

// ---------------------------------------------------------------- display mode
//
// WHY THIS EXISTS (2026-09-08). "Run games in a window", "Titles left fullscreen"
// and "Borderless fullscreen" were three separate rows that together decided one
// thing, and a player had to combine them in their head with the nonsensical
// combinations all allowed. Now there is one row, per title, and it resolves into
// the three globals the rest of this file already keys off. The legacy rows are
// still read first (they are the compatibility path for an ini that predates the
// key) and are then OVERRIDDEN when `display` is present. The except/force lists
// in caller_except_why are consulted only when it is absent.
static wchar_t     g_ini_path[MAX_PATH] = L"";
static DisplayMode g_display = DM_UNSET;      // what the last (re)load resolved

DisplayMode display_mode_parse(const wchar_t* v)
{
    if (!v || !v[0]) return DM_UNSET;
    if (!_wcsicmp(v, L"windowed"))         return DM_WINDOWED;
    if (!_wcsicmp(v, L"borderless"))       return DM_BORDERLESS;
    if (!_wcsicmp(v, L"borderless_crisp")) return DM_BORDERLESS_CRISP;
    if (!_wcsicmp(v, L"fullscreen"))       return DM_FULLSCREEN;
    return DM_UNSET;
}

const wchar_t* display_mode_name(DisplayMode m)
{
    switch (m) {
    case DM_WINDOWED:         return L"windowed";
    case DM_BORDERLESS:       return L"borderless";
    case DM_BORDERLESS_CRISP: return L"borderless_crisp";
    case DM_FULLSCREEN:       return L"fullscreen";
    default:                  return L"(unset)";
    }
}

// Explicit leaf, not the ambient title scope: caller_except_why asks about the
// module that is CALLING, which is not always the title a reload was scoped to.
DisplayMode display_mode_for(const char* leaf, const wchar_t* ini)
{
    if (!ini || !ini[0]) ini = g_ini_path;
    if (!ini[0]) return DM_UNSET;
    wchar_t v[64] = L"";
    if (leaf && leaf[0]) {
        wchar_t sec[96], wleaf[80];
        if (MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf, _countof(wleaf)) > 0) {
            _snwprintf_s(sec, _countof(sec), _TRUNCATE, L"dx.%s", wleaf);
            GetPrivateProfileStringW(sec, L"display", L"", v, _countof(v), ini);
            if (v[0]) { wchar_t* c = wcschr(v, L';'); if (c) *c = 0; }
            DisplayMode m = display_mode_parse(v);
            if (m != DM_UNSET) return m;
        }
    }
    v[0] = 0;
    GetPrivateProfileStringW(L"dx", L"display", L"", v, _countof(v), ini);
    if (v[0]) { wchar_t* c = wcschr(v, L';'); if (c) *c = 0; }
    return display_mode_parse(v);
}

// Resolve `display` (under the ambient title scope, so a [dx.<leaf>] section wins)
// into the three legacy globals. Called at the END of configure and reload, after
// the legacy rows have been read, so it overrides them exactly when it is set.
static void display_apply(const wchar_t* ini)
{
    if (ini && ini[0]) wcsncpy_s(g_ini_path, ini, _TRUNCATE);
    wchar_t v[64] = L"";
    ini_str_title(L"dx", L"display", L"", v, _countof(v), ini);
    DisplayMode m = display_mode_parse(v);
    g_display = m;
    switch (m) {
    case DM_FULLSCREEN:       g_d3d_windowed = 0; g_d3d_borderless = 0; break;
    case DM_BORDERLESS:       g_d3d_windowed = 1; g_d3d_borderless = 1; break;
    case DM_BORDERLESS_CRISP: g_d3d_windowed = 1; g_d3d_borderless = 2; break;
    case DM_WINDOWED:         g_d3d_windowed = 1; g_d3d_borderless = 0; break;
    default: break;                          // absent: the legacy rows stand
    }
}

void d3d_display_windowed(int* windowed, const wchar_t* ini)
{
    if (!windowed) return;
    wchar_t v[64] = L"";
    ini_str_title(L"dx", L"display", L"", v, _countof(v), ini);
    DisplayMode m = display_mode_parse(v);
    if (m != DM_UNSET) *windowed = (m != DM_FULLSCREEN) ? 1 : 0;
}

void d3d8_configure(const wchar_t* ini)
{
    g_d3d_vp       = GetPrivateProfileIntW(L"dx", L"d3d_vp", 0, ini);
    g_d3d_fpu      = GetPrivateProfileIntW(L"dx", L"d3d_fpu_preserve", 0, ini);
    g_d3d_enable   = GetPrivateProfileIntW(L"dx", L"d3d_enable",   0, ini);
    g_d3d_trace    = GetPrivateProfileIntW(L"dx", L"d3d_trace",    trace_at(1), ini);
    g_d3d_windowed  = ini_int_title(L"dx", L"d3d_windowed",  1, ini);
    // d3d_fitwindow is RETIRED (see the declaration). Read only to say so: an
    // install carrying =0 is in the broken state that retirement exists to end,
    // and silently ignoring the row would leave the user's own ini disagreeing
    // with the screen in front of them.
    if (GetPrivateProfileIntW(L"dx", L"d3d_fitwindow", 1, ini) == 0)
        logf("[d3d] NOTE: [dx] d3d_fitwindow=0 in the ini is IGNORED -- it is "
             "retired. It skipped both halves of the windowed conversion (size "
             "the client to the backbuffer, and turn the title's fullscreen "
             "WS_POPUP into a real window), which left every game an unmovable "
             "borderless box. Delete the row; d3d_windowed owns this now.");
    g_d3d_unmask    = ini_int_title(L"dx", L"d3d_unmask",    2, ini);
    g_d3d_blackbg   = ini_int_title(L"dx", L"d3d_blackbg",   1, ini);
    g_d3d_cursor    = GetPrivateProfileIntW(L"dx", L"d3d_cursor",    2, ini);
    g_d3d_nosteal   = ini_int_title(L"dx", L"d3d_nosteal",   1, ini);
    g_d3d_nograb    = ini_int_title(L"dx", L"d3d_nograb",    1, ini);
    g_nosteal_grace = GetPrivateProfileIntW(L"dx", L"d3d_nosteal_grace", 60000, ini);
    g_d3d_msgspy    = GetPrivateProfileIntW(L"dx", L"d3d_msgspy",    trace_at(2), ini);
    g_d3d_fakemode  = GetPrivateProfileIntW(L"dx", L"d3d_fakemode",  1, ini);
    // LEVEL 3: this WRITES a SizeOfImage-byte dump of the calling title next to
    // our DLL on the first CreateDevice. It shipped compiled ON -- an RE tool
    // running in every player's session, producing a multi-megabyte file nobody
    // asked for.
    g_hang_dump_ms   = GetPrivateProfileIntW(L"dx", L"hang_dump_ms", 0, ini);
    g_d3d_frame     = GetPrivateProfileIntW(L"dx", L"d3d_frame",     1, ini);
    g_d3d_inputhwnd = GetPrivateProfileIntW(L"dx", L"d3d_inputhwnd", 1, ini);
    // DEFAULT 0, matching every shipped ini. It used to compile as 1, so an
    // install whose polshim.ini predates the key got a behaviour nobody tested:
    // GetWindowRect(GetDesktopWindow()) answering with the game's 640x480
    // drawable area, which is what pol.exe's FUN_004067B0 centres windows
    // against (see hook_GetWindowRect). The substitution is gated on the title
    // module now, but the default should still be the value we actually ship.
    g_d3d_clientrect = GetPrivateProfileIntW(L"dx", L"d3d_clientrect", 0, ini);
    g_d3d_freecursor = ini_int_title(L"dx", L"d3d_freecursor", 0, ini);
    g_tm_se_unstick  = GetPrivateProfileIntW(L"dx", L"tm_se_unstick",     0, ini);
    g_tm_unstick_rva = GetPrivateProfileIntW(L"dx", L"tm_se_unstick_rva", 0x2FF294, ini);
    g_tm_unstick_id  = GetPrivateProfileIntW(L"dx", L"tm_se_unstick_id",  201, ini);
    g_tm_unstick_sub = GetPrivateProfileIntW(L"dx", L"tm_se_unstick_sub", 0, ini);
    g_tm_diag        = GetPrivateProfileIntW(L"dx", L"tm_diag",           1, ini);
    g_tm_basedir_rva = GetPrivateProfileIntW(L"dx", L"tm_diag_basedir_rva", 0x2FF308, ini);
    g_tm_map_rva     = GetPrivateProfileIntW(L"dx", L"tm_diag_map_rva",     0x2FC438, ini);
    g_tm_resv_diag      = GetPrivateProfileIntW(L"dx", L"tm_resv_diag",         1, ini);
    g_tm_resv_obj_rva   = GetPrivateProfileIntW(L"dx", L"tm_resv_obj_rva",   0x2B6254, ini);
    g_tm_resv_store_rva = GetPrivateProfileIntW(L"dx", L"tm_resv_store_rva", 0x298250, ini);
    g_tm_resv_count_rva = GetPrivateProfileIntW(L"dx", L"tm_resv_count_rva", 0x2B31C4, ini);
    g_d3d_scale      = ini_int_title(L"dx", L"d3d_scale",      0, ini);
    g_d3d_remember   = GetPrivateProfileIntW(L"dx", L"d3d_remember_window", 1, ini);
    g_saved_cw       = GetPrivateProfileIntW(L"dx", L"d3d_window_w", 0, ini);
    g_saved_ch       = GetPrivateProfileIntW(L"dx", L"d3d_window_h", 0, ini);
    // A remembered size is only honoured when it carries the rev marker, i.e. it was
    // written by remember_window_size SINCE sizes became strictly user-chosen (the
    // 2026-08-18 rework). Older builds' d3d_window_w/h mixed shim-imposed sizes in
    // with the user's, which is how a Deck ended up pinned to a stale postage stamp
    // that beat d3d_scale=auto on every launch. Unmarked values are discarded ONCE,
    // here, so scale/auto decides again; the next hand resize or maximise re-records
    // with the marker and wins from then on.
    if ((g_saved_cw > 0 || g_saved_ch > 0) &&
        GetPrivateProfileIntW(L"dx", L"d3d_window_rev", 0, ini) < 1) {
        logf("[d3d] discarding the remembered client %dx%d: it predates the "
             "user-sized-only regime (no d3d_window_rev), so d3d_scale decides "
             "again; resizing or maximising the window re-records it",
             g_saved_cw, g_saved_ch);
        g_saved_cw = g_saved_ch = 0;
        WritePrivateProfileStringW(L"dx", L"d3d_window_w", NULL, ini);
        WritePrivateProfileStringW(L"dx", L"d3d_window_h", NULL, ini);
    }
    wcscpy_s(g_dx_ini, ini);
    g_d3d_aspect     = ini_int_title(L"dx", L"d3d_aspect",     1, ini);
    g_d3d_borderless = ini_int_title(L"dx", L"d3d_borderless", 0, ini);
    g_d3d_renderspy  = GetPrivateProfileIntW(L"dx", L"d3d_renderspy",  trace_at(3), ini);
    g_d3d_fitvideo   = ini_int_title(L"dx", L"d3d_fitvideo",   0, ini);
    // IMPORTANT: DEFAULT EMPTY SINCE 2026-08-24. It was L"FFXiMain.dll" -- and the reasoning
    // for that is worth keeping, because it was right for its time: d3d_windowed was
    // an opt-in checkbox, ticking it black-screened FFXI on a Steam Deck (2026-08-15),
    // and "a user-facing toggle must not need a second, undocumented key set correctly
    // to be safe" is still the rule. The safe value was the default.
    //
    // What changed is which way "safe" points. d3d_windowed now ships as 1, so this
    // list is no longer a guard on a toggle somebody chose -- it is a title being held
    // in the exclusive-fullscreen mode the account holder reports as the actual
    // problem ("cannot leave the game"). Requested explicitly: everything windowed, no
    // exceptions. FFXI's black screen is a real measurement and is NOT disproved; it is
    // accepted, with this list kept as the one-line way back. See profiles.cpp's FFXI
    // row for the full accounting and for the better unbuilt fix (FFXI's own native
    // windowed mode, in its numbered registry values).
    //
    // WARNING: Clearing this does NOT touch the add-on-core stand-down below in
    // caller_except_why(). With Ashita or Windower loaded FFXI's device is still left
    // entirely alone -- that is a correctness guard, not a preference.
    wchar_t exceptw[256] = L"";
    ini_str(L"dx", L"d3d_windowed_except", L"", exceptw,
            _countof(exceptw), ini);
    WideCharToMultiByte(CP_ACP, 0, exceptw, -1, g_windowed_except,
                        sizeof(g_windowed_except), NULL, NULL);

    // [dx] d3d_windowed_force -- the exact opposite of d3d_windowed_except, and
    // the thing that list could never do. The except list ADDS a title to "leave
    // fullscreen"; this one REMOVES one, by clearing the verdict its compat
    // PROFILE hard-codes.
    //
    // Until 2026-08-21 caller_excepted()'s own comment claimed the except list
    // also served "a user who deliberately clears a profile's default". It did
    // not: the PW_FULLSCREEN branch returned true BEFORE the list was ever read,
    // so a title in the profile table could not be windowed from the ini at all.
    // On a machine whose adapter cannot serve the fullscreen mode the title asks
    // for -- no 800x600 mode on a modern panel, the b59 case -- that left no way
    // to run the title, because the one setting that would have helped was
    // documented, believed, and inert.
    //
    // Empty by default: a profile is a MEASURED verdict and overriding it costs
    // whatever the profile exists to avoid (for FMO, the windowless VMR-9 FMV
    // racing a windowed Present). This is a deliberate escape hatch, not a knob
    // to reach for.
    wchar_t forcew[256] = L"";
    ini_str(L"dx", L"d3d_windowed_force", L"", forcew, _countof(forcew), ini);
    WideCharToMultiByte(CP_ACP, 0, forcew, -1, g_windowed_force,
                        sizeof(g_windowed_force), NULL, NULL);
    if (g_windowed_force[0])
        logf("[d3d] d3d_windowed_force=\"%s\" -- these titles' profile window "
             "verdicts are CLEARED; the global d3d_windowed decides instead",
             g_windowed_force);

    // Which window classes count as the FMV window -- see is_video_class.
    wchar_t vcw[128] = L"";
    ini_str(L"dx", L"d3d_videoclass", L"VideoRenderer,FilterGraphWindow",
            vcw, _countof(vcw), ini);
    if (vcw[0])
        WideCharToMultiByte(CP_ACP, 0, vcw, -1, g_videoclass,
                            sizeof(g_videoclass), NULL, NULL);

    wchar_t bb[64] = L"";
    ini_str(L"dx", L"d3d_bbsize", L"", bb, _countof(bb), ini);
    if (bb[0]) {
        unsigned w = 0, h = 0;
        if (swscanf_s(bb, L"%ux%u", &w, &h) == 2 && w && h) {
            g_bb_force_w = w; g_bb_force_h = h;
        } else {
            logf("[d3d] d3d_bbsize=\"%ls\" is not WxH -- ignored", bb);
        }
    }
    display_apply(ini);
    if (g_display != DM_UNSET)
        logf("[d3d] display=%ls (%s) -> windowed=%d borderless=%d; the legacy "
             "d3d_windowed / d3d_windowed_except / d3d_borderless rows are overridden",
             display_mode_name(g_display), title_scope()[0] ? title_scope() : "global",
             g_d3d_windowed, g_d3d_borderless);
}

// Live re-read for the settings dialog: the curated subset a running session can
// safely change. d3d_enable is NOT re-read -- it decided at startup whether the
// d3d8 factory got patched. The d3d_window_rev/w/h migration block is NOT re-run
// either: it DELETES ini rows, and g_saved_cw/ch are the user's remembered
// window size, owned by remember_window_size, not by config.
void d3d8_reload(const wchar_t* ini)
{
    g_d3d_trace    = GetPrivateProfileIntW(L"dx", L"d3d_trace",    trace_at(1), ini);
    g_d3d_windowed  = ini_int_title(L"dx", L"d3d_windowed",  1, ini);
    g_d3d_unmask    = ini_int_title(L"dx", L"d3d_unmask",    2, ini);
    g_d3d_blackbg   = ini_int_title(L"dx", L"d3d_blackbg",   1, ini);
    g_d3d_cursor    = GetPrivateProfileIntW(L"dx", L"d3d_cursor",    2, ini);
    g_d3d_nosteal   = ini_int_title(L"dx", L"d3d_nosteal",   1, ini);
    g_d3d_nograb    = ini_int_title(L"dx", L"d3d_nograb",    1, ini);
    g_d3d_freecursor = ini_int_title(L"dx", L"d3d_freecursor", 0, ini);
    g_d3d_scale      = ini_int_title(L"dx", L"d3d_scale",      0, ini);
    g_d3d_remember   = GetPrivateProfileIntW(L"dx", L"d3d_remember_window", 1, ini);
    g_d3d_aspect     = ini_int_title(L"dx", L"d3d_aspect",     1, ini);
    g_d3d_borderless = ini_int_title(L"dx", L"d3d_borderless", 0, ini);
    g_d3d_renderspy  = GetPrivateProfileIntW(L"dx", L"d3d_renderspy",  trace_at(3), ini);
    g_d3d_fitvideo   = ini_int_title(L"dx", L"d3d_fitvideo",   0, ini);

    // Empty, matching d3d8_configure above (2026-08-24). These two defaults MUST agree:
    // this is the live-reload path ([[shim-settings-live-reload]], Save applies without
    // a restart), so a disagreement means the same ini produces one behaviour on launch
    // and a different one after pressing Save -- which reads as the setting being
    // ignored, and is unfindable from the ini.
    wchar_t exceptw[256] = L"";
    ini_str(L"dx", L"d3d_windowed_except", L"", exceptw,
            _countof(exceptw), ini);
    WideCharToMultiByte(CP_ACP, 0, exceptw, -1, g_windowed_except,
                        sizeof(g_windowed_except), NULL, NULL);

    wchar_t forcew[256] = L"";
    ini_str(L"dx", L"d3d_windowed_force", L"", forcew, _countof(forcew), ini);
    WideCharToMultiByte(CP_ACP, 0, forcew, -1, g_windowed_force,
                        sizeof(g_windowed_force), NULL, NULL);

    wchar_t vcw[128] = L"";
    ini_str(L"dx", L"d3d_videoclass", L"VideoRenderer,FilterGraphWindow",
            vcw, _countof(vcw), ini);
    if (vcw[0])
        WideCharToMultiByte(CP_ACP, 0, vcw, -1, g_videoclass,
                            sizeof(g_videoclass), NULL, NULL);

    // Cleared before the parse so REMOVING d3d_bbsize also takes effect live;
    // configure never needs that because it starts from zero.
    g_bb_force_w = g_bb_force_h = 0;
    wchar_t bb[64] = L"";
    ini_str(L"dx", L"d3d_bbsize", L"", bb, _countof(bb), ini);
    if (bb[0]) {
        unsigned w = 0, h = 0;
        if (swscanf_s(bb, L"%ux%u", &w, &h) == 2 && w && h) {
            g_bb_force_w = w; g_bb_force_h = h;
        } else {
            logf("[d3d] d3d_bbsize=\"%ls\" is not WxH -- ignored", bb);
        }
    }

    display_apply(ini);
    logf("[reload] d3d8: display=%ls trace=%d windowed=%d unmask=%d cursor=%d nosteal=%d "
         "nograb=%d freecursor=%d scale=%d remember=%d aspect=%d borderless=%d "
         "renderspy=%d fitvideo=%d except=\"%s\" force=\"%s\" videoclass=\"%s\" "
         "bbsize=%ux%u",
         display_mode_name(g_display),
         g_d3d_trace, g_d3d_windowed, g_d3d_unmask, g_d3d_cursor, g_d3d_nosteal,
         g_d3d_nograb, g_d3d_freecursor, g_d3d_scale, g_d3d_remember, g_d3d_aspect,
         g_d3d_borderless, g_d3d_renderspy, g_d3d_fitvideo, g_windowed_except,
         g_windowed_force, g_videoclass, g_bb_force_w, g_bb_force_h);
}

// PER-TITLE opt-out for the windowed override.
//
// The override is a global switch, but "force this fullscreen game windowed" is
// a per-title judgement: Tetra Master wants it, and a title whose input breaks
// under it does not. One d3d8.dll vtable serves TM.dll, FFXiMain.dll and
// FE_Client.dll, so the hook cannot tell them apart -- but CreateDevice's RETURN
// ADDRESS lands inside whichever title called it, which names it exactly. That
// is the same fact d3d_dumpcaller already relies on.
//
// Value is a comma-separated list of module leaf names, e.g.
//   d3d_windowed_except=FE_Client.dll
// Whole-field, case-insensitive membership in a comma-separated module list.
// Substring matching would let "TM.dll" match "SYSTM.dll", so the field has to
// end exactly where the leaf does. Shared by d3d_windowed_except and its
// opposite number d3d_windowed_force.
static bool list_has_module(const char* list, const char* leaf)
{
    if (!list || !list[0] || !leaf) return false;
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char* start = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - start);
        while (n && start[n - 1] == ' ') n--;
        if (n && _strnicmp(leaf, start, n) == 0 && leaf[n] == '\0') return true;
    }
    return false;
}

// WHY a caller is excepted, not just whether. THREE different sources can except
// a title and they are fixed in three different places -- an add-on core in the
// process, the compiled profile table, and the ini list. A log line that names
// the wrong one sends the reader to a file that has nothing to do with the
// behaviour, which is exactly what "caller is in d3d_windowed_except" did for
// FMO: FMO is not in that list and never was. Its exception comes from
// PW_FULLSCREEN in profiles.cpp, so a user who set "Run games in a window",
// watched FMO go fullscreen anyway, and went to check d3d_windowed_except found
// nothing there and no way forward. Reporting WHICH source excepted a title is
// the difference between a log that explains the behaviour and one that denies
// it.
enum ExceptWhy { EXC_NONE = 0, EXC_ADDON_CORE, EXC_PROFILE, EXC_INI_LIST };

static bool caller_except_why(void* retaddr, ExceptWhy* why)
{
    if (why) *why = EXC_NONE;
    // The FFXI add-on case is an exception with no ini entry behind it, so it is
    // tested BEFORE the empty-list bail: with Ashita or Windower in the process the
    // list can be empty and FFXI must still be left alone.
    if (!retaddr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(retaddr, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return false;
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) return false;
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;

    // HANDS OFF FFXI'S DEVICE WHILE AN ADD-ON CORE IS PRESENT (ffxiplug.cpp).
    //
    // This one switch is the whole stand-down, because every intrusive thing this
    // file does to a title -- the windowed override, fit_window, the mask realign,
    // the window icon, the Reset override -- lives inside the branch it guards.
    // Ashita and Windower each do their own device wrapping and their own
    // windowing; two of us resizing and re-presenting one device is how a lost
    // device happens, and the user would read it as the add-on being broken.
    if (_stricmp(leaf, "FFXiMain.dll") == 0 && ffxiplug_compat_active()) {
        static volatile LONG said = 0;
        if (say_once_per_title(&said))
            logf("[d3d] FFXI is running under %s -- leaving its device entirely "
                 "alone (no windowed override, no fit, no mask realign)",
                 ffxiplug_core() ? ffxiplug_core() : "an add-on core");
        if (why) *why = EXC_ADDON_CORE;
        return true;
    }

    // ...and the arm the live run proved necessary (2026-08-18):
    // Ashita interposes the D3D8 layer, so FFXI's device calls arrive with the
    // return address inside ASHITA's module, not FFXiMain -- the test above never
    // fired in the first real session. A call made by the core itself is FFXI's
    // device coming through the core's interposer; same hands-off treatment.
    // (Consequence to measure, not guess at: if a core wraps EVERY caller's
    // device, TM/FE calls arrive as the core too and lose the shim's handling --
    // that is the add-on coexistence experiment, and the workaround is simply
    // launching those titles without the core.)
    if (ffxiplug_compat_active() && ffxiplug_module_is_core(path, leaf)) {
        // ...BUT ONLY WHILE FFXI IS ACTUALLY IN THIS PROCESS.
        //
        // This arm exists because a core interposes the D3D8 layer, so FFXI's device
        // calls arrive wearing the core's return address. The cost, written down as
        // the add-on coexistence experiment and never run, is that EVERY title's calls arrive that way
        // -- so loading Ashita for FFXI silently stripped Tetra Master and Fantasy
        // Earth of the shim's handling too, and the only advice was "turn it off
        // before playing something else". That is the whole reason [ashita] enable
        // ships off.
        //
        // FFXiMain.dll being absent is a POSITIVE, cheap proof that the title on
        // screen is not FFXI: the module is loaded when FFXI launches and this is the
        // Viewer's own process, so no FFXiMain means FFXI has not run this session.
        // The test is one-directional on purpose -- once FFXI HAS loaded the module
        // stays mapped, so a later title in the same session falls back to standing
        // down, which is exactly today's behaviour. This can only ever RESTORE shim
        // handling in a session FFXI never touched; it can never remove it.
        if (!GetModuleHandleW(L"FFXiMain.dll")) {
            static volatile LONG said_nof = 0;
            if (say_once_per_title(&said_nof))
                logf("[d3d] device call from %s itself (%s), but FFXiMain is NOT loaded "
                     "-- this is not FFXI, so the shim keeps its own handling for this "
                     "title (the core is in the process for a LATER FFXI launch)",
                     ffxiplug_core() ? ffxiplug_core() : "the add-on core", leaf);
            // Deliberately falls THROUGH to the profile and ini-list tests below: this
            // says "not excepted for the add-on reason", not "never excepted".
        } else {
            static volatile LONG said = 0;
            if (say_once_per_title(&said))
                logf("[d3d] device call from %s itself (%s) -- FFXI's device arrives "
                     "through the core's interposer; leaving it entirely alone",
                     ffxiplug_core() ? ffxiplug_core() : "the add-on core", leaf);
            if (why) *why = EXC_ADDON_CORE;
            return true;
        }
    }

    // PROFILE AS AUTHORITY. A title's profile names the window treatment it needs,
    // delivered in CODE so it reaches an install that never saw a shipped ini (the
    // iniheal Rule-1 gap). Two profile verdicts mean "keep this title fullscreen --
    // the shim must not override its device":
    //   PW_FULLSCREEN        NO TITLE USES THIS ANY MORE (2026-08-24). It was FFXI and
    //                        FMO -- windowed black-screens / races the FMV -- and that
    //                        verdict outranked the global switch, which is why "Run
    //                        games in a window" did nothing for either of them. Both
    //                        are PW_DEFAULT now and the branch below is dead code on a
    //                        stock build, kept because the mechanism is right.
    //   PW_NATIVE_WINDOWED   (Fantasy Earth): the TITLE windows itself via -windowmode,
    //                        so the shim's override would fight SE's own windowing.
    // The ini list stays as an explicit override for anything not in the table, or for
    // a user who deliberately clears a profile's default (see caller_excepted note).
    // An ini that deliberately clears this title's profile verdict wins over the
    // table -- but NOT over the add-on stand-down above, which is a correctness
    // guard (two device wrappers on one device is how a lost device happens),
    // not a preference. Order is the whole safety here.
    // THE DISPLAY ROW OUTRANKS BOTH LISTS (2026-09-08). [dx.<leaf>] display /
    // [dx] display is the one row a player sees; when it is set it is the whole
    // answer for this half of the decision, and the except/force lists below are
    // the compatibility path for an ini that never had it.
    const DisplayMode dm = display_mode_for(leaf, NULL);
    const bool lists_apply = (dm == DM_UNSET);
    if (dm == DM_FULLSCREEN) {
        static volatile LONG said = 0;
        if (say_once_per_title(&said))
            logf("[d3d] display=fullscreen for %s -- its device is left in the mode it "
                 "asks for (the title's own exclusive fullscreen)", leaf);
        if (why) *why = EXC_INI_LIST;
        return true;
    }

    // THE USER'S OWN "LEAVE THIS TITLE FULLSCREEN" IS READ FIRST.
    //
    // d3d_windowed_except is what the "Titles left fullscreen" dropdown writes, so
    // it is a direct, per-title statement of intent and nothing below may silently
    // overrule it. It used to be read LAST, after d3d_windowed_force -- and force
    // returns false, so a title named in BOTH lists was forced windowed and its
    // except entry was dead. Found 2026-08-25 in a shipped ini that had exactly
    // that: except="FFXiMain.dll,FrontMissionOnline.dll" force="FrontMissionOnline.dll".
    //
    // WARNING: THE "force IS A NO-OP NOW" CLAIM IS WRONG, and this is the retraction.
    // [[shim-windowed-by-default]] recorded that with no PW_FULLSCREEN rows left
    // d3d_windowed_force does nothing and could be hidden. It never stopped doing
    // ONE thing: returning early, which skips the except list underneath it. A knob
    // whose stated job is dead can still have a live side effect, and hiding it
    // from the dialog made that side effect unreachable to the person it affected.
    if (lists_apply && list_has_module(g_windowed_except, leaf)) {
        if (list_has_module(g_windowed_force, leaf)) {
            static volatile LONG said = 0;
            if (say_once_per_title(&said))
                logf("[d3d] %s is in BOTH [dx] d3d_windowed_except and "
                     "d3d_windowed_force, which contradict. EXCEPT WINS -- the title "
                     "is left fullscreen, because that is the one the dropdown "
                     "writes. Remove it from d3d_windowed_force to silence this.",
                     leaf);
        }
        if (why) *why = EXC_INI_LIST;
        return true;
    }

    if (lists_apply && list_has_module(g_windowed_force, leaf)) {
        static volatile LONG said = 0;
        if (say_once_per_title(&said))
            logf("[d3d] d3d_windowed_force names %s -- its profile's window "
                 "verdict is CLEARED; the global d3d_windowed decides. Whatever "
                 "the profile existed to avoid is now in play.", leaf);
        return false;
    }

    const TitleProfile* prof = profile_for_module(leaf);
    if (prof && prof->window == PW_FULLSCREEN) {
        static volatile LONG said = 0;
        if (say_once_per_title(&said))
            logf("[d3d] profile: leaving %s's device alone (%s)", prof->title, prof->note);
        if (why) *why = EXC_PROFILE;
        return true;
    }
    // PW_NATIVE_WINDOWED (Fantasy Earth): this build does not deliver SE's own
    // -windowmode switch, so excepting the title would strand it in the
    // exclusive-fullscreen mode it CRASHES in. Fall through and let the ordinary
    // windowed override force it windowed the proven way instead.
    if (prof && prof->window == PW_NATIVE_WINDOWED) {
        static volatile LONG said = 0;
        if (say_once_per_title(&said))
            logf("[d3d] profile: %s is native-windowed; -windowmode is not delivered "
                 "by this build -- using the shim's windowed override so it is not "
                 "left fullscreen", prof->title);
        // not excepted -> the windowed override applies.
    }

    // Unreachable for a listed module now that the except list is read at the top,
    // and deliberately kept: it costs one string compare and it means the function
    // still gives the right answer if the order above is ever changed back.
    if (lists_apply && list_has_module(g_windowed_except, leaf)) {
        if (why) *why = EXC_INI_LIST;
        return true;
    }
    return false;
}

static bool caller_excepted(void* retaddr)
{
    return caller_except_why(retaddr, NULL);
}

// EXCEPTED *BECAUSE OF AN ADD-ON CORE* -- which is a different question from
// "excepted", and the two were conflated until 2026-08-25.
//
// The stand-down is one boolean guarding five things: the windowed override, the
// Reset override, fit_window, the POL mask realign and the window icon. Only the
// first three are things Ashita or Windower also do. MEASURED, not assumed: the
// shipped Ashita.dll overrides nine presentparams.* keys and `windowed` is not one
// of them -- the string does not occur in the binary at all -- and neither core has
// ever heard of PlayOnline's mask window, which is a Viewer artifact they cannot
// see. So handing those last two over hands them to nobody.
//
// Callers use this to replay the POL-specific half inside the pass-through branch,
// exactly as the native-windowed arm below already does for Fantasy Earth.
static bool caller_excepted_by_core(void* retaddr)
{
    ExceptWhy w = EXC_NONE;
    return caller_except_why(retaddr, &w) && w == EXC_ADDON_CORE;
}

// Where an exception came from, in words -- for the log line that used to blame
// d3d_windowed_except for every one of them.
static const char* except_source(ExceptWhy w)
{
    switch (w) {
        case EXC_ADDON_CORE: return "an add-on core is present";
        case EXC_PROFILE:    return "its per-title compat PROFILE (profiles.cpp), "
                                    "NOT the ini list -- clear it with "
                                    "[dx] d3d_windowed_force";
        case EXC_INI_LIST:   return "the ini list [dx] d3d_windowed_except";
        default:             return "an unrecorded source";
    }
}
// Is the caller a title that windows ITSELF (PW_NATIVE_WINDOWED -- Fantasy Earth via
// -windowmode)? Such a title creates a WINDOWED device from its own code, so the
// fullscreen-CreateDevice signal the shim uses to spot "a title is starting" never
// fires for it, and the windowed-override branch is skipped (it is excepted). But it
// still needs the parts of the post-device sequence that are NOT about sizing -- above
// all the FMV adoption, since no POL title places its own video window. This
// tells those parts apart from an ordinary excepted fullscreen title (FFXI, FMO).
static bool caller_native_windowed(void* retaddr)
{
    const TitleProfile* p = profile_for_addr(retaddr);
    return p && p->window == PW_NATIVE_WINDOWED;
}

int d3d8_enabled() { return g_d3d_enable; }
HWND d3d8_game_window() { return g_game_window; }

// The two pieces of window state inputgate.cpp needs. Both are written ONLY by
// spy_proc (WM_ENTERSIZEMOVE / WM_SIZE), which is why they live here and are
// read through accessors rather than duplicated: a second reader of the same
// user intent that disagreed with the focus hooks would be its own bug.
LONG d3d8_in_modal()       { return InterlockedCompareExchange(&g_in_modal, 0, 0); }
LONG d3d8_user_minimized() { return InterlockedCompareExchange(&g_user_minimized, 0, 0); }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static const char* swapeffect_name(DWORD s)
{
    switch (s) {
        case 1: return "DISCARD";
        case 2: return "FLIP";
        case 3: return "COPY";
        case 4: return "COPY_VSYNC";
        default: return "?";
    }
}

static void log_pp(const char* who, const D3D8_PRESENT_PARAMETERS* p)
{
    if (!p) { logf("[d3d] %s: <null present params>", who); return; }
    logf("[d3d] %s: %ux%u fmt=%lu count=%u swap=%s hwnd=%p Windowed=%d "
         "refresh=%uHz interval=%u flags=%08lX",
         who, p->BackBufferWidth, p->BackBufferHeight, p->BackBufferFormat,
         p->BackBufferCount, swapeffect_name(p->SwapEffect), p->hDeviceWindow,
         p->Windowed, p->FullScreen_RefreshRateInHz,
         p->FullScreen_PresentationInterval, p->Flags);
}

// The real desktop mode, used as ground truth for the vtable check below.
static bool desktop_mode(UINT* w, UINT* h)
{
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) return false;
    *w = dm.dmPelsWidth; *h = dm.dmPelsHeight;
    return true;
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

// Arm `slot` ONLY if it still holds `expect`, capturing `expect` as the original.
//
// The plain patch_slot above reads the slot and writes it as two steps, which is
// fine for a first arm but not for a RE-arm: two threads doing "the slot is not
// ours, take it" can interleave so that the second one captures the FIRST one's
// hook as "the original", and the hook then calls itself until the stack runs
// out. That is the gamestart re-arm crash, and it is a real hazard here because
// the whole point of a re-arm is that it happens more than once. The exchange
// itself decides the winner, so the loser is told and changes nothing.
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

// Name whatever is sitting at a code address, for the log: module + RVA, with an
// unmapped address called out loudly rather than printed as a bare number. Same
// shape as the crash logger's frame lines, but that one is static to
// crashlog.cpp; this is the shared copy the vtable arming uses to say WHO has
// taken a slot off us. Exported for d3d9hook.cpp, which has the same problem.
void dx_describe_code(const void* p, char* out, size_t cb)
{
    HMODULE m = NULL;
    char full[MAX_PATH];
    if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)p, &m) && m &&
        GetModuleFileNameA(m, full, sizeof(full))) {
        const char* leaf = strrchr(full, '\\');
        _snprintf_s(out, cb, _TRUNCATE, "%p = %s+0x%IX", p,
                    leaf ? leaf + 1 : full,
                    (size_t)((const unsigned char*)p - (const unsigned char*)m));
    } else {
        _snprintf_s(out, cb, _TRUNCATE, "%p = NOT IN ANY LOADED MODULE (unmapped "
                    "-- whoever hooked it has been unloaded without restoring)", p);
    }
}

// ---------------------------------------------------------------------------
// vtable validation -- run BEFORE anything is patched
// ---------------------------------------------------------------------------

// Proves the hand-declared slot numbering matches the real d3d8.dll by asking
// the object for something we already know the answer to. A shifted vtable
// cannot pass this: slot 8 would not return the true desktop resolution.
static bool validate_d3d8(void* d3d8)
{
    UINT dw = 0, dh = 0;
    if (!desktop_mode(&dw, &dh)) {
        logf("[d3d] cannot read the desktop mode -- refusing to patch unvalidated");
        return false;
    }
    void** vt = *(void***)d3d8;
    UINT adapters = 0;
    D3D8_DISPLAYMODE m;
    ZeroMemory(&m, sizeof(m));

    __try {
        adapters = ((PFN_GetAdapterCount)vt[S_GetAdapterCount])(d3d8);
        if (adapters == 0 || adapters > 16) {
            logf("[d3d] vtable check FAILED: GetAdapterCount (slot %d) -> %u",
                 S_GetAdapterCount, adapters);
            return false;
        }
        if (FAILED(((PFN_GetAdapterDisplayMode)vt[S_GetAdapterDisplayMode])(d3d8, 0, &m))) {
            logf("[d3d] vtable check FAILED: GetAdapterDisplayMode (slot %d) errored",
                 S_GetAdapterDisplayMode);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("[d3d] vtable check FAULTED -- the hand-declared slot numbers are wrong "
             "for this d3d8.dll; hook disabled, nothing patched");
        return false;
    }

    if (m.Width != dw || m.Height != dh) {
        logf("[d3d] vtable check FAILED: adapter says %ux%u, desktop is %ux%u "
             "-- slot numbering suspect, nothing patched", m.Width, m.Height, dw, dh);
        return false;
    }
    logf("[d3d] vtable VALIDATED: %u adapter(s), adapter mode %ux%u @%uHz == desktop",
         adapters, m.Width, m.Height, m.RefreshRate);
    return true;
}

static bool validate_device(void* dev)
{
    UINT dw = 0, dh = 0;
    if (!desktop_mode(&dw, &dh)) return false;
    void** vt = *(void***)dev;
    D3D8_DISPLAYMODE m;
    ZeroMemory(&m, sizeof(m));
    __try {
        if (FAILED(((PFN_DevGetDisplayMode)vt[SD_GetDisplayMode])(dev, &m))) {
            logf("[d3d] device vtable check FAILED: GetDisplayMode errored");
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("[d3d] device vtable check FAULTED -- Reset left unhooked");
        return false;
    }
    // In fullscreen the device mode is the mode it just SET, so accept either
    // the desktop mode or any sane mode -- the point is that the call returned
    // a plausible structure rather than garbage from a wrong slot.
    if (m.Width < 320 || m.Width > 16384 || m.Height < 200 || m.Height > 16384) {
        logf("[d3d] device vtable check FAILED: GetDisplayMode -> %ux%u (implausible)",
             m.Width, m.Height);
        return false;
    }
    logf("[d3d] device vtable VALIDATED: device display mode %ux%u @%uHz "
         "(desktop %ux%u)", m.Width, m.Height, m.RefreshRate, dw, dh);
    return true;
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------

// The adapter's CURRENT display-mode format. A windowed backbuffer has to be
// compatible with the desktop, and the game's fullscreen choice (often R5G6B5)
// usually is not -- so we ask rather than assume.
static DWORD adapter_format(void* d3d8, UINT adapter)
{
    void** vt = *(void***)d3d8;
    D3D8_DISPLAYMODE m;
    ZeroMemory(&m, sizeof(m));
    __try {
        if (SUCCEEDED(((PFN_GetAdapterDisplayMode)vt[S_GetAdapterDisplayMode])(d3d8, adapter, &m)))
            return m.Format;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return D3D8FMT_UNKNOWN;
}

static DWORD device_format(void* dev)
{
    void** vt = *(void***)dev;
    D3D8_DISPLAYMODE m;
    ZeroMemory(&m, sizeof(m));
    __try {
        if (SUCCEEDED(((PFN_DevGetDisplayMode)vt[SD_GetDisplayMode])(dev, &m)))
            return m.Format;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return D3D8FMT_UNKNOWN;
}

static void log_window(const char* tag, HWND h)
{
    if (!h) { logf("[d3d] %s: <null hwnd>", tag); return; }
    RECT w, c;
    ZeroMemory(&w, sizeof(w)); ZeroMemory(&c, sizeof(c));
    GetWindowRect(h, &w);
    GetClientRect(h, &c);
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    LONG st = GetWindowLongA(h, GWL_STYLE), ex = GetWindowLongA(h, GWL_EXSTYLE);
    logf("[d3d] %s hwnd=%p class=%s win=(%ld,%ld %ldx%ld) client=%ldx%ld "
         "style=%08lX exstyle=%08lX visible=%d",
         tag, h, cls, w.left, w.top, w.right - w.left, w.bottom - w.top,
         c.right, c.bottom, st, ex, IsWindowVisible(h) ? 1 : 0);
}

// Turn the game's exclusive-mode window into an actual window.
//
// A game that only ever ran fullscreen makes a screen-sized borderless window
// and lets the exclusive device cover everything; the window's style and size
// never mattered. Once the device is windowed they matter completely -- leave it
// alone and you get a screen-filling frameless window, which is what "it
// definitely wasn't windowed" looks like, and the pointer keeps mapping against
// a client area that is not the size of the rendered image.
//
// Sizing the CLIENT area to exactly the backbuffer is also the mouse fix: it
// puts the rendered image and the OS pointer back in one coordinate space.
// ---------------------------------------------------------------------------
// BORDERLESS FULLSCREEN, with letterboxing.
//
// Exclusive fullscreen makes the cursor correct for free -- the game's field IS
// the screen -- but it costs alt-tab, which is a daily annoyance traded for an
// intermittent one. A borderless window covering the monitor keeps alt-tab and
// overlays working AND keeps the pointer inside the game area, which is what the
// absolute tracking needs.
//
// The catch is shape: the backbuffer is 4:3 and a modern monitor is not, so
// filling the screen would stretch the picture -- the exact fault d3d_aspect
// exists to prevent. So the GAME window is sized to the largest correctly-shaped
// rect that fits and centred, and a plain black window is parked behind it to
// cover what is left. D3D needs no involvement: it already stretches the
// backbuffer to the client rect, and the client rect is now the right shape.
//
// The backdrop must not become a black sheet over the desktop when the player
// alt-tabs away, so it is shown and hidden with the game's foreground state (see
// backdrop_thread). It never activates, so clicking a black bar cannot pull the
// game out of the foreground.
static HWND g_backdrop = NULL;

static LRESULT CALLBACK backdrop_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // a click on the bars does not steal focus
    case WM_CLOSE:         DestroyWindow(h); return 0;
    case WM_DESTROY:       if (h == g_backdrop) g_backdrop = NULL; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

// Poll the game's foreground state and follow it. Runs OFF the game's thread for
// the same reason mask_thread does -- nothing that touches another thread's
// window may block inside CreateDevice -- and every cross-thread window call
// here is async or non-blocking.
static DWORD WINAPI backdrop_thread(LPVOID)
{
    bool shown = true;
    for (;;) {
        HWND game = g_game_window, bd = g_backdrop;
        if (!bd || !IsWindow(bd)) return 0;
        if (!game || !IsWindow(game)) {
            // DESTROY it, do not merely hide it. A hidden window is still an
            // un-owned top-level window, and leaving one behind is what stopped
            // pol.exe from ever exiting (maskguard honours the mask's close only
            // once the mask is the LAST such window). The zombie then kept the
            // single-instance mutex and every later launch died at startup.
            // PostMessage, not DestroyWindow: the backdrop belongs to the game's
            // thread and only that thread may destroy it.
            PostMessage(bd, WM_CLOSE, 0, 0);
            return 0;
        }
        HWND fg = GetForegroundWindow();
        // Only while borderless is the mode in force: the live hotkey can switch
        // a title back to a framed window, and a black screen-sized backdrop
        // behind a framed window hides the desktop for no reason.
        bool want = g_d3d_borderless && (fg == game || fg == bd);
        if (want != shown) {
            ShowWindow(bd, want ? SW_SHOWNA : SW_HIDE);
            shown = want;
        }
        // Keep it directly BEHIND the game: inserting after `game` in z-order is
        // what stops it covering the picture it is supposed to frame.
        if (want)
            SetWindowPos(bd, game, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        Sleep(150);
    }
}

static void ensure_backdrop(HWND game, const RECT* mon)
{
    if (g_backdrop && IsWindow(g_backdrop)) {
        SetWindowPos(g_backdrop, game, mon->left, mon->top,
                     mon->right - mon->left, mon->bottom - mon->top,
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = backdrop_proc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "PolShimBackdrop";
        if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            logf("[bd] RegisterClass failed (err=%lu) -- no letterbox backdrop; the "
                 "desktop will show in the bars", GetLastError());
            return;
        }
        registered = true;
    }
    // WS_EX_NOACTIVATE: never takes focus. WS_EX_TOOLWINDOW: stays out of the
    // taskbar and Alt+Tab, so the player never sees a stray black window listed.
    // Created on the GAME's thread, so the game's own message loop pumps it.
    g_backdrop = CreateWindowExA(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "PolShimBackdrop", "",
        WS_POPUP, mon->left, mon->top,
        mon->right - mon->left, mon->bottom - mon->top,
        NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!g_backdrop) {
        logf("[bd] CreateWindowEx failed (err=%lu) -- no letterbox backdrop",
             GetLastError());
        return;
    }
    ShowWindow(g_backdrop, SW_SHOWNA);
    SetWindowPos(g_backdrop, game, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    HANDLE t = CreateThread(NULL, 0, backdrop_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    logf("[bd] letterbox backdrop %p covering (%ld,%ld %ldx%ld)", g_backdrop,
         mon->left, mon->top, mon->right - mon->left, mon->bottom - mon->top);
}

// Returns true when it has fully placed the window and fit_window should stop.
static bool fit_borderless(HWND h, UINT bw, UINT bh, LONG st, LONG ex)
{
    // The FULL monitor rect, not the work area: this mode is meant to cover the
    // taskbar the way fullscreen does.
    RECT m;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoA(mon, &mi)) m = mi.rcMonitor;
    else {
        m.left = m.top = 0;
        m.right  = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CXSCREEN)
                                         : GetSystemMetrics(SM_CXSCREEN);
        m.bottom = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CYSCREEN)
                                         : GetSystemMetrics(SM_CYSCREEN);
    }
    int mw = m.right - m.left, mh = m.bottom - m.top;
    if (mw <= 0 || mh <= 0 || !bw || !bh) return false;

    int cw, ch;
    const char* how;
    if (g_d3d_borderless >= 2) {
        // Whole-number scale: every source pixel becomes an identical square
        // block. Crisper, at the cost of thicker bars (2x on a 1080p screen
        // leaves 60 rows top and bottom as well as the sides).
        int s = 1;
        while ((int)(bw * (s + 1)) <= mw && (int)(bh * (s + 1)) <= mh) s++;
        cw = (int)bw * s; ch = (int)bh * s;
        how = "integer";
    } else {
        // Fill one axis completely, keeping the shape. Bigger picture, but a
        // fractional scale, so the upscale is uneven at the pixel level.
        cw = mw; ch = MulDiv(cw, (int)bh, (int)bw);
        if (ch > mh) { ch = mh; cw = MulDiv(ch, (int)bw, (int)bh); }
        how = "fill";
    }

    LONG want_st = (st & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP | WS_VISIBLE;
    LONG want_ex = ex & ~WS_EX_TOPMOST;
    SetWindowLongA(h, GWL_STYLE, want_st);
    SetWindowLongA(h, GWL_EXSTYLE, want_ex);

    ensure_backdrop(h, &m);

    // A borderless popup has no frame, so the window size IS the client size.
    int x = m.left + (mw - cw) / 2, y = m.top + (mh - ch) / 2;
    SetWindowPos(h, HWND_NOTOPMOST, x, y, cw, ch, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    logf("[d3d] BORDERLESS (%s): client %dx%d at (%d,%d) on a %dx%d monitor; "
         "bars %dpx each side, %dpx top and bottom",
         how, cw, ch, x, y, mw, mh, (mw - cw) / 2, (mh - ch) / 2);
    return true;
}

// A title launch (Tetra Master is the worst offender) DESTROYS the "PlayOnline
// Viewer" top-level window -- the one that carried the taskbar button -- and the
// game brings up its own new top-level window. Windows adds a taskbar button when
// a window is SHOWN with a taskbar-eligible style; it does NOT retroactively add
// one when an already-shown window's style changes. So the game window ends up
// visible and ON-SCREEN with NO taskbar entry and no easy way back -- the reported
// "the TM window disappears while it's still running; I have to use Task Manager to
// bring it back." Measured 2026-08-21 on native Windows (pol.exe is High-integrity,
// so an external SetWindowLong is refused by UIPI -- the fix MUST run in-process,
// which is here). Force the shell to (re)create the button: set WS_EX_APPWINDOW and
// hide/show. ONCE per window -- fit_window re-runs on every Reset (alt-tab, a mode
// change), and a hide/show flash on every alt-tab would be its own bug.
static HWND g_taskbar_fixed = NULL;
static void assert_taskbar_button(HWND h)
{
    if (!h || h == g_taskbar_fixed) return;
    LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return;   // backdrop/mask: deliberately taskbar-less
    SetWindowLongA(h, GWL_EXSTYLE, ex | WS_EX_APPWINDOW);
    // Self-contained hide+show so the window is guaranteed visible-with-a-button
    // afterwards regardless of what the caller's later SetWindowPos does.
    ShowWindow(h, SW_HIDE);
    ShowWindow(h, SW_SHOW);
    g_taskbar_fixed = h;
    logf("[d3d] taskbar button re-asserted for game window %p (WS_EX_APPWINDOW + "
         "hide/show) -- the title-launch handoff destroys the Viewer's taskbar entry "
         "and Windows does not recreate one for the game's own window", (void*)h);
}

static void fit_window(HWND h, UINT bw, UINT bh)
{
    if (!h || !IsWindow(h)) return;

    // NEVER UN-MINIMISE THE WINDOW. Minimising a windowed device LOSES it, and a
    // lost device is exactly when a title Resets -- which lands back here, where
    // the style write (WS_VISIBLE) and the SetWindowPos placement below would
    // together haul the window the user just minimised back onto the screen.
    // The shim's own half of "I can't minimise the game": the title's half is
    // caught in spy_proc's WM_WINDOWPOSCHANGING arm.
    //
    // A minimised window also has no useful geometry to fit TO, so there is
    // nothing lost by standing off. The next Reset or device creation after the
    // user restores it does the fit, with real numbers.
    if (IsIconic(h) || InterlockedCompareExchange(&g_user_minimized, 0, 0)) {
        static volatile LONG said = 0;
        if (say_once_per_title(&said))
            logf("[d3d] fit_window: SKIPPED -- the window is minimised. Fitting it "
                 "here would restore a window the user just put away (a Reset "
                 "arrives on every device loss, and minimising loses the device).");
        return;
    }

    // Reset() re-runs this on every device loss -- alt-tab, a mode change, a
    // display switch. Re-imposing the configured size there is what made a manual
    // resize snap back mid-session. Once the user has sized the window, the only
    // thing this may still do is fix the STYLE.
    if (InterlockedCompareExchange(&g_user_sized, 0, 0)) {
        LONG st0 = GetWindowLongA(h, GWL_STYLE);
        if (st0 & WS_POPUP) {
            SetWindowLongA(h, GWL_STYLE,
                           (st0 & ~(WS_POPUP | WS_MAXIMIZE)) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(h, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        logf("[d3d] fit_window: SKIPPED -- the user sized this window by hand");
        return;
    }
    LONG st = GetWindowLongA(h, GWL_STYLE);
    LONG ex = GetWindowLongA(h, GWL_EXSTYLE);

    // Borderless fullscreen is a different shape of answer, not a scale factor,
    // so it takes over from here rather than being one more d3d_scale value.
    if (g_d3d_borderless && fit_borderless(h, bw, bh, st, ex)) return;

    LONG want_st = (st & ~(WS_POPUP | WS_MAXIMIZE)) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    LONG want_ex = ex & ~WS_EX_TOPMOST;

    SetWindowLongA(h, GWL_STYLE, want_st);
    SetWindowLongA(h, GWL_EXSTYLE, want_ex);

    // The game's window inherited no taskbar button from the destroyed Viewer
    // window; give it one (once per window, before the branch-specific placement).
    assert_taskbar_button(h);

    // NATIVE-WINDOWED TITLE, forced windowed as a fallback: size to the backbuffer
    // EXACTLY (1x), centred, ignoring the remembered size and any scale. Fantasy Earth
    // maps its cursor in backbuffer space and assumes the window is that size; a larger
    // window (e.g. a stale remembered 1084x813 over an 800x600 backbuffer) makes its
    // on-screen cursor lag the pointer by the size ratio -- the "the busy icon follows
    // the mouse at a big offset" report. window == backbuffer removes the ratio.
    if (g_title_native_windowed) {
        int sx = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CXSCREEN)
                                       : GetSystemMetrics(SM_CXSCREEN);
        int sy = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CYSCREEN)
                                       : GetSystemMetrics(SM_CYSCREEN);
        RECT rr = { 0, 0, (int)bw, (int)bh };
        AdjustWindowRectEx(&rr, want_st, FALSE, want_ex);
        int rw = rr.right - rr.left, rh = rr.bottom - rr.top;
        int rx = (sx - rw) / 2, ry = (sy - rh) / 2;
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        SetWindowPos(h, HWND_NOTOPMOST, rx, ry, rw, rh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        logf("[d3d] fit_window: native-windowed title -> window sized to its backbuffer "
             "%ux%u exactly (1x), so its cursor tracks the pointer; the remembered size "
             "is ignored for this title (it desyncs FE's cursor)", bw, bh);
        return;
    }

    // Open at a WHOLE-NUMBER multiple of the backbuffer. A 640x480 client on a
    // modern desktop is a postage stamp, so the window got maximised by hand --
    // which stretches a 4:3 image across a 16:9 client and quietly distorts
    // everything in it. An integer multiple avoids BOTH: the picture is big and
    // it is the right shape, and each source pixel maps to a whole square block
    // instead of an uneven smear.
    //   [dx] d3d_scale: 0 = auto (largest multiple that fits the desktop with a
    //   margin for the taskbar and frame), N = exactly Nx, so 1 restores the
    //   original native-size behaviour.
    int scrw0 = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CXSCREEN)
                                      : GetSystemMetrics(SM_CXSCREEN);
    int scrh0 = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CYSCREEN)
                                      : GetSystemMetrics(SM_CYSCREEN);
    // What fits: the work area minus this window's own frame. Every branch below
    // is clamped to this, because a size that does not fit is not a preference,
    // it is a window with its bottom edge under the taskbar.
    RECT wa;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = wa.top = 0; wa.right = scrw0; wa.bottom = scrh0;
    }
    RECT frame0 = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&frame0, want_st, FALSE, want_ex);
    int availw = (wa.right - wa.left) - (frame0.right - frame0.left);
    int availh = (wa.bottom - wa.top) - (frame0.bottom - frame0.top);
    if (availw < (int)bw) availw = (int)bw;      // never below 1x
    if (availh < (int)bh) availh = (int)bh;

    // A REMEMBERED size outranks any scale factor: the user resized the window by
    // hand, which is a more direct statement of intent than a number in an ini.
    // Still clamped -- a size remembered on a 1920x1080 desktop must not be
    // restored verbatim onto a laptop panel.
    if (g_d3d_remember && g_saved_cw > 0 && g_saved_ch > 0) {
        int cw = g_saved_cw, ch = g_saved_ch;
        if (cw > availw) cw = availw;
        if (ch > availh) ch = availh;
        // FLOOR at the backbuffer. A remembered size SMALLER than the game's native
        // resolution just downscales it into a tiny, soft window -- measured on Front
        // Mission Online 2026-08-18: a stale 588x441 remembered against an 800x600
        // backbuffer, so the whole game rendered at ~0.74x in a postage-stamp window.
        // The game's own pixels are the smallest useful window; never restore below 1x.
        if (cw < (int)bw || ch < (int)bh) {
            logf("[d3d] fit_window: remembered client %dx%d is smaller than the "
                 "backbuffer %ux%u -- flooring to 1x, or the game would be downscaled "
                 "into a tiny window", cw, ch, bw, bh);
            cw = (int)bw; ch = (int)bh;
        }
        const bool clamped = (cw != g_saved_cw || ch != g_saved_ch);
        // ASPECT LOCK, part 3 of 3: the REMEMBERED size.
        //
        // Parts 1 and 2 (spy_proc: WM_SIZING, WM_GETMINMAXINFO) only cover sizes
        // the user produces through the window manager's own resize paths. A
        // remembered size arrives from the ini, so it went through neither -- and
        // a client rect that is not the backbuffer's shape is stretched by D3D
        // exactly as hard here as anywhere else.
        //
        // MEASURED on Front Mission Online, 2026-08-17: the ini held
        // d3d_window_w/h = 1324x813 (1.63) against an 800x600 backbuffer (1.33),
        // and the log line below reported restoring it verbatim -- 1.22x too
        // wide, which is the "the Please Wait spinner is an ellipse" report to
        // the pixel. Aero Snap and a DPI change both produce such a size without
        // ever sending WM_SIZING, so plugging the individual routes in is not the
        // fix; refusing to RESTORE a wrong shape is.
        //
        // Shrink, never grow: the largest correctly-shaped client that fits
        // INSIDE what was remembered. The size the user chose is honoured as far
        // as the shape allows, and the result cannot overflow the screen the
        // clamp above just fitted it to.
        if (g_d3d_aspect && bw && bh && cw * (int)bh != ch * (int)bw) {
            int was_w = cw, was_h = ch;
            int fit_w = MulDiv(ch, (int)bw, (int)bh);   // width implied by the height
            if (fit_w <= cw) cw = fit_w;
            else             ch = MulDiv(cw, (int)bh, (int)bw);
            logf("[d3d] fit_window: the remembered client %dx%d is not the "
                 "backbuffer's shape (%ux%u) -- restoring %dx%d instead, or D3D "
                 "would stretch every sprite by %d%% horizontally "
                 "(d3d_aspect=0 to restore it verbatim)",
                 was_w, was_h, bw, bh, cw, ch,
                 MulDiv(was_w * (int)bh, 100, was_h * (int)bw) - 100);
        }
        RECT rr = { 0, 0, cw, ch };
        AdjustWindowRectEx(&rr, want_st, FALSE, want_ex);
        int rw = rr.right - rr.left, rh = rr.bottom - rr.top;
        int rx = (scrw0 - rw) / 2, ry = (scrh0 - rh) / 2;
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        SetWindowPos(h, HWND_NOTOPMOST, rx, ry, rw, rh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        logf("[d3d] fit_window: RESTORED the remembered client %dx%d%s "
             "(backbuffer %ux%u, %dx%d available)",
             cw, ch, clamped ? " (clamped to this screen)" : "",
             bw, bh, availw, availh);
        return;
    }

    int scale = g_d3d_scale;
    if (scale > 0) {
        // An EXPLICIT scale is a request, not a guarantee. d3d_scale=2 on a panel
        // that cannot hold 2x used to be applied anyway, leaving the window bigger
        // than the screen -- and since Reset re-runs this, resizing it by hand did
        // not stick either. Clamp down to what fits; never up.
        int fit = scale;
        while (fit > 1 && ((int)(bw * fit) > availw || (int)(bh * fit) > availh))
            fit--;
        if (fit != scale)
            logf("[d3d] d3d_scale=%d does not fit this screen -- using %dx "
                 "(%ux%u client; %dx%d available)",
                 scale, fit, bw * fit, bh * fit, availw, availh);
        scale = fit;
    }
    if (scale <= 0) {
        // Measure against the WORK AREA minus the actual frame, not a percentage
        // of the screen. A percentage guess picked 1x on 1920x1080 -- 2x is
        // 1280x960, which really does fit under a taskbar once you subtract the
        // caption properly, and 1x on a 1080p screen is the postage stamp this
        // setting exists to avoid.
        scale = 1;
        while (bw * (scale + 1) <= (UINT)availw && bh * (scale + 1) <= (UINT)availh)
            scale++;
        logf("[d3d] d3d_scale=auto -> %dx (%ux%u client; %dx%d fits inside the "
             "%dx%d work area after the frame)",
             scale, bw * scale, bh * scale, availw, availh,
             (int)(wa.right - wa.left), (int)(wa.bottom - wa.top));
    }
    if (scale > 16) scale = 16;

    RECT r;
    r.left = 0; r.top = 0;
    r.right = (LONG)(bw * scale); r.bottom = (LONG)(bh * scale);
    AdjustWindowRectEx(&r, want_st, FALSE, want_ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    // The REAL metrics: GetSystemMetrics is one of our own hooks now, and it
    // reports the backbuffer as the screen. Centring against that would put the
    // window at (0,0) on a 640x480 "screen" that does not exist.
    int scrw = scrw0, scrh = scrh0;
    int sx = (scrw - ww) / 2;
    int sy = (scrh - wh) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    SetWindowPos(h, HWND_NOTOPMOST, sx, sy, ww, wh,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    logf("[d3d] fit_window: style %08lX -> %08lX, exstyle %08lX -> %08lX, "
         "client now %ux%u (%dx backbuffer %ux%u) at (%d,%d)",
         st, want_st, ex, want_ex, bw * scale, bh * scale, scale, bw, bh, sx, sy);
}

// Record the size the user just chose, so the next launch opens at it instead of
// at whatever d3d_scale says.
//
// Stored as the CLIENT size, not the window size: the frame differs between
// styles and DPI settings, so a window rect remembered under one would restore
// wrong under the other. The client is the thing the player was actually looking
// at, and it is what fit_window re-derives a window rect from.
//
// Writes straight to the ini. That is the same channel dinputhook uses for the
// gain and the found cursor address, and it works for the same reason: pol.exe
// runs with write access to its own install directory even though the user's
// shell does not.
static void remember_window_size(HWND h)
{
    if (!g_d3d_remember || !h || !IsWindow(h)) return;
    if (IsIconic(h)) return;                 // a minimised window has no useful size
    RECT c;
    if (!GetClientRect(h, &c)) return;
    int cw = c.right - c.left, ch = c.bottom - c.top;
    if (cw < 64 || ch < 64) return;          // nonsense; do not persist it

    InterlockedExchange(&g_user_sized, 1);   // and stop Reset re-imposing a size
    if (cw == g_saved_cw && ch == g_saved_ch) return;
    g_saved_cw = cw; g_saved_ch = ch;

    if (!g_dx_ini[0]) return;
    wchar_t v[32];
    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%d", cw);
    WritePrivateProfileStringW(L"dx", L"d3d_window_w", v, g_dx_ini);
    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%d", ch);
    WritePrivateProfileStringW(L"dx", L"d3d_window_h", v, g_dx_ini);
    // The rev marker says "a USER chose this size, under the user-sized-only regime".
    // A d3d_window_w/h without it is treated as stale at startup and discarded --
    // that is the whole defence against pre-rework sizes pinning a window small.
    WritePrivateProfileStringW(L"dx", L"d3d_window_rev", L"1", g_dx_ini);
    logf("[d3d] remembered the window: client %dx%d (saved to the ini; "
         "clear d3d_window_w/h to go back to d3d_scale)", cw, ch);
}

// Give the game window the Viewer's own icon (title bar + taskbar), so a windowed
// title shows the PlayOnline icon instead of the generic default. Pulled from
// pol.exe (this process's main module) via ExtractIcon, so it is exactly the
// app's icon with no resource-id guessing.
static void set_game_icon(HWND h)
{
    if (!h || !IsWindow(h)) return;
    char path[MAX_PATH] = "";
    HICON ico = NULL;
    const char* src = NULL;

    // Preferred: <title dir>\polboot.exe. Each POL title's boot exe carries that
    // title's OWN icon -- the exact one its desktop shortcut uses (TetraMaster\
    // polboot.exe = the Tetra Master logo). TM.dll itself has no icon resource.
    if (g_title_module && GetModuleFileNameA(g_title_module, path, MAX_PATH)) {
        char* slash = strrchr(path, '\\');
        if (slash) {
            size_t room = MAX_PATH - (size_t)(slash + 1 - path);
            strcpy_s(slash + 1, room, "polboot.exe");
            ico = ExtractIconA(GetModuleHandleA(NULL), path, 0);
            if (ico && ico != (HICON)1) src = "title polboot.exe";
            else ico = NULL;
        }
    }
    // Fallback: the title module itself, then pol.exe's PlayOnline icon.
    if (!ico && g_title_module && GetModuleFileNameA(g_title_module, path, MAX_PATH)) {
        ico = ExtractIconA(g_title_module, path, 0);
        if (ico && ico != (HICON)1) src = "title module";
        else ico = NULL;
    }
    if (!ico && GetModuleFileNameA(NULL, path, MAX_PATH)) {
        ico = ExtractIconA(GetModuleHandleA(NULL), path, 0);
        if (ico && ico != (HICON)1) src = "pol.exe (fallback)";
        else ico = NULL;
    }
    if (!ico) return;
    SendMessageA(h, WM_SETICON, ICON_BIG,   (LPARAM)ico);
    SendMessageA(h, WM_SETICON, ICON_SMALL, (LPARAM)ico);
    logf("[d3d] set game window icon from %s (%s)", path, src);
}

// Get the Viewer's full-screen black MASK window out of the way.
//
// pol.exe registers a mask window whose class is region-suffixed -- all three
// literals `PlayOnlineMask`, `PlayOnlineMaskEU`, `PlayOnlineMaskUS` are UTF-16
// strings in pol.exe, and its HWND is what polcore's SetMaskWindowHandle
// (IPOLCoreCom slot 22) receives. It exists to cover the desktop during
// transitions, and while a game held an EXCLUSIVE fullscreen device it never
// mattered: the device covered everything. Windowed, the mask is simply a
// screen-sized black window sitting on top of the game -- which is a black
// screen that still shows the game correctly in the taskbar thumbnail, and
// swallows every click. Measured on Tetra Master, 2026-08-12.
//
// Only ever touches windows owned by THIS process.
//
// WARNING: IF A GAME RUNS WITH NO WINDOW AND NO TASKBAR BUTTON, IT IS NOT THIS.
// The natural suspicion is that mask handling got greedy and hid the game too.
// It cannot: the three class names above are matched EXACTLY, mode 2 (default)
// ALIGNS the mask rather than hiding it, and the WS_EX_TOOLWINDOW /
// ~WS_EX_APPWINDOW restyle below is applied only to a window that matched.
//
// The real mechanism, measured 2026-08-15: **Tetra Master's window is born
// hidden.** At device creation the log reads
//     focus window BEFORE class="Tetra Master" style=84000000 visible=0
// -- WS_POPUP|WS_CLIPSIBLINGS, not visible -- and it is fit_window() that turns
// it into 14CF0000 / visible=1 at (512,265). So the window only ever becomes
// visible as a SIDE EFFECT of the d3d device being created. A launch that
// stalls BEFORE CreateDevice leaves a live process with a window that was never
// shown: nothing on screen, nothing in the taskbar.
//
// Diagnose it in one grep, do not theorise: count Direct3DCreate8/CreateDevice
// in the run's polshim.<pid>.log (INSTALL tree, not build/ -- see the ini note).
// A run that showed a window had 3; the invisible run had 0 and its log stopped
// right after `[titletag]`, i.e. the game never reached graphics init at all.
//
// Run the mask work OFF the game's thread.
//
// Anything that touches another thread's window can block, and blocking inside
// CreateDevice means the game never gets a device. Belt and braces alongside
// SWP_ASYNCWINDOWPOS: even if some call still stalls, it stalls a throwaway
// thread and the game carries on.
static int tame_mask_window();

// Held for the WORKER's whole lifetime (cleared at the end of mask_thread), so
// schedule_mask_align never spawns a second mask_thread while one is still running.
// It used to reset right after CreateThread, which excluded only the microseconds of
// thread creation -- so a title that Resets often ran many concurrent tame_mask_window
// passes, each calling install_msgspy and racing g_spy with no lock.
static LONG g_mask_align_running = 0;

static DWORD WINAPI mask_thread(LPVOID)
{
    tame_mask_window();          // the WORKER, not the scheduler
    InterlockedExchange(&g_mask_align_running, 0);   // release only now the work is done
    return 0;
}

// --- delayed window audit ---------------------------------------------------
//
// Everything we log at CreateDevice is a snapshot of the moment we ourselves
// acted. It cannot show a style being re-applied afterwards, which is one of the
// two remaining explanations for "no X, cannot drag" (the other being that the
// window on screen is not the one we restyled -- the aligned, alpha-0 mask sits
// at exactly the same rect by construction, so the two are indistinguishable by
// eye). Static RE ruled out the obvious causes: app.dll never MOVES a window
// (its one SetWindowPos is NOMOVE|NOSIZE|FRAMECHANGED), polcore imports no
// windowing API, and pol.exe's wndproc 0x0041BEC4 is a bare jmp to
// DefWindowProcA -- so nothing is swallowing the frame messages.
//
// So: look again LATER, and report both windows plus who is actually on top.
static void audit_window(const char* when, HWND h, const char* tag)
{
    if (!h || !IsWindow(h)) { logf("[aud] %-4s %s: window gone", when, tag); return; }
    RECT w, c;
    ZeroMemory(&w, sizeof(w)); ZeroMemory(&c, sizeof(c));
    GetWindowRect(h, &w);
    GetClientRect(h, &c);
    LONG st = GetWindowLongA(h, GWL_STYLE), ex = GetWindowLongA(h, GWL_EXSTYLE);
    char f[128] = "";
    if (st & WS_CAPTION)   strncat_s(f, sizeof(f), "CAPTION ", _TRUNCATE);
    if (st & WS_SYSMENU)   strncat_s(f, sizeof(f), "SYSMENU ", _TRUNCATE);   // the X
    if (st & WS_THICKFRAME)strncat_s(f, sizeof(f), "SIZEBOX ", _TRUNCATE);
    // MIN/MAXIMIZEBOX and DISABLED were missing, and they are the flags that say
    // WHY a frame went inert: fit_window sets the whole WS_OVERLAPPEDWINDOW group,
    // so a later style of CAPTION|SYSMENU alone means someone stripped the other
    // three, and WS_DISABLED means the window is refusing input outright. Without
    // these the audit could not tell either case from a healthy window.
    if (st & WS_MINIMIZEBOX)strncat_s(f, sizeof(f), "MINBOX ",  _TRUNCATE);
    if (st & WS_MAXIMIZEBOX)strncat_s(f, sizeof(f), "MAXBOX ",  _TRUNCATE);
    if (st & WS_DISABLED)  strncat_s(f, sizeof(f), "DISABLED ",_TRUNCATE);
    if (st & WS_POPUP)     strncat_s(f, sizeof(f), "POPUP ",   _TRUNCATE);
    if (st & WS_VISIBLE)   strncat_s(f, sizeof(f), "VISIBLE ", _TRUNCATE);
    if (ex & WS_EX_LAYERED)strncat_s(f, sizeof(f), "LAYERED ", _TRUNCATE);
    if (ex & WS_EX_TRANSPARENT)strncat_s(f, sizeof(f), "TRANSPARENT ", _TRUNCATE);
    if (ex & WS_EX_APPWINDOW)strncat_s(f, sizeof(f), "APPWINDOW ", _TRUNCATE);
    if (ex & WS_EX_TOPMOST)strncat_s(f, sizeof(f), "TOPMOST ", _TRUNCATE);
    logf("[aud] %-4s %-4s hwnd=%p style=%08lX ex=%08lX [%s] win=(%ld,%ld %ldx%ld) client=%ldx%ld",
         when, tag, h, st, ex, f, w.left, w.top, w.right - w.left, w.bottom - w.top,
         c.right, c.bottom);

    // The GUI-thread state, which is what actually decides whether the frame can
    // be grabbed. GetCapture() is useless here -- it reports only the CALLING
    // thread's capture, and this audit runs on its own thread, so it would say
    // "none" no matter what. GetGUIThreadInfo asks about the window's own thread.
    //
    // hwndCapture non-NULL with no button down is the measured stuck state
    // (2026-08-24): a captured window receives no non-client hit-testing, so its
    // caption stops being draggable while its client area still works.
    GUITHREADINFO gt;
    ZeroMemory(&gt, sizeof(gt));
    gt.cbSize = sizeof(gt);
    DWORD wtid = GetWindowThreadProcessId(h, NULL);
    if (wtid && GetGUIThreadInfo(wtid, &gt)) {
        logf("[aud] %-4s   tid=%lu flags=%08X%s active=%p focus=%p capture=%p%s "
             "movesize=%p", when, wtid, (unsigned)gt.flags,
             (gt.flags & 0x00000002) ? " INMOVESIZE" : "",
             gt.hwndActive, gt.hwndFocus, gt.hwndCapture,
             gt.hwndCapture ? "  <-- CAPTURED: no non-client hit-testing, the "
                              "frame cannot be dragged or sized" : "",
             gt.hwndMoveSize);
    }
}

// Log every CHILD of the game window, with its rect.
//
// Added after the FMO run of 2026-08-13, where the audit said the window at the
// game's centre was class=VideoRenderer -- DirectShow's renderer, parked over the
// game -- and the log had no way to say how big it was or where. "Something else
// is on top" is only half a diagnosis; the rect is the other half, and eyeballing
// it off a screenshot is not a measurement.
struct ChildAudit { const char* when; int n; };

static BOOL CALLBACK audit_child(HWND h, LPARAM lp)
{
    ChildAudit* ca = (ChildAudit*)lp;
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    RECT w; ZeroMemory(&w, sizeof(w));
    GetWindowRect(h, &w);
    // Screen rect AND the game-client-relative rect: the second is what decides
    // whether this thing covers the rendered image, and is the awkward one to
    // work out by hand.
    POINT o = { w.left, w.top };
    if (g_game_window && IsWindow(g_game_window)) ScreenToClient(g_game_window, &o);
    logf("[aud] %-4s   child #%d %p class=%-20s screen=(%ld,%ld %ldx%ld) "
         "client=(%ld,%ld) visible=%d",
         ca->when, ++ca->n, h, cls, w.left, w.top,
         w.right - w.left, w.bottom - w.top, o.x, o.y,
         IsWindowVisible(h) ? 1 : 0);
    return TRUE;
}

static DWORD WINAPI audit_thread(LPVOID)
{
    static const int WAITS[] = { 3000, 9000 };
    for (int i = 0; i < _countof(WAITS); i++) {
        Sleep(WAITS[i]);
        char when[16];
        _snprintf_s(when, sizeof(when), _TRUNCATE, "+%ds", WAITS[i] / 1000);
        audit_window(when, g_game_window, "GAME");
        if (g_game_window && IsWindow(g_game_window)) {
            ChildAudit ca; ca.when = when; ca.n = 0;
            EnumChildWindows(g_game_window, audit_child, (LPARAM)&ca);
            if (!ca.n) logf("[aud] %-4s   (game window has no child windows)", when);
        }
        // the mask, found the same way tame_mask_window does
        DWORD me = GetCurrentProcessId();
        static const char* CLS[] = { "PlayOnlineMaskUS", "PlayOnlineMaskEU", "PlayOnlineMask" };
        for (int k = 0; k < _countof(CLS); k++) {
            HWND m = NULL;
            while ((m = FindWindowExA(NULL, m, CLS[k], NULL)) != NULL) {
                DWORD o = 0; GetWindowThreadProcessId(m, &o);
                if (o == me) audit_window(when, m, "MASK");
            }
        }
        // and who is genuinely in front at the game window's centre
        if (g_game_window && IsWindow(g_game_window)) {
            RECT r; GetWindowRect(g_game_window, &r);
            POINT mid; mid.x = (r.left + r.right) / 2; mid.y = (r.top + r.bottom) / 2;
            HWND top = WindowFromPoint(mid);
            char cls[64] = "";
            if (top) GetClassNameA(top, cls, sizeof(cls));
            logf("[aud] %-4s window at the game's centre point = %p class=%s%s",
                 when, top, cls,
                 (top == g_game_window) ? "  (the game -- good)" : "  <-- NOT the game window");
            // If it is NOT the game, say exactly what it is and how it relates to
            // the game window. An intruder that is a CHILD of the game is a very
            // different problem from an unrelated top-level window on top of it:
            // the first is the title's own doing, the second is ours or the
            // Viewer's. Walk the parent chain rather than assuming either.
            if (top && top != g_game_window) {
                RECT tr; ZeroMemory(&tr, sizeof(tr));
                GetWindowRect(top, &tr);
                POINT o = { tr.left, tr.top };
                ScreenToClient(g_game_window, &o);
                logf("[aud] %-4s   intruder screen=(%ld,%ld %ldx%ld) "
                     "client=(%ld,%ld) style=%08lX ex=%08lX",
                     when, tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top,
                     o.x, o.y, GetWindowLongA(top, GWL_STYLE),
                     GetWindowLongA(top, GWL_EXSTYLE));
                HWND p = top;
                for (int depth = 0; depth < 6; depth++) {
                    HWND parent = GetAncestor(p, GA_PARENT);
                    if (!parent || parent == GetDesktopWindow()) {
                        logf("[aud] %-4s   parent chain: %p is TOP-LEVEL "
                             "(not a child of the game)", when, p);
                        break;
                    }
                    char pcls[64] = "";
                    GetClassNameA(parent, pcls, sizeof(pcls));
                    logf("[aud] %-4s   parent chain: %p -> %p class=%s%s",
                         when, p, parent, pcls,
                         (parent == g_game_window) ? "  (the GAME window)" : "");
                    if (parent == g_game_window) break;
                    p = parent;
                }
            }
        }
    }
    return 0;
}

// Re-arm PER GAME WINDOW, not once per process.
//
// The old latch was a plain once-per-process flag, which is wrong for the way the
// Viewer is actually used: it is a shell that launches title after title in ONE
// process. In the FMO session of 2026-08-13 the user launched Fantasy Earth
// first, so the audit spent its two samples on FE's window and had already
// latched by the time FMO started -- the run produced no audit of the title we
// were actually debugging. Keying the latch on the window means each new title
// gets its own audit, and a re-created device for the SAME window still does not
// spam a second one.
static void schedule_audit()
{
    static HWND audited = NULL;
    HWND now = g_game_window;
    if (!now) return;
    if ((HWND)InterlockedExchangePointer((void**)&audited, now) == now) return;
    HANDLE t = CreateThread(NULL, 0, audit_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void schedule_mask_align()
{
    // Latch for the worker's whole lifetime; mask_thread clears g_mask_align_running
    // when it finishes. Only reset here if the thread never started, so a failed
    // CreateThread does not leave the latch stuck on.
    if (InterlockedCompareExchange(&g_mask_align_running, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, mask_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_mask_align_running, 0);
}

// Share this whole windowed-mode support layer with the Direct3D 9 path.
//
// Everything below `g_game_window` -- mask alignment, cursor translation, the
// focus-stealing guard -- is API-AGNOSTIC: it works on HWNDs and user32, and
// nothing in it is specific to Direct3D 8. It was simply written here first,
// keyed off a window that only the d3d8 CreateDevice path ever set. So Front
// Mission Online, the one d3d9 title, ran windowed with none of it: the Viewer's
// full-screen mask stayed over the game ("PlayOnlineMask took over my screen")
// and, because polcore's CreateInput is bound to that mask, the game window took
// no input either ("I could hear FMO but the window was not responding"), which
// is exactly the pair of symptoms Tetra Master had before this code existed.
//
// One call from d3d9hook wires FMO into all of it. Sharing the single
// g_game_window is correct rather than merely convenient -- only one title runs
// at a time, and the cursor/focus hooks must agree with the mask about which
// window is the game.
// THE WHITE BOX WHILE A GAME LOADS -- and why it was still there.
//
// FMO registers FMOClass with hbrBackground = (HBRUSH)6 = COLOR_WINDOW+1
// (RegisterClassA at FrontMissionOnline.dll+0xAEEC, field written at +0xAEDC), so
// every erase before the first Present -- the whole loading phase -- paints
// system-window WHITE. Retail ran fullscreen exclusive, where a class brush never
// shows; forcing it windowed exposed it. Swapping the CLASS brush to black once
// matches what the original looked like, and holds for every later erase (device
// reset, resize) because it is the class's, not the window's.
//
// IMPORTANT: THIS LIVED INSIDE d3d_set_game_window, WHICH FMO'S LAUNCH NEVER CALLS
// (2026-09-08, reported with a screenshot: white window, black box top-left). The d3d9
// CreateDevice path calls d3d_after_device_created(); d3d_set_game_window is only
// reached from a RESET and from the sleep/resume recovery. So the fix written for
// FMO in August ran on every path except the one FMO takes to get its first
// window, and the log said so by having no "class background brush" line at all.
// Its own function now, called from both.
//
// Not for a native-windowed title (Fantasy Earth owns its window's look), and
// repairable with [dx] d3d_blackbg=0.
static void apply_black_class_brush(HWND h)
{
    if (!h || !IsWindow(h)) return;
    if (!g_d3d_blackbg) return;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HBRUSH old = (HBRUSH)SetClassLongPtrA(h, GCLP_HBRBACKGROUND, (LONG_PTR)black);

    // KEY: PAINT IT BLACK NOW, DO NOT ASK THE GAME TO. Swapping the class brush and
    // invalidating only queues a WM_ERASEBKGND, and FMO's loading phase does not
    // PUMP -- it loads synchronously with its message loop blocked, which is why
    // the box sat there and why a click cleared it (the click is what let the loop
    // run). So the brush swap alone changed nothing on screen, measured live
    // 2026-09-08: the log had "class background brush -> BLACK (was 00000006)" and
    // the user still saw white.
    //
    // Filling the client through a DC is not a request: it draws immediately, and
    // it needs nothing from the owning thread's message queue. Here that thread is
    // the caller (a title creates its device on its own thread), so this lands
    // before the game ever gets back to loading. GetDC/FillRect on another thread's
    // window is legal and does not touch the queue either, which keeps this safe on
    // the Reset path where we may not be the game's thread.
    RECT rc;
    if (GetClientRect(h, &rc) && rc.right > 0 && rc.bottom > 0) {
        HDC dc = GetDC(h);
        if (dc) { FillRect(dc, &rc, black); ReleaseDC(h, dc); }
    }
    InvalidateRect(h, NULL, TRUE);
    if (old == black) return;                     // already ours; say nothing more
    logf("[d3d] class background brush -> BLACK for the game window (was %p; FMO "
         "ships COLOR_WINDOW+1, the white loading box) and the client painted black "
         "now, because the title does not pump while it loads -- [dx] d3d_blackbg=0 "
         "to restore", (void*)old);
}

// ---- THE WHITE FLASH, KILLED AT THE SOURCE -----------------------------------
//
// Painting the client black (above) removes the white box but not the FLASH before
// it: the title registers its class with a white background, creates the window and
// SHOWS it, and only then creates its device -- so there is a window of time, the
// length of everything between, in which Windows erases it white and we have not
// been called yet. Reported 2026-09-08: "the white square bit flashed for a brief
// second before it went black".
//
// The only way to have no white at all is for the class never to be white. FMO's
// RegisterClassA (FrontMissionOnline.dll+0xAEEC) passes hbrBackground =
// (HBRUSH)(COLOR_WINDOW+1); rewriting that ONE value as the call goes past means the
// first erase Windows ever performs is already black.
//
// DELIBERATELY NARROW, three ways: only a class registered BY A PROFILED TITLE (the
// IAT patch reaches pol.exe and app.dll too, and the Viewer's own windows are not
// ours to restyle), only the exact system-colour brush FMO uses, and only while
// [dx] d3d_blackbg is on. A class asking for anything else -- a real brush, NULL
// (no erase at all), any other system colour -- is passed through untouched.
static HBRUSH blackbg_swap(HBRUSH want, const char* what)
{
    if (!g_d3d_blackbg) return want;
    if (want != (HBRUSH)(UINT_PTR)(COLOR_WINDOW + 1)) return want;
    static volatile LONG said = 0;
    if (InterlockedCompareExchange(&said, 1, 0) == 0)
        logf("[d3d] %s asked for the white system background -- registering it BLACK "
             "instead, so the game's window is never painted white even once "
             "([dx] d3d_blackbg=0 to restore)", what);
    return (HBRUSH)GetStockObject(BLACK_BRUSH);
}

typedef ATOM (WINAPI* PFN_RegisterClassA)(const WNDCLASSA*);
typedef ATOM (WINAPI* PFN_RegisterClassExA)(const WNDCLASSEXA*);
static PFN_RegisterClassA   real_RegisterClassA   = NULL;
static PFN_RegisterClassExA real_RegisterClassExA = NULL;

static ATOM WINAPI hook_RegisterClassA(const WNDCLASSA* wc)
{
    if (wc && profile_for_addr(_ReturnAddress())) {
        WNDCLASSA c = *wc;
        c.hbrBackground = blackbg_swap(c.hbrBackground, "the game's window class");
        if (c.hbrBackground != wc->hbrBackground) return real_RegisterClassA(&c);
    }
    return real_RegisterClassA(wc);
}

static ATOM WINAPI hook_RegisterClassExA(const WNDCLASSEXA* wc)
{
    if (wc && profile_for_addr(_ReturnAddress())) {
        WNDCLASSEXA c = *wc;
        c.hbrBackground = blackbg_swap(c.hbrBackground, "the game's window class");
        if (c.hbrBackground != wc->hbrBackground) return real_RegisterClassExA(&c);
    }
    return real_RegisterClassExA(wc);
}

void* d3d8_real_RegisterClassA()
{
    if (!real_RegisterClassA) {
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u) real_RegisterClassA = (PFN_RegisterClassA)GetProcAddress(u, "RegisterClassA");
    }
    return (void*)real_RegisterClassA;
}
void* d3d8_hook_RegisterClassA() { return (void*)hook_RegisterClassA; }
void* d3d8_real_RegisterClassExA()
{
    if (!real_RegisterClassExA) {
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u) real_RegisterClassExA = (PFN_RegisterClassExA)GetProcAddress(u, "RegisterClassExA");
    }
    return (void*)real_RegisterClassExA;
}
void* d3d8_hook_RegisterClassExA() { return (void*)hook_RegisterClassExA; }

void d3d_set_game_window(HWND h)
{
    // Keep the sleep/resume recovery's window pointer with the live one -- the
    // nudge is worth nothing aimed at a window the title has stopped using.
    wake_note_window(h);
    if (!h || !IsWindow(h)) return;
    g_game_window = h;
    if (!g_game_born_tick) g_game_born_tick = GetTickCount();
    logf("[d3d] game window set to %p by the d3d9 path -- mask alignment, cursor "
         "translation and the focus guard now apply to it", (void*)h);
    // KEY: THE WHITE BOX AT STARTUP (FMO, measured 2026-08-26 on two machines) is the
    // game window's own class brush: FMO registers FMOClass with hbrBackground =
    // (HBRUSH)6 = COLOR_WINDOW+1 (RegisterClassA at FrontMissionOnline.dll+0xAEEC,
    // field written at +0xAEDC), so every erase before the first Present -- the
    // whole loading phase -- paints system-window WHITE. Retail ran fullscreen
    // exclusive, where the class brush never shows; forced windowed exposed it.
    // Match the original's look: swap the CLASS brush to black once, when the
    // game window is learned. Class-wide is right -- only the game registers this
    // class -- and it holds for every later erase (device reset, resize) too.
    // Not for a native-windowed title (it owns its window's look), and repairable
    // via [dx] d3d_blackbg=0.
    apply_black_class_brush(h);
    if (g_d3d_unmask) schedule_mask_align();
}

static int tame_mask_window()
{
    static const char* CLASSES[] = {
        "PlayOnlineMaskUS", "PlayOnlineMaskEU", "PlayOnlineMask"
    };
    DWORD me = GetCurrentProcessId();
    int hidden = 0;
    for (int i = 0; i < _countof(CLASSES); i++) {
        HWND h = NULL;
        while ((h = FindWindowExA(NULL, h, CLASSES[i], NULL)) != NULL) {
            DWORD owner = 0;
            GetWindowThreadProcessId(h, &owner);
            if (owner != me) continue;
            BOOL vis = IsWindowVisible(h);
            RECT r;
            ZeroMemory(&r, sizeof(r));
            GetWindowRect(h, &r);
            logf("[d3d] mask window %p class=%s (%ld,%ld %ldx%ld) visible=%d",
                 h, CLASSES[i], r.left, r.top, r.right - r.left, r.bottom - r.top,
                 vis ? 1 : 0);
            SetWindowLongA(h, GWL_EXSTYLE, GetWindowLongA(h, GWL_EXSTYLE) & ~WS_EX_TOPMOST);

            // MODE 2 (default): ALIGN the mask to the game window instead of
            // hiding it.
            //
            // Measured across four sessions: IPOLCoreCom::CreateInput(HWND) --
            // the Viewer's input system -- is handed the MASK window's handle,
            // not the game's. The mask is full-screen, so every coordinate the
            // input layer produces is effectively a SCREEN coordinate, and the
            // game consumes it as a coordinate in its 640x480 space. That is
            // both the offset and the ~1366/640 = 2.13x over-sensitivity, and
            // it is why the pointer "snaps" as it clamps at the edges.
            //
            // Hiding the mask cannot fix that; making its client rect identical
            // to the game's can, because then the two coordinate spaces are the
            // same space. It is parked directly BEHIND the game window, so it is
            // completely occluded and no longer a black full-screen cover.
            if (g_d3d_unmask >= 2 && g_game_window && IsWindow(g_game_window)) {
                RECT c;
                POINT o;
                GetClientRect(g_game_window, &c);
                o.x = 0; o.y = 0;
                ClientToScreen(g_game_window, &o);
                // ASYNCWINDOWPOS *posts* instead of sending. The mask belongs to
                // the Viewer's UI thread and we are on the game's thread inside
                // CreateDevice; a synchronous cross-thread move/resize deadlocks
                // there, and the game then never gets its device at all. That is
                // exactly what happened on the first attempt -- the mask was on
                // screen and the game had never rendered.
                SetWindowPos(h, g_game_window, o.x, o.y,
                             c.right - c.left, c.bottom - c.top,
                             SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                // Make it INVISIBLE but still present.
                //
                // Measured: after aligning, the mask is what receives the mouse
                // (screen 756,557 -> lParam 436,426 against a client origin of
                // 320,131 -- exact), which is consistent with polcore's
                // CreateInput being bound to it. So it must keep hit-testing.
                // But it also sits ON TOP of the game, which is the black screen.
                //
                // A layered window at alpha 0 is the one state that satisfies
                // both: it paints nothing, so the game shows through, and it
                // still hit-tests, so the input path is untouched. Hiding it or
                // pushing it below would fix the black screen and take the
                // coordinates away with it.
                // WS_EX_TOOLWINDOW keeps this invisible helper OUT of the taskbar
                // and Alt+Tab, so a player never sees a stray "what is this" window;
                // WS_EX_APPWINDOW (the taskbar-button flag) is cleared for the same
                // reason. Tool-window status does not affect hit-testing, so the
                // input binding above is untouched. FRAMECHANGED makes the shell
                // re-read the style so the taskbar button actually drops.
                // WS_EX_TRANSPARENT makes the mask click-through, so it can never
                // eat a mouse hit meant for the game window in front of it -- e.g.
                // the bottom-right sizing grip, which sits right at the aligned
                // mask's own corner. The old requirement that the mask hit-test
                // (for polcore's cursor mapping) is obsolete now the cursor comes
                // from DirectInput.
                LONG mex = GetWindowLongA(h, GWL_EXSTYLE);
                mex = (mex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT)
                      & ~WS_EX_APPWINDOW;
                SetWindowLongA(h, GWL_EXSTYLE, mex);
                SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);

                install_msgspy(h, "MASK");
                hidden++;
                logf("[d3d]   -> ALIGNED to the game's client rect "
                     "(%ld,%ld %ldx%ld) and parked behind it -- polcore's "
                     "CreateInput is bound to THIS window, so its coordinates "
                     "now share the game's space",
                     o.x, o.y, c.right - c.left, c.bottom - c.top);
                continue;
            }

            if (!vis) continue;
            SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(h, SW_HIDE);
            hidden++;
            logf("[d3d]   -> hidden and pushed to the bottom");
        }
    }
    if (!hidden)
        logf("[d3d] no visible PlayOnlineMask* window found in this process");
    return hidden;
}

// Rewrite fullscreen present params into legal windowed ones.
//
// The two trailing fields are documented as "must be zero for windowed mode".
// The backbuffer format is the subtle one: passing D3DFMT_UNKNOWN and letting
// the runtime inherit the desktop format is a **D3D9** convenience that D3D8
// does not have -- D3D8 answers D3DERR_INVALIDCALL. Measured, not assumed:
// src/d3d8probe.cpp reproduced exactly that against the real d3d8.dll, and
// passing the adapter's current format instead made the same call succeed.
// So windowed mode gets a real format, taken from the adapter.
// `focus` is CreateDevice's hFocusWindow, used when the caller left
// hDeviceWindow NULL -- which a fullscreen-only game does, because fullscreen
// ignores that field. Windowed mode does not: it is the window D3D presents to.
static void make_windowed(D3D8_PRESENT_PARAMETERS* p, DWORD desktop_fmt, HWND focus)
{
    p->Windowed                        = TRUE;
    if (desktop_fmt != D3D8FMT_UNKNOWN)
        p->BackBufferFormat            = desktop_fmt;
    p->FullScreen_RefreshRateInHz      = 0;
    p->FullScreen_PresentationInterval = D3D8PRESENT_INTERVAL_DEFAULT;

    // Give the present target a window. Measured: TM asks with hDeviceWindow=0.
    if (!p->hDeviceWindow && focus)
        p->hDeviceWindow = focus;

    // FLIP is the fullscreen presentation model. Windowed wants DISCARD; leaving
    // FLIP in place is a documented mismatch and presents nothing useful.
    if (p->SwapEffect == D3D8SWAPEFFECT_FLIP)
        p->SwapEffect = D3D8SWAPEFFECT_DISCARD;
    if (p->BackBufferCount == 0)
        p->BackBufferCount = 1;
}

// CreateDevice installs the Reset hook on the first device it sees, so it needs
// the address before Reset itself is defined.
static HRESULT STDMETHODCALLTYPE hook_Reset(void* self, D3D8_PRESENT_PARAMETERS* pp);

// --- device-lost diagnostic (TM zero-draw on the Deck) -----------------------
// TM's frame loop polls TestCooperativeLevel and refuses to draw until it says
// D3D_OK. On the Deck it hangs with zero draws, so log every CHANGE in that
// return (and the first few) to see whether the device sits at DEVICELOST /
// DEVICENOTRESET or flaps -- passively, because a debugger attach perturbs it.
static PFN_DevTestCoop orig_TestCoop = NULL;
static LONG    g_n_tcl    = 0;
static HRESULT g_last_tcl = (HRESULT)0xDEADBEEF;
static HRESULT STDMETHODCALLTYPE hook_TestCoopLevel(void* self);

// --- sleep/resume recovery (wakerecover.cpp) --------------------------------
// The device the title is rendering with, and the EXACT present parameters it
// was last successfully created or reset with. Both exist for one reason: after
// a suspend the device is lost, and a Reset needs the parameters the device
// really has -- not the ones the title asked for, which for every forced-
// windowed title are a fullscreen set we rewrote. Recording the FINAL set is
// what makes an external Reset reproduce the device rather than change it.
static void*                    g_game_device = NULL;
static D3D8_PRESENT_PARAMETERS  g_pp_last;
static bool                     g_pp_have     = false;
static bool                     g_dev_vt_ok   = false;   // the vtable validated
// Defined below hook_Present, which it installs; called from hook_CreateDevice.
static void wake_arm_d3d8(void* dev, HWND wnd);

// --- "what is the screen?" ---------------------------------------------------
//
// MEASURED: the game window receives CORRECT client coordinates -- a mouse at
// screen (613,263) arrives as lParam (293,132) inside a 640x480 client, which is
// exact. So the coordinates reaching the game are right and the game is
// transforming them itself.
//
// The transform has a suspect with exactly the right magnitude. A fullscreen-only
// game asks the device what the screen is; in exclusive fullscreen the answer is
// its OWN mode, 640x480. Windowed, IDirect3DDevice8::GetDisplayMode returns the
// DESKTOP mode instead -- our own device validation logged 1366x768. Scaling
// input by that gives 1366/640 = 2.13x, which is the reported over-sensitivity.
// It also explains why the pointer was wrong in the original fullscreen report:
// if the mode switch never really took, GetDisplayMode answered 1366x768 then too.
//
// So while a windowed override is in force, tell the game the screen is exactly
// its backbuffer -- through the device, and through GetSystemMetrics, which is
// the other common way to ask the same question.
static UINT g_bb_w = 0, g_bb_h = 0;
static PFN_DevGetDisplayMode orig_GetDisplayMode = NULL;
static LONG g_n_modefaked = 0, g_n_metricsfaked = 0;

static HRESULT STDMETHODCALLTYPE hook_GetDisplayMode(void* self, D3D8_DISPLAYMODE* m)
{
    HRESULT hr = orig_GetDisplayMode(self, m);
    if (SUCCEEDED(hr) && m && g_d3d_fakemode && g_bb_w && g_bb_h) {
        UINT ow = m->Width, oh = m->Height;
        m->Width  = g_bb_w;
        m->Height = g_bb_h;
        if (ow != m->Width || oh != m->Height) {
            InterlockedIncrement(&g_n_modefaked);
            if (g_d3d_trace && (g_n_modefaked < 6 || (g_n_modefaked % 600) == 0))
                logf("[mode] device GetDisplayMode %ux%u -> %ux%u "
                     "(the game is told the screen IS its backbuffer)",
                     ow, oh, m->Width, m->Height);
        }
    }
    return hr;
}

// WHO the screen-size substitution is FOR -- and who it must never reach.
//
// It is written for TITLE code: "tell the game the screen is its backbuffer".
// Everything else in the process has to keep seeing the real desktop, and one
// caller in particular MUST: pol.exe's own window procedure.
//
// MEASURED on a pol.exe memory image (image base 0x00400000). pol.exe has
// exactly three GetSystemMetrics call sites, and every one of them is window
// management -- none is cursor mapping:
//
//   0x00409E90 / 0x00409E9C   WM_GETMINMAXINFO -> MINMAXINFO.ptMaxTrackSize
//                             (the slot is loaded once, `mov edi,[0x4412C8]`,
//                              then `call edi` twice -- which is why a scan for
//                              `FF 15` call sites does not find them)
//   0x00409F97 / 0x00409FB2   WM_SIZING -> clamps the drag rect, 4:3
//   0x00419833                index 0x50 = SM_CMONITORS -- not ours
//
// All four of the first kind are inside FUN_00409E40, which is the VIEWER'S
// WINDOW PROCEDURE (it also handles WM_CLOSE, WM_SYSCOMMAND, WM_SETCURSOR,
// WM_MOVING). So answering 640x480 here told the Viewer "your maximum window
// size is 640x480" and Windows clamped the shell window to it -- turning on
// [dx] d3d_windowed visibly SHRANK the Viewer's own window, and the WM_SIZING
// clamp then refused to let it be dragged back out.
//
// The comment that used to stand here read those two WM_SIZING sites as
// "pol.exe's cursor routine scaling the pointer field against the screen" and
// removed the caller gate on that basis. The pointer bug it was chasing was
// real; it was fixed elsewhere and by other means (d3d_freecursor +
// dinput_mouseabs). Nothing is left that wants this lie except the title.
//
// So: substitute ONLY when the caller is the title module itself. That is the
// module CreateDevice's return address landed in -- the same fact
// d3d_windowed_except and the game-window icon already rely on.
static LONG g_n_metrics_shell = 0;

static bool caller_is_title(void* retaddr)
{
    HMODULE title = g_title_module;
    if (!title || !retaddr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(retaddr, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return false;
    return (HMODULE)mbi.AllocationBase == title;
}

// Name the module we refused to lie to, for the first few calls. This is the
// diagnostic that keeps the gate honest: if some title ever DOES need the
// substitution before its own CreateDevice runs, its name shows up here rather
// than the symptom being an unexplained mis-scaled pointer.
static void log_metrics_suppressed(void* retaddr, int index, int v)
{
    LONG n = InterlockedIncrement(&g_n_metrics_shell);
    if (!g_d3d_trace || n > 8) return;
    char path[MAX_PATH] = "";
    unsigned off = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(retaddr, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
        GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH);
        off = (unsigned)((BYTE*)retaddr - (BYTE*)mbi.AllocationBase);
    }
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : (path[0] ? path : "?");
    logf("[mode] GetSystemMetrics(%s) left REAL (%d) for %s+0x%X -- not the "
         "title module, so it keeps the true desktop size",
         index == SM_CXSCREEN ? "SM_CXSCREEN" : "SM_CYSCREEN", v, leaf, off);
}

static int WINAPI hook_GetSystemMetrics(int index)
{
    void* ra = _ReturnAddress();
    int v = real_GetSystemMetrics(index);
    if (!g_d3d_fakemode || !g_bb_w || !g_bb_h) return v;
    if (index != SM_CXSCREEN && index != SM_CYSCREEN) return v;
    if (!caller_is_title(ra)) { log_metrics_suppressed(ra, index, v); return v; }

    int out = (index == SM_CXSCREEN) ? (int)g_bb_w : (int)g_bb_h;
    if (out != v) {
        InterlockedIncrement(&g_n_metricsfaked);
        if (g_d3d_trace && g_n_metricsfaked < 6)
            logf("[mode] GetSystemMetrics(%s) %d -> %d (the title asked)",
                 index == SM_CXSCREEN ? "SM_CXSCREEN" : "SM_CYSCREEN", v, out);
    }
    return out;
}

void* d3d8_real_GetSystemMetrics() { return (void*)real_GetSystemMetrics; }
void* d3d8_hook_GetSystemMetrics() { return (void*)hook_GetSystemMetrics; }

// ---------------------------------------------------------------------------
// shared with the Direct3D 9 path (d3d9hook.cpp)
// ---------------------------------------------------------------------------
//
// d3d9hook.cpp exists because Front Mission Online is the one d3d9 title and
// this file is structurally blind to it. But only the CreateDevice/Reset calls
// themselves are API-specific: the backbuffer bookkeeping, the window restyle,
// the message spy, the icon, the input-HWND repoint, the delayed audit and the
// mask handling are all HWND/user32 work with nothing d3d8 about them. They
// simply live here because d3d8 was written first.
//
// The d3d9 twin re-implemented a thin subset of that and then fell behind every
// improvement made here, which is why FMO ran with NO fakemode (zero [mode]
// lines), NO message spy on the game window (only the mask was subclassed) and
// NO window audit at all (zero [aud] lines) -- the one diagnostic that would say
// which window is actually in front of the game. These entry points let the d3d9
// path run the IDENTICAL sequence instead of a copy of it, so the two cannot
// drift apart again.

int d3d_caller_excepted(void* retaddr)
{
    return caller_excepted(retaddr) ? 1 : 0;
}

// NULL when the caller is not excepted; otherwise the SOURCE of the exception in
// words. The d3d9 path used to print "caller is in d3d_windowed_except" for an
// exception that came from the profile table, which is how a user who set
// "Run games in a window" ended up checking a list FMO was never in.
const char* d3d_caller_except_source(void* retaddr)
{
    ExceptWhy why = EXC_NONE;
    if (!caller_except_why(retaddr, &why)) return NULL;
    return except_source(why);
}

// Everything the d3d8 path does BEFORE calling the real CreateDevice/Reset.
// Order matters: the backbuffer size is recorded first because fit_window sizes
// the client area to it, and hook_GetSystemMetrics starts reporting it as the
// screen from this moment -- exactly as on the d3d8 side. fit_window itself uses
// real_GetSystemMetrics for centring, so it is not misled by our own hook.
//
// `fit` is passed in rather than read from g_d3d_fitwindow because the d3d9 side
// has its own d3d9_fitwindow override of the same setting; the caller has
// already resolved which one applies.
void d3d_prepare_game_window(void* retaddr, HWND game, UINT bw, UINT bh, int fit)
{
    // Is this a NEW title? Everything below -- the icon, the window fit, and
    // (through d3d_after_device_created) the msgspy subclass -- is per-title
    // state, so the boundary has to be declared before any of it is touched.
    // This is the d3d9 path's only boundary: FMO reaches the shim here, not
    // through d3d8's hook_CreateDevice.
    d3d_title_boundary(retaddr, game);

    // Which title called us -- the game window wears that title's own icon.
    if (retaddr && !g_title_module)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)retaddr, &g_title_module);
    if (game && IsWindow(game)) { g_game_window = game;
        if (!g_game_born_tick) g_game_born_tick = GetTickCount(); }
    if (bw && bh) { g_bb_w = bw; g_bb_h = bh; }
    if (fit && g_game_window && bw && bh)
        fit_window(g_game_window, bw, bh);
}

// Undo the backbuffer record when a windowed override was REJECTED and the
// caller's own fullscreen request gets replayed. Without this the game runs
// fullscreen at the desktop mode while GetSystemMetrics still tells it the
// screen is the backbuffer -- a wrong answer we would have invented ourselves,
// and worse than not hooking at all. (Same reasoning as the DirectSound and
// CreateDevice replay paths: never leave the client worse off than we found it.)
void d3d_forget_backbuffer_size()
{
    if (!g_bb_w && !g_bb_h) return;
    logf("[mode] backbuffer record cleared -- the windowed override was rejected, "
         "so the screen size must NOT be faked");
    g_bb_w = g_bb_h = 0;
}

// Everything the d3d8 path does AFTER a successful windowed CreateDevice.
void d3d_after_device_created(HWND game)
{
    if (game && IsWindow(game)) { g_game_window = game;
        if (!g_game_born_tick) g_game_born_tick = GetTickCount(); }
    if (!g_game_window) return;
    install_msgspy(g_game_window, "GAME");
    set_game_icon(g_game_window);
    // The white loading box. See apply_black_class_brush: this is the path FMO's
    // launch actually takes, and it was the one place the brush swap was missing.
    apply_black_class_brush(g_game_window);
    inspect_input_hwnd();
    schedule_audit();
    schedule_video_adopt();
    if (g_d3d_unmask) {
        schedule_mask_align();
        raise_game_window_once(g_game_window);
    }
}

int d3d_renderspy_requested() { return g_d3d_renderspy; }

// --- DirectShow video window adoption ---------------------------------------
//
// A title that plays an FMV through DirectShow gets a windowed video renderer:
// quartz makes a top-level window of class "VideoRenderer" and the game is
// supposed to place it over itself (IVideoWindow::put_Owner + SetWindowPosition).
// A game written for exclusive fullscreen computes that placement from the
// fullscreen rect it believes it owns -- so once we force it windowed, it can
// leave the video parked at DirectShow's DEFAULT 320x240 at (52,52), nowhere
// near the game.
//
// Measured on FMO, 2026-08-14: VMR-9 was PRESENTING FRAMES the whole time (the
// swap-chain counter proves it), into a 320x240 window off to one side, while
// the game window showed the black/white screen the user reported. So the movie
// worked; only its window was in the wrong place.
//
// The rule is deliberately conservative: adopt ONLY a video window that does not
// intersect the game's client area at all. Fantasy Earth positions its own video
// correctly (measured: it walks itself to 800x533 over the game), and this must
// not disturb a title that already gets it right.
//
// "VideoRenderer" IS NOT THE ONLY CLASS -- MEASURED 2026-08-17.
//
// Enumerating a live pol.exe's own windows found the FMV window sitting there as
// class "FilterGraphWindow", title "ActiveMovie Window", at DirectShow's default
// 320x240 near the origin -- and NOT ONE window of class "VideoRenderer" in the
// process. That single-name test is why a session showing the fault logged no
// [vid] lines at all: the matcher never saw the window, so nothing downstream of
// it -- neither the old intersection guard nor the size test that replaced it --
// ever got a say. Both classes are the same thing under different renderers
// (legacy Video Renderer vs VMR-7/9 in windowed mode), so match a LIST.
//
// It also explains "the logo is drawn twice": an un-adopted 320x240 movie window
// is a second top-level window sitting ON the game, playing the same title
// sequence small and soft next to the game's own sharp copy. One fault, two
// reports (the doubled logo and the fraction-sized FMV) -- fitting the window
// answers both.
//
// A candidate must be VISIBLE. The old code force-showed whatever it adopted
// (SWP_SHOWWINDOW), which was harmless while only a playing renderer could
// match; with FilterGraphWindow in the list it is not -- the graph window
// outlives the clip and sits hidden at 320x240 between FMVs (that is exactly the
// state it was measured in), and adopting it would paste a dead black rectangle
// over the whole game. Hidden means "not playing": skip it and re-check on the
// next poll, which is what the poll is for.
static LONG g_n_video_adopted = 0;

// [dx] d3d_videoclass (declared at the top of the file, read by d3d8_configure)
// -- comma-separated window classes to treat as the FMV window. A setting rather
// than a constant because the class depends on which DirectShow renderer the
// title's graph resolved to, and that is a property of the machine's codecs as
// much as of the title.
static bool is_video_class(const char* cls)
{
    const char* p = g_videoclass;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char* s = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - s);
        while (n && s[n - 1] == ' ') n--;
        if (n && _strnicmp(cls, s, n) == 0 && cls[n] == '\0') return true;
    }
    return false;
}

static bool rects_intersect(const RECT& a, const RECT& b)
{
    return a.left < b.right && b.left < a.right &&
           a.top < b.bottom && b.top < a.bottom;
}

// Name every top-level window this process owns, once. The failure this exists
// to end is the silent one: a session where the FMV is visibly wrong and the log
// has nothing in it, because the window it should have adopted was called
// something the matcher does not know. Printing the candidates makes the next
// unknown renderer a one-line ini change instead of another investigation.
static BOOL CALLBACK video_census_proc(HWND h, LPARAM lp)
{
    DWORD me = GetCurrentProcessId(), owner = 0;
    GetWindowThreadProcessId(h, &owner);
    if (owner != me) return TRUE;
    char cls[64] = "", txt[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    GetWindowTextA(h, txt, sizeof(txt));
    RECT r; ZeroMemory(&r, sizeof(r));
    GetWindowRect(h, &r);
    RECT c; ZeroMemory(&c, sizeof(c));
    GetClientRect(h, &c);
    logf("[vid]   window %p class=%s (%ld,%ld %ldx%ld) client=%ldx%ld visible=%d "
         "text='%s'%s",
         h, cls, r.left, r.top, r.right - r.left, r.bottom - r.top,
         c.right, c.bottom, IsWindowVisible(h) ? 1 : 0, txt,
         h == g_game_window ? "  <-- the game" :
         (is_video_class(cls) ? "  <-- matches d3d_videoclass" : ""));
    (*(int*)lp)++;
    return TRUE;
}

static BOOL CALLBACK adopt_video_proc(HWND h, LPARAM lp)
{
    DWORD me = GetCurrentProcessId(), owner = 0;
    GetWindowThreadProcessId(h, &owner);
    if (owner != me) return TRUE;
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    if (!is_video_class(cls)) return TRUE;
    if (!IsWindow(g_game_window)) return TRUE;
    // Not playing -- see the class note above. Re-checked every poll.
    //
    // Say so ONCE per window. A session that ends with "adopted 0" and a movie
    // visibly in the corner is exactly what happened on 2026-08-17, and the log
    // could not distinguish "never matched the class", "never seen" and "seen but
    // skipped". Now it can, and the answer points at whether vidfit's API route
    // is the one doing the work.
    if (!IsWindowVisible(h)) {
        static HWND said[8]; static int n_said = 0;
        bool told = false;
        for (int i = 0; i < n_said; i++) if (said[i] == h) { told = true; break; }
        if (!told && n_said < 8) {
            said[n_said++] = h;
            logf("[vid] %p class=%s matches but is HIDDEN -- not adopting. If a "
                 "movie is playing anyway, the renderer is WINDOWLESS and only "
                 "vidfit's IVMRWindowlessControl path can move it.", h, cls);
        }
        return TRUE;
    }

    RECT v, gc;
    GetWindowRect(h, &v);
    GetClientRect(g_game_window, &gc);
    POINT o = { 0, 0 };
    ClientToScreen(g_game_window, &o);
    RECT g = { o.x, o.y, o.x + gc.right, o.y + gc.bottom };

    // POSITION IS NOT ENOUGH, SIZE IS NOT ENOUGH EITHER -- SHAPE DECIDES.
    //
    // Two earlier rules, each fixing the one before and each wrong:
    //   1. bail whenever the video overlapped the game ("already over it, leave
    //      it") -- which left the common case untouched: the renderer sitting ON
    //      the game at its own native size, movie in a corner box.
    //   2. fit unless it already equals the client EXACTLY -- which stretches a
    //      movie whose shape is not the game's.
    //
    // Fit means fit the SHAPE: the largest rect with the movie's own aspect that
    // fits the game's client, centred in it. A 4:3 movie fills a 4:3 client
    // outright; anything else gets bars instead of distortion.
    //
    // WHERE THE MOVIE'S SHAPE COMES FROM -- and why it is the CLIENT rect.
    //
    // An earlier version of this took the WINDOW rect at first sighting. Measured
    // on a live session: both FMO's and FE's video windows sit at 320x240 with a
    // 304x201 client -- 320x240 is DirectShow's default WINDOW size and 304x201 is
    // simply what is left after an WS_OVERLAPPEDWINDOW frame. Neither number is
    // the movie. Using the outer one silently declares every unplaced movie 4:3,
    // which is right for FMO by luck and wrong for anything else.
    //
    // The client rect of a video window that is actually PLAYING is the video,
    // because that is what the renderer sizes it to. This only ever samples a
    // VISIBLE window (see the guard above), so what it records is a playing
    // window's client -- and it records it before our own resize destroys the
    // evidence. One slot per window, because a graph is rebuilt per clip and the
    // next movie may be a different shape.
    //
    // NO POL TITLE HAS BEEN SEEN TO PLACE ITS OWN VIDEO. This code used to say
    // Fantasy Earth "walks itself to 800x533 over the game" and must not be
    // disturbed. Measured 2026-08-17 on a live FE session, FE's video window is
    // top-level, un-owned, at DirectShow's default (78,78) 320x240 -- the same
    // state as FMO's. The claim was never tested, because until today the
    // adoption never ran for a d3d8 title at all.
    // Same PLACE-ONCE rule as vidfit (see GraphSlot there): re-fitting a movie the
    // title has since moved is a fight the user loses, because ours is the last
    // write every 500 ms. `placed` latches; only a game-window resize clears it.
    struct NaturalSize { HWND h; int w, ht; bool placed; int cw, ch; };
    static NaturalSize nat[8];
    static int n_nat = 0;
    RECT vc;
    if (!GetClientRect(h, &vc) || vc.right <= 0 || vc.bottom <= 0) return TRUE;
    int nw = v.right - v.left, nh = v.bottom - v.top;      // outer, for logging
    NaturalSize* ns = NULL;
    for (int i = 0; i < n_nat; i++) if (nat[i].h == h) { ns = &nat[i]; break; }
    if (!ns && n_nat < (int)(sizeof(nat) / sizeof(nat[0]))) {
        ns = &nat[n_nat++];
        ns->h = h; ns->w = vc.right; ns->ht = vc.bottom;
        ns->placed = false; ns->cw = 0; ns->ch = 0;
        logf("[vid] video window %p class=%s first seen VISIBLE at %dx%d (client "
             "%ldx%ld) -- taking the CLIENT as the movie's shape",
             h, cls, nw, nh, vc.right, vc.bottom);
    }
    if (!ns || ns->w <= 0 || ns->ht <= 0) return TRUE;

    int tw = gc.right, th = MulDiv(tw, ns->ht, ns->w);
    if (th > gc.bottom) { th = gc.bottom; tw = MulDiv(th, ns->w, ns->ht); }
    int tx = (gc.right - tw) / 2, ty = (gc.bottom - th) / 2;

    // Already where it belongs? Compare in the game's CLIENT space, which is the
    // space the target is computed in -- v is in screen coordinates.
    if (ns->placed && ns->cw == gc.right && ns->ch == gc.bottom)
        return TRUE;                       // answered once; the title owns it now
    const bool over = rects_intersect(v, g);
    if (over && GetParent(h) == g_game_window &&
        (v.left - g.left) == tx && (v.top - g.top) == ty && nw == tw && nh == th)
        return TRUE;

    // Exactly what IVideoWindow would have done, at the window level.
    //
    // STRIP THE WHOLE FRAME, not just WS_POPUP. DirectShow's default video window
    // is WS_OVERLAPPEDWINDOW (measured: style 0x04CF0000 on both titles), so
    // clearing only WS_POPUP left the adopted child wearing a CAPTION, a sizing
    // border and min/max boxes -- a title bar drawn across the top of the movie,
    // inside the game window, and a client area smaller than the rect we had just
    // computed for the video. Once it is a frameless child, client == window and
    // the size below IS the movie's size.
    SetWindowLongA(h, GWL_STYLE,
                   (GetWindowLongA(h, GWL_STYLE)
                    & ~(WS_POPUP | WS_OVERLAPPEDWINDOW))
                   | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    SetWindowLongA(h, GWL_EXSTYLE,
                   GetWindowLongA(h, GWL_EXSTYLE)
                   & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
    if (GetParent(h) != g_game_window)
        SetParent(h, g_game_window);
    SetWindowPos(h, HWND_TOP, tx, ty, tw, th,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    ns->placed = true; ns->cw = gc.right; ns->ch = gc.bottom;
    InterlockedIncrement(&g_n_video_adopted);
    logf("[vid] fitted DirectShow video window %p class=%s: was (%ld,%ld %dx%d) "
         "[%s the game] -> (%d,%d %dx%d) in a %ldx%ld client, keeping the movie's "
         "%d:%d shape%s",
         h, cls, v.left, v.top, nw, nh, over ? "over" : "OUTSIDE",
         tx, ty, tw, th, gc.right, gc.bottom, ns->w, ns->ht,
         (tw == gc.right && th == gc.bottom) ? " (fills the client)" : " (letterboxed)");
    return TRUE;
}

// The video window appears some time after the device, and may be recreated per
// clip, so this watches for a while rather than looking once.
//
// It watches for as long as there is a game to watch, not for a fixed two
// minutes. The old 240-iteration budget was spent by the time FMO reached its
// MENUS, so an FMV played from a menu fell outside it and was never adopted.
// The poll is one EnumWindows every 500 ms; what bounds it is the game window
// going away, which is the honest condition.
//
// It then EXITS rather than idling forever, and schedule_video_adopt can start
// it again -- the pairing matters, because this used to be armed once per
// PROCESS and a second title launched in the same session would have got a dead
// thread either way. One title at a time, one thread at a time.
static LONG g_video_thread_live = 0;

static DWORD WINAPI video_thread(LPVOID)
{
    // The graph interfaces are called from this thread, so it needs an apartment.
    // MTA: the filter graph registers ThreadingModel=Both, so it lives directly
    // in ours with no marshalling, and nothing else on this thread touches COM.
    HRESULT coinit = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    int idle = 0, census = 0;
    while (idle < 240) {                     // ~2 minutes with no game window at all
        Sleep(500);
        if (!g_game_window || !IsWindow(g_game_window)) {
            // The title is gone: drop our references to its graphs rather than
            // holding them for the rest of the process.
            if (idle == 0) vidfit_release_all();
            idle++;
            continue;
        }
        idle = 0;
        // API route FIRST. When it works it re-parents the video through
        // IVideoWindow::put_Owner, which makes the window a CHILD of the game --
        // and EnumWindows below only walks TOP-LEVEL windows, so the two compose
        // instead of fighting: whatever vidfit has placed, adopt_video_proc no
        // longer sees.
        vidfit_poll(g_game_window);
        EnumWindows(adopt_video_proc, 0);
        // One census, ~5s after the first game window exists: late enough that
        // the title's graph has been built, early enough to be near the launch
        // in the log. Costs one enumeration per title.
        if (++census == 10) {
            int n = 0;
            logf("[vid] window census (d3d_videoclass=%s):", g_videoclass);
            EnumWindows(video_census_proc, (LPARAM)&n);
            logf("[vid] %d window(s) in this process; adopted %ld so far. If the "
                 "FMV is wrong and none is marked as matching, add its class to "
                 "[dx] d3d_videoclass.", n, g_n_video_adopted);
        }
    }
    vidfit_release_all();
    if (SUCCEEDED(coinit)) CoUninitialize();
    InterlockedExchange(&g_video_thread_live, 0);
    return 0;
}

static void schedule_video_adopt()
{
    // The PROBE needs this thread too, and it is meant to be run with
    // d3d_fitvideo OFF -- so the old gate would have stopped the one
    // configuration the probe exists for.
    //
    // ...and so would it have stopped the title whose PROFILE asks for adoption.
    // d3d_fitvideo ships 0 as a mitigation for build 37's unskippable-FMV bug in
    // FMO, fixed in build 38 and never lifted; a per-title verdict is how a title
    // that needs adoption stops being held hostage by it. See TitleProfile::adopt_fmv.
    if (!g_d3d_fitvideo && !vidfit_probe_enabled() && !d3d_title_fmv_adopt()) return;
    if (InterlockedCompareExchange(&g_video_thread_live, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, video_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else   InterlockedExchange(&g_video_thread_live, 0);
}

static HRESULT STDMETHODCALLTYPE hook_CreateDevice(void* self, UINT adapter, DWORD devtype,
                                                   HWND focus, DWORD behavior,
                                                   D3D8_PRESENT_PARAMETERS* pp, void** ppDev)
{
    InterlockedIncrement(&g_n_create);
    if (pp && !pp->Windowed) InterlockedIncrement(&g_n_fullscreen_seen);
    if (focus) g_focus_window = focus;      // Reset() needs it later

    // Remember which title module called us (TM.dll for Tetra Master), so the
    // game window can wear that title's OWN icon rather than the generic pol.exe.
    if (!g_title_module)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)_ReturnAddress(), &g_title_module);

    // Arm the Fantasy Earth teardown guard. CreateDevice's return address lands in
    // the calling title, and FE's teardown crash only occurs once its device is
    // created (i.e. after this call), so this is both the reliable post-unpack
    // moment and strictly before the crash. Idempotent, byte-verified, FE-only --
    // a non-FE caller is a cheap name compare and no-op. See fepatch.cpp.
    if (fepatch_enabled()) {
        HMODULE fem = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)_ReturnAddress(), &fem) && fem) {
            char fpath[MAX_PATH] = "";
            GetModuleFileNameA(fem, fpath, MAX_PATH);
            const char* leaf = strrchr(fpath, '\\');
            leaf = leaf ? leaf + 1 : fpath;
            fepatch_apply_module((void*)fem, leaf);
        }
    }

    if (g_d3d_trace) {
        if (g_d3d_fpu)
            logf("[d3d] D3DCREATE_FPU_PRESERVE is being ADDED to this device "
                 "([dx] d3d_fpu_preserve=1) -- Direct3D will leave the x87 "
                 "control word alone instead of forcing single precision for "
                 "the whole process");
        logf("[d3d] CreateDevice(adapter=%u devtype=%lu focus=%p behavior=%08lX)",
             adapter, devtype, focus, behavior);
        log_pp("  requested", pp);
        if (pp && !pp->Windowed)
            logf("[d3d]   ^ FULLSCREEN -- this is the call that changes the monitor mode");
        // FE dies INSIDE the real d3d8 CreateDevice below (Wine wined3d abort: no CRT exit,
        // no VEH catch -- measured 2026-08-19, polshim.332.log). The buffered request line
        // dies with it, so flush the request now: on the next crash it survives and names the
        // exact device config that aborts. See the per-call flushes at each orig_CreateDevice.
        log_flush();
    }

    // RE-RESOLVE which title we are in, on the call that identifies one.
    //
    // A title asks for a FULLSCREEN device; the Viewer's own device is windowed.
    // So a fullscreen CreateDevice is exactly "a title is starting", and its
    // return address lands inside that title. Re-resolving here (rather than
    // latching the first caller for the life of the process) is what makes the
    // window icon follow the title actually running: with the latch, launching
    // Front Mission Online and then Fantasy Earth in one Viewer session left FE
    // wearing FMO's icon, because g_title_module still pointed at FMO's module.
    //
    // Deliberately NOT hung off gamestart.cpp, which was the first attempt: that
    // hook does not fire in every build/path (measured 2026-08-14: zero [gs]
    // lines in a session that launched two titles), so the reset never ran. This
    // sits on the call we know happens.
    if (pp && !pp->Windowed) {
        // Same fact, one step further: if this call identifies a title, it also
        // identifies the moment the PREVIOUS title's state stopped being true.
        // Declared here rather than only in d3d_prepare_game_window because an
        // EXCEPTED title is left fullscreen and never reaches that function -- and
        // it needs the reset just as much. (Until 2026-08-24 that meant FFXI and
        // FMO via their PW_FULLSCREEN profiles; now it means a title someone put
        // back by hand with d3d_windowed_except, or FFXI under an add-on core. The
        // path is rarer, not gone -- and rarer is exactly when it stops being
        // tested, so it stays here.)
        d3d_title_boundary(_ReturnAddress(), focus);
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)_ReturnAddress(), &m) && m) {
            if (m != g_title_module) {
                char nm[MAX_PATH] = "";
                GetModuleFileNameA(m, nm, MAX_PATH);
                const char* leaf = strrchr(nm, '\\');
                logf("[d3d] title module -> %s (was %p)", leaf ? leaf + 1 : nm,
                     (void*)g_title_module);
            }
            g_title_module = m;
        }
    }

    // Per-title opt-out. Logged rather than silent: "the override did not fire"
    // and "the override fired and did not help" are different findings, and with
    // one shared vtable there is nothing else in the log to tell them apart.
    ExceptWhy exc_why = EXC_NONE;
    bool excepted = caller_except_why(_ReturnAddress(), &exc_why);
    // Record whether THIS title windows itself, so fit_window (here and on a later Reset)
    // sizes it to its backbuffer instead of a remembered size. Set per device creation.
    g_title_native_windowed = caller_native_windowed(_ReturnAddress());
    if (excepted && g_d3d_windowed && pp && !pp->Windowed)
        logf("[d3d]   caller is EXCEPTED from the windowed override by %s -- "
             "leaving it FULLSCREEN", except_source(exc_why));

    // Backbuffer override, applied to BOTH paths -- the pointer mapping is wrong
    // at any size but 640x480, windowed or not.
    D3D8_PRESENT_PARAMETERS forced;
    if (pp && g_bb_force_w && g_bb_force_h) {
        forced = *pp;
        if (force_bbsize(&forced)) {
            logf("[d3d]   backbuffer FORCED %ux%u -> %ux%u (d3d_bbsize) -- pol.exe "
                 "maps the cursor into a fixed 640x480 space, so a title at any "
                 "other size receives mis-scaled clicks",
                 pp->BackBufferWidth, pp->BackBufferHeight,
                 forced.BackBufferWidth, forced.BackBufferHeight);
            pp = &forced;
        }
    }

    // The present parameters the device is REALLY built with, whichever branch
    // below wins. wakerecover.cpp Resets with these; see g_pp_last.
    D3D8_PRESENT_PARAMETERS pp_used;
    bool pp_used_ok = false;

    HRESULT hr;
    if (g_d3d_windowed && !excepted && pp && !pp->Windowed) {
        // Vertex-processing override (see apply_vp_override), scoped to the forced-windowed
        // path so it never touches an excepted title (FFXI/FMO). FE asks for HARDWARE VP,
        // which aborts wined3d on the Deck; TM's SOFTWARE VP on the same vtable works.
        if (g_d3d_vp) {
            DWORD nb = apply_vp_override(behavior);
            if (nb != behavior) {
                static LONG said = 0;
                if (InterlockedCompareExchange(&said, 1, 0) == 0)
                    logf("[d3d] vertex-processing override: behavior %08lX -> %08lX "
                         "(d3d_vp=%d, %s)", behavior, nb, g_d3d_vp,
                         g_d3d_vp == 2 ? "mixed" : "software");
                behavior = nb;
            }
        }
        D3D8_PRESENT_PARAMETERS mod = *pp;
        make_windowed(&mod, adapter_format(self, adapter), focus);
        if (g_d3d_trace) log_window("focus window BEFORE", focus);
        // Resize BEFORE creating the device: windowed presentation is sized from
        // the client area, so the device must be built against the final one.
        g_game_window = mod.hDeviceWindow ? mod.hDeviceWindow : focus;
        if (!g_game_born_tick) g_game_born_tick = GetTickCount();
        g_bb_w = mod.BackBufferWidth;      // what the game must believe the screen is
        g_bb_h = mod.BackBufferHeight;
        if (g_d3d_fitwindow)
            fit_window(g_game_window, mod.BackBufferWidth, mod.BackBufferHeight);
        logf("[d3d] -> real CreateDevice, FORCED WINDOWED (windowed=%d %ux%u fmt=%u behavior=%08lX). "
             "If the log stops HERE, Wine aborted inside CreateDevice with these params.",
             (int)mod.Windowed, mod.BackBufferWidth, mod.BackBufferHeight,
             mod.BackBufferFormat, behavior);
        log_flush();
        hr = orig_CreateDevice(self, adapter, devtype, focus, behavior, &mod, ppDev);
        if (g_d3d_trace) log_window("focus window AFTER", focus);
        if (SUCCEEDED(hr)) install_msgspy(g_game_window, "GAME");
        if (SUCCEEDED(hr)) set_game_icon(g_game_window);
        if (SUCCEEDED(hr)) inspect_input_hwnd();
        if (SUCCEEDED(hr)) schedule_audit();
        // THE FMV ADOPTION NEVER RAN FOR A d3d8 TITLE. Added 2026-08-17.
        //
        // schedule_video_adopt lived only in d3d_after_device_created, and only
        // the d3d9 path calls that -- so the whole feature had only ever run for
        // FMO, the one d3d9 title. Fantasy Earth, Tetra Master and FFXI are d3d8
        // and reached it never. That is the drift the "run the IDENTICAL sequence
        // instead of a copy of it" comment above d3d_prepare_game_window warns
        // about, still present in this branch.
        //
        // It also means the code's claim that FE "positions its own video
        // correctly" was never tested by anything: measured on a live FE session
        // today, FE's video window is class FilterGraphWindow at (78,78) 320x240,
        // top-level and un-owned -- DirectShow's untouched default, exactly like
        // FMO's. No POL title has been seen to claim its own video window.
        if (SUCCEEDED(hr)) schedule_video_adopt();
        if (SUCCEEDED(hr) && g_d3d_unmask) {
            schedule_mask_align();
            // Put the game window in front of the mask -- once per title (see
            // raise_game_window_once: re-raising on every device churn is the fight).
            HWND target = mod.hDeviceWindow ? mod.hDeviceWindow : focus;
            raise_game_window_once(target);
        }
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_n_forced);
            log_pp("  FORCED WINDOWED", &mod);
            d3d_note_device_mode(1);
            pp_used = mod; pp_used_ok = true;
        } else {
            // Same rule as the DirectSound path: never leave the client worse
            // off than we found it. Replay exactly what it asked for.
            InterlockedIncrement(&g_n_retry);
            logf("[d3d] windowed override rejected (hr=0x%08lX) -- replaying the "
                 "client's own fullscreen request", hr);
            // The game is going fullscreen after all, so stop reporting the
            // backbuffer as the screen -- see d3d_forget_backbuffer_size.
            d3d_forget_backbuffer_size();
            logf("[d3d] -> real CreateDevice, REPLAY client's own request (windowed=%d %ux%u). "
                 "If the log stops HERE, Wine aborted inside it.",
                 pp ? (int)pp->Windowed : -1, pp ? pp->BackBufferWidth : 0,
                 pp ? pp->BackBufferHeight : 0);
            log_flush();
            hr = orig_CreateDevice(self, adapter, devtype, focus, behavior, pp, ppDev);
            if (SUCCEEDED(hr) && pp) { d3d_note_device_mode(pp->Windowed);
                                       pp_used = *pp; pp_used_ok = true; }
        }
    } else {
        logf("[d3d] -> real CreateDevice, params UNCHANGED (windowed=%d %ux%u; excepted=%d). "
             "If the log stops HERE, Wine aborted inside it.",
             pp ? (int)pp->Windowed : -1, pp ? pp->BackBufferWidth : 0,
             pp ? pp->BackBufferHeight : 0, (int)excepted);
        log_flush();
        hr = orig_CreateDevice(self, adapter, devtype, focus, behavior, pp, ppDev);
        if (SUCCEEDED(hr) && pp) { d3d_note_device_mode(pp->Windowed);
                                   pp_used = *pp; pp_used_ok = true; }

        // NATIVE-WINDOWED TITLE (Fantasy Earth, -windowmode). The shim does NOT touch
        // its window or device -- FE sizes and frames its own -- but the FMV adoption
        // still has to run, because no POL title places its own video window and
        // without adoption FE's raw DirectShow video window pops up over the game as
        // clips play. This is the part of the forced-windowed post-device sequence that
        // is about the MOVIE, not the sizing, so it is the only part replayed here.
        // AN ADD-ON CORE IS DRIVING THIS TITLE -- but only the DEVICE is theirs.
        //
        // Same shape as the native-windowed arm below, and for the same reason: the
        // pass-through branch skips the entire post-device sequence, and some of that
        // sequence is not about sizing at all. Ashita wraps the device and does its own
        // presentation; it does not know PlayOnline's mask window exists, so leaving
        // the mask full-screen over the desktop is not "letting the core handle it",
        // it is nobody handling it.
        //
        // DELIBERATELY NARROW. Only the two things a core provably cannot do:
        //   * the mask realign (align it to the title's window, alpha-0, parked behind
        //     -- NOT hide it: SW_HIDE here black-screened the display, 2026-08-18)
        //   * the window icon, which is cosmetic and touches nothing the core owns
        // Not the title boundary (under a core the return address is the CORE's, so it
        // would name Ashita.dll as the title), not msgspy (the core owns input), and
        // nothing that resizes or re-presents. Those stay handed over.
        if (SUCCEEDED(hr) && excepted && caller_excepted_by_core(_ReturnAddress())) {
            HWND w = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;
            if (w && IsWindow(w)) {
                set_game_icon(w);
                if (g_d3d_unmask) schedule_mask_align();
                logf("[d3d] add-on core owns this device -- device, sizing and cursor "
                     "left entirely to it, but the POL mask is still aligned to %p and "
                     "the window icon set. Neither is anything a core can do: the mask "
                     "is a Viewer window they cannot see.", (void*)w);
            }
        }

        if (SUCCEEDED(hr) && excepted && caller_native_windowed(_ReturnAddress())) {
            HWND w = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;
            if (w && IsWindow(w)) {
                // A NATIVE-WINDOWED TITLE NEEDS A TITLE BOUNDARY TOO, and this is the
                // only place it can get one. d3d_title_boundary is otherwise driven off
                // a FULLSCREEN CreateDevice ("a title is starting") -- but FE, handed
                // -windowmode, creates a WINDOWED device and never trips that test, and
                // it never reaches d3d_prepare_game_window either. So without this call
                // FE inherits the previous title's latched state, including which title
                // the profile lookups resolve against.
                d3d_title_boundary(_ReturnAddress(), w);
                g_game_window = w;
                if (!g_game_born_tick) g_game_born_tick = GetTickCount();
                install_msgspy(w, "GAME");
                set_game_icon(w);
                // THE FMV ADOPTION. The block comment above has always said this is
                // "the only part replayed here" -- and the call was never made, so the
                // one thing this branch existed to keep doing was the one thing it did
                // not do. Reported live 2026-08-25: FE's opening movie playing over the
                // Viewer UI. The log line below said "no ... video override" and was
                // right about the code and wrong about the intent, which is why reading
                // it did not contradict the comment.
                schedule_video_adopt();
                // ALIGN the POL mask to the title's window (shrunk to it, alpha-0, parked
                // behind), NOT hide it. Measured 2026-08-18: SW_HIDE-ing the mask here
                // black-screened the whole display, while aligning it leaves the title
                // visible -- the shell keeps the mask load-bearing in a way a hide breaks.
                // The title reads its own input in -windowmode, so binding polcore's
                // CreateInput to the aligned mask is harmless; this only stops the
                // full-screen mask covering the desktop.
                if (g_d3d_unmask) schedule_mask_align();
                logf("[d3d] native-windowed title: window %p left to the title (no size "
                     "or cursor override); POL mask aligned behind it, and its FMV "
                     "window IS adopted -- the title places its own frame, not its own "
                     "video.", (void*)w);
            }
        }
    }

    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        InterlockedIncrement(&g_n_device);
        // The FINAL parameters, for the sleep/resume recovery. Recorded before
        // anything else can fail, and only when a branch above actually said
        // which set the device was built with.
        if (pp_used_ok) { g_pp_last = pp_used; g_pp_have = true; }
        g_game_device = *ppDev;
        // Reset is how a device changes mode AFTER creation (alt-tab, an
        // in-game resolution change). Hook it on the first device we see.
        if (!g_dev_hooked) {
            g_dev_hooked = true;          // one attempt, whatever the outcome
            if (validate_device(*ppDev)) {
                g_dev_vt_ok = true;
                if (patch_slot(*(void***)*ppDev, SD_Reset, (void*)hook_Reset,
                               (void**)&orig_Reset)) {
                    logf("[d3d] Reset hooked at device vtable slot %d (orig=%p)",
                         SD_Reset, orig_Reset);
                    // Device-lost diagnostic: hook TestCooperativeLevel (slot 3)
                    // to see, passively, what state TM's poll actually gets.
                    if (patch_slot(*(void***)*ppDev, SD_TestCoopLevel,
                                   (void*)hook_TestCoopLevel, (void**)&orig_TestCoop))
                        logf("[d3dcoop] TestCooperativeLevel hooked at device slot %d "
                             "(orig=%p)", SD_TestCoopLevel, orig_TestCoop);
                    // Slot 8 was already validated against the real desktop mode
                    // by validate_device(), so this index is proven, not guessed.
                    if (g_d3d_fakemode &&
                        patch_slot(*(void***)*ppDev, SD_GetDisplayMode,
                                   (void*)hook_GetDisplayMode,
                                   (void**)&orig_GetDisplayMode))
                        logf("[mode] GetDisplayMode hooked at device slot %d -- "
                             "the game will be told the screen is %ux%u",
                             SD_GetDisplayMode, g_bb_w, g_bb_h);
                } else {
                    orig_Reset = NULL;
                    logf("[d3d] VirtualProtect failed on the device vtable -- "
                         "Reset not hooked");
                }
                if (g_d3d_renderspy) install_renderspy(*ppDev);
                arm_hang_probe();
            }
        }
        // AFTER renderspy, which shares the Present slot: whichever installs
        // first wins the slot and the other reuses it. Outside the !g_dev_hooked
        // guard on purpose -- the vtable is patched once, but the DEVICE POINTER
        // changes when a title recreates its device, and the recovery must
        // follow the live one.
        if (g_dev_vt_ok) wake_arm_d3d8(*ppDev, g_game_window);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_Reset(void* self, D3D8_PRESENT_PARAMETERS* pp)
{
    InterlockedIncrement(&g_n_reset);
    // The title is resetting its OWN device. That is the whole lost-device
    // contract being honoured, and it is the signal wakerecover.cpp stands down
    // on -- noted before the call, so a Reset that hangs or aborts still counts
    // as an attempt rather than looking like the title did nothing.
    wake_note_reset(self, S_OK);
    if (g_d3d_trace) log_pp("Reset requested", pp);

    // Same opt-out as CreateDevice. A title excluded there must be excluded here
    // too, or its first Reset would silently drag it back into windowed mode.
    if (g_d3d_windowed && !caller_excepted(_ReturnAddress()) && pp && !pp->Windowed) {
        D3D8_PRESENT_PARAMETERS mod = *pp;
        // Reset has no hFocusWindow argument, so the only window we can name is
        // whatever the caller put in hDeviceWindow -- plus the one we recorded at
        // CreateDevice, which is the usual case since the caller passes NULL.
        make_windowed(&mod, device_format(self), g_focus_window);
        if (g_d3d_fitwindow)
            fit_window(mod.hDeviceWindow ? mod.hDeviceWindow : g_focus_window,
                       mod.BackBufferWidth, mod.BackBufferHeight);
        HRESULT hr = orig_Reset(self, &mod);
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_n_forced);
            log_pp("Reset FORCED WINDOWED", &mod);
            g_pp_last = mod; g_pp_have = true;   // the device's real parameters now
            d3d_note_device_mode(1);
            // A Reset is exactly when the Viewer tends to put the mask back up.
            if (g_d3d_unmask) schedule_mask_align();
            return hr;
        }
        InterlockedIncrement(&g_n_retry);
        logf("[d3d] windowed Reset rejected (hr=0x%08lX) -- replaying the client's", hr);
    }
    {
        // A Reset is how a title changes mode after creation, so it is the other
        // place the live device's fullscreen state can flip.
        HRESULT hr = orig_Reset(self, pp);
        // Device-lost diagnostic: log whether the pass-through Reset actually
        // succeeds. TM Resets every frame while the device is lost; if this keeps
        // returning an error the device never recovers and nothing draws.
        static LONG g_n_reset_pt = 0;
        LONG rn = InterlockedIncrement(&g_n_reset_pt);
        if (rn <= 8 || (rn % 500) == 0) {
            logf("[d3d] Reset(passthru Windowed=%d %ux%u) -> 0x%08lX",
                 pp ? (int)pp->Windowed : -1, pp ? pp->BackBufferWidth : 0,
                 pp ? pp->BackBufferHeight : 0, (unsigned long)hr);
            log_flush();
        }
        if (SUCCEEDED(hr) && pp) { d3d_note_device_mode(pp->Windowed);
                                   g_pp_last = *pp; g_pp_have = true; }
        return hr;
    }
}

// Pass-through TestCooperativeLevel: log the device-lost state on every change
// (plus the first few). Change-gated so a device stuck at one value logs once.
static HRESULT STDMETHODCALLTYPE hook_TestCoopLevel(void* self)
{
    HRESULT hr = orig_TestCoop(self);
    LONG n = InterlockedIncrement(&g_n_tcl);
    if (hr != g_last_tcl || n <= 4) {
        g_last_tcl = hr;
        logf("[d3dcoop] TestCooperativeLevel #%ld -> 0x%08lX%s", n, (unsigned long)hr,
             hr == 0                      ? " (D3D_OK)" :
             hr == (HRESULT)0x88760868    ? " (D3DERR_DEVICELOST)" :
             hr == (HRESULT)0x88760869    ? " (D3DERR_DEVICENOTRESET)" : "");
        log_flush();
    }
    // The pump's OTHER entry point, and for a title that skips Present while the
    // device is lost it is the only one. Same thread as Present by construction.
    wake_note_test(self, hr);
    return hr;
}

// Draw-gate diagnostic: TM proceeds to BeginScene when the device is healthy,
// and returns WITHOUT drawing if BeginScene fails. So logging its result (and
// whether Present ever fires) separates "D3D can't draw" from "game clears the
// frame but has no content to draw" (e.g. stuck waiting on its game server).
static PFN_DevBeginScene orig_BeginScene = NULL;
static PFN_DevPresent    orig_Present    = NULL;
static LONG    g_n_begin = 0, g_n_present = 0;
static HRESULT g_last_begin = (HRESULT)0xDEADBEEF;

// Hang probe: armed once at device creation when [dx] hang_dump_ms > 0. Sleeps the
// delay, then -- if the title has still not presented a frame -- dumps every thread's
// stack so a wedged-but-running title (FE loads the in-game Friend List and never
// presents) names what its main thread is blocked on. One-shot; in-process, so it
// works under Proton where a host debugger cannot attach.
static DWORD WINAPI hang_dump_thread(void*)
{
    Sleep(g_hang_dump_ms > 0 ? (DWORD)g_hang_dump_ms : 20000);
    LONG p = g_n_present;
    if (p >= 30) {
        logf("[tdump] hang probe: title is presenting (%ld frames) -- not dumping", p);
        return 0;
    }
    char why[128];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                "title has presented only %ld frame(s) %dms after device creation "
                "(wedged/black?)", p, g_hang_dump_ms);
    crashlog_dump_all_threads(why);
    return 0;
}

static void arm_hang_probe()
{
    if (g_hang_dump_ms <= 0) return;
    static LONG armed = 0;
    if (InterlockedCompareExchange(&armed, 1, 0) != 0) return;
    logf("[tdump] hang probe armed: will dump all thread stacks in %dms if the "
         "title has not presented by then", g_hang_dump_ms);
    HANDLE t = CreateThread(NULL, 0, hang_dump_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static HRESULT STDMETHODCALLTYPE hook_BeginScene(void* self)
{
    HRESULT hr = orig_BeginScene(self);
    LONG n = InterlockedIncrement(&g_n_begin);
    if (hr != g_last_begin || n <= 4) {
        g_last_begin = hr;
        logf("[d3dcoop] BeginScene #%ld -> 0x%08lX%s", n, (unsigned long)hr,
             hr == 0                   ? " (OK)" :
             hr == (HRESULT)0x8876086C ? " (D3DERR_INVALIDCALL)" :
             hr == (HRESULT)0x88760868 ? " (D3DERR_DEVICELOST)" : "");
        log_flush();
    }
    return hr;
}

// Deck/Proton workaround: if TM.dll's SE-request vector holds the stuck entry
// {id,sub} at its head, clear the vector so TM stops waiting on the load that
// never completes and proceeds to draw. A single write to the vector's END
// pointer (begin==end => empty), which is exactly the manual poke that unblocked
// it live; no memcpy, so the race window against TM's own thread is one aligned
// store into memory that stays valid. SEH- and sanity-guarded so a wrong RVA on
// some other build is a no-op, never a corruption. Runs each Present (the frame
// loop keeps running during the hang, so this fires ~60x/s and re-clears as the
// entry re-queues).
static LONG g_n_unstick = 0;
static void tm_unstick_se(void)
{
    if (!g_tm_se_unstick) return;
    HMODULE tm = GetModuleHandleA("TM.dll");
    if (!tm) return;
    unsigned char* base = (unsigned char*)tm;
    __try {
        unsigned int* pbegin = (unsigned int*)(base + g_tm_unstick_rva);
        unsigned int* pend   = (unsigned int*)(base + g_tm_unstick_rva + 4);
        unsigned int  begin  = *pbegin, end = *pend;
        // Sanity: a plausible std::vector of 12-byte entries. Bail on anything odd
        // rather than trust a possibly-wrong offset.
        if (!begin || end <= begin) return;
        unsigned int span = end - begin;
        if ((span % 12) != 0 || span > 0x100000) return;
        // SURGICAL: compact out ONLY the stuck {id,[sub]} entries and keep every
        // other entry, so the OTHER pending loads (images that were queued behind
        // the stall) still complete. A whole-vector clear here also threw those
        // away -> missing images everywhere; this drops only the gW201 resource
        // that actually hangs under Proton. The memcpy shifts kept entries down
        // within the same buffer, then we lower `end`; bounded, SEH-guarded.
        unsigned char* rd = (unsigned char*)begin;
        unsigned char* wr = (unsigned char*)begin;
        unsigned char* e  = (unsigned char*)end;
        int removed = 0; unsigned int hit_sub = 0;
        while (rd + 12 <= e) {
            unsigned int rid = *(unsigned int*)(rd + 4), rsub = *(unsigned int*)(rd + 8);
            if (rid == g_tm_unstick_id && (g_tm_unstick_sub == 0 || rsub == g_tm_unstick_sub)) {
                removed++; hit_sub = rsub;                 // drop this one
            } else {
                if (wr != rd) memcpy(wr, rd, 12);          // keep it, shift down
                wr += 12;
            }
            rd += 12;
        }
        if (removed) {
            *pend = (unsigned int)(UINT_PTR)wr;            // new end past the kept entries
            LONG n = InterlockedIncrement(&g_n_unstick);
            if (n <= 5 || (n % 600) == 0)
                logf("[tmse] dropped %d stuck {id=%u sub=%u} entr(ies), kept the rest "
                     "(%u byte queue) -> TM can draw (#%ld)",
                     removed, g_tm_unstick_id, hit_sub, span, n);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Wrong RVA for this build, or the vector moved mid-read. Disable further
        // attempts this session so we never fault the game from here.
        g_tm_se_unstick = 0;
        logf("[tmse] fault reading TM.dll+0x%X -- unstick disabled for this session", g_tm_unstick_rva);
    }
}

// Read-only observable for the gW path-dot hypothesis -- see the g_tm_diag
// comment at the top of the file for why this exists and what the max key
// means. Logs the basedir once it is non-empty and the map shape once it is
// populated; if the map is still empty after ~1 minute of frames, says so once
// (that is the "glob never ran / found nothing" arm, also worth a line).
// Everything here is a guarded read; on any fault it disarms for the session.
static int  g_tmdiag_basedir_logged = 0;
static int  g_tmdiag_map_logged     = 0;
static LONG g_tmdiag_frames         = 0;
static void tm_index_diag(void)
{
    if (!g_tm_diag || (g_tmdiag_basedir_logged && g_tmdiag_map_logged)) return;
    HMODULE tm = GetModuleHandleA("TM.dll");
    if (!tm) return;
    unsigned char* base = (unsigned char*)tm;
    LONG frames = InterlockedIncrement(&g_tmdiag_frames);
    __try {
        if (!g_tmdiag_basedir_logged) {
            char dir[104];
            memcpy(dir, base + g_tm_basedir_rva, 100);
            dir[100] = 0;
            if (dir[0]) {
                for (char* p = dir; *p; ++p)
                    if ((unsigned char)*p < 0x20) *p = '?';
                logf("[tmdiag] basedir @TM.dll+0x%X = \"%s\"%s",
                     g_tm_basedir_rva, dir,
                     strchr(dir, '.') ? "  (CONTAINS A DOT before the filename "
                                        "-> gW fileNumber will misparse)" : "");
                g_tmdiag_basedir_logged = 1;
                log_flush();
            }
        }
        if (!g_tmdiag_map_logged && frames >= 600) {
            // Raw dump, not a modeled read: v1 gated on "size" at +8 and never
            // fired, most plausibly because VC6's _Tree keeps a _Multi bool
            // between _Head and _Size. Print the four dwords and both candidate
            // key reads and let the analyst decide; the verdict heuristic uses
            // the rightmost key (gW keys are fileNumber*10000+sub, so any
            // correctly parsed file puts the max at >= 10000).
            unsigned int m0   = *(unsigned int*)(base + g_tm_map_rva);
            unsigned int head = *(unsigned int*)(base + g_tm_map_rva + 4);
            unsigned int m2   = *(unsigned int*)(base + g_tm_map_rva + 8);
            unsigned int m3   = *(unsigned int*)(base + g_tm_map_rva + 12);
            unsigned int lo_n = 0, hi_n = 0, lo = 0, hi = 0;
            if (head > 0x10000) {
                lo_n = *(unsigned int*)(head + 0x00);   // header->_Left  = leftmost
                hi_n = *(unsigned int*)(head + 0x08);   // header->_Right = rightmost
                if (lo_n > 0x10000) lo = *(unsigned int*)(lo_n + 0x0C);
                if (hi_n > 0x10000) hi = *(unsigned int*)(hi_n + 0x0C);
            }
            logf("[tmdiag] gW index raw @+0x%X: [+0]=%08X head=%08X [+8]=%u [+C]=%u "
                 "| leftmost node=%08X key=%u rightmost node=%08X key=%u -> %s",
                 g_tm_map_rva, m0, head, m2, m3, lo_n, lo, hi_n, hi,
                 (hi && hi < 10000) ? "COLLAPSED to bare subs (path-dot bug LIVE)" :
                 hi >= 10000        ? "file-numbered (path parse OK)"
                                    : "unreadable/empty -- interpret the raw dwords");
            g_tmdiag_map_logged = 1;
            log_flush();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tm_diag = 0;
        logf("[tmdiag] fault reading TM.dll+0x%X/0x%X -- diag disabled for this session",
             g_tm_basedir_rva, g_tm_map_rva);
    }
}

// Two ASCII hex chars at p -> byte value, or -1. Mirrors TM.dll 0xA9780, used
// to read the (code, msgid) out of a store entry's ASCII header (entry+0x25).
static int resv_hex2(const unsigned char* p)
{
    int v = 0;
    for (int i = 0; i < 2; ++i) {
        unsigned char c = p[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

// See the g_tm_resv_diag comment for what this settles and why it is safe.
static unsigned g_rd_last_108   = 0xFFFFFFFF;
static unsigned g_rd_last_10c   = 0xFFFFFFFF;
static int      g_rd_last_arm   = -1;
static int      g_rd_last_0xc   = -1;
static int      g_rd_last_obj   = -1;
static LONG     g_rd_lines      = 0;
static LONG     g_rd_polls      = 0;
static int      g_rd_latch_armreply = 0;   // saw arm=1 && 0x41/0xC=1 together
static int      g_rd_latch_filled   = 0;   // saw +0x108 >= 1
// Non-static: also driven from inject.cpp's arm_title_probes_late() hot path,
// because on native Windows TM does NOT go through our d3d8 Present hook (the
// Deck does) -- so Present-only would never fire here. The hot path (every
// socket/OutputDebugString) is what makes tmpathfix work on Windows.
void tm_resv_diag(void)
{
    if (!g_tm_resv_diag) return;
    HMODULE tm = GetModuleHandleA("TM.dll");
    if (!tm) return;
    unsigned char* base = (unsigned char*)tm;
    __try {
        unsigned int obj  = *(unsigned int*)(base + g_tm_resv_obj_rva);
        int obj_valid = (obj > 0x10000) ? 1 : 0;
        unsigned int v108 = 0, v10c = 0, v41c = 0;
        if (obj_valid) {
            v108 = *(unsigned int*)(obj + 0x108);
            v10c = *(unsigned int*)(obj + 0x10c);
            v41c = *(unsigned int*)(obj + 0x41c);
        }
        unsigned int cnt = *(unsigned int*)(base + g_tm_resv_count_rva);
        int have0xc = 0, have0x4 = 0;
        if (cnt > 0 && cnt <= 128) {
            for (unsigned int i = 0; i < cnt; ++i) {
                unsigned char* e = base + g_tm_resv_store_rva + i * 0x3D8;
                int code  = resv_hex2(e + 0x25);
                int msgid = resv_hex2(e + 0x25 + 4);
                if (code == 0x41 && msgid == 0x0C) have0xc = 1;
                if (code == 0x41 && msgid == 0x04) have0x4 = 1;
            }
        }
        int arm = (v41c != 0) ? 1 : 0;
        LONG poll = InterlockedIncrement(&g_rd_polls);

        // LATCHES -- fire ONCE, regardless of the change/heartbeat gate, so a
        // single-sample event cannot be missed. These settle the whole question.
        if (arm && have0xc && !g_rd_latch_armreply) {
            g_rd_latch_armreply = 1;
            logf("[resvdiag] *** LATCH: arm=1 AND 0x41/0xC=1 in the SAME sample "
                 "(+0x108=%u) -- the reply DOES reach the armed branch%s", v108,
                 v108 >= 1 ? " AND +0x108 filled -> IT WORKS"
                           : " but +0x108 is still 0 -> the branch REJECTS it (parser)");
            log_flush();
        }
        if (v108 >= 1 && !g_rd_latch_filled) {
            g_rd_latch_filled = 1;
            logf("[resvdiag] *** LATCH: +0x108 FILLED to %u -- the copy 0xA90A0 ran!", v108);
            log_flush();
        }

        int changed = (v108 != g_rd_last_108 || v10c != g_rd_last_10c ||
                       arm != g_rd_last_arm || have0xc != g_rd_last_0xc ||
                       obj_valid != g_rd_last_obj);
        int heartbeat = (poll % 200) == 0;   // ~every 5s at 25ms, so we always see the live state
        if (changed || heartbeat) {
            LONG n = InterlockedIncrement(&g_rd_lines);
            // Cap total lines so an oscillating state can never flood the log.
            if (n <= 800 || (n % 300) == 0) {
                logf("[resvdiag] obj=%s +0x108(resv)=%u +0x10c(memb)=%u arm(+0x41c)=%d "
                     "store=%u 0x41/0xC=%d 0x41/0x4=%d%s%s",
                     obj_valid ? "OK" : "null", v108, v10c, arm, cnt, have0xc, have0x4,
                     (heartbeat && !changed) ? " [hb]" : "",
                     (have0xc && arm && v108 == 0)
                        ? "  <== REPLY PRESENT + ARMED but +0x108 STILL 0 (parser rejects)"
                        : (v108 >= 1) ? "  <== +0x108 FILLED" : "");
                log_flush();
            }
            g_rd_last_108 = v108; g_rd_last_10c = v10c; g_rd_last_arm = arm;
            g_rd_last_0xc = have0xc; g_rd_last_obj = obj_valid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tm_resv_diag = 0;
        logf("[resvdiag] fault reading TM.dll reservation slots -- disabled for this session");
    }
}

// The reservation state changes on a scale of seconds (message arrival, scene
// change), and NEITHER available driver on native Windows ticks continuously: the
// d3d8 Present hook never fires here, and arm_title_probes_late() runs only on
// socket()/OutputDebugString (great for tmpathfix's one-shot patch, useless for
// polling -- it logged one all-zeros line pre-lobby then went quiet). So poll from
// our own thread: every 200ms is ample resolution for a second-scale state, and it
// is the sole caller of tm_resv_diag() so the change-trackers never race.
static DWORD WINAPI resv_diag_thread(LPVOID)
{
    // 25ms (~40Hz): the reservation reply can be consumed by the armed branch
    // within a client frame (~16ms), so a 200ms poll missed the transient. At
    // 25ms we sample faster than the ~15s re-feed's residency window, and the
    // latches below flag the key events even on a single hit.
    for (;;) {
        Sleep(25);
        tm_resv_diag();
    }
}
void tm_resv_diag_start(void)
{
    static LONG started = 0;
    if (!g_tm_resv_diag) return;
    if (InterlockedExchange(&started, 1)) return;   // exactly once per process
    HANDLE h = CreateThread(NULL, 0, resv_diag_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    logf("[resvdiag] poller thread started (200ms) -- watching TM.dll reservation slot +0x108");
    log_flush();
}

static HRESULT STDMETHODCALLTYPE hook_Present(void* self, const void* a, const void* b,
                                              HWND c, const void* d)
{
    tm_unstick_se();
    tm_index_diag();
    tm_resv_diag_start();
    HRESULT hr = orig_Present(self, a, b, c, d);
    LONG n = InterlockedIncrement(&g_n_present);
    // The heartbeat is renderspy's instrument. Since the sleep/resume recovery
    // also needs this slot, Present is now hooked in sessions where renderspy is
    // OFF -- and at 60fps a line every 1000 frames is one every 16 seconds for
    // the whole session. Keep the diagnostic cadence when it was asked for, and
    // drop to a five-minute heartbeat when the hook is only here for the wake.
    LONG every = g_d3d_renderspy ? 1000 : 18000;
    if (n <= 4 || (n % every) == 0) {
        logf("[d3dcoop] Present #%ld -> 0x%08lX (frame loop IS running)", n, (unsigned long)hr);
        log_flush();
    }
    // Frame accounting + the lost-device trigger. wake_note_present is an
    // increment and a test unless a recovery episode is actually running.
    wake_note_present(self, hr);
    return hr;
}

// --- sleep/resume recovery: the d3d8 half of wakerecover.cpp -----------------
//
// Two callbacks and an installer. Everything API-specific about the recovery is
// here; the policy (when to reset, how long to wait for the title, when to give
// up) is in wakerecover.cpp so the d3d8 and d3d9 paths cannot drift apart.

// Both call through orig_*, never through the live vtable slot: the slot holds
// OUR hook, and re-entering it would apply the windowed rewrite a second time
// and re-notify the pump from inside itself. When a slot was never patched
// (validation failed) there is no orig_ and the answer is "not available",
// which wakerecover.cpp reports rather than guesses around.
static HRESULT d3d8_wake_test(void* dev)
{
    if (!orig_TestCoop) return E_NOTIMPL;
    return orig_TestCoop(dev);
}

static HRESULT d3d8_wake_reset(void* dev)
{
    if (!orig_Reset || !g_pp_have) return E_NOTIMPL;
    // A COPY: Reset is documented to be able to write back into the structure,
    // and g_pp_last must keep describing the device for the next attempt.
    D3D8_PRESENT_PARAMETERS pp = g_pp_last;
    HRESULT hr = orig_Reset(dev, &pp);
    if (SUCCEEDED(hr)) {
        d3d_note_device_mode(pp.Windowed);
        // A Reset is exactly when the Viewer puts its mask back up, and the
        // title's window has just been rebuilt underneath it -- the same
        // re-alignment the ordinary Reset path does.
        if (g_d3d_unmask) schedule_mask_align();
    }
    return hr;
}

static void wake_arm_d3d8(void* dev, HWND wnd)
{
    if (!dev || !wake_enabled()) return;
    // Present is the pump's main entry point. renderspy may already own the
    // slot; sharing it is the point of the check, not an accident.
    if (!orig_Present) {
        if (patch_slot(*(void***)dev, SD_Present, (void*)hook_Present,
                       (void**)&orig_Present))
            logf("[wake] Present hooked at device vtable slot %d for the "
                 "sleep/resume recovery (orig=%p)", SD_Present, orig_Present);
        else {
            orig_Present = NULL;
            logf("[wake] could NOT hook Present -- the recovery will only be "
                 "driven from TestCooperativeLevel, which a title that stops "
                 "polling while lost never calls");
        }
    }
    wake_register_device("d3d8", dev,
                         orig_TestCoop ? d3d8_wake_test : NULL,
                         (orig_Reset && g_pp_have) ? d3d8_wake_reset : NULL, wnd);
}

// --- render diagnostic hooks (read-only; d3d_renderspy) --------------------
// These answer one question: does the title draw its frame as pre-transformed
// 2D screen-space quads (D3DFVF_XYZRHW) or as real 3D geometry the pipeline
// transforms? That decides whether "render at higher resolution" is a viewport
// bump (3D, cheap, real gain) or a per-draw vertex-rescale (2D, invasive, and
// limited by the source textures). Nothing here alters a draw.
static void rs_count(void)
{
    LONG n = InterlockedIncrement(&g_rs_draw_total);
    DWORD f = g_rs_cur_fvf;
    if (f & 0x004)      InterlockedIncrement(&g_rs_draw_2d);     // XYZRHW = 2D screen space
    else if (f & 0x002) InterlockedIncrement(&g_rs_draw_3d);     // XYZ = transformed 3D
    else                InterlockedIncrement(&g_rs_draw_other);  // shader / unknown
    // These sessions are force-killed, so the clean summary rarely prints. Emit a
    // running tally periodically so a killed session still yields the verdict data.
    if (g_d3d_trace && (n % 4000) == 0)
        logf("[rs] tally @%ld draws: 2D/XYZRHW=%ld 3D/XYZ=%ld other=%ld | "
             "*UP=%ld | PROJ persp=%ld ortho=%ld",
             n, g_rs_draw_2d, g_rs_draw_3d, g_rs_draw_other,
             g_rs_up_calls, g_rs_proj_persp, g_rs_proj_ortho);
}

static HRESULT STDMETHODCALLTYPE hook_SetVertexShader(void* self, DWORD handle)
{
    // In fixed-function d3d8 the "vertex shader" handle IS the FVF code: small,
    // with position bits set. A compiled shader handle is larger / has no FVF
    // position bits, so we record it as unknown.
    if ((handle & 0xFFFF0000) == 0 && (handle & 0x00E)) g_rs_cur_fvf = handle;
    else                                                g_rs_cur_fvf = 0;
    return orig_SetVertexShader(self, handle);
}

static HRESULT STDMETHODCALLTYPE hook_SetViewport(void* self, const D3D8_VIEWPORT* vp)
{
    if (vp && g_d3d_trace && InterlockedIncrement(&g_rs_vp_logged) <= 8)
        logf("[rs] SetViewport (%lu,%lu %lux%lu) z=[%.2f,%.2f]",
             vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
    return orig_SetViewport(self, vp);
}

static HRESULT STDMETHODCALLTYPE hook_SetTransform(void* self, DWORD state, const float* m)
{
    // D3DTS_PROJECTION == 3. A perspective projection has _34 (m[11]) != 0 and
    // _44 (m[15]) == 0; an orthographic/2D one has _34 == 0, _44 == 1.
    if (state == 3 && m) {
        bool persp = (m[11] != 0.0f) && (m[15] == 0.0f);
        if (persp) InterlockedIncrement(&g_rs_proj_persp);
        else       InterlockedIncrement(&g_rs_proj_ortho);
        static LONG n = 0;
        if (g_d3d_trace && InterlockedIncrement(&n) <= 6)
            logf("[rs] SetTransform(PROJECTION) %s  _11=%.3f _22=%.3f _34=%.3f "
                 "_43=%.3f _44=%.3f", persp ? "PERSPECTIVE(3D)" : "ORTHO/2D",
                 m[0], m[5], m[11], m[14], m[15]);
    }
    return orig_SetTransform(self, state, m);
}

static HRESULT STDMETHODCALLTYPE hook_DrawPrimitive(void* self, DWORD t, UINT s, UINT c)
{ rs_count(); return orig_DrawPrimitive(self, t, s, c); }
static HRESULT STDMETHODCALLTYPE hook_DrawIndexedPrimitive(void* self, DWORD t, UINT mn,
                                                           UINT nm, UINT st, UINT pc)
{ rs_count(); return orig_DrawIndexedPrimitive(self, t, mn, nm, st, pc); }
static HRESULT STDMETHODCALLTYPE hook_DrawPrimitiveUP(void* self, DWORD t, UINT c,
                                                      const void* v, UINT stride)
{ InterlockedIncrement(&g_rs_up_calls); rs_count(); return orig_DrawPrimitiveUP(self, t, c, v, stride); }
static HRESULT STDMETHODCALLTYPE hook_DrawIndexedPrimitiveUP(void* self, DWORD t, UINT mnv,
        UINT nmv, UINT pc, const void* idx, DWORD idxf, const void* v, UINT stride)
{ InterlockedIncrement(&g_rs_up_calls); rs_count();
  return orig_DrawIndexedPrimitiveUP(self, t, mnv, nmv, pc, idx, idxf, v, stride); }

static void install_renderspy(void* dev)
{
    if (g_rs_hooked || !dev) return;
    g_rs_hooked = true;
    void** vt = *(void***)dev;
    struct { int slot; void* hook; void** orig; const char* name; } S[] = {
        { SD_BeginScene,             (void*)hook_BeginScene,            (void**)&orig_BeginScene,            "BeginScene" },
        { SD_Present,                (void*)hook_Present,               (void**)&orig_Present,               "Present" },
        { SD_DrawPrimitive,          (void*)hook_DrawPrimitive,         (void**)&orig_DrawPrimitive,         "DrawPrimitive" },
        { SD_DrawIndexedPrimitive,   (void*)hook_DrawIndexedPrimitive,  (void**)&orig_DrawIndexedPrimitive,  "DrawIndexedPrimitive" },
        { SD_DrawPrimitiveUP,        (void*)hook_DrawPrimitiveUP,       (void**)&orig_DrawPrimitiveUP,       "DrawPrimitiveUP" },
        { SD_DrawIndexedPrimitiveUP, (void*)hook_DrawIndexedPrimitiveUP,(void**)&orig_DrawIndexedPrimitiveUP,"DrawIndexedPrimitiveUP" },
        { SD_SetVertexShader,        (void*)hook_SetVertexShader,       (void**)&orig_SetVertexShader,       "SetVertexShader" },
        { SD_SetViewport,            (void*)hook_SetViewport,           (void**)&orig_SetViewport,           "SetViewport" },
        { SD_SetTransform,           (void*)hook_SetTransform,          (void**)&orig_SetTransform,          "SetTransform" },
    };
    // Slots are ordered {BeginScene, Present, the 4 Draw*, the 3 Set* state} so a
    // level can take a prefix:
    //   d3d_renderspy=2 == PRESENT-ONLY: BeginScene + Present only (the first two),
    //     no draw or state slots -- answers "frame loop running / beginning scenes"
    //     (present-black vs frozen) without touching the draw path.
    //   d3d_renderspy=3 == DRAW-COUNT: the first SIX -- BeginScene + Present + the
    //     4 Draw* slots, but NOT the 3 Set* state hooks. The [rs] tally then reports
    //     draws/frame, which separates "black because it issues 0 draws" (stuck,
    //     the TM shape) from "draws but the output is black" (wrong target). The
    //     Set* hooks are skipped because FE destabilised under the full 9-slot
    //     patch, and they only feed the 2D/3D FVF split (g_rs_cur_fvf), not the
    //     total -- so level 3 loses the split but keeps the count FE needs.
    //   d3d_renderspy=1 == the full FVF/draw-count spy, for titles that tolerate
    //     it (TM). FE may destabilise at this level.
    int total = (g_d3d_renderspy == 2) ? 2 :
                (g_d3d_renderspy == 3) ? 6 : (int)(sizeof(S)/sizeof(S[0]));
    int ok = 0;
    for (int i = 0; i < total; i++)
        // Already ours (the sleep/resume recovery shares the Present slot).
        // Patching a second time would store OUR hook as "the original" and the
        // hook would call itself until the stack ran out -- the exact shape of
        // the gamestart re-arm crash.
        if (*S[i].orig) ok++;
        else if (patch_slot(vt, S[i].slot, S[i].hook, S[i].orig)) ok++;
        else logf("[rs] FAILED to hook %s (slot %d)", S[i].name, S[i].slot);
    logf("[rs] renderspy installed: %d/%d slots hooked (%s) -- read-only", ok, total,
         g_d3d_renderspy == 2 ? "PRESENT-ONLY: BeginScene+Present" :
         g_d3d_renderspy == 3 ? "DRAW-COUNT: BeginScene+Present+Draw* (no state hooks)" :
         "full draw-path");
}

static void* WINAPI hook_Direct3DCreate8(UINT sdk)
{
    void* d3d8 = real_Direct3DCreate8(sdk);
    logf("[d3d] Direct3DCreate8(SDK=%u) -> %p", sdk, d3d8);
    // d3d8.dll is now definitely resident, so this is the first moment the question
    // "wined3d or DXVK?" can be answered. Once per process; read-only. See
    // protondxvk.cpp -- the answer decides this Deck's framerate and went unnoticed
    // for eleven days for want of exactly this line.
    protondxvk_report_d3d8();
    if (!d3d8) return d3d8;

    // Idempotent, but VERIFIED rather than assumed. d3d8's IDirect3D8 vtable is
    // shared, so one patch does cover every later Direct3DCreate8 in the process
    // -- for exactly as long as the patch is still in the slot. The old test was
    //
    //     if (orig_CreateDevice) return d3d8;      // "we armed this already"
    //
    // which answers a DIFFERENT question, and the two answers come apart the
    // moment anything else writes the slot.
    //
    // IMPORTANT: MEASURED 2026-09-09 on the reference Windows install, 4/4 relaunches:
    // the first d3d8 title launch of a Viewer session arms this and works; on the
    // SECOND launch of a d3d8 title in the same pol.exe the slot is no longer
    // ours, the flag still says "armed", so we never re-armed and
    // hook_CreateDevice silently stopped running for the rest of the session.
    // Tetra Master then dies inside its own InitDX and shows
    // "起動できませんでした。終了します。" ("could not start up, exiting"),
    // because TM asks for EXCLUSIVE FULLSCREEN 640x480 with a NULL hwnd and
    // BackBufferCount 0 and only ever starts because this hook rewrites that
    // call. Logs: polshim.410696.log, polshim.408476.log (twice),
    // polshim.404512.log -- in each, the working launch has
    // "[d3d] CreateDevice(adapter=..." and the failing one has no [d3d] line at
    // all after Direct3DCreate8.
    //
    // KEY: The control case is in polshim.404512.log: Fantasy Earth launched after
    // TM, was un-hooked in exactly the same way, and ran fine -- it selects
    // windowed itself with -windowmode and needs no rewrite. So it is the missing
    // REWRITE that kills TM and FMO, not the relaunch as such.
    //
    // WARNING: WHO clears the slot is NOT established. Ashita is the suspect (it is
    // loaded process-wide and wraps Direct3D 8; the title boundary names
    // Ashita.dll on every TM launch), but that is a hypothesis, not a
    // measurement -- which is why the re-arm below PRINTS the current occupant.
    // If it reports the same address we captured as `orig`, someone restored the
    // genuine pointer; if it reports another module, that module has taken the
    // slot; if it reports unmapped, it was hooked by something since unloaded.
    void** vt  = *(void***)d3d8;
    void*  cur = vt[S_CreateDevice];
    if (cur == (void*)hook_CreateDevice) return d3d8;      // still ours

    const bool rearm = (orig_CreateDevice != NULL);
    if (rearm) {
        char who[200];
        dx_describe_code(cur, who, sizeof(who));
        logf("[d3d] CreateDevice slot %d is NOT ours any more: it holds %s "
             "(we wrote hook=%p over orig=%p). Something took it during a title "
             "teardown. RE-ARMING over what is there now -- without this the "
             "forced-windowed rewrite stops happening and a fullscreen-only "
             "title fails to start on its second launch.",
             S_CreateDevice, who, (void*)hook_CreateDevice, orig_CreateDevice);
    }

    if (!validate_d3d8(d3d8)) {
        logf("[d3d] NOT patching CreateDevice -- validation failed (see above). "
             "Tracing is inert; the client runs exactly as it would unhooked.");
        return d3d8;
    }
    // CAS, not a plain write: `cur` is what we are chaining to, so the exchange
    // must fail rather than capture something else if the slot moved under us.
    if (patch_slot_cas(vt, S_CreateDevice, (void*)hook_CreateDevice, cur,
                       (void**)&orig_CreateDevice)) {
        logf("[d3d] CreateDevice %s at vtable slot %d (orig=%p) -- covers "
             "TM.dll, FFXiMain.dll and FE_Client.dll, which share this vtable",
             rearm ? "RE-ARMED" : "hooked", S_CreateDevice, orig_CreateDevice);
    } else {
        // orig_CreateDevice is untouched on failure, so a re-arm that loses the
        // race keeps the original it already had rather than being disarmed.
        logf("[d3d] could not arm the d3d8 CreateDevice slot (VirtualProtect "
             "failed, or another thread wrote it first) -- %s",
             rearm ? "the previous arming stands" : "not hooked");
    }
    return d3d8;
}

void* d3d8_real_Direct3DCreate8() { return (void*)real_Direct3DCreate8; }
void* d3d8_hook_Direct3DCreate8() { return (void*)hook_Direct3DCreate8; }

// ---------------------------------------------------------------------------
// cursor translation
// ---------------------------------------------------------------------------
//
// A game built only for exclusive fullscreen believes its client area starts at
// screen (0,0): in fullscreen that is true, so it clips and positions the cursor
// in raw screen coordinates and never converts anything. Put that same game in a
// window at (355,124) and every cursor call is off by the window origin --
// ClipCursor confines the pointer to a rectangle that is not the game, and
// SetCursorPos snaps it somewhere outside. Which is exactly "it aggressively
// steals my mouse and snaps to points that are never accurate".
//
// The correction is a pure offset, because fit_window() already made the client
// area exactly the backbuffer size, so there is no scaling term:
//
//     the game thinks in   (0,0)..(bw,bh)
//     the screen really is (O)..(O+bw,O+bh)     O = client origin, in screen px
//
// Guarded on the game window being FOREGROUND so the Viewer's own UI is never
// affected. [dx] d3d_cursor: 0 off, 1 log only, 2 log + translate.

typedef BOOL    (WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL    (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL    (WINAPI *PFN_ClipCursor)(const RECT*);
typedef HCURSOR (WINAPI *PFN_SetCursor)(HCURSOR);
typedef int     (WINAPI *PFN_ShowCursor)(BOOL);
typedef HWND    (WINAPI *PFN_SetCapture)(HWND);
typedef BOOL    (WINAPI *PFN_ReleaseCapture)(void);

static PFN_SetCursorPos real_SetCursorPos = NULL;
static PFN_GetCursorPos real_GetCursorPos = NULL;
static PFN_ClipCursor   real_ClipCursor   = NULL;
static PFN_SetCursor    real_SetCursor    = NULL;
static PFN_ShowCursor   real_ShowCursor   = NULL;
static PFN_SetCapture     real_SetCapture     = NULL;
static PFN_ReleaseCapture real_ReleaseCapture = NULL;

static LONG g_n_setpos = 0, g_n_clip = 0, g_n_getpos = 0, g_n_xlated = 0;
static LONG g_n_nullcur = 0;    // SetCursor(NULL) calls -- the SHAPE half of an
                                // invisible pointer, see hook_SetCursor.
static LONG g_n_setcap = 0, g_n_relcap = 0;

// Screen coordinates of the game window's client (0,0), AND the client-to-
// backbuffer scale. Returns false when translation should not apply.
//
// THE SCALE IS NOT OPTIONAL, and leaving it out was a real bug. This translation
// exists to hand the client a coordinate in the space its 640x480 backbuffer
// lives in. While fit_window sized the client to exactly the backbuffer, client
// pixels WERE backbuffer pixels and subtracting the origin was the whole job. The
// moment the window opens at 2x (d3d_scale), that identity is gone and a bare
// offset hands over a coordinate twice as large as the field it is read against
// -- the pointer then reaches the right-hand edge of the game at the middle of
// the window. Measured as a regression the day d3d_scale=auto landed.
static bool client_map(POINT* o, int* cw, int* ch, int* bbw, int* bbh)
{
    if (g_d3d_cursor < 2) return false;
    HWND h = g_game_window;
    if (!h || !IsWindow(h)) return false;
    if (GetForegroundWindow() != h) return false;   // never touch the Viewer's UI
    RECT c;
    if (!GetClientRect(h, &c) || c.right <= 0 || c.bottom <= 0) return false;
    int bw = 0, bh = 0;
    d3d8_backbuffer_size(&bw, &bh);
    if (bw <= 0 || bh <= 0) { bw = c.right; bh = c.bottom; }   // unknown yet: 1:1
    *cw = c.right; *ch = c.bottom; *bbw = bw; *bbh = bh;
    o->x = 0; o->y = 0;
    return ClientToScreen(h, o) != 0;
}

static BOOL WINAPI hook_SetCursorPos(int x, int y)
{
    InterlockedIncrement(&g_n_setpos);
    // Free-cursor mode: the game parks the OS pointer at a fixed point every
    // frame (Tetra Master warps to (30,30)), which reads as the cursor being
    // captured -- fatal over remote desktop, where there is no Alt+Tab to break
    // out. Swallow the warp entirely and report success; the game's own cursor
    // is driven from DirectInput deltas, so it does not depend on this call.
    //
    // IMPORTANT: NOT ONLY WHILE THE GAME IS FOREGROUND (2026-08-26, FMO, reference Windows install, build 151). The
    // suppression used to require GetForegroundWindow() == the game, so the moment
    // the player clicked into another application FMO's per-frame SetCursorPos
    // (200,200) fell through to the translate/pass-through paths below and the OS
    // pointer was dragged back every frame: "it stole my mouse and aggressively
    // kept it in place". An unfocused title warping the pointer is the one case
    // that must NEVER pass -- that is the whole reason free-cursor exists. Swallow
    // it whenever the game window exists; the input gate (inputgate.cpp) already
    // feeds the title a frozen position while it is not in front, so the title's
    // own cursor logic stays consistent without the warp.
    if (g_d3d_freecursor && g_game_window && IsWindow(g_game_window)) {
        bool fg = GetForegroundWindow() == g_game_window;
        if (g_d3d_trace && (g_n_setpos < 6 || (g_n_setpos % 600) == 0))
            logf("[cur] SetCursorPos(%d,%d) SUPPRESSED (freecursor%s)", x, y,
                 fg ? "" : "; game NOT foreground -- would have dragged the pointer");
        return TRUE;
    }
    POINT o; int cw, ch, bw, bh;
    if (!InterlockedCompareExchange(&g_in_modal, 0, 0) && client_map(&o, &cw, &ch, &bw, &bh)) {
        // Backbuffer space -> client pixels -> screen. The inverse of the read
        // path below, scale included.
        int sx = MulDiv(x, cw, bw) + o.x;
        int sy = MulDiv(y, ch, bh) + o.y;
        InterlockedIncrement(&g_n_xlated);
        if (g_d3d_trace && (g_n_setpos < 12 || (g_n_setpos % 240) == 0))
            logf("[cur] SetCursorPos(%d,%d) -> screen (%d,%d)  [client %dx%d, "
                 "backbuffer %dx%d, origin %d,%d]",
                 x, y, sx, sy, cw, ch, bw, bh, o.x, o.y);
        return real_SetCursorPos(sx, sy);
    }
    if (g_d3d_trace && g_n_setpos < 12)
        logf("[cur] SetCursorPos(%d,%d) passthrough", x, y);
    return real_SetCursorPos(x, y);
}

// Set when WE applied a clip. A game written for exclusive fullscreen confines
// the pointer and never expects to lose focus, so it never releases -- which is
// why the mouse stays trapped and alt-tab feels impossible. GetCursorPos runs
// constantly, so it is the cheapest place to notice focus has left and let go.
static LONG g_clip_applied = 0;

static void release_clip_if_unfocused()
{
    if (!InterlockedCompareExchange(&g_clip_applied, 0, 0)) return;
    HWND h = g_game_window;
    if (h && IsWindow(h) && GetForegroundWindow() == h) return;
    InterlockedExchange(&g_clip_applied, 0);
    if (real_ClipCursor) real_ClipCursor(NULL);
    logf("[cur] game lost foreground -- cursor clip released");
}

// SEAM 1 of the focus gate (inputgate.cpp): the shell's own cursor poll.
//
// app.dll reads the pointer with GetCursorPos and converts it against the input
// window ([[viewer-owns-the-mouse]]) -- every title's pointer comes through here,
// and GetCursorPos does not care which application is in front. FMO's camera is
// driven from the CHANGE in this value, so while you are in another window the
// only thing that keeps the character still is returning the SAME point.
//
// Frozen, not zeroed: (0,0) is a real screen position and would read as an
// enormous mouse movement -- the camera would snap, which is worse than drift.
static POINT g_gate_frozen = { 0, 0 };
static bool  g_gate_frozen_ok = false;
static bool  g_gate_prev_cursor = false;

static BOOL WINAPI hook_GetCursorPos(LPPOINT p)
{
    InterlockedIncrement(&g_n_getpos);
    release_clip_if_unfocused();
    BOOL r = real_GetCursorPos(p);

    if (r && p) {
        bool blocked = false;
        if (inputgate_edge(&g_gate_prev_cursor, &blocked))
            logf("[gate] another application took the foreground -- the cursor is "
                 "frozen at (%ld,%ld) for the title until you come back "
                 "([dx] mouse_focus_gate=0 to stop)",
                 g_gate_frozen_ok ? g_gate_frozen.x : p->x,
                 g_gate_frozen_ok ? g_gate_frozen.y : p->y);
        if (blocked) {
            // Hold the last position seen while we WERE in front. Before the
            // first such sample there is nothing to hold, so let the real one
            // through -- a frozen (0,0) at startup would be a worse lie.
            if (g_gate_frozen_ok) *p = g_gate_frozen;
            inputgate_note_blocked(0);
        } else {
            g_gate_frozen = *p;
            g_gate_frozen_ok = true;
        }
    }
    POINT o; int cw, ch, bw, bh;
    if (r && p && !InterlockedCompareExchange(&g_in_modal, 0, 0) && client_map(&o, &cw, &ch, &bw, &bh)) {
        // Screen -> client pixels -> BACKBUFFER space. The second step is the one
        // that was missing; without it a 2x window reports every coordinate at
        // twice its true value in the field the client reads it against.
        LONG lx = p->x - o.x, ly = p->y - o.y;
        p->x = MulDiv(lx, bw, cw);
        p->y = MulDiv(ly, bh, ch);
        if (g_d3d_trace && (g_n_getpos < 12 || (g_n_getpos % 600) == 0))
            logf("[cur] GetCursorPos -> (%ld,%ld) [client (%ld,%ld) of %dx%d "
                 "-> backbuffer %dx%d, origin %d,%d]",
                 p->x, p->y, lx, ly, cw, ch, bw, bh, o.x, o.y);
    }
    return r;
}

static BOOL WINAPI hook_ClipCursor(const RECT* rc)
{
    InterlockedIncrement(&g_n_clip);
    // Free-cursor mode: never let the game confine the pointer. Release any
    // existing clip and report success. (Measured: Tetra Master does not call
    // this -- its capture is SetCursorPos -- but a sibling title might, and the
    // promise of this mode is "the pointer is never trapped".)
    if (g_d3d_freecursor) {
        if (real_ClipCursor) real_ClipCursor(NULL);
        InterlockedExchange(&g_clip_applied, 0);
        if (g_d3d_trace && g_n_clip < 6)
            logf("[cur] ClipCursor SUPPRESSED (freecursor)");
        return TRUE;
    }
    POINT o; int cw, ch, bw, bh;
    if (rc && client_map(&o, &cw, &ch, &bw, &bh)) {
        // Same backbuffer -> screen mapping as SetCursorPos: a confinement rect
        // given in the game's field has to be scaled, not just shifted, or a 2x
        // window pens the pointer into the top-left quarter of the game.
        RECT r;
        r.left   = MulDiv(rc->left,   cw, bw) + o.x;
        r.right  = MulDiv(rc->right,  cw, bw) + o.x;
        r.top    = MulDiv(rc->top,    ch, bh) + o.y;
        r.bottom = MulDiv(rc->bottom, ch, bh) + o.y;
        InterlockedIncrement(&g_n_xlated);
        InterlockedExchange(&g_clip_applied, 1);
        logf("[cur] ClipCursor(%ld,%ld,%ld,%ld) -> (%ld,%ld,%ld,%ld)",
             rc->left, rc->top, rc->right, rc->bottom,
             r.left, r.top, r.right, r.bottom);
        return real_ClipCursor(&r);
    }
    if (!rc) InterlockedExchange(&g_clip_applied, 0);
    if (g_d3d_trace)
        logf("[cur] ClipCursor(%s) passthrough",
             rc ? "rect" : "NULL -- released");
    return real_ClipCursor(rc);
}

// --- MOUSE CAPTURE: who takes it, who gives it back ------------------------
//
// MEASURED 2026-08-24, on a live FMO window that had gone inert after one drag
// (an external GetGUIThreadInfo probe):
//
//     style   = 0x14CF0000  [CAPTION SYSMENU SIZEBOX MINIMIZEBOX MAXIMIZEBOX]
//     enabled = True        hittest at the caption = 2 (CAPTION)
//     capture = 0x012C0BA6  class=FMOClass          <-- the game window itself
//     movesz  = none        flags = 0               <-- NOT in a modal loop
//
// The window is perfectly healthy and the frame still hit-tests as a caption.
// The one abnormal fact is that the window holds the MOUSE CAPTURE with no
// button down. That is sufficient on its own: while a window has captured the
// mouse, Windows delivers all mouse input to it as CLIENT messages and performs
// no non-client hit-testing for it at all. So a click on the title bar never
// becomes WM_NCLBUTTONDOWN, never becomes SC_MOVE, and no drag or size loop can
// start -- while the client area keeps working normally. That is exactly the
// report: "I can drag it once, then it is inert."
//
// What is NOT yet known is WHO leaves it held, which is what these two hooks
// answer. They are pure instrumentation -- they log the caller's module and
// return address and pass every call straight through. Do NOT add a "release it
// for them" here before the writer is identified: a forced ReleaseCapture would
// be a debug lever, not a fix, and would paper over whichever code path is
// actually dropping its own release.
static void log_capture_caller(const char* what, HWND h, LONG n, void* ret)
{
    char leaf[64] = "?";
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ret, &m) && m) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* p = strrchr(path, '\\');
        strncpy_s(leaf, sizeof(leaf), p ? p + 1 : path, _TRUNCATE);
        // Offset within the module, so the call site can be looked up in a
        // disassembly directly rather than against a randomised load address.
        logf("[cap] %-14s hwnd=%p  by %s+0x%X  [call #%ld]  live capture now=%p",
             what, h, leaf, (unsigned)((BYTE*)ret - (BYTE*)m), n, GetCapture());
    } else {
        logf("[cap] %-14s hwnd=%p  by ret=%p (no module)  [call #%ld]  "
             "live capture now=%p", what, h, ret, n, GetCapture());
    }
}

static HWND WINAPI hook_SetCapture(HWND h)
{
    InterlockedIncrement(&g_n_setcap);
    // Capture calls are rare (a handful per drag), so log them ALL rather than
    // sampling -- the whole question is which one has no matching release.
    log_capture_caller("SetCapture", h, g_n_setcap, _ReturnAddress());
    return real_SetCapture(h);
}

static BOOL WINAPI hook_ReleaseCapture(void)
{
    InterlockedIncrement(&g_n_relcap);
    log_capture_caller("ReleaseCapture", GetCapture(), g_n_relcap, _ReturnAddress());
    return real_ReleaseCapture();
}

// Show the STANDARD system cursor over the window frame. The game re-sets (and
// over the client, hides) its cursor every frame, which overrides the resize/
// arrow cursors DefWindowProc sets on WM_SETCURSOR -- so the borders gave no
// visual "you can size here" feedback even though sizing worked. Here we let the
// game's SetCursor stand over the CLIENT area (it draws its own pointer there),
// but over any non-client region we substitute the matching system cursor.
static HCURSOR WINAPI hook_SetCursor(HCURSOR c)
{
    // The OTHER way a pointer disappears, and the one a count reading cannot
    // distinguish: SetCursor(NULL) draws nothing whatever the display count says.
    // GetCursorInfo reports hCursor=NULL for a HIDDEN cursor too, so an external
    // probe cannot tell these apart -- only the call can. Rate-limited; a title
    // that sets a shape every frame must not drown the log.
    if (!c) {
        LONG k = InterlockedIncrement(&g_n_nullcur);
        if (k <= 8 || (k % 512) == 0) {
            char leaf[64] = "?";
            HMODULE m = NULL;
            void* ret = _ReturnAddress();
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ret, &m) && m) {
                char path[MAX_PATH] = "";
                GetModuleFileNameA(m, path, MAX_PATH);
                const char* p = strrchr(path, '\\');
                strncpy_s(leaf, sizeof(leaf), p ? p + 1 : path, _TRUNCATE);
                logf("[cur] SetCursor(NULL) by %s+0x%X  [call #%ld] -- a NULL SHAPE "
                     "draws nothing even at a non-negative display count",
                     leaf, (unsigned)((BYTE*)ret - (BYTE*)m), k);
            }
        }
    }
    HWND gw = g_game_window;
    if (g_d3d_cursor && gw && IsWindow(gw) && GetForegroundWindow() == gw) {
        POINT p;
        BOOL ok = real_GetCursorPos ? real_GetCursorPos(&p) : GetCursorPos(&p);
        if (ok) {
            LRESULT ht = SendMessageA(gw, WM_NCHITTEST, 0,
                                      MAKELPARAM((WORD)p.x, (WORD)p.y));
            LPCSTR id = NULL;
            switch (ht) {
                case HTLEFT:      case HTRIGHT:       id = IDC_SIZEWE;   break;
                case HTTOP:       case HTBOTTOM:      id = IDC_SIZENS;   break;
                case HTTOPLEFT:   case HTBOTTOMRIGHT: id = IDC_SIZENWSE; break;
                case HTTOPRIGHT:  case HTBOTTOMLEFT:  id = IDC_SIZENESW; break;
                case HTCAPTION:   case HTSYSMENU:     case HTMENU:
                case HTMINBUTTON: case HTMAXBUTTON:   case HTCLOSE:
                    id = IDC_ARROW; break;
            }
            if (id) return real_SetCursor(LoadCursorA(NULL, id));
        }
    }
    return real_SetCursor(c);
}

// --- WHO HIDES THE POINTER OVER FANTASY EARTH ------------------------------
//
// MEASURED 2026-08-25 with an external cursor probe, while FE sat with no pointer
// over its client area:
//
//     window under it = 0x009717B6 class=MainWindow   Fantasy Earth
//     visible  = HIDDEN  (display count is negative)
//     class cur= 0x71250591 (not a standard shape -- the title's own)
//
// So it is the COUNT, not the shape -- and the shape it is failing to draw is
// FE's OWN cursor, registered on FE's own window class. Fantasy Earth is the one
// title that drives the OS pointer itself (FE_Client.dll imports SetCursor,
// SetCursorPos, GetCursorPos, ShowCursor, ClipCursor, LoadCursorA and
// RegisterClassExA; TM.dll's entire user32 surface is CallNextHookEx), so its
// in-game pointer IS the Windows cursor and a negative display count erases it.
//
// The reported repro is a STATE TRANSITION, not a machine: fresh launch shows
// the pointer, minimise+restore brings it back, and mousing away from the window
// and returning loses it. The shim's own nc_cursor_show/restore pair is NOT the
// cause -- it never logged a single raise in that session, so it never took a
// boost and never gave one back.
//
// That leaves an unbalanced ShowCursor(FALSE) somewhere in pol.exe or FE itself,
// riding the activation cycle. This hook answers WHICH, the same way the [cap]
// hooks answered the capture question: it logs the caller's module and offset and
// passes every call straight through. WARNING: Do NOT "fix" this by forcing the count
// back up before the writer is named -- that is a debug lever, not a fix, and it
// would hide whichever call is missing its partner.
//
// Volume: every call is logged while the picture is being built, and after that
// only the calls that CROSS ZERO -- the transitions between a drawn and an
// undrawn pointer, which are the only ones that change what the user sees.
static LONG g_n_showcur = 0;
static LONG g_cur_count = 0;      // last count user32 reported (starts at 0 = visible)
static LONG g_n_nearzero = 0;     // calls logged from the count<=1 region

static int WINAPI hook_ShowCursor(BOOL show)
{
    int n = real_ShowCursor(show);
    LONG k = InterlockedIncrement(&g_n_showcur);
    LONG was = InterlockedExchange(&g_cur_count, n);
    // A crossing in either direction is what the user actually sees change.
    const char* edge = "";
    if (was >= 0 && n < 0) edge = "   <-- POINTER NOW INVISIBLE (count went negative)";
    else if (was < 0 && n >= 0) edge = "   <-- pointer visible again";

    // WARNING: CROSSINGS ALONE ARE NOT ENOUGH -- learned 2026-08-25, first run with
    // this hook. The log jumped from call #24 at count=+2 straight to call #404 at
    // count=-1: ~380 calls walked the count from +2 down through zero and NONE of
    // them was a crossing, so the whole descent -- the only part that explains WHY
    // it ended up negative -- was invisible. A "log the transitions" instrument
    // hides the drift that produces them.
    //
    // So: also log every call that lands in the region where visibility is
    // decided (count <= 1), which is where the interesting traffic is and where
    // the Viewer's idle +1/+2 toggling is not. Full detail for the first 200 of
    // those, then 1-in-64 so a title that toggles per frame cannot flood the log.
    bool near_zero = (n <= 1);
    if (!*edge && k > 24) {
        if (!near_zero) return n;
        LONG nz = InterlockedIncrement(&g_n_nearzero);
        if (nz > 200 && (nz % 64) != 0) return n;
    }

    char leaf[64] = "?";
    HMODULE m = NULL;
    void* ret = _ReturnAddress();
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ret, &m) && m) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* p = strrchr(path, '\\');
        strncpy_s(leaf, sizeof(leaf), p ? p + 1 : path, _TRUNCATE);
        logf("[cur] ShowCursor(%s) by %s+0x%X -> count=%d  [call #%ld]%s",
             show ? "TRUE " : "FALSE", leaf,
             (unsigned)((BYTE*)ret - (BYTE*)m), n, k, edge);
    } else {
        logf("[cur] ShowCursor(%s) by ret=%p (no module) -> count=%d  [call #%ld]%s",
             show ? "TRUE " : "FALSE", ret, n, k, edge);
    }
    return n;
}

// --- message spy ------------------------------------------------------------
//
// Every theory about the pointer so far has been wrong, each killed by a
// measurement: DirectDraw, ChangeDisplaySettings, DirectInput exclusivity, the
// user32 cursor APIs, and now the mask alignment. Time to stop reasoning about
// where the coordinates come from and just watch the messages.
//
// This subclasses the GAME window and the MASK window and logs the mouse
// messages each one receives, with the raw lParam coordinates. That settles it
// in one run:
//
//   * game window gets WM_MOUSEMOVE with 0..639 / 0..479   -> the coordinates
//     are already correct and the game is mangling them internally
//   * MASK window gets the moves, with screen-sized values -> that is the
//     mismatch, and the fix belongs at that window
//   * neither gets them -> the pointer comes from polcore's input layer and the
//     answer is in its common function table, not in messages at all
//
// Read-only: it logs and forwards. Never alters a message.

struct SpyEntry { HWND h; WNDPROC orig; char tag[16]; LONG seen; LONG frame_seen; };
// Eight, not four. Four was exactly the number a session of two titles fills
// (one GAME + one MASK each), so the THIRD title was already running on a full
// table -- see install_msgspy, which now reaps dead entries and, when it really
// cannot watch a window, says so instead of returning silently.
static SpyEntry g_spy[8];
static int      g_nspy = 0;

static const char* mouse_msg_name(UINT m)
{
    switch (m) {
        case WM_MOUSEMOVE:   return "MOUSEMOVE";
        case WM_LBUTTONDOWN: return "LBUTTONDOWN";
        case WM_LBUTTONUP:   return "LBUTTONUP";
        case WM_RBUTTONDOWN: return "RBUTTONDOWN";
        case WM_MOUSEWHEEL:  return "MOUSEWHEEL";
        case WM_SETCURSOR:   return "SETCURSOR";
        default: return NULL;
    }
}

// Messages that decide whether a window can be dragged, resized or closed.
// A fullscreen-only game usually swallows or mishandles these, because in
// exclusive mode there is no frame to drag and no X to click.
static const char* frame_msg_name(UINT m)
{
    switch (m) {
        case WM_NCHITTEST:    return "NCHITTEST";
        case WM_NCLBUTTONDOWN:return "NCLBUTTONDOWN";
        // NCLBUTTONDBLCLK (double-click the caption to maximise) and NCRBUTTONUP
        // (right-click it for the system menu) were ALREADY handed to DefWindowProc
        // below -- but that handler sits inside `if (fm && ...)`, and neither
        // message was named here, so `fm` was NULL and the branch could never run.
        // Both routes were dead: a caption double-click and a caption right-click
        // did nothing at all, which is part of what "the frame is inert" looks like.
        case WM_NCLBUTTONDBLCLK: return "NCLBUTTONDBLCLK";
        case WM_NCRBUTTONUP:  return "NCRBUTTONUP";
        case WM_SYSCOMMAND:   return "SYSCOMMAND";
        case WM_CLOSE:        return "CLOSE";
        case WM_DESTROY:      return "DESTROY";
        case WM_ENTERSIZEMOVE:return "ENTERSIZEMOVE";
        case WM_EXITSIZEMOVE: return "EXITSIZEMOVE";
        // Capture handovers: lParam is the window RECEIVING the capture (NULL =
        // it was released). Paired with the [cap] hooks this gives both halves --
        // who asked, and what the window itself was told happened.
        case WM_CAPTURECHANGED: return "CAPTURECHANGED";
        case WM_MOVE:         return "MOVE";
        case WM_WINDOWPOSCHANGING: return "WINDOWPOSCHANGING";
        case WM_WINDOWPOSCHANGED:  return "WINDOWPOSCHANGED";
        case WM_ACTIVATE:     return "ACTIVATE";
        case WM_ACTIVATEAPP:  return "ACTIVATEAPP";
        default: return NULL;
    }
}

static bool would_steal(HWND target);      // defined with the focus hooks below

// ---------------------------------------------------------------------------
// THE POINTER IS INVISIBLE OVER THE WINDOW FRAME
// ---------------------------------------------------------------------------
//
// A fullscreen-only title hides the OS pointer once, at startup, because it
// draws its own -- and it hides it with ShowCursor(FALSE), which decrements a
// per-queue DISPLAY COUNT. While that count is negative the cursor is invisible
// EVERYWHERE the process owns, and no amount of SetCursor will show it:
// SetCursor picks the SHAPE, the count decides whether a shape is drawn at all.
//
// Fullscreen that is correct. Windowed it is not, because the shim gave the
// title a FRAME the title knows nothing about: the caption, the borders and the
// buttons are ours, they are the only way to move, size or close the window, and
// the user cannot see the pointer while over any of them. Reported against Tetra
// Master 2026-08-24 ("hover the title bar and the mouse is still invisible").
//
// WARNING: NOT the same fault as hook_SetCursor above, and not fixed by it. That hook
// substitutes the correct system SHAPE over the non-client area, and it is
// installed (EAT-patched in d3d8_resolve, so it is live even though no IAT row
// names it). Shape was the half that was already handled; VISIBILITY is this
// half, and a negative display count defeats the other one entirely.
//
// Strictly balanced: whatever this adds over the frame it takes back the instant
// the pointer returns to the client area, so the title's own count is exactly as
// it left it and the game's in-client pointer is untouched.
static LONG g_nc_cursor_boost = 0;   // ShowCursor(TRUE) calls the shim owns

static void nc_cursor_show(void)
{
    if (g_nc_cursor_boost) return;                 // already raised by us
    // ASK WITHOUT TOUCHING. GetCursorInfo reports visibility as STATE; the old
    // probe below asked by raising the count and putting it back, which is a
    // read-modify-write on a value other threads are also driving. Any title that
    // tracks "is my cursor shown" in its own flag rather than reading the count
    // back can be desynchronised by that momentary +1, and Fantasy Earth is one
    // (see the gate in spy_proc). Nothing to do here when it is already visible,
    // so find that out without writing anything.
    CURSORINFO ci; ZeroMemory(&ci, sizeof(ci)); ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) return;
    // ShowCursor returns the NEW count. Probe with one raise: if that lands
    // above 0 the cursor was already visible and nothing here is needed, so put
    // the count straight back rather than leaving a boost to unwind. (Reached
    // only if GetCursorInfo failed -- kept as the fallback it now is.)
    int c = ShowCursor(TRUE);
    if (c > 0) { ShowCursor(FALSE); return; }
    int n = 1;
    while (c < 0 && n < 16) { c = ShowCursor(TRUE); n++; }   // raise to exactly 0
    g_nc_cursor_boost = n;
    static volatile LONG said = 0;
    if (say_once_per_title(&said))
        logf("[cur] pointer was hidden over the window FRAME (display count was "
             "negative -- the title hid it for fullscreen). Raised by %d while "
             "over the non-client area; restored on re-entering the client.", n);
}

static void nc_cursor_restore(void)
{
    while (g_nc_cursor_boost > 0) { ShowCursor(FALSE); g_nc_cursor_boost--; }
}

#define WM_POLSHIM_REFIT (WM_APP + 0x5F1)

static LRESULT CALLBACK spy_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = NULL;
    SpyEntry* e = NULL;
    for (int i = 0; i < g_nspy; i++)
        if (g_spy[i].h == h) { e = &g_spy[i]; orig = g_spy[i].orig; break; }

    if (e) {
        // THE LIVE DISPLAY SWITCH (d3d_display_toggle). Posted from the hotkey
        // watcher so the restyle and resize run on the window's OWN thread.
        if (msg == WM_POLSHIM_REFIT && e->h == g_game_window) {
            fit_window(h, g_bb_w, g_bb_h);
            return 0;
        }
        // POINTER VISIBILITY OVER THE FRAME. See nc_cursor_show above. The pair
        // of messages is exact: WM_MOUSEMOVE is delivered only over the CLIENT
        // area and WM_NCMOUSEMOVE only over the NON-CLIENT area, so together they
        // are an unambiguous "which side is the pointer on". WM_SETCURSOR carries
        // the hit-test as well and is handled for the case where the title
        // swallows the move messages.
        //
        // The restores are deliberately generous -- deactivating, losing focus or
        // leaving the window all give the boost back. Without WM_NCMOUSELEAVE
        // (which needs TrackMouseEvent) a pointer flicked from the caption
        // straight off the window would otherwise never see a client move, and
        // the boost would linger and draw the OS pointer on top of the game's own.
        // IMPORTANT: NOT FOR A TITLE THAT OWNS ITS OWN POINTER (added 2026-08-25).
        //
        // This whole feature exists because the shim gives a title a FRAME the
        // title knows nothing about. A NATIVE-WINDOWED title has SE's own frame
        // (FE, via -windowmode) and drives the OS cursor itself -- FE_Client.dll
        // imports LoadCursorA/SetCursor/ShowCursor/SetCursorPos/ClipCursor and
        // registers its own window class with its own shape -- so by this
        // feature's own rationale it does not apply, and applying it BREAKS FE.
        //
        // How it breaks: nc_cursor_show's probe path (`c = ShowCursor(TRUE); if
        // (c > 0) { ShowCursor(FALSE); return; }`) runs whenever the pointer is
        // currently VISIBLE -- which over FE is most of the time -- and it is a
        // read-modify-write on the process-global display count that FE is
        // concurrently driving from ITS OWN "is the cursor shown" flag. FE never
        // reads the count back, so perturbing it desynchronises FE's model: FE
        // believes it already showed the pointer and never re-issues, and the
        // pointer stays hidden -- including over menus, where you cannot see what
        // you are clicking. Reported as "cross the caption on the way out, come
        // back and it is gone; minimise+restore brings it back" (the restore
        // forces FE to re-evaluate). That probe path logs NOTHING, which is why
        // an earlier reading of the log wrongly cleared this code.
        //
        // Gated on the LATCH, not g_title_native_windowed: that flag is reset by a
        // later non-FE CreateDevice in the same session, which is exactly the bug
        // the b55 cursor-translation attempt hit before it moved to the latch.
        //
        // The SHAPE half below (WM_SETCURSOR over a non-client hit-test ->
        // DefWindowProc) is deliberately left alone: it cannot touch the count,
        // so it cannot desynchronise anything.
        if (e->h == g_game_window && g_d3d_cursor) {
            switch (msg) {
                case WM_NCMOUSEMOVE:
                    nc_cursor_show();
                    break;
                case WM_MOUSEMOVE:
                    nc_cursor_restore();
                    break;
                case WM_SETCURSOR:
                    if (LOWORD(lp) == HTCLIENT) nc_cursor_restore();
                    else                        nc_cursor_show();
                    break;
                case WM_NCMOUSELEAVE:
                case WM_MOUSELEAVE:
                case WM_KILLFOCUS:
                case WM_ACTIVATEAPP:
                    nc_cursor_restore();
                    break;
                case WM_ACTIVATE:
                    if (LOWORD(wp) == WA_INACTIVE) nc_cursor_restore();
                    break;
                default: break;
            }
        }

        // Track the minimised state off the ACTUAL size change, not the button
        // command (which the game's foreground spam can eat before it fires):
        // WM_SIZE(SIZE_MINIMIZED) sets the hold, any other size clears it. This
        // is the robust release too -- it fires however the restore happened.
        if (e->h == g_game_window && msg == WM_SIZE)
            InterlockedExchange(&g_user_minimized, wp == SIZE_MINIMIZED ? 1 : 0);
            // Maximise is a deliberate sizing action that never produces an
            // EXITSIZEMOVE (there is no drag loop), so it would otherwise be
            // forgotten and snapped back by the next Reset like any other.
            if (msg == WM_SIZE && wp == SIZE_MAXIMIZED && e->h == g_game_window)
                remember_window_size(e->h);
        // ASPECT LOCK, part 1 of 2: dragging a border.
        //
        // The backbuffer is 4:3 and D3D stretches it to whatever the client rect
        // is, so a 16:9 window silently shows the whole game stretched wide --
        // cards and faces included. Lock the drag to the backbuffer's ratio, the
        // same thing pol.exe already does for its own shell window (its WM_SIZING
        // case ratio-locks against 640x480). Corners and the top/bottom edges
        // drive the other axis; the edge the user is holding stays put.
        if (e->h == g_game_window && msg == WM_SIZING &&
            g_d3d_aspect && g_bb_w && g_bb_h && lp) {
            RECT* dr = (RECT*)lp;
            RECT fr = { 0, 0, 0, 0 };
            AdjustWindowRectEx(&fr, GetWindowLongA(h, GWL_STYLE), FALSE,
                               GetWindowLongA(h, GWL_EXSTYLE));
            int fw = (fr.right - fr.left), fh = (fr.bottom - fr.top);
            int cw = (dr->right - dr->left) - fw, ch = (dr->bottom - dr->top) - fh;
            if (cw > 0 && ch > 0) {
                if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM)
                    cw = MulDiv(ch, (int)g_bb_w, (int)g_bb_h);
                else
                    ch = MulDiv(cw, (int)g_bb_h, (int)g_bb_w);
                int nw = cw + fw, nh = ch + fh;
                if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT)
                    dr->left = dr->right - nw;
                else
                    dr->right = dr->left + nw;
                if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)
                    dr->top = dr->bottom - nh;
                else
                    dr->bottom = dr->top + nh;
                return TRUE;
            }
        }

        // ASPECT LOCK, part 2 of 2: MAXIMISE. A drag sends WM_SIZING; maximising
        // does not -- it asks WM_GETMINMAXINFO how big to be and then fills the
        // work area, which is exactly the 4:3-into-16:9 stretch again, and the
        // maximised window is how this is actually played. Answer with the
        // largest client that keeps the backbuffer's ratio, centred on the work
        // area. The result covers as much screen as it honestly can and leaves
        // the desktop showing at the sides rather than distorting the picture.
        if (e->h == g_game_window && msg == WM_GETMINMAXINFO &&
            g_d3d_aspect && g_bb_w && g_bb_h && lp) {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            RECT wa;
            HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi; mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoA(mon, &mi)) wa = mi.rcWork;
            else if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
                wa.left = wa.top = 0;
                wa.right  = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CXSCREEN)
                                                  : GetSystemMetrics(SM_CXSCREEN);
                wa.bottom = real_GetSystemMetrics ? real_GetSystemMetrics(SM_CYSCREEN)
                                                  : GetSystemMetrics(SM_CYSCREEN);
            }
            RECT fr = { 0, 0, 0, 0 };
            AdjustWindowRectEx(&fr, GetWindowLongA(h, GWL_STYLE), FALSE,
                               GetWindowLongA(h, GWL_EXSTYLE));
            int fw = (fr.right - fr.left), fh = (fr.bottom - fr.top);
            int availw = (wa.right - wa.left) - fw, availh = (wa.bottom - wa.top) - fh;
            if (availw > 0 && availh > 0) {
                int cw = availw, chh = MulDiv(cw, (int)g_bb_h, (int)g_bb_w);
                if (chh > availh) { chh = availh; cw = MulDiv(chh, (int)g_bb_w, (int)g_bb_h); }
                mm->ptMaxSize.x     = cw + fw;
                mm->ptMaxSize.y     = chh + fh;
                mm->ptMaxPosition.x = wa.left + ((wa.right - wa.left) - mm->ptMaxSize.x) / 2;
                mm->ptMaxPosition.y = wa.top  + ((wa.bottom - wa.top) - mm->ptMaxSize.y) / 2;
                // ptMaxTrackSize must not be smaller than ptMaxSize or the window
                // manager clamps the size back down and the centring is lost.
                if (mm->ptMaxTrackSize.x < mm->ptMaxSize.x) mm->ptMaxTrackSize.x = mm->ptMaxSize.x;
                if (mm->ptMaxTrackSize.y < mm->ptMaxSize.y) mm->ptMaxTrackSize.y = mm->ptMaxSize.y;
                return 0;
            }
        }

        // Hold the game's self-refocus off for the whole modal move/size loop, so
        // its per-frame SetForegroundWindow can't cancel the drag partway.
        if (e->h == g_game_window && msg == WM_ENTERSIZEMOVE)
            InterlockedExchange(&g_in_modal, 1);
        if (e->h == g_game_window && msg == WM_EXITSIZEMOVE) {
            InterlockedExchange(&g_in_modal, 0);
            // The user just finished dragging the frame. THIS is the signal that
            // their size beats the configured one -- recorded on EXITSIZEMOVE
            // rather than on WM_SIZE so a drag writes the ini once, at the end,
            // instead of on every intermediate pixel.
            remember_window_size(e->h);
        }

        // Minimise-path tracing (UNCONDITIONAL -- these are rare and we need to
        // know exactly where the click dies). Logs the caption-button press, the
        // resulting SYSCOMMAND, and the actual size transition, so one session
        // shows whether SC_MINIMIZE ever fires and whether the window minimises.
        if (e->h == g_game_window) {
            if (msg == WM_NCLBUTTONDOWN) {
                WORD ht = (WORD)wp;
                if (ht == HTMINBUTTON || ht == HTMAXBUTTON || ht == HTCLOSE)
                    logf("[frm] GAME caption-button DOWN hittest=%d "
                         "(8=MIN 9=MAX 20=CLOSE)", ht);
            } else if (msg == WM_SYSCOMMAND) {
                UINT cmd = (UINT)(wp & 0xFFF0);
                if (cmd == 0xF020 || cmd == 0xF030 || cmd == 0xF120)
                    logf("[frm] GAME SYSCOMMAND cmd=0x%04X "
                         "(F020=MIN F030=MAX F120=RESTORE)", cmd);
            } else if (msg == WM_SIZE &&
                       (wp == SIZE_MINIMIZED || wp == SIZE_MAXIMIZED)) {
                logf("[frm] GAME WM_SIZE %s",
                     wp == SIZE_MINIMIZED ? "MINIMIZED" : "MAXIMIZED");
            }
        }

        // Standard cursor over the FRAME. pol.exe hides the OS cursor over the
        // whole window (so the game can draw its own), which also kills the resize
        // arrows on the borders and the arrow on the caption -- the frame then
        // gives no "you can drag/size here" feedback. Over any NON-client area we
        // let DefWindowProc set the proper system cursor (IDC_SIZE*/IDC_ARROW);
        // only over HTCLIENT does the game keep its own cursor.
        if (e->h == g_game_window && msg == WM_SETCURSOR &&
            LOWORD(lp) != HTCLIENT)
            return DefWindowProcA(h, msg, wp, lp);

        const char* nm = mouse_msg_name(msg);
        // Sample rather than flood: the first few of each, then occasionally.
        // Gated on the flag so a spy attached purely for its BEHAVIOUR (see
        // install_msgspy) stays silent.
        if (g_d3d_msgspy && nm && (e->seen < 10 || (e->seen % 500) == 0)) {
            int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
            POINT sp;
            real_GetCursorPos ? real_GetCursorPos(&sp) : GetCursorPos(&sp);
            logf("[msg] %-4s %-11s lParam=(%d,%d)  screen=(%ld,%ld)  #%ld",
                 e->tag, nm, x, y, sp.x, sp.y, e->seen);
        }
        if (nm) InterlockedIncrement(&e->seen);

        // SUPPRESS A SELF-RAISE WHILE THE USER IS ELSEWHERE. A forced-windowed title that
        // still thinks it is fullscreen calls SetWindowPos(HWND_TOP) on its own window to
        // stay on top -- correct fullscreen behaviour, but in a window it yanks itself
        // back over whatever the user clicked behind it (FMO "fights me for the window",
        // and it steals the shim's own settings window too). SetWindowPos does not go
        // through the SetForegroundWindow/BringWindowToTop hooks, so it is caught here, on
        // the game's own WINDOWPOSCHANGING: if the game is raising its z-order to the top
        // while another app -- or our config dialog -- is foreground, hold its z-order.
        // The first presentation and any user-driven raise (the game IS foreground then)
        // are untouched, and a move/size the game itself performs keeps SWP_NOMOVE clear
        // so only the z-order half is pinned.
        if (e->h == g_game_window && msg == WM_WINDOWPOSCHANGING && g_d3d_nosteal &&
            InterlockedCompareExchange(&g_game_ever_fg, 0, 0)) {
            WINDOWPOS* wpz = (WINDOWPOS*)lp;
            bool raising = wpz && !(wpz->flags & SWP_NOZORDER) &&
                           (wpz->hwndInsertAfter == HWND_TOP ||
                            wpz->hwndInsertAfter == HWND_TOPMOST);
            if (raising && would_steal(g_game_window)) {
                wpz->flags |= SWP_NOZORDER;      // keep its current z-order; do not jump up
                static volatile LONG said = 0;
                if (say_once_per_title(&said))
                    logf("[foc] game tried to raise its window over the foreground app/"
                         "dialog (SetWindowPos HWND_TOP) -- z-order held so it stops "
                         "fighting the user's focus");
            }
        }

        // SUPPRESS A SELF-RESTORE WHILE THE USER HAS MINIMISED THE GAME.
        //
        // The same fault as the z-order guard above, with the same blind spot, and
        // it is why a windowed title cannot be minimised: the title re-places and
        // re-shows its OWN window with SetWindowPos, which goes through neither the
        // SetForegroundWindow/BringWindowToTop hooks nor ShowWindow (only the Friend
        // List's copy of ShowWindow is hooked, and that one is a pass-through for
        // every other window). `g_user_minimized` is SET by this wndproc and, until
        // now, was read ONLY by the focus hooks -- so nothing on the path the title
        // actually uses ever asked, and a minimised window was hauled straight back
        // up.
        //
        // MEASURED 2026-08-24, polshim.961088.log (Front Mission Online, windowed):
        // ten consecutive
        //     WINDOWPOSCHANGING (-32000,-32000 160x28)  <- minimise
        //     WM_SIZE MINIMIZED
        //     WINDOWPOSCHANGING (1,1 638x430)           <- restored again
        //     WINDOWPOSCHANGING (550,219 819x641)
        // cycles, with NO [foc] line anywhere near them -- because no focus API was
        // involved at any point. That absence is the whole reason this looked like
        // "minimise does nothing" rather than "something un-minimises it".
        //
        // Held only while the window really IS iconic and the user is demonstrably
        // elsewhere -- would_steal(), the same predicate the z-order arm has shipped
        // with. A user restore from the taskbar or Alt-Tab activates the window, so
        // would_steal is false and the restore proceeds untouched; the title's own
        // restore, fired while the user is working in another app, is the only case
        // that gets pinned. Failure mode if that ever misjudges is benign: one
        // restore attempt is held and the next one, with the game foreground,
        // succeeds.
        if (e->h == g_game_window && msg == WM_WINDOWPOSCHANGING && g_d3d_nosteal &&
            InterlockedCompareExchange(&g_user_minimized, 0, 0) &&
            IsIconic(g_game_window) && would_steal(g_game_window)) {
            WINDOWPOS* wpm = (WINDOWPOS*)lp;
            if (wpm && ((wpm->flags & SWP_SHOWWINDOW) ||
                        !(wpm->flags & (SWP_NOMOVE | SWP_NOSIZE)))) {
                wpm->flags &= ~SWP_SHOWWINDOW;
                wpm->flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
                static volatile LONG said = 0;
                if (say_once_per_title(&said))
                    logf("[frm] the title tried to restore its own MINIMISED window "
                         "(SetWindowPos, not a focus API -- which is why nothing "
                         "logged it before) while the user is in another app. Held "
                         "minimised; it will restore normally when the user asks.");
            }
        }

        // --- frame behaviour: move / resize / close -------------------------
        //
        // A fullscreen-only game has no frame, so its wndproc typically never
        // handles these and DefWindowProc never gets a chance either. We answer
        // them ourselves so the window behaves like a window.
        const char* fm = frame_msg_name(msg);
        if (fm && g_d3d_frame) {
            // WM_NCHITTEST fires on every mouse-move over the window and would
            // drown the budget before a single click is seen, so it is neither
            // logged nor counted here (it is handled below). Everything else --
            // the actual drag/resize/button/pos-change traffic -- is what we want.
            //
            // But the budget cannot apply to ALL of it. ONE drag emits three
            // messages per mouse-move (WINDOWPOSCHANGING/CHANGED/MOVE) and blows
            // through 80 in well under a second -- so every log we have goes
            // silent at exactly the moment the user's *second* drag attempt
            // happens, which is the one being reported as "inert". Measured on
            // 2026-08-24 across five sessions: not one recorded a WM_EXITSIZEMOVE
            // or a second WM_NCLBUTTONDOWN, purely because of this cap.
            //
            // So: the RARE, decisive messages are always logged (a whole session
            // produces a handful), and the budget is spent only on the
            // high-volume per-pixel traffic it was written for.
            bool rare = (msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK ||
                         msg == WM_NCRBUTTONUP   || msg == WM_SYSCOMMAND ||
                         msg == WM_ENTERSIZEMOVE || msg == WM_EXITSIZEMOVE ||
                         msg == WM_CAPTURECHANGED ||
                         msg == WM_CLOSE         || msg == WM_DESTROY);
            if (msg != WM_NCHITTEST && (rare || e->frame_seen < 80)) {
                if (msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED) {
                    WINDOWPOS* wp2 = (WINDOWPOS*)lp;
                    logf("[frm] %-4s %-18s pos=(%d,%d %dx%d) flags=%08X",
                         e->tag, fm, wp2->x, wp2->y, wp2->cx, wp2->cy,
                         (unsigned)wp2->flags);
                } else {
                    logf("[frm] %-4s %-18s wParam=%08lX lParam=%08lX",
                         e->tag, fm, (unsigned long)wp, (unsigned long)lp);
                }
                // Rare messages are exempt, so they must not spend the budget
                // either -- otherwise a long session's drags would still starve
                // the pos-change traffic they are meant to be read against.
                if (!rare) InterlockedIncrement(&e->frame_seen);
            }

            // THE MOUSE CAPTURE RE-GRAB -- why a forced-windowed frame goes inert
            // after exactly one drag.
            //
            // Measured 2026-08-24 on a stuck FMO window (Probe-StuckWindow.ps1):
            // the style was intact, the window enabled, the hit test still
            // answered HTCAPTION, no modal loop was running -- and the window held
            // the MOUSE CAPTURE with no button down. A window that has the capture
            // gets NO non-client hit-testing: all mouse input arrives as CLIENT
            // messages, so a caption click never becomes WM_NCLBUTTONDOWN, never
            // becomes SC_MOVE, and no drag or size loop can start.
            //
            // Then the RE, on the unpacked FrontMissionOnline.dll:
            // FrontMissionOnline.dll has exactly ONE SetCapture call site, and its
            // wndproc's jump table reaches it from WM_CAPTURECHANGED -- the message
            // Windows sends a window when it LOSES the capture:
            //
            //   6100A7E1  mov  eax, [0x613AE688]   ; the game's window object
            //   6100A7E6  mov  cl,  [eax+0x36]     ; 0 = "the game owns the mouse"
            //   6100A7EB  jne  0x6100A7F7          ; non-zero -> leave it alone
            //   6100A7ED  mov  eax, [eax+5]        ; its HWND
            //   6100A7F1  call [SetCapture]        ; else TAKE IT STRAIGHT BACK
            //
            // The same [obj+0x36] drives ShowCursor(FALSE) at 0x6100A6E5, so it is
            // the game's own "I own the mouse" flag -- correct for exclusive
            // fullscreen, wrong in a window. Its other three ReleaseCapture sites
            // are all teardown paths (ClipCursor(NULL), ShowCursor, ReleaseCapture),
            // so this re-grab is the ONLY acquisition in the module.
            //
            // Sequence: the caption drag runs DefWindowProc's modal loop, which
            // captures the mouse itself -- that first drag works. The loop then
            // RELEASES on exit, that release sends WM_CAPTURECHANGED, and the game
            // takes the capture back and keeps it. The frame is inert from then on.
            //
            // It also explains why nothing recovers it: any ReleaseCapture we or
            // anyone else issued would send WM_CAPTURECHANGED and be undone on the
            // spot. Releasing it is NOT a fix -- refusing to deliver the message
            // that triggers the re-grab is.
            //
            // Swallowing costs nothing: the whole 0x215 arm IS the re-grab, and it
            // falls straight into the shared `xor al,al; ret 0xC` epilogue.
            //
            // Restricted to a window WE restyled: a native-windowed title
            // (g_title_native_windowed) manages its own frame and must keep its own
            // capture handling. [dx] d3d_nograb=0 restores SE's behaviour.
            if (e->h == g_game_window && msg == WM_CAPTURECHANGED &&
                g_d3d_nograb && !g_title_native_windowed) {
                static LONG said = 0;
                if (InterlockedIncrement(&said) <= 3)
                    logf("[cap] WM_CAPTURECHANGED withheld from the game (capture "
                         "went to %p) -- delivering it makes the title re-take the "
                         "mouse, and a captured window gets no non-client "
                         "hit-testing, so its caption stops dragging. "
                         "[dx] d3d_nograb=0 to restore SE's behaviour.", (void*)lp);
                return 0;
            }

            // WM_NCHITTEST is what makes a title bar draggable. If the game
            // returns HTCLIENT for everything (common: it wants every pixel),
            // the frame becomes inert. Let DefWindowProc decide instead, which
            // restores dragging, the sizing borders and the caption buttons.
            if (msg == WM_NCHITTEST) {
                LRESULT ht = DefWindowProcA(h, msg, wp, lp);
                if (e->frame_seen < 4)
                    logf("[frm] %s NCHITTEST -> %ld (DefWindowProc)", e->tag, (long)ht);
                return ht;
            }

            // THE non-client INTERACTION messages. pol.exe's wndproc was written
            // for a borderless fullscreen window and silently drops these, so the
            // caption never drags, the borders never size and the min/max/close
            // buttons never fire. Hand them to DefWindowProc, which runs the
            // modal move/size loop and drives the caption buttons natively.
            if (msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK ||
                msg == WM_NCRBUTTONUP) {
                return DefWindowProcA(h, msg, wp, lp);
            }

            // Caption-button / system-menu commands. Everything except CLOSE is a
            // pure window operation DefWindowProc should carry out. CLOSE is left
            // to the Viewer's own wndproc, which ends the TITLE (DefWindowProc
            // would only destroy the window and leave the title running headless).
            if (msg == WM_SYSCOMMAND) {
                UINT cmd = (UINT)(wp & 0xFFF0);
                // Remember a user minimise so the focus hooks can hold the game
                // down (it re-foregrounds itself every frame); a restore/maximise
                // clears it. Only for the game window itself.
                if (h == g_game_window) {
                    if (cmd == SC_MINIMIZE)
                        InterlockedExchange(&g_user_minimized, 1);
                    else if (cmd == SC_RESTORE || cmd == SC_MAXIMIZE)
                        InterlockedExchange(&g_user_minimized, 0);
                }
                if (cmd == SC_CLOSE) {
                    logf("[frm] %s: user asked to CLOSE", e->tag);
                    // Only the GAME window asks. The Viewer and its dialogs
                    // have their own close behaviour and PlayOnline already
                    // prompts for its own exit; a second prompt on top of
                    // SE's would be ours interrupting theirs.
                    if (h == g_game_window) {
                        // exitprompt ARMS the real close (WM_CLOSE on its own
                        // thread, then the Viewer for "exit to desktop") and
                        // returns. We swallow SC_CLOSE in every case: passing it
                        // to the title's wndproc is measured to do nothing at
                        // all, and handing DefWindowProc a close for a window we
                        // have just asked to close politely is how you get a
                        // destroyed window with the title still running headless.
                        exitprompt_on_close(h, g_title_leaf);
                        return 0;
                    }
                } else {
                    return DefWindowProcA(h, msg, wp, lp);
                }
            }
        }
    }
    return orig ? CallWindowProcA(orig, h, msg, wp, lp)
                : DefWindowProcA(h, msg, wp, lp);
}

static void install_msgspy(HWND h, const char* tag)
{
    if (!h || !IsWindow(h)) return;
    // The GAME window is subclassed even with the debug spy OFF, because this
    // subclass is NOT only a debug facility: `spy_proc` is the only thing that
    // sets `g_in_modal` (WM_ENTERSIZEMOVE) and `g_user_minimized` (WM_SIZE),
    // and the focus hooks read both to hold the game's per-frame
    // SetForegroundWindow off. Without it the modal move/size loop is cancelled
    // mid-drag and the window cannot be moved, resized or kept minimised.
    //
    // Measured 2026-08-15: a user set `d3d_msgspy=0` because it reads as a
    // debug knob, and the window became completely immovable -- the run's log
    // has no `[msg] watching GAME` line at all. Chatty per-message tracing is
    // still gated on the flag below; only the behaviour is unconditional.
    if (!g_d3d_msgspy && h != g_game_window) return;

    // REAP FIRST. This table is per-title state, and until the title boundary
    // began clearing it, it only ever GREW: one GAME (and usually one MASK)
    // entry per launch, four slots, no eviction, and -- the part that made this
    // invisible -- a SILENT return once full. In polshim.962956.log the fourth
    // title of the session got no subclass and no line saying so, so its window
    // could not be dragged, resized or minimised.
    //
    // Reaping here is belt-and-braces behind d3d_title_boundary: even if a
    // boundary is ever missed, the table cannot fill up with corpses.
    //
    // A stale entry is a correctness hazard in its own right, not just a wasted
    // slot: Windows RECYCLES HWND values, so a new window can arrive wearing a
    // dead entry's handle and be skipped as "already watched". That is why the
    // liveness test is "is OUR spy_proc still this window's wndproc", not
    // "is the handle still valid".
    for (int i = 0; i < g_nspy; ) {
        SpyEntry* s = &g_spy[i];
        bool live = s->h && IsWindow(s->h) &&
                    (WNDPROC)GetWindowLongPtrA(s->h, GWLP_WNDPROC) == spy_proc;
        if (live) { i++; continue; }
        logf("[msg] reaping stale spy slot %d (%s window %p) -- that window is "
             "gone, or is no longer ours to watch", i, s->tag, (void*)s->h);
        g_spy[i] = g_spy[--g_nspy];
    }

    for (int i = 0; i < g_nspy; i++) if (g_spy[i].h == h) return;
    if (g_nspy >= _countof(g_spy)) {
        // NEVER silent. The symptom of a full table is a window the user cannot
        // move, and there is nothing else in the log that tells it apart from
        // the half-dozen other causes of the same complaint.
        logf("[msg] SPY TABLE FULL (%d slots, all live) -- NOT watching the %s "
             "window %p. That window will not be draggable, resizable or "
             "minimisable: spy_proc is the only handler of its frame messages.",
             (int)_countof(g_spy), tag, (void*)h);
        return;
    }

    SpyEntry* e = &g_spy[g_nspy];
    e->h = h;
    e->seen = 0;
    e->frame_seen = 0;
    strncpy_s(e->tag, sizeof(e->tag), tag, _TRUNCATE);
    e->orig = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)spy_proc);
    if (!e->orig) {
        logf("[msg] could not subclass %s window %p (err=%lu)", tag, h, GetLastError());
        return;
    }
    g_nspy++;
    RECT c;
    GetClientRect(h, &c);
    logf("[msg] watching %s window %p, client %ldx%ld", tag, h, c.right, c.bottom);
}

// Clear the spy table at a title boundary (d3d8_new_title_launch). Un-subclass any
// window that somehow survived, but ONLY if we are still its outermost wndproc:
// restoring e->orig over someone else's later subclass would corrupt their chain, so
// in that case we just drop our record and leave the chain intact. Called on the
// title-launch thread; the previous title's short-lived mask_thread has finished
// aligning by the time gamestart starts the next title, so this does not contend with
// install_msgspy in practice.
static void spy_reset_for_new_title(void)
{
    for (int i = 0; i < g_nspy; i++) {
        SpyEntry* e = &g_spy[i];
        if (e->h && IsWindow(e->h) &&
            (WNDPROC)GetWindowLongPtrA(e->h, GWLP_WNDPROC) == spy_proc)
            SetWindowLongPtrA(e->h, GWLP_WNDPROC, (LONG_PTR)e->orig);
    }
    g_nspy = 0;
}

// --- focus stealing ---------------------------------------------------------
//
// MEASURED, not assumed. The DirectInput theory for "it grabs my window back"
// is dead: the game asks for NONEXCLUSIVE|FOREGROUND, so exclusivity was never
// the reason. Something is actively calling a focus API instead. These hooks
// name it, and optionally refuse it.
//
// The refusal rule is deliberately narrow: only a call that would pull focus TO
// the game FROM another process is blocked, and it still returns TRUE so the
// caller's own logic proceeds. Focus changes within our own process are left
// alone, so the Viewer can still manage its own windows.

typedef BOOL (WINAPI *PFN_SetForegroundWindow)(HWND);
typedef BOOL (WINAPI *PFN_BringWindowToTop)(HWND);
typedef HWND (WINAPI *PFN_SetActiveWindow)(HWND);
typedef HWND (WINAPI *PFN_SetFocus)(HWND);

static PFN_SetForegroundWindow real_SetForegroundWindow = NULL;
static PFN_BringWindowToTop    real_BringWindowToTop    = NULL;
static PFN_SetActiveWindow     real_SetActiveWindow     = NULL;
static PFN_SetFocus            real_SetFocus            = NULL;

static LONG g_n_fg = 0, g_n_fg_blocked = 0, g_n_btt = 0, g_n_setactive = 0;

// ===========================================================================
// THE STARTUP EXEMPTION -- why nosteal cannot be a flat rule.
//
// nosteal exists because these titles re-grab the foreground mid-session and
// yank you out of whatever you were doing. But it was written as "refuse any
// call that takes focus from another process", and that description also fits
// the ONE call that has to be allowed: the game raising its own window for the
// first time, at launch. If you alt-tab while a title loads -- which is the
// normal thing to do, since it takes several seconds -- the presentation call
// arrives while some other app is foreground, gets refused, and the game sits
// at the bottom of the z-order with a valid, visible, unreachable window.
//
// Observed exactly that way (polshim.507768.log): calls #1 and #2 succeeded
// while the Viewer still had focus, then
//     [foc] SetForegroundWindow(02D211A2)  <-- WOULD STEAL FROM ANOTHER APP  [call #3]
// and the window was left at z-rank 95 of ~98. Nothing covered it; the audit
// even confirmed "window at the game's centre point = the game -- good". It had
// simply never been raised.
//
// The exemption is deliberately narrow, because a wide one is just nosteal off:
//   * the target must be OUR OWN game window (not any window, not the Viewer's);
//   * the game window must never yet have reached the foreground -- one raise,
//     not a licence to grab focus repeatedly;
//   * and it must be within d3d_nosteal_grace ms of that window first appearing,
//     so a title running for an hour can never invoke it.
// Once the game has been foreground even once, every later grab is refused
// exactly as before -- which is the behaviour nosteal was actually built for.
// ===========================================================================
static LONG  g_n_fg_startup   = 0;      // exemptions granted

// Called wherever we already ask who is foreground, so noticing costs nothing.
static void note_foreground(HWND fg)
{
    if (fg && fg == g_game_window) InterlockedExchange(&g_game_ever_fg, 1);
}

// May this call be allowed through even though it takes focus from another app?
static bool startup_presentation(HWND target)
{
    if (!target || target != g_game_window) return false;
    if (InterlockedCompareExchange(&g_game_ever_fg, 0, 0)) return false;
    if (!g_game_born_tick) return false;
    if (g_nosteal_grace >= 0 && (GetTickCount() - g_game_born_tick) > (DWORD)g_nosteal_grace)
        return false;
    return true;
}

// True when this call would yank focus away from a DIFFERENT application.
// Is `h` one of the shim's OWN dialog windows -- the settings window, the padmap
// mapper, or a prompt/text box? The game must not be allowed to steal focus from these
// while the user is configuring: they belong to our process (same pid), so the plain
// "another app is foreground" test misses them, and a forced-windowed title that
// re-foregrounds itself every frame makes the settings window unusable (and, fighting
// it, the user toggles a fix-pack off by accident -- exactly how Tetra Master's mouse
// got reverted, 2026-08-18).
static bool is_shim_dialog_window(HWND h)
{
    if (!h) return false;
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, _countof(cls));
    return !wcscmp(cls, L"PolShimSettings")   || !wcscmp(cls, L"PolShimSettingsPane") ||
           !wcscmp(cls, L"PolShimPrompt")     || !wcscmp(cls, L"PolShimText")         ||
           !wcscmp(cls, L"PolShimPadMap");
}

static bool would_steal(HWND target)
{
    if (!target) return false;
    HWND fg = GetForegroundWindow();
    note_foreground(fg);
    if (!fg || fg == target) return false;
    DWORD fgpid = 0, me = GetCurrentProcessId();
    GetWindowThreadProcessId(fg, &fgpid);
    if (fgpid != me) return true;             // the user is in someone else's window
    return is_shim_dialog_window(fg);         // ...or in OUR settings/config dialog
}

static BOOL WINAPI hook_SetForegroundWindow(HWND h)
{
    InterlockedIncrement(&g_n_fg);
    // The Viewer's mask never holds the foreground (maskguard.cpp's invariant).
    if (maskguard_refuse_activation(h, "SetForegroundWindow")) return TRUE;
    bool steal = would_steal(h);
    if (g_d3d_trace && (g_n_fg < 20 || (g_n_fg % 100) == 0))
        logf("[foc] SetForegroundWindow(%p)%s  [call #%ld]",
             h, steal ? "  <-- WOULD STEAL FROM ANOTHER APP" : "", g_n_fg);
    if (steal && g_d3d_nosteal) {
        if (startup_presentation(h)) {
            // The game showing itself for the first time. Refusing this is what
            // leaves the window visible, uncovered and unreachable at the bottom
            // of the z-order.
            InterlockedIncrement(&g_n_fg_startup);
            logf("[foc] ALLOWED as the game's first presentation "
                 "(%lu ms after its window appeared) -- refusing this is what "
                 "launches it hidden", (unsigned long)(GetTickCount() - g_game_born_tick));
            return real_SetForegroundWindow(h);
        }
        InterlockedIncrement(&g_n_fg_blocked);
        return TRUE;                 // pretend success; do not actually steal
    }
    if (h == g_game_window &&
        (InterlockedCompareExchange(&g_user_minimized, 0, 0) ||
         InterlockedCompareExchange(&g_in_modal, 0, 0)))
        return TRUE;                 // minimised, or mid drag/resize -- hold focus
    return real_SetForegroundWindow(h);
}

static BOOL WINAPI hook_BringWindowToTop(HWND h)
{
    InterlockedIncrement(&g_n_btt);
    if (maskguard_refuse_activation(h, "BringWindowToTop")) return TRUE;
    bool steal = would_steal(h);
    if (g_d3d_trace && (g_n_btt < 20 || (g_n_btt % 100) == 0))
        logf("[foc] BringWindowToTop(%p)%s  [call #%ld]",
             h, steal ? "  <-- WOULD STEAL" : "", g_n_btt);
    if (steal && g_d3d_nosteal) {
        // Same exemption as SetForegroundWindow: a title that presents itself
        // with BringWindowToTop instead must not be penalised for the choice.
        if (startup_presentation(h)) {
            InterlockedIncrement(&g_n_fg_startup);
            return real_BringWindowToTop(h);
        }
        return TRUE;
    }
    if (h == g_game_window &&
        (InterlockedCompareExchange(&g_user_minimized, 0, 0) ||
         InterlockedCompareExchange(&g_in_modal, 0, 0)))
        return TRUE;
    return real_BringWindowToTop(h);
}

static HWND WINAPI hook_SetActiveWindow(HWND h)
{
    InterlockedIncrement(&g_n_setactive);
    if (maskguard_refuse_activation(h, "SetActiveWindow")) return GetActiveWindow();
    if (g_d3d_trace && (g_n_setactive < 20 || (g_n_setactive % 200) == 0))
        logf("[foc] SetActiveWindow(%p)  [call #%ld]", h, g_n_setactive);
    if (h == g_game_window &&
        (InterlockedCompareExchange(&g_user_minimized, 0, 0) ||
         InterlockedCompareExchange(&g_in_modal, 0, 0)))
        return h;                    // minimised, or mid drag/resize -- keep it
    return real_SetActiveWindow(h);  // intra-process; never blocked
}

void* d3d8_real_SetForegroundWindow() { return (void*)real_SetForegroundWindow; }
void* d3d8_hook_SetForegroundWindow() { return (void*)hook_SetForegroundWindow; }
void* d3d8_real_BringWindowToTop()    { return (void*)real_BringWindowToTop; }
void* d3d8_hook_BringWindowToTop()    { return (void*)hook_BringWindowToTop; }
void* d3d8_real_SetActiveWindow()     { return (void*)real_SetActiveWindow; }
void* d3d8_hook_SetActiveWindow()     { return (void*)hook_SetActiveWindow; }

void* d3d8_real_SetCursorPos() { return (void*)real_SetCursorPos; }
void* d3d8_hook_SetCursorPos() { return (void*)hook_SetCursorPos; }
// The same icon the game window gets, for any window of ours that should look
// like it belongs to PlayOnline rather than to a generic Win32 app.
void d3d8_set_window_icon(HWND h) { set_game_icon(h); }

void* d3d8_real_GetCursorPos() { return (void*)real_GetCursorPos; }
void* d3d8_hook_GetCursorPos() { return (void*)hook_GetCursorPos; }
void* d3d8_real_ClipCursor()   { return (void*)real_ClipCursor; }
void* d3d8_hook_ClipCursor()   { return (void*)hook_ClipCursor; }
void* d3d8_real_ShowCursor()   { return (void*)real_ShowCursor; }
void* d3d8_hook_ShowCursor()   { return (void*)hook_ShowCursor; }
void* d3d8_real_SetCapture()     { return (void*)real_SetCapture; }
void* d3d8_hook_SetCapture()     { return (void*)hook_SetCapture; }
void* d3d8_real_ReleaseCapture() { return (void*)real_ReleaseCapture; }
void* d3d8_hook_ReleaseCapture() { return (void*)hook_ReleaseCapture; }

// The forced-windowed backbuffer size -- i.e. the coordinate field the game
// believes it is drawing into. dinputhook maps the OS pointer into this so
// absolute mouse tracking stays correct even when the window is resized (the
// client area then differs from the backbuffer and D3D stretches to fit).
void d3d8_backbuffer_size(int* w, int* h)
{
    if (w) *w = (int)g_bb_w;
    if (h) *h = (int)g_bb_h;
}

// --- the Viewer's input HWND ------------------------------------------------
//
// REVERSED from app.dll, not guessed. app.dll -- not the game -- owns the mouse
// for every title: it is the only module importing SetCursorPos / GetCursorPos /
// ClipCursor / ScreenToClient / ClientToScreen. TM.dll imports just
// CallNextHookEx, and polcore only SetWindowsHookExA/CallNextHookEx (the hook
// plumbing). That is why FMO shows the same symptoms: it is ONE piece of shared
// shell code, not a per-game bug.
//
// Its mouse-message handler at app.dll+0x27886A reads:
//
//     call [GetCursorPos]                  ; screen coordinates
//     push dword ptr [0x04E15E70]          ; <-- the window to convert against
//     call [ScreenToClient]
//     movzx eax, word ptr [ebp-4]          ; y
//     movzx ecx, word ptr [ebp-8]          ; x
//
// So one global decides the coordinate space the whole Viewer -- and every game
// running inside it -- works in. If it holds the full-screen mask window while
// the game presents into a 640x480 window, every coordinate is off by the window
// origin and scaled by the size ratio, which is the runaway we measured.
#define APP_INPUT_HWND_RVA 0x5C5E70     // app.dll VA 0x04E15E70

static HWND  g_saved_input_hwnd = NULL;
static HWND* g_input_hwnd_slot  = NULL;

static const char* describe_hwnd(HWND h, char* buf, size_t cb)
{
    if (!h) { strncpy_s(buf, cb, "<null>", _TRUNCATE); return buf; }
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    RECT r; ZeroMemory(&r, sizeof(r));
    GetWindowRect(h, &r);
    _snprintf_s(buf, cb, _TRUNCATE, "%p class=%s (%ld,%ld %ldx%ld)",
                h, cls, r.left, r.top, r.right - r.left, r.bottom - r.top);
    return buf;
}

// Find the Viewer's app module by ENUMERATION, not by guessing its name.
// GetModuleHandleA("app.dll") came back NULL in a process whose COM log is full
// of app.dll vtable traces, so the base name is not what we assumed -- the
// regional builds ship appEU.dll / appJP.dll, and the loader may hold it under a
// path. Enumerate and match a prefix, and if that fails, LIST what is loaded so
// the next run does not have to guess either.
static HMODULE find_app_module()
{
    static const char* CANDIDATES[] = { "app.dll", "appEU.dll", "appJP.dll", "appUS.dll" };
    for (int i = 0; i < _countof(CANDIDATES); i++) {
        HMODULE m = GetModuleHandleA(CANDIDATES[i]);
        if (m) return m;
    }
    // Prefix matching was a mistake: "app" + ".dll" happily matches Windows'
    // own apphelp.dll, and we then read a garbage address inside it. Require the
    // module to live under the PlayOnline install, which no system DLL does.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    MODULEENTRY32 me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);
    HMODULE hit = NULL;
    char listed[2048] = "";
    if (Module32First(snap, &me)) {
        do {
            bool ours = (StrStrIA(me.szExePath, "\\PlayOnline\\") != NULL) ||
                        (StrStrIA(me.szExePath, "\\SquareEnix\\") != NULL);
            if (!hit && ours) {
                const char* dot = strrchr(me.szModule, '.');
                if (dot && _stricmp(dot, ".dll") == 0 &&
                    _strnicmp(me.szModule, "app", 3) == 0) {
                    hit = (HMODULE)me.modBaseAddr;
                    logf("[in] app module: %s at %p (%s)",
                         me.szModule, me.modBaseAddr, me.szExePath);
                }
            }
            // Always record the PlayOnline-owned modules -- if the match fails,
            // this is the list that says what is actually loaded here.
            if (ours && strlen(listed) < sizeof(listed) - 80) {
                strncat_s(listed, sizeof(listed), me.szModule, _TRUNCATE);
                strncat_s(listed, sizeof(listed), " ", _TRUNCATE);
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    logf("[in] PlayOnline modules in this process: %s", listed[0] ? listed : "(none)");
    if (!hit) logf("[in] no app*.dll among them -- this process does not host the Viewer UI");
    return hit;
}

static void inspect_input_hwnd()
{
    HMODULE app = find_app_module();
    if (!app) return;
    HWND* slot = (HWND*)((BYTE*)app + APP_INPUT_HWND_RVA);
    HWND cur = NULL;
    __try { cur = *slot; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("[in] app.dll+0x%06X unreadable -- wrong RVA for this build?",
             APP_INPUT_HWND_RVA);
        return;
    }
    g_input_hwnd_slot = slot;
    if (!g_saved_input_hwnd) g_saved_input_hwnd = cur;

    char a[160], b[160];
    logf("[in] app.dll+0x%06X (the ScreenToClient window) = %s",
         APP_INPUT_HWND_RVA, describe_hwnd(cur, a, sizeof(a)));
    logf("[in]   game window is                            %s",
         describe_hwnd(g_game_window, b, sizeof(b)));
    // VALIDATE before believing the RVA. The offset was read off an app.dll dump
    // rebased to 0x04850000; a different build would put something else here.
    // A real HWND is the check, and it is a cheap one.
    if (!IsWindow(cur)) {
        logf("[in]   value is NOT a window -- app.dll+0x%06X is not the input "
             "HWND in this build. Refusing to write.", APP_INPUT_HWND_RVA);
        g_input_hwnd_slot = NULL;
        return;
    }
    if (cur == g_game_window)
        logf("[in]   -> ALREADY the game window; the coordinate space is right");
    else
        logf("[in]   -> DIFFERENT. Every mouse coordinate the Viewer computes is "
             "in that window's space, not the game's.");

    if (g_d3d_inputhwnd && g_game_window && cur != g_game_window) {
        DWORD prot;
        if (VirtualProtect(slot, sizeof(HWND), PAGE_READWRITE, &prot)) {
            *slot = g_game_window;
            VirtualProtect(slot, sizeof(HWND), prot, &prot);
            logf("[in]   REPOINTED to the game window -- ScreenToClient now "
                 "converts into the space the game actually renders in");
        } else {
            logf("[in]   could not make the slot writable");
        }
    }
}

// --- GetWindowRect: the actual cursor bug -----------------------------------
//
// REVERSED from pol.exe, and this is the one that matters. The game process
// holds pol.exe + PolHook.dll + polcore.dll + TM.dll and **no app.dll**, so the
// app.dll cursor code reversed earlier cannot be involved in a game session.
// pol.exe owns it, in a single routine -- the only GetCursorPos call site in the
// whole binary:
//
//     00409FDC  mov  eax, [ebp+0x14]        ; the game window
//     00409FE5  call [GetWindowRect]        ; <-- the WINDOW rect
//     00409FF0  call [GetCursorPos]         ; screen position
//     0040A006  jmp  [ecx*4 + 0x40A454]     ; 8 aspect/scale modes
//     0040A00D  fild/fidiv                  ; aspect = w / h
//     0040A022  lea edi,[esi+esi*4]; shl 7  ; * 640
//     0040A03F  imul edx, edx, 0x1E0        ; * 480
//
// It maps the screen cursor into the title's 640x480 space using the WINDOW
// rectangle. Fullscreen that is harmless: a borderless full-screen window's
// window rect IS its client area. But fit_window gives the window a caption and
// a sizing border to make it draggable and closable, and pol.exe then treats
// that frame as part of the play field:
//
//     window (312,100) 656x519   vs   client (320,131) 640x480
//
// -> origin off by the frame (8,31), scale off by 656/640 and 519/480, and
//    re-derived every frame, which is the runaway rather than a fixed offset.
//
// So: tell pol.exe the truth about the drawable area. For the game window only,
// GetWindowRect returns the CLIENT rect in screen coordinates. That keeps the
// caption, the X and the sizing border while making the mapping exact -- the
// alternative (dropping the frame so window == client) would fix the pointer by
// taking away the window controls that were asked for.
typedef BOOL (WINAPI *PFN_GetWindowRect)(HWND, LPRECT);
static PFN_GetWindowRect real_GetWindowRect = NULL;
static LONG g_n_rectfixed = 0;

static BOOL WINAPI hook_GetWindowRect(HWND h, LPRECT r)
{
    BOOL ok = real_GetWindowRect(h, r);

    // Log EVERY call, not just the game window's. pol.exe's cursor routine makes
    // THREE GetWindowRect calls (0x00409ECD, 0x00409FE5, 0x0040A13A) plus two
    // GetClientRect, and the pointer still behaves as if the field were the whole
    // monitor even with GetSystemMetrics reporting 640x480. So one of those calls
    // is asking about a window we are not correcting. This names it.
    if (ok && r && g_d3d_trace) {
        static LONG n = 0;
        LONG i = InterlockedIncrement(&n);
        if (i <= 24) {
            char cls[64] = "";
            GetClassNameA(h, cls, sizeof(cls));
            logf("[rect] #%ld GetWindowRect(%p class=%s) = (%ld,%ld %ldx%ld)%s",
                 i, h, cls, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 (h == g_game_window) ? "  <-- the game window" :
                 (h == GetDesktopWindow()) ? "  <-- THE DESKTOP (full monitor)" : "");
        }
    }

    if (!ok || !r || !g_d3d_clientrect) return ok;
    if (!g_game_window || !IsWindow(g_game_window)) return ok;

    // The DESKTOP substitution is gated on the caller being the title, for the
    // same reason and on the same evidence as hook_GetSystemMetrics above.
    //
    // The note that used to stand here -- "pol.exe's cursor routine asks about
    // the DESKTOP and about its own PlayOnlineUS window" -- named call sites
    // 0x00409ECD, 0x00409FE5 and 0x0040A13A, and all three are inside
    // FUN_00409E40, the Viewer's window procedure (WM_NCLBUTTONDBLCLK, WM_SIZING
    // and WM_MOVING respectively). The fourth, 0x004067B4, is FUN_004067B0:
    //
    //   GetWindowRect(GetDesktopWindow(), &r);
    //   SetWindowPos(h, NULL, (r.right - w) / 2, (r.bottom - h) / 2, w, h, ...);
    //
    // -- i.e. CENTRE THIS WINDOW ON THE DESKTOP. Handing that routine the game's
    // 640x480 drawable area as the desktop centres every window pol.exe centres
    // at ((640-w)/2, (480-h)/2), which is off-screen for anything wider. That is
    // the same class of damage the screen-size lie was doing, and it is why this
    // one is gated too rather than left to d3d_clientrect's default.
    if (!caller_is_title(_ReturnAddress())) return ok;

    bool is_desktop = (h == GetDesktopWindow());
    if (h != g_game_window && !is_desktop) return ok;

    RECT c;
    POINT o;
    HWND src = g_game_window;      // always describe the GAME's drawable area
    if (!GetClientRect(src, &c)) return ok;
    o.x = 0; o.y = 0;
    if (!ClientToScreen(src, &o)) return ok;

    RECT before = *r;
    r->left   = o.x;
    r->top    = o.y;
    r->right  = o.x + (c.right  - c.left);
    r->bottom = o.y + (c.bottom - c.top);

    InterlockedIncrement(&g_n_rectfixed);
    if (g_d3d_trace && (g_n_rectfixed < 6 || (g_n_rectfixed % 900) == 0))
        logf("[rect] %s rect (%ld,%ld %ldx%ld) -> game's drawable area "
             "(%ld,%ld %ldx%ld)  [pol.exe maps the cursor with this]",
             is_desktop ? "DESKTOP" : "game",
             before.left, before.top, before.right - before.left,
             before.bottom - before.top,
             r->left, r->top, r->right - r->left, r->bottom - r->top);
    return ok;
}

void* d3d8_real_GetWindowRect() { return (void*)real_GetWindowRect; }
void* d3d8_hook_GetWindowRect() { return (void*)hook_GetWindowRect; }

// --- EAT patch on d3d8.dll --------------------------------------------------
//
// WHY THIS IS NEEDED, measured 2026-08-12. The IAT patch DID land -- the log
// says `[iat] hooked d3d8:Direct3DCreate8 in TM.dll` -- and the hook still never
// fired. The reason is in TM.dll's section table: its names are NULLED
// (`,,,,.rsrc,,.data,.data`), i.e. TM.dll is POL1/ASProtect-packed like the rest
// of the client. The packer's stub re-resolves imports at runtime, which
// overwrites the slot we patched at DLL-load time. Patching a packed module's
// IAT is a race we lose.
//
// So patch the SOURCE instead of the call site: rewrite d3d8.dll's own export
// table entry. That is the same mechanism inject.cpp already uses for
// DllGetClassObject, and its rationale applies verbatim -- every resolution path
// (a loader bind, GetProcAddress, LdrGetProcedureAddress, a packer stub
// re-resolving late) reads the EAT, so all of them land on our thunk. We install
// it at startup, long before TM.dll loads, so the packer's own resolution is
// caught too.
//
// EAT entries are 32-bit RVAs added to the module base. In a 32-bit address
// space (hook - base) as a DWORD round-trips correctly even when the hook sits
// below d3d8's base, because base + RVA wraps mod 2^32 back to the hook.
// Shared with dinputhook.cpp -- see polshim.h.
bool dx_eat_patch_export(HMODULE mod, const char* want, void* hook,
                         const char* tag)
{
    if (!mod) return false;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_DATA_DIRECTORY& ed =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!ed.VirtualAddress) return false;

    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + ed.VirtualAddress);
    DWORD* names = (DWORD*)(base + exp->AddressOfNames);
    WORD*  ords  = (WORD*) (base + exp->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* nm = (const char*)(base + names[i]);
        if (strcmp(nm, want) != 0) continue;
        DWORD* slot = &funcs[ords[i]];
        DWORD  old_rva = *slot;
        DWORD  new_rva = (DWORD)((BYTE*)hook - base);
        DWORD  prot;
        if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &prot)) return false;
        *slot = new_rva;
        VirtualProtect(slot, sizeof(DWORD), prot, &prot);
        logf("[d3d] EAT patched %s: RVA %08lX -> %08lX", tag, old_rva, new_rva);
        return true;
    }
    logf("[d3d] EAT patch FAILED: %s not found in the module's exports", tag);
    return false;
}

void d3d8_resolve()
{
    if (!g_d3d_enable) return;
    // d3d8.dll is still shipped in SysWOW64 on Windows 11 (verified), so this
    // resolves on a stock machine. Loaded explicitly for the same reason as
    // ws2_32/dsound: patch_iat matches by resolved address.
    HMODULE m = LoadLibraryW(L"d3d8.dll");
    if (m) real_Direct3DCreate8 = (PFN_Direct3DCreate8)GetProcAddress(m, "Direct3DCreate8");
    // NO pre-seed of the "screen size" here -- deliberately, and this is the one
    // place that would tempt you to add one back.
    //
    // There used to be a `g_bb_w = 640; g_bb_h = 480;` at this point, armed by
    // d3d_windowed alone, to catch "pol.exe samples GetSystemMetrics early and
    // caches it". Two things are wrong with that. The sampling it named is
    // pol.exe's WINDOW PROCEDURE answering WM_GETMINMAXINFO, not a cursor
    // routine (see hook_GetSystemMetrics for the call-site evidence) -- so the
    // seed's only real effect was to clamp the Viewer's own window to 640x480
    // the moment d3d_windowed was switched on. And 640x480 is a guess that is
    // wrong for Front Mission Online, which asks for 800x600 (measured, [d3d9]
    // CreateDevice).
    //
    // The substitution is now gated on the caller being the title module, which
    // is not known until CreateDevice runs -- so a seed placed here could not be
    // delivered to anyone anyway. g_bb_w is set from the real backbuffer in
    // CreateDevice/d3d_prepare_game_window, and everything else that reads it
    // (dinput's absolute mapping, the aspect clamp) already falls back sanely
    // while it is still zero.

    logf("[d3d] enable=1 trace=%d windowed=%d | Direct3DCreate8=%p",
         g_d3d_trace, g_d3d_windowed, real_Direct3DCreate8);
    if (!real_Direct3DCreate8) {
        logf("[d3d] WARN: Direct3DCreate8 unresolved -- the game hooks are dead");
        return;
    }
    // Resolve FIRST, then patch: GetProcAddress above must return the genuine
    // function, not our own thunk, or the hook would call itself.
    dx_eat_patch_export(m, "Direct3DCreate8", (void*)hook_Direct3DCreate8,
                     "d3d8!Direct3DCreate8 (beats the packed TM.dll re-resolving "
                     "its imports, which the IAT patch alone loses to)");

    // Cursor API, same treatment and for the same reason -- TM.dll is packed, so
    // an IAT patch on it is a race we lose.
    if (g_d3d_cursor) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) {
            real_SetCursorPos = (PFN_SetCursorPos)GetProcAddress(u, "SetCursorPos");
            real_GetCursorPos = (PFN_GetCursorPos)GetProcAddress(u, "GetCursorPos");
            real_ClipCursor   = (PFN_ClipCursor)  GetProcAddress(u, "ClipCursor");
            if (real_SetCursorPos)
                dx_eat_patch_export(u, "SetCursorPos", (void*)hook_SetCursorPos, "user32!SetCursorPos");
            if (real_GetCursorPos)
                dx_eat_patch_export(u, "GetCursorPos", (void*)hook_GetCursorPos, "user32!GetCursorPos");
            if (real_ClipCursor)
                dx_eat_patch_export(u, "ClipCursor", (void*)hook_ClipCursor, "user32!ClipCursor");
            real_SetCursor = (PFN_SetCursor)GetProcAddress(u, "SetCursor");
            if (real_SetCursor)
                dx_eat_patch_export(u, "SetCursor", (void*)hook_SetCursor, "user32!SetCursor");

            // ShowCursor is the COUNT half -- see the [cur] ShowCursor hook. Pure
            // attribution: which module drives the display count negative over a
            // title that draws no pointer of its own.
            real_ShowCursor = (PFN_ShowCursor)GetProcAddress(u, "ShowCursor");
            if (real_ShowCursor)
                dx_eat_patch_export(u, "ShowCursor", (void*)hook_ShowCursor, "user32!ShowCursor");

            // Mouse capture -- see the [cap] hooks. A window that holds the
            // capture gets NO non-client hit-testing, so its frame goes inert
            // while its client area still works; that is the measured state of a
            // stuck FMO window. These log the caller and pass through.
            real_SetCapture     = (PFN_SetCapture)    GetProcAddress(u, "SetCapture");
            real_ReleaseCapture = (PFN_ReleaseCapture)GetProcAddress(u, "ReleaseCapture");
            if (real_SetCapture)
                dx_eat_patch_export(u, "SetCapture", (void*)hook_SetCapture, "user32!SetCapture");
            if (real_ReleaseCapture)
                dx_eat_patch_export(u, "ReleaseCapture", (void*)hook_ReleaseCapture,
                                    "user32!ReleaseCapture");

            // Focus APIs -- who actually grabs the window back.
            real_GetWindowRect = (PFN_GetWindowRect)GetProcAddress(u, "GetWindowRect");
            if (real_GetWindowRect)
                dx_eat_patch_export(u, "GetWindowRect", (void*)hook_GetWindowRect, "user32!GetWindowRect");
            real_SetForegroundWindow = (PFN_SetForegroundWindow)GetProcAddress(u, "SetForegroundWindow");
            real_BringWindowToTop    = (PFN_BringWindowToTop)   GetProcAddress(u, "BringWindowToTop");
            real_SetActiveWindow     = (PFN_SetActiveWindow)    GetProcAddress(u, "SetActiveWindow");
            if (real_SetForegroundWindow)
                dx_eat_patch_export(u, "SetForegroundWindow", (void*)hook_SetForegroundWindow,
                                    "user32!SetForegroundWindow");
            if (real_BringWindowToTop)
                dx_eat_patch_export(u, "BringWindowToTop", (void*)hook_BringWindowToTop,
                                    "user32!BringWindowToTop");
            if (real_SetActiveWindow)
                dx_eat_patch_export(u, "SetActiveWindow", (void*)hook_SetActiveWindow,
                                    "user32!SetActiveWindow");
            real_GetSystemMetrics = (PFN_GetSystemMetrics)GetProcAddress(u, "GetSystemMetrics");

            if (real_GetSystemMetrics)

                dx_eat_patch_export(u, "GetSystemMetrics", (void*)hook_GetSystemMetrics,

                                    "user32!GetSystemMetrics");

            logf("[cur] cursor mode=%d (0 off, 1 log, 2 translate), nosteal=%d",
                 g_d3d_cursor, g_d3d_nosteal);
        }
    }
}

void d3d8_summary()
{
    if (!g_d3d_enable) return;
    logf("[d3d] summary: CreateDevice=%ld (fullscreen requests=%ld) devices=%ld "
         "Reset=%ld forced-windowed=%ld retries=%ld",
         g_n_create, g_n_fullscreen_seen, g_n_device, g_n_reset,
         g_n_forced, g_n_retry);
    if (g_d3d_fitvideo)
        logf("[vid] summary: %ld DirectShow video window(s) adopted onto the game "
             "(0 = none were misplaced, which is the normal case for a title that "
             "positions its own FMV)", g_n_video_adopted);
    if (!g_n_create)
        logf("[d3d] NOTE: CreateDevice never ran -- no game was launched this "
             "session. If a game DID launch, the EAT patch on d3d8.dll is the "
             "thing to doubt: the content modules are POL1-packed and re-resolve "
             "their imports, so an IAT patch alone is lost (measured on TM.dll)");
    else if (g_n_fullscreen_seen)
        logf("[d3d] NOTE: %ld fullscreen device request(s) seen. THAT is what "
             "changes the monitor mode -- not DirectDraw, not ChangeDisplaySettings. "
             "Set [dx] d3d_windowed=1 to override it.", g_n_fullscreen_seen);
    logf("[cur] summary: SetCursorPos=%ld ClipCursor=%ld GetCursorPos=%ld "
         "translated=%ld", g_n_setpos, g_n_clip, g_n_getpos, g_n_xlated);
    // An UNBALANCED pair is the finding: every SetCapture must have a matching
    // ReleaseCapture, and a surplus of SetCapture is a window left holding the
    // mouse -- i.e. a frame that can no longer be dragged or sized.
    logf("[cap] summary: SetCapture=%ld ReleaseCapture=%ld%s",
         g_n_setcap, g_n_relcap,
         (g_n_setcap > g_n_relcap)
            ? "  <-- UNBALANCED: more grabs than releases, so the capture was "
              "left held. Read the last [cap] SetCapture line for the caller."
            : "");
    logf("[mode] summary: GetDisplayMode faked=%ld GetSystemMetrics faked=%ld "
         "left-real-for-non-title=%ld (screen reported as %ux%u)",
         g_n_modefaked, g_n_metricsfaked, g_n_metrics_shell, g_bb_w, g_bb_h);
    // faked=0 with a non-zero suppression count is the interesting case: the
    // screen-size substitution is armed and NOTHING in the title is asking for
    // it. That is the state to be in before deciding d3d_fakemode has a purpose.
    if (g_d3d_fakemode && !g_n_metricsfaked && g_n_metrics_shell)
        logf("[mode] NOTE: no title asked for the screen size this session; all "
             "%ld SM_CXSCREEN/SM_CYSCREEN calls came from the shell and got the "
             "real desktop. See the [mode] lines above for which module.",
             g_n_metrics_shell);
    if (g_d3d_renderspy) {
        LONG tot = g_rs_draw_total ? g_rs_draw_total : 1;
        logf("[rs] summary: draws total=%ld  2D/XYZRHW=%ld (%ld%%)  3D/XYZ=%ld (%ld%%)  "
             "other=%ld  |  *UP(inline-verts)=%ld  |  PROJECTION persp=%ld ortho=%ld",
             g_rs_draw_total, g_rs_draw_2d, g_rs_draw_2d * 100 / tot,
             g_rs_draw_3d, g_rs_draw_3d * 100 / tot, g_rs_draw_other,
             g_rs_up_calls, g_rs_proj_persp, g_rs_proj_ortho);
        // The verdict, in one line, so no one has to interpret the counters.
        if (g_rs_draw_total == 0)
            logf("[rs] VERDICT: no draws seen -- renderspy hooked but no frame drawn "
                 "(did a game actually run?).");
        else if (g_rs_draw_2d * 100 / tot >= 90 && g_rs_proj_persp == 0)
            logf("[rs] VERDICT: essentially ALL 2D screen-space. HD = per-draw vertex "
                 "rescale (invasive) AND still capped by 640x480-era textures -- low payoff.");
        else if (g_rs_proj_persp > 0 && g_rs_draw_3d * 100 / tot >= 25)
            logf("[rs] VERDICT: meaningful 3D content (perspective projection + %ld%% "
                 "transformed draws). HD = viewport/backbuffer bump, real gain on the 3D. "
                 "Worth pursuing.", g_rs_draw_3d * 100 / tot);
        else
            logf("[rs] VERDICT: MIXED. Some 3D but mostly 2D -- HD helps the 3D parts "
                 "only; weigh effort vs. that partial gain.");
    }
    logf("[foc] summary: SetForegroundWindow=%ld (blocked %ld) "
         "BringWindowToTop=%ld SetActiveWindow=%ld",
         g_n_fg, g_n_fg_blocked, g_n_btt, g_n_setactive);
    for (int i = 0; i < g_nspy; i++)
        logf("[msg] summary: %s window %p saw %ld mouse message(s)",
             g_spy[i].tag, g_spy[i].h, g_spy[i].seen);
    if (g_d3d_msgspy && g_nspy && !g_spy[0].seen)
        logf("[msg] NOTE: the watched windows got NO mouse messages at all. The "
             "pointer is coming from polcore's input layer (CreateInput was given "
             "the mask window), not from window messages -- look in the common "
             "function table next, not here.");
    if (!g_n_fg && !g_n_btt)
        logf("[foc] NOTE: nothing called SetForegroundWindow or BringWindowToTop. "
             "The window is being taken by some other route -- a WM_ACTIVATE "
             "handler, or a topmost restyle -- not by a focus API.");
    if (g_d3d_cursor && !g_n_setpos && !g_n_clip)
        logf("[cur] NOTE: the game called NEITHER SetCursorPos NOR ClipCursor. "
             "It is doing its pointer with DirectInput instead (TM.dll imports "
             "DINPUT8!DirectInput8Create) -- translate there, not here.");
}

// ---------------------------------------------------------------------------
// title_reset_state -- forget everything that was true of the title that ended.
// ---------------------------------------------------------------------------
//
// Declared up beside g_title_gen; defined HERE because it clears state declared
// all the way down this file, and a reset that silently omits a variable is the
// bug it is meant to fix.
//
// What is deliberately NOT in this list: g_saved_cw / g_saved_ch, the remembered
// window SIZE. That is the user's own choice, it is persisted to the ini on
// purpose, and it must survive the title that was running when they chose it.
// What must NOT survive is g_user_sized -- the "do not re-fit this window"
// LATCH -- because the window it was asserted about no longer exists. Those two
// living in one feature is what made "fit_window: SKIPPED" outlive its title and
// take the frame with it.
static void title_reset_state(void)
{
    // Which title we are in. The game window's icon is resolved from it, and a
    // stale value is why launches 2..4 wore pol.exe's icon instead of the
    // title's own.
    g_title_module = NULL;

    // Window identity. A destroyed HWND read by a later hook is at best a no-op
    // and, once Windows recycles the value, an action on somebody else's window.
    g_game_window   = NULL;
    g_focus_window  = NULL;
    g_taskbar_fixed = NULL;   // the new game window needs its OWN taskbar button

    // "We already did the once-per-title thing."
    InterlockedExchange(&g_shim_raised, 0);   // the new title may present itself
    InterlockedExchange(&g_user_sized,  0);   // <- the fit_window: SKIPPED bug
    InterlockedExchange(&g_clip_applied, 0);

    // Focus/startup grace. g_game_born_tick is only ever assigned when it is
    // zero, so without this the d3d_nosteal startup grace is measured from the
    // FIRST title of the session and every later title launches with none of it.
    g_game_born_tick = 0;
    InterlockedExchange(&g_game_ever_fg, 0);

    // Window-state flags whose only writer is spy_proc, describing a window that
    // is now destroyed. Left set, they hold the NEXT title's window down.
    InterlockedExchange(&g_user_minimized, 0);
    InterlockedExchange(&g_in_modal, 0);

    // Device and backbuffer facts.
    g_game_device = NULL;
    g_pp_have     = false;
    g_bb_w = g_bb_h = 0;
    InterlockedExchange(&g_dev_fullscreen, -1);   // unknown until the next device

    // The Viewer's input HWND slot, repointed per title by inspect_input_hwnd.
    g_saved_input_hwnd = NULL;
    g_input_hwnd_slot  = NULL;

    // The previous title's windows are gone; drop their subclass records so the
    // table cannot fill and start refusing to watch new windows.
    spy_reset_for_new_title();

    // Give back any cursor display count we were holding for the old title's
    // frame. The pointer can be sitting on the caption at the moment the title
    // quits, in which case no client WM_MOUSEMOVE ever arrives to balance it and
    // the boost would ride into the next title -- where it would draw the OS
    // pointer on top of the game's own.
    nc_cursor_restore();

    // Per-title latches: the previous title's identity must not leak into the
    // next launch.
    g_title_native_windowed = false;
    g_title_leaf[0] = 0;
}

// ---------------------------------------------------------------- live display switch
//
// Alt+Enter ([settings] display_hotkey). Windowed <-> borderless for the title that
// is running, by writing ITS per-title row and re-running the same reload + fit that
// a launch does. Not fullscreen: leaving or entering an exclusive device needs the
// title's own Reset, which nothing here can ask for -- that choice is made in the
// settings and applies at the next launch. Fantasy Earth places its own window
// (-windowmode) and fit_window never touches it, so it is windowed-only for now.
// WHICH TITLE IS THIS, REALLY.
//
// g_title_leaf is set from the CreateDevice RETURN ADDRESS, and with an add-on core
// in the process that address is inside the CORE. Measured live 2026-09-08: a Tetra
// Master session logged `[title] TITLE #2: Ashita.dll -- no compat profile`, so the
// first Alt+Enter wrote `[dx.Ashita.dll] display` and Tetra Master's own choice was
// never stored. Front Mission was unaffected because it is the one d3d9 title and
// Ashita hooks d3d8 only -- exactly the split that hid the mask realign for a fortnight.
//
// A core is not a title and has no profile row, so when the leaf names something the
// profile table does not know, ask which PROFILED module is actually loaded instead.
// Only one title runs at a time, which makes that answer exact rather than a guess.
static const char* display_title_leaf(void)
{
    const char* leaf = g_title_leaf;
    if (leaf && leaf[0] && profile_for_module(leaf)) return leaf;
    for (int i = 0; i < profiles_count(); i++) {
        const TitleProfile* p = profiles_at(i);
        if (p && p->module && p->module[0] && GetModuleHandleA(p->module)) {
            if (leaf && leaf[0] && _stricmp(leaf, p->module) != 0)
                logf("[display] the device caller is %s, which is not a title -- this is "
                     "%s, and its setting is stored under that name", leaf, p->module);
            return p->module;
        }
    }
    return leaf;
}

static bool spy_watching(HWND h)
{
    for (int i = 0; i < g_nspy; i++) if (g_spy[i].h == h && IsWindow(h)) return true;
    return false;
}

bool d3d_display_toggle(char* why, size_t cap)
{
    if (why && cap) why[0] = 0;
    HWND h = g_game_window;
    const char* leaf = display_title_leaf();
    if (!h || !IsWindow(h) || !leaf || !leaf[0]) {
        if (why) _snprintf_s(why, cap, _TRUNCATE, "no game window is up");
        return false;
    }
    HWND fg = GetForegroundWindow();
    DWORD fgpid = 0; if (fg) GetWindowThreadProcessId(fg, &fgpid);
    if (fgpid != GetCurrentProcessId()) {
        if (why) _snprintf_s(why, cap, _TRUNCATE, "the game is not in front");
        return false;
    }
    if (!g_ini_path[0]) {
        if (why) _snprintf_s(why, cap, _TRUNCATE, "no ini path known");
        return false;
    }
    DisplayMode cur = display_mode_for(leaf, NULL);
    if (cur == DM_UNSET)
        cur = !g_d3d_windowed ? DM_FULLSCREEN
            : g_d3d_borderless >= 2 ? DM_BORDERLESS_CRISP
            : g_d3d_borderless ? DM_BORDERLESS : DM_WINDOWED;
    if (cur == DM_FULLSCREEN) {
        if (why) _snprintf_s(why, cap, _TRUNCATE,
                             "%s is running exclusive fullscreen; choose windowed or "
                             "borderless in its Display setting and relaunch", leaf);
        return false;
    }
    DisplayMode next = (cur == DM_WINDOWED) ? DM_BORDERLESS : DM_WINDOWED;

    wchar_t sec[96], wleaf[80];
    if (MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf, _countof(wleaf)) <= 0) return false;
    _snwprintf_s(sec, _countof(sec), _TRUNCATE, L"dx.%s", wleaf);
    if (!WritePrivateProfileStringW(sec, L"display", display_mode_name(next), g_ini_path)) {
        if (why) _snprintf_s(why, cap, _TRUNCATE, "could not write the ini (%lu)", GetLastError());
        return false;
    }
    // Same path a launch takes: the title's scope, the per-module reloads, then the
    // fit. The user-sized latch is cleared on purpose -- they asked for a new shape.
    shim_reload_for_title(g_ini_path, leaf);
    InterlockedExchange(&g_user_sized, 0);
    bool posted = spy_watching(h) && PostMessageA(h, WM_POLSHIM_REFIT, 0, 0);
    if (!posted) fit_window(h, g_bb_w, g_bb_h);
    logf("[display] %s: %ls -> %ls (hotkey) -- written to [%ls] display and the window "
         "refitted %s", leaf, display_mode_name(cur), display_mode_name(next), sec,
         posted ? "on its own thread" : "directly");
    if (why) _snprintf_s(why, cap, _TRUNCATE, "%s is now %ls", leaf, display_mode_name(next));
    return true;
}
