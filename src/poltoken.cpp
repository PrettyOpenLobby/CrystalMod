// poltoken.cpp -- bind an FFXI launch to the PlayOnline session that started it.
//
// THE PROBLEM
//
// The server's FFXI bridge has to decide which POL member
// a game connection belongs to, because the LSB account it authenticates as
// decides which characters the player is shown. **FFXI's own lobby stream names
// nobody**: `0x26` is version and expansions (its only high-entropy field is
// uninitialised client memory -- FFXiMain pointers and all), `0x1F` is a bare
// poke, and the first packet carrying a Content ID is `0x07`, the character
// SELECT, which arrives long after the character list has been built.
//
// So the bridge inferred it, and inference failed three separate ways in the
// field: by peer address (every client shares one behind NAT), by recency (a
// stale session outranked the live one), and by a "signed in" flag (it decayed
// for the active user). Each wrong guess put a player on someone else's account;
// two characters were created on accounts that never asked for them.
//
// THE FIX, AND WHY IT LIVES HERE
//
// PlayOnline already identifies a launch, and it does not use the address: the
// client sends `USER x 8 * :<48 chars>` on every hop of a login, that token is
// unique per launch, and responders.py hashes it into the session id that all of
// POL's per-session state hangs off (`_sid_for_user_token`).
//
// **This process sends that token AND opens the FFXI lobby socket** -- FFXiMain
// runs inside pol.exe -- so it is the one place where the two can be tied
// together with no inference at all. We capture the token on its way out, hash
// it exactly as POL does, and stamp the result into the FFXI lobby packets.
//
// WHERE THE STAMP GOES, AND WHY IT IS FREE
//
// Bytes [12:28] of a lobby packet are the `identifer` field. On the way to the
// server the bridge OVERWRITES it with LSB's session hash (that is how xiloader
// works and how the bridge impersonates it), so whatever the client puts there
// is discarded before LSB ever sees it. It is therefore a free 16-byte channel
// between the shim and the bridge, needing no new connection, no new port, and
// no change to LSB or to the packet's size or checksum.
//
//     +0   "POLS"                magic, so an untagged client is unambiguous
//     +4   sha1(USER token)[0:8] -> POL's session id is "u" + these bytes in hex
//     +12  zero                  reserved
//
// ...EXCEPT AGAINST REAL SQUARE ENIX, WHERE THAT FIELD IS THE PACKET'S CHECKSUM.
// This file used to claim the stamp needed "no change to the packet's size or
// checksum". The size half is right; the checksum half was exactly backwards.
// Bytes [12:28] ARE the checksum -- MD5 of the whole packet with those 16 bytes
// zeroed -- and LSB simply never verifies it. See the scope guard in
// poltoken_tag() for the measurement and for what SE does about it.
//
// LIMITS -- read before trusting it
//
//   * This is ATTRIBUTION, not authentication. It binds a launch to a POL
//     session; it does not prove who the human is. A client that knew another
//     player's USER token could stamp it -- but that token is high-entropy and
//     per-launch, so this is exactly as strong as the POL session itself, and no
//     weaker than what POL already relies on.
//   * **PC only.** The PS2 Viewer cannot load this DLL, so PS2 clients are never
//     tagged. They fall back to the bridge's address match, which is exact in
//     production (host networking, real client addresses) -- so PS2 is covered
//     where it matters and only loses determinism on a NAT'd dev box.
//   * Strictly additive: no tag means the bridge behaves exactly as before.
#include "polshim.h"

#include <string.h>

// ---------------------------------------------------------------------------
// SHA-1. Implemented here rather than through wincrypt so this links nothing
// new, and so the digest can be proven identical to Python's `hashlib.sha1`
// (which is what POL uses) by the self-test at the bottom of this file.
// ---------------------------------------------------------------------------
namespace {

struct Sha1 {
    unsigned int  h[5];
    unsigned long long len;
    unsigned char buf[64];
    unsigned int  n;
};

inline unsigned int rol(unsigned int v, int b) { return (v << b) | (v >> (32 - b)); }

void sha1_block(Sha1* c, const unsigned char* p)
{
    unsigned int w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    unsigned int a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        unsigned int k, t;
        if      (i < 20) { t = (b & d) | (~b & e);           k = 0x5A827999u; }
        else if (i < 40) { t = b ^ d ^ e;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);  k = 0x8F1BBCDCu; }
        else             { t = b ^ d ^ e;                    k = 0xCA62C1D6u; }
        unsigned int tmp = rol(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

void sha1_init(Sha1* c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
}

void sha1_update(Sha1* c, const unsigned char* p, size_t n)
{
    c->len += (unsigned long long)n * 8;
    while (n) {
        unsigned int take = 64 - c->n;
        if (take > n) take = (unsigned int)n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

void sha1_final(Sha1* c, unsigned char out[20])
{
    unsigned long long bits = c->len;
    unsigned char pad = 0x80;
    sha1_update(c, &pad, 1);
    unsigned char z = 0;
    while (c->n != 56) sha1_update(c, &z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - i * 8));
    // Length is appended directly: going through sha1_update would add to c->len.
    memcpy(c->buf + 56, lb, 8);
    sha1_block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

}  // namespace

// ---------------------------------------------------------------------------

static int            g_enable   = 1;
static bool           g_have     = false;
static unsigned char  g_sid[8]   = {0};
static long           g_tagged   = 0;
static long           g_refused_se = 0;   // SE-bypass refusals, for the once-only log
static CRITICAL_SECTION g_cs;
// One-time, thread-safe init. The old lazy `if (!g_cs_ready) Initialize...` ran in
// two places with no interlock, so two threads could double-initialize the CS; and
// poltoken_tag bailed entirely when it saw the flag unset. InitOnceExecuteOnce
// guarantees exactly-one init with no spin, and every entry point calls ensure_cs().
static INIT_ONCE      g_cs_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK  cs_init_cb(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&g_cs); return TRUE; }
static void           ensure_cs() { InitOnceExecuteOnce(&g_cs_once, cs_init_cb, NULL, NULL); }

//: The stamp's magic. Four bytes so an untagged client can never be mistaken for
//: a tagged one by accident -- the bridge requires it before reading anything.
static const char POLTOKEN_MAGIC[4] = {'P', 'O', 'L', 'S'};

void poltoken_configure(const wchar_t* ini)
{
    ensure_cs();
    g_enable = GetPrivateProfileIntW(L"poltoken", L"enable", 1, ini);
    logf("[poltoken] %s -- %s", g_enable ? "on" : "off",
         g_enable ? "FFXI launches will carry the PlayOnline session id, so the "
                    "bridge never has to guess which member launched"
                  : "FFXI launches will be attributed by the bridge's own "
                    "heuristics (see [poltoken] enable)");
}

// Every outgoing buffer passes through here. Two shapes matter; everything else
// is ignored without copying.
void poltoken_note_send(const char* buf, int len)
{
    if (!g_enable || !buf || len < 7) return;
    // `USER x 8 * :<token>` -- cleartext on the auth band, on every hop of a
    // login. POL takes everything after the FIRST colon and strips it, so this
    // must too or the digests will not agree.
    if (memcmp(buf, "USER ", 5) != 0) return;
    const char* colon = (const char*)memchr(buf, ':', (size_t)len);
    if (!colon) return;
    const char* s = colon + 1;
    const char* e = buf + len;
    while (s < e && (unsigned char)*s <= ' ') s++;
    while (e > s && (unsigned char)e[-1] <= ' ') e--;
    if (e <= s) return;

    unsigned char dig[20];
    Sha1 c; sha1_init(&c);
    sha1_update(&c, (const unsigned char*)s, (size_t)(e - s));
    sha1_final(&c, dig);

    ensure_cs();
    EnterCriticalSection(&g_cs);
    bool changed = !g_have || memcmp(g_sid, dig, 8) != 0;
    memcpy(g_sid, dig, 8);
    g_have = true;
    LeaveCriticalSection(&g_cs);

    if (changed) {
        char hex[17];
        for (int i = 0; i < 8; i++) {
            static const char* H = "0123456789abcdef";
            hex[i * 2] = H[dig[i] >> 4];
            hex[i * 2 + 1] = H[dig[i] & 15];
        }
        hex[16] = 0;
        logf("[poltoken] this launch is PlayOnline session u%s (%d-byte USER "
             "token); FFXI lobby packets will carry it", hex, (int)(e - s));
    }
}

// Stamp an FFXI lobby packet. Returns 1 and fills `out` when it rewrote, 0 to
// send the original untouched.
//
// The packet is identified by SHAPE, not by socket: a 4-byte little-endian size
// equal to the buffer length, followed by the magic `IXFF`. That is exactly the
// lobby framing and nothing else in this client looks like it, so no socket
// bookkeeping (and no dependence on which port the bridge happens to be on) is
// needed.
int poltoken_tag(const char* buf, int len, char* out, int outcap)
{
    if (!g_enable || !buf || len < 28 || len > outcap) return 0;
    if (memcmp(buf + 4, "IXFF", 4) != 0) return 0;
    unsigned int size = (unsigned char)buf[0] | ((unsigned char)buf[1] << 8) |
                        ((unsigned char)buf[2] << 16) | ((unsigned char)buf[3] << 24);
    if (size != (unsigned int)len) return 0;

    // REAL SQUARE ENIX VALIDATES BYTES [12:28]. They are not a free field there:
    // they are MD5(packet with those 16 bytes zeroed). Measured in BOTH directions
    // from one live SE session (2026-08-25, polshim.1056480.log, FFXI lobby
    // 124.150.154.122:54001): the client's 152-byte cmd 0x26 hashed to the field it
    // carried, and SE's own 36-byte reply hashed to the field IT carried. LSB never
    // checks it and the bridge overwrites it on the way in, which is why the stamp
    // is free against our server and invisible in every test we have -- but SE
    // answered the stamped packet with cmd 0x04, code 329, which the client shows
    // as FFXI-3329 "Transmission Packet Error #3", and the link went dead before
    // the character list.
    //
    // There is no version of this that both stamps and stays valid: the stamp lands
    // IN the checksum. So SE-bypass wins -- the same scope guard authkey.cpp uses,
    // for the same reason -- and refusing costs nothing, because an FFXI session
    // against Square Enix never reaches our bridge and has nothing to attribute.
    if (netredir_se_bypass()) {
        if (InterlockedIncrement(&g_refused_se) == 1)
            logf("[poltoken] NOT stamping FFXI lobby packets: this install is "
                 "pointed at REAL Square Enix ([redirect] enable=1), where bytes "
                 "[12:28] are the packet's own MD5 -- overwriting them is FFXI-3329");
        return 0;
    }

    ensure_cs();
    EnterCriticalSection(&g_cs);
    bool have = g_have;
    unsigned char sid[8];
    memcpy(sid, g_sid, 8);
    LeaveCriticalSection(&g_cs);
    if (!have) return 0;          // no token seen yet: send it as the client wrote it

    memcpy(out, buf, (size_t)len);
    memcpy(out + 12, POLTOKEN_MAGIC, 4);
    memcpy(out + 16, sid, 8);
    memset(out + 24, 0, 4);
    long n = InterlockedIncrement(&g_tagged);
    if (n == 1)
        logf("[poltoken] stamped the first FFXI lobby packet (cmd 0x%02x) -- the "
             "bridge can now attribute this launch without guessing",
             (unsigned char)buf[8]);
    return 1;
}

// The "u"+16-hex session-id form for consumers OUTSIDE the lobby packet path
// (the settings dialog's character import states it as an HTTP header, and the
// bridge resolves it through the same member_for_sid the launch path uses).
// Returns 0 with out[0]=0 when no USER token has been seen this process --
// i.e. nobody has signed into PlayOnline yet.
int poltoken_session_hex(char out[17])
{
    ensure_cs();
    EnterCriticalSection(&g_cs);
    bool have = g_have;
    unsigned char sid[8];
    memcpy(sid, g_sid, 8);
    LeaveCriticalSection(&g_cs);
    if (!have) { out[0] = 0; return 0; }
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i * 2]     = H[sid[i] >> 4];
        out[i * 2 + 1] = H[sid[i] & 15];
    }
    out[16] = 0;
    return 1;
}

int poltoken_selftest(void)
{
    int fails = 0;
    // 1. SHA-1 against the standard vectors, because the whole scheme rests on
    //    this digest agreeing with Python's hashlib byte for byte.
    struct { const char* in; const char* want; } V[] = {
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"The quick brown fox jumps over the lazy dog",
         "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"},
    };
    for (int i = 0; i < 3; i++) {
        unsigned char d[20]; char hex[41];
        Sha1 c; sha1_init(&c);
        sha1_update(&c, (const unsigned char*)V[i].in, strlen(V[i].in));
        sha1_final(&c, d);
        for (int j = 0; j < 20; j++) {
            static const char* H = "0123456789abcdef";
            hex[j * 2] = H[d[j] >> 4]; hex[j * 2 + 1] = H[d[j] & 15];
        }
        hex[40] = 0;
        if (strcmp(hex, V[i].want) != 0) {
            logf("[poltoken] SELFTEST FAIL sha1(%s) = %s, want %s",
                 V[i].in, hex, V[i].want);
            fails++;
        }
    }
    // 2. A USER line is parsed the way responders.py parses it, and the stamp
    //    lands in the identifer field without disturbing anything else.
    g_enable = 1; g_have = false;
    ensure_cs();
    const char* user = "USER x 8 * :abcdefghijklmnopqrstuvwxyz012345\r\n";
    poltoken_note_send(user, (int)strlen(user));
    if (!g_have) { logf("[poltoken] SELFTEST FAIL: USER token not captured"); fails++; }

    char pkt[64], out[64];
    memset(pkt, 0xAA, sizeof(pkt));
    int len = 40;
    pkt[0] = (char)len; pkt[1] = pkt[2] = pkt[3] = 0;
    memcpy(pkt + 4, "IXFF", 4);
    pkt[8] = 0x26;
    if (!poltoken_tag(pkt, len, out, sizeof(out))) {
        logf("[poltoken] SELFTEST FAIL: a well-formed IXFF packet was not stamped");
        fails++;
    } else {
        if (memcmp(out + 12, POLTOKEN_MAGIC, 4) != 0) {
            logf("[poltoken] SELFTEST FAIL: magic missing"); fails++;
        }
        if (memcmp(out, pkt, 12) != 0 || memcmp(out + 28, pkt + 28, len - 28) != 0) {
            logf("[poltoken] SELFTEST FAIL: bytes outside [12:28] were disturbed");
            fails++;
        }
    }
    // 3. Anything that is not a lobby packet is left alone.
    const char* junk = "not a lobby packet at all, definitely not IXFF framed..";
    if (poltoken_tag(junk, (int)strlen(junk), out, sizeof(out))) {
        logf("[poltoken] SELFTEST FAIL: a non-IXFF buffer was rewritten"); fails++;
    }
    logf("[poltoken] selftest %s", fails ? "FAILED" : "passed");
    return fails;
}
