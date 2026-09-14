// patches.cpp -- persistent byte patches applied to POL modules post-unpack.
//
// app.dll ships ASProtect-packed (POL1 section), so these bytes do not exist in
// the file on disk -- they only appear once the stub has unpacked .text. That is
// the same constraint probes.cpp lives under, so we hang off the same hook: each
// module's first DllGetClassObject, which is post-unpack and pre-login.
//
// WHAT THIS FIXES -- (1) the pre-login "Check Files" list (sites A/B below), and
// (2) the native sign-up wizard's hardcoded https: scheme (site C), so in-client
// account registration works over plain HTTP with no certificate. Site C is the
// in-memory equivalent of the on-disk sign-up scheme patch; both are idempotent,
// so having one does not conflict with the other.
//
// The enumerator walks the content table from data/doc/sqpolcts.bin (ids 1,2,3,4,
// 5,6,7,10,11,12,13,14,15,999,1000..1005) and marks every candidate "rejected" via
// a Product\%04d registry check that fails for essentially everyone. app.dll then
// runs a HARDCODED CHAIN of per-id cases, each of which may undo that rejection:
//
//     dec eax; je ...        id 1    FFXI
//     dec eax; je ...        id 2    Tetra Master
//     sub eax,0xc; je ...    id 14   Friend List
//     dec eax; je ...        id 15   FFXI Test Server
//     sub eax,0x3d8; je ...  id 1000 Navigator
//     cmp [ebp+8],4 ...      id 4    Front Mission Online   <- the fall-through case
//
// Fantasy Earth is id 11 and has NO case, so it keeps the rejection and is dropped
// before the client even opens its directory. Site A NOPs the `jne` guarding the
// fall-through case, so every id that reaches it gets the reprieve. CONFIRMED live
// 2026-08-11: Fantasy Earth then appears in Check Files.
//
// Site B is a genuine one-instruction bug in SE's code. Ids with their OWN case
// branch away before reaching site A, so site A cannot help them -- and Friend
// List's case clears only *esi (the status) and never *ebx (the reject flag),
// unlike Tetra Master (-> and [ebx],0) and FMO (mov [ebx],edx). Fixing it is
// correct but currently moot: Friend List ships no file.txt, and the LAST gate
// opens <installdir>\file.txt and requires it non-empty, so it is dropped there
// regardless. Site B is off by default for that reason.
//
// Everything here is RVA-based against the unpacked image and verified byte-for-
// byte before writing, so a different build fails safe instead of corrupting code.
#include "polshim.h"

struct Patch {
    const char* module;
    DWORD       rva;
    const BYTE* orig;
    const BYTE* want;
    int         len;
    int         optional;   // 1 = only applied when explicitly enabled
    const char* name;
    BYTE*       addr;
    int         applied;
    LONG        reverts;    // times the page was found restored and re-applied
    int         quiet;      // 1 = stop logging this site (unexpected bytes)
};

// site A: `cmp dword [ebp+8],4` + `jne +6` -> NOP the jne.
static const BYTE A_ORIG[] = { 0x83, 0x7D, 0x08, 0x04, 0x75, 0x06 };
static const BYTE A_WANT[] = { 0x83, 0x7D, 0x08, 0x04, 0x90, 0x90 };

// site B: the Friend List / Navigator accept tail.
//   85 F6        test esi,esi        ->  83 26 00   and dword [esi],0
//   74 03        je   +3             ->  83 23 00   and dword [ebx],0
//   83 26 00     and dword [esi],0   ->  90         nop
// The null guards are safe to drop (esi is `lea esi,[edi+0x338]`, ebx a caller
// local; both always non-null here). The replacement is EXACTLY 7 bytes because
// `mov bl,1` must stay at RVA 0x27a53d -- Tetra Master's tail jumps there.
static const BYTE B_ORIG[] = { 0x85, 0xF6, 0x74, 0x03, 0x83, 0x26, 0x00 };
static const BYTE B_WANT[] = { 0x83, 0x26, 0x00, 0x83, 0x23, 0x00, 0x90 };

// site C: the native sign-up wizard's URL scheme. BuildSignupUrlAndOpen does
// sprintf("https:%s%s", POL_UCS_URL, POL_UCS_CGI); that format string is a UNIQUE
// UTF-16LE literal in .rdata at RVA 0x3CF920. Rewriting "https:%s%s\0" to
// "http:%s%s\0\0" (one code unit shorter, tail padded with a NUL word, so the
// 22-byte region and every later offset are unchanged) makes the wizard build an
// http:// URL and fetch it over plain HTTP -- no certificate, no SSLv3, no trust
// store. Pair with POL_UCS_URL=//ucs.pol.com:8080/pml-cgi-bin/ in env.dat so the
// fetch lands on the plain ucscgi port (an implicit-port URL maps to band 51305,
// which is TLS-only). This is the in-memory twin of the on-disk sign-up scheme patch:
// same bytes, but applied at load so it needs no on-disk edit and no closed client.
// Doing it here also dodges the DLL-is-locked-while-running problem the on-disk
// tool hits. The byte-verify guard makes it fail safe on any other build.
static const BYTE C_ORIG[] = { 0x68,0x00, 0x74,0x00, 0x74,0x00, 0x70,0x00, 0x73,0x00,
                               0x3A,0x00, 0x25,0x00, 0x73,0x00, 0x25,0x00, 0x73,0x00, 0x00,0x00 };
static const BYTE C_WANT[] = { 0x68,0x00, 0x74,0x00, 0x74,0x00, 0x70,0x00, 0x3A,0x00,
                               0x25,0x00, 0x73,0x00, 0x25,0x00, 0x73,0x00, 0x00,0x00, 0x00,0x00 };

static Patch g_patches[] = {
    { "app.dll", 0x27A439, A_ORIG, A_WANT, sizeof(A_ORIG), 0, "filecheck_all", 0, 0 },
    { "app.dll", 0x27A536, B_ORIG, B_WANT, sizeof(B_ORIG), 1, "filecheck_fl",  0, 0 },
    { "app.dll", 0x3CF920, C_ORIG, C_WANT, sizeof(C_ORIG), 0, "signup_http",   0, 0 },
};

static int g_enabled  = 0;   // [polshim] patches=
static int g_optional = 0;   // [polshim] patches_optional=

void patches_set_enabled(int on)  { g_enabled = on; }
int  patches_enabled()            { return g_enabled; }
void patches_set_optional(int on) { g_optional = on; }

// Apply every patch belonging to `module`. Called once per module on its first
// DllGetClassObject, after the POL1 stub has unpacked .text.
void patches_apply_module(void* module_base, const char* module)
{
    if (!g_enabled || !module_base || !module) return;
    BYTE* base = (BYTE*)module_base;

    for (int i = 0; i < _countof(g_patches); i++) {
        Patch* p = &g_patches[i];
        if (p->applied) continue;
        if (_stricmp(p->module, module) != 0) continue;
        if (p->optional && !g_optional) {
            logf("[patch] %s: skipped (optional; set patches_optional=1)", p->name);
            continue;
        }
        p->addr = base + p->rva;

        // Refuse unless the bytes are exactly what this build should have. Being
        // already-patched is fine and idempotent; anything else means a different
        // app.dll and we leave it strictly alone.
        if (memcmp(p->addr, p->want, p->len) == 0) {
            logf("[patch] %s @ %p (%s+0x%05lX): already applied", p->name, p->addr, module, p->rva);
            p->applied = 1;
            continue;
        }
        if (memcmp(p->addr, p->orig, p->len) != 0) {
            char got[3 * 8 + 1] = "";
            for (int k = 0; k < p->len && k < 8; k++)
                sprintf(got + strlen(got), "%02X ", p->addr[k]);
            logf("[patch] %s @ %p (%s+0x%05lX): UNEXPECTED bytes [%s] -- NOT patching "
                 "(different build?)", p->name, p->addr, module, p->rva, got);
            continue;
        }

        DWORD old;
        if (!VirtualProtect(p->addr, p->len, PAGE_EXECUTE_READWRITE, &old)) {
            logf("[patch] %s: VirtualProtect failed at %p err=%lu",
                 p->name, p->addr, GetLastError());
            continue;
        }
        memcpy(p->addr, p->want, p->len);
        VirtualProtect(p->addr, p->len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p->addr, p->len);

        p->applied = (memcmp(p->addr, p->want, p->len) == 0);
        logf("[patch] %s @ %p (%s+0x%05lX): %s", p->name, p->addr, module, p->rva,
             p->applied ? "applied" : "WRITE VERIFY FAILED");
    }
    log_flush();
}

// Forget every site belonging to `module`, so the next patches_apply_module puts
// them back. Called by the injector the moment it re-arms a module it has hooked
// before (inject.cpp, eat_patch) -- i.e. the module was UNLOADED and re-LOADED.
//
// This is the other half of the decay fix, and the half that actually matters.
// A re-loaded module's .text is unpacked from the file again, so it carries
// ORIGINAL bytes; our `applied=1` and our stored `addr` both describe a mapping
// that no longer exists. Without this, patches_apply_module's `if (p->applied)
// continue` skips the fresh image and patches_reverify's `if (!p->applied ||
// !p->addr) continue` skips it too -- the two guards deadlock and the site is
// never restored for the rest of the session. `quiet` is cleared as well: it was
// set against the old mapping's bytes and must not silence the new one.
void patches_forget_module(const char* module)
{
    if (!module) return;
    for (int i = 0; i < _countof(g_patches); i++) {
        Patch* p = &g_patches[i];
        if (_stricmp(p->module, module) != 0) continue;
        p->addr    = NULL;
        p->applied = 0;
        p->quiet   = 0;
    }
}

// --- re-apply: these patches DECAY -------------------------------------------
//
// patches_apply_module runs ONCE, on a module's first DllGetClassObject. That is
// the earliest safe moment (the POL1 stub has just unpacked .text), but it is not
// the last word: something restores app.dll's page afterwards, and the patch is
// gone by the time the user launches a title.
//
// Measured 2026-08-13: launching Fantasy Earth IMMEDIATELY after a fresh Viewer
// start shows no "beta test is installed" error; the same launch later in the
// same session does -- while the log still says "filecheck_all: applied" from
// startup. Site A is what clears that flag (it NOPs a `jne` so id 11 falls into
// `mov [ebx],edx`), so a decayed patch means the beta gate comes back.
//
// CORRECTION 2026-08-17: "something restores the page" was the wrong model, and
// this function has therefore never once fired -- no log on this box has ever
// carried a "page had been RESTORED" line. What actually happens is that a failed
// title launch UNLOADS and re-LOADS app.dll wholesale; the new mapping is original
// because it was just read off disk, not because anyone rewrote our bytes. That
// case is handled by patches_forget_module + the injector's re-hook, above.
// Keep this function anyway: it is cheap, and it is the only thing that would
// catch a genuine in-place rewrite if one ever does turn up.
//
// Cheap enough to call from a hot path: a memcmp of <=22 bytes per site, and
// nothing else unless a site actually needs rewriting. Rides COM activation
// (comtrace) because that recurs constantly and, more to the point, a content
// title is LAUNCHED through it -- so the check lands right before it matters.
void patches_reverify(void)
{
    if (!g_enabled) return;
    // One writer at a time; a second caller just skips this round.
    static LONG busy = 0;
    if (InterlockedCompareExchange(&busy, 1, 0) != 0) return;

    for (int i = 0; i < _countof(g_patches); i++) {
        Patch* p = &g_patches[i];
        if (!p->applied || !p->addr) continue;

        // AVOID the fault, do not merely catch it. The __try below is a backstop,
        // but a CAUGHT first-chance access violation is not free on Proton/Wine:
        // a title with its own first-chance/unhandled crash reporter (Fantasy
        // Earth walks the stack via dbghelp on ANY exception) reacts to it, and
        // Wine's dbghelp then crashes the process (measured 2026-08-19: this
        // memcmp faults the instant app.dll UNLOADS during the FE launch hand-off,
        // and FE's dbghelp reporter turns it fatal). VirtualQuery never faults, so
        // gate on it: if p->addr's page is no longer committed+readable (the module
        // unloaded under us), forget the patch instead of touching it.
        MEMORY_BASIC_INFORMATION mbi;
        const DWORD READABLE = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY;
        if (VirtualQuery(p->addr, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
            !(mbi.Protect & READABLE) ||
            (BYTE*)p->addr + p->len > (BYTE*)mbi.BaseAddress + mbi.RegionSize) {
            p->addr = NULL;
            p->applied = 0;
            continue;
        }

        // SEH backstop: even after the VirtualQuery gate, the page could be torn
        // down between the query and the compare.
        __try {
            if (memcmp(p->addr, p->want, p->len) == 0) continue;   // still good

            if (memcmp(p->addr, p->orig, p->len) != 0) {
                if (!p->quiet) {
                    p->quiet = 1;
                    logf("[patch] %s @ %p: bytes are neither patched nor original "
                         "-- leaving alone (something else owns this page)",
                         p->name, p->addr);
                    log_flush();
                }
                continue;
            }

            DWORD old;
            if (!VirtualProtect(p->addr, p->len, PAGE_EXECUTE_READWRITE, &old))
                continue;
            memcpy(p->addr, p->want, p->len);
            VirtualProtect(p->addr, p->len, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p->addr, p->len);

            if (memcmp(p->addr, p->want, p->len) == 0) {
                LONG n = InterlockedIncrement(&p->reverts);
                // Log the first few and then powers of ten: a page restored on a
                // timer would otherwise bury the log, and the useful signal is
                // "it happens at all" plus roughly how often.
                if (n <= 3 || n == 10 || n == 100 || n == 1000 || n == 10000)
                    logf("[patch] %s @ %p: page had been RESTORED -- re-applied "
                         "(%ld time%s)", p->name, p->addr, n, n == 1 ? "" : "s");
                log_flush();
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Module went away (or the page is gone): forget the address rather
            // than fault again on every activation.
            p->addr = NULL;
            p->applied = 0;
        }
    }
    InterlockedExchange(&busy, 0);
}

// Called from the summary path so a session says plainly whether decay happened.
void patches_summary(void)
{
    if (!g_enabled) return;
    for (int i = 0; i < _countof(g_patches); i++) {
        Patch* p = &g_patches[i];
        if (p->reverts)
            logf("[patch] %s: re-applied %ld time(s) this session -- the page IS "
                 "being restored, the one-shot at startup is not enough",
                 p->name, p->reverts);
    }
}
