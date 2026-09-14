// vidfit.cpp -- place a title's DirectShow FMV through the GRAPH's own
// interfaces, instead of pushing its window around from the outside.
//
// WHY THIS EXISTS, AFTER d3d8hook.cpp's adopt_video_proc already tried
//
// The window route (EnumWindows -> find a "VideoRenderer"/"FilterGraphWindow"
// -> SetParent + SetWindowPos) is a guess in three places, and every one of them
// was measured wrong at least once:
//
//   * WHICH WINDOW. The class depends on the renderer the graph resolved to, so
//     the matcher needs a list that can never be complete (2026-08-17: FMO's is
//     FilterGraphWindow and there is no VideoRenderer window in the process).
//   * HOW BIG THE MOVIE IS. A window rect is the renderer's DEFAULT frame, not
//     the video: both titles sit at 320x240 outer / 304x201 client, and neither
//     number is the movie.
//   * WHETHER IT IS A WINDOW AT ALL. A VMR in WINDOWLESS mode has no video
//     window -- it composites into the application's own surface -- so there is
//     nothing to enumerate and no amount of window poking can move the picture.
//
// DirectShow answers all three exactly, and the calls are the ones the title
// itself should have made:
//
//   IBasicVideo::GetVideoSize          -- the movie's true dimensions
//   IVideoWindow::put_Owner            -- what SetParent was imitating
//   IVideoWindow::SetWindowPosition    -- in the owner's CLIENT coordinates
//   IVMRWindowlessControl9::SetVideoPosition  -- the windowless equivalent
//
// HOW WE GET THE GRAPH
//
// comtrace.cpp already sits on every CoCreateInstance in the process, and a
// title builds its graph through exactly that call. Measured on a live session:
//
//   [com] CoCreateInstance clsid={E436EBB3-524F-11CE-9F53-0020AF0BA770}
//         iid={56A868A9-0AD4-11CE-B03A-0020AF0BA770} -> OK
//         from=FE_Client.dll+0x33CE7
//
// -- CLSID_FilterGraph, IID_IGraphBuilder, from Fantasy Earth. So the object we
// need is handed to us on a path we already own; nothing has to be searched for.
//
// WHAT IT DOES NOT DO
//
// It does not build, run, seek or stop anything -- the title owns playback. This
// only ever answers "where should the picture be", which the title got wrong for
// one reason: it computed the placement from the fullscreen rect it believed it
// owned, and we took that fullscreen away when we forced it windowed. Every call
// here is idempotent and re-checked on a poll, so a title that repositions its
// own video wins the next tick and then agrees with us.
// SHIPPED OFF, 2026-08-17. d3d_fitvideo defaults to 0 everywhere.
//
// This feature has produced two regressions and one crash and has never once
// been observed to IMPROVE a movie, so the burden of proof is on turning it on.
//
// What the last live run established (build 38, FMO):
//
//   [vidfit] graph ...: WINDOWLESS movie is 800x600, game client is 1084x813
//            -> (0,0 1084x813) filling the client  (was 0,0 799x599)
//
// Exactly ONE placement -- the place-once rule works, and the fight was not a
// re-placement loop. The renderer is WINDOWLESS, which is the part that matters:
// a windowless VMR has no window of its own, it composites into the GAME'S
// window. Enlarging its destination rect from 800x600 to the whole client does
// not "position the movie" -- it hands the movie every pixel the game draws to.
// The user's report ("I dismiss it and it forcefully comes back") is consistent
// with the video repainting over the game across the full client, and the
// session then died with no crash record.
//
// HYPOTHESIS, not measured. What would settle it: whether the picture the user
// sees after dismissing is a live movie or a stale frame, and what the title
// does with the graph on dismiss (stop, or leave it running).
//
// The real question is not "where should the rect be" but "who owns the surface
// after the clip ends", and answering that needs the title's playback lifecycle,
// not a geometry calculation. Until someone has that, OFF is the honest default:
// an FMV in a corner is cosmetic, an FMV painted over the game is unplayable.
#include "polshim.h"
#include <dshow.h>
// d3d9.h BEFORE vmr9.h: the VMR-9 headers declare methods taking
// IDirect3DSurface9/IDirect3DDevice9 and do not include d3d9.h themselves, so
// on its own vmr9.h is a wall of C2061s. This is a separate translation unit
// from d3d8hook.cpp, whose hand-declared d3d8 ABI must never meet a real header.
#include <d3d9.h>
#include <vmr9.h>

// The graphs we have seen. Small and fixed: a title rebuilds its graph per clip,
// so this is a ring of the most recent ones rather than a registry.
#define VIDFIT_MAX_GRAPHS 8

// PLACE ONCE, THEN LEAVE IT ALONE.
//
// The first version re-imposed placement on every 500 ms tick whenever the video
// was not where we wanted it. That is a FIGHT, and we win every round because
// ours is always the last write: dismissing an FMV in Front Mission Online moved
// the video and we dragged it straight back, half a second later, so the movie
// could not be skipped at all (reported live, 2026-08-17). put_WindowStyle
// forcing WS_VISIBLE made it worse -- even hiding the video did not stick.
//
// "Where should the picture be" is worth answering ONCE per clip. A title moving
// its own video afterwards is a decision, not a mistake for us to correct. The
// only thing that legitimately invalidates our answer is the GAME window changing
// size, so that -- and only that -- re-arms us.
struct GraphSlot {
    IFilterGraph* fg;
    bool          placed;
    long          cw, ch;      // the game client we placed against
    // --- probe state (d3d_videoprobe), all "last seen" so we log CHANGES only
    long          st;          // OAFilterState + 1; 0 = never read
    RECT          rc;          // last video destination rect
    long          vis;         // last IVideoWindow visibility, -1 = n/a
    bool          named;       // have we listed this graph's filters yet
};

static CRITICAL_SECTION g_lock;
static bool             g_lock_ready = false;
static GraphSlot        g_graphs[VIDFIT_MAX_GRAPHS];
static int              g_ngraphs = 0;
static int              g_enable  = 0;
static LONG             g_n_placed = 0;

// [dx] d3d_videoprobe -- WATCH ONLY. Never moves anything.
//
// The one thing three attempts at this feature never had: the playback
// LIFECYCLE. We could always compute where the picture should go; we could never
// see when the clip starts, ends, or whether the title stops its graph. So every
// fix was geometry applied at a moment we could not identify, and the last one
// left a movie painted over the whole game with no way to dismiss it.
//
// IMediaControl::GetState is that missing signal, and it costs one call per
// graph per poll. This logs it -- with the video's current destination rect --
// whenever either CHANGES, and does nothing else. Independent of d3d_fitvideo on
// purpose: the whole point is to observe with placement switched OFF.
//
// What the log will settle, in one FMV:
//   * dismissing STOPS the graph      -> the picture left behind is a stale
//                                        frame, and clearing it is the fix
//   * dismissing leaves it RUNNING    -> the title never stops the movie; it
//                                        relied on the video rect being small,
//                                        and enlarging it is simply wrong
//   * the rect changes under us       -> the title repositions on dismiss and
//                                        we were fighting a moving target
static int              g_probe = 0;

void vidfit_configure(const wchar_t* ini)
{
    if (!ini) return;
    g_enable = ini_int_title(L"dx", L"d3d_fitvideo",  0, ini);
    g_probe  = GetPrivateProfileIntW(L"dx", L"d3d_videoprobe", 0, ini);
    if (g_probe)
        logf("[vidfit] PROBE ON (d3d_videoprobe=1): graph state + video rect will "
             "be logged on every change. Placement is %s.",
             g_enable ? "ALSO on (d3d_fitvideo=1)" : "OFF -- nothing will be moved");
}

// Live-reload for the in-game settings dialog. Both flags are consulted per call
// (vidfit_note_graph, vidfit_poll), so new values just apply. No thread work
// here on purpose: the poll lives on d3d8hook's video thread, and if that
// thread is not running these flags are inert -- which is handled there, not
// here. Nothing one-shot in this module's config to withhold.
void vidfit_reload(const wchar_t* ini)
{
    if (!ini) return;
    g_enable = ini_int_title(L"dx", L"d3d_fitvideo",  0, ini);
    g_probe  = GetPrivateProfileIntW(L"dx", L"d3d_videoprobe", 0, ini);
    logf("[reload] vidfit: d3d_fitvideo=%d d3d_videoprobe=%d", g_enable, g_probe);
}

// The EFFECTIVE answer, not the raw ini value: [dx] d3d_fitvideo ships 0 as a
// mitigation for build 37's unskippable-FMV bug in FMO (fixed in build 38, never
// lifted), so a title whose PROFILE asks for adoption must not be held off by it.
// See TitleProfile::adopt_fmv. The raw g_enable is still what the ini reports.
int vidfit_enabled() { return g_enable || d3d_title_fmv_adopt(); }
int vidfit_probe_enabled() { return g_probe; }
// Graphs are worth tracking for EITHER job -- the probe needs them with
// placement off, which is the configuration it exists to be run in.
int vidfit_wants_graphs() { return vidfit_enabled() || g_probe; }

static void lock_init_once()
{
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    // A second caller must not proceed before the section exists.
    while (!g_lock_ready) Sleep(0);
}

// Called by comtrace.cpp for every SUCCESSFUL CLSID_FilterGraph activation.
// Takes its own reference: the title may release the graph the moment a clip
// ends, and a pointer we did not AddRef would be a use-after-free on the next
// poll. The reference is dropped in vidfit_release_all when the title's window
// goes away, which bounds it to that title's session.
void vidfit_note_graph(void* punk)
{
    if ((!vidfit_enabled() && !g_probe) || !punk) return;
    IUnknown* u = (IUnknown*)punk;
    IFilterGraph* fg = NULL;
    if (FAILED(u->QueryInterface(IID_IFilterGraph, (void**)&fg)) || !fg) return;

    lock_init_once();
    EnterCriticalSection(&g_lock);
    bool dup = false;
    for (int i = 0; i < g_ngraphs; i++) if (g_graphs[i].fg == fg) { dup = true; break; }
    if (dup) {
        fg->Release();                       // already held; drop the extra ref
    } else if (g_ngraphs < VIDFIT_MAX_GRAPHS) {
        GraphSlot* s = &g_graphs[g_ngraphs++];
        s->fg = fg; s->placed = false; s->cw = 0; s->ch = 0;
        s->st = 0; s->vis = -1; s->named = false; ZeroMemory(&s->rc, sizeof(s->rc));
        logf("[vidfit] tracking filter graph %p (%d held) -- %s",
             (void*)fg, g_ngraphs,
             vidfit_enabled() ? "its FMV will be placed ONCE" : "WATCHING ONLY, not moving it");
    } else {
        // Full: retire the oldest. A stale graph is not worth a leak, and the
        // NEWEST is the one about to play.
        g_graphs[0].fg->Release();
        memmove(&g_graphs[0], &g_graphs[1], sizeof(g_graphs[0]) * (VIDFIT_MAX_GRAPHS - 1));
        GraphSlot* s = &g_graphs[VIDFIT_MAX_GRAPHS - 1];
        s->fg = fg; s->placed = false; s->cw = 0; s->ch = 0;
        s->st = 0; s->vis = -1; s->named = false; ZeroMemory(&s->rc, sizeof(s->rc));
    }
    LeaveCriticalSection(&g_lock);
}

// The largest rect with the movie's shape that fits `cw x ch`, centred in it.
// The same arithmetic fit_window uses on the game window itself -- fitting means
// fitting the SHAPE, at every level.
static void letterbox(long vw, long vh, long cw, long ch,
                      long* x, long* y, long* w, long* h)
{
    long tw = cw, th = (vw > 0) ? MulDiv(cw, vh, vw) : ch;
    if (th > ch) { th = ch; tw = (vh > 0) ? MulDiv(ch, vw, vh) : cw; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    *w = tw; *h = th;
    *x = (cw - tw) / 2;
    *y = (ch - th) / 2;
}

// WINDOWED renderer: the graph exposes IVideoWindow. Returns true if it handled
// this graph -- including "there is a video window and it is already right",
// which must not fall through to the windowless path.
static bool place_windowed(IFilterGraph* fg, HWND game, long cw, long ch)
{
    IVideoWindow* vw = NULL;
    if (FAILED(fg->QueryInterface(IID_IVideoWindow, (void**)&vw)) || !vw)
        return false;

    bool handled = false;
    long visible = 0;
    // get_Visible fails (VFW_E_NOT_CONNECTED) until the graph is actually
    // rendering something, which is the honest "no clip is playing" test -- far
    // better than the window route's IsWindowVisible guess.
    if (SUCCEEDED(vw->get_Visible(&visible)) && visible == OATRUE) {
        long vidw = 0, vidh = 0;
        IBasicVideo* bv = NULL;
        if (SUCCEEDED(fg->QueryInterface(IID_IBasicVideo, (void**)&bv)) && bv) {
            if (FAILED(bv->GetVideoSize(&vidw, &vidh))) { vidw = 0; vidh = 0; }
            bv->Release();
        }
        if (vidw > 0 && vidh > 0) {
            long x, y, w, h;
            letterbox(vidw, vidh, cw, ch, &x, &y, &w, &h);
            long cx = 0, cy = 0, cwid = 0, chgt = 0;
            vw->GetWindowPosition(&cx, &cy, &cwid, &chgt);
            OAHWND owner = 0;
            vw->get_Owner(&owner);
            // No "is it already right?" test: the caller only calls us while
            // this graph is unplaced, and re-testing here is how the fight got in.
            {
                // put_Owner is what SetParent was imitating, and it is the
                // supported call: the renderer re-parents, drops its frame and
                // starts routing messages itself. MessageDrain sends the mouse
                // and keyboard the movie window swallows back to the game, so a
                // click still skips the FMV.
                vw->put_Owner((OAHWND)game);
                vw->put_MessageDrain((OAHWND)game);
                vw->put_WindowStyle(WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
                HRESULT hr = vw->SetWindowPosition(x, y, w, h);
                InterlockedIncrement(&g_n_placed);
                logf("[vidfit] graph %p: movie is %ldx%ld, game client is %ldx%ld "
                     "-> placed at (%ld,%ld %ldx%ld)%s hr=0x%08lX (was owner=%p "
                     "at (%ld,%ld %ldx%ld))",
                     (void*)fg, vidw, vidh, cw, ch, x, y, w, h,
                     (w == cw && h == ch) ? " filling the client" : " letterboxed",
                     (unsigned long)hr, (void*)owner, cx, cy, cwid, chgt);
            }
            handled = true;
        }
    }
    vw->Release();
    return handled;
}

// WINDOWLESS renderer: there is no video window at all -- the VMR composites
// into the application's surface, and the picture's position is a property of
// the renderer filter, not of any HWND. This is the case no amount of window
// enumeration can reach, and the reason this file exists.
static bool place_windowless(IFilterGraph* fg, long cw, long ch)
{
    IEnumFilters* en = NULL;
    if (FAILED(fg->EnumFilters(&en)) || !en) return false;

    bool handled = false;
    IBaseFilter* f = NULL;
    ULONG got = 0;
    while (!handled && en->Next(1, &f, &got) == S_OK && f) {
        IVMRWindowlessControl9* wc = NULL;
        if (SUCCEEDED(f->QueryInterface(IID_IVMRWindowlessControl9, (void**)&wc)) && wc) {
            LONG vidw = 0, vidh = 0;
            if (SUCCEEDED(wc->GetNativeVideoSize(&vidw, &vidh, NULL, NULL)) &&
                vidw > 0 && vidh > 0) {
                long x, y, w, h;
                letterbox(vidw, vidh, cw, ch, &x, &y, &w, &h);
                RECT dst;
                dst.left = x; dst.top = y; dst.right = x + w; dst.bottom = y + h;
                RECT cur;
                ZeroMemory(&cur, sizeof(cur));
                wc->GetVideoPosition(NULL, &cur);
                {
                    HRESULT hr = wc->SetVideoPosition(NULL, &dst);
                    InterlockedIncrement(&g_n_placed);
                    logf("[vidfit] graph %p: WINDOWLESS movie is %ldx%ld, game "
                         "client is %ldx%ld -> (%ld,%ld %ldx%ld)%s hr=0x%08lX "
                         "(was %ld,%ld %ldx%ld)",
                         (void*)fg, vidw, vidh, cw, ch, x, y, w, h,
                         (w == cw && h == ch) ? " filling the client" : " letterboxed",
                         (unsigned long)hr, cur.left, cur.top,
                         cur.right - cur.left, cur.bottom - cur.top);
                }
                handled = true;
            }
            wc->Release();
        }
        f->Release();
        f = NULL;
    }
    en->Release();
    return handled;
}

// Called from d3d8hook.cpp's video poll, on its thread. Cheap when nothing is
// playing: one QI and one get_Visible per tracked graph.

// --- the probe: read, compare, log. It MUST NOT change anything ------------
static const char* state_name(OAFilterState s)
{
    switch (s) {
    case State_Stopped: return "STOPPED";
    case State_Paused:  return "PAUSED";
    case State_Running: return "RUNNING";
    }
    return "?";
}

// Reads the graph's run state and the video's current destination rect, and logs
// only when one of them has moved since last time. Writes NOTHING back.
static void probe_graph(GraphSlot* s)
{
    if (!s || !s->fg) return;

    OAFilterState fs = (OAFilterState)-1;
    bool have_state = false;
    IMediaControl* mc = NULL;
    if (SUCCEEDED(s->fg->QueryInterface(IID_IMediaControl, (void**)&mc)) && mc) {
        // Timeout 0: never block the poll thread on a graph mid-transition.
        // VFW_S_STATE_INTERMEDIATE is a SUCCESS code and still fills fs.
        if (SUCCEEDED(mc->GetState(0, &fs))) have_state = true;
        mc->Release();
    }

    RECT rc; ZeroMemory(&rc, sizeof(rc));
    long vis = -1;
    const char* how = "none";

    // TRY IVideoWindow, THEN FALL THROUGH -- do not make the VMR search an `else`.
    //
    // The filter graph MANAGER always exposes IVideoWindow; it delegates to
    // whatever renderer is in the graph. So the QI succeeds even when the
    // renderer is windowless, and only GetWindowPosition fails. Putting the VMR
    // search in the else branch meant it never ran, and the probe's first live
    // outing reported "video rect (0,0 0x0) via none" for a graph that build 38
    // had already driven through IVMRWindowlessControl9 successfully. The test
    // has to be "did I get a rect", not "did the QI work".
    IVideoWindow* vw = NULL;
    if (SUCCEEDED(s->fg->QueryInterface(IID_IVideoWindow, (void**)&vw)) && vw) {
        long x = 0, y = 0, w = 0, h = 0;
        if (SUCCEEDED(vw->GetWindowPosition(&x, &y, &w, &h))) {
            rc.left = x; rc.top = y; rc.right = x + w; rc.bottom = y + h;
            how = "IVideoWindow";
        }
        vw->get_Visible(&vis);
        vw->Release();
    }
    if (how[0] == 'n') {                       // still "none" -- try windowless
        IEnumFilters* en = NULL;
        if (SUCCEEDED(s->fg->EnumFilters(&en)) && en) {
            IBaseFilter* f = NULL; ULONG got = 0;
            while (en->Next(1, &f, &got) == S_OK && f) {
                IVMRWindowlessControl9* wc = NULL;
                if (SUCCEEDED(f->QueryInterface(IID_IVMRWindowlessControl9, (void**)&wc)) && wc) {
                    if (SUCCEEDED(wc->GetVideoPosition(NULL, &rc))) how = "VMR9 windowless";
                    wc->Release();
                    f->Release();
                    break;
                }
                f->Release(); f = NULL;
            }
            en->Release();
        }
    }

    // NAME THE FILTERS, ONCE PER GRAPH. Without this, "no video rect" is
    // ambiguous between "windowless renderer we failed to reach" and "this graph
    // has no video in it at all" -- and a title uses DirectShow for background
    // MUSIC as readily as for movies, so an audio-only graph is not a surprising
    // thing to be holding. Guessing which one we were looking at is how the last
    // round produced a confident answer to the wrong question.
    if (!s->named) {
        s->named = true;
        IEnumFilters* en = NULL;
        if (SUCCEEDED(s->fg->EnumFilters(&en)) && en) {
            IBaseFilter* f = NULL; ULONG got = 0;
            int n = 0;
            while (en->Next(1, &f, &got) == S_OK && f) {
                FILTER_INFO fi;
                ZeroMemory(&fi, sizeof(fi));
                if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
                    // QueryFilterInfo AddRefs the graph it hands back.
                    if (fi.pGraph) fi.pGraph->Release();
                    logf("[vidprobe]   graph %p filter %d: %ls", (void*)s->fg, n, fi.achName);
                }
                n++;
                f->Release(); f = NULL;
            }
            en->Release();
            logf("[vidprobe]   graph %p has %d filter(s)%s", (void*)s->fg, n,
                 n == 0 ? " -- EMPTY, nothing has been rendered into it yet" : "");
        }
    }

    long st_now = have_state ? (long)fs + 1 : 0;
    if (st_now == s->st && EqualRect(&rc, &s->rc) && vis == s->vis) return;

    logf("[vidprobe] graph %p: state %s -> %s | video rect (%ld,%ld %ldx%ld) via %s"
         "%s%s",
         (void*)s->fg,
         s->st ? state_name((OAFilterState)(s->st - 1)) : "-",
         have_state ? state_name(fs) : "unreadable",
         rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, how,
         vis >= 0 ? (vis == OATRUE ? " visible=1" : " visible=0") : "",
         (st_now != s->st && s->st) ? "   <-- STATE CHANGED" : "");
    log_flush();

    s->st = st_now;
    s->rc = rc;
    s->vis = vis;
}

void vidfit_poll(HWND game)
{
    if ((!vidfit_enabled() && !g_probe) || !g_ngraphs || !game || !IsWindow(game)) return;

    // WATCH FIRST, and independently of placement: with d3d_fitvideo=0 this is
    // the entire job. Snapshot under the lock, call outside it -- same rule as
    // the placement path below, for the same reason.
    if (g_probe) {
        IFilterGraph* pg[VIDFIT_MAX_GRAPHS];
        int pn = 0;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_ngraphs; i++) { pg[pn] = g_graphs[i].fg; pg[pn]->AddRef(); pn++; }
        LeaveCriticalSection(&g_lock);
        for (int i = 0; i < pn; i++) {
            EnterCriticalSection(&g_lock);
            GraphSlot* s = NULL;
            for (int j = 0; j < g_ngraphs; j++) if (g_graphs[j].fg == pg[i]) { s = &g_graphs[j]; break; }
            GraphSlot copy;
            if (s) copy = *s;
            LeaveCriticalSection(&g_lock);
            if (s) {
                probe_graph(&copy);
                EnterCriticalSection(&g_lock);
                for (int j = 0; j < g_ngraphs; j++)
                    if (g_graphs[j].fg == pg[i]) {
                        g_graphs[j].st = copy.st; g_graphs[j].rc = copy.rc;
                        g_graphs[j].vis = copy.vis; g_graphs[j].named = copy.named;
                        break;
                    }
                LeaveCriticalSection(&g_lock);
            }
            pg[i]->Release();
        }
    }
    if (!vidfit_enabled()) return;
    RECT gc;
    if (!GetClientRect(game, &gc) || gc.right <= 0 || gc.bottom <= 0) return;

    lock_init_once();
    // Copy under the lock, call outside it: a graph method can block (it talks to
    // the renderer), and holding the lock across that would stall comtrace's
    // CoCreateInstance hook -- which is on the title's own startup path.
    IFilterGraph* snap[VIDFIT_MAX_GRAPHS];
    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ngraphs; i++) {
        // Already answered for this client size -- the title owns the picture
        // from here. Only a resize of the GAME window re-arms us.
        if (g_graphs[i].placed &&
            g_graphs[i].cw == gc.right && g_graphs[i].ch == gc.bottom) continue;
        snap[n] = g_graphs[i].fg;
        snap[n]->AddRef();
        n++;
    }
    LeaveCriticalSection(&g_lock);

    for (int i = 0; i < n; i++) {
        bool done = place_windowed(snap[i], game, gc.right, gc.bottom);
        if (!done) done = place_windowless(snap[i], gc.right, gc.bottom);
        if (done) {
            EnterCriticalSection(&g_lock);
            // Re-find by POINTER: the ring may have shifted while we were out of
            // the lock, so an index captured above is not trustworthy.
            for (int j = 0; j < g_ngraphs; j++) {
                if (g_graphs[j].fg != snap[i]) continue;
                g_graphs[j].placed = true;
                g_graphs[j].cw = gc.right;
                g_graphs[j].ch = gc.bottom;
                break;
            }
            LeaveCriticalSection(&g_lock);
        }
        snap[i]->Release();
    }
}

// Release ONE held graph, and survive it being dead.
//
// Measured 2026-09-07 (pol.exe.324716.dmp, WER: quartz.dll+0x8315F, c0000005 READ 0):
// FFXI played its opening movie through polmvf, the graph was tracked here
// (212A6468, "WATCHING ONLY"), FFXI then exited twice on its own, and the Release
// below -- the LAST reference, ours -- ran quartz's graph destructor over filters
// the title had already torn down. `mov esi,[eax]` with eax=0 inside quartz, on our
// thread, and the whole Viewer went with it. A graph we merely WATCHED cost the
// user the session.
//
// The reference is held on purpose (the poll needs a live pointer), so the fix is
// not "stop holding it" but "our Release must never be what kills the process":
// the call is fenced with SEH, an access violation inside it is swallowed and
// logged, and the slot is dropped either way. A leaked refcount on an object that
// is already broken is the cheapest possible outcome. Own function so the SEH
// frame sits in a function with no C++ objects to unwind (C2712).
static bool release_graph_guarded(IFilterGraph* fg)
{
    __try {
        fg->Release();
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Drop every reference. Called when the title's window is gone, so our refs
// cannot outlive the session that created them.
void vidfit_release_all()
{
    if (!g_lock_ready || !g_ngraphs) return;
    EnterCriticalSection(&g_lock);
    int dead = 0;
    for (int i = 0; i < g_ngraphs; i++) {
        IFilterGraph* fg = g_graphs[i].fg;
        if (!fg) continue;
        if (!release_graph_guarded(fg)) {
            dead++;
            logf("[vidfit] graph %p was already torn down by its title -- our Release "
                 "faulted inside quartz and was swallowed (this used to crash pol.exe)",
                 (void*)fg);
        }
    }
    ZeroMemory(g_graphs, sizeof(g_graphs));
    int had = g_ngraphs;
    g_ngraphs = 0;
    LeaveCriticalSection(&g_lock);
    if (had) logf("[vidfit] released %d filter graph(s) (%d dead) -- the title's window is gone",
                  had, dead);
}

void vidfit_summary()
{
    if (!vidfit_enabled() && !g_probe) return;
    logf("[vidfit] summary: %ld placement(s), %d graph(s) held", g_n_placed, g_ngraphs);
}
