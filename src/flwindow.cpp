// flwindow.cpp -- make the Friend List's shaped window composite correctly on
// the Steam Deck instead of showing a black surround.
//
// THE SYMPTOM (2026-08-23, standalone PolFL on the Deck in KDE desktop mode).
// On Windows the Friend List is "a transparent little chat window": FriendList.dll
// builds a large top-level window of class "WInFriendListWindow" (measured
// 1280x746) and clips it to the chat shape with SetWindowRgn + CreateRectRgn --
// NOT layered transparency (SetLayeredWindowAttributes has zero references in the
// image). The desktop shows through the un-regioned area.
//
// Under Wine/KWin the same window renders its whole rectangle BLACK around the
// chat. A window shaped only by SetWindowRgn -- with no WS_EX_LAYERED -- is not
// alpha-composited by KWin, so the area outside the region is painted opaque
// (black, the class background) rather than left transparent.
//
// THE FIX. When that window appears, give it WS_EX_LAYERED and a black colour
// key (SetLayeredWindowAttributes LWA_COLORKEY, RGB 0,0,0). KWin then composites
// it and keys the black surround out, so the chat floats over the desktop the
// way it does on Windows. Harmless on Windows too (the region already clips, so
// there is no black to key), and scoped to the one window class, so nothing else
// is touched.
//
// [dx] fl_window_transparent, default ON. Triggered from hooks the FL drives on
// its own window through the standard USER32 IAT (ShowWindow, and SetWindowRgn
// which also tells us the chat's real bounds) -- patch_iat swaps them by value.

#include "polshim.h"
#include <windows.h>

static int g_on = 1;

typedef BOOL (WINAPI* PFN_ShowWindow)(HWND, int);
typedef int  (WINAPI* PFN_SetWindowRgn)(HWND, HRGN, BOOL);
static PFN_ShowWindow   g_real_show = NULL;
static PFN_SetWindowRgn g_real_rgn  = NULL;

static bool is_fl_window(HWND h)
{
    if (!h) return false;
    wchar_t cls[64] = L"";
    if (!GetClassNameW(h, cls, _countof(cls))) return false;
    // Class is "WInFriendListWindow" (SE's own capitalisation). Match the stem
    // case-insensitively so a build-to-build casing change cannot silently miss.
    return wcsstr(cls, L"FriendList") != NULL || wcsstr(cls, L"friendlist") != NULL;
}

// Give the FL window a black colour key so KWin composites the surround away.
// Idempotent: re-applying the same style/attrs is harmless, so it is safe to
// call on every ShowWindow.
static void make_transparent(HWND h)
{
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED))
        SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(h, RGB(0, 0, 0), 0, LWA_COLORKEY);

    static bool logged = false;
    if (!logged) {
        logged = true;
        RECT r = {};
        GetWindowRect(h, &r);
        logf("[flwin] WInFriendListWindow %p made layered + black colour-key "
             "(%ldx%ld) -- the black surround keys out under the compositor",
             h, r.right - r.left, r.bottom - r.top);
        log_flush();
    }
}

static BOOL WINAPI hook_ShowWindow(HWND h, int cmd)
{
    BOOL r = g_real_show(h, cmd);
    if (g_on && cmd != SW_HIDE && is_fl_window(h)) make_transparent(h);
    return r;
}

static int WINAPI hook_SetWindowRgn(HWND h, HRGN rgn, BOOL redraw)
{
    int r = g_real_rgn(h, rgn, redraw);
    if (g_on && is_fl_window(h)) {
        RECT box = {};
        if (rgn && GetRgnBox(rgn, &box))
            logf("[flwin] SetWindowRgn on the FL window -- chat shape bounds "
                 "(%ld,%ld)-(%ld,%ld)", box.left, box.top, box.right, box.bottom);
        make_transparent(h);
    }
    return r;
}

void flwindow_init(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"dx", L"fl_window_transparent", 1, ini) != 0;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        g_real_show = (PFN_ShowWindow)  GetProcAddress(u32, "ShowWindow");
        g_real_rgn  = (PFN_SetWindowRgn)GetProcAddress(u32, "SetWindowRgn");
    }
    if (!g_real_show || !g_real_rgn) g_on = 0;
    if (!g_on) logf("[flwin] off (fl_window_transparent=0%s)",
                    (!g_real_show || !g_real_rgn) ? ", or user32 unresolved" : "");
}

// patch_iat by-value swap accessors. NULL `from` never matches, so off leaves
// every IAT untouched.
void* flwindow_real_ShowWindow()   { return g_on ? (void*)g_real_show : NULL; }
void* flwindow_hook_ShowWindow()   { return g_on ? (void*)hook_ShowWindow : NULL; }
void* flwindow_real_SetWindowRgn() { return g_on ? (void*)g_real_rgn  : NULL; }
void* flwindow_hook_SetWindowRgn() { return g_on ? (void*)hook_SetWindowRgn : NULL; }
