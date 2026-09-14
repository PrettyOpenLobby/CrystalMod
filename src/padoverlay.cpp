// padoverlay.cpp -- the controller mapper, drawn ON the Viewer's own DirectDraw
// surface and driven entirely by the pad.
//
// WHY THIS EXISTS
//
// padmap.cpp does the mapping correctly and has done since build 5x. What it never had
// was a way to SEE itself on the machine where it matters. The only feedback surface is
// the "Controller mapping..." window in polsettings.cpp, and polsettings.cpp's own header
// has recorded the defect since the day it was written:
//
//     "the Viewer's shell is DirectDraw and may hold the display EXCLUSIVELY. Over an
//      exclusive-fullscreen surface a normal top-level window can be invisible even
//      though it exists and has focus, which reads exactly like the chord did nothing."
//
// On a Steam Deck that is not a corner case, it is the normal path: the shell owns the
// display, the mapper window is created, is focused, receives every click -- and is
// never drawn. So the account holder gets a feature whose entire purpose is live
// feedback, offering none, while adding four ini keys of new variables to a problem that
// already had too many. That is the report this module answers.
//
// THE FIX IS TO STOP ASKING THE WINDOW MANAGER FOR A WINDOW. dxhook.cpp already
// intercepts the shell's present -- the Blt whose destination is the windowed primary
// (dxhook.cpp, Blt_hook) -- so the mapper can be drawn straight onto that surface after
// the shell has finished with it. A DirectDraw surface hands out a GDI DC on demand
// (GetDC/ReleaseDC), so this is plain TextOut on the frame the user is already looking
// at. Nothing is asked of the compositor, nothing can be occluded, and gamescope is not
// involved: if the shell can draw, so can we.
//
// AND THE PAD DRIVES IT. Every input arrives through padmap's observer, which already
// sees the raw DIJOYSTATE before the permutation. While the overlay is up those buttons
// are swallowed on the way to the client (the same trick padmap's capture already uses),
// so pressing buttons to map them cannot also press things in the shell behind.
//
// THE NAVIGATION RULE, and why it is the D-PAD that acts and never a face button:
// the toggle chord is itself a pair of buttons, and a face button that BINDS on sight
// would bind whatever the chord happened to be. So:
//
//     D-pad up/down    move the selection
//     D-pad right      ARM the selected row -- "press the button you want"
//     D-pad left       clear that row's binding
//     any button       binds ONLY while armed; otherwise it is just shown as pressed
//
// The hat is the one control padmap deliberately never permutes (it is a POV, not a
// button), so it means the same thing on every pad and cannot be remapped out from under
// the mapper -- which is the property the UI of a remapper needs above all others.
//
// WHAT THIS IS NOT: it is not a second mapping engine. Every edit goes through
// padmap_bind / padmap_preset / padmap_save exactly as the dialog's do, and the two are
// interchangeable -- bind on the desktop in the window, or on the Deck on the surface,
// and the same pad_bind line comes out. This module owns pixels and a cursor, nothing
// more.
//
// SCOPE: the Viewer SHELL. A title that renders through d3d8/d3d9 does not present
// through this Blt, and a title's pad is a separate map besides (padmap's per-title
// path); the overlay reports that rather than pretending to cover it.

#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <ddraw.h>
#include <dinput.h>   // the self-test drives a real DIJOYSTATE through padmap
#include <stdio.h>
#include "polshim.h"

// ---------------------------------------------------------------------------
// rows -- the four mappable actions, then the two commands
// ---------------------------------------------------------------------------

#define ROW_PRESET  PAD_NACTIONS          // 4
#define ROW_MEASURE (PAD_NACTIONS + 1)    // 5
#define ROW_SAVE    (PAD_NACTIONS + 2)    // 6
#define ROW_COUNT   (PAD_NACTIONS + 3)

#define PO_MAXBTN 32

static int  g_on      = 1;                // [inputmode] pad_overlay -- may it ever open
static int  g_trace   = 0;                // [inputmode] pad_overlay_trace
static volatile LONG g_active = 0;        // is it open right now
static volatile LONG g_sel    = 0;        // selected row
static volatile LONG g_arm    = 0;        // waiting for a button to bind g_sel
static volatile LONG g_dirty  = 0;        // an edit is unsaved
static wchar_t g_ini[MAX_PATH] = L"";

// Edge state for the pad. Written on the client's input thread only.
static BYTE  g_prev[PO_MAXBTN];
static DWORD g_prev_pov = 0xFFFFFFFF;
static volatile LONG g_last_bound = -1;   // for the "you just bound X" line

// MEASURING -- which slot the client really reads for an action, as opposed to which
// physical button we hand it. Everything else in this UI is a BINDING; this is the only
// thing that is a MEASUREMENT, and on a Steam Deck it is the one that decides whether any
// of it works: this install reads Confirm from slot 2 and Cancel from slot 0, nothing like
// POL's documented defaults, so with no measurement the permutation delivers buttons to
// slots the client never looks at and the pad goes completely dead. It existed only in the
// settings dialog -- which cannot be seen on a Deck -- so clearing it there left the
// machine with no way to put it back. That is why it is here.
static volatile LONG g_measuring   = -1;  // action being measured, or -1
static volatile LONG g_measure_was = 0;   // pad_remap to restore when the wizard ends
static volatile LONG g_last_press  = -1;  // physical index last pressed, for the readout

// A note the overlay shows for one moment -- "bound A to Confirm", "saved".
static char          g_note[128] = "";
static volatile LONG g_note_when = 0;

static void note(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(g_note, sizeof(g_note), _TRUNCATE, fmt, ap);
    va_end(ap);
    InterlockedExchange(&g_note_when, (LONG)GetTickCount());
}

int padoverlay_enabled() { return g_on; }
int padoverlay_active()  { return (int)g_active; }

// padmap asks this on every poll: hide the BUTTONS from the client while we own them.
//
// Not while MEASURING. A measurement asks "which button does the client treat as Confirm
// today", and the only honest way to know is to press it and watch the client react -- so
// during the wizard the buttons are let through, unpermuted (the wizard forces pad_remap
// off), and the shell behaves normally behind the panel. Swallowing there would ask the
// user to recall a fact the machine is right there able to demonstrate.
//
// The HAT is always ours while the overlay is up -- see padmap_on_state. That is what
// keeps a commit gesture available even while every button is reaching the client.
int padoverlay_swallows() { return g_active && g_measuring < 0; }

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

// Defined below with the wizard; needed here because closing must end a measurement in
// progress and restore pad_remap.
static void measure_end(const char* why);

void padoverlay_close(int save)
{
    if (!InterlockedExchange(&g_active, 0)) return;
    InterlockedExchange(&g_arm, 0);
    // Closing mid-wizard must restore pad_remap. Leaving it off would hand back a pad
    // with no mapping at all and no visible reason -- the exact failure this panel exists
    // to make impossible.
    measure_end("closed mid-measure");
    if (save && g_ini[0]) {
        padmap_save(g_ini);
        logf("[padui] closed -- bindings SAVED to %ls", g_ini);
    } else {
        logf("[padui] closed%s", g_dirty ? " -- edits left live but NOT saved to the ini "
                                           "(they revert on the next launch)" : "");
    }
    InterlockedExchange(&g_dirty, 0);
}

void padoverlay_open()
{
    if (!g_on) {
        logf("[padui] overlay is disabled ([inputmode] pad_overlay=0) -- ignoring the chord");
        return;
    }
    if (InterlockedExchange(&g_active, 1)) return;
    InterlockedExchange(&g_sel, 0);
    InterlockedExchange(&g_arm, 0);
    InterlockedExchange(&g_dirty, 0);
    for (int i = 0; i < PO_MAXBTN; i++) g_prev[i] = 0;
    g_prev_pov = 0xFFFFFFFF;
    g_note[0] = 0;
    char prod[128] = "";
    int ndev = padmap_device_seen(prod, sizeof(prod));
    logf("[padui] OPEN -- on-surface mapper. device=%s remap=%d",
         ndev ? (prod[0] ? prod : "(unnamed)") : "NONE YET", padmap_enabled());
}

void padoverlay_toggle()
{
    if (g_active) padoverlay_close(0);
    else          padoverlay_open();
}

// ---------------------------------------------------------------------------
// input -- fed from padmap's observer, which sees the pad BEFORE the permutation
// ---------------------------------------------------------------------------
//
// Raw state, deliberately: the mapper must show the physical button you pressed, not
// the logical one the permutation would hand the client. Showing the permuted view here
// is how a mapper becomes a hall of mirrors -- press A, see "B", rebind, repeat.

enum { POV_NONE = 0, POV_UP, POV_RIGHT, POV_DOWN, POV_LEFT };

// DirectInput reports the hat in hundredths of a degree, 0 = up, clockwise; centred is
// documented as -1 but drivers also report 0xFFFF in the low word, so both are tested.
static int pov_dir(DWORD pov)
{
    if (pov == 0xFFFFFFFF || (pov & 0xFFFF) == 0xFFFF) return POV_NONE;
    DWORD deg = (pov / 100) % 360;
    if (deg >= 315 || deg <  45) return POV_UP;
    if (deg >=  45 && deg < 135) return POV_RIGHT;
    if (deg >= 135 && deg < 225) return POV_DOWN;
    return POV_LEFT;
}

// Turning a binding on must turn the ENGINE on, or the mapper shows a perfect map that
// is not being applied -- the exact "looks configured, does nothing" failure padmap.cpp's
// header was written about.
static void arm_engine_if_bound()
{
    for (int a = 0; a < PAD_NACTIONS; a++)
        if (padmap_binding(a) >= 0) { padmap_set_enabled(1); return; }
}

static void do_preset_cycle(int delta)
{
    // The layouts, then "off" as a final entry, so a preset row can also clear.
    int n = padmap_layout_count() + 1;
    static int cur = -1;
    if (cur < 0) cur = 0;
    cur = ((cur + delta) % n + n) % n;
    if (cur < padmap_layout_count()) {
        padmap_preset(padmap_layout_key(cur));
        note("preset: %s", padmap_layout_label(cur));
    } else {
        padmap_preset("off");
        note("preset: cleared");
    }
    arm_engine_if_bound();
    InterlockedExchange(&g_dirty, 1);
}

static void measure_start()
{
    InterlockedExchange(&g_measure_was, padmap_enabled());
    padmap_set_enabled(0);          // a measurement taken THROUGH a permutation measures
                                    // the permutation -- the trap this whole feature has
                                    // fallen into twice. Off for the duration.
    InterlockedExchange(&g_measuring, PAD_OK);
    InterlockedExchange(&g_last_press, -1);
    note("press what CONFIRMS today, then D-pad right");
    logf("[padui] MEASURE started -- pad_remap forced off for the duration");
}

static void measure_end(const char* why)
{
    if (g_measuring < 0) return;
    InterlockedExchange(&g_measuring, -1);
    padmap_set_enabled((int)g_measure_was);
    InterlockedExchange(&g_last_press, -1);
    logf("[padui] MEASURE %s -- pad_remap restored to %ld", why, g_measure_was);
}

// Advance to the next action, or finish. Kept separate so "locked" and "skipped" cannot
// drift apart about what ends the wizard.
static void measure_next()
{
    int a = (int)g_measuring + 1;
    if (a >= PAD_NACTIONS) { measure_end("complete"); note("measured -- save to keep it"); return; }
    InterlockedExchange(&g_measuring, a);
    InterlockedExchange(&g_last_press, -1);
    note("press what %s does today, then D-pad right", padmap_action_name(a));
}

void padoverlay_feed(const unsigned char* btn, int nbtn, unsigned long pov)
{
    if (!g_active || !btn) return;
    if (nbtn > PO_MAXBTN) nbtn = PO_MAXBTN;

    // ---- MEASURING: the hat commits, the buttons are only observed ----------------
    if (g_measuring >= 0) {
        for (int i = 0; i < nbtn; i++) {
            BYTE down = btn[i] ? 1 : 0, was = g_prev[i];
            g_prev[i] = down;
            if (down && !was) InterlockedExchange(&g_last_press, i);
        }
        int d = pov_dir(pov), p = pov_dir(g_prev_pov);
        g_prev_pov = pov;
        if (d == POV_NONE || d == p) return;
        if (d == POV_RIGHT) {
            int got = (int)g_last_press;
            if (got < 0) { note("press a button first"); return; }
            padmap_learn_slot((int)g_measuring, got);
            char n[32];
            note("%s reads slot %d (%s)", padmap_action_name((int)g_measuring), got,
                 padmap_button_name(got, n, sizeof(n)));
            measure_next();
        } else if (d == POV_LEFT) {
            padmap_learn_slot((int)g_measuring, -1);   // unknown: fall back to the guess
            note("%s left unmeasured", padmap_action_name((int)g_measuring));
            measure_next();
        } else if (d == POV_DOWN) {
            measure_end("aborted");
            note("measuring cancelled");
        }
        InterlockedExchange(&g_dirty, 1);
        return;
    }

    // --- the hat: navigation, and the only control that ever acts on its own ---
    int dir = pov_dir(pov);
    int prev = pov_dir(g_prev_pov);
    g_prev_pov = pov;
    if (dir != POV_NONE && dir != prev) {
        int sel = (int)g_sel;
        switch (dir) {
        case POV_UP:   InterlockedExchange(&g_sel, (sel + ROW_COUNT - 1) % ROW_COUNT);
                       InterlockedExchange(&g_arm, 0); break;
        case POV_DOWN: InterlockedExchange(&g_sel, (sel + 1) % ROW_COUNT);
                       InterlockedExchange(&g_arm, 0); break;
        case POV_RIGHT:
            if (sel < PAD_NACTIONS) {
                InterlockedExchange(&g_arm, 1);
                note("press the button you want for %s", padmap_action_name(sel));
            } else if (sel == ROW_PRESET) {
                do_preset_cycle(+1);
            } else if (sel == ROW_MEASURE) {
                measure_start();
            } else {
                padoverlay_close(1);
                note("saved");
                return;
            }
            break;
        case POV_LEFT:
            if (sel < PAD_NACTIONS) {
                padmap_bind(sel, -1);
                InterlockedExchange(&g_arm, 0);
                InterlockedExchange(&g_dirty, 1);
                note("cleared %s", padmap_action_name(sel));
            } else if (sel == ROW_PRESET) {
                do_preset_cycle(-1);
            } else if (sel == ROW_MEASURE) {
                for (int a = 0; a < PAD_NACTIONS; a++) padmap_learn_slot(a, -1);
                InterlockedExchange(&g_dirty, 1);
                note("all measurements cleared -- back to POL's defaults (a GUESS)");
            }
            break;
        }
        if (g_trace) logf("[padui] hat=%d sel=%ld arm=%ld", dir, g_sel, g_arm);
    }

    // --- buttons: shown always, but they only BIND while armed ---
    for (int i = 0; i < nbtn; i++) {
        BYTE down = btn[i] ? 1 : 0, was = g_prev[i];
        g_prev[i] = down;
        if (!down || was) continue;                    // press edge only
        if (!InterlockedCompareExchange(&g_arm, 0, 1)) continue;   // not armed: display only
        int sel = (int)g_sel;
        if (sel < 0 || sel >= PAD_NACTIONS) continue;
        padmap_bind(sel, i);
        arm_engine_if_bound();
        InterlockedExchange(&g_last_bound, i);
        InterlockedExchange(&g_dirty, 1);
        char n[32];
        note("%s = %s", padmap_action_name(sel), padmap_button_name(i, n, sizeof(n)));
        logf("[padui] bound %s -> physical button %d", padmap_action_name(sel), i);
    }
}

// ---------------------------------------------------------------------------
// drawing -- GDI onto the DirectDraw surface the shell just presented
// ---------------------------------------------------------------------------
//
// A DirectDraw surface lends a DC (GetDC) that must be given back before the surface is
// used again, so everything here is between one GetDC/ReleaseDC pair and nothing is held
// across a frame. The font is created once: a CreateFont per present is a GDI handle
// leak measured in thousands per minute.

// ---------------------------------------------------------------------------
// XInput fallback -- so the mapper is navigable in the state it exists to REPORT
// ---------------------------------------------------------------------------
//
// The overlay is normally driven by padmap's observer, which is fed by the client's own
// DirectInput device. But the single most common broken state -- the one the overlay
// says "NO pad device" about -- is precisely the state where that device does not exist,
// and an overlay that draws a diagnosis you cannot scroll is the original complaint
// again in a new costume. So when the client is not polling a pad, the mapper reads
// XInput directly and drives itself from that.
//
// This is a FALLBACK and is labelled as one on screen: its index space is the standard
// enumeration order an XInput pad presents through DirectInput, which is an assumption,
// not a measurement (padmap_live_buttons carries the same caveat for the same reason).
// It is enough to navigate, and enough to bind something you then verify against the
// real device once one exists.

typedef struct { DWORD packet; struct { WORD buttons; BYTE lt, rt; SHORT tlx, tly, trx, trY; } pad; } PO_XI;
typedef DWORD (WINAPI *PFN_POXI)(DWORD, PO_XI*);
static PFN_POXI g_xi = NULL;
static int      g_xi_tried = 0;

static void xi_resolve()
{
    if (g_xi_tried) return;
    g_xi_tried = 1;
    static const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < _countof(dlls); i++) {
        HMODULE h = LoadLibraryW(dlls[i]);
        if (h && (g_xi = (PFN_POXI)GetProcAddress(h, "XInputGetState"))) return;
    }
}

// Same XInput-bit -> DirectInput-index table padmap's fallback uses. Kept identical on
// purpose: two fallbacks that disagreed about which index is "A" would be worse than
// none, because the mapper would teach you a number the engine does not use.
static const struct { WORD bit; int idx; } g_xi_di[] = {
    { 0x1000, 0 }, { 0x2000, 1 }, { 0x4000, 2 }, { 0x8000, 3 },   // A B X Y
    { 0x0100, 4 }, { 0x0200, 5 },                                  // LB RB
    { 0x0020, 6 }, { 0x0010, 7 },                                  // Back Start
    { 0x0040, 8 }, { 0x0080, 9 },                                  // L3 R3
};

static int xi_view(unsigned char* out, unsigned long* pov)
{
    xi_resolve();
    memset(out, 0, PO_MAXBTN);
    *pov = 0xFFFFFFFF;
    if (!g_xi) return 0;
    for (DWORD s = 0; s < 4; s++) {
        PO_XI st; ZeroMemory(&st, sizeof(st));
        if (g_xi(s, &st) != 0) continue;
        WORD b = st.pad.buttons;
        for (int i = 0; i < _countof(g_xi_di); i++)
            if (b & g_xi_di[i].bit) out[g_xi_di[i].idx] = 1;
        // The D-pad, rebuilt as a POV so the navigation rule is the same one the
        // DirectInput path uses -- one decoder, one set of directions.
        const bool up = (b & 0x0001) != 0, dn = (b & 0x0002) != 0;
        const bool lf = (b & 0x0004) != 0, rt = (b & 0x0008) != 0;
        if      (up && !dn && !lf && !rt) *pov = 0;
        else if (rt && !lf && !up && !dn) *pov = 9000;
        else if (dn && !up && !lf && !rt) *pov = 18000;
        else if (lf && !rt && !up && !dn) *pov = 27000;
        return 1;
    }
    return 0;
}

static HFONT g_font = NULL;
static int   g_lineh = 14;
static volatile LONG g_drawing = 0;      // re-entry guard: our GDI must never nest

static HFONT overlay_font()
{
    if (g_font) return g_font;
    // A fixed-pitch face so the columns line up. Courier New is present in a bare Wine
    // prefix where Consolas is not; if neither resolves GDI substitutes something
    // fixed-pitch rather than failing, and the stock font is the last resort.
    g_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
    if (!g_font) g_font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
    return g_font;
}

struct DrawCtx { HDC dc; int x; int y; };

static void line(DrawCtx* c, COLORREF col, const char* fmt, ...)
{
    char s[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(s, sizeof(s), _TRUNCATE, fmt, ap);
    va_end(ap);
    SetTextColor(c->dc, col);
    TextOutA(c->dc, c->x, c->y, s, (int)strlen(s));
    c->y += g_lineh;
}

// The live button strip. This is the single most important thing on the screen: it is
// the answer to "is the pad even reaching this process", which every previous round of
// this problem had to guess at.
static void draw_strip(DrawCtx* c, const unsigned char* btns, int nbtn)
{
    char s[256]; int n = 0;
    n += _snprintf_s(s + n, sizeof(s) - n, _TRUNCATE, "pressed now:  ");
    if (nbtn <= 0) {
        _snprintf_s(s + n, sizeof(s) - n, _TRUNCATE, "(no pad)");
    } else {
        for (int i = 0; i < nbtn && i < 16 && n < 200; i++)
            n += _snprintf_s(s + n, sizeof(s) - n, _TRUNCATE, "%s%d",
                             i ? " " : "", i);
    }
    SetTextColor(c->dc, RGB(150, 150, 150));
    TextOutA(c->dc, c->x, c->y, s, (int)strlen(s));

    // Overprint the indices that are DOWN in green, in place, so the strip reads as a
    // row of lamps rather than a list that changes length as you press things.
    if (nbtn > 0) {
        SIZE sz; GetTextExtentPoint32A(c->dc, "pressed now:  ", 14, &sz);
        int x = c->x + sz.cx;
        SIZE d; GetTextExtentPoint32A(c->dc, "0 ", 2, &d);
        for (int i = 0; i < nbtn && i < 16; i++) {
            char one[8]; _snprintf_s(one, sizeof(one), _TRUNCATE, "%d", i);
            if (btns[i]) {
                SetTextColor(c->dc, RGB(90, 255, 90));
                TextOutA(c->dc, x, c->y, one, (int)strlen(one));
            }
            x += d.cx;
        }
    }
    c->y += g_lineh;
}

void padoverlay_draw(void* surface, const void* dstrect)
{
    if (!g_active || !surface) return;

    // No DirectInput device polling us? Then nothing has fed the state machine and the
    // mapper would be a picture you cannot move. Drive it from XInput instead, on the
    // present tick -- which is a real clock, running at the shell's frame rate.
    // Deliberately BEFORE the draw guard, and g_active is re-tested after, because this
    // is the path on which the Save row can close the overlay.
    {
        unsigned char probe[PO_MAXBTN]; int nb = 0; const char* who = "none";
        if (!padmap_live_buttons(probe, &nb, &who) || strcmp(who, "game") != 0) {
            unsigned char b[PO_MAXBTN]; unsigned long pov = 0xFFFFFFFF;
            if (xi_view(b, &pov)) padoverlay_feed(b, PO_MAXBTN, pov);
        }
    }
    if (!g_active) return;

    if (InterlockedCompareExchange(&g_drawing, 1, 0) != 0) return;

    IDirectDrawSurface7* s = (IDirectDrawSurface7*)surface;
    HDC dc = NULL;
    HRESULT hr = s->GetDC(&dc);
    if (hr == DDERR_SURFACELOST) { s->Restore(); InterlockedExchange(&g_drawing, 0); return; }
    if (FAILED(hr) || !dc) {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            logf("[padui] GetDC on the primary failed (0x%08lX) -- the overlay cannot "
                 "draw on this surface; use the settings dialog instead", hr);
        InterlockedExchange(&g_drawing, 0);
        return;
    }

    // Where the shell just presented. Anchoring to that rect (rather than to 0,0) puts
    // the overlay inside the Viewer's picture on a scaled or letterboxed window instead
    // of in the corner of the desktop behind it.
    RECT area = { 0, 0, 640, 480 };
    if (dstrect) {
        area = *(const RECT*)dstrect;
    } else {
        DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
        d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT;
        if (SUCCEEDED(s->GetSurfaceDesc(&d))) {
            area.right  = (LONG)d.dwWidth;
            area.bottom = (LONG)d.dwHeight;
        }
    }

    HGDIOBJ oldf = SelectObject(dc, overlay_font());
    TEXTMETRICA tm;
    if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0) g_lineh = tm.tmHeight + 2;

    const int pad = 10;
    int w = (area.right - area.left) - 24;
    if (w > 620) w = 620;
    if (w < 240) w = 240;
    int rows = ROW_COUNT + 9;   // 6 rows + title + 2 status + strip + 2 help + the note
    int h = rows * g_lineh + pad * 2;
    RECT box;
    box.left   = area.left + 12;
    box.top    = area.top  + 12;
    box.right  = box.left + w;
    box.bottom = box.top  + h;

    HBRUSH bg = CreateSolidBrush(RGB(12, 12, 24));
    FillRect(dc, &box, bg);
    DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(RGB(120, 120, 200));
    FrameRect(dc, &box, fr);
    DeleteObject(fr);

    SetBkMode(dc, TRANSPARENT);
    DrawCtx c; c.dc = dc; c.x = box.left + pad; c.y = box.top + pad;

    unsigned char btns[PO_MAXBTN]; int nbtn = 0; const char* src = "none";
    int have = padmap_live_buttons(btns, &nbtn, &src);
    if (!have) { nbtn = 0; memset(btns, 0, sizeof(btns)); }

    line(&c, RGB(255, 255, 120), "POLSHIM CONTROLLER MAPPING");

    char prod[128] = "";
    int ndev = padmap_device_seen(prod, sizeof(prod));
    // "Is the client actually reading a pad" is a DIFFERENT question from "can I see my
    // buttons", and conflating them is what made this feature impossible to diagnose.
    // Both are answered, separately, in the two lines below.
    if (ndev) {
        line(&c, RGB(170, 170, 170), "pad: %.40s  (%s)  remapped polls: %d",
             prod[0] ? prod : "(unnamed)", src, padmap_remapped_count());
        line(&c, RGB(170, 170, 170), "mapping engine: %s",
             padmap_enabled() ? "ON" : "OFF (binding anything turns it on)");
    } else if (have) {
        line(&c, RGB(255, 210, 120),
             "The CLIENT is not reading a pad -- this is XInput, so indices are ASSUMED.");
        line(&c, RGB(255, 210, 120),
             "Set [inputmode] mode=force_gamepad, then re-check these here.");
    } else {
        line(&c, RGB(255, 140, 140),
             "NO pad at all. Put Steam Input in a CONTROLLER layout, not Keyboard/Mouse,");
        line(&c, RGB(255, 140, 140),
             "and set [inputmode] mode=force_gamepad. Nothing can be mapped until then.");
    }

    draw_strip(&c, btns, nbtn);
    c.y += 4;

    for (int r = 0; r < ROW_COUNT; r++) {
        bool sel = (r == (int)g_sel);
        COLORREF col = sel ? RGB(255, 255, 120) : RGB(225, 225, 225);
        const char* cur = sel ? ">" : " ";
        if (r < PAD_NACTIONS) {
            char n[32];
            padmap_button_name(padmap_binding(r), n, sizeof(n));
            int phys = padmap_binding(r);
            bool lit = (phys >= 0 && phys < nbtn && btns[phys]);
            if (lit) col = RGB(90, 255, 90);
            int conflict = padmap_slot_conflict(r);
            if (conflict >= 0) col = RGB(255, 120, 120);
            line(&c, col, "%s %-10s %-14s slot %-2d %s", cur,
                 padmap_action_name(r), n, padmap_pol_index(r, NULL, 0),
                 conflict >= 0 ? "<< CLASHES WITH THE ROW ABOVE -- NOT APPLIED"
                 : (sel && g_arm) ? "<< PRESS A BUTTON" : (lit ? "<< down" : ""));
        } else if (r == ROW_PRESET) {
            line(&c, col, "%s %-10s %s", cur, "Layout",
                 "left/right cycles a preset (deck / xbox / ps / off)");
        } else if (r == ROW_MEASURE) {
            int nmeas = 0;
            for (int a = 0; a < PAD_NACTIONS; a++) if (padmap_slot_observed(a) >= 0) nmeas++;
            if (g_measuring >= 0) col = RGB(120, 220, 255);
            line(&c, col, "%s %-10s %s", cur, "Measure",
                 g_measuring >= 0
                   ? "press the button, then right = lock / left = skip / down = stop"
                   : (nmeas ? "right = re-measure which slots the client reads"
                            : "right = MEASURE (needed here -- nothing is measured yet)"));
        } else {
            line(&c, col, "%s %-10s %s", cur, "Save",
                 g_dirty ? "right = save to polshim.ini and close  [UNSAVED EDITS]"
                         : "right = save to polshim.ini and close");
        }
    }

    c.y += 4;
    if (g_measuring >= 0) {
        char n[32];
        line(&c, RGB(120, 220, 255), "MEASURING %s -- buttons are LIVE, watch what they do",
             padmap_action_name((int)g_measuring));
        line(&c, RGB(120, 220, 255), "last pressed: %s",
             g_last_press >= 0 ? padmap_button_name((int)g_last_press, n, sizeof(n))
                               : "(none yet)");
    }
    line(&c, RGB(150, 150, 190),
         "D-pad: up/down move   right = bind / act   left = clear");
    line(&c, RGB(150, 150, 190),
         "chord again = close without saving");

    DWORD age = GetTickCount() - (DWORD)g_note_when;
    if (g_note[0] && age < 4000)
        line(&c, RGB(120, 220, 255), "%s", g_note);

    SelectObject(dc, oldf);
    s->ReleaseDC(dc);
    InterlockedExchange(&g_drawing, 0);
}

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------

void padoverlay_configure(const wchar_t* ini)
{
    g_on    = GetPrivateProfileIntW(L"inputmode", L"pad_overlay",       1, ini);
    g_trace = GetPrivateProfileIntW(L"inputmode", L"pad_overlay_trace", 0, ini);
    if (ini) wcsncpy_s(g_ini, ini, _TRUNCATE);
}

// Live-reload for the in-game settings dialog: both are plain gates read at
// chord / draw time, so new values apply on the next feed. The ini path is not
// re-copied -- it cannot change mid-session -- and there is nothing one-shot
// here to withhold.
void padoverlay_reload(const wchar_t* ini)
{
    g_on    = GetPrivateProfileIntW(L"inputmode", L"pad_overlay",       1, ini);
    g_trace = GetPrivateProfileIntW(L"inputmode", L"pad_overlay_trace", 0, ini);
    logf("[reload] padoverlay: pad_overlay=%d pad_overlay_trace=%d", g_on, g_trace);
}

// ---------------------------------------------------------------------------
// self-test -- the state machine is pure logic and needs no pad, no surface and no Deck
// ---------------------------------------------------------------------------

static int po_fail = 0;
static void po_check(const char* what, bool ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) po_fail++;
}

// One press-and-release of physical button `i`, fed the way the wire does.
static void press(int i)
{
    unsigned char b[PO_MAXBTN]; memset(b, 0, sizeof(b));
    b[i] = 1; padoverlay_feed(b, PO_MAXBTN, 0xFFFFFFFF);
    b[i] = 0; padoverlay_feed(b, PO_MAXBTN, 0xFFFFFFFF);
}

static void hat(int dir)
{
    static const DWORD deg[] = { 0xFFFFFFFF, 0, 9000, 18000, 27000 };
    unsigned char b[PO_MAXBTN]; memset(b, 0, sizeof(b));
    padoverlay_feed(b, PO_MAXBTN, deg[dir]);
    padoverlay_feed(b, PO_MAXBTN, 0xFFFFFFFF);      // release, so the next is an edge
}

int padoverlay_selftest()
{
    po_fail = 0;
    g_on = 1;
    g_ini[0] = 0;                                    // never write an ini from a test

    padmap_preset("off");
    padmap_set_enabled(0);

    // Closed, the overlay must be completely inert -- a stray press while it is not open
    // must never reach padmap_bind.
    padoverlay_close(0);
    press(3);
    po_check("closed: a button press binds nothing", padmap_binding(PAD_OK) < 0);
    po_check("closed: the pad is NOT swallowed from the client", padoverlay_swallows() == 0);

    padoverlay_open();
    po_check("open: the pad is swallowed while we own it", padoverlay_swallows() == 1);
    po_check("open: selection starts on the first action", (int)g_sel == PAD_OK);

    // THE RULE THIS MODULE IS BUILT AROUND: an unarmed press must not bind. The toggle
    // chord is itself buttons, so a mapper that bound on sight would bind the chord.
    press(5);
    po_check("unarmed press does NOT bind (the chord cannot bind itself)",
             padmap_binding(PAD_OK) < 0);

    // Arm, then press: that binds, and binding must turn the engine on -- a map that is
    // not applied is the failure mode this whole feature exists to end.
    hat(POV_RIGHT);
    po_check("right arms the selected row", (int)g_arm == 1);
    press(0);
    po_check("armed press binds the selected action", padmap_binding(PAD_OK) == 0);
    po_check("binding disarms (one press, one binding)", (int)g_arm == 0);
    po_check("binding turns the mapping engine ON", padmap_enabled() == 1);

    // A second press must not re-bind: arming is per press, not a mode.
    press(4);
    po_check("the NEXT press does not re-bind", padmap_binding(PAD_OK) == 0);

    // Navigation, and that moving cancels an arm (so an armed row you walked away from
    // cannot swallow a press meant for something else).
    hat(POV_RIGHT);
    hat(POV_DOWN);
    po_check("down moves the selection", (int)g_sel == PAD_CANCEL);
    po_check("moving cancels the arm", (int)g_arm == 0);
    hat(POV_RIGHT); press(2);
    po_check("second action binds independently", padmap_binding(PAD_CANCEL) == 2);
    po_check("the first binding is untouched", padmap_binding(PAD_OK) == 0);

    // Left clears exactly one row.
    hat(POV_LEFT);
    po_check("left clears the selected binding", padmap_binding(PAD_CANCEL) < 0);
    po_check("clearing leaves the other rows alone", padmap_binding(PAD_OK) == 0);

    // Wrap-around, both ways, over the full row list including the two command rows.
    InterlockedExchange(&g_sel, 0);
    hat(POV_UP);
    po_check("up from the first row wraps to the last", (int)g_sel == ROW_SAVE);
    hat(POV_DOWN);
    po_check("down from the last row wraps to the first", (int)g_sel == 0);

    // The preset row cycles rather than binding, and never leaves a partial map.
    InterlockedExchange(&g_sel, ROW_PRESET);
    hat(POV_RIGHT);
    bool anybound = false;
    for (int a = 0; a < PAD_NACTIONS; a++) if (padmap_binding(a) >= 0) anybound = true;
    po_check("the preset row applies a preset", anybound);
    // A command row has no action to bind, so even an ARMED press there must be inert
    // rather than falling through onto action 0.
    int before[PAD_NACTIONS];
    for (int a = 0; a < PAD_NACTIONS; a++) before[a] = padmap_binding(a);
    InterlockedExchange(&g_arm, 1);
    press(9);
    bool same = true;
    for (int a = 0; a < PAD_NACTIONS; a++) if (before[a] != padmap_binding(a)) same = false;
    po_check("a press on a command row changes no binding", same);

    // THE MEASURE WIZARD -- the control that was missing, and whose absence made the pad
    // unrecoverable from the Deck. Binding says which physical button to hand over;
    // measuring says which slot the client actually READS. This install reads Confirm
    // from slot 2 and Cancel from slot 0, so without a measurement the permutation
    // delivers to slots nobody looks at and every face button goes dead.
    padmap_preset("deck");
    for (int a = 0; a < PAD_NACTIONS; a++) padmap_learn_slot(a, -1);
    padmap_set_enabled(1);
    InterlockedExchange(&g_sel, ROW_MEASURE);
    hat(POV_RIGHT);
    po_check("measuring starts on the first action", (int)g_measuring == PAD_OK);
    po_check("the remap is forced OFF while measuring", padmap_enabled() == 0);
    po_check("buttons are NOT swallowed while measuring (you must see them work)",
             padoverlay_swallows() == 0);
    // Committing with nothing pressed must not record a slot.
    hat(POV_RIGHT);
    po_check("commit with no press does not record", padmap_slot_observed(PAD_OK) < 0
             && (int)g_measuring == PAD_OK);
    press(2);                                   // the client reads Confirm from slot 2
    hat(POV_RIGHT);
    po_check("the pressed index is recorded as the SLOT", padmap_slot_observed(PAD_OK) == 2);
    po_check("and it advances to the next action", (int)g_measuring == PAD_CANCEL);
    press(0);                                   // ...and Cancel from slot 0
    hat(POV_RIGHT);
    po_check("second action measured", padmap_slot_observed(PAD_CANCEL) == 0);
    hat(POV_LEFT);                              // Menu: skip, leave unmeasured
    po_check("left skips without recording", padmap_slot_observed(PAD_MENU) < 0);
    press(3);
    hat(POV_RIGHT);
    po_check("the wizard ends after the last action", (int)g_measuring < 0);
    po_check("the remap is restored when it ends", padmap_enabled() == 1);
    po_check("buttons are swallowed again once it ends", padoverlay_swallows() == 1);

    // The measured slots must actually steer the permutation -- this is the whole point.
    // CLOSED first, deliberately: nothing is permuted while the panel owns the pad, so
    // checking here with it open would assert the swallow rather than the mapping.
    padoverlay_close(0);
    DIJOYSTATE jz; ZeroMemory(&jz, sizeof(jz));
    jz.rgbButtons[0] = 0x80;                    // physical A, bound to Confirm by "deck"
    padmap_on_state(&jz, sizeof(jz), NULL);
    po_check("A now reaches the MEASURED confirm slot 2", jz.rgbButtons[2] != 0);

    // ...and with the panel OPEN the same poll must NOT be permuted -- it is swallowed
    // instead, so a press cannot both drive the mapper and act on the shell behind it.
    padoverlay_open();
    DIJOYSTATE jo2; ZeroMemory(&jo2, sizeof(jo2));
    jo2.rgbButtons[0] = 0x80;
    padmap_on_state(&jo2, sizeof(jo2), NULL);
    po_check("panel OPEN: the poll is swallowed, not permuted",
             jo2.rgbButtons[0] == 0 && jo2.rgbButtons[2] == 0);

    // Aborting must not strand the pad with the remap off.
    InterlockedExchange(&g_sel, ROW_MEASURE);
    hat(POV_RIGHT);
    po_check("wizard restarted", (int)g_measuring == PAD_OK && padmap_enabled() == 0);
    hat(POV_DOWN);
    po_check("down aborts", (int)g_measuring < 0);
    po_check("aborting restores the remap", padmap_enabled() == 1);
    InterlockedExchange(&g_sel, ROW_MEASURE);
    hat(POV_RIGHT);
    padoverlay_close(0);
    po_check("closing mid-measure restores the remap too", padmap_enabled() == 1);
    po_check("closing mid-measure ends the wizard", (int)g_measuring < 0);
    padoverlay_open();
    for (int a = 0; a < PAD_NACTIONS; a++) padmap_learn_slot(a, -1);

    // Save row closes. With no ini path it must still close and must not write.
    InterlockedExchange(&g_sel, ROW_SAVE);
    hat(POV_RIGHT);
    po_check("the save row closes the overlay", padoverlay_active() == 0);
    po_check("closed again: the client sees its pad", padoverlay_swallows() == 0);

    // Toggle is a toggle.
    padoverlay_toggle();
    po_check("toggle opens", padoverlay_active() == 1);
    padoverlay_toggle();
    po_check("toggle closes", padoverlay_active() == 0);

    // Disabled by ini means the chord does nothing at all.
    g_on = 0;
    padoverlay_toggle();
    po_check("pad_overlay=0 refuses to open", padoverlay_active() == 0);
    g_on = 1;

    // The hat decoder, which is the one piece of wire format here.
    po_check("hat: centred (-1) is no direction", pov_dir(0xFFFFFFFF) == POV_NONE);
    po_check("hat: centred (0xFFFF) is no direction", pov_dir(0x0000FFFF) == POV_NONE);
    po_check("hat: 0 is up",        pov_dir(0)     == POV_UP);
    po_check("hat: 9000 is right",  pov_dir(9000)  == POV_RIGHT);
    po_check("hat: 18000 is down",  pov_dir(18000) == POV_DOWN);
    po_check("hat: 27000 is left",  pov_dir(27000) == POV_LEFT);
    po_check("hat: 35900 rounds to up (the wrap case)", pov_dir(35900) == POV_UP);

    padmap_preset("off");
    padmap_set_enabled(0);
    printf("\n%s (%d failure%s)\n", po_fail ? "FAILED" : "all checks passed",
           po_fail, po_fail == 1 ? "" : "s");
    return po_fail;
}
