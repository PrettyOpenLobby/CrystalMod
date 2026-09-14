// com_allow.h -- the IID allow-list for the TYPED COM proxy engine, each with its
// EXACT vtable slot count. Only interfaces listed here are ever wrapped; every
// other interface passes through untouched. That scoping -- plus sizing from the
// typelib instead of a heuristic probe -- is what makes the typed engine safe
// where the generic one (proxy.cpp) was not: it never wraps the transient system
// COM (dxdiagn/Defender/WMI) that crashed the Viewer.
//
// GENERATED from the components' embedded MIDL typelibs by build\tlbdump.exe
// (src/tlbdump.cpp), 2026-08-14, with manual OVERRIDES where a typelib is
// incomplete. Regenerate after a client update:
//     build\tlbdump.exe "<...>\viewer\com\polcore.dll"
//     build\tlbdump.exe "<...>\viewer\com\app.dll"
//     build\tlbdump.exe "<...>\viewer\contents\PolContents.dll"
//     build\tlbdump.exe "<...>\FantasyEarth\FE_Client.dll"      (says 3 -- override to 4)
//     build\tlbdump.exe "<...>\TetraMaster\TM.dll"
// TYPEATTR.cbSizeVft/4 is the slot count (own methods + inherited IUnknown).
#pragma once
#include <windows.h>

struct ComAllow { GUID iid; int slots; const char* name; const char* src; };

static const ComAllow g_com_allow[] = {
    // NOTE: do NOT add IUnknown here. It was tried as a "bridge" (pol.exe does
    // CoCreateInstance(clsid, IUnknown) then QIs for the typed interface, so
    // wrapping the IUnknown result would let us follow the QI). It is TOO INVASIVE:
    // the client passes that IUnknown between polcore and app.dll during early
    // startup, tripped on the wrapped pointer, concluded "new install", and
    // REWROTE login_w.bin as a fresh new-user file -- destroying the saved account
    // (recovered 2026-08-15 from a backup). Reaching IPOLCoreCom needs a
    // non-invasive path (observe the factory-created object's QI without handing
    // the client a wrapped IUnknown), NOT this. Test any such change on a DISPOSABLE
    // install copy, never the primary with a real account.

    // IClassFactory -- the OLE standard factory (IUnknown + CreateInstance +
    // LockServer). Every DllGetClassObject hands one back; it is the entry point
    // through which the typed engine reaches the component's real interface.
    { { 0x00000001,0x0000,0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } }, 5,
      "IClassFactory",    "OLE standard" },

    // The Viewer's three own components (exact, from their typelibs).
    { { 0x6D365D27,0x4999,0x4BC5, { 0xAA,0xDF,0x51,0x3E,0xF0,0xE7,0xB4,0x38 } }, 4,
      "IPolContentsCom",  "PolContents.tlb (== patched IFMOEntry, slot 3 = GameStart)" },
    { { 0xE0516654,0xEF77,0x435D, { 0xAA,0x7D,0x50,0xD2,0xC0,0x69,0xCE,0x34 } }, 32,
      "IPOLCoreCom",      "polcore.tlb (US region IID)" },
    { { 0xA546265E,0x8EB1,0x4444, { 0xA3,0x79,0x7D,0x12,0x67,0x53,0x38,0xC3 } }, 20,
      "IPolAppCom",       "app.tlb" },

    // Content-title entry interfaces.
    { { 0x6560D1E0,0xBA0D,0x4877, { 0xBF,0xB6,0xA4,0x38,0x52,0x9E,0x97,0x01 } }, 4,
      "IFantasyEarthCom", "OVERRIDE: FE_Client.tlb says 3 (IUnknown-only, incomplete); "
                          "pol.exe calls GameStart at slot 3, so the real vtable is 4" },
    { { 0x6C01DADF,0x0A86,0x4C82, { 0xAD,0xF8,0xF1,0xB1,0xE2,0xB1,0xA0,0x59 } }, 7,
      "ITetraMaster",     "TM.tlb (dual/dispatch)" },
};

// Returns the slot count for an allowed IID, or 0 if the interface is not on the
// list (i.e. must NOT be wrapped -- pass the raw pointer through).
static inline int com_allow_slots(REFIID iid) {
    for (int i = 0; i < (int)(sizeof(g_com_allow) / sizeof(g_com_allow[0])); i++)
        if (IsEqualGUID(iid, g_com_allow[i].iid)) return g_com_allow[i].slots;
    return 0;
}
static inline const char* com_allow_name(REFIID iid) {
    for (int i = 0; i < (int)(sizeof(g_com_allow) / sizeof(g_com_allow[0])); i++)
        if (IsEqualGUID(iid, g_com_allow[i].iid)) return g_com_allow[i].name;
    return NULL;
}
