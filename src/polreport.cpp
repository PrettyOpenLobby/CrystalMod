// polreport.cpp -- the player presses one key and we keep the whole story.
//
// WHY THIS EXISTS. A bug that reaches the server operator as "the game went
// black" an hour later is the start of an investigation, not the end of one:
// this session's `polshim.<pid>.log` is overwritten by the next launch, the
// server's own logs have rolled past the moment that mattered, and the machine
// facts that usually decide it (GPU, driver, display scaling, an overlay) were
// never written down anywhere. So the player hits the report key the moment it
// happens, types what went wrong, and the client's log, its ini, a screenshot
// and a diagnostics file go to the server -- which files them beside the SAME
// MINUTES cut from its own logs, under one id the player is shown.
//
//   [report]
//   enable=1            ; the key is armed
//   hotkey=end,ctrl+shift+r
//   pad_chord=back+rb   ; for controllers (a Steam Deck has no End key)
//   screenshot=1        ; include a PNG of the game window
//   diagnostics=1       ; include diag.txt (system and graphics details)
//   max_log_bytes=1048576
//   include_ini=1
//   include_prev=1      ; the previous session's log, when it crashed
//
// NOTHING IS SENT WITHOUT THE PLAYER. The key gathers locally and opens a box
// that says what will be sent; only the Send button sends it. Cancel discards
// everything, including the temporary screenshot.
//
// THE SNAPSHOT HAPPENS AT THE KEYPRESS, BEFORE THE BOX OPENS. The player takes
// tens of seconds to type; in that time the game keeps running, the log keeps
// growing, and the screen stops showing what they are describing. Gathering
// after the dialog would capture a screenshot of a dismissed dialog over a
// recovered game, and a log whose tail is the recovery rather than the fault.
//
// It must not grow its own snapshot or redaction: both are borrowed from
// logship.cpp, which carries the rules that keep a credential off the wire.
//
// THE FORMAT MUST AGREE BYTE FOR BYTE with the server's parser
// (services/issuereport.py, parse_bundle): a length-prefixed bundle, header
// scalars first, then one "==== FILE <name> <len> ====" entry per file.
#include "polshim.h"

#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "buildnum.h"
#include "profiles.h"

char* logship_snapshot_redacted(const wchar_t* path, DWORD max_bytes, DWORD* out_len);
int   logship_redact_inplace(char* buf, DWORD len);
bool  logship_endpoint(const char* leaf, char* out, size_t cch);
const wchar_t* logship_logpath();
bool  logship_prev_logpath(wchar_t* out, size_t cch);
char* sysdiag_collect(DWORD* out_len);

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------
// Unarmed until polreport_configure runs: under [polshim] bypass the settings
// watcher still runs but this module is never configured, and a key that
// gathered nothing and knew no server would only produce a failure box.
static int     g_enable = 0;
static int     g_shot = 1;
static int     g_diag = 1;
static DWORD   g_max_log = 1024 * 1024;
static DWORD   g_max_shot = 8 * 1024 * 1024;
static int     g_include_ini = 1;
static int     g_include_prev = 1;
static wchar_t g_ini[MAX_PATH];
static LONG    g_open = 0;           // the dialog is up

void polreport_configure(const wchar_t* ini)
{
    if (!ini) return;
    wcsncpy_s(g_ini, ini, _TRUNCATE);
    g_enable       = GetPrivateProfileIntW(L"report", L"enable", 1, ini);
    g_shot         = GetPrivateProfileIntW(L"report", L"screenshot", 1, ini);
    g_diag         = GetPrivateProfileIntW(L"report", L"diagnostics", 1, ini);
    g_include_ini  = GetPrivateProfileIntW(L"report", L"include_ini", 1, ini);
    g_include_prev = GetPrivateProfileIntW(L"report", L"include_prev", 1, ini);
    // A modest log cap: a report is anchored to a moment the player just
    // watched, the server keeps only ten minutes of its own logs to match, and
    // the bundle also carries a PNG. 1 MB of tail is minutes of play.
    g_max_log  = (DWORD)GetPrivateProfileIntW(L"report", L"max_log_bytes",
                                              1024 * 1024, ini);
    g_max_shot = (DWORD)GetPrivateProfileIntW(L"report", L"max_shot_bytes",
                                              8 * 1024 * 1024, ini);
}

int polreport_armed() { return g_enable; }

// ---------------------------------------------------------------------------
// the bundle
// ---------------------------------------------------------------------------
//
// LENGTH-PREFIXED, NOT SEPARATOR-SCANNED. The payload is LOGS: whatever
// separator you pick, a log can contain it. A byte count cannot be spoofed by
// content, and it carries the PNG with no base64.
//
//     ==== POLSHIM-REPORT 1 ====\n
//     host: DESKTOP-1234\n
//     ...\n
//     \n
//     ==== FILE description.txt 63 ====\n
//     <exactly 63 bytes>\n
//
// The header block is single-line scalars ONLY. The player's text is
// multi-line by nature and rides as `description.txt`, a file like any other.

struct Buf {
    char*  p;
    size_t len, cap;
    bool   bad;      // an allocation failed; every further append is a no-op
};

static void buf_need(Buf* b, size_t extra)
{
    if (b->bad) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t want = b->cap ? b->cap : 8192;
    while (want < b->len + extra + 1) want *= 2;
    char* np = (char*)realloc(b->p, want);
    if (!np) { b->bad = true; return; }
    b->p = np; b->cap = want;
}

static void buf_add(Buf* b, const void* data, size_t n)
{
    buf_need(b, n);
    if (b->bad) return;
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_str(Buf* b, const char* s) { if (s) buf_add(b, s, strlen(s)); }

static void buf_fmt(Buf* b, const char* fmt, ...)
{
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(tmp, sizeof(tmp), _TRUNCATE, fmt, ap);
    va_end(ap);
    buf_str(b, tmp);
}

// One header line. FLATTENS the value: a newline here would end the header
// block early and let the rest of the value pose as a FILE entry. The server
// flattens too; both sides, because neither should be the only one.
static void hdr_add(Buf* b, const char* key, const char* val)
{
    if (!val || !*val) return;
    buf_str(b, key); buf_str(b, ": ");
    size_t n = strlen(val);
    if (n > 200) n = 200;
    for (size_t i = 0; i < n; i++) {
        char c = val[i];
        buf_add(b, (c == '\n' || c == '\r' || c == 0) ? " " : &c, 1);
    }
    buf_str(b, "\n");
}

static void file_add(Buf* b, const char* name, const void* body, unsigned long len)
{
    if (!name || !*name) return;
    buf_fmt(b, "==== FILE %s %lu ====\n", name, len);
    if (len) buf_add(b, body, len);
    buf_str(b, "\n");
}

// --- the metadata a bundle carries ------------------------------------------
//
// Cheap and local. A value we cannot determine is OMITTED rather than guessed
// (`hdr_add` drops empties): a wrong `title` sends the reader to the wrong game.

static void meta_host(char* out, size_t cch)
{
    wchar_t w[64] = L""; DWORD n = 64;
    if (!GetComputerNameW(w, &n)) { out[0] = 0; return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cch, NULL, NULL);
}

// Which title is running, from the profile table rather than a second list.
// No title loaded (the Viewer on its own) leaves this empty.
static void meta_title(char* out, size_t cch)
{
    out[0] = 0;
    for (int i = 0; ; i++) {
        const TitleProfile* p = profiles_at(i);
        if (!p) break;
        if (p->module && GetModuleHandleA(p->module)) {
            _snprintf_s(out, cch, _TRUNCATE, "%s", p->title ? p->title : p->module);
            return;
        }
    }
}

static void meta_os(char* out, size_t cch)
{
    // GetVersionEx lies to an unmanifested process; the registry build is honest.
    char prod[128] = "Windows", build[32] = "";
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD cb = sizeof(prod), type = 0;
        RegQueryValueExA(k, "ProductName", NULL, &type, (LPBYTE)prod, &cb);
        cb = sizeof(build);
        RegQueryValueExA(k, "CurrentBuild", NULL, &type, (LPBYTE)build, &cb);
        RegCloseKey(k);
    }
    _snprintf_s(out, cch, _TRUNCATE, "%s%s%s", prod, build[0] ? " build " : "", build);
}

static void meta_display(char* out, size_t cch)
{
    _snprintf_s(out, cch, _TRUNCATE, "%dx%d", GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN));
}

// THE CORRELATION KEY. The server cannot tell players apart by address (they
// can share one behind a relay or a NAT), but it logs `session u<16 hex>` at
// every login and `bound to session ... (member=N)` in the lobbies. This is
// that same id, derived from this launch's login token by poltoken.cpp -- a
// one-way hash the server already holds, not the token itself.
static void meta_session(char* out, size_t cch)
{
    out[0] = 0;
    char hex[17] = "";
    if (poltoken_session_hex(hex) && hex[0])
        _snprintf_s(out, cch, _TRUNCATE, "u%s", hex);
}

char* polreport_build(const char* desc, const char* category,
                      const char* const* extra_names,
                      const char* const* extra_bodies,
                      const unsigned long* extra_lens, int extra_count,
                      unsigned long* out_len)
{
    Buf b = { NULL, 0, 0, false };
    buf_str(&b, "==== POLSHIM-REPORT 1 ====\n");

    char host[128] = "", title[128] = "", os[192] = "", disp[64] = "", session[32] = "";
    meta_host(host, sizeof(host));
    meta_title(title, sizeof(title));
    meta_os(os, sizeof(os));
    meta_display(disp, sizeof(disp));
    meta_session(session, sizeof(session));

    char when[64];
    SYSTEMTIME st; GetSystemTime(&st);
    _snprintf_s(when, sizeof(when), _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char build[64];
    _snprintf_s(build, sizeof(build), _TRUNCATE, "CrystalMod %s b%d",
                POLSHIM_VERSION, POLSHIM_BUILD);

    char pid[32];
    _snprintf_s(pid, sizeof(pid), _TRUNCATE, "%lu", (unsigned long)GetCurrentProcessId());

    hdr_add(&b, "host", host);
    hdr_add(&b, "pid", pid);
    hdr_add(&b, "session", session);
    hdr_add(&b, "title", title);
    hdr_add(&b, "shim_build", build);
    hdr_add(&b, "os", os);
    hdr_add(&b, "display", disp);
    hdr_add(&b, "category", category);
    // OUR clock, recorded but not trusted: the server cuts its window from its
    // own receipt time and files this beside it, so a skewed client is visible.
    hdr_add(&b, "when", when);
    buf_str(&b, "\n");

    file_add(&b, "description.txt", desc ? desc : "",
             (unsigned long)(desc ? strlen(desc) : 0));
    for (int i = 0; i < extra_count; i++) {
        if (!extra_names || !extra_names[i] || !extra_lens) continue;
        file_add(&b, extra_names[i], extra_bodies[i], extra_lens[i]);
    }

    if (b.bad) { free(b.p); if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = (unsigned long)b.len;
    return b.p;
}

// ---------------------------------------------------------------------------
// the screenshot
// ---------------------------------------------------------------------------
//
// The game window as the player sees it: a BitBlt of the SCREEN rectangle, not
// PrintWindow, because a Direct3D swapchain usually comes back black through
// PrintWindow -- and a black-screen report whose picture is black for a
// DIFFERENT reason would be worse than no picture. The PNG writer uses stored
// (uncompressed) deflate blocks so the shim links no image library.

static void be32(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static unsigned long crc_table[256];
static bool crc_ready = false;

static unsigned long png_crc(const unsigned char* type, const unsigned char* d, unsigned long n)
{
    if (!crc_ready) {
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = true;
    }
    unsigned long c = 0xFFFFFFFFUL;
    for (int i = 0; i < 4; i++) c = crc_table[(c ^ type[i]) & 0xFF] ^ (c >> 8);
    for (unsigned long i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static bool png_chunk(HANDLE f, const char* type, const unsigned char* d, unsigned long n)
{
    unsigned char hdr[8], tail[4];
    be32(hdr, n); memcpy(hdr + 4, type, 4);
    be32(tail, png_crc((const unsigned char*)type, d, n));
    DWORD wr;
    if (!WriteFile(f, hdr, 8, &wr, NULL) || wr != 8) return false;
    if (n && (!WriteFile(f, d, n, &wr, NULL) || wr != n)) return false;
    return WriteFile(f, tail, 4, &wr, NULL) && wr == 4;
}

// rgb: tightly packed width*height*3 bytes, top row first.
static bool write_png_rgb(const wchar_t* path, int w, int h, const unsigned char* rgb)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;

    DWORD wr;
    const unsigned char sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    WriteFile(f, sig, 8, &wr, NULL);

    unsigned char ihdr[13];
    be32(ihdr, (unsigned long)w); be32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // 8-bit RGB
    if (!png_chunk(f, "IHDR", ihdr, 13)) { CloseHandle(f); return false; }

    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    unsigned char* raw = (unsigned char*)malloc(raw_len);
    if (!raw) { CloseHandle(f); return false; }
    for (int y = 0; y < h; y++) {
        unsigned char* row = raw + (size_t)y * (1 + (size_t)w * 3);
        row[0] = 0;                                    // filter: none
        memcpy(row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }

    // zlib stream: header, stored blocks (<= 65535 each), adler32 trailer.
    size_t nblocks = raw_len / 65535 + 1;
    unsigned char* z = (unsigned char*)malloc(raw_len + nblocks * 5 + 8);
    if (!z) { free(raw); CloseHandle(f); return false; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    size_t off = 0;
    for (;;) {
        size_t block = raw_len - off; if (block > 65535) block = 65535;
        int last = (off + block >= raw_len) ? 1 : 0;
        z[zi++] = (unsigned char)last;                 // BFINAL, BTYPE=00 (stored)
        z[zi++] = (unsigned char)(block & 0xFF);
        z[zi++] = (unsigned char)((block >> 8) & 0xFF);
        z[zi++] = (unsigned char)(~block & 0xFF);
        z[zi++] = (unsigned char)((~block >> 8) & 0xFF);
        if (block) { memcpy(z + zi, raw + off, block); zi += block; }
        off += block;
        if (last) break;
    }
    unsigned long a = 1, bb = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; bb = (bb + a) % 65521; }
    be32(z + zi, (bb << 16) | a); zi += 4;

    bool ok = png_chunk(f, "IDAT", z, (unsigned long)zi);
    ok = ok && png_chunk(f, "IEND", NULL, 0);
    free(z); free(raw);
    CloseHandle(f);
    return ok;
}

struct WinPick { HWND best; long best_score; };

static BOOL CALLBACK pick_proc(HWND h, LPARAM lp)
{
    WinPick* wp = (WinPick*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;            // top-level only
    char cls[64] = ""; GetClassNameA(h, cls, sizeof(cls));
    if (StrStrIA(cls, "PlayOnlineMask")) return TRUE;   // the Viewer's black cover
    RECT r; if (!GetWindowRect(h, &r)) return TRUE;
    long area = (long)(r.right - r.left) * (r.bottom - r.top);
    if (area <= 0) return TRUE;
    // The foreground window wins outright: it is what the player is looking at.
    long score = area + (h == GetForegroundWindow() ? (1L << 29) : 0);
    if (score > wp->best_score) { wp->best_score = score; wp->best = h; }
    return TRUE;
}

static bool capture_png(const wchar_t* path, int* out_w, int* out_h)
{
    WinPick wp = { NULL, 0 };
    EnumWindows(pick_proc, (LPARAM)&wp);
    HWND h = wp.best;
    if (!h || !path) return false;
    RECT rc; GetWindowRect(h, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0 || W > 16384 || H > 16384) return false;

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    bool ok = false;
    if (dib) {
        HGDIOBJ old = SelectObject(mem, dib);
        bool drew = BitBlt(mem, 0, 0, W, H, screen, rc.left, rc.top, SRCCOPY) != 0;
        if (drew && bits) {
            unsigned char* rgb = (unsigned char*)malloc((size_t)W * H * 3);
            if (rgb) {
                const unsigned char* src = (const unsigned char*)bits;   // BGRA
                for (int i = 0; i < W * H; i++) {
                    rgb[i*3+0] = src[i*4+2]; rgb[i*3+1] = src[i*4+1]; rgb[i*3+2] = src[i*4+0];
                }
                ok = write_png_rgb(path, W, H, rgb);
                free(rgb);
            }
        }
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    if (ok) { if (out_w) *out_w = W; if (out_h) *out_h = H; }
    return ok;
}

// ---------------------------------------------------------------------------
// gathering -- runs at the KEYPRESS, before the dialog
// ---------------------------------------------------------------------------

#define RP_MAX_FILES 6

struct Gathered {
    const char*   names[RP_MAX_FILES];
    char*         bodies[RP_MAX_FILES];
    unsigned long lens[RP_MAX_FILES];
    int           n;
    bool          have_shot, have_diag;
    wchar_t       shotpath[MAX_PATH];   // deleted after the send
};

static void gather_free(Gathered* g)
{
    for (int i = 0; i < g->n; i++) free(g->bodies[i]);
    g->n = 0;
    if (g->shotpath[0]) { DeleteFileW(g->shotpath); g->shotpath[0] = 0; }
}

static void gather_add(Gathered* g, const char* name, char* body, unsigned long len)
{
    if (!body) return;
    if (g->n >= RP_MAX_FILES) { free(body); return; }
    g->names[g->n] = name; g->bodies[g->n] = body; g->lens[g->n] = len;
    g->n++;
}

// Read a whole file into a malloc'd buffer, capped.
static char* slurp(const wchar_t* path, DWORD cap, DWORD* out_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)cap) {
        CloseHandle(f); return NULL;
    }
    DWORD want = (DWORD)sz.QuadPart;
    char* buf = (char*)malloc(want + 1);
    if (!buf) { CloseHandle(f); return NULL; }
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf, want, &got, NULL);
    CloseHandle(f);
    if (!ok) { free(buf); return NULL; }
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

static void gather(Gathered* g)
{
    memset(g, 0, sizeof(*g));

    // THE SCREENSHOT FIRST: it is the one thing that changes by the frame, and
    // the diagnostics below spend a second counting frames.
    if (g_shot) {
        wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
        wcscat_s(dir, L"polshim"); CreateDirectoryW(dir, NULL);
        swprintf_s(g->shotpath, L"%s\\report-%lu.png", dir,
                   (unsigned long)GetCurrentProcessId());
        int w = 0, h = 0;
        if (capture_png(g->shotpath, &w, &h)) {
            DWORD slen = 0;
            char* png = slurp(g->shotpath, g_max_shot, &slen);
            if (png) {
                gather_add(g, "shot.png", png, slen);
                g->have_shot = true;
                logf("[report] screenshot %dx%d (%lu bytes)", w, h, (unsigned long)slen);
            } else {
                logf("[report] screenshot dropped -- over max_shot_bytes (%lu) "
                     "or unreadable", (unsigned long)g_max_shot);
            }
        } else {
            logf("[report] screenshot failed -- sending the report without one");
        }
    }

    // THE DIAGNOSTICS, before the log snapshot so the log carries the line
    // saying they were taken.
    if (g_diag) {
        DWORD dlen = 0;
        char* d = sysdiag_collect(&dlen);
        if (d) { gather_add(g, "diag.txt", d, dlen); g->have_diag = true; }
        logf("[report] diagnostics collected (%lu bytes)", (unsigned long)dlen);
    }

    // Flush, so the snapshot contains the lines describing this moment.
    log_flush();

    DWORD len = 0;
    char* mine = logship_snapshot_redacted(logship_logpath(), g_max_log, &len);
    if (mine) gather_add(g, "polshim.log", mine, len);

    // The PREVIOUS session's log, only when it ended in a crash: then it is
    // often the whole story, and "it happened again" is about that session.
    if (g_include_prev) {
        char why[256] = "";
        if (crashlog_prev_crash(g_ini, why, sizeof(why))) {
            wchar_t prev[MAX_PATH] = L"";
            if (logship_prev_logpath(prev, _countof(prev))) {
                DWORD plen = 0;
                char* pb = logship_snapshot_redacted(prev, g_max_log, &plen);
                if (pb) gather_add(g, "polshim-prev.log", pb, plen);
            }
            logf("[report] previous session crashed (%s) -- attaching its log", why);
        }
    }

    // The ini, REDACTED. Most "it does not work for me" is a setting.
    if (g_include_ini) {
        DWORD ilen = 0;
        char* ini = slurp(g_ini, 512 * 1024, &ilen);
        if (ini) {
            logship_redact_inplace(ini, ilen);
            gather_add(g, "polshim.ini", ini, ilen);
        }
    }
}

// ---------------------------------------------------------------------------
// the send
// ---------------------------------------------------------------------------
static bool send_bundle(const char* desc, const char* category, Gathered* g,
                        char* reply, size_t reply_cch, bool* no_server)
{
    *no_server = false;
    unsigned long blen = 0;
    char* body = polreport_build(desc, category, g->names,
                                 (const char* const*)g->bodies, g->lens, g->n,
                                 &blen);
    if (!body) { logf("[report] could not build the bundle (out of memory)"); return false; }

    char url[512];
    if (!logship_endpoint("/_shim/report", url, sizeof(url))) {
        logf("[report] no server configured ([redirect] server is blank) -- cannot send");
        *no_server = true;
        free(body);
        return false;
    }
    // shim_http_post, not a fire-and-forget POST: this endpoint ANSWERS with the
    // report id the player reads off the screen.
    DWORD status = 0;
    bool sent = shim_http_post(url, body, blen, "application/octet-stream", NULL,
                               reply, (DWORD)reply_cch, &status);
    logf("[report] POST %s -> %lu (%lu bytes, %d file(s))", url,
         (unsigned long)status, blen, g->n);
    free(body);
    if (!sent) return false;
    return status == 200;
}

// ---------------------------------------------------------------------------
// the dialog
// ---------------------------------------------------------------------------
//
// Its own window class: somebody describing a bug writes three sentences, so
// the box is multi-line. Enter inserts a newline; Ctrl+Enter or the button sends.

#define IDC_RP_TEXT  100
#define IDC_RP_CAT   101

static wchar_t g_rp_text[4096];
static wchar_t g_rp_cat[32];
static bool    g_rp_ok = false;

static const wchar_t* kCategories[] = {
    L"bug", L"black screen", L"crash", L"graphics", L"sound", L"input",
    L"network", L"text", L"other"
};

static LRESULT CALLBACK rpproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetWindowTextW(GetDlgItem(h, IDC_RP_TEXT), g_rp_text, _countof(g_rp_text));
            HWND cb = GetDlgItem(h, IDC_RP_CAT);
            int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= (int)(sizeof(kCategories)/sizeof(kCategories[0])))
                sel = 0;
            wcscpy_s(g_rp_cat, kCategories[sel]);
            g_rp_ok = true; DestroyWindow(h); return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { g_rp_ok = false; DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: g_rp_ok = false; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK set_font(HWND c, LPARAM p)
{
    SendMessageW(c, WM_SETFONT, (WPARAM)p, TRUE);
    return TRUE;
}

static bool ask(const Gathered* g)
{
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!reg) {
        WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = rpproc; wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"CrystalModReport";
        if (!RegisterClassExW(&wc)) return false;
        reg = true;
    }
    g_rp_ok = false; g_rp_text[0] = 0; g_rp_cat[0] = 0;

    RECT r = { 0, 0, 440, 320 };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"CrystalModReport", L"Report a problem",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, inst, NULL);
    if (!h) return false;

    CreateWindowExW(0, L"STATIC",
        L"What went wrong? What were you doing when it happened?",
        WS_CHILD | WS_VISIBLE, 12, 10, 416, 18, h, NULL, inst, NULL);
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL,
        12, 32, 416, 130, h, (HMENU)IDC_RP_TEXT, inst, NULL);

    CreateWindowExW(0, L"STATIC", L"Kind:", WS_CHILD | WS_VISIBLE,
                    12, 176, 40, 18, h, NULL, inst, NULL);
    HWND cb = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        56, 172, 150, 220, h, (HMENU)IDC_RP_CAT, inst, NULL);
    for (int i = 0; i < (int)(sizeof(kCategories)/sizeof(kCategories[0])); i++)
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kCategories[i]);
    SendMessageW(cb, CB_SETCURSEL, 0, 0);

    // SAY WHAT IS ABOUT TO LEAVE THE MACHINE, in the player's terms and BEFORE
    // they press Send. Built from what the gather actually collected.
    int nlogs = 0;
    for (int i = 0; i < g->n; i++)
        if (strcmp(g->names[i], "shot.png") && strcmp(g->names[i], "diag.txt")) nlogs++;
    wchar_t note[400];
    _snwprintf_s(note, _countof(note), _TRUNCATE,
        L"Sends to your game server: this description, your computer's name, "
        L"%d log/settings file%s%s%s.\n"
        L"Passwords and login tokens are removed first. Nothing is sent if you cancel.",
        nlogs, nlogs == 1 ? L"" : L"s",
        g->have_diag ? L", system and graphics details (GPU, driver, displays)" : L"",
        g->have_shot ? L", and a picture of the game window" : L"");
    CreateWindowExW(0, L"STATIC", note, WS_CHILD | WS_VISIBLE,
                    12, 204, 416, 64, h, NULL, inst, NULL);

    CreateWindowExW(0, L"BUTTON", L"Send report",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        232, 278, 100, 28, h, (HMENU)IDOK, inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        340, 278, 88, 28, h, (HMENU)IDCANCEL, inst, NULL);

    EnumChildWindows(h, set_font, (LPARAM)GetStockObject(DEFAULT_GUI_FONT));

    ShowWindow(h, SW_SHOW); SetForegroundWindow(h); SetFocus(ed);

    MSG m;
    while (IsWindow(h) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            g_rp_ok = false; DestroyWindow(h); break;
        }
        // Ctrl+Enter sends. A bare Enter stays in the edit box, or the player
        // could not start a second paragraph.
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, WM_COMMAND, IDOK, 0);
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && GetFocus() == ed) {
            TranslateMessage(&m); DispatchMessageW(&m);
            continue;
        }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return g_rp_ok;
}

// ---------------------------------------------------------------------------
// the key
// ---------------------------------------------------------------------------
void polreport_open()
{
    if (!g_enable) return;
    if (InterlockedCompareExchange(&g_open, 1, 0) != 0) return;   // already up

    logf("[report] report key pressed -- gathering before the box opens");
    Gathered g;
    gather(&g);

    // Laid out in 96-DPI constants and opened from the watcher thread, so it
    // needs the same HiDPI fit the settings dialog uses.
    HANDLE dpi_prev = ui_dpi_fit_begin();
    bool send_it = ask(&g);
    ui_dpi_fit_end(dpi_prev);

    if (send_it) {
        char desc[8192] = "";
        WideCharToMultiByte(CP_UTF8, 0, g_rp_text, -1, desc, sizeof(desc) - 1, NULL, NULL);
        char cat[32] = "bug";
        WideCharToMultiByte(CP_UTF8, 0, g_rp_cat, -1, cat, sizeof(cat) - 1, NULL, NULL);

        char reply[128] = "";
        bool no_server = false;
        bool ok = send_bundle(desc, cat, &g, reply, sizeof(reply), &no_server);
        if (ok) {
            wchar_t wid[128] = L"";
            MultiByteToWideChar(CP_UTF8, 0, reply, -1, wid, _countof(wid));
            wchar_t msg[512];
            // THE ID IS THE POINT OF THIS BOX: the player can paste it into
            // chat, and the operator can open exactly that report.
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                L"Thanks, your report was sent.\n\n%s%s\n\n"
                L"If you tell the server's admins about it, include this id.",
                wid[0] ? L"Report id:  " : L"", wid[0] ? wid : L"");
            MessageBoxW(NULL, msg, L"Report a problem", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(NULL, no_server
                ? L"The report could not be sent: no game server is set up in "
                  L"CrystalMod, so there is nowhere to send it."
                : L"The report could not be sent: the server did not accept it.\n\n"
                  L"Nothing was lost. Your log is still on this computer, in the "
                  L"PlayOnline folder (polshim.*.log).",
                L"Report a problem", MB_OK | MB_ICONWARNING);
        }
    } else {
        logf("[report] cancelled by the player; nothing sent");
    }

    gather_free(&g);
    InterlockedExchange(&g_open, 0);
}
