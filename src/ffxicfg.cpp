// ffxicfg.cpp -- read FFXI's OWN settings table, the one the numbered registry
// values mirror, and print the mapping.
//
// WHY THIS EXISTS
//
// FFXI keeps its settings under HKLM\SOFTWARE\PlayOnline*\SquareEnix\
// FinalFantasyXI as values named "0000".."0045". Nothing in this project knows
// what any of them MEAN. gamecfg.cpp lists them; it does not decode them. That
// gap is why [[shim-windowed-by-default]] still carries "FFXI has its own native
// windowed mode ... not done because *which* value has never been measured", and
// why the plan of record was to run FFXI's config app between two registry diffs
// and infer -- an inference that gamecfg.cpp's own header records getting wrong
// twice.
//
// There is no need to infer. The client carries the answer.
//
// FFXI's settings live in memory as an array of configuration "subject" objects,
// and EACH ONE CARRIES ITS OWN REGISTRY NAME. Found 2026-08-26 in Ashita's
// `addons/config/config.lua` (atom0s, GPLv3), which declares the layout:
//
//     struct FsConfigSubject {          // 44 bytes
//         uintptr_t VTable;
//         uint32_t  m_configKey;
//         int32_t   m_configValue;
//         uint32_t  m_configType;
//         char      m_polName[8];       // <-- "0017", the PlayOnline value name
//         int32_t   m_minVal, m_maxVal, m_defVal;
//         uintptr_t m_callbackList;
//         int32_t   m_configProc;
//     };
//
// So one pass over the array prints `id -> registry name`, with the type, the
// live value, and the min/max/default the client itself will clamp to. That is
// the decode, measured from the binary rather than guessed from a diff.
//
// HOW THE TABLE IS FOUND -- and why this reads memory instead of CALLING
//
// Ashita locates three functions by byte pattern. We need only the first:
//
//     get_config_value(int32_t id)      8B0D????????85C974??8B44240450E8????????C383C8FFC3
//
// It opens `mov ecx,[imm32]` -- and that imm32 is the address of the global
// pointer to the configuration object. `this = **(void***)(match + 2)`.
//
// The accessor Ashita calls to reach an entry is
//
//     get_config_entry(this, 0, id)     8B490485C974108B4424048D14808D04508D0481C2040033C0C20400
//
// which disassembles to pure arithmetic -- no calls, no side effects:
//
//     8B 49 04            mov  ecx,[ecx+4]        ; this->m_table
//     85 C9               test ecx,ecx
//     74 10               jz   zero
//     8B 44 24 04         mov  eax,[esp+4]        ; id (arg3; __fastcall)
//     8D 14 80            lea  edx,[eax+eax*4]    ; id*5
//     8D 04 50            lea  eax,[eax+edx*2]    ; id*11
//     8D 04 81            lea  eax,[ecx+eax*4]    ; m_table + id*44
//     C2 04 00            ret  4
//
// i.e. `entry = *(char**)(this + 4) + id * 44`. Because it is pure arithmetic we
// do NOT call it: this module only ever READS memory. A diagnostic that dumps
// settings must not be able to run game code on a game thread at a moment of its
// own choosing, and it must stay safe when the pattern matched something that
// merely looks similar. So the two constants (+4 and *44) are taken from the
// matched bytes and cross-checked against the ones above; a mismatch is logged
// loudly and the dump is abandoned rather than reinterpreted.
//
// This is deliberately READ-ONLY. Ashita's `set_config_value(id, value)` applies
// a setting live, with no restart and no registry write, and that is the shape a
// real per-title native-windowed switch would take
// ([[match-the-original-not-a-stopgap]]). It is not built here, because writing a
// setting whose meaning has never been printed is exactly the guess this file
// exists to retire. Print first; act in a later build, against the printed names.
//
// CONFIG ([ffxi] in polshim.ini, next to the DLL -- see [[shim-two-locations]]):
//   cfgdump=1   dump once per FFXiMain.dll load (default). 0 = off.
//   cfgdump=2   also dump entries the client left blank, for completeness.
//
// The dump is ~207 lines once per FFXI launch, against logs that routinely run to
// millions -- it is on by default because the whole point is that the next launch
// answers the question without anyone having to know this setting exists.

#include "polshim.h"

static int  g_dump = 1;
static LONG g_done = 0;          // one dump per module load, not per call

void ffxicfg_configure(const wchar_t* ini)
{
    g_dump = GetPrivateProfileIntW(L"ffxi", L"cfgdump", 1, ini);
}

// ---------------------------------------------------------------------------
// the layout, exactly as Ashita's config.lua declares it
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct FsConfigSubject {
    unsigned int   vtable;
    unsigned int   key;
    int            value;
    unsigned int   type;
    char           polname[8];   // NOT guaranteed NUL-terminated -- 8 chars max
    int            minval;
    int            maxval;
    int            defval;
    unsigned int   callbacks;
    int            proc;
};
#pragma pack(pop)

// The stride the accessor computes. Asserted against the matched bytes below,
// and against the struct, so a future edit to either cannot drift silently.
#define FCFG_STRIDE   44
#define FCFG_TABLEOFS 4
#define FCFG_MAX_ID   207        // Ashita's own bound: /config rejects id > 207

// ---------------------------------------------------------------------------
// readable-memory test
//
// VirtualQuery rather than a __try around each read, for crashlog.cpp's reason:
// when you are walking a table you only believe you have found, a bad address is
// the ordinary case and not the exception, and a fault per entry inside a
// diagnostic is how a diagnostic becomes the bug.
// ---------------------------------------------------------------------------
static bool fcfg_readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return false;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const unsigned char* end = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
    return (const unsigned char*)p + n <= end;
}

// ---------------------------------------------------------------------------
// byte-pattern search, in Ashita's own notation
//
// A pattern is hex with `??` for "any byte", e.g. "8B0D????????85C9". Kept in
// that notation ON PURPOSE: these strings are copied from config.lua, and a
// reader comparing the two should not have to translate a format to do it.
// ---------------------------------------------------------------------------
#define FCFG_PAT_MAX 64

static int fcfg_pat_parse(const char* pat, unsigned char* bytes, unsigned char* mask)
{
    int n = 0;
    for (const char* p = pat; *p && n < FCFG_PAT_MAX; ) {
        if (*p == ' ') { p++; continue; }
        if (!p[1]) return -1;                       // odd nibble count
        if (p[0] == '?' && p[1] == '?') {
            bytes[n] = 0; mask[n] = 0; n++; p += 2; continue;
        }
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; k++) {
            char c = p[k];
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (v < 0) return -1;
            if (k == 0) hi = v; else lo = v;
        }
        bytes[n] = (unsigned char)((hi << 4) | lo); mask[n] = 0xFF; n++; p += 2;
    }
    return (*pat && n > 0) ? n : -1;
}

static const unsigned char* fcfg_find(const unsigned char* base, size_t size,
                                      const char* pat)
{
    unsigned char b[FCFG_PAT_MAX], m[FCFG_PAT_MAX];
    int n = fcfg_pat_parse(pat, b, m);
    if (n <= 0 || (size_t)n > size) return NULL;

    // Page-aware: a module image has holes (uncommitted or NOACCESS sections),
    // and this walks the whole image, so a blind memcmp sweep would fault. Step
    // region by region and only search committed, readable ones.
    const unsigned char* p   = base;
    const unsigned char* end = base + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return NULL;
        const unsigned char* rstart = (const unsigned char*)mbi.BaseAddress;
        const unsigned char* rend   = rstart + mbi.RegionSize;
        if (rend > end) rend = end;
        bool ok = (mbi.State == MEM_COMMIT) &&
                  !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (ok && rend - p >= n) {
            for (const unsigned char* q = p; q <= rend - n; q++) {
                int i = 0;
                for (; i < n; i++) if (m[i] && q[i] != b[i]) break;
                if (i == n) return q;
            }
        }
        p = rend;
        if (p <= rstart) break;                     // VirtualQuery made no progress
    }
    return NULL;
}

static bool fcfg_module_range(const char* name, const unsigned char** base, size_t* size)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)h, &mbi, sizeof(mbi))) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
    if (!fcfg_readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((unsigned char*)h + dos->e_lfanew);
    if (!fcfg_readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    *base = (const unsigned char*)h;
    *size = nt->OptionalHeader.SizeOfImage;
    return true;
}

// ---------------------------------------------------------------------------
// the patterns, verbatim from Ashita's addons/config/config.lua
// ---------------------------------------------------------------------------
static const char* const PAT_GET   =
    "8B0D????????85C974??8B44240450E8????????C383C8FFC3";
static const char* const PAT_ENTRY =
    "8B490485C974108B4424048D14808D04508D0481C2040033C0C20400";

static const char* fcfg_type_name(unsigned int t)
{
    // Not decoded. Printed as a number so the dump states what it measured and
    // nothing more -- naming these would be the guess this file exists to avoid.
    (void)t;
    return "";
}

// ---------------------------------------------------------------------------
// the dump
// ---------------------------------------------------------------------------
void ffxicfg_dump(const char* why)
{
    if (!g_dump) return;

    const unsigned char* base = NULL;
    size_t size = 0;
    if (!fcfg_module_range("FFXiMain.dll", &base, &size)) {
        logf("[fcfg] FFXiMain.dll is not mapped -- nothing to dump (%s)", why);
        return;
    }

    const unsigned char* mget = fcfg_find(base, size, PAT_GET);
    const unsigned char* ment = fcfg_find(base, size, PAT_ENTRY);
    if (!mget) {
        logf("[fcfg] the get_config_value pattern did NOT match in FFXiMain.dll "
             "(base=%p size=%u). This client build differs from the one Ashita's "
             "config.lua was written for; re-derive the pattern before trusting "
             "anything else here.", base, (unsigned)size);
        return;
    }
    logf("[fcfg] get_config_value at %p (FFXiMain.dll+0x%X)%s",
         mget, (unsigned)(mget - base),
         ment ? "" : "  -- NOTE: the get_config_entry pattern did NOT match, so "
                     "the stride below is assumed, not confirmed");

    // The stride and table offset, CONFIRMED from the accessor's own bytes rather
    // than trusted. `mov ecx,[ecx+NN]` is byte 2 of the match; the *44 is the
    // lea chain. If either disagrees with what this file was written against,
    // every offset below would be wrong, so say so and stop.
    if (ment) {
        unsigned tblofs = ment[2];
        if (tblofs != FCFG_TABLEOFS) {
            logf("[fcfg] ABANDONED: get_config_entry reads the table from "
                 "this+%u, not this+%u. The layout this file assumes does not "
                 "match this client -- re-read the function before dumping.",
                 tblofs, (unsigned)FCFG_TABLEOFS);
            return;
        }
        logf("[fcfg] get_config_entry at %p (FFXiMain.dll+0x%X) -- confirms "
             "table at this+%u, entry stride %u bytes",
             ment, (unsigned)(ment - base), tblofs, (unsigned)FCFG_STRIDE);
    }

    // `mov ecx,[imm32]` -- imm32 sits at match+2 and holds the ADDRESS OF the
    // global pointer, so it takes two dereferences to reach the object.
    void** pglobal = *(void***)(mget + 2);
    if (!fcfg_readable(pglobal, sizeof(void*))) {
        logf("[fcfg] the configuration global at %p is not readable -- FFXI has "
             "matched the pattern but not yet built its settings object. A dump "
             "this early is expected to fail; it is retried on the next load.",
             pglobal);
        return;
    }
    unsigned char* self = (unsigned char*)*pglobal;
    if (!fcfg_readable(self, FCFG_TABLEOFS + sizeof(void*))) {
        logf("[fcfg] configuration object %p not readable yet -- nothing dumped", self);
        return;
    }
    unsigned char* table = *(unsigned char**)(self + FCFG_TABLEOFS);
    if (!fcfg_readable(table, FCFG_STRIDE)) {
        logf("[fcfg] configuration table %p not readable yet -- nothing dumped", table);
        return;
    }

    logf("[fcfg] ---- FFXI configuration table (%s) ----", why);
    logf("[fcfg] object=%p table=%p. `pol` is the value's own name under "
         "HKLM\\SOFTWARE\\PlayOnline*\\SquareEnix\\FinalFantasyXI -- this IS the "
         "0000..0045 decode. `val` is live, `def/min/max` are the client's own.",
         self, table);

    int shown = 0, blank = 0;
    for (int id = 1; id <= FCFG_MAX_ID; id++) {
        const FsConfigSubject* e = (const FsConfigSubject*)(table + (size_t)id * FCFG_STRIDE);
        if (!fcfg_readable(e, sizeof(*e))) {
            logf("[fcfg] id=%3d unreadable at %p -- table ends short of id %d",
                 id, e, FCFG_MAX_ID);
            break;
        }
        char pol[9];
        memcpy(pol, e->polname, 8);
        pol[8] = '\0';
        // A blank name is an allocated-but-unused slot. Skipped by default so the
        // dump reads as the mapping it is, not as 207 rows of mostly nothing.
        bool named = (pol[0] != '\0' && (unsigned char)pol[0] != 0xCD);
        if (!named) { blank++; if (g_dump < 2) continue; }
        logf("[fcfg] id=%3d pol=%-8s key=%-5u val=%-11d type=%u%s "
             "min=%-11d max=%-11d def=%-11d flags=0x%02X",
             id, named ? pol : "(none)", e->key, e->value, e->type,
             fcfg_type_name(e->type), e->minval, e->maxval, e->defval,
             (unsigned)e->proc);
        shown++;
    }
    logf("[fcfg] ---- %d named entries, %d blank ----", shown, blank);
    logf("[fcfg] To find a setting: run FFXI's own config app, change ONE thing, "
         "and look for the id whose `val` moved. The `pol` column then names the "
         "registry value to write -- or, better, the id to set live.");
}

// ---------------------------------------------------------------------------
// arming
//
// FFXiMain.dll is MAPPED well before it BUILDS its settings object, so dumping
// straight from the loader callback would reliably print "not readable yet" and
// nothing else. A short bounded poller waits for the object instead -- the same
// shape tmband.cpp uses, and for the same reason: the thing being watched
// appears some time after the module that owns it.
//
// Bounded on purpose. This is a diagnostic; if the object never appears the
// right outcome is one line saying so, not a thread that polls for the life of
// the process.
// ---------------------------------------------------------------------------
static LONG g_running = 0;

#define FCFG_POLL_MS      250
#define FCFG_POLL_TRIES   240        // 60 seconds

static DWORD WINAPI fcfg_thread(LPVOID)
{
    for (int t = 0; t < FCFG_POLL_TRIES; t++) {
        Sleep(FCFG_POLL_MS);
        if (InterlockedCompareExchange(&g_done, 0, 0)) break;

        const unsigned char* base = NULL; size_t size = 0;
        if (!fcfg_module_range("FFXiMain.dll", &base, &size)) continue;
        const unsigned char* mget = fcfg_find(base, size, PAT_GET);
        if (!mget) {
            // A pattern miss will not fix itself by waiting, and the miss is
            // itself the finding (this client build differs from the one the
            // pattern was written against). Report once and stop.
            InterlockedExchange(&g_done, 1);
            ffxicfg_dump("pattern miss");
            break;
        }
        void** pglobal = *(void***)(mget + 2);
        if (!fcfg_readable(pglobal, sizeof(void*)) || !*pglobal) continue;  // not built yet

        InterlockedExchange(&g_done, 1);
        ffxicfg_dump("FFXiMain.dll settings object is up");
        break;
    }
    if (!InterlockedCompareExchange(&g_done, 0, 0))
        logf("[fcfg] gave up after %d seconds -- FFXiMain.dll never presented a "
             "readable settings object. Not necessarily a bug: a title that exits "
             "during its logos never builds one.",
             (FCFG_POLL_MS * FCFG_POLL_TRIES) / 1000);
    InterlockedExchange(&g_running, 0);
    return 0;
}

// Called for every module load and for the startup sweep, beside
// ffxiplug_on_module.
void ffxicfg_on_module(void* base)
{
    if (!g_dump || !base) return;
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA((HMODULE)base, path, MAX_PATH)) return;
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;
    if (_stricmp(leaf, "FFXiMain.dll") != 0) return;

    // A fresh FFXiMain means a fresh table, so this re-arms. One pol.exe runs
    // MANY titles ([[shim-per-title-state-scope]]), and a dump that can only
    // ever happen once per process would miss the second launch -- the exact
    // trap that file records.
    InterlockedExchange(&g_done, 0);
    if (InterlockedExchange(&g_running, 1)) return;      // a poller is already up
    HANDLE h = CreateThread(NULL, 0, fcfg_thread, NULL, 0, NULL);
    if (!h) { InterlockedExchange(&g_running, 0); return; }
    CloseHandle(h);
    logf("[fcfg] FFXiMain.dll mapped -- waiting for its settings object, then "
         "dumping the id -> registry-name map once ([ffxi] cfgdump=0 to stop)");
}

void ffxicfg_summary(void)
{
    if (!g_dump) {
        logf("[fcfg] summary: OFF ([ffxi] cfgdump=0)");
        return;
    }
    logf("[fcfg] summary: %s",
         InterlockedCompareExchange(&g_done, 0, 0)
             ? "the FFXI configuration table was dumped this session"
             : "armed, but no dump ran (FFXI never loaded, or its settings object "
               "never appeared)");
}

// ---------------------------------------------------------------------------
// selftest -- the pattern parser and the layout constants
//
// What CAN be proven without FFXI: that the notation copied from config.lua
// parses to the bytes it denotes, that wildcards match anything and literals do
// not, and that the struct this file declares is the 44 bytes the accessor's
// arithmetic produces. What cannot: that FFXiMain.dll contains either pattern.
// That is a launch, and the log says so in one line either way.
// ---------------------------------------------------------------------------
int ffxicfg_selftest(void)
{
    int fail = 0;
    #define CHK(c, m) do { if (!(c)) { logf("[fcfg] SELFTEST FAIL: %s", m); fail++; } } while (0)

    CHK(sizeof(FsConfigSubject) == FCFG_STRIDE,
        "FsConfigSubject is not 44 bytes -- the accessor's lea chain says it is");

    unsigned char b[FCFG_PAT_MAX], m[FCFG_PAT_MAX];
    int n = fcfg_pat_parse("8B0D????????85C9", b, m);
    CHK(n == 8, "8B0D????????85C9 should parse to 8 bytes");
    CHK(n == 8 && b[0] == 0x8B && m[0] == 0xFF, "literal byte 0 wrong");
    CHK(n == 8 && m[2] == 0 && m[5] == 0, "the four ?? bytes should be wildcards");
    CHK(n == 8 && b[6] == 0x85 && b[7] == 0xC9, "literal tail wrong");
    CHK(fcfg_pat_parse("8B0", b, m) == -1, "an odd nibble count must be rejected");
    CHK(fcfg_pat_parse("zz", b, m) == -1, "a non-hex pattern must be rejected");
    CHK(fcfg_pat_parse("", b, m) == -1, "an empty pattern must be rejected");

    // The wildcard must actually skip a byte, and a literal must actually bind.
    // A pattern engine that matches everything would "find" the table in any
    // module and hand out a garbage pointer, which is the failure that matters.
    static const unsigned char hay[] = { 0x11, 0x8B, 0x0D, 0xAA, 0xBB, 0xCC, 0xDD, 0x85, 0xC9, 0x22 };
    const unsigned char* hit = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(hay, &mbi, sizeof(mbi))) {
        hit = fcfg_find(hay, sizeof(hay), "8B0D????????85C9");
        CHK(hit == hay + 1, "the wildcard pattern should match at offset 1");
        CHK(fcfg_find(hay, sizeof(hay), "8B0D????????85CA") == NULL,
            "a pattern differing in one literal byte must NOT match");
    }

    // The two constants this file asserts against the matched bytes.
    CHK(FCFG_TABLEOFS == 4,  "get_config_entry reads the table from this+4");
    CHK(FCFG_STRIDE   == 44, "get_config_entry strides 44 bytes per entry");

    #undef CHK
    if (!fail) logf("[fcfg] selftest OK");
    return fail;
}
