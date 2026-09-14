// fmoiid.cpp -- re-mint Front Mission Online's interface GUIDs from JP to US, ON DISK.
//
// WHY THIS IS IN THE SHIM AND NOT ONLY IN A SCRIPT
//
// An offline script (patch_fmo_iids.py) has done this since 2026-08-13 -- but a script is something somebody runs on ONE machine. A second
// machine that installs FMO for itself gets the stock JP-IID DLL, and the patch bundle
// is NOT the backstop it was believed to be: the patched DLL is published into W20-0004
// as version `20060823_1`, while that tree's LATEST is `20260813_1` (the translation
// overlay published the same day). A client whose patch.ver already claims the latest is
// answered "registered, you are current" and downloads nothing -- so it never passes
// through `20060823_1` and keeps the original DLL for ever.
//
// Measured 2026-08-18 on the second Windows machine, both halves of that:
//     cmd7 W2U/0004 ver='20260813_1' -> registered, latest=20260813_1
//     ZERO cmd3 (ranged read) requests on the entire patch service
// i.e. the version check passed, nothing was served, and FMO then failed to launch.
//
// So the repair has to be something the CLIENT does for itself, next to the other
// per-title repairs in regfix.cpp. Same shape as repair_patchver: survey first, keep a
// backup, verify our own output before it touches the disk, then write.
//
// WHAT IT DOES -- transcribed from patch_fmo_iids.py, NOT re-derived
//
// FMO is a 2005 JP-era COM content component. The US Viewer is ABI-compatible with it
// and GUID-incompatible, which shows up as two separate failures:
//   1. "Class not registered" on Play -- pol.exe hardcodes IPolContentsCom and FMO
//      implements IFMOEntry, so the QI fails.
//   2. A crash just after GameStart -- FMO QIs the polcore it is handed for JP-era IIDs,
//      and US polcore refuses them while implementing those same interfaces under
//      different GUIDs.
// Both are the same disease, and substituting five GUIDs fixes both at once. That is why
// this is worth doing even though `contentiid_alias` exists: the alias only ever covered
// failure 1. US and JP polcore.dll are the SAME BUILD differing in 181 bytes, all GUIDs,
// at identical file offsets -- which is where the pairing below came from, for free.
//
// IMPORTANT: THE PACKER TOLERATES THIS, and that was measured, not assumed. FrontMissionOnline.dll
// is POL1-packed (.text has raw size 0 and is produced at load time), so the worry was
// that a .rdata edit would be checksummed or overwritten during unpack. `fmoprobe.exe
// file <path>` against a patched COPY flipped IPolContentsCom from E_NOINTERFACE to S_OK
// and IFMOEntry the other way -- a clean inversion. Every GUID here lives in .rdata or
// .rsrc on disk; none is inside the packed section.
//
// IMPORTANT: file.txt MUST MOVE IN THE SAME STEP. The install tree is self-checking: file.txt
// holds a 22-char digest per file, and a mismatch lets Check Files re-download
// FrontMissionOnline.dll from the patch server and SILENTLY REVERT the patch. Patching
// the DLL without the manifest is worse than not patching it, because the revert happens
// later and looks like the patch never worked.

#include "polshim.h"
#include <bcrypt.h>

// The DLL this was derived against. Anything else is refused unless forced: a different
// FMO build could hold these GUIDs at other sites, or hold different ones entirely.
static const char* FMO_ORIG_DIGEST = "xJDVS6u@auQ9rldY9HRtO7";
static const unsigned FMO_ORIG_SIZE = 2766848;

static const char* FMO_DLL_NAME = "FrontMissionOnline.dll";

// (JP, US). The first pair is what FMO IMPLEMENTS; the rest are what it ASKS polcore for.
static const struct { const char* jp; const char* us; const char* what; } SUBS[] = {
    { "2031D0EF-97FA-48AE-A3FD-8260C380029A", "6D365D27-4999-4BC5-AADF-513EF0E7B438",
      "IFMOEntry -> IPolContentsCom (what pol.exe asks FMO for)" },
    { "9A30D565-A74C-4B56-B971-DCF02185B10D", "E0516654-EF77-435D-AA7D-50D2C069CE34",
      "polcore IID #1 (the one the crash was on)" },
    { "B5D910B2-9F60-4E63-B3BB-D815A18FD33E", "0B197CB5-DAFD-48B8-97D2-71E0A776AE4C",
      "polcore IID #2" },
    { "9FD503A1-8F65-4665-9941-8219360E6981", "3B0B8E16-C984-4792-ADD7-C23C75127DFD",
      "polcore IID #3" },
    { "07974581-0DF6-4EF0-BD05-604B3ADA9BE9", "3501F5DD-7894-42DF-866A-A2B6527D8049",
      "polcore IID #4 (also a CLSID)" },
};

// --- the manifest digest ----------------------------------------------------------
//
// base64(MD5) MSB-first over a substituted 64-symbol alphabet: 21 full 6-bit groups
// (126 bits) plus the trailing 2 bits LEFT-aligned in a final group, so character 22
// only ever takes ALPHA[0/16/32/48]. Verified against all 1536 entries of the USA
// 1.10.00c file.txt with 0 mismatches -- a reference Python implementation (polhash.py) owns the
// algorithm and this is a transcription of it, not a second derivation.
static const char POL_ALPHA[] =
    "TSG8IncW3HFKokOg79qzeCmZs2yBYEQVAUxR5rbwi4P@jMDLtpvad0f_J1hlN6uX";

static bool md5_bytes(const void* data, size_t len, unsigned char out[16])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0) != 0) return false;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        // BCryptHashData takes a ULONG; the DLL is ~2.7 MB so this never truncates,
        // but hash in chunks anyway rather than leave a silent cast on a size_t.
        ok = true;
        const unsigned char* p = (const unsigned char*)data;
        size_t left = len;
        while (ok && left) {
            ULONG n = (ULONG)(left > 0x10000000u ? 0x10000000u : left);
            ok = BCryptHashData(h, (PUCHAR)p, n, 0) == 0;
            p += n; left -= n;
        }
        if (ok) ok = BCryptFinishHash(h, out, 16, 0) == 0;
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// 6 bits starting at `bitpos`, MSB-first across the 16-byte digest.
static unsigned bits6(const unsigned char* d, int bitpos)
{
    unsigned v = 0;
    for (int k = 0; k < 6; k++) {
        int b = bitpos + k;
        v = (v << 1) | ((d[b >> 3] >> (7 - (b & 7))) & 1);
    }
    return v;
}

bool pol_digest(const void* data, size_t len, char out[23])
{
    unsigned char md[16];
    if (!md5_bytes(data, len, md)) { out[0] = 0; return false; }
    for (int i = 0; i < 21; i++) out[i] = POL_ALPHA[bits6(md, 6 * i)];
    out[21] = POL_ALPHA[(md[15] & 0x03) << 4];   // the 2-bit tail, LEFT-aligned
    out[22] = 0;
    return true;
}

// --- GUID text -> the 16 bytes as they appear in a binary --------------------------
//
// Little-endian for the first three fields, big-endian for the last eight -- what
// Python calls UUID.bytes_le, and what a compiler emits for a GUID literal.
static bool guid_le(const char* s, unsigned char out[16])
{
    unsigned b[16]; int n = 0;
    for (const char* p = s; *p && n < 16; ) {
        if (*p == '-' || *p == '{' || *p == '}') { p++; continue; }
        unsigned hi, lo;
        const char* q = p;
        for (int k = 0; k < 2; k++) {
            char c = q[k];
            unsigned v;
            if      (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return false;
            if (k == 0) hi = v; else lo = v;
        }
        b[n++] = (hi << 4) | lo;
        p = q + 2;
    }
    if (n != 16) return false;
    out[0] = (unsigned char)b[3];  out[1] = (unsigned char)b[2];      // u32, LE
    out[2] = (unsigned char)b[1];  out[3] = (unsigned char)b[0];
    out[4] = (unsigned char)b[5];  out[5] = (unsigned char)b[4];      // u16, LE
    out[6] = (unsigned char)b[7];  out[7] = (unsigned char)b[6];      // u16, LE
    for (int i = 8; i < 16; i++) out[i] = (unsigned char)b[i];        // as written
    return true;
}

static int count_bytes(const unsigned char* hay, size_t hlen, const unsigned char* nee, size_t nlen)
{
    int n = 0;
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, nee, nlen) == 0) n++;
    return n;
}

static int replace_bytes(unsigned char* hay, size_t hlen,
                         const unsigned char* from, const unsigned char* to, size_t nlen)
{
    int n = 0;
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, from, nlen) == 0) { memcpy(hay + i, to, nlen); n++; }
    return n;
}

// --- whole-file read/write ---------------------------------------------------------

static unsigned char* read_all(const char* path, size_t* len)
{
    *len = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.HighPart || sz.LowPart == 0) { CloseHandle(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc(sz.LowPart);
    if (!buf) { CloseHandle(f); return NULL; }
    DWORD got = 0;
    bool ok = ReadFile(f, buf, sz.LowPart, &got, NULL) && got == sz.LowPart;
    CloseHandle(f);
    if (!ok) { free(buf); return NULL; }
    *len = got;
    return buf;
}

static bool write_all(const char* path, const void* data, size_t len)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, (DWORD)len, &put, NULL) && put == len;
    CloseHandle(f);
    return ok;
}

// --- file.txt ----------------------------------------------------------------------
//
// One LF-separated line per file, "DIGEST:SIZE:PATH". The SIZE is carried over from the
// existing row rather than recomputed: a GUID is always 16 bytes so the size cannot have
// moved, and reusing SE's own number keeps us from introducing a second opinion about it.
//
// WARNING: NOT a strict parser on purpose. file.txt's last line is a bare "::" terminator, and
// a parser that insists every line has three usable fields dies on a perfectly good
// manifest -- which has caught this tree before.
static bool filetxt_row(const unsigned char* buf, size_t len, size_t* row_off, size_t* row_len)
{
    static const char* tail = ":FrontMissionOnline.dll";
    size_t tl = strlen(tail);
    size_t start = 0; int hits = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && buf[i] != '\n') continue;
        size_t l = i - start;
        while (l && buf[start + l - 1] == '\r') l--;          // tolerate CRLF
        if (l >= tl && memcmp(buf + start + l - tl, tail, tl) == 0) {
            if (++hits == 1) { *row_off = start; *row_len = l; }
        }
        start = i + 1;
    }
    return hits == 1;                                          // 0 or 2+ is not ours to guess at
}

// --- the repair --------------------------------------------------------------------

bool fmoiid_repair(const char* installdir, bool apply, bool force, FmoIidReport* r)
{
    memset(r, 0, sizeof(*r));
    r->state = FMOIID_ERROR;

    char dll[MAX_PATH], ftxt[MAX_PATH], bak[MAX_PATH];
    _snprintf_s(dll,  sizeof(dll),  _TRUNCATE, "%s\\%s", installdir, FMO_DLL_NAME);
    _snprintf_s(ftxt, sizeof(ftxt), _TRUNCATE, "%s\\file.txt", installdir);

    size_t dlen = 0;
    unsigned char* data = read_all(dll, &dlen);
    if (!data) {
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "cannot read %s (%lu)", FMO_DLL_NAME, GetLastError());
        return false;
    }

    unsigned char jpb[_countof(SUBS)][16], usb[_countof(SUBS)][16];
    for (int i = 0; i < _countof(SUBS); i++) {
        if (!guid_le(SUBS[i].jp, jpb[i]) || !guid_le(SUBS[i].us, usb[i])) {
            strcpy_s(r->err, "internal: bad GUID literal in SUBS");   // unreachable
            free(data); return false;
        }
        r->jp += count_bytes(data, dlen, jpb[i], 16);
        r->us += count_bytes(data, dlen, usb[i], 16);
    }
    pol_digest(data, dlen, r->digest_before);
    strcpy_s(r->digest_after, r->digest_before);

    r->state = r->us == 0 ? FMOIID_UNPATCHED
             : r->jp == 0 ? FMOIID_PATCHED
                          : FMOIID_MIXED;

    if (r->state == FMOIID_MIXED) {
        strcpy_s(r->err, "MIXED state -- some sites JP, some US. Restore "
                         "FrontMissionOnline.dll.orig first; this will not guess.");
        free(data); return false;
    }

    // The gate. Only ever checked against the ORIGINAL, because a patched DLL of course
    // no longer has the original digest -- checking it there would refuse our own work.
    if (r->state == FMOIID_UNPATCHED && !force
            && (dlen != FMO_ORIG_SIZE || strcmp(r->digest_before, FMO_ORIG_DIGEST) != 0)) {
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "unexpected build (%u B, digest %s; expected %u B / %s). "
                    "[regfix] fmo_iid_force=1 to patch anyway.",
                    (unsigned)dlen, r->digest_before, FMO_ORIG_SIZE, FMO_ORIG_DIGEST);
        free(data); return false;
    }

    // Read the manifest before deciding there is nothing to do: a DLL that is already
    // patched but whose file.txt row still names the ORIGINAL digest is the dangerous
    // state, not a finished one -- Check Files re-downloads and reverts from there.
    size_t flen = 0, roff = 0, rlen = 0;
    unsigned char* ftext = read_all(ftxt, &flen);
    bool have_row = ftext && filetxt_row(ftext, flen, &roff, &rlen);
    if (have_row) {
        size_t n = rlen; const char* p = (const char*)ftext + roff;
        const char* colon = (const char*)memchr(p, ':', n);
        size_t dl = colon ? (size_t)(colon - p) : 0;
        if (dl && dl < sizeof(r->filetxt_listed)) { memcpy(r->filetxt_listed, p, dl); r->filetxt_listed[dl] = 0; }
    }

    if (r->state == FMOIID_PATCHED && have_row
            && strcmp(r->filetxt_listed, r->digest_before) == 0) {
        free(data); free(ftext);
        return true;                                            // already correct, both halves
    }

    if (!apply) { free(data); free(ftext); return false; }       // dry run: state reported, nothing written

    // --- substitute -----------------------------------------------------------------
    if (r->state == FMOIID_UNPATCHED) {
        for (int i = 0; i < _countof(SUBS); i++)
            r->substituted += replace_bytes(data, dlen, jpb[i], usb[i], 16);

        // Verify our own output BEFORE the disk is touched: every JP site gone, every US
        // site present. A partial substitution must never reach the tree.
        int jp2 = 0, us2 = 0;
        for (int i = 0; i < _countof(SUBS); i++) {
            jp2 += count_bytes(data, dlen, jpb[i], 16);
            us2 += count_bytes(data, dlen, usb[i], 16);
        }
        if (jp2 != 0 || us2 != r->jp) {
            _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                        "internal check failed (%d JP left, %d US of %d expected) -- disk untouched",
                        jp2, us2, r->jp);
            free(data); free(ftext); return false;
        }
        pol_digest(data, dlen, r->digest_after);

        _snprintf_s(bak, sizeof(bak), _TRUNCATE, "%s.orig", dll);
        // Never overwrite the DLL without a recoverable backup. CopyFileA(TRUE) fails
        // with ERROR_FILE_EXISTS once the .orig already exists (fine -- keep the first).
        // Any OTHER failure with no .orig on disk is a real backup failure (perms, disk
        // full), and writing anyway would leave every "restore the .orig" instruction
        // pointing at a file that does not exist.
        if (!CopyFileA(dll, bak, TRUE) && GetLastError() != ERROR_FILE_EXISTS &&
            GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
            _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                        "cannot back up %s to .orig (%lu) -- refusing to patch without a backup",
                        FMO_DLL_NAME, GetLastError());
            free(data); free(ftext); return false;
        }
        if (!write_all(dll, data, dlen)) {
            _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                        "cannot write %s (%lu) -- need elevation?", FMO_DLL_NAME, GetLastError());
            free(data); free(ftext); return false;
        }
        r->dll_written = true;
        r->state = FMOIID_PATCHED;
    }

    // --- the manifest ---------------------------------------------------------------
    if (!have_row) {
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    ftext ? "no unique %s row in file.txt -- Check Files may REVERT the patch"
                          : "cannot read file.txt -- Check Files may REVERT the patch",
                    FMO_DLL_NAME);
        free(data); free(ftext);
        return false;
    }
    if (strcmp(r->filetxt_listed, r->digest_after) != 0) {
        // Rebuild the row, keeping SE's own SIZE and PATH fields verbatim.
        const char* p = (const char*)ftext + roff;
        const char* colon = (const char*)memchr(p, ':', rlen);
        size_t restlen = rlen - (size_t)(colon - p);            // ":SIZE:PATH"
        size_t dglen = strlen(r->digest_after);
        size_t nlen = flen - rlen + dglen + restlen;
        unsigned char* nb = (unsigned char*)malloc(nlen);
        if (!nb) { strcpy_s(r->err, "out of memory rewriting file.txt");
                   free(data); free(ftext); return false; }
        memcpy(nb, ftext, roff);
        memcpy(nb + roff, r->digest_after, dglen);
        memcpy(nb + roff + dglen, colon, restlen);
        memcpy(nb + roff + dglen + restlen, ftext + roff + rlen, flen - roff - rlen);

        char fbak[MAX_PATH];
        _snprintf_s(fbak, sizeof(fbak), _TRUNCATE, "%s.orig", ftxt);
        // Same rule as the DLL backup above: don't rewrite file.txt without a .orig.
        if (!CopyFileA(ftxt, fbak, TRUE) && GetLastError() != ERROR_FILE_EXISTS &&
            GetFileAttributesA(fbak) == INVALID_FILE_ATTRIBUTES) {
            _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                        "cannot back up file.txt to .orig (%lu) -- Check Files may REVERT the patch",
                        GetLastError());
            free(data); free(ftext); return false;
        }
        bool ok = write_all(ftxt, nb, nlen);
        free(nb);
        if (!ok) {
            _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                        "cannot write file.txt (%lu) -- Check Files may REVERT the patch",
                        GetLastError());
            free(data); free(ftext); return false;
        }
        r->filetxt_written = true;
    }

    free(data); free(ftext);
    return true;
}
