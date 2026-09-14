// fepatch.cpp -- the Fantasy Earth scene-graph teardown guard.
//
// WHAT THIS FIXES. On Wine/Proton (Steam Deck), Fantasy Earth reaches its logos
// and then dies in a recursive post-order graph destructor at FE_Client.dll RVA
// 0x2AC5B0. The crash instruction is `mov eax,[esi+8]` at 0x2AC5C4 -- reading a
// node's child pointer (+0x08) -- with esi==0. A graph LINK comes out NULL where
// the code requires the runtime list-end SENTINEL global [RVA 0x3CDFD8]; both
// terminator checks (entry 0x2AC5BE, loop 0x2AC5E7) compare ONLY to that sentinel,
// so a NULL link falls through and dereferences address 8. On desktop Windows the
// same graph is sentinel-terminated and the site never sees NULL; the NULL is a
// systematic Windows-vs-Wine data difference upstream (leading suspect: the file
// enumeration ORDER a data-driven builder walks) that is NOT fixed here. This
// patch guards the READ so a NULL link is treated as end-of-list, which is exactly
// the sentinel case.
//
// THE DESTRUCTOR (FE_Client.dll RVA 0x2AC5B0, byte-identical desktop<->Deck except
// the baked absolute sentinel address, which this patch does not touch):
//
//     0x2AC5B0  mov  eax,[0x3CDFD8]     ; eax = sentinel (list end)
//     0x2AC5B8  mov  edi,[esp+0x10]     ; edi = node arg
//     0x2AC5BE  cmp  edi, eax / je end  ; node == sentinel -> return
//     0x2AC5C0  mov  esi, edi
//     0x2AC5C4  mov  eax,[esi+8]        ; *** FAULT when esi==0: eax = node->child
//     0x2AC5C7  mov  ecx, ebx
//     0x2AC5C9  push eax / call 0x2AC5B0; recurse(child)
//     0x2AC5CF  mov  esi,[esi]          ; esi = node->next
//     ...       destruct + free the node ...
//     0x2AC5E7  cmp  esi, eax / jne 0x2AC5C4 ; next == sentinel? loop if not
//     0x2AC5ED  pop edi/esi/ebx / ret 4 ; end
//
// THE PATCH. Steal the 5 bytes at 0x2AC5C4 (`8B 46 08 8B CB` = `mov eax,[esi+8];
// mov ecx,ebx` -- two whole instructions, next instr at 0x2AC5C9) and replace them
// with `E9 <rel32>` to a trampoline that null-checks esi first:
//
//     test esi,esi
//     jnz  do_orig          ; non-NULL -> run the stolen instructions
//     jmp  0x2AC5ED         ; NULL -> jump to the function's own return path
//   do_orig:
//     mov  eax,[esi+8]      ; the stolen bytes
//     mov  ecx,ebx
//     jmp  0x2AC5C9         ; back to just after the stolen region
//
// The loop's own re-entry (`jne 0x2AC5C4`) targets the START of the stolen region,
// so the guard runs on every sibling as well as the first node -- both the ways
// esi can arrive NULL (fall-through from the entry sentinel check on a NULL node,
// and a NULL `next` in the loop) are caught, and both take the same clean exit the
// sentinel case takes.
//
// SAFE BY CONSTRUCTION: treating a NULL link as end-of-list is exactly what the
// sentinel case already does; at a teardown any subtree not walked is a harmless
// leak (the scene is ending). RVA-keyed and byte-verified against the unpacked
// image, so a different FE build fails safe (refuses to patch) instead of
// corrupting code. Must apply AFTER the ASProtect stub unpacks .text -- armed from
// the d3d8 CreateDevice hook, whose return address lands inside FE_Client, the one
// moment FE's image is provably in the clear (same timing constraint as the TM
// byte-patch in tmlog.cpp).
//
// UNKNOWN UNTIL LIVE (flagged, not a build blocker): whether this teardown is a
// normal scene transition (patch -> FE proceeds past the logos) or an error unwind
// (patch -> FE survives this site and dies at the next wall). Do not call the FE
// crash fixed on anything less than FE advancing past the logos on the Deck.
#include "polshim.h"

// The destructor lives in the Fantasy Earth title DLL. SE ships an EN build under a
// separate leaf; both carry the identical RVA (same engine, same relocation-free
// walk bytes). Any other module name is ignored.
static const char* FE_MODULES[] = { "FE_Client.dll", "FE_Client.en.dll" };

// RVAs into the unpacked FE_Client image. Confirmed byte-identical across the
// desktop dump (base 0x05210000) and the Deck dump (base 0x02EA0000).
#define FE_GUARD_RVA   0x2AC5C4UL   // the faulting `mov eax,[esi+8]` -- patch site
#define FE_RESUME_RVA  0x2AC5C9UL   // instruction after the 5 stolen bytes
#define FE_END_RVA     0x2AC5EDUL   // the function's own pop/ret tail

// The 5 bytes we steal: `mov eax,[esi+8]` (8B 46 08) + `mov ecx,ebx` (8B CB).
static const BYTE FE_ORIG[5] = { 0x8B, 0x46, 0x08, 0x8B, 0xCB };

// GUARD 2 -- the per-node destruct method at 0x2AAA90, called from the destructor
// loop above (0x2AC5D4). It takes the sub-object at [ecx+8], null-checks it, then
// tail-calls a virtual method: `mov edx,[eax]; mov ecx,eax; jmp [edx+0xc]`. On Wine
// that field can hold a GARBAGE pointer (measured 0xF15BD98C) that passes the null
// check but whose vtable is junk, so `jmp [edx+0xc]` faults (or eip=0 on a NULL
// slot). The method guards NULL but not garbage. We steal the 7-byte tail at
// 0x2AAA9E and route it through a trampoline that also range-checks the pointer and
// its vtable are sane user-space addresses; a garbage node is skipped (ret) -- a
// harmless teardown leak, exactly the 0x2AC5C4 rationale. Bytes are register-only
// (no absolute refs), byte-identical desktop<->Deck.
#define FE_G2_RVA   0x2AAA9EUL   // `mov edx,[eax]; mov ecx,eax; jmp [edx+0xc]`
static const BYTE FE_G2_ORIG[7] = { 0x8B, 0x10, 0x8B, 0xC8, 0xFF, 0x62, 0x0C };

static int   g_enabled = 0;     // [dx] fe_teardown_guard
static int   g_applied = 0;
static void* g_fe_base = NULL;  // base the guard was armed against
static BYTE* g_stub    = NULL;  // the trampoline (leaked for the process lifetime)
static LONG  g_said_mismatch = 0;
static int   g2_applied = 0;
static BYTE* g2_stub    = NULL;
static LONG  g2_said_mismatch = 0;

void fepatch_set_enabled(int on) { g_enabled = on; }
int  fepatch_enabled()           { return g_enabled; }

static int is_fe_module(const char* module)
{
    if (!module) return 0;
    for (int i = 0; i < _countof(FE_MODULES); i++)
        if (_stricmp(module, FE_MODULES[i]) == 0) return 1;
    return 0;
}

// Build the 19-byte trampoline for this load's base and return a leaked RWX copy,
// or NULL on failure. All three internal jumps are rel32 computed against the live
// base, so the stub is valid only for the base it was built for (re-armed if FE
// reloads elsewhere, mirroring tmlog).
static BYTE* build_stub(BYTE* base)
{
    BYTE* stub = (BYTE*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!stub) {
        logf("[fe] guard: VirtualAlloc for trampoline failed err=%lu", GetLastError());
        return NULL;
    }

    BYTE* end_target    = base + FE_END_RVA;      // 0x2AC5ED (function return)
    BYTE* resume_target = base + FE_RESUME_RVA;   // 0x2AC5C9 (after stolen bytes)

    int n = 0;
    stub[n++] = 0x85; stub[n++] = 0xF6;               // test esi,esi
    stub[n++] = 0x75; stub[n++] = 0x05;               // jnz do_orig (skip the 5-byte jmp)
    // jmp 0x2AC5ED  (NULL link -> end of walk)
    stub[n++] = 0xE9;
    *(LONG*)(stub + n) = (LONG)(end_target - (stub + n + 4)); n += 4;
    // do_orig: the stolen instructions
    stub[n++] = 0x8B; stub[n++] = 0x46; stub[n++] = 0x08;   // mov eax,[esi+8]
    stub[n++] = 0x8B; stub[n++] = 0xCB;                     // mov ecx,ebx
    // jmp 0x2AC5C9  (back into the original stream)
    stub[n++] = 0xE9;
    *(LONG*)(stub + n) = (LONG)(resume_target - (stub + n + 4)); n += 4;

    FlushInstructionCache(GetCurrentProcess(), stub, n);
    return stub;
}

// Guard-2 trampoline. Fully position-independent (register-relative + immediate
// constants only, and the tail `jmp [edx+0xc]` leaves for good), so unlike guard-1
// it needs no per-load rel32s -- only the E9 at the site points here.
//   cmp eax,0x10000 / jb bad ; cmp eax,0x7F000000 / jae bad   (sub-object sane?)
//   mov edx,[eax]                                             (vtable = *obj)
//   cmp edx,0x10000 / jb bad ; cmp edx,0x7F000000 / jae bad   (vtable sane?)
//   mov ecx,eax / jmp [edx+0xc]                               (the real vcall)
//   bad: ret                                                  (skip a garbage node)
static BYTE* build_stub2(void)
{
    BYTE* s = (BYTE*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (!s) { logf("[fe] guard2: VirtualAlloc failed err=%lu", GetLastError()); return NULL; }
    static const BYTE body[] = {
        0x3D,0x00,0x00,0x01,0x00,        // cmp eax,0x00010000       (0x3D = cmp EAX)
        0x72,0x1E,                       // jb  bad (-> 0x25)
        0x3D,0x00,0x00,0x00,0x7F,        // cmp eax,0x7F000000
        0x73,0x17,                       // jae bad
        0x8B,0x10,                       // mov edx,[eax]  (vtable = *obj)
        0x81,0xFA,0x00,0x00,0x01,0x00,   // cmp edx,0x00010000       (81 FA = cmp EDX)
        0x72,0x0D,                       // jb  bad
        0x81,0xFA,0x00,0x00,0x00,0x7F,   // cmp edx,0x7F000000
        0x73,0x05,                       // jae bad
        0x8B,0xC8,                       // mov ecx,eax
        0xFF,0x62,0x0C,                  // jmp dword ptr [edx+0xc]
        0xC3                             // bad: ret (skip the garbage node)
    };
    memcpy(s, body, sizeof(body));
    FlushInstructionCache(GetCurrentProcess(), s, sizeof(body));
    return s;
}

// Re-arm bookkeeping: FE reloaded at a new base means our stub's rel32s and our
// applied flag describe a mapping that no longer exists.
static void fepatch_rearm(const char* why)
{
    if (g_applied || g2_applied)
        logf("[fe] guard: %s -- re-arming", why);
    g_applied = 0;
    g_fe_base = NULL;
    // Leak the old stubs: another thread could still be mid-flight through one during
    // a teardown. A 4 KB page per reload is a non-issue next to a use-after-free.
    g_stub = NULL;
    g_said_mismatch = 0;
    g2_applied = 0;
    g2_stub = NULL;
    g2_said_mismatch = 0;
}

// Install guard 2 at FE_G2_RVA. Same byte-verified, idempotent discipline as the
// primary guard; independent applied flag so one site arming does not mask the other.
static void fepatch_apply_guard2(BYTE* base)
{
    if (g2_applied) return;
    BYTE* site = base + FE_G2_RVA;
    if (IsBadReadPtr(site, sizeof(FE_G2_ORIG))) return;   // not mapped yet; retry later

    if (site[0] == 0xE9) {                                // already a jmp
        if (g2_stub && site + 5 + *(LONG*)(site + 1) == g2_stub) { g2_applied = 1; return; }
        if (!g2_said_mismatch++)
            logf("[fe] guard2 @ %p (FE_Client+0x%05lX): holds a jmp we did not write -- NOT patching",
                 site, FE_G2_RVA);
        return;
    }
    if (memcmp(site, FE_G2_ORIG, sizeof(FE_G2_ORIG)) != 0) {
        int blank = 1;
        for (int k = 0; k < (int)sizeof(FE_G2_ORIG); k++) if (site[k]) { blank = 0; break; }
        if (!g2_said_mismatch++)
            logf(blank ? "[fe] guard2: FE_Client+0x%05lX still blank (packed) -- will retry"
                       : "[fe] guard2: FE_Client+0x%05lX unexpected bytes -- NOT patching (different build?)",
                 FE_G2_RVA);
        return;
    }

    BYTE* stub = build_stub2();
    if (!stub) return;

    // E9 rel32 to the trampoline, then two NOPs so the 7 stolen bytes are fully
    // overwritten and 0x2AAAA5 (the method's own `ret`) stays intact.
    BYTE want[7];
    want[0] = 0xE9;
    *(LONG*)(want + 1) = (LONG)(stub - (site + 5));
    want[5] = 0x90; want[6] = 0x90;

    DWORD old;
    if (!VirtualProtect(site, sizeof(want), PAGE_EXECUTE_READWRITE, &old)) {
        logf("[fe] guard2: VirtualProtect failed at %p err=%lu", site, GetLastError());
        VirtualFree(stub, 0, MEM_RELEASE); return;
    }
    memcpy(site, want, sizeof(want));
    VirtualProtect(site, sizeof(want), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(want));

    g2_applied = (memcmp(site, want, sizeof(want)) == 0);
    if (g2_applied) {
        g2_stub = stub;
        logf("[fe] guard2 @ %p (FE_Client+0x%05lX) -> trampoline %p: applied -- a garbage "
             "node vtable now skips its destructor call (was the 0x2AAA90 vcall crash)",
             site, FE_G2_RVA, stub);
    } else {
        logf("[fe] guard2 @ %p (FE_Client+0x%05lX): WRITE VERIFY FAILED", site, FE_G2_RVA);
        VirtualFree(stub, 0, MEM_RELEASE);
    }
    log_flush();
}

// Arm on FE_Client's unpacked image. Idempotent and byte-verified; the first
// success wins. Called from the d3d8 CreateDevice hook (see d3d8hook.cpp), whose
// return address is inside the calling title -- the reliable post-unpack moment.
void fepatch_apply_module(void* module_base, const char* module)
{
    if (!g_enabled || !module_base || !is_fe_module(module)) return;

    if (g_fe_base && module_base != g_fe_base)
        fepatch_rearm("FE_Client re-loaded at a new base");
    g_fe_base = module_base;

    BYTE* base = (BYTE*)module_base;

    // Guard 2 (the 0x2AAA90 vcall) is independent of guard 1 (the 0x2AC5C4 read):
    // each has its own site and applied flag, and either may arm on a later retry
    // than the other, so run it every call rather than behind guard 1's return.
    fepatch_apply_guard2(base);

    if (g_applied) return;

    BYTE* site = base + FE_GUARD_RVA;

    // A relative jmp to our trampoline: E9 <rel32 to stub>. Idempotent check first.
    if (!IsBadReadPtr(site, sizeof(FE_ORIG))) {
        if (site[0] == 0xE9) {
            // Already a jmp -- ours (from an earlier call this session) or someone
            // else's. If it targets our stub, we are done; otherwise leave it alone.
            if (g_stub) {
                BYTE* tgt = site + 5 + *(LONG*)(site + 1);
                if (tgt == g_stub) { g_applied = 1; return; }
            }
            if (!g_said_mismatch++)
                logf("[fe] guard @ %p (FE_Client+0x%05lX): already holds a jmp we did "
                     "not write -- NOT patching", site, FE_GUARD_RVA);
            return;
        }
        if (memcmp(site, FE_ORIG, sizeof(FE_ORIG)) != 0) {
            // Blank = ASProtect has not written .text yet (normal early; retry).
            // Anything else = a build this patch was not measured against (give up).
            int blank = 1;
            for (int k = 0; k < (int)sizeof(FE_ORIG); k++)
                if (site[k]) { blank = 0; break; }
            if (!g_said_mismatch++) {
                if (blank)
                    logf("[fe] guard: FE_Client+0x%05lX still blank (packed) -- will "
                         "retry once the stub unpacks .text", FE_GUARD_RVA);
                else {
                    char got[3 * 5 + 1] = "";
                    for (int k = 0; k < (int)sizeof(FE_ORIG); k++)
                        sprintf(got + strlen(got), "%02X ", site[k]);
                    logf("[fe] guard: FE_Client+0x%05lX holds [%s] not the expected "
                         "8B 46 08 8B CB -- NOT patching (different build?)",
                         FE_GUARD_RVA, got);
                }
            }
            return;
        }
    } else {
        return;   // page not readable yet; a later CreateDevice retry will catch it
    }

    // Bytes verified original. Build the trampoline, then redirect the site to it.
    g_fe_base = module_base;
    BYTE* stub = build_stub(base);
    if (!stub) return;

    BYTE want[5];
    want[0] = 0xE9;
    *(LONG*)(want + 1) = (LONG)(stub - (site + 5));

    DWORD old;
    if (!VirtualProtect(site, sizeof(want), PAGE_EXECUTE_READWRITE, &old)) {
        logf("[fe] guard: VirtualProtect failed at %p err=%lu", site, GetLastError());
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }
    memcpy(site, want, sizeof(want));
    VirtualProtect(site, sizeof(want), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(want));

    g_applied = (memcmp(site, want, sizeof(want)) == 0);
    if (g_applied) {
        g_stub = stub;
        logf("[fe] guard @ %p (FE_Client+0x%05lX) -> trampoline %p: applied -- NULL "
             "child link now treated as end-of-list (was the 0x2AC5C4 teardown crash)",
             site, FE_GUARD_RVA, stub);
    } else {
        logf("[fe] guard @ %p (FE_Client+0x%05lX): WRITE VERIFY FAILED", site, FE_GUARD_RVA);
        VirtualFree(stub, 0, MEM_RELEASE);
    }
    log_flush();
}
