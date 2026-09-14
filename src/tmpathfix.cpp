// tmpathfix.cpp -- fix Tetra Master's gW resource-index path parser so it works
// under Wine when the install lives on a dotted path (e.g. a Steam library under
// ~/.local/share/Steam on the Steam Deck).
//
// THE BUG (confirmed live 2026-08-20). TM builds its
// gW resource index keyed by fileNumber*10000 + sub, and recovers fileNumber from
// the digits BEFORE THE FIRST '.' in the FULL path handed to the index parser
// (0x510E860). On Windows the first dot is the extension (`...\gW201.dat` -> 201).
// On the Deck the path is `Z:\home\deck\.local\share\...\gW201.dat`, whose first
// dot is in `.local`, so atoi yields 0, every gW file's keys collapse onto the
// bare sub, `find(2011000)` misses, and the loader retries the load forever ->
// black screen / missing images. Measured directly by [tmdiag] in d3d8hook.cpp:
// basedir dotted, index = 27 keys {90..1023}, max < 10000.
//
// THE FIX. Rewrite the first-dot scan to a LAST-dot scan -- SE's obvious intent
// (the extension), which is correct on every host because the LAST dot of a data
// file path is always its `.dat`. Purely in-process; touches no registry and so
// cannot trip POL's running-vs-registered write guard (which is why the registry
// routes were all dead ends).
//
// THE SITE. TM.dll RVA 0x17E899, a 42-byte region (up to but not including
// 0x17E8C3) that computes edx = index of the first '.' (or the string length if
// none), immediately consumed by `lea ecx,[edx+esi-3]` at 0x17E8C3. Byte pattern
// verified UNIQUE and identical across all four dumped load bases (no absolute
// addresses), so an RVA-keyed byte patch is valid on the Wine unpack. Disasm:
//
//   17E899 8bfe        mov edi,esi          ; strlen(esi) into ecx, then...
//   17E89B 83c9ff      or  ecx,-1
//   17E89E 33c0        xor eax,eax
//   17E8A0 83c410      add esp,0x10         ; <-- stack fixup for the prior call; KEEP
//   17E8A3 33d2        xor edx,edx
//   17E8A5 f2ae        repne scasb
//   17E8A7 f7d1        not ecx
//   17E8A9 49          dec ecx
//   17E8AA 7417        je  0x17E8C3         ; len==0
//   17E8AC 803c322e    cmp byte [edx+esi],'.'
//   17E8B0 7411        je  0x17E8C3         ; FIRST dot -> stop (the bug)
//   17E8B2 8bfe        mov edi,esi          ; recompute strlen each iteration...
//   17E8B4 83c9ff      or  ecx,-1
//   17E8B7 33c0        xor eax,eax
//   17E8B9 42          inc edx
//   17E8BA f2ae        repne scasb
//   17E8BC f7d1        not ecx
//   17E8BE 49          dec ecx
//   17E8BF 3bd1        cmp edx,ecx
//   17E8C1 72e9        jb  0x17E8AC
//   17E8C3 33c0        xor eax,eax          ; <-- resume here; edx must be the dot index
//   17E8C5 8d4c32fd    lea ecx,[edx+esi-3]  ; the 3 chars before the dot
//
// THE REPLACEMENT (33 bytes + 9 NOP pad = the full 42). esi/ebx untouched;
// eax/ecx/edx/edi are dead across the region (0x17E8C3 rewrites eax and edx). It
// keeps the `add esp,0x10` fixup, scans once recording the LAST '.' index in ecx
// (sentinel -1 = none), and leaves edx = last-dot index, or the length when there
// is no dot -- byte-identical fallback to the original's no-dot behaviour, and a
// strict no-op wherever a path already parsed correctly (Windows).
//
//   add esp,0x10        83 c4 10
//   mov edi,esi         8b fe
//   xor edx,edx         33 d2          ; edx = running index
//   or  ecx,-1          83 c9 ff       ; ecx = last-dot index, sentinel -1
// scan:                                  (P+0x0A)
//   mov al,[edi]        8a 07
//   test al,al          84 c0
//   je  done           74 0a          ; -> P+0x1A
//   cmp al,0x2e         3c 2e
//   jne notdot          75 02          ; -> P+0x16
//   mov ecx,edx         8b ca          ; record last-dot index
// notdot:                                (P+0x16)
//   inc edi             47
//   inc edx             42             ; edx = index of next char (== len at NUL)
//   jmp scan            eb f0          ; -> P+0x0A
// done:                                  (P+0x1A)
//   cmp ecx,-1          83 f9 ff
//   je  nofix           74 02          ; no dot -> keep edx = len
//   mov edx,ecx         8b d1          ; edx = last-dot index
// nofix:                                 (P+0x21) then NOP*9 to 0x17E8C3
//
// TIMING. TM.dll is ASProtect(POL1)-packed: these bytes do not exist on disk and
// appear only after the stub unpacks .text. Like tmlog/patches, the patch is
// applied from the injector's hot path (every socket / OutputDebugString) and
// byte-verifies before writing, so it lands in the same post-unpack window. The
// patch MUST land before the resource glob (0x510E760) runs, or the map is built
// wrong and stays wrong; [tmdiag]'s max-key line is the observable that says which
// happened (>= 2011000 == the patch won the race).

#include "polshim.h"

static int   g_on = 0;
static void* g_base = NULL;
static int   g_applied = 0;
static int   g_said_unreadable = 0;
static int   g_said_mismatch = 0;

// RVA of the first byte we overwrite, and the region length. Both ini-tunable so a
// TM.dll build with a shifted site can be retargeted without a rebuild.
static unsigned g_rva = 0x17E899;
static unsigned g_len = 42;

// The 42 original bytes we expect at the site (the byte-verify gate) and our 42
// replacement bytes. Kept as file-scope arrays so the sizes are checked at compile.
static const unsigned char ORIG[42] = {
    0x8b,0xfe, 0x83,0xc9,0xff, 0x33,0xc0, 0x83,0xc4,0x10, 0x33,0xd2, 0xf2,0xae,
    0xf7,0xd1, 0x49, 0x74,0x17, 0x80,0x3c,0x32,0x2e, 0x74,0x11, 0x8b,0xfe,
    0x83,0xc9,0xff, 0x33,0xc0, 0x42, 0xf2,0xae, 0xf7,0xd1, 0x49, 0x3b,0xd1, 0x72,0xe9,
};
static const unsigned char REPL[42] = {
    0x83,0xc4,0x10,             // add esp,0x10
    0x8b,0xfe,                  // mov edi,esi
    0x33,0xd2,                  // xor edx,edx
    0x83,0xc9,0xff,             // or  ecx,-1
    // scan: (offset 0x0A)
    0x8a,0x07,                  // mov al,[edi]
    0x84,0xc0,                  // test al,al
    0x74,0x0a,                  // je  done (0x1A)
    0x3c,0x2e,                  // cmp al,'.'
    0x75,0x02,                  // jne notdot (0x16)
    0x8b,0xca,                  // mov ecx,edx
    // notdot: (0x16)
    0x47,                       // inc edi
    0x42,                       // inc edx
    0xeb,0xf0,                  // jmp scan (0x0A)
    // done: (0x1A)
    0x83,0xf9,0xff,             // cmp ecx,-1
    0x74,0x02,                  // je  nofix (0x21)
    0x8b,0xd1,                  // mov edx,ecx
    // nofix: (0x21) -- pad to 42
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
};

int  tmpathfix_enabled() { return g_on; }

// ---------------------------------------------------------------------------
// Deterministic timing: patch when TM GLOBS its gW files, not on a periodic poll.
//
// The periodic hot-path arm loses a race under Proton: ASProtect unpacks .text
// per-page on first execution, and TM's resource glob (0x510E760) + parser
// (0x510E860) + our site (0x510E899) all live in ONE 0x1000 page. TM runs the
// whole glob-and-build in a burst the instant that page unpacks, and the next
// socket/OutputDebugString arm fires only AFTER the (now-wrong) index is built --
// which is lossy (colliding subs overwrite), so a later patch cannot repair it.
// Measured live 2026-08-20: "[tmpathfix] ... fixed" but "[tmdiag] ... COLLAPSED".
//
// The glob calls _findfirst("<LOADDIR>data/gW*.dat") -> FindFirstFileA. At THAT
// call the glob is executing, so its whole page -- including our site -- is
// already unpacked, and the per-file parser has not run yet. So: EAT-hook
// kernel32!FindFirstFileA, and when the search pattern is TM's gW glob, apply the
// byte patch BEFORE chaining to the real FindFirstFileA. That lands the fix in the
// one window that is both unpacked and pre-index-build. Gated tightly on the
// pattern, so the shim's own file enumeration (which uses the W form anyway) and
// every other FindFirstFileA are untouched.
typedef HANDLE (WINAPI *PFN_FFFA)(LPCSTR, LPVOID);
static PFN_FFFA real_FindFirstFileA = NULL;
static int      g_eat_done = 0;

static int is_gw_glob(const char* p)
{
    if (!p) return 0;
    // match "...gW*.dat" case-insensitively, the glob TM builds for its resources.
    size_t n = strlen(p);
    if (n < 6) return 0;
    const char* dot = NULL;
    for (const char* q = p + n - 1; q >= p; --q) if (*q == '.') { dot = q; break; }
    if (!dot || (dot[1] != 'd' && dot[1] != 'D') ||
        (dot[2] != 'a' && dot[2] != 'A') || (dot[3] != 't' && dot[3] != 'T')) return 0;
    // require a 'gW' (any case) and a '*' before the dot -- TM's pattern is gW*.dat
    int seen_g = 0, seen_star = 0;
    for (const char* q = p; q < dot; ++q) {
        if ((q[0] == 'g' || q[0] == 'G') && (q[1] == 'w' || q[1] == 'W')) seen_g = 1;
        if (*q == '*') seen_star = 1;
    }
    return seen_g && seen_star;
}

static HANDLE WINAPI hook_FindFirstFileA(LPCSTR name, LPVOID data)
{
    if (g_on && is_gw_glob(name)) {
        HMODULE tm = GetModuleHandleA("TM.dll");
        if (tm) {
            logf("[tmpathfix] gW glob detected (%s) -- patching the parser NOW, "
                 "before the index is built", name);
            tmpathfix_apply_module((void*)tm, "TM.dll");
        }
    }
    return real_FindFirstFileA ? real_FindFirstFileA(name, data)
                               : FindFirstFileA(name, (LPWIN32_FIND_DATAA)data);
}

void tmpathfix_eat_patch()
{
    if (!g_on || g_eat_done) return;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    if (!real_FindFirstFileA)
        real_FindFirstFileA = (PFN_FFFA)GetProcAddress(k32, "FindFirstFileA");
    if (dx_eat_patch_export(k32, "FindFirstFileA", (void*)hook_FindFirstFileA,
                            "tmpathfix!FindFirstFileA"))
        g_eat_done = 1;
}

void tmpathfix_init(const wchar_t* ini)
{
    // Default ON: this is a strict no-op on any host where the path already parses
    // (the byte-verify still gates every write), and it is the whole point of the
    // Deck effort, so it should reach the Deck without an ini delivery. [dx] is the
    // section the rest of the TM/Deck knobs live in.
    g_on  = GetPrivateProfileIntW(L"dx", L"tm_pathfix",     1,        ini);
    g_rva = GetPrivateProfileIntW(L"dx", L"tm_pathfix_rva", 0x17E899, ini);
    g_len = GetPrivateProfileIntW(L"dx", L"tm_pathfix_len", 42,       ini);
    if (g_len > sizeof(REPL)) g_len = sizeof(REPL);
}

// Re-arm bookkeeping for a fresh TM.dll load (relaunch at a new base, or the same
// base with freshly mapped .text). Mirrors tmlog_rearm's discipline.
static void tmpathfix_rearm(const char* why)
{
    logf("[tmpathfix] re-arming: %s", why);
    g_applied = 0;
    g_said_unreadable = 0;
    g_said_mismatch = 0;
    g_base = NULL;
}

void tmpathfix_apply_module(void* module_base, const char* module)
{
    if (!g_on || !module_base || !module) return;
    if (_stricmp(module, "TM.dll") != 0) return;

    // A new base, or our bytes gone from the old base, means a fresh image.
    if (g_base && module_base != g_base) {
        tmpathfix_rearm("TM.dll re-loaded at a new base");
    } else if (g_applied) {
        unsigned char* a = (unsigned char*)module_base + g_rva;
        if (!IsBadReadPtr(a, g_len) && memcmp(a, REPL, g_len) != 0)
            tmpathfix_rearm("TM.dll re-loaded at the same base");
    }
    g_base = module_base;
    if (g_applied) return;

    unsigned char* addr = (unsigned char*)module_base + g_rva;
    if (IsBadReadPtr(addr, g_len)) {
        if (!g_said_unreadable++)
            logf("[tmpathfix] TM.dll+0x%05X not readable yet -- will retry", g_rva);
        return;
    }

    // BYTE-VERIFY before writing: only patch the exact site we measured. A packed
    // page (all-zero) or an unexpected build must NOT be written -- refuse loudly
    // once, keep retrying (the unpack may not have happened yet).
    if (memcmp(addr, ORIG, g_len) != 0) {
        if (memcmp(addr, REPL, g_len) == 0) {   // already ours (idempotent re-entry)
            g_applied = 1;
            return;
        }
        if (!g_said_mismatch++) {
            // Distinguish "still packed" from "wrong build" for the log reader.
            int allzero = 1;
            for (unsigned i = 0; i < g_len; i++) if (addr[i]) { allzero = 0; break; }
            logf("[tmpathfix] TM.dll+0x%05X %s -- not patching (byte-verify)",
                 g_rva, allzero ? "still blank (POL1 packed) -- will retry once unpacked"
                                : "does not match the known parser site (other build?)");
        }
        return;
    }

    DWORD old = 0;
    if (!VirtualProtect(addr, g_len, PAGE_EXECUTE_READWRITE, &old)) {
        logf("[tmpathfix] VirtualProtect failed at TM.dll+0x%05X (err %lu)",
             g_rva, GetLastError());
        return;
    }
    memcpy(addr, REPL, g_len);
    DWORD junk = 0;
    VirtualProtect(addr, g_len, old, &junk);
    FlushInstructionCache(GetCurrentProcess(), addr, g_len);
    g_applied = 1;
    logf("[tmpathfix] gW path parser fixed: first-dot -> last-dot scan at "
         "TM.dll+0x%05X (%u bytes). Dotted install paths now parse the gW file "
         "number correctly; watch [tmdiag] for max key >= 2011000.", g_rva, g_len);
    log_flush();
}
