// authkey.cpp -- the login-hang fix, phase 1 (measure) + phase 2 (repair),
// behind one trampoline hook on polcore's auth key-setup.
//
// THE BUG (traced, not guessed):
//
//   Sometimes a login sits on "Connecting to PlayOnline" for minutes and then
//   recovers on its own. The Viewer replays a Blowfish session key CACHED from a
//   prior dial when it sends its NICK, instead of re-keying from the fresh
//   greeting our server just sent. Our server tries K=0 + the last-issued key +
//   every persisted stamp and none decrypt the NICK, so it drops the login. The
//   tell is that `nick_ct` is BYTE-IDENTICAL across retries and reboots -- a
//   fixed cached key, not a fresh handshake. It self-recovers because after
//   enough retries the client eventually re-handshakes to K=0.
//
// WHY THE FIX IS CLIENT-SIDE. Post-NICK the auth channel is encrypted with the
// CLIENT'S session key; on a crib failure the server does not hold that key, so
// it cannot send a decodable redirect or error. A bare `return` is all the
// server can do. Proven undeliverable -- do NOT re-attempt a server redirect.
//
// THE HOOK POINT. polcore's auth cipher is keyed by exactly ONE function,
// polcrypt_init (RVA 0x63EB0, decomp FUN_03823eb0):
//
//     void __cdecl polcrypt_init(cipher_ctx, sbox, Klo, Khi, ivptr)
//         bf_setkey(&Klo, 8, cipher_ctx, sbox)   ; the 8-byte key, BY VALUE
//         cipher_ctx[0x60] = 0                    ; stream position
//         cipher_ctx[0x58..0x5f] = ivptr[0..7]    ; IV from the RSA modulus
//
// It has a SINGLE call site in the whole image: 0x037D622B, inside the
// numeric-300 IRC handler (FUN_037d5e80), the state==8 rekey arm that is
// SUPPOSED to consume our greeting token. That single-caller fact is what makes
// this safe: every polcrypt_init call IS the auth rekey. The lobby/world ciphers
// are keyed elsewhere and are never touched here.
//
// WHAT THIS DOES, IN TWO PHASES, ONE HOOK.
//
//   log=1  (default ON): on every call, log K / IV / sbox / ctx / caller. This
//          is the measurement the whole plan hinges on, and it must be a
//          trampoline hook -- probes.cpp's INT3+VEH/hardware-DR probe of this
//          same function is DEAD under Wine/Proton, so it cannot run on the Deck.
//          The question it answers: in a STUCK login, does polcrypt_init fire at
//          all? If it fires with a stale K, phase 2 fixes it. If it does NOT fire
//          (the stale cipher is simply reused, no rekey), then this hook is the
//          wrong lever and the fix moves to zeroing the key at connection setup
//          -- and the log is how we tell those two apart.
//
//   rekey_zero=1  (default OFF): force Klo=Khi=0 before the real keying, so the
//          NICK goes out under K=0, which our server's K=0 candidate ALWAYS
//          tries and therefore always decrypts. This is a no-op in the healthy
//          case (against our server the correct auth key is already K=0: the
//          directory publishes the all-'T' token0 => base=0 => modexp=0) and a
//          repair in the stale-key case. It only helps if the hook actually
//          fires -- see log=1 above. Ship OFF, flip only once the log justifies.
//
// IMPORTANT: SCOPE GUARD. If this install is pointed at REAL Square Enix
// ([redirect] enable=1, netredir_se_bypass()), SE issues a genuine non-zero
// token and the key MUST be the real modexp result -- forcing K=0 would break
// that login. So the force is refused, loudly, whenever SE-bypass is on. Logging
// stays safe in every mode.
//
// WARNING: It logs key material to polshim.<pid>.log. [logship] redaction does not
// cover it. K is normally all-zeros here, but leave `log` off on any install
// whose logs leave the machine and that you do not want carrying auth IVs.
#include "polshim.h"

// --- polcore RVAs (ImageBase 0x037C0000, read off the unpacked polcore.dll) ---
#define AK_POLCRYPT_RVA     0x63EB0     // polcrypt_init / FUN_03823eb0
#define AK_STEAL            9           // relocatable prologue bytes to steal
// 8B 44 24 08   mov eax,[esp+8]      (arg2 sbox)
// 56            push esi
// 8B 74 24 08   mov esi,[esp+8]      (arg1 cipher_ctx, after the push)
// All three are esp-relative with no absolute/relocated operand, so they run
// unchanged from a trampoline -- the same class tmlog.cpp steals.
static const BYTE AK_PROLOGUE[AK_STEAL] =
    { 0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x08 };

// The ONE call site's return address (call at 0x1622B, 5-byte E8, ret=0x16230).
// A call that does NOT land here would be a different (future) caller and is
// logged as such rather than forced.
#define AK_AUTH_CALLER_RET  0x16230

// __cdecl, all args on the stack. Returns DWORD only so the hook can pass the
// original EAX straight through (polcrypt_init is logically void, but preserving
// EAX costs nothing and removes a question).
typedef DWORD (__cdecl *PFN_polcrypt)(DWORD ctx, DWORD sbox, DWORD klo,
                                      DWORD khi, DWORD ivptr);

static int   g_log = 1;                 // default ON: this is the measurement
static int   g_force_zero = 0;          // default OFF: the phase-2 repair
static bool  g_hooked = false;
static BYTE* g_polcore_base = NULL;
static PFN_polcrypt g_tramp = NULL;     // stolen prologue + jmp back
static DWORD g_hook_fp = 0;             // &polcrypt_hook, for the FF25 at entry
static DWORD g_tramp_back = 0;          // resume addr for the trampoline's jmp
static long  g_calls = 0, g_forced = 0;

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

static DWORD __cdecl polcrypt_hook(DWORD ctx, DWORD sbox, DWORD klo,
                                   DWORD khi, DWORD ivptr)
{
    InterlockedIncrement(&g_calls);

    // Entered via the FF25 jmp at the function entry, so the frame is exactly
    // the real caller's: _ReturnAddress() is the site that called polcrypt_init.
    DWORD caller = (DWORD)(ULONG_PTR)_ReturnAddress();
    DWORD caller_rva = g_polcore_base ? caller - (DWORD)(ULONG_PTR)g_polcore_base : 0;
    bool  is_auth = (caller_rva == AK_AUTH_CALLER_RET);

    // Read the IV the real call is about to install (8 bytes at ivptr).
    BYTE iv[8]; bool gotiv = false;
    if (ivptr && !IsBadReadPtr((void*)(ULONG_PTR)ivptr, 8)) {
        memcpy(iv, (void*)(ULONG_PTR)ivptr, 8); gotiv = true;
    }

    // Decide the force. Only for the auth site, only against our own server, and
    // only when nothing about the key is already zero-safe would be irrelevant
    // -- we force unconditionally when armed because forcing K=0 is a no-op when
    // the key is already 0 (the healthy case).
    bool se = (netredir_se_bypass() != 0);
    bool do_force = g_force_zero && is_auth && !se;

    if (g_log) {
        // Klo,Khi in memory order == the 8 key bytes bf_setkey consumes.
        DWORD kk[2] = { klo, khi };
        const BYTE* kb = (const BYTE*)kk;
        char khex[17], ivhex[17], sbx[32];
        for (int i = 0; i < 8; i++) wsprintfA(khex + i * 2, "%02x", kb[i]);
        for (int i = 0; i < 8; i++)
            wsprintfA(ivhex + i * 2, gotiv ? "%02x" : "??", gotiv ? iv[i] : 0);
        khex[16] = ivhex[16] = 0;
        if (g_polcore_base && (BYTE*)(ULONG_PTR)sbox >= g_polcore_base &&
            (BYTE*)(ULONG_PTR)sbox < g_polcore_base + 0x451000)
            wsprintfA(sbx, "polcore+0x%06lX",
                      (DWORD)((BYTE*)(ULONG_PTR)sbox - g_polcore_base));
        else
            wsprintfA(sbx, "0x%08lX", sbox);

        bool kzero = (klo == 0 && khi == 0);
        // One machine-parsable line; keeps the `polcryptInit K=.. IV=..` shape
        // build\poldecrypt.exe already greps.
        logf("[auth] polcryptInit K=%s IV=%s sbox=%s ctx=0x%08lX "
             "caller=polcore+0x%05lX%s Kzero=%d%s (call #%ld)",
             khex, ivhex, sbx, ctx, caller_rva,
             is_auth ? " [AUTH]" : " [non-auth?]", kzero ? 1 : 0,
             do_force ? " -> FORCING K=0" :
                 (g_force_zero && is_auth && se ?
                     " -> force REFUSED (SE-bypass on: real SE needs the real key)"
                     : ""),
             g_calls);
        log_flush();
    }

    if (do_force) {
        klo = 0; khi = 0;
        InterlockedIncrement(&g_forced);
    }

    // Run the real keying via the trampoline (stolen prologue -> jmp 0x63EB0+9).
    // The (possibly zeroed) klo/khi are re-pushed, so bf_setkey sees them.
    return g_tramp ? g_tramp(ctx, sbox, klo, khi, ivptr) : 0;
}

void authkey_configure(const wchar_t* ini)
{
    // log defaults ON (the diagnostic), rekey_zero defaults OFF (the repair).
    // No iniheal row for either -- the fmokey precedent: a key-logging switch
    // does not belong in the settings dialog, and there is nothing for a stale
    // install to heal into while the repair is unproven. When phase 2 is
    // measured and promoted to the shipped fix, rekey_zero becomes an
    // inert-until-healed row with an EXEMPT entry in the ini-defaults check.
    g_log        = GetPrivateProfileIntW(L"auth", L"log", 1, ini);
    g_force_zero = GetPrivateProfileIntW(L"auth", L"rekey_zero", 0, ini);

    if (g_force_zero && netredir_se_bypass())
        logf("[auth] rekey_zero=1 but this install is bypassing to REAL SE "
             "([redirect] enable=1) -- the force will be REFUSED per call so it "
             "cannot break an SE login. Logging still runs.");
}

// Live re-read for the in-game settings dialog: both flags are consulted per
// call inside polcrypt_hook, so a new value takes effect at the very next
// rekey. The detour itself is installed once, at polcore load
// (authkey_apply_module) -- nothing to arm or disarm here, and if BOTH flags
// were off at startup the hook was never placed, so turning one on now is
// restart-bound.
void authkey_reload(const wchar_t* ini)
{
    g_log        = GetPrivateProfileIntW(L"auth", L"log", 1, ini);
    g_force_zero = GetPrivateProfileIntW(L"auth", L"rekey_zero", 0, ini);

    if (g_force_zero && netredir_se_bypass())
        logf("[auth] rekey_zero=1 but this install is bypassing to REAL SE "
             "([redirect] enable=1) -- the force will be REFUSED per call so it "
             "cannot break an SE login. Logging still runs.");
    logf("[reload] authkey: log=%d rekey_zero=%d (hooked=%d; the detour is "
         "polcore-load only)", g_log, g_force_zero, g_hooked ? 1 : 0);
}

int  authkey_enabled()       { return g_log || g_force_zero; }
int  authkey_wants_modules() { return g_log || g_force_zero; }

void authkey_apply_module(void* module_base, const char* module)
{
    if (!module_base || !module) return;
    if (g_hooked || !authkey_wants_modules()) return;
    if (_stricmp(module, "polcore.dll") != 0) return;

    BYTE* base = (BYTE*)module_base;
    BYTE* addr = base + AK_POLCRYPT_RVA;
    if (IsBadReadPtr(addr, AK_STEAL)) {
        logf("[auth] polcore+0x%05X not readable -- not hooked", AK_POLCRYPT_RVA);
        return;
    }

    // Verify the prologue before touching it: a polcore that does not match is a
    // different build and is left strictly alone (patches.cpp / tmlog.cpp rule).
    if (memcmp(addr, AK_PROLOGUE, AK_STEAL) != 0) {
        char got[3 * AK_STEAL + 1] = "";
        for (int k = 0; k < AK_STEAL; k++)
            wsprintfA(got + lstrlenA(got), "%02X ", addr[k]);
        // The common, non-error case on NATIVE Windows: probes.cpp's INT3
        // polcryptInit probe ([polshim] probes=1) has planted 0xCC over the
        // first byte, so only byte 0 differs and it is CC. Two hooks cannot own
        // one entry -- the probe (INT3, works on native Windows) and this
        // trampoline (for Wine/Deck, where INT3 is dead) are complementary, not
        // simultaneous. Say exactly that instead of guessing a build mismatch.
        if (addr[0] == 0xCC &&
            memcmp(addr + 1, AK_PROLOGUE + 1, AK_STEAL - 1) == 0) {
            logf("[auth] polcore+0x%05X starts with 0xCC -- the INT3 polcryptInit "
                 "PROBE already owns this entry ([polshim] probes=1). [auth] stands "
                 "down: the probe logs the SAME K/IV/caller ([probe] polcryptInit). "
                 "To exercise this trampoline instead, set [polshim] probes=0. On "
                 "the Deck there is no conflict -- INT3 does not arm under Wine.",
                 AK_POLCRYPT_RVA);
            return;
        }
        logf("[auth] polcore+0x%05X holds [%s] -- expected "
             "8B 44 24 08 56 8B 74 24 08. NOT hooking; the auth key fix is "
             "INACTIVE (different polcore build?).", AK_POLCRYPT_RVA, got);
        return;
    }

    // Executable trampoline: stolen prologue, then an absolute jmp back to +9.
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        logf("[auth] VirtualAlloc failed err=%lu -- not hooked", GetLastError());
        return;
    }
    memcpy(tramp, AK_PROLOGUE, AK_STEAL);
    g_tramp_back = (DWORD)(ULONG_PTR)(addr + AK_STEAL);
    tramp[AK_STEAL + 0] = 0xFF;                 // jmp dword ptr [&g_tramp_back]
    tramp[AK_STEAL + 1] = 0x25;
    *(DWORD*)(tramp + AK_STEAL + 2) = (DWORD)(ULONG_PTR)&g_tramp_back;
    g_tramp = (PFN_polcrypt)tramp;

    // Overwrite the entry with `jmp dword ptr [&g_hook_fp]` + NOP pad.
    g_hook_fp = (DWORD)(ULONG_PTR)&polcrypt_hook;
    BYTE want[AK_STEAL];
    want[0] = 0xFF; want[1] = 0x25;
    *(DWORD*)(want + 2) = (DWORD)(ULONG_PTR)&g_hook_fp;
    for (int k = 6; k < AK_STEAL; k++) want[k] = 0x90;

    DWORD old;
    if (!VirtualProtect(addr, AK_STEAL, PAGE_EXECUTE_READWRITE, &old)) {
        logf("[auth] VirtualProtect failed err=%lu -- not hooked", GetLastError());
        return;
    }
    memcpy(addr, want, AK_STEAL);
    VirtualProtect(addr, AK_STEAL, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, AK_STEAL);

    g_polcore_base = base;
    g_hooked = (memcmp(addr, want, AK_STEAL) == 0);
    logf("[auth] polcrypt_init hook @ %p (polcore+0x%05X) trampoline %p: %s. "
         "log=%d rekey_zero=%d%s",
         addr, AK_POLCRYPT_RVA, tramp, g_hooked ? "armed" : "WRITE VERIFY FAILED",
         g_log, g_force_zero,
         (g_force_zero && netredir_se_bypass()) ? " (force refused: SE-bypass)" : "");
    log_flush();
}

void authkey_summary()
{
    if (!authkey_wants_modules()) return;
    logf("[auth] summary: hooked=%d polcrypt_init calls=%ld forced-to-K0=%ld",
         g_hooked ? 1 : 0, g_calls, g_forced);
    if (g_hooked && !g_calls)
        logf("[auth]   polcrypt_init NEVER fired this session -- so a stuck "
             "login here would be the 'stale cipher reused, no rekey' case, and "
             "forcing K=0 at this hook cannot help it. The fix would move to "
             "zeroing ctx+0x2b8 at connection setup.");
}
