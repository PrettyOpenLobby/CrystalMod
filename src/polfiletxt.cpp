// polfiletxt.cpp -- keep PlayOnline's install manifest honest about a file we replaced.
//
// WHY THIS EXISTS
//
// `<install>\file.txt` is the manifest the Viewer checks itself against: one
// `DIGEST:SIZE:PATH` line per installed file. Swap a file without rewriting its line and
// the install now disagrees with its own manifest. Two things follow, and both have been
// measured on this project:
//
//   * Check Files silently REVERTS the swapped file -- no error, no symptom until then
//     (the FMO data-table pass of 2026-08-17 sat mismatched for four days).
//   * The Viewer's UPDATE refuses to finish and tells the player a file is missing and
//     that PlayOnline must be reinstalled. Reported 2026-09-21 by the first member of
//     the public to install the shim and then let the Viewer update itself.
//
// Installing the shim IS such a swap: `PolHook.dll` becomes ours and the manifest still
// describes Square Enix's. So the installer has to repair that one line, the same way
// fmoiid.cpp already does for Front Mission's DLL -- this file is that code generalised,
// and it owns `pol_digest` now so there is exactly one implementation of the digest
// rather than a copy per caller.
//
// WHAT IT GUARANTEES
//
//   * ONE row, or nothing. A manifest with no row for the file, or with two, is not ours
//     to guess at: we leave it alone and say so. (file.txt's last line is a bare "::"
//     terminator, so the parser is deliberately loose -- a strict one dies on a good file.)
//   * SIZE AND DIGEST TOGETHER, computed from the bytes actually on disk after the swap,
//     never from what we intended to write.
//   * A BACKUP FIRST. `file.txt.polshim-orig` is written before the first change and is
//     never overwritten afterwards, so revert restores Square Enix's own line rather than
//     reconstructing it.
//   * NOTHING ON FAILURE. The rewrite goes to a temp file and is renamed into place; a
//     half-written manifest is worse than a stale one.
#include "polshim.h"
#include <bcrypt.h>

// SE's substituted base64 alphabet. Verified against all 1536 entries of the USA 1.10.00c
// file.txt with 0 mismatches -- polhash.py owns the algorithm and this is a transcription
// of it, not a second derivation.
static const char POL_ALPHA[] =
    "TSG8IncW3HFKokOg79qzeCmZs2yBYEQVAUxR5rbwi4P@jMDLtpvad0f_J1hlN6uX";

static bool md5_bytes(const void* data, size_t len, unsigned char out_[16])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0) != 0) return false;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        // BCryptHashData takes a ULONG; hash in chunks rather than leave a silent cast.
        ok = true;
        const unsigned char* p = (const unsigned char*)data;
        size_t left = len;
        while (ok && left) {
            ULONG n = (ULONG)(left > 0x10000000u ? 0x10000000u : left);
            ok = BCryptHashData(h, (PUCHAR)p, n, 0) == 0;
            p += n; left -= n;
        }
        if (ok) ok = BCryptFinishHash(h, out_, 16, 0) == 0;
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

bool pol_digest(const void* data, size_t len, char out_[23])
{
    unsigned char md[16];
    if (!md5_bytes(data, len, md)) { out_[0] = 0; return false; }
    for (int i = 0; i < 21; i++) out_[i] = POL_ALPHA[bits6(md, 6 * i)];
    out_[21] = POL_ALPHA[(md[15] & 0x03) << 4];   // the 2-bit tail, LEFT-aligned
    out_[22] = 0;
    return true;
}

// --- small file helpers (the installer and the DLL both link this) -----------------
static unsigned char* slurp(const char* path, size_t* len_out)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > 64u * 1024 * 1024) { CloseHandle(f); return NULL; }
    size_t n = (size_t)sz.QuadPart;
    unsigned char* buf = (unsigned char*)malloc(n ? n : 1);
    DWORD got = 0;
    bool ok = buf && (n == 0 || (ReadFile(f, buf, (DWORD)n, &got, NULL) && got == n));
    CloseHandle(f);
    if (!ok) { free(buf); return NULL; }
    *len_out = n;
    return buf;
}

static bool spit(const char* path, const void* data, size_t len)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, data, (DWORD)len, &wrote, NULL) && wrote == len;
    CloseHandle(f);
    if (!ok) DeleteFileA(path);
    return ok;
}

// The one row whose PATH field equals `name`, or false for none / more than one.
static bool find_row(const unsigned char* buf, size_t len, const char* name,
                     size_t* row_off, size_t* row_len)
{
    size_t tl = strlen(name) + 1;
    char* tail = (char*)malloc(tl + 1);
    if (!tail) return false;
    tail[0] = ':';
    strcpy_s(tail + 1, tl, name);
    size_t start = 0; int hits = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && buf[i] != '\n') continue;
        size_t l = i - start;
        while (l && buf[start + l - 1] == '\r') l--;          // tolerate CRLF
        if (l >= tl && _strnicmp((const char*)buf + start + l - tl, tail, tl) == 0) {
            if (++hits == 1) { *row_off = start; *row_len = l; }
        }
        start = i + 1;
    }
    free(tail);
    return hits == 1;
}

static void backup_path(char* out_, size_t cap, const char* installdir)
{
    _snprintf_s(out_, cap, _TRUNCATE, "%s\\file.txt.polshim-orig", installdir);
}

bool filetxt_sync(const char* installdir, const char* name, bool apply, FileTxtReport* r)
{
    memset(r, 0, sizeof(*r));
    char ftxt[MAX_PATH], target[MAX_PATH], bak[MAX_PATH], tmp[MAX_PATH];
    _snprintf_s(ftxt,   sizeof(ftxt),   _TRUNCATE, "%s\\file.txt", installdir);
    _snprintf_s(target, sizeof(target), _TRUNCATE, "%s\\%s", installdir, name);
    _snprintf_s(tmp,    sizeof(tmp),    _TRUNCATE, "%s\\file.txt.polshim-new", installdir);
    backup_path(bak, sizeof(bak), installdir);

    size_t dlen = 0;
    unsigned char* data = slurp(target, &dlen);
    if (!data) {
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "cannot read %s (%lu)", name, GetLastError());
        return false;
    }
    char digest[23];
    bool got = pol_digest(data, dlen, digest);
    free(data);
    if (!got) {
        strcpy_s(r->err, sizeof(r->err), "could not hash the installed file");
        return false;
    }

    size_t flen = 0;
    unsigned char* f = slurp(ftxt, &flen);
    if (!f) {
        // No manifest at all is not an error: a Steam Deck tree built from the
        // disc may not carry one, and there is then nothing to disagree with.
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "no file.txt in this install -- nothing to keep in step");
        r->no_manifest = true;
        return true;
    }
    size_t off = 0, rlen = 0;
    if (!find_row(f, flen, name, &off, &rlen)) {
        free(f);
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "no single %s row in file.txt -- Check Files may REVERT the shim", name);
        return false;
    }
    size_t keep = rlen < sizeof(r->was) - 1 ? rlen : sizeof(r->was) - 1;
    memcpy(r->was, f + off, keep);
    r->was[keep] = 0;
    // KEEP SE'S OWN SPELLING OF THE PATH. The row is DIGEST:SIZE:PATH and only the
    // first two fields are ours to change. The manifest says `polhook.dll` while the
    // file on disk is `PolHook.dll`; writing our spelling back would edit a field we
    // have no reason to touch, on a manifest whose reader we do not control.
    const char* path = strchr(r->was, ':');
    path = path ? strchr(path + 1, ':') : NULL;
    if (!path) {                                  // not DIGEST:SIZE:PATH after all
        free(f);
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "the %s row in file.txt is not DIGEST:SIZE:PATH -- leaving it alone", name);
        return false;
    }
    _snprintf_s(r->now, sizeof(r->now), _TRUNCATE, "%s:%zu:%s", digest, dlen, path + 1);

    if (strcmp(r->was, r->now) == 0) {            // already agrees
        free(f);
        r->already_ok = true;
        return true;
    }
    if (!apply) { free(f); return true; }

    // The backup is SE's manifest, so it is written once and never refreshed:
    // a second install must not capture our own line as the thing to revert to.
    if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
        if (!spit(bak, f, flen)) {
            free(f);
            _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                        "cannot back up file.txt (%lu) -- leaving it alone", GetLastError());
            return false;
        }
        r->backup_written = true;
    }

    size_t nlen = strlen(r->now);
    size_t total = flen - rlen + nlen;
    unsigned char* nb = (unsigned char*)malloc(total ? total : 1);
    if (!nb) { free(f); strcpy_s(r->err, sizeof(r->err), "out of memory"); return false; }
    memcpy(nb, f, off);
    memcpy(nb + off, r->now, nlen);
    memcpy(nb + off + nlen, f + off + rlen, flen - off - rlen);
    free(f);
    bool ok = spit(tmp, nb, total);
    free(nb);
    if (!ok) {
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "cannot write file.txt (%lu) -- Check Files may REVERT the shim",
                    GetLastError());
        return false;
    }
    if (!MoveFileExA(tmp, ftxt, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "cannot replace file.txt (%lu)", GetLastError());
        return false;
    }
    r->written = true;
    return true;
}

bool filetxt_restore(const char* installdir, FileTxtReport* r)
{
    memset(r, 0, sizeof(*r));
    char ftxt[MAX_PATH], bak[MAX_PATH];
    _snprintf_s(ftxt, sizeof(ftxt), _TRUNCATE, "%s\\file.txt", installdir);
    backup_path(bak, sizeof(bak), installdir);
    if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
        strcpy_s(r->err, sizeof(r->err), "no file.txt backup here -- leaving the manifest alone");
        return false;
    }
    if (!MoveFileExA(bak, ftxt, MOVEFILE_REPLACE_EXISTING)) {
        _snprintf_s(r->err, sizeof(r->err), _TRUNCATE,
                    "cannot put file.txt back (%lu)", GetLastError());
        return false;
    }
    r->written = true;
    return true;
}
