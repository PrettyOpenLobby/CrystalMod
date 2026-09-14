// msgxlate.cpp -- translate the OS-level dialogs the POL titles raise.
//
// WHAT THIS COVERS, AND WHAT IT DOES NOT
//
// The POL titles carry translated UI, but their ERROR paths often do not: those
// dialogs are raised through user32's MessageBox with strings composed at
// runtime (a path plus a formatted Win32 error, say), so there is no resource to
// translate and they slip past a normal translation pass. This catches them at
// the API, which means it works for EVERY title at once -- FMO, Fantasy Earth,
// Tetra Master, FFXI and the Viewer itself -- because the hook is on user32, not
// on any one game.
//
// It does NOT catch a dialog a game DRAWS ITSELF. Measured 2026-08-14: FMO's
// "could not connect to the server" message never reaches MessageBox at all, so
// it is rendered in-engine from FMO's own string data and has to be translated
// there instead. Do not expect this module to find it.
//
// SELF-POPULATING, WHICH IS THE POINT
//
// A translation table is only useful if filling it is cheap. Every dialog that
// is NOT matched is appended to a sidecar file in EXACTLY the table's format, so
// the workflow is: run the client, hit the error, paste the sidecar line into the
// table, add the English. No rebuild -- the table is read at startup from disk.
//
// ENCODING
//
// MessageBoxA hands us bytes in some ANSI codepage; a Japanese title's strings
// are Shift-JIS (CP932) even on an English system, where they would otherwise
// render as mojibake. So an incoming A-string is decoded by TRYING CP932 first
// and the process ANSI codepage second, and matching is done in UTF-8 -- which
// is also the table file's encoding, so the file can be edited in any normal
// editor. Replacements are written back in the width the caller asked for.
#include "polshim.h"

#define MX_MAX_ENTRIES 512
#define MX_MAX_STR     1024

struct XlateEntry {
    char* src;          // UTF-8, as read from the table
    char* dst;          // UTF-8
    int   prefix;       // src ended with '*': match the start only
};

static int g_enable = 1;
static int g_log_missing = 1;
static XlateEntry g_tab[MX_MAX_ENTRIES];
static int  g_n = 0;
static LONG g_hits = 0, g_misses = 0;
static wchar_t g_missing_path[MAX_PATH];
static CRITICAL_SECTION g_cs;
static bool g_cs_ready = false;

// Remember what we have already written to the sidecar, so one dialog shown
// twenty times does not produce twenty identical lines to sift through.
static char* g_seen[MX_MAX_ENTRIES];
static int   g_nseen = 0;

int msgxlate_enabled() { return g_enable; }

// --- encoding helpers -------------------------------------------------------

static char* u8dup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)HeapAlloc(GetProcessHeap(), 0, n);
    if (p) memcpy(p, s, n);
    return p;
}

static bool w_to_u8(const wchar_t* w, char* out, size_t cb)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cb, NULL, NULL) > 0;
}

static bool u8_to_w(const char* u8, wchar_t* out, size_t cch)
{
    return MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, (int)cch) > 0;
}

// Decode an ANSI string to UTF-8, trying the Japanese codepage first.
// MB_ERR_INVALID_CHARS is what makes the first attempt a TEST rather than a
// guess: CP932 rejects byte sequences that are not valid Shift-JIS, so a plain
// Western string falls through to the process codepage instead of being
// silently mangled.
static bool a_to_u8(const char* a, char* out, size_t cb)
{
    wchar_t w[MX_MAX_STR];
    if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, a, -1, w, MX_MAX_STR) > 0)
        return w_to_u8(w, out, cb);
    if (MultiByteToWideChar(CP_ACP, 0, a, -1, w, MX_MAX_STR) > 0)
        return w_to_u8(w, out, cb);
    return false;
}

// --- the table --------------------------------------------------------------

static void trim(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = 0;
}

// Table format, tab-separated, UTF-8:
//     <source><TAB><replacement>
// '#' starts a comment. A source ending in '*' matches by PREFIX, which is what
// handles the dialogs built at runtime (a fixed sentence followed by a path).
// Literal "\n" in either column becomes a newline, since dialog text is often
// multi-line and a table of one-liners is much easier to edit.
static void unescape(char* s)
{
    char* r = s; char* w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r += 2; }
        else *w++ = *r++;
    }
    *w = 0;
}

static void load_table(const wchar_t* path)
{
    FILE* f = _wfopen(path, L"rb");
    if (!f) {
        logf("[mx] no translation table at %S -- nothing will be translated, but "
             "unmatched dialogs are still logged so the table can be built", path);
        return;
    }
    char line[MX_MAX_STR * 2];
    // Skip a UTF-8 BOM: editors add one, and it would otherwise become part of
    // the first source string and stop it ever matching.
    long start = 0;
    unsigned char bom[3];
    if (fread(bom, 1, 3, f) == 3 && !(bom[0]==0xEF && bom[1]==0xBB && bom[2]==0xBF))
        start = 0;
    else if (feof(f)) start = 0;
    else start = 3;
    fseek(f, start, SEEK_SET);

    while (fgets(line, sizeof(line), f) && g_n < MX_MAX_ENTRIES) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char* src = line;
        char* dst = tab + 1;
        if (!src[0] || !dst[0]) continue;
        unescape(src); unescape(dst);
        int pfx = 0;
        size_t sn = strlen(src);
        if (sn && src[sn-1] == '*') { src[sn-1] = 0; pfx = 1; }
        g_tab[g_n].src = u8dup(src);
        g_tab[g_n].dst = u8dup(dst);
        g_tab[g_n].prefix = pfx;
        if (g_tab[g_n].src && g_tab[g_n].dst) g_n++;
    }
    fclose(f);
    logf("[mx] loaded %d translation(s) from %S", g_n, path);
}

static const char* lookup(const char* u8)
{
    for (int i = 0; i < g_n; i++) {
        if (g_tab[i].prefix) {
            if (strncmp(u8, g_tab[i].src, strlen(g_tab[i].src)) == 0) return g_tab[i].dst;
        } else if (strcmp(u8, g_tab[i].src) == 0) {
            return g_tab[i].dst;
        }
    }
    return NULL;
}

// Append an unmatched string to the sidecar, in the table's own format, so
// filling the table is copy-paste rather than transcription.
static void note_missing(const char* u8, const char* where)
{
    if (!g_log_missing || !u8 || !u8[0]) return;
    if (!g_cs_ready) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nseen; i++)
        if (strcmp(g_seen[i], u8) == 0) { LeaveCriticalSection(&g_cs); return; }
    if (g_nseen >= MX_MAX_ENTRIES) {
        // Table full: we can no longer DEDUP a new string, so do not log it either --
        // otherwise every later occurrence appends again, the flood the dedup exists to
        // stop. Say so once so the truncation isn't silent.
        static bool said = false;
        bool first = !said; said = true;
        LeaveCriticalSection(&g_cs);
        if (first)
            logf("[mx] untranslated-string table full (%d distinct) -- further ones are "
                 "not appended to the sidecar this session", MX_MAX_ENTRIES);
        return;
    }
    g_seen[g_nseen++] = u8dup(u8);
    LeaveCriticalSection(&g_cs);

    FILE* f = _wfopen(g_missing_path, L"ab");
    if (!f) return;
    // Escape newlines so one dialog stays one line, matching the table's format.
    fprintf(f, "# untranslated (%s)\n", where ? where : "?");
    for (const char* p = u8; *p; p++) {
        if (*p == '\n') fputs("\\n", f);
        else if (*p != '\r') fputc(*p, f);
    }
    fputs("\t\n", f);          // trailing TAB: paste the English after it
    fclose(f);
    logf("[mx] UNTRANSLATED (%s): \"%s\"  -- appended to the sidecar",
         where ? where : "?", u8);
}

// --- the public entry points, called from inject.cpp's existing hooks --------

int msgxlate_a(const char* src, char* out, size_t cb, const char* where)
{
    if (!g_enable || !src || !src[0]) return 0;
    char u8[MX_MAX_STR * 2];
    if (!a_to_u8(src, u8, sizeof(u8))) return 0;
    const char* hit = lookup(u8);
    if (!hit) { InterlockedIncrement(&g_misses); note_missing(u8, where); return 0; }
    // Replacements are English, so the process codepage always represents them.
    wchar_t w[MX_MAX_STR];
    if (!u8_to_w(hit, w, MX_MAX_STR)) return 0;
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)cb, NULL, NULL) <= 0) return 0;
    InterlockedIncrement(&g_hits);
    return 1;
}

int msgxlate_w(const wchar_t* src, wchar_t* out, size_t cch, const char* where)
{
    if (!g_enable || !src || !src[0]) return 0;
    char u8[MX_MAX_STR * 2];
    if (!w_to_u8(src, u8, sizeof(u8))) return 0;
    const char* hit = lookup(u8);
    if (!hit) { InterlockedIncrement(&g_misses); note_missing(u8, where); return 0; }
    if (!u8_to_w(hit, out, cch)) return 0;
    InterlockedIncrement(&g_hits);
    return 1;
}

void msgxlate_configure(const wchar_t* ini)
{
    if (!ini) return;
    g_enable      = GetPrivateProfileIntW(L"msgxlate", L"enable", 1, ini);
    g_log_missing = GetPrivateProfileIntW(L"msgxlate", L"log_missing", 1, ini);
    if (!g_enable) return;

    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;

    // Both files sit next to the ini (i.e. next to the DLL), so a deployed
    // install carries its own table and its own harvest.
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, MAX_PATH, ini);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *(slash + 1) = 0; else dir[0] = 0;

    wchar_t tbl[MAX_PATH], name[128];
    ini_str(L"msgxlate", L"file", L"polshim-msg.tsv",
            name, 128, ini);
    _snwprintf_s(tbl, MAX_PATH, _TRUNCATE, L"%s%s", dir, name);
    ini_str(L"msgxlate", L"missing_file",
            L"polshim-msg-missing.tsv", name, 128, ini);
    _snwprintf_s(g_missing_path, MAX_PATH, _TRUNCATE, L"%s%s", dir, name);

    load_table(tbl);
}

void msgxlate_summary()
{
    if (!g_enable) return;
    logf("[mx] summary: %d entries loaded, %ld translated, %ld left alone",
         g_n, g_hits, g_misses);
    if (g_misses && g_log_missing)
        logf("[mx]   untranslated strings were appended to %S -- paste them into "
             "the table with an English column to translate them", g_missing_path);
}
