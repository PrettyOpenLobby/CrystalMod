// regfix.cpp -- registration repair.
//
// Titles whose FILES are present but whose registry entries are missing (games copied
// to a new machine / Steam Deck without a real install) don't appear in the games menu
// and can't launch, because the Viewer gates on HKLM\SOFTWARE\PlayOnlineUS, not on disk.
// This detects such titles and WRITES the missing keys so they register.
//
// Runs at startup on the PolHook proxy -- before pol.exe's entry, so writes land before
// the client reads/enumerates. A real WRITE (not the in-process serve regredir uses)
// because the menu ENUMERATES InstallFolder; a synthetic value would need enumeration
// hooks too. pol.exe is elevated (and on the Deck HKLM is the writable Wine prefix).
//
// IMPORTANT: "ONLY EVER CREATES A MISSING VALUE" WAS THE BUG, not the safety property, and this
// header said it for months. On a machine where SE's own installer ran, every value is
// PRESENT -- so create-if-absent is a no-op precisely where a repair is needed, and both
// Fantasy Earth and FMO shipped broken to a second machine because of it (2026-08-18). Values whose CONTENT this file knows are now ENFORCED, not merely created:
// see repair_contents_guids. Everything else is still create-only.
//
// Records what it changed to a sidecar (regfix-added.txt beside the DLL). A value we
// CREATED is recorded as `subkey|name` and [regfix] clean=1 deletes it; a value we
// CORRECTED is recorded as `subkey|name|oldvalue` and clean RESTORES the old value,
// because those keys were SE's before they were ours.
//
// Four tiers are written per title:
//   * InstallFolder\NNNN + (for COM-launched titles) ContentsCLSID\NNNN / ContentsIID\NNNN,
//     the latter pair enforced against this file's table rather than merely created;
//   * the Interface\NNNN <-> patch.ver version stamp, made to AGREE (see repair_patchver);
//   * HKCR\CLSID\{...} class registration for the titles whose enumerator arm calls
//     ProgIDFromCLSID -- FFXI, Tetra Master, Friend List (see repair_comclass). Without
//     it the title is dropped from Check Files entirely, which is what hid Tetra Master
//     on the Steam Deck. Product\NNNN is still not written; nothing has shown it required.
//   * FMO ONLY: FrontMissionOnline.dll's JP->US interface GUIDs, on disk, with file.txt
//     moved in the same step (see repair_fmo_iids / fmoiid.cpp). The one tier that edits
//     a game file, and the one the patch server cannot be relied on to deliver.
//
// CAVEAT on [regfix] clean=1: the sidecar records VALUES, so clean removes the values we
// added but leaves the (now empty) CLSID subkeys behind. Harmless -- ProgIDFromCLSID
// fails on a key with no ProgID value, so the undo is functionally complete. It does NOT
// undo the FMO DLL patch either; FrontMissionOnline.dll.orig and file.txt.orig are the
// backups for that, exactly as the offline patch script's --revert uses.

#include "polshim.h"

static int g_on = 0;
int regfix_enabled() { return g_on; }

struct TitleReg {
    const char* id;           // "0004"
    const char* folder;       // SquareEnix subfolder leaf name
    int trailing_slash;       // 1 if SE's InstallFolder value for this id ends with a separator
    const char* clsid;        // NULL = not a COM-launched title
    const char* iid;
    int patch_port;           // our patch server's port for this title (53000 + id), 0 = none
    int needs_interface;      // Interface\NNNN = the patch.ver decryption stamp
    int needs_product;        // Product\NNNN   = the encrypted product/version blob
    const char* version;      // what a repaired patch.ver should claim
};

// GUIDs/folders measured from a working install. FE's IID is the
// CORRECTED GameStart interface (30B9D121...), not the stock ContentsIID.
// needs_interface / needs_product are MEASURED from a working install, not assumed:
//   Interface: 0001 0002 0011 0014 1000     (0004 has none at all)
//   Product:   0001 is ZERO BYTES; 0011 and 0014 are real 288-byte blobs
// FMO needing neither is why it is the one title the easy tier alone can launch.
//
// IMPORTANT: needs_product is 0 EVERYWHERE, deliberately. FFXI was briefly marked as needing it
// because the value EXISTS -- but it is zero-length, which is an installer artifact and
// not a requirement, and FFXI launches fine on a machine with no such value. Reporting
// it missing was a false alarm on a title that plainly works, caught in the field.
// FE/FL do carry real blobs, but nothing has yet shown they are REQUIRED, and asserting
// that would be the same unfounded claim in the other direction.
//
// version = what a repaired patch.ver should claim. These are the versions our own
// patch server serves for each title, which is right for a title copied from an install
// that matches those bundles -- the case regfix exists for. If the files on disk are
// actually older, the client updates from us, which is the safe direction.
static const TitleReg g_titles[] = {
    { "0001", "FINAL FANTASY XI",     1, NULL, NULL, 53001, 1, 0, "30260703_1" },
    { "0002", "TetraMaster",          0, NULL, NULL, 53002, 1, 0, "20030429_0" },
    { "0004", "FRONT MISSION ONLINE", 1, "94603A98-0067-4F41-89BC-7F89304D6E28", "2031D0EF-97FA-48AE-A3FD-8260C380029A", 53004, 0, 0, "0000000000" },
    { "0011", "FantasyEarth",         1, "0DD61D2C-4C17-464C-AEA3-2D248DC89C86", "30B9D121-E007-41C9-825E-E8F5E3D55D59", 53011, 1, 0, "20051220_1" },
    { "0014", "PlayOnlineFriendList", 0, NULL, NULL, 53014, 1, 0, "20071010_1" },
    // 0015 = FINAL FANTASY XI Test Server (sqpolcts.bin record 15, registered 2011/07/20,
    // launchable flag 1). It has had an accept arm in the Check Files enumerator all along
    // -- ids 1/2/14/15/1000 are the only five -- and was invisible for exactly one reason:
    // nothing had ever registered its COM classes. See g_comclasses below.
    //
    // 2026-08-18: the genuine installer SURVIVES after all -- GameFiles/FFXI Test Server
    // Files/A11203W_001.zip, an MSI whose Registry table was dumped, so this row is
    // now measured, not guessed:
    //   folder          "FINAL FANTASY XI Test Client" = the MSI's own INSTALLDIR leaf
    //                   (ProductName, and the readme's install path). Confirmed.
    //   trailing_slash  SE writes InstallFolder\0015 = [INSTALLDIR]; MSI folder values
    //                   carry the separator, same as retail FFXI. Stays 1.
    //   patch_port      53015 -- published on the patch service since 2026-08-17
    //                   serving the synthesised W20-0015 bundle.
    //   needs_interface 1. SE's MSI seeds Interface\0015 = REG_SZ "0" and ships a 12-byte
    //                   PLAINTEXT patch.ver ("30020917_0" CRLF) -- a floor stamp whose only
    //                   job is to make the first launch pull the full update. A plaintext
    //                   file fails the 288-byte read in repair_patchver, so a factory-fresh
    //                   tree takes the "no patch.ver on disk" path and gets a well-formed
    //                   blob; SE's registry value ("0"), when present, is kept as the key.
    //   version         what OUR patch service serves for W20-0015 (chosen 2026-08-17 from
    //                   the tree's own file mtimes; the FACTORY stamp is 30020917_0, but a
    //                   repaired patch.ver must claim what the server serves, or the client
    //                   re-downloads the world on every launch).
    { "0015", "FINAL FANTASY XI Test Client", 1, NULL, NULL, 53015, 1, 0, "20110811_A" },
};

typedef LONG (WINAPI *PFN_ROK)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *PFN_RCKE)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
typedef LONG (WINAPI *PFN_RQV)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *PFN_RSV)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
typedef LONG (WINAPI *PFN_RCK)(HKEY);
typedef LONG (WINAPI *PFN_RDV)(HKEY, LPCSTR);

static PFN_ROK rok; static PFN_RCKE rcke; static PFN_RQV rqv;
static PFN_RSV rsv; static PFN_RCK rck;   static PFN_RDV rdv;

// The raw advapi32 exports, resolved by name -- so our own reads/writes never bounce
// through regredir's RegQueryValueEx hook.
static bool resolve_reg()
{
    HMODULE a = GetModuleHandleW(L"advapi32.dll");
    if (!a) a = LoadLibraryW(L"advapi32.dll");
    if (!a) return false;
    rok  = (PFN_ROK) GetProcAddress(a, "RegOpenKeyExA");
    rcke = (PFN_RCKE)GetProcAddress(a, "RegCreateKeyExA");
    rqv  = (PFN_RQV) GetProcAddress(a, "RegQueryValueExA");
    rsv  = (PFN_RSV) GetProcAddress(a, "RegSetValueExA");
    rck  = (PFN_RCK) GetProcAddress(a, "RegCloseKey");
    rdv  = (PFN_RDV) GetProcAddress(a, "RegDeleteValueA");
    return rok && rcke && rqv && rsv && rck && rdv;
}

static const char* g_hive = NULL;          // "SOFTWARE\\PlayOnlineUS" (32-bit view auto-redirects to WOW6432Node)
static char        g_sqroot[MAX_PATH] = ""; // ...\SquareEnix (parent of the registered Viewer folder)

// The active hive = whichever PlayOnline* carries InstallFolder\1000; its parent dir is
// where SE installs sibling titles.
static bool detect_hive()
{
    static const char* hives[] = { "SOFTWARE\\PlayOnlineUS", "SOFTWARE\\PlayOnline", "SOFTWARE\\PlayOnlineEU" };
    for (int i = 0; i < 3; i++) {
        char sub[256]; _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\InstallFolder", hives[i]);
        HKEY h;
        if (rok(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        char viewer[MAX_PATH]; DWORD type = 0, sz = sizeof(viewer) - 1;
        LONG r = rqv(h, "1000", NULL, &type, (LPBYTE)viewer, &sz);
        rck(h);
        if (r != ERROR_SUCCESS || sz <= 1) continue;
        viewer[sz] = 0;
        size_t vl = strlen(viewer); if (vl && (viewer[vl-1]=='\\'||viewer[vl-1]=='/')) viewer[--vl]=0;
        char* slash = strrchr(viewer, '\\'); if (!slash) slash = strrchr(viewer, '/');
        if (!slash) continue;
        *slash = 0; strcpy_s(g_sqroot, viewer);   // ...\SquareEnix
        g_hive = hives[i];
        return true;
    }
    return false;
}

static bool value_exists(const char* subkey, const char* name)
{
    HKEY h; if (rok(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD t=0, sz=0; LONG r = rqv(h, name, NULL, &t, NULL, &sz); rck(h);
    return r == ERROR_SUCCESS;
}

// "installed on disk" = the folder exists AND has a non-empty file.txt (the same
// last-gate Check Files uses, so an empty stub folder is not mistaken for an install).
static bool folder_installed(const char* path)
{
    char ft[MAX_PATH]; _snprintf_s(ft, sizeof(ft), _TRUNCATE, "%s\\file.txt", path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(ft, GetFileExInfoStandard, &fa)) return false;
    return (fa.nFileSizeLow | fa.nFileSizeHigh) != 0;
}

// Sidecar (beside the DLL, derived from the ini path) records every value we CREATE.
static void sidecar_path(const wchar_t* ini, char* out, size_t cch)
{
    char inia[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, ini, -1, inia, sizeof(inia), NULL, NULL);
    char* slash = strrchr(inia, '\\'); if (!slash) slash = strrchr(inia, '/');
    if (slash) { *(slash+1) = 0; _snprintf_s(out, cch, _TRUNCATE, "%sregfix-added.txt", inia); }
    else       _snprintf_s(out, cch, _TRUNCATE, "regfix-added.txt");
}
static char g_sidecar[MAX_PATH];

static bool set_sz(const char* subkey, const char* name, const char* val)
{
    HKEY h; DWORD disp;
    if (rcke(HKEY_LOCAL_MACHINE, subkey, 0, NULL, 0, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
        return false;
    LONG r = rsv(h, name, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
    rck(h);
    if (r == ERROR_SUCCESS) {
        FILE* f = fopen(g_sidecar, "a");
        if (f) { fprintf(f, "%s|%s\n", subkey, name); fclose(f); }
    }
    return r == ERROR_SUCCESS;
}

// Create a value only if it is absent; returns 1 if written, 0 if already present.
static int create_if_absent(const char* subkey, const char* name, const char* val)
{
    if (value_exists(subkey, name)) return 0;
    return set_sz(subkey, name, val) ? 1 : 0;
}

// Read a REG_SZ; false if absent. Used by the enforce path, which has to compare a
// value rather than merely notice it exists.
static bool read_sz_at(const char* subkey, const char* name, char* out, size_t cch)
{
    out[0] = 0;
    HKEY h; if (rok(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD ty = 0, sz = (DWORD)cch - 1;
    LONG r = rqv(h, name, NULL, &ty, (LPBYTE)out, &sz);
    rck(h);
    if (r != ERROR_SUCCESS) { out[0] = 0; return false; }
    out[sz < cch ? sz : cch - 1] = 0;
    return true;
}

// GUIDs compare EQUAL across case and optional braces. SE writes them bare and upper
// case; treating "{2031d0ef-...}" as a different value would rewrite a correct key on
// every start and fill the sidecar with churn.
static bool guid_same(const char* a, const char* b)
{
    while (*a == '{') a++;  while (*b == '{') b++;
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca == '}' || ca == 0) ca = 0;
        if (cb == '}' || cb == 0) cb = 0;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

// Overwrite a value that is PRESENT BUT WRONG, recording the ORIGINAL so clean=1 puts
// it back instead of deleting it. Sidecar lines gain an optional third field:
//     subkey|name          we created it       -> clean DELETES
//     subkey|name|oldvalue we overwrote it     -> clean RESTORES oldvalue
// Two-field lines are what every earlier build wrote, so old sidecars still undo.
static bool overwrite_sz(const char* subkey, const char* name, const char* val, const char* old)
{
    HKEY h; DWORD disp;
    if (rcke(HKEY_LOCAL_MACHINE, subkey, 0, NULL, 0, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
        return false;
    LONG r = rsv(h, name, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
    rck(h);
    if (r == ERROR_SUCCESS) {
        FILE* f = fopen(g_sidecar, "a");
        if (f) { fprintf(f, "%s|%s|%s\n", subkey, name, old); fclose(f); }
    }
    return r == ERROR_SUCCESS;
}

static void regfix_clean()
{
    FILE* f = fopen(g_sidecar, "r");
    if (!f) { logf("[regfix] clean: no sidecar (%s) -- nothing to undo", g_sidecar); return; }
    char line[512]; int removed = 0, restored = 0;
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        char* bar = strchr(line, '|'); if (!bar) continue;
        *bar = 0; const char* subkey = line; char* name = bar + 1;
        // Third field = the value we OVERWROTE. Undo is a restore, not a delete: those
        // keys were SE's before they were ours, and deleting one leaves the title in a
        // state its own installer never produced.
        char* bar2 = strchr(name, '|');
        const char* old = NULL;
        if (bar2) { *bar2 = 0; old = bar2 + 1; }
        HKEY h;
        if (rok(HKEY_LOCAL_MACHINE, subkey, 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
            if (old) {
                if (rsv(h, name, 0, REG_SZ, (const BYTE*)old, (DWORD)strlen(old) + 1)
                        == ERROR_SUCCESS) restored++;
            } else if (rdv(h, name) == ERROR_SUCCESS) removed++;
            rck(h);
        }
    }
    fclose(f);
    remove(g_sidecar);
    logf("[regfix] clean: removed %d value(s) we had added, restored %d we had corrected; "
         "sidecar deleted", removed, restored);
}

// ------------------------------------------------------------------ reporting
//
// The scan used to be silent about every path it REJECTED, so "regfix found nothing"
// and "regfix never looked" produced identical output: one summary line. That is
// useless on a machine where the answer is "your titles are somewhere I did not look",
// which is the normal case on a Steam Deck. Every verdict is now spelled out, into a
// caller-supplied buffer, so the same text can go to the log AND to the settings dialog.

static char*  g_rep;         // NULL = report to the log only
static size_t g_repleft;

static void rep(const char* fmt, ...)
{
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    logf("[regfix] %s", line);
    if (g_rep && g_repleft > 1) {
        size_t n = strlen(line);
        if (n + 2 > g_repleft) n = g_repleft - 2;
        memcpy(g_rep, line, n); g_rep += n; *g_rep++ = '\n'; *g_rep = 0;
        g_repleft -= (n + 1);
    }
}

static wchar_t g_inipath[MAX_PATH];

// Interface\NNNN is the install-time stamp that DECRYPTS this title's patch.ver, so a
// title without it reads as "not installed" / "version unknown" no matter how correct
// InstallFolder is. It is not inventable, but it IS recoverable from patch.ver itself
// (the server's tools/patchver.py `recover` -- a direct solve, not a search). Porting that
// solve into the shim is the proper fix; until then this lets the recovered value be
// supplied per title:   [regfix] interface_0002=026130e3
// Repair the (patch.ver, Interface\NNNN) pair -- the actual fix, not advice about one.
//
// Interface is an install-time nonce, and the ONLY property the client checks is that
// the blob decrypts under it. So rather than recover the original (an algebraic solve),
// we pick a key, write a fresh blob claiming the title's version, and store both. The
// pair is self-consistent, which is all that was ever required.
//
// Safe because Interface is MISSING: without it the client cannot decrypt the existing
// patch.ver at all, so that file is already unusable. It is still backed up first, an
// explicit [regfix] interface_NNNN= still wins, and a title whose Interface is present
// is never touched.
// Read a value as a string; returns false if absent.
static bool read_sz(const char* subkey, const char* name, char* out, size_t cch)
{
    HKEY h; if (rok(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD ty = 0, sz = (DWORD)cch - 1;
    LONG r = rqv(h, name, NULL, &ty, (LPBYTE)out, &sz);
    rck(h);
    if (r != ERROR_SUCCESS) return false;
    out[sz < cch ? sz : cch - 1] = 0;
    return out[0] != 0;
}

// Make Interface\NNNN and <installdir>\patch.ver AGREE.
//
// IMPORTANT: THE WHOLE BUG, found over SSH on a real Deck 2026-08-15. Interface is the key that
// decrypts a title's patch.ver, and it only works if the two MATCH. On that machine:
//
//     0001 FFXI    reg 009fe3db  file 009fe3db  -> match, works
//     1000 Viewer  reg 0097d46d  file 0097d46d  -> match, works
//     0002 TM      reg 0097d4db  file 00831a48  -> MISMATCH, "not installed"
//     0011 FE      reg 014c14c0  file 017a36cd  -> MISMATCH, "version unknown"
//     0014 FL      reg 014c14c2  file 017a36cf  -> MISMATCH, drags FE down with it
//
// Five for five. And for Tetra Master the mismatch is the STEAM DEPOT's own doing: it
// ships SE's original patch.ver but its install script writes an unrelated nonce. That
// title is broken on a stock install with no shim anywhere near it.
//
// Two things this earlier got wrong, both now fixed:
//   * it used create_if_absent, so it only ever acted when Interface was MISSING. The
//     case that actually matters -- PRESENT BUT WRONG -- was skipped every single time,
//     which is why every build shipped before this was a no-op on the failing machine.
//   * fixing the REGISTRY is futile: SE's launch path rewrites Interface every run (to
//     the same value). So we adapt the FILE to the registry instead -- re-encrypt
//     patch.ver under whatever Interface currently says. That survives the rewrite by
//     construction, because the value SE keeps writing is the one the file now expects.
static void repair_patchver(const char* insub, const TitleReg* t, const char* installdir)
{
    if (!t->needs_interface) return;

    char pv[MAX_PATH];
    _snprintf_s(pv, sizeof(pv), _TRUNCATE, "%s\\patch.ver", installdir);

    char iface[64] = "";
    bool have_iface = read_sz(insub, t->id, iface, sizeof(iface));

    // Does the file already decrypt under the registry's key? Then leave everything be.
    unsigned char cur[0x120];
    bool have_file = false;
    FILE* rf = fopen(pv, "rb");
    if (rf) { have_file = fread(cur, 1, sizeof(cur), rf) == sizeof(cur); fclose(rf); }

    if (have_iface && have_file) {
        char ver[64] = "";
        if (patchver_read_blob(cur, atoi(t->id), iface, ver, sizeof(ver))) {
            rep("      version stamp OK (%s reads as %s)", iface, ver);
            return;
        }
        rep("      MISMATCH: registry says %s but patch.ver does not decrypt with it", iface);
    } else if (!have_file) {
        rep("      no patch.ver on disk -- writing one");
    }

    // An explicit override always wins.
    wchar_t k[64]; swprintf_s(k, L"interface_%hs", t->id);
    wchar_t wv[64] = L"";
    ini_str(L"regfix", k, L"", wv, _countof(wv), g_inipath);
    char use[64];
    if (wv[0]) WideCharToMultiByte(CP_ACP, 0, wv, -1, use, sizeof(use), NULL, NULL);
    else if (have_iface) strcpy_s(use, iface);          // adapt the FILE to SE's value
    else _snprintf_s(use, sizeof(use), _TRUNCATE, "%08x", (unsigned)GetTickCount());

    if (GetPrivateProfileIntW(L"regfix", L"rekey", 1, g_inipath) == 0) {
        rep("      NOT fixed: [regfix] rekey=0 forbids rewriting patch.ver.");
        return;
    }

    unsigned char blob[0x120];
    patchver_make_blob(t->version, atoi(t->id), use, blob);

    // Verify our own output BEFORE touching the disk -- a wrong cipher must never be
    // allowed to replace a real file with rubbish.
    char check[64] = "";
    if (!patchver_read_blob(blob, atoi(t->id), use, check, sizeof(check))
            || strcmp(check, t->version) != 0) {
        rep("      NOT fixed: internal check failed, disk untouched.");
        return;
    }

    char bak[MAX_PATH];
    _snprintf_s(bak, sizeof(bak), _TRUNCATE, "%s.bak-polshim", pv);
    if (have_file) CopyFileA(pv, bak, TRUE);            // TRUE = keep the FIRST backup

    FILE* f = fopen(pv, "wb");
    if (!f) { rep("      NOT fixed: cannot write %s", pv); return; }
    bool ok = fwrite(blob, 1, sizeof(blob), f) == sizeof(blob);
    fclose(f);
    if (!ok) { rep("      NOT fixed: short write to patch.ver"); return; }

    if (!have_iface) set_sz(insub, t->id, use);         // only write the registry if absent
    rep("      FIXED -- patch.ver re-keyed to %s, version %s", use, t->version);
}


// Make ContentsCLSID\NNNN and ContentsIID\NNNN carry the values in g_titles, OVERWRITING
// one that is present but wrong.
//
// IMPORTANT: THE FANTASY EARTH LAUNCH BUG, found 2026-08-18 from the second Windows machine's uploaded
// logs. This tier used create_if_absent, so it only ever acted when the value was
// MISSING -- and on a machine where SE's own installer ran, ContentsIID\0011 is PRESENT
// and holds the stock {6560D1E0-...}. The title therefore counted as "complete already",
// nothing corrected it, and the corrected GameStart interface {30B9D121-...} this table
// has carried all along was never written. comtrace's contentiid_alias then retries the
// failed activation with the stock IID, whose slot 3 is a GUID-comparing stub rather
// than GameStart, and returns E_NOINTERFACE. FE does not launch.
//
// This is the SAME mistake repair_patchver's header describes for Interface\NNNN --
// "the case that actually matters, PRESENT BUT WRONG, was skipped every single time" --
// repeated one tier up. Generalise: on a machine that had a real installer, every value
// regfix cares about is present; presence is not evidence of correctness.
//
// WARNING: ContentsCLSID is enforced too, but note the asymmetry: our CLSIDs are SE's own
// values, so that write is a no-op on any genuine install and only matters for a COPIED
// tree. Only the FE IID deliberately differs from what SE ships.
static void repair_contents_guids(const char* csub, const char* isub, const TitleReg* t)
{
    if (!t->clsid) return;
    // IMPORTANT: DEFAULT 0 SINCE 2026-08-18, pending a verdict. The account holder reports that
    // "Content Registry Data is corrupted" -- a message that exists in NO client
    // resource we can find -- began appearing only after this tier shipped, where the
    // failure had previously been a generic POL error. Zero writes were logged on that
    // machine (no CORRECTED, no MISSING, no MISMATCH), but "it never wrote" is not "it
    // changed nothing": this function reads ContentsCLSID/ContentsIID for every
    // registered title on every start, where before it only tested existence. Reasoning
    // from the absence of a log line is exactly what produced two wrong diagnoses that
    // day; a first-hand before/after report outranks it.
    //
    // Off by default until the bisect says otherwise, because the cost is asymmetric:
    // wrong-and-on breaks a machine that cannot be reached without the account holder
    // hand-editing an ini on it, and wrong-and-off costs a Fantasy Earth launch that
    // was already being rescued by contentiid_alias before this existed.
    // WARNING: THE GATE GUARDS THE OVERWRITE, NOT THE CREATE -- corrected 2026-08-18, same day
    // it was introduced. Defaulting enforce_guids to 0 (above) accidentally disabled
    // WRITING these values at all, because this function had replaced the two
    // create_if_absent calls that used to live on the register path. A freshly copied
    // title would then get InstallFolder and no ContentsCLSID/ContentsIID -- a title
    // registered even less completely than before the tier existed.
    //
    // Creating an ABSENT value is the original, safe behaviour and is never gated.
    // Only overwriting a value that is PRESENT BUT DIFFERENT is in question, so only
    // that is switchable.
    const bool enforce = GetPrivateProfileIntW(L"regfix", L"enforce_guids", 0, g_inipath) != 0;

    struct { const char* sub; const char* want; const char* label; } pair[2] = {
        { csub, t->clsid, "ContentsCLSID" },
        { isub, t->iid,   "ContentsIID"   },
    };
    for (int i = 0; i < 2; i++) {
        char cur[128] = "";
        if (!read_sz_at(pair[i].sub, t->id, cur, sizeof(cur))) {
            if (set_sz(pair[i].sub, t->id, pair[i].want))
                rep("      + %s written (was absent)", pair[i].label);
            else
                rep("      %s MISSING and the write failed (%lu) -- need elevation?",
                    pair[i].label, GetLastError());
            continue;
        }
        if (guid_same(cur, pair[i].want)) continue;                 // already right, say nothing
        if (!enforce) {
            rep("      %s is %s, table says %s -- LEFT ALONE ([regfix] enforce_guids=1 "
                "to correct it)", pair[i].label, cur, pair[i].want);
            continue;
        }
        if (overwrite_sz(pair[i].sub, t->id, pair[i].want, cur))
            rep("      %s CORRECTED: %s -> %s (clean=1 restores the old value)",
                pair[i].label, cur, pair[i].want);
        else
            rep("      %s is %s, should be %s, and the write FAILED (%lu) -- need elevation?",
                pair[i].label, cur, pair[i].want, GetLastError());
    }
}


// FMO only: re-mint FrontMissionOnline.dll's interface GUIDs JP->US on disk, and move
// file.txt with it. The mechanism and the measurements are in fmoiid.cpp; this is the
// reporting shell that puts it in the same place as the other per-title repairs.
//
// Why regfix owns it: the served bundle cannot be relied on to deliver the patched DLL.
// It is published as W20-0004 version 20060823_1 while that tree's LATEST is 20260813_1,
// so a client already stamped at latest is answered "registered, you are current" and
// fetches nothing -- measured on the second Windows machine 2026-08-18, with zero cmd3 reads server-side.
static void repair_fmo_iids(const TitleReg* t, const char* installdir)
{
    if (strcmp(t->id, "0004") != 0) return;
    // IMPORTANT: DEFAULT 0 SINCE 2026-08-18 -- see the note on enforce_guids above; same report,
    // same bisect. This is the half to suspect first: even on the do-nothing path it
    // OPENS FrontMissionOnline.dll and MD5s all 2.7 MB of it, plus reads file.txt, on
    // every single start, before pol.exe's entry. That is new I/O in the startup path
    // that did not exist before this tier.
    //
    // And it was earning nothing where it ran: the second machine's tree already reported
    // `IID patch OK (US GUIDs x7, file.txt agrees)`, i.e. its DLL had been patched by
    // some other route long before. The tier was written for a machine that turned out
    // not to need it, so leaving it armed by default was never paying for its risk.
    if (GetPrivateProfileIntW(L"regfix", L"fmo_iid_patch", 0, g_inipath) == 0) {
        rep("      FMO IID patch OFF ([regfix] fmo_iid_patch=1 to arm it)");
        return;
    }
    bool force = GetPrivateProfileIntW(L"regfix", L"fmo_iid_force", 0, g_inipath) != 0;

    FmoIidReport r;
    bool ok = fmoiid_repair(installdir, true, force, &r);

    if (ok && !r.dll_written && !r.filetxt_written) {
        rep("      IID patch OK (US GUIDs x%d, file.txt agrees)", r.us);
        return;
    }
    if (r.dll_written)
        rep("      IID patch APPLIED -- %d GUID site(s) JP->US, %s -> %s "
            "(original kept as %s.orig)",
            r.substituted, r.digest_before, r.digest_after, "FrontMissionOnline.dll");
    if (r.filetxt_written)
        rep("      file.txt digest updated %s -> %s, or Check Files would REVERT it",
            r.filetxt_listed[0] ? r.filetxt_listed : "(none)", r.digest_after);
    if (!ok)
        rep("      IID patch NOT applied: %s", r.err[0] ? r.err : "unknown");
    else if (r.dll_written)
        rep("      FMO now activates natively -- this fixes BOTH the 'class not "
            "registered' refusal and the crash just after GameStart.");
}


// --- COM class registration (HKCR\CLSID) ------------------------------------------
//
// IMPORTANT: THE REASON TETRA MASTER NEVER REACHED CHECK FILES ON THE DECK, found 2026-08-15
// by reading app.dll's content enumerator instead of guessing at its inputs.
//
// The hardcoded per-id chain at app.dll+0x27A40D (mapped in patches.cpp) gives ids
// 1/2/14/15/1000 their own arm. Three of those arms OPEN WITH A COM LOOKUP:
//
//     id 1  FFXI          ProgIDFromCLSID({989D790D-6236-11D4-80E9-00105A81E890})
//     id 2  Tetra Master  ProgIDFromCLSID(clsid_table[region])   <- table @0x4D073F0,
//                                                  region = *(int*)0x4E17B44 (0 JP/1 US/2 EU)
//     id 14 Friend List   ProgIDFromCLSID({7E2702CC-2338-4087-8355-FA9882B5BDA8})
//
// `call dword [0x4B6D420]` is ole32!ProgIDFromCLSID -- slot 0 of app.dll's ole32
// import thunk, resolved from the import directory, not inferred. A non-zero return
// takes the branch to +0x27A554, which sets message 0x16 and leaves the title
// REJECTED, so the id is dropped before the client ever looks at its folder. Note
// only Tetra Master's arm is region-indexed; FFXI's and the Friend List's GUIDs are
// single literals used in every region.
//
// ProgIDFromCLSID reads HKCR\CLSID\{guid}\ProgID, which exists only because a real
// installer ran regsvr32. A title COPIED onto a machine -- the whole reason regfix
// exists -- has files, has InstallFolder, has a good patch.ver, and still cannot be
// seen, because nothing ever registered its class. Measured on the Deck: FFXI's CLSID
// was present (its depot registers it) while Tetra Master's and the Friend List's had
// ZERO occurrences in the prefix. On a genuine Windows install all three are present.
//
// So we write the class registration ourselves, transcribed from a genuine 2011 US
// install rather than invented. pol.exe is 32-bit, so HKLM\SOFTWARE\Classes redirects
// to ...\Wow6432Node\Classes, which is exactly where the working FFXI entry sits.
struct ComClass {
    const char* id;            // content id, matches TitleReg::id
    const char* clsid;         // the CLSID app.dll asks about (all regions)
    const char* clsid_jp;      // NULL = no separate JP class (only Tetra Master has one)
    const char* classname;     // the key's default value
    const char* progid;
    const char* viprogid;
    const char* typelib;
    const char* typelib_jp;
    const char* dll;           // leaf name under the install folder
    const char* threading;     // NULL = the real install sets none (FFXI)
    const char* licence;       // ASProtect's REG_MULTI_SZ blob, NULL = none
    const char* licence_jp;
    const char* appid;         // NULL = none
    int         programmable;  // 1 = the empty Programmable subkey a real install has
};

// Verbatim from HKLM\SOFTWARE\Classes\WOW6432Node\CLSID on a working install. The
// "InprocServer32" value INSIDE InprocServer32 is ASProtect's licence blob; it is not
// what the enumerator checks, but a genuine install has it and TM.dll is the packed
// module that reads it, so it is reproduced byte for byte rather than dropped.
static const ComClass g_comclasses[] = {
    { "0001", "{989D790D-6236-11D4-80E9-00105A81E890}", NULL,
      "FFXiEntry Class", "FFXi.FFXiEntry.1", "FFXi.FFXiEntry",
      "{989D7900-6236-11D4-80E9-00105A81E890}", NULL,
      "FFXi.dll", NULL, NULL, NULL, NULL, 0 },
    // FFXi.dll is an ATL server with TWO classes, and FFXiEntry cannot build its object
    // without FxFileManager -- the 0015 track measured the failure shape live (2026-08-18):
    // with only the entry class registered, CoCreateInstance on the ENTRY CLSID fails
    // 0x80040154 because the DLL's own factory asks for the file manager first. A retail
    // FFXI tree copied to a fresh machine has the identical latent hole, so both rows are
    // here. Retail's second CLSID is from a genuine registered install;
    // TypeLib = retail FFXi.dll's own {989D7900}.
    { "0001", "{0DF0E951-D03C-4A94-90EF-40AE60668F5F}", NULL,
      "FxFileManager Class", "FFXi.FxFileManager.1", "FFXi.FxFileManager",
      "{989D7900-6236-11D4-80E9-00105A81E890}", NULL,
      "FFXi.dll", NULL, NULL, NULL, NULL, 0 },
    { "0002", "{72B2FE03-BA77-4867-84A2-7BAD0F5A8FBB}", "{21089BA0-46E7-4F75-9465-63A19827F3BB}",
      "TetraMaster Class", "TM.TetraMaster.1", "TM.TetraMaster",
      "{E78FD700-B05B-4d9d-97D5-0F37DD3649E8}", "{80EF32B2-2DD7-460C-ACBC-2F94FCB53E9D}",
      "TM.dll", "Apartment",
      "[pan=7=)`8-Sv4[)($1lTetraMasterProgramEUUS>+^!=pQ&SS=uu61H-2._^",
      "iB@&!.0xR?CzKNZf{PvhTetraMasterProgramJP>m0wNF[^6Y?7ZTm)]W5A2",
      NULL, 1 },
    { "0014", "{7E2702CC-2338-4087-8355-FA9882B5BDA8}", NULL,
      "FriendListCom Class", "FriendList.FriendListCom.1", "FriendList.FriendListCom",
      "{152A8E16-D126-4255-BFB6-CAFE3E7DE723}", NULL,
      "FriendList.dll", "Apartment", NULL, NULL,
      "{1AEB129C-AE5C-4A3F-AE8B-0435B1A14480}", 0 },

    // IMPORTANT: 0004 AND 0011 WERE THE WHOLE BUG, added 2026-08-18. Their absence here is why a
    // machine that regfix had "registered" still could not launch them.
    //
    // These two are the only titles regfix writes ContentsCLSID for -- it tells the
    // Viewer "content 4 activates through {94603A98-...}, content 11 through
    // {0DD61D2C-...}" -- and it then never registered either CLASS, because this table
    // stopped at 1/2/14. So the content registry pointed at classes that resolved to
    // nothing: no InprocServer32, no server DLL, nothing. Measured on the account
    // holder's machine, whose regfix-added.txt records ContentsCLSID\0004 and \0011
    // being created while the only HKCR\CLSID written was the Friend List's.
    //
    // A pointer without a target is WORSE than no pointer. It is not "incomplete so the
    // title stays hidden" -- it is a registry the client reads, believes, and then finds
    // inconsistent, and the failure it produces says the content registry data is
    // corrupt. Which, read literally, is exactly what it was.
    //
    // WARNING: Generalise, because this is the same shape as the two bugs above it in this
    // file: regfix writes SETS of values that only mean anything TOGETHER. Interface
    // without a matching patch.ver was one. ContentsCLSID without HKCR\CLSID is another.
    // Adding half a set is not a partial fix, it is a new failure mode.
    //
    // Transcribed from a genuine registered install (this project's own dev box), not
    // invented -- every field below was read back out of its live registry, including
    // FMO's single-element MULTI_SZ ASProtect licence blob, which sits under
    // InprocServer32 exactly as Tetra Master's does.
    { "0004", "{94603A98-0067-4F41-89BC-7F89304D6E28}", NULL,
      "FMOEntry Class", "FMO.FMOEntry.1", "FMO.FMOEntry",
      "{0308BFE0-5D87-4397-9838-A4BF023C98CA}", NULL,
      "FrontMissionOnline.dll", "Apartment",
      "Bzo9b]C31Ab3+Ch,fSDC>EAgH2Ke,o@Y3Bt,j,p6r", NULL,
      NULL, 0 },
    { "0011", "{0DD61D2C-4C17-464C-AEA3-2D248DC89C86}", NULL,
      "FantasyEarthCom Class", "FE_Client_Com.FantasyEarthCom.1", "FE_Client_Com.FantasyEarthCom",
      "{9D401B32-8543-4C9F-ADD9-C2EE43E31136}", NULL,
      "FE_Client.dll", "Apartment", NULL, NULL,
      NULL, 0 },
    // 0015 FFXI Test Server. Every field here is transcribed from the .rgs script embedded
    // in the test build's own FFXi.dll (an ATL server -- its REGISTRY resource is the
    // authority, no guessing). Two things differ from retail FFXI and both are verbatim,
    // not typos: the class GUID is {6D5C9FF8...} where retail is {989D790D...}, and the
    // test build uses ITS OWN CLSID as the TypeLib GUID, where retail has a separate
    // {989D7900...}. No ThreadingModel, no AppID, no Programmable subkey, same as retail.
    //
    // IMPORTANT: THE PROGID COLLIDES WITH RETAIL FFXI, AND THAT IS FINE -- here is why, so nobody
    // re-opens it. Both DLLs' rgs register the SAME names: FFXi.FFXiEntry(.1) and
    // FFXi.FxFileManager(.1), pointing at different CLSIDs. `regsvr32` on the test DLL
    // would therefore repoint HKCR\FFXi.FFXiEntry.1\CLSID away from retail. We never do,
    // for two independent reasons:
    //
    //   1. repair_comclass writes ONLY the CLSID subtree -- HKLM\SOFTWARE\Classes\CLSID\
    //      {guid}\{default,InprocServer32,ProgID,VersionIndependentProgID,TypeLib}. It
    //      never creates HKCR\<ProgID>\CLSID at all, so the colliding key is not written
    //      and the two classes coexist. This is a reason to PREFER regfix over regsvr32
    //      here: regsvr32 runs the whole rgs, and its DllUnregisterServer would later
    //      delete ProgID keys that retail FFXI shares.
    //   2. Nothing resolves the name->CLSID direction anyway. `CLSIDFromProgID` is
    //      imported by ZERO binaries in the stack -- checked app.dll (unpacked), polcore
    //      .dll (unpacked), pol.exe, PolContents.dll, both polboot.exe builds, FFXiMain
    //      .dll and FFXi.dll. The only COM-registry call in the launch path is
    //      ProgIDFromCLSID, which goes CLSID->ProgID and reads under the CLSID key. The
    //      "FFXi.FFXiEntry" strings that do exist are the rgs script text inside FFXi.dll
    //      itself, not a runtime lookup.
    { "0015", "{6D5C9FF8-2915-4ABA-AB47-D397EC8EC907}", NULL,
      "FFXiEntry Class", "FFXi.FFXiEntry.1", "FFXi.FFXiEntry",
      "{6D5C9FF8-2915-4ABA-AB47-D397EC8EC907}", NULL,
      "FFXi.dll", NULL, NULL, NULL, NULL, 0 },
    // 0015's second class -- THE missing key that kept the Test Client dead (hand-written
    // first, folded in here). Every field is
    // now confirmed by SE's own recovered MSI: Class table row {56AA0ECF} ->
    // FFXi.dll / FFXi.FxFileManager.1, and its _ISComExtract Registry row gives TypeLib =
    // {6D5C9FF8} (the test build reuses its entry CLSID as its typelib id). No
    // ThreadingModel, same as every FFXi.dll class.
    { "0015", "{56AA0ECF-A1AD-4EF7-9DF2-3DA175443687}", NULL,
      "FxFileManager Class", "FFXi.FxFileManager.1", "FFXi.FxFileManager",
      "{6D5C9FF8-2915-4ABA-AB47-D397EC8EC907}", NULL,
      "FFXi.dll", NULL, NULL, NULL, NULL, 0 },
};

// A title may register SEVERAL classes from one DLL (FFXi.dll carries FFXiEntry AND
// FxFileManager, and the entry object cannot be built without the file manager), so
// everything below iterates every row with the title's id rather than taking the first.
// "Registered" for a title means ALL of its classes are.

// 0 = JP (SOFTWARE\PlayOnline), 1 = US, 2 = EU -- the same order as app.dll's table at
// 0x4D073F0 and polcore's SetAreaCode registry-root table, so "which hive won" and
// "which class the client will ask for" cannot drift apart.
static int hive_region()
{
    if (!g_hive) return 1;
    if (strstr(g_hive, "PlayOnlineEU")) return 2;
    if (strstr(g_hive, "PlayOnlineUS")) return 1;
    return 0;
}

static const char* comclass_guid(const ComClass* c)
{
    return (hive_region() == 0 && c->clsid_jp) ? c->clsid_jp : c->clsid;
}

static bool create_key(const char* subkey)
{
    HKEY h; DWORD disp;
    if (rcke(HKEY_LOCAL_MACHINE, subkey, 0, NULL, 0, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
        return false;
    rck(h);
    return true;
}

// REG_MULTI_SZ holding one string. RegSetValueExA widens it for us, so the stored
// value matches a genuine install exactly (an ANSI .reg imported through wine does
// NOT -- wine widens the hex bytes a second time and the blob comes out doubled).
static bool set_multisz(const char* subkey, const char* name, const char* val)
{
    size_t n = strlen(val);
    char buf[512];
    if (n + 2 > sizeof(buf)) return false;
    memcpy(buf, val, n); buf[n] = 0; buf[n+1] = 0;

    HKEY h; DWORD disp;
    if (rcke(HKEY_LOCAL_MACHINE, subkey, 0, NULL, 0, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
        return false;
    LONG r = rsv(h, name, 0, REG_MULTI_SZ, (const BYTE*)buf, (DWORD)(n + 2));
    rck(h);
    if (r == ERROR_SUCCESS) {
        FILE* f = fopen(g_sidecar, "a");
        if (f) { fprintf(f, "%s|%s\n", subkey, name); fclose(f); }
    }
    return r == ERROR_SUCCESS;
}

// Is the class already registered as far as the enumerator is concerned? That is
// precisely "does ProgIDFromCLSID succeed", i.e. does CLSID\{guid}\ProgID have a
// default value -- not "does the CLSID key exist".
static bool comclass_registered(const ComClass* c)
{
    char sub[256];
    _snprintf_s(sub, sizeof(sub), _TRUNCATE, "SOFTWARE\\Classes\\CLSID\\%s\\ProgID", comclass_guid(c));
    return value_exists(sub, "");
}

// Every class this title needs, registered? True when the title has no rows at all.
static bool comclasses_all_registered(const char* id)
{
    for (int i = 0; i < _countof(g_comclasses); i++)
        if (strcmp(g_comclasses[i].id, id) == 0 && !comclass_registered(&g_comclasses[i]))
            return false;
    return true;
}

static void register_comclass(const ComClass* c, const char* installdir)
{
    if (comclass_registered(c)) return;      // silent: the normal case on a real install

    // Never register a class whose server is not there -- that turns "invisible" into
    // "visible and fails on load", which is strictly worse to debug.
    char dll[MAX_PATH];
    _snprintf_s(dll, sizeof(dll), _TRUNCATE, "%s\\%s", installdir, c->dll);
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        rep("      COM class NOT registered: %s is not in that folder", c->dll);
        return;
    }

    const char* guid = comclass_guid(c);
    const int   jp   = (hive_region() == 0);
    char root[256], sub[320];
    _snprintf_s(root, sizeof(root), _TRUNCATE, "SOFTWARE\\Classes\\CLSID\\%s", guid);

    create_key(root);
    set_sz(root, "", c->classname);
    if (c->appid) set_sz(root, "AppID", c->appid);

    _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\InprocServer32", root);
    set_sz(sub, "", dll);
    if (c->threading) set_sz(sub, "ThreadingModel", c->threading);
    const char* lic = jp ? c->licence_jp : c->licence;
    if (lic) set_multisz(sub, "InprocServer32", lic);

    _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\ProgID", root);
    set_sz(sub, "", c->progid);
    _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\VersionIndependentProgID", root);
    set_sz(sub, "", c->viprogid);
    _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\TypeLib", root);
    set_sz(sub, "", (jp && c->typelib_jp) ? c->typelib_jp : c->typelib);
    if (c->programmable) {
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\Programmable", root);
        create_key(sub);
    }

    if (comclass_registered(c))
        rep("      COM class REGISTERED %s -> %s", guid, c->dll);
    else
        rep("      COM class write FAILED (%lu) -- need elevation?", GetLastError());
}

static void repair_comclass(const TitleReg* t, const char* installdir)
{
    for (int i = 0; i < _countof(g_comclasses); i++)
        if (strcmp(g_comclasses[i].id, t->id) == 0)
            register_comclass(&g_comclasses[i], installdir);
}

// Why a candidate folder is not usable -- the distinction that matters, because a
// present-but-file.txt-less folder (the Friend List ships none) is a DIFFERENT problem
// from a folder that is not there at all, and only one of them is regfix's to fix.
static const char* folder_verdict(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES)     return "no such folder";
    if (!(a & FILE_ATTRIBUTE_DIRECTORY))  return "exists but is not a folder";
    char ft[MAX_PATH]; _snprintf_s(ft, sizeof(ft), _TRUNCATE, "%s\\file.txt", path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(ft, GetFileExInfoStandard, &fa)) return "folder is there but has NO file.txt";
    if ((fa.nFileSizeLow | fa.nFileSizeHigh) == 0)             return "folder is there but file.txt is EMPTY";
    return NULL;   // usable
}

// The whole scan. apply=0 reports without touching the registry; apply=1 writes.
// Independent of [regfix] enable, so the dialog can run it on demand.
int regfix_scan(char* out, size_t cch, int apply)
{
    g_rep = out; g_repleft = cch;
    if (out && cch) *out = 0;

    if (!resolve_reg()) { rep("advapi32 registry API unavailable -- cannot continue"); g_rep = NULL; return -1; }
    sidecar_path(g_inipath, g_sidecar, sizeof(g_sidecar));

    // Refuse BEFORE writing anything. An unelevated process (every legacy PlayOnline
    // binary) has its HKLM writes silently redirected to the per-user VirtualStore with
    // ERROR_SUCCESS -- so every "REGISTERED / CORRECTED" line below would be a FALSE
    // pass and the elevated client that launches titles, reading real HKLM, would still
    // see nothing (the "a fix that never reached the install" class). Report-only
    // (apply=0) is read-only and stays allowed. Same guard gamecfg.cpp uses.
    if (apply && polshim_token_virtualized()) {
        rep("REFUSED: UAC registry virtualization is ON for this process. An HKLM write");
        rep("would land in the per-user VirtualStore and the title would never read it.");
        rep("Nothing was written -- run the Viewer elevated and repair again.");
        logf("[regfix] REFUSED apply: token virtualization on -- an HKLM write would go to "
             "the per-user VirtualStore, not real HKLM. Nothing written.");
        g_rep = NULL;
        return -1;
    }

    if (!detect_hive()) {
        rep("NO registered Viewer found (no InstallFolder\\1000 in any PlayOnline hive).");
        rep("regfix locates every other title RELATIVE to the Viewer's folder, so it");
        rep("cannot do anything until the Viewer itself is registered.");
        g_rep = NULL; return -1;
    }
    rep("hive   HKLM\\%s", g_hive);   // g_hive already begins "SOFTWARE\"
    rep("root   %s   (parent of the registered Viewer folder)", g_sqroot);

    char extra[MAX_PATH] = ""; wchar_t we[MAX_PATH] = L"";
    ini_str(L"regfix", L"root", L"", we, _countof(we), g_inipath);
    if (we[0]) {
        WideCharToMultiByte(CP_ACP, 0, we, -1, extra, sizeof(extra), NULL, NULL);
        rep("extra  %s   ([regfix] root=)", extra);
    } else
        rep("extra  (none)  -- set [regfix] root= if your titles live elsewhere");
    rep("");

    char ifsub[256], csub[256], isub[256], insub[256], prsub[256];
    _snprintf_s(ifsub, sizeof(ifsub), _TRUNCATE, "%s\\InstallFolder",  g_hive);
    _snprintf_s(csub,  sizeof(csub),  _TRUNCATE, "%s\\ContentsCLSID", g_hive);
    _snprintf_s(isub,  sizeof(isub),  _TRUNCATE, "%s\\ContentsIID",   g_hive);
    _snprintf_s(insub, sizeof(insub), _TRUNCATE, "%s\\Interface",     g_hive);
    _snprintf_s(prsub, sizeof(prsub), _TRUNCATE, "%s\\Product",       g_hive);

    int repaired = 0, already = 0, missing = 0, incomplete = 0, fixed = 0;
    for (int i = 0; i < _countof(g_titles); i++) {
        const TitleReg* t = &g_titles[i];
        // IMPORTANT: This used to report "already registered" on InstallFolder ALONE, which is how
        // a title with no Interface\NNNN was called healthy while the client reported it
        // not installed / version unknown. Check every key the title actually needs.
        if (value_exists(ifsub, t->id)) {
            char miss[256] = "";
            if (t->needs_interface && !value_exists(insub, t->id)) strcat_s(miss, " Interface");
            if (t->needs_product   && !value_exists(prsub, t->id)) strcat_s(miss, " Product");
            if (t->clsid && !value_exists(csub, t->id))            strcat_s(miss, " ContentsCLSID");
            if (t->iid   && !value_exists(isub, t->id))            strcat_s(miss, " ContentsIID");
            // The COM class the enumerator asks ProgIDFromCLSID about. Missing it is
            // not "registered but cannot launch" -- the title is dropped from the list
            // outright, so it has to count as incomplete like anything else.
            if (!comclasses_all_registered(t->id))                 strcat_s(miss, " ComClass");
            // IMPORTANT: REGISTERED IS NOT INSTALLED, and reporting "OK" on the registry alone
            // is how this said everything was fine while three titles were unusable.
            // The client's LAST Check Files gate opens <installdir>\file.txt and
            // requires it NON-EMPTY (patches.cpp), and InstallFolder can perfectly well
            // point at a path that does not exist on this machine -- which is the normal
            // outcome when a hive is copied from another install. So follow the value.
            char reg[MAX_PATH] = "";
            HKEY hk;
            if (rok(HKEY_LOCAL_MACHINE, ifsub, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
                DWORD ty = 0, sz = sizeof(reg) - 1;
                if (rqv(hk, t->id, NULL, &ty, (LPBYTE)reg, &sz) == ERROR_SUCCESS && sz) {
                    reg[sz] = 0;
                    size_t rl = strlen(reg);
                    while (rl && (reg[rl-1] == '\\' || reg[rl-1] == '/')) reg[--rl] = 0;
                }
                rck(hk);
            }
            const char* fverdict = reg[0] ? folder_verdict(reg) : "InstallFolder is empty";
            if (fverdict) {
                rep("%s %-22s BROKEN -- %s", t->id, t->folder, fverdict);
                rep("      registry points at: %s", reg[0] ? reg : "(nothing)");
                rep("      The registry is fine; the FILES are the problem. Check Files");
                rep("      needs a non-empty file.txt in that exact folder, so the title");
                rep("      stays invisible/unlaunchable until the path and files agree.");
                incomplete++;
                continue;
            }
            // A registered title still gets its version stamp CHECKED. This is the case
            // that was skipped for every build before this one: the registry looked
            // complete, so nothing verified that Interface and patch.ver agree -- and
            // when they disagree the client calls the title not installed.
            if (!miss[0]) {
                rep("%s %-22s registered  (%s)", t->id, t->folder, reg);
                if (apply) {
                    repair_patchver(insub, t, reg);
                    // IMPORTANT: AND the GUIDs, and the FMO DLL. "Registered" was taken to mean
                    // "correct" here, which is exactly how a stock ContentsIID\0011 and a
                    // stock FrontMissionOnline.dll both survived on a machine this
                    // reported as complete. Presence is not correctness -- check the
                    // CONTENT of every value and file a title's launch depends on.
                    repair_contents_guids(csub, isub, t);
                    repair_fmo_iids(t, reg);
                } else {
                    rep("      (dry run -- press Repair to check/fix its version stamp,");
                    rep("       its ContentsCLSID/IID values and, for FMO, its DLL IIDs)");
                }
                already++;
                continue;
            }
            // Registered but not fully. This is the case the Deck kept hitting, so it
            // gets FIXED here rather than described -- apply=1 means repair, not report.
            rep("%s %-22s needs repair --%s", t->id, t->folder, miss);
            if (apply) {
                // reg[] is this title's install folder and is known good -- folder_verdict
                // passed above, which it cannot do on an empty path. Run BOTH repairs
                // unconditionally: each is a no-op when its own item is already right, and
                // gating the version-stamp check on "Interface is MISSING" is exactly the
                // mistake that let a title with a WRONG stamp through untouched.
                repair_patchver(insub, t, reg);
                repair_contents_guids(csub, isub, t);   // this path never wrote them at all
                repair_comclass(t, reg);
                repair_fmo_iids(t, reg);
                // Repaired IS NOT still-broken. Counting it as INCOMPLETE printed the
                // "registered enough to appear but not to launch" warning underneath the
                // very lines saying it had just been fixed, which reads as a failure.
                fixed++;
            } else {
                rep("      (dry run -- press Repair to fix this)");
                incomplete++;
            }
            continue;
        }

        char path[MAX_PATH]; const char* found = NULL; const char* why = NULL;
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_sqroot, t->folder);
        why = folder_verdict(path);
        if (!why) found = path;
        if (!found && extra[0]) {
            char p2[MAX_PATH]; _snprintf_s(p2, sizeof(p2), _TRUNCATE, "%s\\%s", extra, t->folder);
            const char* w2 = folder_verdict(p2);
            if (!w2) { strcpy_s(path, p2); found = path; why = NULL; }
        }
        if (!found) {
            rep("%s %-22s NOT registered -- %s", t->id, t->folder, why ? why : "not found");
            rep("      looked in %s\\%s", g_sqroot, t->folder);
            // Say what it would take, rather than leaving "not found" as a dead end.
            // Deliberately does NOT promise an install: our server does carry a tree for
            // these ids, but installing a title FROM NOTHING is a separate unproven
            // workstream ("stub install") -- SE's file.txt covers data
            // files, not the executables, so a tree alone is not a complete install.
            if (t->patch_port)
                rep("      to fix: copy this title's folder here, or point [regfix] root= at it.");
            missing++;
            continue;
        }

        char ival[MAX_PATH];
        _snprintf_s(ival, sizeof(ival), _TRUNCATE, t->trailing_slash ? "%s\\" : "%s", found);
        if (!apply) { rep("%s %-22s WOULD register -> %s", t->id, t->folder, ival); repaired++; continue; }

        if (!create_if_absent(ifsub, t->id, ival)) {
            rep("%s %-22s found at %s but the registry WRITE FAILED (%lu) -- need elevation?",
                t->id, t->folder, ival, GetLastError());
            continue;
        }
        repaired++;
        rep("%s %-22s REGISTERED -> %s", t->id, t->folder, ival);
        if (t->clsid) {
            rep("      + ContentsCLSID/IID (COM-launched title)");
            repair_contents_guids(csub, isub, t);
        }
        repair_patchver(insub, t, found);
        repair_comclass(t, found);
        repair_fmo_iids(t, found);
        rep("      NOTE Interface/Product not written (crypto-tied, v1) -- if it appears");
        rep("      but fails Check Files or launch, that pair is why.");
    }

    rep("");
    rep("%d registered now, %d repaired, %d complete already, %d INCOMPLETE, %d not found.",
        repaired, fixed, already, incomplete, missing);
    if (fixed) rep("A repaired title needs a Viewer RESTART before the client re-reads it.");
    if (incomplete) {
        rep("An INCOMPLETE title is registered enough to APPEAR but not to LAUNCH -- which");
        rep("is exactly the state that reads as installed in the menu and 'not installed'");
        rep("when you press Play.");
    }
    if (missing)
        rep("A title listed as not found needs its FILES on this machine, in a folder of "
            "exactly that name. If yours are elsewhere, set [regfix] root=<folder>.");
    if (repaired) rep("Restart the Viewer for the games menu to pick them up.");
    rep("Undo everything regfix added with [regfix] clean=1.");

    g_rep = NULL;
    return repaired;
}

// SE's title DLLs read their OWN InstallFolder from the BASE hive name "SOFTWARE\PlayOnline"
// regardless of region -- measured on Fantasy Earth 2026-08-19: FE_Client.dll opens
// HKLM\Software\PlayOnline\InstallFolder\0011 to SetCurrentDirectory into its game folder.
// But a Steam (FFXINA) install registers everything under the SUFFIXED "SOFTWARE\PlayOnlineUS"
// (detect_hive picks that as the active hive). On such an install FE's base-hive read returns
// nothing, its SetCurrentDirectory silently no-ops, and it crashes on the first RELATIVE
// resource path (Data\Window\Etc\copyright.tex -> deref of a failed load). regfix writes only
// the active hive, so mirror its InstallFolder\NNNN values into the base hive too. No-op when
// the active hive already IS the base name (a normal Windows install), and never clobbers an
// existing base value.
static void mirror_installfolder_to_base(void)
{
    if (!g_hive || _stricmp(g_hive, "SOFTWARE\\PlayOnline") == 0) return;
    char src[256];
    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\InstallFolder", g_hive);
    const char* dst = "SOFTWARE\\PlayOnline\\InstallFolder";
    int copied = 0;
    for (int i = 0; i < _countof(g_titles); i++) {
        const char* id = g_titles[i].id;
        HKEY hs;
        if (rok(HKEY_LOCAL_MACHINE, src, 0, KEY_READ, &hs) != ERROR_SUCCESS) return;
        char val[MAX_PATH]; DWORD ty = 0, sz = sizeof(val) - 1;
        LONG r = rqv(hs, id, NULL, &ty, (LPBYTE)val, &sz);
        rck(hs);
        if (r != ERROR_SUCCESS || !sz) continue;
        val[sz] = 0;
        copied += create_if_absent(dst, id, val);
    }
    if (copied)
        logf("[regfix] mirrored %d InstallFolder value(s) into the base HKLM\\SOFTWARE\\"
             "PlayOnline hive -- SE title DLLs (e.g. FE_Client) read the base name, but this "
             "Steam/FFXINA install registered under HKLM\\%s", copied, g_hive);
}


// --- EverQuest II: the App Paths key that makes the game findable -----------
//
// KEY: Measured 2026-08-25. LaunchPad's "Game Options" button runs EQ2Settings.exe
// and it puts up **"ERROR: can not find game."** -- and the install is NOT the
// problem: 5.5 GB of it is present, EverQuest2.exe included. What is missing is
// the lookup. EQ2's own EQ2\gameconfig.xml states it outright:
//
//     <RegKeyRoot>HKEY_LOCAL_MACHINE</RegKeyRoot>
//     <RegKeyPath>Software\Microsoft\Windows\CurrentVersion\App Paths</RegKeyPath>
//     <RegKeyName>EverQuest2.exe</RegKeyName>
//
// and that key is absent in BOTH registry views on this machine. Same disease as
// every other title here: "the game says it is not installed" is a registration
// hole, not a missing file (see repair_comclass above).
//
// WARNING: We are a 32-bit process, so this HKLM path auto-redirects to WOW6432Node --
// which is exactly where a 32-bit EQ2Settings.exe reads it, so the two agree by
// construction. WARNING: And an HKLM write from a NON-elevated process is silently
// VIRTUALISED into HKCU and reads back as success, so the failure log below says
// "need elevation?" in the same words the rest of this file uses. polshim_launch
// is requireAdministrator, so under the normal launch path this is elevated.
//
// Off unless [regfix] eq2_path= names the install, because nothing should write
// an App Paths entry for a game the machine may not have.
static void repair_eq2_apppaths()
{
    wchar_t wdir[MAX_PATH] = L"";
    ini_str(L"regfix", L"eq2_path", L"", wdir, _countof(wdir), g_inipath);
    if (!wdir[0]) return;

    char dir[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wdir, -1, dir, sizeof(dir), NULL, NULL);
    size_t n = strlen(dir);
    while (n && (dir[n - 1] == '\\' || dir[n - 1] == '/')) dir[--n] = '\0';

    char exe[MAX_PATH];
    _snprintf_s(exe, sizeof(exe), _TRUNCATE, "%s\\EverQuest2.exe", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        rep("[regfix] eq2_path=%s has no EverQuest2.exe -- not registering", dir);
        return;
    }

    static const char* SUB =
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\EverQuest2.exe";

    char cur[MAX_PATH] = "";
    if (read_sz_at(SUB, NULL, cur, sizeof(cur)) && _stricmp(cur, exe) == 0) {
        rep("[regfix] EverQuest II App Paths already correct (%s)", exe);
        return;
    }

    HKEY k = NULL;
    DWORD disp = 0;
    LONG rc = rcke(HKEY_LOCAL_MACHINE, SUB, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, &disp);
    if (rc != ERROR_SUCCESS) {
        rep("[regfix] EverQuest II App Paths: cannot open the key (%ld) -- need "
            "elevation?", rc);
        return;
    }
    LONG r1 = rsv(k, NULL,   0, REG_SZ, (const BYTE*)exe, (DWORD)strlen(exe) + 1);
    LONG r2 = rsv(k, "Path", 0, REG_SZ, (const BYTE*)dir, (DWORD)strlen(dir) + 1);
    rck(k);
    if (r1 == ERROR_SUCCESS && r2 == ERROR_SUCCESS)
        rep("[regfix] EverQuest II REGISTERED: App Paths\\EverQuest2.exe -> %s", exe);
    else
        rep("[regfix] EverQuest II App Paths write FAILED (%ld/%ld) -- need "
            "elevation?", r1, r2);
}

void regfix_run(const wchar_t* ini)
{
    wcsncpy_s(g_inipath, ini, _TRUNCATE);
    // Default 1 to MATCH iniheal's healed default for this key (iniheal.cpp). They
    // used to disagree (compiled 0 vs healed 1), masked only by iniheal running first
    // and writing enable=1 before this read -- reorder or skip iniheal and the whole
    // registration-repair layer silently disabled. Agreeing by construction removes
    // that trap.
    g_on      = GetPrivateProfileIntW(L"regfix", L"enable", 1, ini);
    int clean = GetPrivateProfileIntW(L"regfix", L"clean",  0, ini);
    if (!g_on && !clean) return;

    if (clean) {
        if (!resolve_reg()) { logf("[regfix] advapi32 registry API unavailable -- skipped"); return; }
        sidecar_path(ini, g_sidecar, sizeof(g_sidecar));
        regfix_clean();
        return;
    }
    regfix_scan(NULL, 0, 1);
    repair_eq2_apppaths();      // no-op unless [regfix] eq2_path= is set
    if (!g_sidecar[0]) sidecar_path(ini, g_sidecar, sizeof(g_sidecar));
    mirror_installfolder_to_base();
}
