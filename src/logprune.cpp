// logprune.cpp -- keep the per-pid log directory from growing without bound.
//
// The shim names its log <stem>.<pid>.<ext> because pol.exe fans out into several
// injected processes and a shared "w" log would truncate itself to nothing. Nothing
// ever removed the old ones, so a Steam Deck install accumulates hundreds of files --
// reported from the field, and it also makes finding the RIGHT log tedious, which
// matters because reading the right log is how most of this gets diagnosed.
//
// Own file, no dependencies: log.cpp pulls in the proxy engine, and the test harness
// deliberately does not link that.

#include "polshim.h"

int log_prune(const wchar_t* dir, const wchar_t* logname, int keep, int dry_run)
{
    if (keep <= 0 || !dir || !logname) return 0;

    // Pattern from the CONFIGURED name, so it can never match an unrelated file:
    //   polshim.log -> polshim.*.log      mylog -> mylog.*
    wchar_t pat[MAX_PATH];
    wcscpy_s(pat, dir);
    const wchar_t* d = wcsrchr(logname, L'.');
    if (d) {
        wchar_t stem[MAX_PATH];
        wcsncpy_s(stem, logname, (size_t)(d - logname));
        wcscat_s(pat, stem); wcscat_s(pat, L".*"); wcscat_s(pat, d);
    } else {
        wcscat_s(pat, logname); wcscat_s(pat, L".*");
    }

    struct Ent { wchar_t name[MAX_PATH]; ULONGLONG t; };
    static Ent ents[1024];
    int n = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n >= (int)(sizeof(ents) / sizeof(ents[0]))) break;
        wcscpy_s(ents[n].name, fd.cFileName);
        ents[n].t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32)
                  |  fd.ftLastWriteTime.dwLowDateTime;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Newest first (insertion sort -- n is small), then drop everything past `keep`.
    for (int i = 1; i < n; i++) {
        Ent k = ents[i]; int j = i - 1;
        while (j >= 0 && ents[j].t < k.t) { ents[j + 1] = ents[j]; j--; }
        ents[j + 1] = k;
    }

    int removed = 0;
    for (int i = keep; i < n; i++) {
        wchar_t full[MAX_PATH];
        wcscpy_s(full, dir); wcscat_s(full, ents[i].name);
        if (dry_run || DeleteFileW(full)) removed++;
    }
    return removed;
}
