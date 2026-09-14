// fmokey.cpp -- tap the two polcore values Front Mission Online's world login
// is built out of, and that our server cannot currently produce.
//
// WHY
//
// FMO's world door (TCP 61300) gets as far as the credentials
// exchange and then stops, because two things in that conversation are minted by
// polcore and are opaque both to FMO and to us:
//
//   * a 16-BYTE KEY. It becomes bytes 0..15 of the 20-byte session key that
//     FrontMissionOnline.dll+0x199eb0 loads into its RX/TX crypto contexts.
//     Bytes 16..19 come from OUR reply (the 0x0322 message's payload +0x08),
//     so the server already owns a fifth of the key and none of the rest.
//   * a 52-BYTE AUTH BLOB, which FMO copies whole into the credentials message
//     and never inspects.
//
// Both are read through polcore's common function table, and in polcore both are
// trivial getters over globals -- so a tap here sees exactly what FMO sees:
//
//   slot 1003 (+0xFAC) = polcore+0x1C870:  memcpy(dst, 0x0386AAD0, 0x10)
//   slot  936 (+0xEA0) = polcore+0x20020:  memcpy(dst, 0x03BC57B0, 0x34)
//
// The writer of both globals is one routine around polcore+0x1DDAA, which takes
// the key from its session object at [esi+0x328] and then DERIVES the 52-byte
// blob from that same key (0x37DA2D0(blob, ..., key, ..)). So the two are not
// independent secrets: the key is the root, and it is session state established
// during the POL login our own server drives.
//
// WHICH IS THE QUESTION THIS ANSWERS, and it is worth being precise about it,
// because the answer decides how much work FMO's world server is:
//
//   If the 16 bytes turn out to be something our auth stack already issues or
//   can recompute, the server can derive the session key and FMO's world
//   protocol opens up with no shim in the loop.
//   If they are client-local (a machine value, a random the client never sends),
//   then no server can derive them, and FMO would need the shim resident to
//   report the key per session -- a very different and much worse outcome.
//
// A capture cannot distinguish those two, which is exactly why this exists.
//
// HOW IT ATTACHES
//
// The same mechanism polfetch.cpp already uses: polcore publishes a flat
// dispatch table at polcore+0x6FBE8 and callers re-read the slot each time, so
// swapping an entry is enough -- no code patching. Both slots are VERIFIED to
// hold the addresses this build expects before anything is written; a polcore
// that does not match is left completely alone and the tap reports itself
// inactive rather than silently doing nothing.
//
// THIS MODULE CHANGES NO BEHAVIOUR. Both hooks call the original and log what it
// produced. If it is ever seen to alter a session, that is a bug.
//
// WARNING: It logs key material to polshim.<pid>.log by design. That log can be shipped
// by [logship], whose redaction covers credentials and not this, so leave
// `enable` off on any install whose logs leave the machine.
#include "polshim.h"
#include "fmokey.h"

// --- polcore RVAs -----------------------------------------------------------
// Read off the unpacked polcore.dll (ImageBase 0x037C0000) and cross-checked
// against the client's CFT table, which lists slot 1003 as polcore+0x01C870
// and slot 936 as polcore+0x020020. Every one is verified at runtime.
#define FK_CFT_RVA        0x6FBE8      // the common function table (same as polfetch)
#define FK_SLOT_KEY       0xFAC        // byte offset; index 1003
#define FK_SLOT_AUTH      0xEA0        // byte offset; index  936
#define FK_FN_KEY_RVA     0x1C870      // what slot 1003 must currently hold
#define FK_FN_AUTH_RVA    0x20020      // what slot  936 must currently hold

#define FK_KEY_LEN        0x10         // 16, per the getter's own memcpy length
#define FK_AUTH_LEN       0x34         // 52, likewise

typedef int (__cdecl *PFN_fk_key)(void* dst);
typedef int (__cdecl *PFN_fk_auth)(void* dst);

// --- the 0x13B GATE WATCHER -------------------------------------------------
//
// FMO's login reaches state 3 of the jump table at 0x6117A274 and then sends
// message 0x13B -- but only if 0x6119A380 says yes. That gate wants THREE things,
// and the third is easy to miss:
//
//     ctx+0x20 == 3      the connection is established
//     ctx+0x24 == 0      no send in flight
//     the linked list at ctx+0x7A0E is drained
//                        (0x6119A240 walks it to the tail; a tail with a
//                         non-zero +4 makes the gate FAIL)
//
// Live, the client sits connected and silent forever, which is exactly what a
// failing gate looks like. Which of the three is false has never been observed --
// it was inferred, and this exists to stop it being inferred.
//
// READ-ONLY BY CONSTRUCTION. It patches nothing and detours nothing: every input
// is at a known address, so a poller can just read them. That matters here
// because the alternative -- an inline detour on 0x6119A380 -- would mean
// rewriting code inside the very state machine under investigation.
//
//     ctx = *(void**)(*(void**)(fmo + 0x3AE664) + 0x190)
//
// Logs only on CHANGE, so a stuck client produces a handful of lines rather than
// one per tick.
#define FK_FMO_NETOBJ_RVA  0x3AE664    // FMO's global -> the object whose +0x190
#define FK_NET_OFF         0x190       //   is the network context
#define FK_ST_CONN         0x20        // == 3 when connected
#define FK_ST_SENDING      0x24        // == 0 when no send is in flight
#define FK_ST_QUEUE        0x7A0E      // list head the gate walks
// The RECEIVE side, so "the client never got it" and "the client got it and
// rejected it" stop looking identical from the server:
//   ctx+0x28   receive state -- 0x61199A00 switches on it; 0x61199C79 sets 4
//              when a packet has validated. The poll 0x61199E30 returns 0
//              immediately unless it is 4.
//   ctx+0x7580 the rx buffer. Its +0x06 is the message id the client is
//              holding and its +0x10 the correlation id it will compare.
#define FK_ST_RXSTATE      0x28
#define FK_ST_RXBUF        0x7580

// --- the LOGIN STATE BYTE ---------------------------------------------------
//
// The gate turned out to be OPEN while 0x13B still never went out, so the block
// is upstream: the login machine is not reaching the state that sends it. Its
// dispatcher is 0x61179DA0 --
//
//     mov al, byte [ebp+0x2C] / movzx edx,al / cmp edx,6 / ja default
//     jmp [edx*4 + 0x6117A274]        ; SEVEN states, 0..6
//
// -- and state 3 (0x61179FE8) is the one that sends 0x13B. So `obj+0x2C` is the
// number that matters, and this reports it.
//
// FINDING THE OBJECT WITHOUT GUESSING. It is not passed through a global we can
// read directly; the dispatcher takes it as `this`. But its constructor
// (0x61175DEB) writes a known VTABLE at offset 0:
//
//     mov dword [ebp], 0x6133B48C
//
// so we can SEARCH the game-globals struct for a pointer whose target begins
// with that vtable, and know we have the right object rather than hoping. A
// candidate that does not match is skipped; if none matches we say so instead of
// reporting a number from whatever happened to be at +0x2C.
#define FK_LOGIN_VTABLE    0x6133B48C  // written by the ctor at 0x61175DEB
#define FK_LOGIN_STATE     0x2C        // the state byte the dispatcher switches on
#define FK_SCAN_LO         0x100       // offsets of the globals struct to search
#define FK_SCAN_HI         0x400

static int  g_watch_gate = 0;
static HANDLE g_watch_thread = NULL;
static BYTE* g_fmo_base = NULL;
static volatile LONG g_watch_stop = 0;   // set by fmokey_stop; the daemon checks it

static int  g_enable = 0;              // opt-in: it logs key material
static bool g_hooked = false;
static PFN_fk_key  orig_key  = NULL;
static PFN_fk_auth orig_auth = NULL;
static long g_n_key = 0, g_n_auth = 0;

// Remember the last value so a REPEAT can be reported as such. Whether the key
// is stable across logins, across a whole process, or per world connection is
// precisely what we are trying to learn, and "same as last time" is the finding.
static BYTE g_last_key[FK_KEY_LEN];
static bool g_have_last = false;

static void hexline(const char* tag, const BYTE* p, int n)
{
    char buf[256];
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(buf) - 4; i++)
        o += wsprintfA(buf + o, "%02x", p[i]);
    buf[o] = 0;
    logf("[fmokey] %s = %s", tag, buf);
}

static int __cdecl hook_key(void* dst)
{
    int r = orig_key ? orig_key(dst) : -1;
    InterlockedIncrement(&g_n_key);
    if (dst && !IsBadReadPtr(dst, FK_KEY_LEN)) {
        const BYTE* p = (const BYTE*)dst;
        bool same = g_have_last && memcmp(p, g_last_key, FK_KEY_LEN) == 0;
        logf("[fmokey] *** 16-BYTE SESSION KEY read (call #%ld)%s -- this is "
             "bytes 0..15 of FMO's 20-byte world key; bytes 16..19 come from the "
             "server's 0x0322 reply",
             g_n_key, same ? " [SAME as the previous read]" : "");
        hexline("key16", p, FK_KEY_LEN);
        memcpy(g_last_key, p, FK_KEY_LEN);
        g_have_last = true;
    } else {
        logf("[fmokey] key read #%ld returned an unreadable buffer -- not logged",
             g_n_key);
    }
    return r;
}

static int __cdecl hook_auth(void* dst)
{
    int r = orig_auth ? orig_auth(dst) : -1;
    InterlockedIncrement(&g_n_auth);
    if (dst && !IsBadReadPtr(dst, FK_AUTH_LEN)) {
        logf("[fmokey] 52-byte auth blob read (call #%ld) -- what FMO copies "
             "whole into its credentials message", g_n_auth);
        hexline("auth52", (const BYTE*)dst, FK_AUTH_LEN);
    }
    return r;
}

// Read a pointer without faulting on a half-built object.
static void* safe_ptr(void* p)
{
    if (!p || IsBadReadPtr(p, sizeof(void*))) return NULL;
    return *(void**)p;
}

static DWORD WINAPI gate_watch(LPVOID)
{
    DWORD last_conn = 0xFFFFFFFF, last_send = 0xFFFFFFFF;
    DWORD last_depth = 0xFFFFFFFF, last_tail4 = 0xFFFFFFFF;
    DWORD last_state = 0xFFFFFFFE;
    DWORD last_rxst = 0xFFFFFFFE, last_rxmsg = 0xFFFFFFFE, last_rxseq = 0xFFFFFFFE;
    int   announced = 0;

    // WAIT FOR THE MODULE OURSELVES rather than being armed by a callback.
    // The first cut armed this from fmokey_apply_module -- but that is only
    // reached from the DllGetClassObject interposer, which fires for polcore.dll
    // and app.dll and NEVER for FrontMissionOnline.dll (no [gco] line for it in
    // any log). So the watcher could not have started, whatever the ini said.
    // GetModuleHandle needs no cooperation from any other subsystem, which is
    // the point: this is a diagnostic, and a diagnostic that depends on the
    // machinery under investigation is worth very little.
    while (!g_fmo_base) {
        if (g_watch_stop) return 0;
        HMODULE h = GetModuleHandleA("FrontMissionOnline.dll");
        if (h) { g_fmo_base = (BYTE*)h; break; }
        Sleep(1000);
    }
    logf("[fmokey] gate watcher: FrontMissionOnline.dll at %p -- watching the "
         "three inputs to 0x6119A380", g_fmo_base);

    for (;;) {
        if (g_watch_stop) return 0;
        Sleep(500);
        void* holder = safe_ptr(g_fmo_base + FK_FMO_NETOBJ_RVA);
        if (!holder) continue;
        BYTE* ctx = (BYTE*)safe_ptr((BYTE*)holder + FK_NET_OFF);
        if (!ctx || IsBadReadPtr(ctx, FK_ST_QUEUE + 8)) continue;

        DWORD conn = *(DWORD*)(ctx + FK_ST_CONN);
        DWORD send = *(DWORD*)(ctx + FK_ST_SENDING);
        DWORD rxst = *(DWORD*)(ctx + FK_ST_RXSTATE);
        DWORD rxmsg = 0xFFFFFFFF, rxseq = 0xFFFFFFFF;
        BYTE* rx = (BYTE*)safe_ptr(ctx + FK_ST_RXBUF);
        if (rx && !IsBadReadPtr(rx, 0x14)) {
            rxmsg = *(WORD*)(rx + 6);
            rxseq = *(DWORD*)(rx + 0x10);
        }

        // Walk the list the way 0x6119A240 does, but bounded -- a corrupt or
        // circular list must not hang the watcher inside the game's process.
        DWORD depth = 0, tail4 = 0;
        void* node = safe_ptr(ctx + FK_ST_QUEUE);
        while (node && !IsBadReadPtr(node, 8) && depth < 64) {
            void* next = *(void**)node;
            if (!next) { tail4 = *(DWORD*)((BYTE*)node + 4); break; }
            node = next;
            depth++;
        }

        // The login object, identified by its vtable rather than by a guessed
        // offset. Re-searched each tick: it is constructed some time after the
        // network context exists, so a single search at startup would miss it.
        BYTE* login = NULL;
        DWORD login_at = 0;
        for (DWORD o = FK_SCAN_LO; o < FK_SCAN_HI && !login; o += 4) {
            BYTE* cand = (BYTE*)safe_ptr((BYTE*)holder + o);
            if (!cand || IsBadReadPtr(cand, 4)) continue;
            if (*(DWORD*)cand == FK_LOGIN_VTABLE) { login = cand; login_at = o; }
        }
        DWORD state = 0xFFFFFFFF;
        if (login && !IsBadReadPtr(login + FK_LOGIN_STATE, 1))
            state = *(BYTE*)(login + FK_LOGIN_STATE);

        if (!announced) {
            announced = 1;
            if (login)
                logf("[fmokey] login object found at globals+0x%X (vtable %08X "
                     "matches the ctor at 0x61175DEB) -- state byte is +0x%X",
                     login_at, FK_LOGIN_VTABLE, FK_LOGIN_STATE);
            else
                logf("[fmokey] login object NOT found: no pointer in "
                     "globals+0x%X..0x%X targets vtable %08X. Reporting no state "
                     "rather than a number from the wrong object.",
                     FK_SCAN_LO, FK_SCAN_HI, FK_LOGIN_VTABLE);
            logf("[fmokey] gate watcher live: ctx=%p (FMO+0x%X -> +0x%X). "
                 "Reporting the three inputs to 0x6119A380 on change.",
                 ctx, FK_FMO_NETOBJ_RVA, FK_NET_OFF);
        }
        if (conn == last_conn && send == last_send &&
            depth == last_depth && tail4 == last_tail4 && state == last_state &&
            rxst == last_rxst && rxmsg == last_rxmsg && rxseq == last_rxseq)
            continue;
        last_conn = conn; last_send = send;
        last_depth = depth; last_tail4 = tail4; last_state = state;
        last_rxst = rxst; last_rxmsg = rxmsg; last_rxseq = rxseq;

        // Say which condition is false, not just the numbers -- the whole point
        // is to stop this being guessed from a silent client.
        const char* verdict =
            (conn != 3)   ? "BLOCKED: not connected (ctx+0x20 != 3)" :
            (send != 0)   ? "BLOCKED: a send is in flight (ctx+0x24 != 0)" :
            (tail4 != 0)  ? "BLOCKED: the ctx+0x7A0E queue is not drained" :
                            "OPEN -- 0x13B should go out";
        // state 3 is the handler that sends 0x13B (jump table 0x6117A274).
        char st[64];
        if (state == 0xFFFFFFFF) wsprintfA(st, "login_state=? (object not found)");
        else wsprintfA(st, "login_state=%lu%s", state,
                       state == 3 ? " <-- the 0x13B state" : "");
        logf("[fmokey] 0x13B gate: conn=%lu send=%lu queue_depth=%lu tail+4=%08lX"
             " %s | rx_state=%lu rx_msg=0x%04lX rx_seq=0x%04lX -> %s",
             conn, send, depth, tail4, st, rxst, rxmsg, rxseq, verdict);
    }
}

static bool patch_slot(DWORD* slot, void* hook, void** out_orig, const char* what)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old)) {
        logf("[fmokey] VirtualProtect failed on the %s slot -- not hooked", what);
        return false;
    }
    if (out_orig) *out_orig = (void*)(*slot);
    *slot = (DWORD)(ULONG_PTR)hook;
    VirtualProtect(slot, sizeof(DWORD), old, &old);
    return true;
}

void fmokey_configure(const wchar_t* ini)
{
    // Default 0. Every other module here defaults its feature ON; this one does
    // not, because it writes key material into a log that [logship] can POST.
    // WARNING: Two defaults per setting normally: the literal AND the iniheal row.
    // This one DELIBERATELY HAS NO iniheal ROW, which is the one case where that
    // is correct: the compiled default is OFF and off is the intended state, so
    // there is nothing for a stale install to heal INTO, and a checkbox that
    // turns on key logging does not belong in the settings dialog. Do not "fix"
    // this by adding a row. The ini-defaults check passes as-is.
    g_enable = GetPrivateProfileIntW(L"fmokey", L"enable", 0, ini);
    // Separate switch: the gate watcher logs no key material, so it is safe to
    // leave on for a session where `enable` is off.
    g_watch_gate = GetPrivateProfileIntW(L"fmokey", L"watch_gate", 0, ini);

    // Started HERE, not from a module callback -- the thread finds FMO itself.
    if (g_watch_gate && !g_watch_thread) {
        g_watch_thread = CreateThread(NULL, 0, gate_watch, NULL, 0, NULL);
        logf("[fmokey] gate watcher started; waiting for FrontMissionOnline.dll "
             "(read-only -- it patches and detours NOTHING)");
    }
}

// Signal-only, no join: called from DllMain's FreeLibrary detach path (loader lock
// held), so it must not wait on the daemon -- the same rule as maskguard_stop. The
// watcher checks g_watch_stop at the top of both its loops and returns; on process
// exit it is reaped anyway.
void fmokey_stop(void) { InterlockedExchange(&g_watch_stop, 1); }

int fmokey_enabled() { return g_enable; }
int fmokey_wants_modules() { return g_enable; }   // the watcher no longer needs callbacks

void fmokey_apply_module(void* module_base, const char* module)
{
    if (!module_base || !module) return;

    if (!g_enable || g_hooked) return;
    if (_stricmp(module, "polcore.dll") != 0) return;

    BYTE* base = (BYTE*)module_base;
    DWORD* table = (DWORD*)(base + FK_CFT_RVA);
    if (IsBadReadPtr(table, FK_SLOT_KEY + sizeof(DWORD))) {
        logf("[fmokey] polcore+0x%X is not readable -- common function table not "
             "found, nothing hooked", FK_CFT_RVA);
        return;
    }

    DWORD* slot_key  = (DWORD*)((BYTE*)table + FK_SLOT_KEY);
    DWORD* slot_auth = (DWORD*)((BYTE*)table + FK_SLOT_AUTH);
    DWORD want_key  = (DWORD)(ULONG_PTR)(base + FK_FN_KEY_RVA);
    DWORD want_auth = (DWORD)(ULONG_PTR)(base + FK_FN_AUTH_RVA);

    // Verify before writing, exactly like polfetch.cpp and patches.cpp: a build
    // whose table does not hold what we expect is left alone, not corrupted.
    if (*slot_key != want_key || *slot_auth != want_auth) {
        logf("[fmokey] common function table does NOT hold what this build "
             "expects -- slot 1003 = %08lX (expected %08lX), slot 936 = %08lX "
             "(expected %08lX). Nothing hooked; the FMO key tap is INACTIVE.",
             *slot_key, want_key, *slot_auth, want_auth);
        logf("[fmokey]   either this is a different polcore build (compare "
             "against the CFT table) or something else already "
             "swapped these slots.");
        return;
    }

    bool a = patch_slot(slot_key,  (void*)hook_key,  (void**)&orig_key,  "key");
    bool b = patch_slot(slot_auth, (void*)hook_auth, (void**)&orig_auth, "auth");
    if (!a || !b) return;
    g_hooked = true;
    logf("[fmokey] tapped polcore common-function-table slots 1003 (key16, "
         "+0x%X) and 936 (auth52, +0x%X). LOGGING ONLY -- no behaviour change. "
         "WARNING: key material goes to this log.",
         FK_SLOT_KEY, FK_SLOT_AUTH);
}

void fmokey_summary()
{
    if (!g_enable) return;
    logf("[fmokey] summary: hooked=%d key16 reads=%ld auth52 reads=%ld",
         g_hooked ? 1 : 0, g_n_key, g_n_auth);
    if (!g_n_key)
        logf("[fmokey]   no key read -- FMO never reached its world login, so "
             "this run says nothing about whether the key is derivable.");
}
