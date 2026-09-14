// padmap.cpp -- LIVE controller button mapping for the Viewer shell.
//
// WHY THIS EXISTS (and why the previous two attempts did not work)
//
// POL stores four action->button INDICES as REG_DWORDs under
//     HKLM\SOFTWARE\PlayOnlineUS\SquareEnix\PlayOnlineViewer\Settings\Controller
//     HKLM\SOFTWARE\PlayOnline\Square\PlayOnlineViewer\Settings\Controler   (JP, one `l`)
// shipped in PS2 face order (Menu=0 Ok=1 Cancel=2 Navi=3), which on an Xbox-layout pad --
// what a Steam Deck presents -- puts CONFIRM ON B and CANCEL ON X.
//
// Both earlier fixes rewrote those registry values in flight (regredir.cpp:
// swap_confirm, then pad_layout / btn_*). They share two defects that no amount of
// tuning removes:
//
//   1. THEY ARE PARASITIC ON THE KEY EXISTING. The rewrite rides the RegQueryValueEx
//      hook, so it only fires if the client actually QUERIES the value. On a fresh
//      Proton prefix the Controller key is absent (same reason UseGameController is --
//      the prefs app is never run), the client falls back to its built-in defaults
//      without a query, and the override is silently inert while looking configured.
//   2. THEY CANNOT BE TESTED. The client reads its settings once at startup, so every
//      experiment costs a full relaunch, and the only feedback is "did the button feel
//      right". That is what turned one wrong assumption about the index space into
//      three rounds of guessing across two sessions.
//
// THIS MODULE MOVES THE MAPPING TO THE INPUT PATH INSTEAD.
//
// polcore.dll and app.dll both import DINPUT8.dll!DirectInput8Create (checked in the
// shipped binaries; neither imports any winmm joystick entry point, and pol.exe imports
// no input API at all), so the shell's pad state arrives as a DIJOYSTATE/DIJOYSTATE2
// through the device wrapper dinputhook.cpp already owns. Permuting rgbButtons THERE:
//
//   * works whether or not the registry key exists -- it never consults it;
//   * takes effect on the very next poll, i.e. INSTANTLY, so a binding can be tried,
//     felt, and changed inside one session with the shell still running behind;
//   * is exactly reversible -- clearing the bindings restores the identity map.
//
// It composes with the registry overrides rather than fighting them: pol_index() asks
// regredir what value the client will actually SEE for an action (a live btn_*/
// pad_layout override first, then the registry, then POL's shipped defaults), and the
// permutation is built against that. So an install already relying on pad_layout=xbox
// keeps working and can still be re-bound here.
//
// THE PERMUTATION IS A PRODUCT OF TRANSPOSITIONS, NOT AN ASSIGNMENT. Binding an action
// to a physical button SWAPS that button with whatever occupied the action's slot. That
// keeps the map a bijection -- no two actions can end up reading the same physical
// button, which would fire both from one press -- and it makes the common case ("put
// confirm on A") come out as the plain A<->B swap a user expects.
//
// WHAT IS NOT HERE: the D-pad (POL reads it as a POV hat, not as buttons) and the
// per-title pad configs (FFXI `padsin000`, TM `B_00OK`/`B_01CANCEL` -- those are the
// titles' own files, and this remap is deliberately applied to the shell's device only
// while no title is running; see padmap_gate_title()).

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include "polshim.h"
#include "profiles.h"

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

#define PADMAP_MAXBTN 32          // DIJOYSTATE has 32; DIJOYSTATE2's extra 96 are unused
                                  // by anything POL reads and are passed through as-is.

static int  g_enabled = 0;                    // [inputmode] pad_remap
static int  g_trace   = 0;                    // [inputmode] pad_trace
static int  g_pad_title = 1;                  // [inputmode] pad_title -- apply a profiled
                                              // title's OWN pad map to its own pad device
static int  g_bind[PAD_NACTIONS] = { -1, -1, -1, -1 };   // action -> PHYSICAL index
static int  g_phys_for[PADMAP_MAXBTN];        // logical (what POL reads) -> physical
static int  g_log_for[PADMAP_MAXBTN];         // physical -> logical (the inverse)
// The per-TITLE permutation. A title's pad device (owner != NULL on the wire calls)
// reads the pad in the TITLE's own button order (profiles.h pad_slot), so it gets its
// own permutation built from the same user bindings. One slot suffices: pol.exe runs
// one title at a time, and a stale entry is replaced on the next poll of another
// title's device.
static const TitleProfile* volatile g_title = NULL;  // whose permutation is loaded
static int  g_tp_phys_for[PADMAP_MAXBTN];
static int  g_tp_log_for[PADMAP_MAXBTN];

// Live view of the REAL pad, captured before the permutation is applied. Written by the
// device wrapper on the client's own polling thread, read by the settings-dialog thread.
// Plain aligned scalars, deliberately unlocked: a byte that is one frame stale in a
// 60 ms UI refresh is not a defect, and a lock on the client's input path would be.
static volatile BYTE  g_phys[PADMAP_MAXBTN];
static volatile LONG  g_phys_seen  = 0;       // polls observed, ever
static volatile DWORD g_phys_when  = 0;       // GetTickCount of the last poll
static volatile LONG  g_nbuttons   = 0;       // buttons the device reports
static char           g_product[128] = "";
static volatile LONG  g_ndevices   = 0;
static volatile LONG  g_in_title   = 0;       // a title owns the pad -> stop remapping
// Polls we actually permuted. The dialog shows it, because "the mapping is wrong" and
// "the mapping never runs" are different faults with different fixes and look identical
// from the outside -- which is precisely how this feature wasted two rounds already.
static volatile LONG  g_n_remapped = 0;

// Capture ("press a button to bind"). While armed the buttons are ALSO swallowed on the
// way to the client, so binding confirm does not simultaneously click whatever the shell
// had focused behind the dialog.
static volatile LONG  g_capture_armed = 0;
static volatile LONG  g_capture_got   = -1;
static BYTE           g_capture_prev[PADMAP_MAXBTN];

// Last physical button seen going down, for the dialog's "what did that do" readout.
static volatile LONG  g_last_down = -1;

// Last POV (D-pad) reading seen. Kept because the on-surface mapper navigates with the
// hat, and the buffered read path delivers the hat as its own event rather than inside a
// struct -- so the two paths have to agree on one current value.
static volatile DWORD g_pov = 0xFFFFFFFF;

static CRITICAL_SECTION g_cs;
static bool             g_cs_ready = false;

static const char* const g_action_key[PAD_NACTIONS] = { "ok", "cancel", "menu", "navi" };
static const wchar_t* const g_action_regW[PAD_NACTIONS] = { L"Ok", L"Cancel", L"Menu", L"Navi" };
static const char* const g_action_label[PAD_NACTIONS] =
    { "Confirm", "Cancel", "Menu", "Navigate" };

// POL's SHIPPED defaults, in the same order as PAD_*. Measured from a real install's
// Controller key (Ok=1 Cancel=2) and from the PS2 face ordering the other two follow.
// Used only when nothing else answers -- i.e. the fresh-prefix case the old override
// could not handle at all.
static const int g_pol_default[PAD_NACTIONS] = { 1, 2, 0, 3 };

const char* padmap_action_name(int a)
{
    return (a >= 0 && a < PAD_NACTIONS) ? g_action_label[a] : "?";
}

// ---------------------------------------------------------------------------
// PAD LAYOUTS -- a preset and its button NAMES together, because they are the same
// fact about one piece of hardware and splitting them is how they drift apart.
//
// A name is printed ONLY where it was measured. Everything else prints as a bare
// "button N", which is honest and still perfectly usable -- you bind by pressing, so an
// index you cannot name is not an index you cannot use. Half of the trouble this module
// exists to end came from a confidently-printed name that was a guess.
//
// IMPORTANT: THE INDEX SPACE. This has now been wrong in BOTH directions, so the reasoning
// is kept rather than just the answer.
//
// It was first recorded as "0=A 1=B 2=X 3=Y everywhere", INFERRED 2026-08-15 from two
// swap_confirm observations. On 2026-08-15 a supposed direct measurement contradicted it
// ("B is index 2 on a Deck"), and because a measurement outranks an inference, the deck
// preset and its names were changed to match and a comment was added here telling the
// next person not to "correct" them back.
//
// That was wrong, and it was RETRACTED 2026-08-19 when the account holder reported that
// the deck preset put Cancel on X. Two independent facts settle it:
//
//   * the Deck's pad enumerates as 'Controller (XBOX 360 For Windows)' with **10
//     buttons** (read off the live [pad] device line), i.e. an XInput-emulated pad
//     presented through DirectInput, whose enumeration order is the documented
//     0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back 7=Start 8=L3 9=R3 -- and 10 is exactly the count
//     that order produces;
//   * binding Cancel to index 2 lands it on X, live, on the machine in question.
//
// So the ORIGINAL inference was right and the "measurement" that displaced it was not a
// measurement of what it claimed. The lesson worth keeping is not "trust inferences" but
// that a reading taken THROUGH a mechanism (swap_confirm, which is inert without the
// Controller key) measures the mechanism, not the hardware. The overlay's live button
// lamps are the honest instrument: press a button, watch which index lights.
struct PadLayout {
    const char* key;                     // preset name, and the [inputmode] pad_names value
    const char* label;                   // what the dialog calls it
    int         act[PAD_NACTIONS];       // ok, cancel, menu, navi -> PHYSICAL index
    const char* btn[12];                 // physical index -> printed name; NULL = unmeasured
};

static const PadLayout g_layouts[] = {
    // Steam Deck. The face four are the standard XInput-through-DirectInput order, which
    // is what its pad enumerates as ('Controller (XBOX 360 For Windows)', 10 buttons);
    // View/Menu are the Deck's names for what that order calls Back/Start. Differs from
    // "xbox" below only in putting Menu and Navigate on the two small buttons rather than
    // on Y and X, which is what the hardware invites.
    { "deck", "Steam Deck", { 0, 1, 7, 6 },
      { "A", "B", "X", "Y", "LB", "RB", "View", "Menu", "L3", "R3", NULL, NULL } },
    // Desktop XInput-emulated pad. The face four are the 2026-08-15 inference above --
    // now known NOT to hold on a Deck, and not re-measured on a desktop either, so treat
    // them as provisional; the rest is the standard enumeration order.
    { "xbox", "Xbox layout", { 0, 1, 3, 2 },
      { "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "L3", "R3", "Guide", NULL } },
    // POL's OWN shipped values, which are PS2 face order (Menu=0 Ok=1 Cancel=2 Navi=3) --
    // read out of a real install's Controller key, so the PRESET is measured even though
    // the button names on that hardware are not.
    { "ps", "PlayStation layout", { 1, 2, 0, 3 },
      { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL } },
};

static int g_style = 1;                  // index into g_layouts; names only. Default xbox.

static const PadLayout* layout_by_key(const char* key)
{
    if (!key) return NULL;
    for (int i = 0; i < _countof(g_layouts); i++)
        if (!_stricmp(key, g_layouts[i].key)) return &g_layouts[i];
    return NULL;
}

int padmap_layout_count() { return (int)_countof(g_layouts); }
const char* padmap_layout_key(int i)
{ return (i >= 0 && i < _countof(g_layouts)) ? g_layouts[i].key : ""; }
const char* padmap_layout_label(int i)
{ return (i >= 0 && i < _countof(g_layouts)) ? g_layouts[i].label : ""; }

const char* padmap_button_name(int phys, char* buf, size_t cch)
{
    if (phys < 0) { _snprintf_s(buf, cch, _TRUNCATE, "(not set)"); return buf; }
    const char* n = (phys < (int)_countof(g_layouts[g_style].btn))
                    ? g_layouts[g_style].btn[phys] : NULL;
    if (n) _snprintf_s(buf, cch, _TRUNCATE, "%s (%d)", n, phys);
    else   _snprintf_s(buf, cch, _TRUNCATE, "button %d", phys);
    return buf;
}

// ---------------------------------------------------------------------------
// what index will the CLIENT read for this action?
// ---------------------------------------------------------------------------
//
// Three sources, most authoritative first. Whence is filled in for the dialog, because
// "where did that number come from" is the question every previous round of this got
// wrong by assuming.

// OBSERVED slots -- see padmap_learn_slot(). -1 = not measured.
static int g_slot_obs[PAD_NACTIONS] = { -1, -1, -1, -1 };
static void rebuild();     // defined below; a measurement changes the permutation

// `whence` is deliberately SHORT -- it sits in a dialog column, and a full registry path
// there pushed the useful part off the edge. padmap_report prints the path in full.
// The decision, as a PURE function of its four inputs -- deliberately separable from
// where they come from. The environment cannot be arranged on a dev box (this machine has
// a real Settings\\Controller key; a Proton prefix has none), so a test that reached for
// the registry would prove whatever the machine happened to be. Driving the rule directly
// tests the rule.
//
//   measured  what the client was WATCHED reading, or -1
//   ovr       btn_* / pad_layout, or -1
//   reg       what Settings\\Controller holds, or -1 when the key is absent
int padmap_resolve_slot(int action, int measured, int ovr, int reg,
                        char* whence, size_t cch)
{
    if (action < 0 || action >= PAD_NACTIONS) return -1;
    if (whence && cch) whence[0] = 0;

    // A MEASUREMENT BEATS EVERY INFERENCE, so it is consulted first. Everything below is
    // us reasoning about what the client will do; this is the client having been watched
    // doing it. The whole feature was wrong for three rounds because no step in the chain
    // was ever measured directly -- this is the step that ends that.
    if (measured >= 0) {
        if (whence) _snprintf_s(whence, cch, _TRUNCATE, "MEASURED on this machine");
        return measured;
    }

    // THE LEGACY OVERRIDE IS ONLY REAL IF THE CLIENT WILL ACTUALLY READ IT.
    //
    // btn_* / pad_layout are served through regredir's RegQueryValueEx hook, so they
    // reach the client only when it QUERIES the value -- which needs Settings\\Controller
    // to exist. On a fresh Proton prefix it does not (polbtn_registry documents its -1 as
    // exactly that), the client falls back to its own COMPILED defaults, and the override
    // is inert. This module's header has said so since it was written.
    //
    // Trusting it anyway is a silent, TOTAL mis-map, and it was reported live from a Deck
    // on 2026-08-19: with `pad_layout=xbox` and the measurements cleared, padmap built the
    // permutation against the slots the override CLAIMED ({0,1,3,2}) while the client was
    // reading its compiled PS2 order ({1,2,0,3}). Every binding landed in the wrong slot
    // and the face buttons did nothing at all. The old order was right about which source
    // is most authoritative and wrong about whether this one is a source at all.
    if (ovr >= 0 && reg >= 0) {
        if (whence) _snprintf_s(whence, cch, _TRUNCATE, "from btn_%s / pad_layout",
                                g_action_key[action]);
        return ovr;
    }
    if (ovr >= 0) {
        // Configured but undeliverable. Said where the value is READ, not only in the
        // log: "it is set" and "it is in force" are the exact pair this feature keeps
        // confusing, and a dialog column is where that confusion happens.
        if (whence) _snprintf_s(whence, cch, _TRUNCATE,
                                "pad_layout/btn_%s is INERT (no Controller key) -- "
                                "using POL's default", g_action_key[action]);
        return g_pol_default[action];
    }
    if (reg >= 0) {
        if (whence) _snprintf_s(whence, cch, _TRUNCATE, "from the registry");
        return reg;
    }
    if (whence) _snprintf_s(whence, cch, _TRUNCATE, "GUESSED -- POL's default, unverified here");
    return g_pol_default[action];
}

int padmap_pol_index(int action, char* whence, size_t cch)
{
    if (action < 0 || action >= PAD_NACTIONS) return -1;
    wchar_t keypath[512] = L"";
    return padmap_resolve_slot(action, g_slot_obs[action],
                               polbtn_override_by_name(g_action_regW[action]),
                               polbtn_registry(g_action_regW[action], keypath,
                                               _countof(keypath)),
                               whence, cch);
}

// ---------------------------------------------------------------------------
// LEARNING A SLOT -- the one measurement that removes every remaining assumption.
//
// Everything else in this file reasons about which rgbButtons slot the client reads for
// an action: an override we are serving, else the registry, else POL's shipped defaults.
// On a Steam Deck the first two are absent, so it lands on the guess -- and if that guess
// is wrong the permutation writes the right button into the WRONG slot, which from
// outside looks exactly like "the mapping is ignored". That is not a hypothesis worth
// arguing about, because it is directly measurable:
//
//   with the remap OFF, the physical button that confirms IS the slot the client reads
//   for confirm.
//
// So the mapper asks the user to press it, and we read the answer off the wire. No
// inference, no platform assumption, no relaunch. `learn` therefore forces the remap off
// while it is armed -- measuring through a live permutation would measure the
// permutation.
int padmap_slot_observed(int action)
{
    return (action >= 0 && action < PAD_NACTIONS) ? g_slot_obs[action] : -1;
}

void padmap_learn_slot(int action, int phys)
{
    if (action < 0 || action >= PAD_NACTIONS) return;
    g_slot_obs[action] = (phys >= 0 && phys < PADMAP_MAXBTN) ? phys : -1;
    rebuild();
    logf("[pad] MEASURED: the client reads %s from slot %d%s", g_action_label[action],
         g_slot_obs[action],
         g_slot_obs[action] < 0 ? " (cleared)" : " -- guesses for it are now ignored");
}

static void parse_slots(const wchar_t* s)
{
    for (int i = 0; i < PAD_NACTIONS; i++) g_slot_obs[i] = -1;
    wchar_t buf[256]; wcsncpy_s(buf, s, _TRUNCATE);
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(buf, L",; \t", &ctx); t; t = wcstok_s(NULL, L",; \t", &ctx)) {
        wchar_t* eq = wcschr(t, L'=');
        if (!eq) continue;
        *eq = 0;
        for (int a = 0; a < PAD_NACTIONS; a++) {
            wchar_t key[16]; size_t got = 0;
            mbstowcs_s(&got, key, _countof(key), g_action_key[a], _TRUNCATE);
            if (_wcsicmp(t, key) != 0) continue;
            int v = (int)wcstol(eq + 1, NULL, 10);
            g_slot_obs[a] = (v >= 0 && v < PADMAP_MAXBTN) ? v : -1;
            break;
        }
    }
}

static void format_slots(wchar_t* out, size_t cch)
{
    out[0] = 0;
    for (int a = 0; a < PAD_NACTIONS; a++) {
        if (g_slot_obs[a] < 0) continue;
        wchar_t one[32];
        swprintf_s(one, L"%s%hs=%d", out[0] ? L"," : L"", g_action_key[a], g_slot_obs[a]);
        wcsncat_s(out, cch, one, _TRUNCATE);
    }
}

// ---------------------------------------------------------------------------
// the permutation
// ---------------------------------------------------------------------------

// Build one permutation from the user's bindings against a given action->slot map.
// Shared by the shell (slots = what the CLIENT reads, via padmap_pol_index) and by a
// title (slots = its profile's measured pad_slot row) -- the transposition rule is the
// same fact in both: binding an action to a physical button SWAPS that button with
// whatever occupied the action's slot, so the map stays a bijection.
static void build_perm_locked(const int slots[PAD_NACTIONS],
                              int phys_for[PADMAP_MAXBTN], int log_for[PADMAP_MAXBTN])
{
    for (int i = 0; i < PADMAP_MAXBTN; i++) phys_for[i] = i;

    // WHICH ACTION HAS ALREADY CLAIMED EACH SLOT.
    //
    // Two actions can resolve to the SAME slot -- most easily by measurement, since
    // padmap_learn_slot records whatever button the user pressed and nothing stopped
    // them pressing one button for two actions. Found live on the Deck 2026-08-19:
    // `pad_slots=ok=2,cancel=0,menu=2,navi=3` had Confirm and Menu both on slot 2.
    //
    // Without this guard the loop below just does `phys_for[L] = p` twice and the LATER
    // action silently overwrites the earlier -- so Confirm's binding vanished, the pad
    // did something nobody asked for, and the map printed as perfectly configured. That
    // is exactly the "looks configured, does nothing" failure this module exists to end,
    // reappearing one layer up.
    //
    // FIRST CLAIM WINS, and the loser is reported (padmap_slot_conflict) rather than
    // quietly dropped: a collision means one of the two measurements is WRONG, and only
    // the user knows which. Silently picking one and saying nothing is what we are fixing.
    int claimed[PADMAP_MAXBTN];
    for (int i = 0; i < PADMAP_MAXBTN; i++) claimed[i] = -1;

    for (int a = 0; a < PAD_NACTIONS; a++) {
        int p = g_bind[a];
        if (p < 0 || p >= PADMAP_MAXBTN) continue;
        int L = slots[a];
        if (L < 0 || L >= PADMAP_MAXBTN) continue;
        if (claimed[L] >= 0) continue;          // an earlier action owns this slot
        claimed[L] = a;
        if (phys_for[L] == p) continue;
        // Transposition, so the map stays a bijection: whoever was reading p now reads
        // whatever L was reading.
        for (int i = 0; i < PADMAP_MAXBTN; i++)
            if (phys_for[i] == p) { phys_for[i] = phys_for[L]; break; }
        phys_for[L] = p;
    }
    for (int i = 0; i < PADMAP_MAXBTN; i++) log_for[i] = i;
    for (int L = 0; L < PADMAP_MAXBTN; L++) log_for[phys_for[L]] = L;
}

// action -> the EARLIER action that already owns its slot, or -1. Recomputed with the
// permutation, so it can never disagree with the map actually in force.
static int g_conflict[PAD_NACTIONS] = { -1, -1, -1, -1 };

int padmap_slot_conflict(int action)
{
    return (action >= 0 && action < PAD_NACTIONS) ? g_conflict[action] : -1;
}

static void note_conflicts(const int slots[PAD_NACTIONS])
{
    int prev[PAD_NACTIONS];
    for (int a = 0; a < PAD_NACTIONS; a++) { prev[a] = g_conflict[a]; g_conflict[a] = -1; }
    for (int a = 0; a < PAD_NACTIONS; a++) {
        if (g_bind[a] < 0 || slots[a] < 0) continue;
        for (int b = 0; b < a; b++) {
            if (g_bind[b] < 0) continue;
            if (slots[b] == slots[a]) { g_conflict[a] = b; break; }
        }
    }
    // Logged only when it CHANGES, because rebuild() runs on every edit and a warning
    // repeated every keystroke is a warning nobody reads.
    for (int a = 0; a < PAD_NACTIONS; a++) {
        if (g_conflict[a] == prev[a] || g_conflict[a] < 0) continue;
        logf("[pad] SLOT COLLISION: %s and %s are both mapped to slot %d. The client "
             "cannot tell them apart, so %s KEEPS the slot and %s's binding is NOT "
             "applied. One of the two measurements is wrong -- re-measure it "
             "(pad_slots=%s) or clear it.",
             padmap_action_name(g_conflict[a]), padmap_action_name(a), slots[a],
             padmap_action_name(g_conflict[a]), padmap_action_name(a),
             g_action_key[a]);
    }
}

static void rebuild_locked()
{
    int slots[PAD_NACTIONS];
    for (int a = 0; a < PAD_NACTIONS; a++) slots[a] = padmap_pol_index(a, NULL, 0);
    note_conflicts(slots);
    build_perm_locked(slots, g_phys_for, g_log_for);
    const TitleProfile* t = g_title;
    if (t) build_perm_locked(t->pad_slot, g_tp_phys_for, g_tp_log_for);
}

static void rebuild()
{
    if (!g_cs_ready) { rebuild_locked(); return; }
    EnterCriticalSection(&g_cs);
    rebuild_locked();
    LeaveCriticalSection(&g_cs);
}

int padmap_binding(int action)
{
    return (action >= 0 && action < PAD_NACTIONS) ? g_bind[action] : -1;
}

void padmap_bind(int action, int phys)
{
    if (action < 0 || action >= PAD_NACTIONS) return;
    if (phys >= PADMAP_MAXBTN) return;
    g_bind[action] = (phys < 0) ? -1 : phys;
    rebuild();
    if (g_trace) {
        char n[32];
        logf("[pad] bind %s -> %s", g_action_label[action], padmap_button_name(phys, n, sizeof(n)));
    }
}

void padmap_snapshot_bindings(int out[PAD_NACTIONS], int* enabled)
{
    for (int i = 0; i < PAD_NACTIONS; i++) out[i] = g_bind[i];
    if (enabled) *enabled = g_enabled;
}

void padmap_restore_bindings(const int in[PAD_NACTIONS], int enabled)
{
    for (int i = 0; i < PAD_NACTIONS; i++) g_bind[i] = in[i];
    g_enabled = enabled ? 1 : 0;
    rebuild();
}

// The presets bind ACTIONS TO PHYSICAL BUTTONS, which is a different (and safer)
// statement than the old pad_layout=, which rewrote POL's indices. Whatever the client
// thinks its indices are, "confirm is the A button" comes out the same.
//
// Choosing a preset also switches which layout's NAMES are printed: the two are one fact
// about one piece of hardware, and a Deck showing "X (2)" for the button its owner calls
// B is the same class of bug as a wrong binding.
void padmap_preset(const char* name)
{
    const PadLayout* L = layout_by_key(name);
    if (L) {
        for (int i = 0; i < PAD_NACTIONS; i++) g_bind[i] = L->act[i];
        g_style = (int)(L - g_layouts);
    } else {                                   // "off" / anything else: clear
        for (int i = 0; i < PAD_NACTIONS; i++) g_bind[i] = -1;
    }
    rebuild();
}

// Swap two actions' physical buttons -- the one-click answer to "A and B are backwards".
// Unbound actions resolve to the button the client currently reads for them, so this
// works from a virgin state without needing every action bound first.
void padmap_swap(int a, int b)
{
    if (a < 0 || a >= PAD_NACTIONS || b < 0 || b >= PAD_NACTIONS) return;
    int pa = g_bind[a] >= 0 ? g_bind[a] : padmap_pol_index(a, NULL, 0);
    int pb = g_bind[b] >= 0 ? g_bind[b] : padmap_pol_index(b, NULL, 0);
    g_bind[a] = pb; g_bind[b] = pa;
    rebuild();
}

int padmap_logical_for_phys(int phys)
{
    if (phys < 0 || phys >= PADMAP_MAXBTN) return -1;
    return g_enabled ? g_log_for[phys] : phys;
}

int padmap_action_for_phys(int phys)
{
    int L = padmap_logical_for_phys(phys);
    if (L < 0) return -1;
    for (int a = 0; a < PAD_NACTIONS; a++)
        if (padmap_pol_index(a, NULL, 0) == L) return a;
    return -1;
}

// ---------------------------------------------------------------------------
// the live wire -- called from dinputhook.cpp's pad wrapper
// ---------------------------------------------------------------------------

void padmap_note_device(const char* product, int nbuttons)
{
    InterlockedIncrement(&g_ndevices);
    if (product && *product) strncpy_s(g_product, product, _TRUNCATE);
    if (nbuttons > 0) InterlockedExchange(&g_nbuttons, nbuttons);
    logf("[pad] game-controller device: '%s', %d buttons -- this is the device the "
         "mapping applies to", g_product[0] ? g_product : "(unnamed)", nbuttons);
}

void padmap_note_title(int running)
{
    InterlockedExchange(&g_in_title, running ? 1 : 0);
}

// Record what the pad REALLY reports, then answer the capture if one is armed.
static void observe(const BYTE* btns, int n)
{
    if (n > PADMAP_MAXBTN) n = PADMAP_MAXBTN;
    for (int i = 0; i < n; i++) g_phys[i] = btns[i] ? 1 : 0;
    InterlockedIncrement(&g_phys_seen);
    InterlockedExchange((LONG*)&g_phys_when, (LONG)GetTickCount());
    // NOTE: do not touch g_nbuttons here. Every observe() caller passes n =
    // PADMAP_MAXBTN (a constant, not the hardware count), so growing g_nbuttons to it
    // just ratcheted the honest count from padmap_note_device up to 32 -- making every
    // diagnostic report "32 buttons". padmap_note_device is the sole authority;
    // padmap_live_buttons falls back to PADMAP_MAXBTN when it is still 0.

    for (int i = 0; i < n; i++) {
        bool down = g_phys[i] != 0, was = g_capture_prev[i] != 0;
        g_capture_prev[i] = g_phys[i];
        if (!down || was) continue;                       // edge only
        InterlockedExchange(&g_last_down, i);
        if (InterlockedCompareExchange(&g_capture_armed, 0, 1) == 1)
            InterlockedExchange(&g_capture_got, i);
    }
}

// Arm the per-title permutation for `owner` and say whether it applies. A profiled
// title with no measured pad map gets a STOCK pad -- passing it through is this
// feature's honest failure mode, where applying the SHELL's permutation to a title
// that reads the pad in its own order would be a scramble.
static bool title_perm(const TitleProfile* owner)
{
    if (!g_pad_title || !owner) return false;
    bool mapped = owner->pad_slot[PAD_OK] >= 0 || owner->pad_slot[PAD_CANCEL] >= 0 ||
                  owner->pad_slot[PAD_MENU] >= 0 || owner->pad_slot[PAD_NAVI] >= 0;
    if (owner != g_title) {
        g_title = owner;
        rebuild();
        if (mapped)
            logf("[pad] pad polled by %s -- applying ITS pad map "
                 "(ok=%d cancel=%d menu=%d navi=%d), not the shell's",
                 owner->title, owner->pad_slot[PAD_OK], owner->pad_slot[PAD_CANCEL],
                 owner->pad_slot[PAD_MENU], owner->pad_slot[PAD_NAVI]);
        else
            logf("[pad] pad polled by %s -- its pad map is unmeasured, so its pad is "
                 "passed through STOCK (the shell's permutation would scramble it)",
                 owner->title);
    }
    return mapped;
}

// The client's own DIJOYSTATE, rewritten in place. Both DIJOYSTATE and DIJOYSTATE2 put
// rgbButtons at the same offset (48) and differ only in what follows it, so one test on
// the reported struct size covers both -- and a CUSTOM data format, whose size matches
// neither, is passed through untouched rather than scribbled on at a guessed offset.
void padmap_on_state(void* data, unsigned long cb, const TitleProfile* owner)
{
    if (!data) return;
    if (cb != sizeof(DIJOYSTATE) && cb != sizeof(DIJOYSTATE2)) {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            logf("[pad] the client polls the pad with a %lu-byte custom data format, "
                 "not DIJOYSTATE(%u)/DIJOYSTATE2(%u) -- button mapping is PASSED THROUGH "
                 "because the button offsets are unknown for it", cb,
                 (unsigned)sizeof(DIJOYSTATE), (unsigned)sizeof(DIJOYSTATE2));
        return;
    }
    BYTE* btn = (BYTE*)data + FIELD_OFFSET(DIJOYSTATE, rgbButtons);
    observe(btn, PADMAP_MAXBTN);

    // THE ON-SURFACE MAPPER (padoverlay.cpp) OWNS THE PAD WHILE IT IS UP.
    //
    // It is fed the RAW state -- physical buttons, before any permutation -- because a
    // mapper that showed you the permuted view would report "B" when you pressed A and
    // send you round the loop this whole feature exists to end. Its navigation is the
    // POV hat, which is deliberately the one control padmap never permutes.
    //
    // Only the SHELL's own device (no owner, no title running): the overlay is drawn on
    // the shell's DirectDraw present, so it is not on screen over a title, and
    // swallowing a title's pad would take the controller away from a game that is being
    // played with no visible reason why.
    DWORD* pov = ((DIJOYSTATE*)data)->rgdwPOV;
    if (!owner && !g_in_title && padoverlay_active()) {
        BYTE raw[PADMAP_MAXBTN];
        memcpy(raw, btn, PADMAP_MAXBTN);
        g_pov = pov[0];
        padoverlay_feed(raw, PADMAP_MAXBTN, pov[0]);
        // THE HAT IS ALWAYS OURS while the panel is up -- it is the panel's only control,
        // so it must never also drive the menu behind it. The BUTTONS are ours only when
        // the panel says so: during a MEASUREMENT they are deliberately let through, raw,
        // so the user can see which one the client treats as Confirm rather than having to
        // remember. Axes are left alone -- their neutral value is not knowable from here,
        // and a stick cannot press anything.
        for (int i = 0; i < 4; i++) pov[i] = 0xFFFFFFFF;
        if (padoverlay_swallows()) memset(btn, 0, PADMAP_MAXBTN);
        // Never permuted while the panel owns the pad: a measurement taken through a
        // permutation measures the permutation.
        return;
    }
    g_pov = pov[0];

    // Swallow while a capture is armed: the press that binds an action must not also
    // reach whatever the shell had focused behind the dialog.
    if (g_capture_armed) {
        memset(btn, 0, PADMAP_MAXBTN);
        return;
    }
    if (!g_enabled) return;

    const int* perm = g_phys_for;
    if (owner) {
        // A title's own device: its profile's map or nothing. Never the shell's.
        if (!title_perm(owner)) return;
        perm = g_tp_phys_for;
    } else if (g_in_title) {
        return;                    // shell device while a title runs: stand down
    }

    BYTE in[PADMAP_MAXBTN];
    memcpy(in, btn, PADMAP_MAXBTN);
    for (int L = 0; L < PADMAP_MAXBTN; L++) btn[L] = in[perm[L]];
    InterlockedIncrement(&g_n_remapped);

    if (g_trace) {
        static LONG n = 0;
        LONG k = InterlockedIncrement(&n);
        if (k < 6 || (k % 600) == 0)
            logf("[pad] state #%ld %s raw=%02X%02X%02X%02X -> served=%02X%02X%02X%02X",
                 k, owner ? owner->title : "shell",
                 in[0], in[1], in[2], in[3], btn[0], btn[1], btn[2], btn[3]);
    }
}

// Buffered reads. One element per event; a button event carries dwOfs = rgbButtons + n,
// so the remap is a rewrite of dwOfs -- the physical button p is reported at the logical
// slot that now reads it.
void padmap_on_data(void* rgod, unsigned long cbOd, unsigned long n,
                    const TitleProfile* owner)
{
    if (!rgod || !cbOd) return;
    const DWORD base = FIELD_OFFSET(DIJOYSTATE, rgbButtons);
    const DWORD povbase = FIELD_OFFSET(DIJOYSTATE, rgdwPOV);
    const bool  ui = (!owner && !g_in_title && padoverlay_active());
    for (DWORD i = 0; i < n; i++) {
        DIDEVICEOBJECTDATA* d = (DIDEVICEOBJECTDATA*)((BYTE*)rgod + i * cbOd);
        // A HAT event. The polled path reads the hat out of the struct; here it arrives
        // as its own element, so it is recorded (and, while the mapper owns the pad,
        // routed to it and centred on the way to the client) before the button filter
        // below discards every non-button offset.
        if (d->dwOfs >= povbase && d->dwOfs < povbase + sizeof(DWORD) * 4) {
            g_pov = d->dwData;
            if (ui) {
                BYTE view[PADMAP_MAXBTN];
                for (int k = 0; k < PADMAP_MAXBTN; k++) view[k] = g_phys[k];
                padoverlay_feed(view, PADMAP_MAXBTN, d->dwData);
                d->dwData = 0xFFFFFFFF;               // centred
            }
            continue;
        }
        if (d->dwOfs < base || d->dwOfs >= base + PADMAP_MAXBTN) continue;
        int p = (int)(d->dwOfs - base);
        BYTE down = (d->dwData & 0x80) ? 1 : 0;
        // Feed the observer a one-button edge without disturbing the rest of the view.
        BYTE view[PADMAP_MAXBTN];
        for (int k = 0; k < PADMAP_MAXBTN; k++) view[k] = g_phys[k];
        view[p] = down;
        observe(view, PADMAP_MAXBTN);
        if (ui) {                       // the panel owns the pad
            padoverlay_feed(view, PADMAP_MAXBTN, g_pov);
            // Swallow the button only when the panel says so -- the SAME gate the
            // polled path (padmap_on_state) uses. During a MEASUREMENT
            // padoverlay_swallows() is false, so the press passes through RAW
            // (unremapped) and the user can see which physical button the client
            // treats as Confirm; zeroing it unconditionally here broke the Measure
            // wizard on any shell that reads its pad buffered. Otherwise eat it so it
            // does not reach whatever is focused behind the panel. Never remapped
            // while the panel owns the pad (a measurement through a permutation would
            // measure the permutation).
            if (padoverlay_swallows()) d->dwData = 0;
            continue;
        }
        if (g_capture_armed) { d->dwData = 0; continue; }
        if (!g_enabled) continue;
        const int* inv = g_log_for;
        if (owner) {
            if (!title_perm(owner)) continue;
            inv = g_tp_log_for;
        } else if (g_in_title) {
            continue;
        }
        d->dwOfs = base + (DWORD)inv[p];
        InterlockedIncrement(&g_n_remapped);
    }
}

// ---------------------------------------------------------------------------
// live view for the dialog
// ---------------------------------------------------------------------------
//
// The client's own device is the ground truth and is preferred whenever it has been
// polled recently. XInput is the FALLBACK, used when the shell has not created a pad
// device (or has stopped polling it) so that the dialog can still show a pad and still
// bind -- clearly labelled, because its index space is an ASSUMPTION (the standard
// enumeration order an XInput-emulated pad presents through DirectInput) rather than
// something read off the device the client uses.

typedef struct { DWORD packet; struct { WORD buttons; BYTE lt, rt; SHORT tlx, tly, trx, trY; } pad; } PM_XI;
typedef DWORD (WINAPI *PFN_XIGET)(DWORD, PM_XI*);
static PFN_XIGET g_xiget = NULL;
static int       g_xitried = 0;

static void xi_resolve()
{
    if (g_xitried) return;
    g_xitried = 1;
    static const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < _countof(dlls); i++) {
        HMODULE h = LoadLibraryW(dlls[i]);
        if (!h) continue;
        g_xiget = (PFN_XIGET)GetProcAddress(h, "XInputGetState");
        if (g_xiget) return;
    }
}

// XInput button bit -> the DirectInput index the same physical button occupies.
static const struct { WORD bit; int idx; } g_xi_to_di[] = {
    { 0x1000, 0 }, { 0x2000, 1 }, { 0x4000, 2 }, { 0x8000, 3 },   // A B X Y
    { 0x0100, 4 }, { 0x0200, 5 },                                  // LB RB
    { 0x0020, 6 }, { 0x0010, 7 },                                  // Back Start
    { 0x0040, 8 }, { 0x0080, 9 },                                  // L3 R3
};

int padmap_live_buttons(unsigned char out[PADMAP_MAXBTN], int* nbtn, const char** src)
{
    DWORD age = GetTickCount() - g_phys_when;
    if (g_phys_seen && age < 1500) {
        for (int i = 0; i < PADMAP_MAXBTN; i++) out[i] = g_phys[i];
        if (nbtn) *nbtn = g_nbuttons ? (int)g_nbuttons : PADMAP_MAXBTN;
        if (src)  *src  = "game";
        return 1;
    }
    xi_resolve();
    memset(out, 0, PADMAP_MAXBTN);
    if (g_xiget) {
        for (DWORD s = 0; s < 4; s++) {
            PM_XI st; ZeroMemory(&st, sizeof(st));
            if (g_xiget(s, &st) != 0) continue;
            for (int i = 0; i < _countof(g_xi_to_di); i++)
                if (st.pad.buttons & g_xi_to_di[i].bit) out[g_xi_to_di[i].idx] = 1;
            if (nbtn) *nbtn = 10;
            if (src)  *src  = "xinput";
            observe(out, PADMAP_MAXBTN);   // so capture works off the fallback too
            return 1;
        }
    }
    if (nbtn) *nbtn = 0;
    if (src)  *src  = "none";
    return 0;
}

int padmap_device_seen(char* product, size_t cch)
{
    if (product && cch) strncpy_s(product, cch, g_product[0] ? g_product : "", _TRUNCATE);
    return (int)g_ndevices;
}

int padmap_last_down() { return (int)g_last_down; }
int padmap_remapped_count() { return (int)g_n_remapped; }

// ---------------------------------------------------------------------------
// capture
// ---------------------------------------------------------------------------

void padmap_capture_begin()
{
    for (int i = 0; i < PADMAP_MAXBTN; i++) g_capture_prev[i] = g_phys[i];
    InterlockedExchange(&g_capture_got, -1);
    InterlockedExchange(&g_capture_armed, 1);
}

void padmap_capture_cancel()
{
    InterlockedExchange(&g_capture_armed, 0);
    InterlockedExchange(&g_capture_got, -1);
}

int padmap_capture_poll()
{
    LONG got = InterlockedExchange(&g_capture_got, -1);
    return (int)got;
}

int padmap_capture_armed() { return (int)g_capture_armed; }

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------
//
// pad_bind is written and read as `ok=0,cancel=1,menu=3,navi=2`, naming the PHYSICAL
// button each action sits on. Names, not positions, so a hand-edited line cannot mean
// something different by having one field missing -- the failure that a bare
// "0,1,3,2" list invites.

static void parse_bind(const wchar_t* s)
{
    for (int i = 0; i < PAD_NACTIONS; i++) g_bind[i] = -1;
    wchar_t buf[256]; wcsncpy_s(buf, s, _TRUNCATE);
    wchar_t* ctx = NULL;
    for (wchar_t* t = wcstok_s(buf, L",; \t", &ctx); t; t = wcstok_s(NULL, L",; \t", &ctx)) {
        wchar_t* eq = wcschr(t, L'=');
        if (!eq) continue;
        *eq = 0;
        for (int a = 0; a < PAD_NACTIONS; a++) {
            wchar_t key[16];
            size_t got = 0;
            mbstowcs_s(&got, key, _countof(key), g_action_key[a], _TRUNCATE);
            if (_wcsicmp(t, key) != 0) continue;
            int v = (int)wcstol(eq + 1, NULL, 10);
            g_bind[a] = (v >= 0 && v < PADMAP_MAXBTN) ? v : -1;
            break;
        }
    }
}

void padmap_format_bind(wchar_t* out, size_t cch)
{
    out[0] = 0;
    for (int a = 0; a < PAD_NACTIONS; a++) {
        if (g_bind[a] < 0) continue;
        wchar_t one[32];
        swprintf_s(one, L"%s%hs=%d", out[0] ? L"," : L"", g_action_key[a], g_bind[a]);
        wcsncat_s(out, cch, one, _TRUNCATE);
    }
}

void padmap_configure(const wchar_t* ini)
{
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    g_enabled = GetPrivateProfileIntW(L"inputmode", L"pad_remap", 0, ini);
    g_trace   = GetPrivateProfileIntW(L"inputmode", L"pad_trace",  0, ini);
    g_pad_title = GetPrivateProfileIntW(L"inputmode", L"pad_title", 1, ini);
    wchar_t style[32];
    ini_str(L"inputmode", L"pad_names", L"xbox", style, _countof(style), ini);
    {
        char s8[32]; size_t got = 0;
        wcstombs_s(&got, s8, sizeof(s8), style, _TRUNCATE);
        const PadLayout* L = layout_by_key(s8);
        if (L) g_style = (int)(L - g_layouts);
    }
    wchar_t bind[256];
    ini_str(L"inputmode", L"pad_bind", L"", bind, _countof(bind), ini);
    parse_bind(bind);
    ini_str(L"inputmode", L"pad_slots", L"", bind, _countof(bind), ini);
    parse_slots(bind);
    rebuild();
    if (g_enabled) {
        char names[256] = "";
        for (int a = 0; a < PAD_NACTIONS; a++) {
            char n[32], one[80];
            _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s=%s", names[0] ? " " : "",
                        g_action_label[a], padmap_button_name(g_bind[a], n, sizeof(n)));
            strncat_s(names, one, _TRUNCATE);
        }
        logf("[pad] live button mapping ON -- %s", names);
    }
}

// Live-reload for the in-game settings dialog: the two OBSERVER flags only.
// pad_remap / pad_bind (and pad_names / pad_slots with them) are deliberately
// NOT re-read: the live mapper owns those in memory -- the overlay edits them
// and padmap_save is what carries them to disk -- so re-reading the ini here
// would revert unsaved mapper state to whatever the file happens to hold.
void padmap_reload(const wchar_t* ini)
{
    g_trace   = GetPrivateProfileIntW(L"inputmode", L"pad_trace",  0, ini);
    g_pad_title = GetPrivateProfileIntW(L"inputmode", L"pad_title", 1, ini);
    logf("[reload] padmap: pad_trace=%d pad_title=%d (pad_remap/pad_bind stay "
         "with the live mapper)", g_trace, g_pad_title);
}

void padmap_save(const wchar_t* ini)
{
    wchar_t bind[256];
    padmap_format_bind(bind, _countof(bind));
    WritePrivateProfileStringW(L"inputmode", L"pad_bind", bind, ini);
    WritePrivateProfileStringW(L"inputmode", L"pad_remap", g_enabled ? L"1" : L"0", ini);
    wchar_t style[32];
    swprintf_s(style, L"%hs", g_layouts[g_style].key);
    WritePrivateProfileStringW(L"inputmode", L"pad_names", style, ini);
    wchar_t slots[256];
    format_slots(slots, _countof(slots));
    WritePrivateProfileStringW(L"inputmode", L"pad_slots", slots, ini);
    logf("[pad] saved pad_remap=%d pad_bind=%ls pad_names=%ls pad_slots=%ls",
         g_enabled, bind, style, slots);
}

int  padmap_enabled() { return g_enabled; }
void padmap_set_enabled(int on) { g_enabled = on ? 1 : 0; rebuild(); }

// ---------------------------------------------------------------------------
// report
// ---------------------------------------------------------------------------

static char*  g_rep;
static size_t g_repleft;

static void rep(const char* fmt, ...)
{
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_rep && g_repleft > 1) {
        size_t n = strlen(line);
        if (n + 2 > g_repleft) n = g_repleft - 2;
        memcpy(g_rep, line, n); g_rep += n; *g_rep++ = '\n'; *g_rep = 0;
        g_repleft -= (n + 1);
    }
}

int padmap_report(char* out, size_t cch)
{
    g_rep = out; g_repleft = cch;
    if (out && cch) *out = 0;

    rep("LIVE BUTTON MAPPING (pad_remap)");
    rep("");
    rep("pad_remap = %d   %s", g_enabled,
        g_enabled ? "(the permutation below is being applied to every poll)"
                  : "(OFF -- the client sees the pad exactly as the OS reports it)");
    rep("pad_names = %s   (button NAMES only -- an unmeasured index prints as 'button N'",
        g_layouts[g_style].key);
    rep("            rather than being guessed. The index is the truth; the name is a label.)");
    if (g_ndevices) {
        rep("device    : '%s', %ld buttons, %ld poll(s) observed",
            g_product[0] ? g_product : "(unnamed)", g_nbuttons, g_phys_seen);
        rep("            ^ this is the client's OWN DirectInput device -- ground truth.");
    } else {
        rep("device    : the client has NOT created a game-controller device.");
        rep("            Either no pad is connected, or the Viewer is in keyboard mode");
        rep("            ([inputmode] mode=force_gamepad fixes that on a Steam Deck), or");
        rep("            Steam Input is presenting a keyboard/mouse layout instead of a");
        rep("            pad. Until it does, the mapper reads XInput as a fallback and");
        rep("            NOTHING IS BEING REMAPPED -- there is no poll to remap.");
    }
    rep("");
    rep("%-10s %-14s %-6s %s", "action", "your button", "slot", "and that slot number came from");
    for (int a = 0; a < PAD_NACTIONS; a++) {
        char n[32], w[160];
        int L = padmap_pol_index(a, w, sizeof(w));
        wchar_t keypath[512] = L"";
        polbtn_registry(g_action_regW[a], keypath, _countof(keypath));
        rep("%-10s %-14s %-6d %s%s%ls", g_action_label[a],
            padmap_button_name(g_bind[a], n, sizeof(n)), L, w,
            keypath[0] ? "  " : "", keypath[0] ? keypath : L"");
        int c = padmap_slot_conflict(a);
        if (c >= 0)
            rep("           !! COLLISION: slot %d already belongs to %s, so THIS BINDING "
                "IS NOT APPLIED.", L, g_action_label[c]);
    }
    rep("");
    rep("permutation (what the client receives in each slot):");
    for (int L = 0; L < 12; L++) {
        char a[32], b[32];
        int act = -1;
        for (int i = 0; i < PAD_NACTIONS; i++) if (padmap_pol_index(i, NULL, 0) == L) act = i;
        rep("  slot %-2d %-12s <- %-12s %s", L,
            padmap_button_name(L, a, sizeof(a)),
            padmap_button_name(g_enabled ? g_phys_for[L] : L, b, sizeof(b)),
            act >= 0 ? g_action_label[act] : "");
    }
    rep("");
    rep("Read a row as: 'when the client looks at slot N -- which it calls the action on");
    rep("the right -- it is handed the state of the physical button in the middle.'");
    rep("");
    const TitleProfile* t = g_title;
    rep("PER-TITLE MAP (pad_title=%d): a title's own pad device is permuted against", g_pad_title);
    rep("THAT title's measured button order, not the shell's -- or passed through stock");
    rep("when the title's map is unmeasured (see the [prof] table in the log).");
    if (t)
        rep("  last title device seen: %s (ok=%d cancel=%d menu=%d navi=%d)",
            t->title, t->pad_slot[PAD_OK], t->pad_slot[PAD_CANCEL],
            t->pad_slot[PAD_MENU], t->pad_slot[PAD_NAVI]);
    else
        rep("  no title has polled a pad device this session -- only the shell's.");
    rep("");
    rep("The registry route (pad_layout / btn_ok / swap_confirm) is a SEPARATE mechanism");
    rep("and is still honoured: it changes which slot each action reads, and the index");
    rep("column above already accounts for it. If you are using this dialog you do not");
    rep("need it, and leaving it at its default is the simpler configuration.");
    g_rep = NULL;
    return g_ndevices ? 1 : 0;
}

// ---------------------------------------------------------------------------
// self-test -- the permutation is pure logic, so it is checkable with no pad, no
// Viewer and no Deck. Every rule this module claims is asserted here.
// ---------------------------------------------------------------------------

static int pm_fail = 0;
static void pm_check(const char* what, bool ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) pm_fail++;
}

int padmap_selftest()
{
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    pm_fail = 0;
    g_enabled = 1;

    // With nothing bound the map must be the identity -- an "on" switch alone may never
    // move a button.
    padmap_preset("off");
    bool ident = true;
    for (int i = 0; i < PADMAP_MAXBTN; i++) if (g_phys_for[i] != i || g_log_for[i] != i) ident = false;
    pm_check("no bindings -> identity permutation", ident);

    // The Steam Deck case. POL's defaults put Ok at slot 1; binding confirm to A(0) must
    // make slot 1 read physical 0, and -- the bijection rule -- must move whatever was
    // at slot 1 onto A's old slot rather than duplicating it.
    padmap_preset("off");
    padmap_bind(PAD_OK, 0);
    int L_ok = padmap_pol_index(PAD_OK, NULL, 0);
    pm_check("confirm bound to A -> its slot reads physical 0", g_phys_for[L_ok] == 0);
    bool bij = true;
    for (int i = 0; i < PADMAP_MAXBTN; i++)
        for (int j = i + 1; j < PADMAP_MAXBTN; j++)
            if (g_phys_for[i] == g_phys_for[j]) bij = false;
    pm_check("map stays a bijection (no button drives two actions)", bij);
    bool inv = true;
    for (int i = 0; i < PADMAP_MAXBTN; i++) if (g_log_for[g_phys_for[i]] != i) inv = false;
    pm_check("inverse map agrees with the forward map", inv);

    // A DIJOYSTATE goes through the real path: A down must arrive in the slot the client
    // reads for confirm.
    DIJOYSTATE js; ZeroMemory(&js, sizeof(js));
    js.rgbButtons[0] = 0x80;
    padmap_on_state(&js, sizeof(js), NULL);
    pm_check("A held -> confirm's slot is the one set", js.rgbButtons[L_ok] != 0);
    pm_check("A held -> physical slot 0 is NOT also set (unless it is the same slot)",
             L_ok == 0 || js.rgbButtons[0] == 0);

    // A custom data format must be left alone rather than scribbled on at a guessed
    // offset -- the pass-through this module promises.
    BYTE custom[24]; memset(custom, 0xAB, sizeof(custom));
    padmap_on_state(custom, sizeof(custom), NULL);
    bool untouched = true;
    for (int i = 0; i < (int)sizeof(custom); i++) if (custom[i] != 0xAB) untouched = false;
    pm_check("unknown data format is passed through untouched", untouched);

    // Presets.
    padmap_preset("xbox");
    pm_check("xbox preset: confirm on A, cancel on B",
             padmap_binding(PAD_OK) == 0 && padmap_binding(PAD_CANCEL) == 1);
    padmap_swap(PAD_OK, PAD_CANCEL);
    pm_check("swap confirm/cancel exchanges the two",
             padmap_binding(PAD_OK) == 1 && padmap_binding(PAD_CANCEL) == 0);

    // The Steam Deck: its pad enumerates as an XInput-emulated Xbox 360 controller with
    // 10 buttons, so the face four are the standard order and Cancel belongs on index 1.
    // Pinned because this file previously asserted the OPPOSITE (cancel=2, "index 2 is
    // B") on the strength of a reading taken through swap_confirm, which is inert without
    // the Controller key -- it measured the mechanism, not the pad. Binding Cancel to 2
    // put it on X, live, which is what retracted it.
    padmap_preset("deck");
    pm_check("deck preset: confirm 0, cancel 1, menu 7, navi 6",
             padmap_binding(PAD_OK) == 0 && padmap_binding(PAD_CANCEL) == 1 &&
             padmap_binding(PAD_MENU) == 7 && padmap_binding(PAD_NAVI) == 6);
    char bn[32];
    pm_check("deck names: index 1 is B",
             strncmp(padmap_button_name(1, bn, sizeof(bn)), "B (1)", 5) == 0);
    pm_check("deck names: index 2 is X, NOT B (the retraction)",
             strncmp(padmap_button_name(2, bn, sizeof(bn)), "X (2)", 5) == 0);
    pm_check("an index past the measured 10 is not given a guessed name",
             strncmp(padmap_button_name(10, bn, sizeof(bn)), "button 10", 9) == 0);
    padmap_preset("xbox");
    pm_check("switching preset switches the names with it",
             strncmp(padmap_button_name(2, bn, sizeof(bn)), "X (2)", 5) == 0);

    // A MEASURED slot must beat every inference, and must actually redirect the
    // permutation -- this is the escape hatch for a wrong guess about POL's defaults,
    // which is the last assumption left in the chain.
    padmap_preset("deck");
    padmap_learn_slot(PAD_OK, 5);
    pm_check("a measured slot overrides the guess", padmap_pol_index(PAD_OK, NULL, 0) == 5);
    DIJOYSTATE js3; ZeroMemory(&js3, sizeof(js3));
    js3.rgbButtons[0] = 0x80;                       // physical A, bound to confirm
    padmap_on_state(&js3, sizeof(js3), NULL);
    pm_check("A now arrives in the MEASURED confirm slot", js3.rgbButtons[5] != 0);
    padmap_learn_slot(PAD_OK, -1);
    pm_check("clearing a measurement falls back to the guess",
             padmap_pol_index(PAD_OK, NULL, 0) == 1);

    // The ini round trip: what the dialog writes must read back as the same bindings.
    padmap_preset("xbox");
    wchar_t line[256];
    padmap_format_bind(line, _countof(line));
    int saved[PAD_NACTIONS];
    for (int i = 0; i < PAD_NACTIONS; i++) saved[i] = g_bind[i];
    parse_bind(line);
    bool same = true;
    for (int i = 0; i < PAD_NACTIONS; i++) if (saved[i] != g_bind[i]) same = false;
    pm_check("pad_bind round-trips through the ini text", same);
    parse_bind(L"cancel=1,ok=0");
    pm_check("order-independent, partial line parses",
             g_bind[PAD_OK] == 0 && g_bind[PAD_CANCEL] == 1 && g_bind[PAD_MENU] == -1);
    parse_bind(L"ok=99,cancel=nonsense");
    pm_check("out-of-range / unparsable values clear rather than corrupt",
             g_bind[PAD_OK] == -1 && g_bind[PAD_CANCEL] == 0);

    // OFF must be genuinely inert, not "identity-ish".
    padmap_preset("xbox");
    padmap_set_enabled(0);
    DIJOYSTATE js2; ZeroMemory(&js2, sizeof(js2));
    js2.rgbButtons[1] = 0x80;
    padmap_on_state(&js2, sizeof(js2), NULL);
    pm_check("pad_remap=0 leaves the state exactly as it arrived",
             js2.rgbButtons[1] != 0 && js2.rgbButtons[0] == 0);
    padmap_set_enabled(1);

    // Buffered events must move with the same permutation the polled state does.
    padmap_preset("xbox");
    DIDEVICEOBJECTDATA od; ZeroMemory(&od, sizeof(od));
    od.dwOfs = FIELD_OFFSET(DIJOYSTATE, rgbButtons) + 0;   // physical A
    od.dwData = 0x80;
    padmap_on_data(&od, sizeof(od), 1, NULL);
    pm_check("buffered A event is reported in confirm's slot",
             od.dwOfs == FIELD_OFFSET(DIJOYSTATE, rgbButtons) + (DWORD)padmap_pol_index(PAD_OK, NULL, 0));

    // PER-TITLE MAP. Tetra Master's measured row is ok=1 cancel=2 menu=0 (profiles.cpp,
    // read out of TM.dll itself). With the deck bindings (confirm=A(0), cancel=B(2),
    // menu=7), a poll attributed to TM must land A in TM's OK slot 1 and button 7 in
    // TM's MENU slot 0 -- and the same poll with no owner must keep using the SHELL
    // permutation, because the two maps agree only by coincidence.
    static const TitleProfile tp_tm = { "TM.dll", "Tetra Master (test)",
                                        PW_SHIM_WINDOWED, NULL, "", { 1, 2, 0, -1 } };
    static const TitleProfile tp_un = { "Unmeasured.dll", "Unmeasured (test)",
                                        PW_DEFAULT, NULL, "", { -1, -1, -1, -1 } };
    padmap_set_enabled(1);
    padmap_preset("deck");
    DIJOYSTATE jt; ZeroMemory(&jt, sizeof(jt));
    jt.rgbButtons[0] = 0x80;                        // physical A
    jt.rgbButtons[7] = 0x80;                        // physical Menu
    padmap_on_state(&jt, sizeof(jt), &tp_tm);
    pm_check("title map: A arrives in TM's OK slot 1", jt.rgbButtons[1] != 0);
    pm_check("title map: button 7 arrives in TM's MENU slot 0", jt.rgbButtons[0] != 0);
    bool tbij = true;
    for (int i = 0; i < PADMAP_MAXBTN; i++)
        for (int j = i + 1; j < PADMAP_MAXBTN; j++)
            if (g_tp_phys_for[i] == g_tp_phys_for[j]) tbij = false;
    pm_check("title permutation stays a bijection", tbij);

    // A title with NO measured map must get a stock pad, not the shell's permutation.
    DIJOYSTATE ju; ZeroMemory(&ju, sizeof(ju));
    ju.rgbButtons[0] = 0x80;
    padmap_on_state(&ju, sizeof(ju), &tp_un);
    pm_check("unmeasured title: pad passed through stock",
             ju.rgbButtons[0] != 0 && ju.rgbButtons[1] == 0);

    // The title path must survive the shell's stand-down: g_in_title gates only the
    // owner-less (shell) device.
    padmap_note_title(1);
    DIJOYSTATE jg; ZeroMemory(&jg, sizeof(jg));
    jg.rgbButtons[0] = 0x80;
    padmap_on_state(&jg, sizeof(jg), &tp_tm);
    pm_check("title map still applies while note_title(1)", jg.rgbButtons[1] != 0);
    DIJOYSTATE jh; ZeroMemory(&jh, sizeof(jh));
    jh.rgbButtons[0] = 0x80;
    padmap_on_state(&jh, sizeof(jh), NULL);
    pm_check("shell device stands down while note_title(1)",
             jh.rgbButtons[0] != 0 && jh.rgbButtons[1] == 0);
    padmap_note_title(0);

    // Buffered events move with the title permutation too.
    DIDEVICEOBJECTDATA ot; ZeroMemory(&ot, sizeof(ot));
    ot.dwOfs = FIELD_OFFSET(DIJOYSTATE, rgbButtons) + 0;   // physical A
    ot.dwData = 0x80;
    padmap_on_data(&ot, sizeof(ot), 1, &tp_tm);
    pm_check("buffered A event lands in TM's OK slot 1",
             ot.dwOfs == FIELD_OFFSET(DIJOYSTATE, rgbButtons) + 1);

    // pad_title=0 turns the whole per-title path off -- stock, never the shell map.
    g_pad_title = 0;
    DIJOYSTATE jo; ZeroMemory(&jo, sizeof(jo));
    jo.rgbButtons[0] = 0x80;
    padmap_on_state(&jo, sizeof(jo), &tp_tm);
    pm_check("pad_title=0: title pad passed through stock",
             jo.rgbButtons[0] != 0 && jo.rgbButtons[1] == 0);
    g_pad_title = 1;
    g_title = NULL;

    // WHICH SOURCE DECIDES A SLOT -- the rule that broke the Deck on 2026-08-19.
    //
    // `pad_layout=xbox` with the measurements cleared made every face button dead: the
    // override is delivered through regredir's RegQueryValueEx hook, so with no
    // Settings\\Controller key the client never sees it and uses its COMPILED defaults --
    // while padmap was building the permutation against the override's claim. These
    // assert the rule directly, because the registry half cannot be arranged on a dev
    // box: this machine HAS the key, a Proton prefix has not, and a test that consulted
    // the real one would assert whatever the machine happened to be.
    {
        char w[160];
        pm_check("measured beats everything",
                 padmap_resolve_slot(PAD_OK, 5, 0, 1, w, sizeof(w)) == 5);
        pm_check("override counts WHEN the client can read it (key present)",
                 padmap_resolve_slot(PAD_OK, -1, 0, 1, w, sizeof(w)) == 0);
        // The regression itself.
        pm_check("override is IGNORED with no Controller key -- POL's default instead",
                 padmap_resolve_slot(PAD_OK, -1, 0, -1, w, sizeof(w)) == g_pol_default[PAD_OK]);
        pm_check("...and it SAYS the override is inert rather than claiming it applied",
                 strstr(w, "INERT") != NULL);
        pm_check("cancel falls back to POL's default too, not the xbox claim",
                 padmap_resolve_slot(PAD_CANCEL, -1, 1, -1, w, sizeof(w)) == g_pol_default[PAD_CANCEL]);
        pm_check("the registry answers when there is no override",
                 padmap_resolve_slot(PAD_OK, -1, -1, 3, w, sizeof(w)) == 3);
        pm_check("nothing anywhere -> POL's default, flagged as a guess",
                 padmap_resolve_slot(PAD_MENU, -1, -1, -1, w, sizeof(w)) == g_pol_default[PAD_MENU]
                 && strstr(w, "GUESSED") != NULL);
    }

    // SLOT COLLISION -- pinned to the exact config found live on the Deck 2026-08-19,
    // `pad_slots=ok=2,cancel=0,menu=2,navi=3` with the deck preset. Confirm and Menu both
    // claimed slot 2; before the guard, Menu's binding overwrote Confirm's and the map
    // printed as fully configured while Confirm did something nobody asked for.
    padmap_preset("deck");
    padmap_set_enabled(1);
    padmap_learn_slot(PAD_OK, 2);
    padmap_learn_slot(PAD_CANCEL, 0);
    padmap_learn_slot(PAD_MENU, 2);
    padmap_learn_slot(PAD_NAVI, 3);
    pm_check("collision is REPORTED against the earlier action",
             padmap_slot_conflict(PAD_MENU) == PAD_OK);
    pm_check("the earlier action is the one that keeps the slot",
             padmap_slot_conflict(PAD_OK) < 0);
    DIJOYSTATE jc; ZeroMemory(&jc, sizeof(jc));
    jc.rgbButtons[0] = 0x80;                      // physical A = the deck preset's Confirm
    padmap_on_state(&jc, sizeof(jc), NULL);
    pm_check("Confirm SURVIVES the collision (A reaches slot 2)", jc.rgbButtons[2] != 0);
    DIJOYSTATE jm; ZeroMemory(&jm, sizeof(jm));
    jm.rgbButtons[7] = 0x80;                      // physical Menu, the loser
    padmap_on_state(&jm, sizeof(jm), NULL);
    pm_check("the losing binding does NOT hijack the shared slot", jm.rgbButtons[2] == 0);
    for (int a = 0; a < PAD_NACTIONS; a++) padmap_learn_slot(a, -1);
    pm_check("clearing the measurements clears the collision",
             padmap_slot_conflict(PAD_MENU) < 0);

    padmap_preset("off");
    padmap_set_enabled(0);
    printf("\n%s (%d failure%s)\n", pm_fail ? "FAILED" : "all checks passed",
           pm_fail, pm_fail == 1 ? "" : "s");
    return pm_fail;
}
