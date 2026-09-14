// patchver.cpp -- write a CONSISTENT (patch.ver, Interface\NNNN) pair, so registration
// repair can actually fix a title instead of telling you to run a script.
//
// THE PROBLEM
//
// Interface\NNNN is the install-time nonce that keys a title's 288-byte patch.ver.
// regfix can synthesise InstallFolder and the COM GUIDs from constants, but not this --
// so a repaired title appeared in the menu and then reported "not installed" or
// "version unknown", which is where the Deck kept landing.
//
// THE SHORTCUT (from the server's tools/patchver.py header, reversed from sqexca.dll)
//
//     "That value is just sprintf("%08x", timeGetTime()) captured at install time --
//      an install nonce, not a version. ANY VALUE WORKS as long as the blob is
//      encrypted with the same one."
//
// So we do not need to RECOVER the original nonce (an algebraic solve). We pick a key,
// build a fresh blob carrying the right version string, encrypt it with that key, and
// write both. The pair is self-consistent, which is the only property the client checks.
//
// This is safe precisely BECAUSE Interface is missing: without it the client cannot
// decrypt the existing patch.ver at all, so that file is already dead weight. We still
// back it up to patch.ver.bak-polshim before replacing it, and we never touch a title
// whose Interface value is present.
//
// The cipher is ported from patchver.py, which is validated against real installed
// files; patchvertest.exe re-checks the port by round-tripping and by reproducing a
// known (key, version) pair.

#include "polshim.h"

#define BLOB_LEN    0x120       // 288 bytes on disk
#define CIPHER_LEN  0x118       // only the first 280 are enciphered
#define TEXT_OFF    24          // the version string lives here

typedef unsigned __int64 u64;
typedef unsigned int     u32;

static u32 rol32(u32 v, int n) { return (u32)((v << n) | (v >> (32 - n))); }
static u32 ror32(u32 v, int n) { return (u32)((v >> n) | (v << (32 - n))); }

// acc[i & 7] += s[i]. For the usual 8-character key this is just the ASCII bytes.
u64 patchver_key64(const char* keystring)
{
    unsigned char acc[8] = { 0 };
    for (int i = 0; keystring[i]; i++) acc[i & 7] = (unsigned char)(acc[i & 7] + (unsigned char)keystring[i]);
    u64 k = 0;
    for (int i = 7; i >= 0; i--) k = (k << 8) | acc[i];
    return k;
}

u64 patchver_seed64(int content_id, const char* keystring)
{
    return (u64)((u64)content_id + patchver_key64(keystring));
}

static void state_init(u64 seed, unsigned char s[0x100])
{
    memset(s, 0, 0x100);
    u32 lo = (u32)(seed & 0xffffffffu), hi = (u32)(seed >> 32);
    u32 a = ror32(hi, 16), b = rol32(lo, 8);
    memcpy(s + 0, &a, 4);
    memcpy(s + 4, &b, 4);
    s[0] = (unsigned char)(s[0] + 0x45);
    for (int i = 1; i < 8; i++) {
        unsigned char prev = s[i - 1];
        unsigned char t = (unsigned char)(s[i] + (unsigned char)(prev - 0x2c));
        s[i] = (unsigned char)(((unsigned char)(prev << 2)) ^ t ^ 0x45);
    }
    for (int i = 1; i < 32; i++) {
        u64 prev; memcpy(&prev, s + (i - 1) * 8, 8);
        u64 cur = prev * 5;
        memcpy(s + i * 8, &cur, 8);
    }
}

static u64 mix_f(u64 ctr)
{
    u64 v = ctr, x = ctr;
    for (int i = 0; i < 3; i++) v = (v << 10) | x;
    return v + 0xA1652347ull;
}

// SBOX[i] = i + 0x88 -- a permutation, so the inverse is a subtraction.
void patchver_encrypt(unsigned char* buf, size_t len, u64 seed)
{
    unsigned char s[0x100];
    state_init(seed, s);
    for (size_t i = 0; i < len / 8; i++) {
        u64 ctr = i * 8; int k = (int)(i & 0x1f);
        const unsigned char* qb = s + k * 8;
        u64 q; memcpy(&q, s + k * 8, 8);

        unsigned char blk[8];
        memcpy(blk, buf + i * 8, 8);
        u32 lo, hi;
        memcpy(&lo, blk, 4); memcpy(&hi, blk + 4, 4);
        memcpy(blk, &hi, 4); memcpy(blk + 4, &lo, 4);       // swap the two dwords
        u64 v; memcpy(&v, blk, 8);
        v = ((v ^ q) ^ mix_f(ctr)) + q;
        memcpy(blk, &v, 8);

        unsigned char d = (unsigned char)((i ^ 0x45) & 0xff);
        for (int j = 0; j < 8; j++) {
            unsigned char bb = blk[j];
            for (int t = 0; t < 8; t++) bb = (unsigned char)((qb[t] + bb) + 0x88);
            bb ^= d;
            blk[j] = bb;
            d = bb;
        }
        memcpy(buf + i * 8, blk, 8);
    }
}

void patchver_decrypt(unsigned char* buf, size_t len, u64 seed)
{
    unsigned char s[0x100];
    state_init(seed, s);
    for (size_t i = 0; i < len / 8; i++) {
        u64 ctr = i * 8; int k = (int)(i & 0x1f);
        const unsigned char* qb = s + k * 8;
        u64 q; memcpy(&q, s + k * 8, 8);

        unsigned char blk[8];
        memcpy(blk, buf + i * 8, 8);
        unsigned char d = (unsigned char)((i ^ 0x45) & 0xff);
        for (int j = 0; j < 8; j++) {
            unsigned char c = blk[j];
            unsigned char bb = (unsigned char)(c ^ d);
            for (int t = 7; t >= 0; t--) bb = (unsigned char)((bb - 0x88) - qb[t]);
            blk[j] = bb;
            d = c;
        }
        u64 v; memcpy(&v, blk, 8);
        v = ((v - q) ^ mix_f(ctr)) ^ q;
        memcpy(blk, &v, 8);
        u32 lo, hi;
        memcpy(&lo, blk, 4); memcpy(&hi, blk + 4, 4);
        memcpy(blk, &hi, 4); memcpy(blk + 4, &lo, 4);
        memcpy(buf + i * 8, blk, 8);
    }
}

// Build the full 288-byte on-disk blob for `text` under `keystring`.
void patchver_make_blob(const char* text, int content_id, const char* keystring,
                        unsigned char out[BLOB_LEN])
{
    memset(out, 0, BLOB_LEN);
    size_t n = strlen(text);
    if (n > 0xFF) n = 0xFF;                 // sqexca rejects longer
    memcpy(out + TEXT_OFF, text, n);
    patchver_encrypt(out, CIPHER_LEN, patchver_seed64(content_id, keystring));
}

// Read a blob back -- used by the self-test, and to CHECK our own write.
bool patchver_read_blob(const unsigned char in[BLOB_LEN], int content_id,
                        const char* keystring, char* text, size_t cch)
{
    unsigned char tmp[BLOB_LEN];
    memcpy(tmp, in, BLOB_LEN);
    patchver_decrypt(tmp, CIPHER_LEN, patchver_seed64(content_id, keystring));
    // A correct key leaves the buffer zero-filled bar the string at TEXT_OFF; that is
    // the property the self-test asserts, and the cheapest "did this key fit" check.
    size_t i = 0;
    while (i < cch - 1 && TEXT_OFF + i < CIPHER_LEN && tmp[TEXT_OFF + i] >= 0x20 && tmp[TEXT_OFF + i] < 0x7f) {
        text[i] = (char)tmp[TEXT_OFF + i]; i++;
    }
    text[i] = 0;
    int nonzero = 0;
    for (size_t j = 0; j < CIPHER_LEN; j++) {
        if (j >= TEXT_OFF && j < TEXT_OFF + i) continue;
        if (tmp[j]) nonzero++;
    }
    return i > 0 && nonzero == 0;
}
