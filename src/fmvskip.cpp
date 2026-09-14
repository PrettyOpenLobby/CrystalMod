// fmvskip.cpp -- skip a title's opening/FMV DirectShow movie so Wine's quartz
// never has to decode it.
//
// WHY. Fantasy Earth (and FMO) play a movie through a DirectShow filter graph:
// CoCreateInstance(CLSID_FilterGraph, IID_IGraphBuilder) then
// IGraphBuilder::RenderFile(<the .avi>). On the Steam Deck under Proton, FE now
// reaches this point (the 0x2AC5C4 teardown is guarded, see fepatch.cpp) and dies
// SILENTLY right after the graph is created -- the next call is RenderFile, and
// Wine's DirectShow AVI/codec path is a long-standing crash source. FE's own
// OPENING_MOVIE=0 ini switch does not prevent the graph being built under Wine,
// so the reliable skip has to live below the title, at the graph.
//
// HOW. comtrace.cpp already sits on the CoCreateInstance that creates the graph
// (it hands the pointer to vidfit). When [dx] fmv_skip is on we wrap that pointer
// before it reaches the title. The wrapper forwards EVERY IGraphBuilder method to
// the real graph UNCHANGED except RenderFile: for a video-extension file it does
// NOT call the real RenderFile (so no decoder is ever built and nothing is
// decoded) and returns the level's chosen result. Audio-only / non-video paths
// and every other method pass straight through, so this cannot disturb a title
// that is not playing a movie.
//
// LEVELS ([dx] fmv_skip):
//   0  off -- no wrap.
//   1  trace only -- forward RenderFile to the real graph, log the path + HRESULT.
//      Answers "is the crash inside RenderFile, and on which file" with ZERO
//      behaviour change -- the safe first measurement.
//   2  skip, report success -- RenderFile returns S_OK without rendering. The
//      title believes the movie played; it Runs an empty graph, which is inert.
//      FE never calls IGraphBuilder::RenderFile (it dies setting the graph up),
//      so for FE only level 4 does anything. Kept for a title that does use it.
//   3  skip, report failure -- RenderFile returns VFW_E_CANNOT_RENDER, so a title
//      that checks the result takes its own "movie unavailable, skip it" path.
//      KEY: THIS IS FMO'S LEVEL (decompiled 2026-08-26, FrontMissionOnline.dll
//      0x6122AE00): its movie object AddFilters a VMR-9, then calls
//      IGraphBuilder::RenderFile(<install>\fmo.dat) -- vtable slot 0x34 -- and on
//      a FAILED result jumps to 0x6122AF0F, which is byte-for-byte its own
//      "movie finished" path (release the graph, clear the movie flag at
//      0x613CEF7C, request the next scene via 0x6102D000). So a failed RenderFile
//      IS the skip SE wrote, with no dangling object. The earlier claim that "FMO
//      never calls RenderFile" was false: it does, on a .dat -- see is_video_path.
//
// SAFETY. One interface wrapped, own refcount, the real graph's reference simply
// transferred in (no extra AddRef/Release to leak or double-free). QueryInterface
// returns `this` only for the graph interfaces and forwards everything else to the
// real object, so IMediaControl/IMediaEvent/IVideoWindow all come straight from
// quartz -- the title drives the real (now empty) graph exactly as before.
#include "polshim.h"
#include "profiles.h"
#include <dshow.h>
#include <wchar.h>
#include <stdarg.h>

static int g_level = 4;   // [dx] fmv_skip -- default REFUSE (see iniheal row)

// The ini path, kept so the CALLER-TIME decision below can re-read it. See
// fmvskip_level_for_caller: the per-title answer cannot be computed at configure()
// time because no title is in scope then.
static wchar_t g_ini[MAX_PATH] = L"";

// THIS WHOLE FEATURE IS A WINE WORKAROUND ALMOST EVERYWHERE, SO GATE IT ON WINE --
// BUT PER TITLE (see fmv_effective_level; TitleProfile::fmv_windows carries the
// exception, and Front Mission Online is currently the only title that has it).
//
// The skip exists because Wine's quartz dies BUILDING the graph -- FE's own
// OPENING_MOVIE=0 stops the playback but not the construction, measured on the
// Deck 2026-08-20 with the switch set in both GLOBAL.INI copies and no
// VirtualStore shadow. None of that is true on Windows: quartz is fine there,
// the title's own switch works, and we have no reason to touch the graph at all.
//
// And interfering is not free. Level 4 makes the title take its no-movie path,
// and on Windows THAT PATH CRASHES FANTASY EARTH: c0000005 at FE_Client RVA
// 0x1DBD24 right after the two logos, in a forwarding thunk
// `mov eax,[ecx+4]; mov ecx,[eax]; call [ecx+0x24]` faulting because [this+4] is
// NULL -- an object the movie path was supposed to build. Found live 2026-08-23,
// the same day the rev-6 iniheal migration turned fmv_skip=4 on for everyone.
//
// WARNING: WHY THIS OVERRIDES AN "EXPLICIT" VALUE, against the usual rule. The 4 sitting
// in the ini on a Windows box was not typed by a person -- iniheal rev 6 WROTE it
// into every install. Honouring it as intent would mean honouring a machine's
// guess about a platform it could not see. A user who really does want the
// wrap on Windows sets [dx] fmv_skip_force=1 and gets it, logged either way.
//
// (The Wine probe is duplicated from maskguard.cpp rather than shared through
// polshim.h on purpose: a two-line probe is not worth a dependency on the shared
// header.)
static bool fmv_under_wine()
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != NULL;
}

// IMPORTANT: THE STARTUP EXPLANATION WAS BEING THROWN AWAY (found 2026-08-25).
//
// fmvskip_configure runs from inject.cpp's *_configure block, which is ~40 lines
// BEFORE log_open(). Every logf below it went into a NULL FILE* and vanished, so
// a user who set [dx] fmv_skip and still got the opening movie found NOTHING
// in the log to explain it -- the one line that says "IGNORED, this is a Wine-only
// workaround" is exactly the line that could not be written. Reported as "the
// cutscene plays even though skip is enabled", and it cost this whole diagnosis.
//
// Same trap, same fix as dxhook.cpp's DPI declaration: RECORD the outcome and
// replay it from inject.cpp once the log exists (fmvskip_log_result, called
// beside dpi_log_result). The live-reload path logs immediately as before -- the
// log is open by then -- so `defer` is what distinguishes the two callers.
static char g_fmv_result[512] = "";

static void fmv_say(bool defer, const char* fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!defer) { logf("%s", msg); return; }
    // APPEND, don't overwrite: the force=1 path records its verdict AND then the
    // level line, and keeping only the last of the two would drop the reason.
    if (g_fmv_result[0]) strcat_s(g_fmv_result, sizeof(g_fmv_result), "\n");
    strcat_s(g_fmv_result, sizeof(g_fmv_result), msg);
}

void fmvskip_log_result()
{
    for (char* p = g_fmv_result; *p; ) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        logf("%s", p);
        if (!nl) break;
        p = nl + 1;
    }
}

// Is [dx.<module>] fmv_skip actually PRESENT for the title in scope? ini_int_title
// falls back to the global when it is not, and here the two must be told apart: an
// absent per-title key means "use the level this title's profile knows works", while
// a present one is the user's explicit choice and outranks the profile.
// The sentinel is the same trick iniheal uses -- "" is a legitimate value, absence is not.
static bool title_key_present(const wchar_t* ini, const wchar_t* key)
{
    wchar_t ts[96];
    if (!ini_title_section(ts, _countof(ts), L"dx")) return false;
    wchar_t probe[64] = L"";
    GetPrivateProfileStringW(ts, key, L"\x01", probe, _countof(probe), ini);
    return probe[0] != 1;
}
static bool title_level_set(const wchar_t* ini) { return title_key_present(ini, L"fmv_skip"); }

// IMPORTANT: FOURTH FAULT (2026-08-26, second Windows machine's log, build 149): `[dx] fmv_skip_force` is an
// iniheal row, so EVERY ini carries an explicit `fmv_skip_force=0`. Reading it with
// the profile as the DEFAULT meant the profile could only win when the key was
// absent -- and iniheal guarantees it never is. The healed zero shadowed the
// profile's level on every machine: `[fmv] graph: fmv_skip=4 IGNORED for
// FrontMissionOnline.dll` with the profile saying 3. Same class as
// shim-compiled-vs-healed-defaults / served-zeros-launder-into-choices.
//
// Resolution now: an EXPLICIT per-title `[dx.<module>] fmv_skip_force` is the
// user's word either way; a GLOBAL 1 forces every title; a global 0 -- the
// healed default -- is no opinion about this title, and the profile decides.
static int fmv_force_for_title(const wchar_t* ini, int prof_default)
{
    if (title_key_present(ini, L"fmv_skip_force"))
        return ini_int_title(L"dx", L"fmv_skip_force", prof_default, ini);
    if (GetPrivateProfileIntW(L"dx", L"fmv_skip_force", 0, ini) > 0)
        return 1;
    return prof_default;
}

// The ini value as written, then the platform gate. Returns the level to use and
// logs any downgrade, so a silent "why is the movie playing" is impossible.
static int fmv_effective_level(const wchar_t* ini, const char* when, bool defer)
{
    int want = ini_int_title(L"dx", L"fmv_skip", 4, ini);
    if (want <= 0) {
        // Also worth saying out loud: "off" and "on but gated off" look identical
        // from the outside, and only one of them is the user's own choice.
        fmv_say(defer, "[fmv] %s: fmv_skip=%d -- the movie skip is OFF; the title "
                "plays its own opening movie (FE: OPENING_MOVIE in "
                "Settings\\GLOBAL.INI, and check the VirtualStore copy too).",
                when, want);
        return want;
    }
    // IMPORTANT: THE PER-TITLE GATE USED TO END HERE, AND THAT WAS THE BUG (fixed 2026-08-27).
    //
    // This was a bare `if (fmv_under_wine()) return want;`, so everything below --
    // the whole reason profiles carry a level -- was WINDOWS-ONLY. Under Wine every
    // title got the global, and the global is 4. Level 4 is the one level our own
    // source says kills Fantasy Earth (RVA 0x1DBD24), so the Steam Deck had been
    // handing FE its documented poison for months while the same ini on Windows
    // gated it off correctly. See the fmv_wine_level note in profiles.h for the full
    // chain, including why the visible symptom was a crash in the VIEWER afterwards.
    //
    // The shape is now the same on both platforms: an explicit per-title fmv_skip is
    // the user's word; otherwise the profile's level for THIS platform decides;
    // otherwise the global stands. Wine needs no force check -- the skip is a
    // Wine workaround, so being under Wine IS the "on" condition.
    if (fmv_under_wine()) {
        const char* leaf = title_scope();
        const TitleProfile* prof = (leaf && *leaf) ? profile_for_module(leaf) : NULL;
        int wine_level = prof ? prof->fmv_wine_level : 0;
        if (wine_level && !title_level_set(ini)) {
            if (want != wine_level)
                fmv_say(defer, "[fmv] %s: using level %d for %s under Wine, not the "
                        "global %d -- that is this title's own level, and level %d is "
                        "not what its movie path answers to. Set [dx.%s] fmv_skip to "
                        "choose yourself.",
                        when, wine_level, leaf, want, want, leaf);
            return wine_level;
        }
        return want;
    }

    // ON WINDOWS THE GATE IS PER TITLE, NOT PROCESS-WIDE.
    //
    // It used to be one global GetPrivateProfileIntW, which meant a single answer for
    // every title pol.exe launches -- and the two titles disagree. Fantasy Earth MUST
    // stay gated (level 4's no-movie path crashes it, RVA 0x1DBD24, live 2026-08-23),
    // while Front Mission Online has no movie switch of its own and its unadopted
    // VMR-9 window plays over the game. One flag could not say both, so what shipped
    // was FE's answer for everybody: the settings dialog offered "Skip the opening
    // movie", it was ticked, and the movie played. Reported 2026-08-26.
    //
    // The profile supplies the DEFAULT and the ini still decides, in either direction:
    //   [dx] fmv_skip_force=1                        -- on for everything (as before)
    //   [dx.FrontMissionOnline.dll] fmv_skip_force=0 -- off for that title alone
    // Reading it through ini_int_title is what makes the second line work; the scope is
    // ambient and inject.cpp sets it around fmvskip_reload, so `def` below is already
    // this title's verdict by the time we ask.
    const char* leaf = title_scope();
    const TitleProfile* prof = (leaf && *leaf) ? profile_for_module(leaf) : NULL;
    int prof_level = prof ? prof->fmv_windows_level : 0;
    int def = prof_level ? 1 : 0;

    if (fmv_force_for_title(ini, def)) {
        // KEY: AND THE LEVEL, NOT JUST THE SWITCH. The levels are not interchangeable
        // per title (FE never calls RenderFile, so only 4 refuses its graph; FMO
        // does call it, on fmo.dat, and 3 is its own clean no-movie path), so a
        // title whose profile knows its level uses it unless
        // the user named one explicitly for that title. The global value stays in
        // charge of ON vs OFF -- a global 0 returned above, and that is obeyed.
        if (prof_level && !title_level_set(ini)) {
            if (want != prof_level)
                fmv_say(defer, "[fmv] %s: using level %d for %s, not the global %d -- "
                        "level %d is not the one this title's movie path answers to. "
                        "Set [dx.%s] fmv_skip to choose yourself.",
                        when, prof_level, (leaf && *leaf) ? leaf : "?", want, want,
                        (leaf && *leaf) ? leaf : "<module>");
            want = prof_level;
        }
        fmv_say(defer, "[fmv] %s: level %d kept on Windows for %s -- %s", when, want,
                (leaf && *leaf) ? leaf : "this process",
                def ? "this title ships no movie switch of its own (its profile "
                      "carries the level), so the shim's is the only one there is"
                    : "[dx] fmv_skip_force=1");
        return want;
    }

    fmv_say(defer,
         "[fmv] %s: fmv_skip=%d IGNORED for %s -- not running under Wine/Proton, so the "
         "movie WILL play. The skip is a Wine-quartz workaround, and on Windows "
         "level 4's no-movie path crashes Fantasy Earth (RVA 0x1DBD24). Use the "
         "title's own switch instead -- they are all rows in Settings > Each game's own "
         "settings now (FE: OPENING_MOVIE, Viewer: PlayOpeningMovie, FFXI: Show opening "
         "movie). [dx.<module>] fmv_skip_force=1 overrides for one title.",
         when, want, (leaf && *leaf) ? leaf : "this process");
    return 0;
}

void fmvskip_configure(const wchar_t* ini)
{
    if (ini) wcsncpy_s(g_ini, ini, _TRUNCATE);
    // defer=true: the log is not open yet. See fmvskip_log_result above.
    g_level = fmv_effective_level(ini, "startup", true);
    if (g_level)
        fmv_say(true, "[fmv] skip level %d -- %s", g_level,
             g_level == 1 ? "TRACE only (RenderFile forwarded + logged)" :
             g_level == 2 ? "SKIP, report success (movie not decoded) -- NOTE FE never "
                            "calls RenderFile, so this is a no-op for FE; FMO does (fmo.dat)" :
             g_level == 3 ? "SKIP, report failure (title takes its own skip path -- FMO's level)" :
             g_level == 4 ? "REFUSE the filter graph (title takes its no-movie path) "
                            "-- WARNING: CRASHES Fantasy Earth (RVA 0x1DBD24); FE wants 5" :
             g_level == 5 ? "BUILD the graph but never Run it (FE's level -- FE calls "
                            "no RenderFile, so nothing earlier can reach its movie)" :
                            "unknown level -- treated as off");
}

// Live-reload for the in-game settings dialog. g_level is read at wrap time and
// again inside every RenderFile, so a new level applies to the next graph the
// title builds -- and, for the RenderFile decision, even to one already wrapped.
// The one thing it cannot do is wrap a graph that was handed out UNWRAPPED while
// the level was 0: comtrace only calls fmvskip_wrap when the level is on, so
// 0 -> nonzero bites at the next CoCreateInstance, which is the next movie.
void fmvskip_reload(const wchar_t* ini)
{
    if (ini) wcsncpy_s(g_ini, ini, _TRUNCATE);
    // Same path as fmvskip_configure -- INCLUDING the Wine gate. A live reload
    // that disagreed with startup would silently change behaviour the first time
    // the settings dialog is saved, which is precisely how a Windows box would
    // re-arm the level-4 FE crash after we had gated it out at boot.
    g_level = fmv_effective_level(ini, "reload", false);
    logf("[reload] fmvskip: effective level=%d", g_level);
}

int fmvskip_enabled() { return g_level > 0; }

// IMPORTANT: DECIDE FROM THE CALLER, NOT FROM WHATEVER TITLE HAPPENS TO BE IN SCOPE.
//
// g_level is set by fmvskip_configure at injection (no title in scope -- so on Windows
// it is 0) and re-set by fmvskip_reload from d3d8hook's d3d_title_boundary as each
// title starts. That boundary is a CreateDevice. **A title builds its opening-movie
// graph BEFORE it creates its device** -- that is what an opening movie IS -- so at the
// moment the graph is requested, g_level is still the no-title answer and the wrap
// never armed. Measured on this machine: FMO's `CoCreateInstance
// clsid={E436EBB3-...}` (CLSID_FilterGraph) is in the log with no per-title `[fmv]`
// line anywhere before it.
//
// So the level is computed HERE, from the module that owns the return address --
// exactly the fact comtrace already prints as `from=FrontMissionOnline.dll`. The
// thread-local title scope is set around the read so [dx.<module>] resolves, and
// restored afterwards: the scope belongs to whoever set it.
//
// Cached per module, and logged once per module, because CoCreateInstance is hot.
int fmvskip_level_for_caller(void* ra)
{
    const TitleProfile* p = profile_for_addr(ra);
    const char* leaf = p ? p->module : NULL;

    static const char* s_logged[8];
    static int         s_levels[8];
    static int         s_n;
    for (int i = 0; i < s_n; i++)
        if (s_logged[i] == leaf) return s_levels[i];

    // No ini path yet means configure() has not run; there is nothing to read and
    // GetPrivateProfileString(NULL) would search %WINDIR% and answer with somebody
    // else's file. Fall back to the process-wide level, which is what we had before.
    if (!g_ini[0]) return g_level;

    char saved[80] = "";
    const char* prev = title_scope();
    if (prev) strncpy_s(saved, prev, _TRUNCATE);

    if (leaf) title_scope_set(leaf);
    int lvl = fmv_effective_level(g_ini, "graph", false);
    title_scope_set(saved[0] ? saved : NULL);

    if (s_n < 8) { s_logged[s_n] = leaf; s_levels[s_n] = lvl; s_n++; }
    return lvl;
}

// Video containers whose RenderFile we intercept. Audio-only playback (a title
// streaming BGM through a graph) is left alone: skipping it would drop the music,
// and it is not the crash.
static bool is_video_path(const wchar_t* path)
{
    if (!path) return false;
    // KEY: FMO's opening movie is `<install>\fmo.dat` -- a 270 MB MPEG-1 program
    // stream (00 00 01 BA) behind a .dat extension. The extension list below never
    // matched it, so every RenderFile("...\fmo.dat") was forwarded as "not video"
    // and the log showed no `[fmv] RenderFile` line at all. That absence was then
    // read as "FMO never calls RenderFile" (2026-08-23). It does; the filter was
    // blind to the name. Matched by leaf name, not by extension, so an audio .dat
    // in some other title is still left alone.
    {
        const wchar_t* leaf = wcsrchr(path, L'\\');
        const wchar_t* leaf2 = wcsrchr(path, L'/');
        if (leaf2 && (!leaf || leaf2 > leaf)) leaf = leaf2;
        leaf = leaf ? leaf + 1 : path;
        if (_wcsicmp(leaf, L"fmo.dat") == 0) return true;
    }
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) return false;
    static const wchar_t* kExt[] = {
        L".avi", L".bik", L".wmv", L".mpg", L".mpeg", L".mp4",
        L".m2v", L".asf", L".mov", L".sfd", L".pmp"
    };
    for (int i = 0; i < _countof(kExt); i++)
        if (_wcsicmp(dot, kExt[i]) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// FE loads and plays its movie through IMediaControl (measured: it QIs the graph
// for IMediaControl, IMediaEvent, IMediaPosition, IBasicAudio, then dies BEFORE
// IGraphBuilder::RenderFile -- so the load is IMediaControl::RenderFile, a slot we
// never saw). So the graph wrapper hands back a WRAPPED IMediaControl whose
// RenderFile is skipped and whose Run posts EC_COMPLETE (via the real graph's
// event sink) so the title's "play then wait for the movie to finish" completes
// instantly instead of decoding the .avi Wine crashes on.
class GraphSkip;
static IMediaControl* make_control_skip(GraphSkip* owner, IMediaControl* real);

// The graph wrapper. IGraphBuilder : IFilterGraph : IUnknown -- every slot
// forwarded to `m_real` except QueryInterface (identity for the graph interfaces,
// a skip-wrapper for IMediaControl) and RenderFile.
class GraphSkip : public IGraphBuilder {
public:
    GraphSkip(IGraphBuilder* real, const char* caller, int level)
        : m_real(real), m_ref(1), m_traced(0), m_qi_logged(0), m_skipped(0), m_level(level) {
        m_caller[0] = 0;
        if (caller) { strncpy(m_caller, caller, sizeof(m_caller) - 1); m_caller[sizeof(m_caller)-1] = 0; }
    }

    // Shared skip state, set by the IMediaControl wrapper when it skips a movie
    // RenderFile and read by its Run to decide whether to fake completion.
    void set_skipped() { InterlockedExchange(&m_skipped, 1); }
    bool skipped()     { return m_skipped != 0; }

    // Does this graph actually render VIDEO? Level 5 suppresses Run(), and this is
    // what keeps it from silencing a title's audio-only DirectShow playback: quartz
    // only exposes IVideoWindow once a video renderer is in the graph, so a graph
    // built for sound alone answers E_NOINTERFACE and is left strictly alone.
    // Asked at Run() time, not at wrap time -- at wrap time the graph is empty.
    bool has_video() {
        IVideoWindow* vw = NULL;
        if (SUCCEEDED(m_real->QueryInterface(IID_IVideoWindow, (void**)&vw)) && vw) {
            vw->Release();
            return true;
        }
        return false;
    }
    // IMPORTANT: THE LEVEL IS THE CALLER'S, NOT g_level. g_level is the process-wide answer,
    // which on Windows is 0 until a title's CreateDevice -- and the opening movie
    // is built BEFORE that. Every RenderFile decision below used to read g_level,
    // so the per-title level comtrace had resolved (and logged) never reached the
    // wrapper: it said "using level 4 for FrontMissionOnline.dll" and then skipped
    // nothing. The wrap now carries the level it was armed with; the live-reload
    // in fmvskip_reload still wins when it sets something (a user change).
    int level() const  { return g_level > 0 ? g_level : m_level; }
    const char* caller() const { return m_caller; }

    // Post EC_COMPLETE into the REAL graph's event queue so the title's completion
    // wait -- however it listens (WaitForCompletion, GetEvent, or an EC_COMPLETE
    // window notify via IMediaEventEx) -- fires at once for the skipped movie.
    void post_complete() {
        IMediaEventSink* sink = NULL;
        if (SUCCEEDED(m_real->QueryInterface(IID_IMediaEventSink, (void**)&sink)) && sink) {
            sink->Notify(EC_COMPLETE, (LONG_PTR)S_OK, 0);
            sink->Release();
        }
    }

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFilterGraph ||
            riid == IID_IGraphBuilder ||
            riid == __uuidof(IFilterGraph2)) {
            *ppv = static_cast<IGraphBuilder*>(this);
            AddRef();
            return S_OK;
        }
        // IMediaControl is the movie load+play path -- hand back a skip-wrapper.
        if (g_level >= 2 && riid == IID_IMediaControl) {
            IMediaControl* real_mc = NULL;
            HRESULT hr = m_real->QueryInterface(IID_IMediaControl, (void**)&real_mc);
            if (SUCCEEDED(hr) && real_mc) {
                IMediaControl* w = make_control_skip(this, real_mc);  // takes real_mc
                if (w) { *ppv = w; logf("[fmv] handed wrapped IMediaControl to %s", m_caller); log_flush(); return S_OK; }
                real_mc->Release();
            }
            *ppv = NULL;
            return FAILED(hr) ? hr : E_NOINTERFACE;
        }
        // Everything else (IMediaEvent, IMediaPosition, IBasicAudio, IVideoWindow,
        // ...) comes straight from the real graph, unwrapped. Trace which, once.
        bool trace = (g_level && m_qi_logged < 24);
        if (trace) {
            m_qi_logged++;
            const GUID& g = riid;
            logf("[fmv] graph QI {%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X} from %s ...",
                 (unsigned long)g.Data1, g.Data2, g.Data3,
                 g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                 g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], m_caller);
            log_flush();
        }
        HRESULT hr = m_real->QueryInterface(riid, ppv);
        if (trace) { logf("[fmv] graph QI -> 0x%08lX", (unsigned long)hr); log_flush(); }
        return hr;
    }
    // First call of each traced method, once per graph, so the log names the last
    // graph operation before a silent death without drowning in AddFilter spam.
    void trace1(DWORD bit, const char* what) {
        if (!g_level || (m_traced & bit)) return;
        m_traced |= bit;
        logf("[fmv] graph->%s (first call) from %s", what, m_caller);
        log_flush();
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) { m_real->Release(); delete this; }
        return (ULONG)n;
    }

    // --- IFilterGraph (all forwarded) ---
    HRESULT STDMETHODCALLTYPE AddFilter(IBaseFilter* f, LPCWSTR name) override { trace1(0x001, "AddFilter"); return m_real->AddFilter(f, name); }
    HRESULT STDMETHODCALLTYPE RemoveFilter(IBaseFilter* f) override { return m_real->RemoveFilter(f); }
    HRESULT STDMETHODCALLTYPE EnumFilters(IEnumFilters** e) override { trace1(0x002, "EnumFilters"); return m_real->EnumFilters(e); }
    HRESULT STDMETHODCALLTYPE FindFilterByName(LPCWSTR name, IBaseFilter** f) override { return m_real->FindFilterByName(name, f); }
    HRESULT STDMETHODCALLTYPE ConnectDirect(IPin* a, IPin* b, const AM_MEDIA_TYPE* mt) override { trace1(0x004, "ConnectDirect"); return m_real->ConnectDirect(a, b, mt); }
    HRESULT STDMETHODCALLTYPE Reconnect(IPin* p) override { return m_real->Reconnect(p); }
    HRESULT STDMETHODCALLTYPE Disconnect(IPin* p) override { return m_real->Disconnect(p); }
    HRESULT STDMETHODCALLTYPE SetDefaultSyncSource() override { return m_real->SetDefaultSyncSource(); }

    // --- IGraphBuilder ---
    HRESULT STDMETHODCALLTYPE Connect(IPin* out, IPin* in) override { trace1(0x008, "Connect"); return m_real->Connect(out, in); }
    HRESULT STDMETHODCALLTYPE Render(IPin* out) override { trace1(0x010, "Render(pin)"); return m_real->Render(out); }
    HRESULT STDMETHODCALLTYPE RenderFile(LPCWSTR file, LPCWSTR playlist) override {
        int lvl = level();
        if (lvl >= 2 && is_video_path(file)) {
            // KEY: NEVER REPORT SUCCESS FOR A GRAPH WE DID NOT BUILD.
            //
            // Level 5's contract is "BUILD the graph but never Run it", and it is
            // FE's level because FE never calls RenderFile -- so for FE the branch
            // is unreachable and the contract holds. A title that DOES call it and
            // is handed S_OK is told its movie rendered when nothing was
            // constructed, and the next call it makes on that graph faults inside
            // quartz. Measured on the Deck 2026-09-08 with FMO at the global 5:
            // quartz.dll+0x1E32F, READ from 0x4C, returning through
            // FrontMissionOnline.dll+0x22AFF0.
            //
            // So 5 answers like 3 here -- a refusal the title can act on, which is
            // its own no-movie path. FMO also carries fmv_wine_level=3 now, which
            // stops it reaching this case at all; this is the guard for the NEXT
            // title in that position rather than a second copy of that fix.
            HRESULT hr = (lvl == 3 || lvl >= 5) ? VFW_E_CANNOT_RENDER : S_OK;
            logf("[fmv] SKIP RenderFile(\"%ls\") from %s -- not decoded, returning 0x%08lX",
                 file ? file : L"(null)", m_caller, (unsigned long)hr);
            log_flush();
            return hr;
        }
        // Level 1 (trace), or a non-video file at any level: forward and report.
        HRESULT hr = m_real->RenderFile(file, playlist);
        if (is_video_path(file))
            logf("[fmv] RenderFile(\"%ls\") from %s -> 0x%08lX (forwarded at level %d; [dx] fmv_skip>=2 to skip)",
                 file ? file : L"(null)", m_caller, (unsigned long)hr, lvl);
        log_flush();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE AddSourceFilter(LPCWSTR file, LPCWSTR name, IBaseFilter** f) override {
        if (g_level)
            logf("[fmv] graph->AddSourceFilter(\"%ls\") from %s%s", file ? file : L"(null)", m_caller,
                 is_video_path(file) ? "  <-- VIDEO SOURCE" : "");
        log_flush();
        return m_real->AddSourceFilter(file, name, f);
    }
    HRESULT STDMETHODCALLTYPE SetLogFile(DWORD_PTR h) override { return m_real->SetLogFile(h); }
    HRESULT STDMETHODCALLTYPE Abort() override { trace1(0x020, "Abort"); return m_real->Abort(); }
    HRESULT STDMETHODCALLTYPE ShouldOperationContinue() override { return m_real->ShouldOperationContinue(); }

private:
    IGraphBuilder* m_real;
    LONG           m_ref;
    int            m_level;    // the caller's level at wrap time (see level())
    char           m_caller[48];
    DWORD          m_traced;
    int            m_qi_logged;
    volatile LONG  m_skipped;
};

// The IMediaControl skip-wrapper. IMediaControl : IDispatch : IUnknown. Every slot
// forwards to the real control except RenderFile (skip the movie) and Run (fake
// completion for a skipped movie). Holds a ref on the owning GraphSkip so the
// shared skip flag and the event sink outlive it.
class ControlSkip : public IMediaControl {
public:
    ControlSkip(GraphSkip* owner, IMediaControl* real)
        : m_owner(owner), m_real(real), m_ref(1) { m_owner->AddRef(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IMediaControl) {
            *ppv = static_cast<IMediaControl*>(this); AddRef(); return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) { m_real->Release(); m_owner->Release(); delete this; }
        return (ULONG)n;
    }

    // --- IDispatch (forwarded) ---
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* p) override { return m_real->GetTypeInfoCount(p); }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT i, LCID l, ITypeInfo** t) override { return m_real->GetTypeInfo(i, l, t); }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID r, LPOLESTR* n, UINT c, LCID l, DISPID* d) override { return m_real->GetIDsOfNames(r, n, c, l, d); }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID d, REFIID r, LCID l, WORD f, DISPPARAMS* pp, VARIANT* pv, EXCEPINFO* pe, UINT* pa) override { return m_real->Invoke(d, r, l, f, pp, pv, pe, pa); }

    // --- IMediaControl ---
    HRESULT STDMETHODCALLTYPE Run() override {
        if (m_owner->skipped()) {
            logf("[fmv] IMediaControl::Run on a SKIPPED movie (%s) -- posting EC_COMPLETE, not running",
                 m_owner->caller());
            log_flush();
            m_owner->post_complete();
            return S_OK;
        }
        // LEVEL 5 -- SKIP AT RUN, THE ONLY SKIP FANTASY EARTH ANSWERS TO.
        //
        // Every other level intercepts a LOAD call (RenderFile on either interface),
        // and FE makes none: it builds its graph itself and starts it here, which is
        // why levels 2 and 3 were silently no-ops for it and why level 4 -- refusing
        // the graph outright -- was historically the only thing that stopped its
        // movie. But level 4 also denies FE the graph object its no-movie path
        // assumes exists, and it dies on that (RVA 0x1DBD24, see profiles.h).
        //
        // So: let FE build the graph completely, then decline to START it. Nothing is
        // withheld, no call fails, the object graph FE reaches for afterwards is all
        // there -- the movie simply never plays, and EC_COMPLETE is posted so a
        // "play, then wait for the end" sequence finishes at once instead of hanging.
        if (m_owner->level() >= 5 && m_owner->has_video()) {
            logf("[fmv] level 5: IMediaControl::Run SUPPRESSED for %s -- graph built "
                 "and left intact, playback never started; posting EC_COMPLETE",
                 m_owner->caller());
            log_flush();
            m_owner->set_skipped();     // Stop/StopWhenReady now agree with Run
            m_owner->post_complete();
            return S_OK;
        }
        return m_real->Run();
    }
    HRESULT STDMETHODCALLTYPE Pause() override { return m_real->Pause(); }
    HRESULT STDMETHODCALLTYPE Stop() override { return m_real->Stop(); }
    HRESULT STDMETHODCALLTYPE GetState(LONG ms, OAFilterState* pfs) override { return m_real->GetState(ms, pfs); }
    HRESULT STDMETHODCALLTYPE RenderFile(BSTR fn) override {
        // IMediaControl::RenderFile is the automation load call. This is FE's movie
        // load path. Skip a video file; leave audio/other alone.
        bool vid = is_video_path((const wchar_t*)fn);
        int lvl = m_owner->level();
        if (lvl >= 2 && vid) {
            HRESULT hr = (lvl == 3) ? VFW_E_CANNOT_RENDER : S_OK;
            logf("[fmv] SKIP IMediaControl::RenderFile(\"%ls\") from %s -- not decoded, returning 0x%08lX",
                 fn ? (const wchar_t*)fn : L"(null)", m_owner->caller(), (unsigned long)hr);
            log_flush();
            if (SUCCEEDED(hr)) m_owner->set_skipped();
            return hr;
        }
        HRESULT hr = m_real->RenderFile(fn);
        logf("[fmv] IMediaControl::RenderFile(\"%ls\") from %s -> 0x%08lX (forwarded)",
             fn ? (const wchar_t*)fn : L"(null)", m_owner->caller(), (unsigned long)hr);
        log_flush();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE AddSourceFilter(BSTR fn, IDispatch** ppUnk) override {
        logf("[fmv] IMediaControl::AddSourceFilter(\"%ls\") from %s",
             fn ? (const wchar_t*)fn : L"(null)", m_owner->caller());
        log_flush();
        return m_real->AddSourceFilter(fn, ppUnk);
    }
    HRESULT STDMETHODCALLTYPE get_FilterCollection(IDispatch** p) override { return m_real->get_FilterCollection(p); }
    HRESULT STDMETHODCALLTYPE get_RegFilterCollection(IDispatch** p) override { return m_real->get_RegFilterCollection(p); }
    HRESULT STDMETHODCALLTYPE StopWhenReady() override {
        if (m_owner->skipped()) { m_owner->post_complete(); return S_OK; }
        return m_real->StopWhenReady();
    }

private:
    GraphSkip*     m_owner;
    IMediaControl* m_real;
    LONG           m_ref;
};

static IMediaControl* make_control_skip(GraphSkip* owner, IMediaControl* real)
{
    return new ControlSkip(owner, real);   // takes ownership of `real`
}

// Called by comtrace.cpp for every successful CLSID_FilterGraph activation whose
// interface is the graph builder. Returns the HRESULT comtrace should hand the
// title: S_OK when the graph is wrapped/traced (the usual case), or a FAILURE at
// level 4, where the graph is refused outright so the title takes its own
// "no movie" path -- FE dies SETTING UP the movie graph under Wine (before ever
// calling RenderFile), so the only skip that helps is denying it a graph at all.
// FMO is the opposite: it DOES call RenderFile (on fmo.dat) and handles a failure
// cleanly, so level 3 is its skip -- see the level table at the top.
// `caller` is a human label (module+RVA) for the log; NULL is fine.
// `level` is the CALLER's level (fmvskip_level_for_caller). It must not be read
// from g_level here: on Windows g_level is 0 while the opening movie is built,
// which is exactly why builds 145/146 resolved level 4 for FMO, logged it, and
// then returned S_OK on this function's first line.
// Refcount: the incoming reference on the real graph is TRANSFERRED to the
// wrapper (or released, at level 4), so no extra AddRef/Release to leak here.
HRESULT fmvskip_wrap(REFIID riid, void** ppv, const char* caller, int level)
{
    if (level <= 0) level = g_level;
    if (level <= 0 || !ppv || !*ppv) return S_OK;
    // Only act when *ppv really is a graph-builder-shaped pointer. The title asks
    // for IID_IGraphBuilder (measured); guard so a different riid is never
    // reinterpret_cast to the wrong vtable.
    if (!(riid == IID_IGraphBuilder || riid == IID_IFilterGraph ||
          riid == __uuidof(IFilterGraph2) || riid == IID_IUnknown))
        return S_OK;

    IGraphBuilder* real = NULL;
    if (FAILED(((IUnknown*)*ppv)->QueryInterface(IID_IGraphBuilder, (void**)&real)) || !real)
        return S_OK;                              // not actually a graph builder

    // We now hold one ref via `real`. Drop the original interface pointer's ref
    // (the caller's), since we either wrap `real` or refuse it below.
    ((IUnknown*)*ppv)->Release();

    // WARNING: `== 4`, NOT `>= 4`. Level 5 is a HIGHER number that must do LESS here: it
    // needs the graph to exist and be wrapped so it can decline the Run later, and a
    // `>=` test would refuse it instead -- handing Fantasy Earth back the very
    // no-graph condition (RVA 0x1DBD24) that level 5 exists to avoid. Refusal is one
    // specific strategy, not the top of a scale.
    if (level == 4) {
        // REFUSE: release the real graph, null the out-pointer, and tell comtrace
        // to return a class-not-registered failure. A title that checks the
        // CoCreateInstance result then skips the movie instead of setting it up.
        real->Release();
        *ppv = NULL;
        logf("[fmv] REFUSED filter graph for %s -- returning REGDB_E_CLASSNOTREG so "
             "the title skips its movie (it dies setting the graph up under Wine)",
             caller ? caller : "?");
        log_flush();
        return REGDB_E_CLASSNOTREG;
    }

    GraphSkip* w = new GraphSkip(real, caller, level);   // takes ownership of `real`
    *ppv = static_cast<IGraphBuilder*>(w);
    logf("[fmv] wrapped filter graph %p -> %p (from %s) at level %d -- movie RenderFile will be "
         "%s", (void*)real, (void*)w, caller ? caller : "?", level,
         level == 1 ? "traced" :
         level == 3 ? "skipped (reported as a FAILURE: the title's own no-movie path)" : "skipped");
    log_flush();
    return S_OK;
}
