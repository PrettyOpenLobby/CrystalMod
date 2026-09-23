// polinject -- in-process interposition, nothing touched on disk.
//
// The Viewer restores any modified component from its patchfiles copy at
// startup, so file replacement cannot survive a launch. This reaches the same
// place without disk changes, and covers the two things a naive GetProcAddress
// hook misses:
//
//   1. EAT patching. We patch the target component's *export table* entry for
//      DllGetClassObject when the module loads. Every resolution path -- plain
//      GetProcAddress, ntdll!LdrGetProcedureAddress used by COM activation,
//      anything -- reads the EAT, so all of them land on our thunk. (A raw
//      GetProcAddress IAT hook misses the COM path, which is exactly how the
//      Viewer obtains polcore.)
//
//   2. Child-process propagation. pol.exe spawns the real Viewer as a child.
//      We IAT-hook CreateProcessW/A so every process we're in injects its
//      children too; coverage becomes recursive from the launcher down.
//
// No inline hooks and no length disassembler anywhere: exports and imports are
// patched by value, and LdrRegisterDllNotification catches modules (and thus
// EAT targets) that appear after we're in.
#include "polshim.h"
#include "profiles.h"
#include "fmokey.h"
#include <tlhelp32.h>
#include <winternl.h>
#include <objbase.h>       // CLSIDFromString, for the [replace] map
#include <wincrypt.h>      // CryptoAPI MD5, for the [patch] noupdate POLP checksum

int   g_verbose   = 1;
int   g_max_slots = 128;

static HMODULE g_self = NULL;
static wchar_t g_self_path[MAX_PATH];
static char    g_targets[8][64];
static int     g_ntargets = 0;
static BYTE*   g_thunks = NULL;
static size_t  g_thunk_used = 0;

typedef HRESULT (__stdcall *PFN_GCO)(REFCLSID, REFIID, void**);

struct ModHook {
    HMODULE mod;
    PFN_GCO real;      // original DllGetClassObject (from the EAT, pre-patch)
    void*   thunk;
    char    name[64];
};
static ModHook g_mods[8];
static int     g_nmods = 0;
static CRITICAL_SECTION g_mlock;

// --- substitution -----------------------------------------------------------
//
// Interposition wraps the genuine object. Substitution replaces it outright:
// for a CLSID listed in the ini's [replace] section we load our own component
// and serve the activation from it, and the original component's class object
// is never created at all.
//
// This works because the Viewer reaches every component through
// DllGetClassObject, which we already own via the EAT patch. It does not matter
// that the Viewer loads its DLLs by path rather than by CLSID -- by the time
// anything asks for a class object, we are the export it calls.
struct Replacement {
    CLSID   clsid;
    char    dll[MAX_PATH];
    HMODULE mod;
    PFN_GCO gco;
    bool    failed;
};
static Replacement g_repl[8];
static int         g_nrepl = 0;

// [polshim] comproxy: wrap COM interfaces with the generic proxy engine
// (proxy.cpp). OFF by default -- it is a tracing/porting tool (needs [replace] or
// an active COM-debugging session), and its heuristic vtable sizing plus the lack
// of COM-lifetime tracking make it crash when it wraps transient SYSTEM COM
// (dxdiagn / Defender / WMI) that a target module merely touches at startup. Set
// comproxy=1 only when interposing or tracing COM.
static int g_comproxy = 0;

static bool is_target(const char* name)
{
    for (int i = 0; i < g_ntargets; i++)
        if (_stricmp(name, g_targets[i]) == 0) return true;
    return false;
}

static void module_leaf(HMODULE h, char* out, size_t cb)
{
    char path[MAX_PATH] = "";
    GetModuleFileNameA(h, path, MAX_PATH);
    const char* leaf = strrchr(path, '\\');
    strcpy_s(out, cb, leaf ? leaf + 1 : path);
}

// --- per-module work done on a module's first activation ---------------------
//
// Arming INT3 probes, patching unpacked .text and swapping polcore's common-
// function-table slots all want the same window: the module is loaded and
// unpacked, and we are at a safe pre-login point. That window is the module's
// first DllGetClassObject -- and it is reached whether the class object came
// from the module itself or from a [replace] substitute, so this runs on BOTH
// paths.
//
// It used to sit inline after the fall-through call ONLY. A registered
// replacement returns early, so it silently disabled every one of these for
// that module: with polcore served from polcore_re.dll a live log showed
// `[replace] polcore.dll ... SERVED BY ...` and then zero [pf] lines -- the FMO
// livelock fix never attached and said nothing, indistinguishable from a build
// compiled without it. polcore is asked for exactly one CLSID, the one being
// replaced, so there was no second activation to catch it either.
//
// Calling this from the replacement path is correct, not just convenient:
// `m->mod` is the GENUINE module on both paths, and polcore_re is a forwarding
// shell that hands back the GENUINE common function table with its own ported
// entries patched into it in place -- one live table, this module's. The slots
// taken here (polfetch 1134/1135, fmokey 1003/936) are disjoint from the ones
// polcore_re ports (128-130, 216, 1079, 1499-1501).
//
// Ordering: on the replacement path the genuine component has NOT been activated
// yet -- polcore_re resolves it lazily, on CreateInstance. That does not matter
// for the table, which is ordinary initialized .data: polcore.dll's on-disk
// .data already holds slot 1134 = base+0x497F0 and 1135 = base+0x497D0, only
// .text is stripped (raw size 0, unpacked by the POL1 stub at load). Every call
// below verifies what it finds before writing regardless, so a build that does
// not match is left alone -- loudly.
extern int g_dbgstring;                     // defined further down, by the ODS hooks
static bool g_dbgstring_explicit = false;   // did the ini NAME it? set in startup()

// The title components -- the modules whose arrival means A GAME IS NOW RUNNING.
// Deliberately a small closed list rather than "anything not ours": the point is to
// listen while a game runs, not to log the whole process.
static bool is_title_module(const char* name)
{
    static const char* kTitles[] = {
        "FE_Client.dll", "FrontMissionOnline.dll", "TM.dll", "FFXiMain.dll",
    };
    for (int i = 0; i < _countof(kTitles); i++)
        if (_stricmp(name, kTitles[i]) == 0) return true;
    return false;
}

static void apply_module_hooks(ModHook* m)
{
    // The Check-Files accept list lives in app.dll's unpacked .text, so it
    // cannot be patched on disk.
    if (patches_enabled())
        patches_apply_module((void*)m->mod, m->name);

    // Record what the Viewer SAYS. Same trigger as the byte patches above (app.dll,
    // once it is unpacked and present) and the same discipline: verify the site, refuse
    // a build that does not match, never patch on faith.
    uitrace_apply_module((void*)m->mod, m->name);

    // FMO's livelock fix swaps two entries of polcore's common function table.
    // Verified against the expected addresses before writing.
    if (polfetch_enabled())
        polfetch_apply_module((void*)m->mod, m->name);

    // Same table: the FMO session-key tap swaps two more slots. Logging only,
    // off by default, and it verifies both slots before writing (see fmokey.cpp).
    if (fmokey_wants_modules())
        fmokey_apply_module((void*)m->mod, m->name);

    // TM's gW path parser fix lives in TM.dll's unpacked .text; byte-verified,
    // no-op unless the site matches.
    if (tmpathfix_enabled())
        tmpathfix_apply_module((void*)m->mod, m->name);

    // The login-hang fix: a trampoline hook on polcore's single auth key-setup.
    // Same discipline -- verified against the expected prologue before writing.
    if (authkey_wants_modules())
        authkey_apply_module((void*)m->mod, m->name);

    // NOTE: a TITLE's own DLL is never armed here. This function only ever sees
    // the shim's own COM targets (polcore.dll, app.dll, PolContents.dll) -- a
    // title's DLL never reaches [gco] -- so a title hook placed here would
    // silently never fire. Those live in arm_title_hooks_late().
}

// --- the replacement DllGetClassObject --------------------------------------

static HRESULT __stdcall gco_impl(ModHook* m, REFCLSID rclsid, REFIID riid, void** ppv)
{
    char a[64], b[64];
    logf("[gco] %s clsid=%s iid=%s", m->name,
         iid_name(rclsid, a, sizeof(a)), iid_name(riid, b, sizeof(b)));

    // Substitution takes priority: if this CLSID is ours, the original never
    // gets asked. Loaded lazily so a missing or broken replacement costs
    // nothing until the component is actually activated.
    for (int i = 0; i < g_nrepl; i++) {
        if (!IsEqualCLSID(rclsid, g_repl[i].clsid)) continue;
        Replacement* r = &g_repl[i];
        if (!r->gco && !r->failed) {
            r->mod = LoadLibraryExA(r->dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            r->gco = r->mod ? (PFN_GCO)GetProcAddress(r->mod, "DllGetClassObject") : NULL;
            if (!r->gco) {
                r->failed = true;
                logf("[replace] cannot use %s (err=%lu) -- falling back to the original",
                     r->dll, GetLastError());
            } else {
                logf("[replace] loaded %s at %p", r->dll, (void*)r->mod);
            }
        }
        if (r->gco) {
            HRESULT rhr = r->gco(rclsid, riid, ppv);
            logf("[replace] %s clsid=%s SERVED BY %s -> 0x%08lX obj=%p",
                 m->name, a, r->dll, rhr, ppv ? *ppv : NULL);
            // The substitute serves the object, but the module's own hooks
            // still have to be applied or [replace] silently disables them.
            // No comproxy wrap here: *ppv is already ours.
            apply_module_hooks(m);
            log_flush();
            return rhr;
        }
        break;
    }

    HRESULT hr = m->real(rclsid, riid, ppv);
    // COM interposition of the returned class factory. [polshim] comproxy is a MODE:
    //   0 = off (default): the client gets the genuine object untouched.
    //   1 = LEGACY generic engine (proxy.cpp): probe-sized synthetic vtable, wraps
    //       every out-param. Fragile -- its heuristic sizing + lack of COM-lifetime
    //       tracking crash the Viewer when it wraps the transient system COM
    //       (dxdiagn/Defender/WMI) pol.exe touches at startup. Kept only for A/B.
    //   2 = TYPED engine (typed_wrap, com_allow.h): IID-scoped (only allow-listed
    //       interfaces are wrapped), typelib-sized (exact vtable, no probe), and
    //       lifetime-tracked (retired on Release->0). Safe by construction where 1
    //       was not. `riid` is what the factory was asked for (usually IClassFactory).
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (g_comproxy == 2)      *ppv = typed_wrap(*ppv, riid);
        else if (g_comproxy == 1) *ppv = proxy_wrap(*ppv, KIND_CLASSFACTORY, m->name);
    }

    logf("[gco] %s -> 0x%08lX obj=%p (comproxy=%d)", m->name, hr, ppv ? *ppv : NULL, g_comproxy);

    apply_module_hooks(m);

    log_flush();
    return hr;
}

//   B8 <ctx>   mov eax, ModHook*      (EAX is scratch at __stdcall entry)
//   E9 <rel32> jmp generic_gco
__declspec(naked) static void __stdcall generic_gco()
{
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp + 16]   // ppv
        push dword ptr [ebp + 12]   // riid
        push dword ptr [ebp + 8]    // rclsid
        push eax                    // ModHook*
        call gco_impl               // __stdcall: cleans its own 16 bytes
        pop  ebp
        ret  12
    }
}

static void* thunk_for(ModHook* m)
{
    if (!g_thunks) {
        g_thunks = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
        g_thunk_used = 0;
    }
    BYTE* t = g_thunks + g_thunk_used;
    g_thunk_used += 16;
    t[0] = 0xB8;                                            // mov eax, imm32
    *(DWORD*)(t + 1) = (DWORD)(UINT_PTR)m;
    t[5] = 0xE9;                                            // jmp rel32
    *(DWORD*)(t + 6) = (DWORD)((BYTE*)generic_gco - (t + 10));
    return t;
}

// --- EAT patching -----------------------------------------------------------
//
// Rewrite the module's exported DllGetClassObject RVA to point at our thunk.
// EAT entries are 32-bit RVAs added to the module base; in a 32-bit address
// space (thunk - base) as u32 rounds-trips correctly even when the thunk sits
// below the base, because base + RVA wraps mod 2^32 back to the thunk.

static void eat_patch(HMODULE mod)
{
    if (!mod) return;
    char leaf[64];
    module_leaf(mod, leaf, sizeof(leaf));
    if (!is_target(leaf)) return;

    EnterCriticalSection(&g_mlock);
    // A target module can be UNLOADED and re-LOADED inside one session: every
    // failed title launch does exactly that to app.dll, and Windows readily hands
    // the fresh mapping the SAME base it just freed. So an HMODULE match cannot
    // distinguish "already hooked" from "recycled base, brand-new image" -- and in
    // the second case the new image's EAT is untouched and its .text is unpacked
    // from scratch, so returning early leaves the module un-hooked and unpatched
    // for the rest of the session. Measured 2026-08-17 (polshim.546484.log): app.dll
    // loads three times, the third reuses base 0x10010000, gets no [eat] and no
    // [gco], and the beta-version gate that `filecheck_all` NOPs comes back.
    //
    // Match on the module NAME instead -- that survives both a same-base and a
    // relocated reload -- and settle it below against the one authoritative test,
    // which is whether OUR thunk is still sitting in the export slot.
    ModHook* prev = NULL;
    for (int i = 0; i < g_nmods; i++)
        if (_stricmp(g_mods[i].name, leaf) == 0) { prev = &g_mods[i]; break; }
    if (!prev && g_nmods >= _countof(g_mods)) { LeaveCriticalSection(&g_mlock); return; }

    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY& ed =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!ed.VirtualAddress) { LeaveCriticalSection(&g_mlock); return; }

    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + ed.VirtualAddress);
    DWORD* names = (DWORD*)(base + exp->AddressOfNames);
    WORD*  ords  = (WORD*)(base + exp->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* nm = (const char*)(base + names[i]);
        if (strcmp(nm, "DllGetClassObject") != 0) continue;

        WORD  fidx = ords[i];
        DWORD old_rva = funcs[fidx];

        // Still our thunk in the slot => the module really is the one we hooked
        // and it never went away. Nothing to do. Anything else -- including the
        // original export back in place at a base we have seen before -- is a
        // fresh image that needs hooking again.
        if (prev && prev->mod == mod && (BYTE*)base + old_rva == (BYTE*)prev->thunk) {
            LeaveCriticalSection(&g_mlock);
            return;
        }

        PFN_GCO real = (PFN_GCO)(base + old_rva);

        ModHook* m;
        if (prev) {
            // Re-load. Reuse the slot -- its thunk already carries this ModHook*
            // as its immediate, so it stays valid -- but re-read `real` out of the
            // NEW image; the old one's entry point is gone.
            m = prev;
            m->mod  = mod;
            m->real = real;
            logf("[eat] %s was re-loaded at %p -- its EAT held the original export "
                 "again, so this is a NEW image, not the one we hooked. Re-hooking; "
                 "any byte patch in its .text is gone with the old mapping.",
                 m->name, (void*)mod);
            // Bookkeeping that says "applied" refers to the mapping that just went
            // away. The fresh .text is unpacked from the file, so every site
            // belonging to this module is unpatched again no matter what we think.
            patches_forget_module(m->name);
        } else {
            m = &g_mods[g_nmods];
            m->mod  = mod;
            m->real = real;
            module_leaf(mod, m->name, sizeof(m->name));
            m->thunk = thunk_for(m);
        }

        DWORD new_rva = (DWORD)((BYTE*)m->thunk - base);
        DWORD prot;
        if (VirtualProtect(&funcs[fidx], sizeof(DWORD), PAGE_READWRITE, &prot)) {
            funcs[fidx] = new_rva;
            VirtualProtect(&funcs[fidx], sizeof(DWORD), prot, &prot);
            if (!prev) g_nmods++;
            logf("[eat] %s DllGetClassObject hooked (real=%p)", m->name, real);
        } else {
            logf("[warn] %s: VirtualProtect on EAT failed, err=%lu", m->name, GetLastError());
        }
        break;
    }
    LeaveCriticalSection(&g_mlock);
    log_flush();
}

// --- child-process injection ------------------------------------------------

typedef BOOL (WINAPI *PFN_CPW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
    LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *PFN_CPA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR,
    LPSTARTUPINFOA, LPPROCESS_INFORMATION);

static PFN_CPW real_CreateProcessW = NULL;
static PFN_CPA real_CreateProcessA = NULL;

// connect() -- the first lobby probe: every dial destination.
// WSAAPI is __stdcall. sockaddr_in bytes: +0 family, +2 port (network), +4 addr.
typedef int (WINAPI *PFN_CONNECT)(UINT_PTR, const void*, int);
static PFN_CONNECT real_connect = NULL;

static int WINAPI hook_connect(UINT_PTR s, const void* name, int namelen)
{
    if (name && namelen >= 8) {
        const BYTE* b = (const BYTE*)name;
        WORD family = (WORD)(b[0] | (b[1] << 8));
        unsigned port = (b[2] << 8) | b[3];          // network order -> host
        logf("[connect] sock=%p family=%u %u.%u.%u.%u:%u",
             (void*)s, family, b[4], b[5], b[6], b[7], port);
        log_flush();
        // Tag (or untag) this handle as the auth-band SESSION socket, whose
        // death is what the client shows as POL-0008. See sessionwatch.cpp --
        // it only records; nothing about this call changes.
        sessionwatch_note_connect(s, port);
    }
    return real_connect(s, name, namelen);
}

// socket() and bind() -- added 2026-08-14 for the FFXI world-login dead end.
//
// The world (map) channel is UDP, so it never calls connect(); the only trace it
// could leave through the existing hooks is a sendto, and there is none. That
// makes "FFXI never started the world connection" and "it started one and gave up
// before the first datagram" indistinguishable -- which is exactly the ambiguity
// blocking the POL-0001 chase.
//
// Static RE bounds the question: FFXiMain's world transport is one function
// (0x100EC440 at load base 0x10010000) which does socket(AF_INET, SOCK_DGRAM,
// IPPROTO_UDP) then bind(), and BOTH gates on the path to it -- 0x100EA500 and
// 0x100EDA20 -- are unconditional passes (`mov al,1; ret`, and a size compare
// that is 30 > 10). So if the chain is entered at all, socket+bind MUST appear.
// Seeing them moves the failure to the post-socket gates; not seeing them proves
// the chain is never entered, and that is a pure static question from there.
//
// SOCK_DGRAM is 2 and IPPROTO_UDP is 17, so a world socket logs as
// "af=2 type=2 proto=17". Every other FFXI socket is type=1 (TCP).
typedef UINT_PTR (WINAPI *PFN_SOCKET)(int, int, int);
typedef int (WINAPI *PFN_BIND)(UINT_PTR, const void*, int);
static PFN_SOCKET real_socket = NULL;
static PFN_BIND   real_bind   = NULL;

// Deferred arming for a TITLE module's hooks. The earlier arming points do not
// work for a packed title: it has no COM entry point the shim proxies (only
// polcore and app.dll reach [gco]), and at its own LDR load its .text is still
// blank -- the POL1/ASProtect stub unpacks it later, in DllMain. So arm from
// something the title ITSELF does (OutputDebugString, socket, a COM activation,
// any later module load): by then its image is unquestionably live. Every hook
// armed here is idempotent and byte-verified, so re-trying on each pass costs
// one memcmp until the site matches, and then nothing.
//
// NOTE the base is NOT stable across sessions, so everything here is keyed by
// RVA -- never hardcode a title VA.
// From d3d8hook.cpp -- starts the reservation-slot (+0x108) poller thread once.
// Kicked from this hot path (which fires at least at socket/ODS startup) because
// native Windows TM does not hit our d3d8 Present hook; the thread then polls
// continuously (the hot path itself is too sparse to poll from). Idempotent.
void tm_resv_diag_start(void);

static void arm_title_hooks_late(void)
{
    // The gW path-parser fix. TM.dll is ASProtect-packed, so the LDR/IAT path is
    // too early and the site only matches after the POL1 stub unpacks .text. It
    // MUST land before the resource glob, so it is armed from this hot path.
    if (tmpathfix_enabled()) {
        HMODULE tm = GetModuleHandleW(L"TM.dll");
        if (tm) {
            char leaf[64];
            module_leaf(tm, leaf, sizeof(leaf));
            tmpathfix_apply_module((void*)tm, leaf);
        }
    }

    // Kick the reservation-slot (+0x108) poller thread once. This hot path fires on
    // socket()/OutputDebugString -- reliable at startup but far too sparse to poll
    // from -- so it only STARTS the thread, which then reads every 200ms. Native
    // Windows TM never reaches the d3d8 Present hook, so this is the driver here.
    tm_resv_diag_start();
}

extern "C" void shim_arm_title_late(void) { arm_title_hooks_late(); }

static UINT_PTR WINAPI hook_socket(int af, int type, int proto)
{
    arm_title_hooks_late();
    UINT_PTR s = real_socket(af, type, proto);
    logf("[socket] af=%d type=%d proto=%d -> sock=%p%s",
         af, type, proto, (void*)s,
         (type == 2 /*SOCK_DGRAM*/) ? "   <-- UDP" : "");
    log_flush();
    return s;
}

static int WINAPI hook_bind(UINT_PTR s, const void* name, int namelen)
{
    int r = real_bind(s, name, namelen);
    if (name && namelen >= 8) {
        const BYTE* b = (const BYTE*)name;
        logf("[bind] sock=%p %u.%u.%u.%u:%u -> %d",
             (void*)s, b[4], b[5], b[6], b[7], (b[2] << 8) | b[3], r);
    } else {
        logf("[bind] sock=%p (namelen=%d) -> %d", (void*)s, namelen, r);
    }
    log_flush();
    return r;
}

// --- payload capture --------------------------------------------------------
//
// polcore's entire TCP client stack is one region -- polcore+0x3b28f holds the
// socket/connect/send/recv calls for the login and lobby session (the UDP calls
// around polcore+0xe290 are the separate GM-chat subsystem). Rather than
// breakpoint inside it, hook the four ws2_32 entry points by value, the same
// way connect is already hooked, and hex-dump what crosses.
//
// This is a wire-level capture: it sits below polcore's cipher, so payloads are
// still encrypted. Its value is the framing -- record boundaries, lengths and
// direction -- which is what the 40-byte lobby records need. Off unless
// capture=1, since it logs every byte of every session.

typedef int (WINAPI *PFN_SEND)(UINT_PTR, const char*, int, int);
typedef int (WINAPI *PFN_RECV)(UINT_PTR, char*, int, int);
typedef int (WINAPI *PFN_SENDTO)(UINT_PTR, const char*, int, int, const void*, int);
typedef int (WINAPI *PFN_RECVFROM)(UINT_PTR, char*, int, int, void*, int*);

// THE WSA FAMILY IS A SEPARATE SET OF ENTRY POINTS, and until 2026-08-15 we did
// not hook it -- so anything the client sent through WSASend/WSARecv was absent
// from the capture with no error and no gap marker. That was demonstrated, not
// theorised: in a two-account SE session the auth band captured perfectly while a
// received Message, a friend-list accept and a comment change the account holder
// WATCHED ARRIVE ON SCREEN could not be found in any stream under any session key.
//
// WARNING: OVERLAPPED CALLS CANNOT BE LOGGED HERE. When lpOverlapped is non-NULL the
// call returns WSA_IO_PENDING and the buffers are filled later, by the completion
// -- so the bytes are not ours to read at return. We log the synchronous case and
// COUNT the overlapped ones, so the log says how much it could not see instead of
// pretending the wire was quiet.
// This file includes windows.h, not winsock2.h, so WSABUF is not in scope --
// and pulling winsock2.h in after windows.h is the classic redefinition trap.
// The layout is fixed ABI (ULONG len; CHAR* buf), so declare it here.
typedef struct PolWsaBuf { unsigned long len; char* buf; } PolWsaBuf;

typedef int (WINAPI *PFN_WSASEND)(UINT_PTR, PolWsaBuf*, DWORD, LPDWORD, DWORD, void*, void*);
typedef int (WINAPI *PFN_WSARECV)(UINT_PTR, PolWsaBuf*, DWORD, LPDWORD, LPDWORD, void*, void*);

static PFN_WSASEND  real_wsasend  = NULL;
static PFN_WSARECV  real_wsarecv  = NULL;
static long g_wsa_overlapped = 0;

static PFN_SEND     real_send     = NULL;
static PFN_RECV     real_recv     = NULL;
static PFN_SENDTO   real_sendto   = NULL;
static PFN_RECVFROM real_recvfrom = NULL;

int g_capture     = 0;
int g_capture_max = 256;
// [patch] noupdate -- dev machines: pin the Viewer (product 1000) patch version check
// high so the server answers "current" and never overwrites the local polhook.dll.
static int g_noupdate = 0;

static void log_payload(const char* tag, UINT_PTR s, const char* buf, int len)
{
    if (!g_capture || len <= 0 || !buf) return;
    int show = len < g_capture_max ? len : g_capture_max;

    logf("[%s] sock=%p len=%d%s", tag, (void*)s, len, show < len ? " (truncated)" : "");
    // 16 bytes per line, hex + printable ASCII -- the usual shape, so record
    // boundaries in a binary protocol are visible by eye.
    for (int off = 0; off < show; off += 16) {
        char hex[16 * 3 + 1]; char asc[17];
        int n = (show - off) < 16 ? (show - off) : 16;
        for (int i = 0; i < n; i++) {
            wsprintfA(hex + i * 3, "%02x ", (BYTE)buf[off + i]);
            asc[i] = (buf[off + i] >= 0x20 && (BYTE)buf[off + i] < 0x7f) ? buf[off + i] : '.';
        }
        hex[n * 3] = 0; asc[n] = 0;
        logf("  %04x  %-48s %s", off, hex, asc);
    }
    log_flush();
}

// --- which product asked, per socket --------------------------------------------------
// The cmd8 REPLY carries no product field -- status, download host and version only.
// Only the cmd7 REQUEST has one (offset 0x14). So scoping the reply rewrite to the
// Viewer means remembering what the request on THIS SOCKET asked for.
//
// This exists because it did NOT (found 2026-08-18, FFXI Test Server track): the SEND
// half below filters on product "1000", the RECV half filtered on nothing, and so a knob
// documented as pinning THE VIEWER was zeroing the advertised version of EVERY title.
// The client believes the zeroed reply and writes it into its own patch.ver, so the
// damage outlives the launch -- content 0015's stamp went 20110811_A -> 00000000_A and
// LSB then refused the login ("got 000000xx_x, expected 302607xx_x"). The portal's era
// router reads the same reported build, so it could mis-era a client too.
//
// One 64-bit slot per bucket (socket in the low half, the 4 product chars in the high
// half) so a slot is written and read atomically without a lock. Collisions simply
// overwrite: POLP is one short exchange per connection, and a MISS is deliberately the
// SAFE direction -- we leave the reply alone rather than risk corrupting a version.
#define NOUPD_SLOTS 16
static volatile LONG64 g_noupd_slot[NOUPD_SLOTS];

static unsigned noupd_bucket(UINT_PTR s) { return (unsigned)((s >> 2) % NOUPD_SLOTS); }

static void noupd_note_request(UINT_PTR s, const char* buf, int len)
{
    if (len < 0x18 || memcmp(buf + 8, "POLP", 4) != 0) return;
    DWORD cmd; memcpy(&cmd, buf + 12, 4);
    if (cmd != 7) return;
    DWORD prod; memcpy(&prod, buf + 0x14, 4);           // "1000", "0015", ...
    LONG64 v = (LONG64)((ULONG64)(DWORD)s | ((ULONG64)prod << 32));
    InterlockedExchange64(&g_noupd_slot[noupd_bucket(s)], v);
}

// True only if the last POLP cmd7 on this socket was the Viewer's.
static bool noupd_is_viewer_socket(UINT_PTR s)
{
    LONG64 v = InterlockedCompareExchange64(&g_noupd_slot[noupd_bucket(s)], 0, 0);
    if ((DWORD)(ULONG64)v != (DWORD)s) return false;    // stale/evicted slot -> leave it alone
    DWORD prod = (DWORD)((ULONG64)v >> 32);
    return memcmp(&prod, "1000", 4) == 0;
}

// --- [patch] noupdate: keep a dev box from auto-patching (SEND side) ------------------
// Two-sided: this is the SEND half; noupdate_reply_fix below is the RECV half and the
// decisive one (our server ignores the sent version). The client asks the patch server
// "am I current?" with a POLP cmd7 carrying its version at 0x18. We rewrite the 8 version
// DIGITS to '9' (lexically max, same length) for the Viewer service (product "1000") so a
// server that DOES compare replies "you're ahead". Same-length edit; only the mandatory
// POLP checksum (u32_LE(MD5(pkt[8:])[:4]) at offset 4) needs recomputing, via CryptoAPI MD5.
static bool md5_first4(const BYTE* data, DWORD len, BYTE out4[4])
{
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0; bool ok = false;
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_MD5, 0, 0, &h)) {
            BYTE d[16]; DWORD dl = 16;
            if (CryptHashData(h, data, len, 0) &&
                CryptGetHashParam(h, HP_HASHVAL, d, &dl, 0) && dl == 16) {
                memcpy(out4, d, 4);            // first 4 MD5 bytes = the LE u32 checksum
                ok = true;
            }
            CryptDestroyHash(h);
        }
        CryptReleaseContext(prov, 0);
    }
    return ok;
}

// If `buf` is a Viewer (product 1000) POLP cmd7, write a version-pinned copy into `out`
// (capacity outcap) and return true; else false and `out` is untouched.
static bool noupdate_pin(const char* buf, int len, char* out, int outcap)
{
    if (!g_noupdate || !buf || len < 0x22 || len > outcap) return false;
    if (memcmp(buf + 8, "POLP", 4) != 0) return false;
    DWORD cmd; memcpy(&cmd, buf + 12, 4);
    if (cmd != 7) return false;
    if (memcmp(buf + 0x14, "1000", 4) != 0) return false;      // Viewer product only
    for (int i = 0; i < 8; i++)
        if (buf[0x18 + i] < '0' || buf[0x18 + i] > '9') return false;
    memcpy(out, buf, len);
    for (int i = 0; i < 8; i++) out[0x18 + i] = '9';           // pin the version high
    BYTE ck[4];
    if (!md5_first4((const BYTE*)out + 8, len - 8, ck)) return false;
    memcpy(out + 4, ck, 4);                                    // fix the packet checksum
    return true;
}

// Log every buffer of a WSABUF array as one payload each: the scatter/gather
// vector is how the caller chose to split ONE message, and joining them here
// would invent a framing the wire never had.
static void log_wsabufs(const char* tag, UINT_PTR s, PolWsaBuf* bufs, DWORD count, DWORD cap)
{
    DWORD left = cap;
    for (DWORD i = 0; i < count && left; i++) {
        DWORD n = bufs[i].len < left ? bufs[i].len : left;
        if (n && bufs[i].buf) log_payload(tag, s, bufs[i].buf, (int)n);
        left -= n;
    }
}

static int WINAPI hook_wsasend(UINT_PTR s, PolWsaBuf* bufs, DWORD count, LPDWORD sent,
                               DWORD flags, void* ov, void* cr)
{
    if (bufs && count) {
        DWORD total = 0;
        for (DWORD i = 0; i < count; i++) total += bufs[i].len;
        log_wsabufs("send", s, bufs, count, total);   // same tag: polcapx needs no change
    }
    if (ov) InterlockedIncrement(&g_wsa_overlapped);
    return real_wsasend(s, bufs, count, sent, flags, ov, cr);
}

static int WINAPI hook_wsarecv(UINT_PTR s, PolWsaBuf* bufs, DWORD count, LPDWORD recvd,
                               LPDWORD flags, void* ov, void* cr)
{
    int r = real_wsarecv(s, bufs, count, recvd, flags, ov, cr);
    DWORD werr = (r != 0) ? GetLastError() : 0;
    // Only the NON-overlapped path can be judged here: an overlapped call has
    // not finished, so its failure is not ours to read (see sessionwatch.cpp).
    if (!ov) sessionwatch_note_recv(s, (r == 0 && recvd) ? (int)*recvd : -1, werr);
    if (ov) {
        // Overlapped: the buffers are not filled yet. Count it and say so once in
        // a while, so a quiet log is never mistaken for a quiet wire.
        long n = InterlockedIncrement(&g_wsa_overlapped);
        if (n == 1 || (n % 100) == 0)
            logf("[cap] %ld overlapped WSA call(s) seen -- their payloads are NOT in this log", n);
    } else if (r == 0 && recvd && *recvd && bufs && count) {
        log_wsabufs("recv", s, bufs, count, *recvd);
    }
    if (r != 0) SetLastError(werr);   // the caller's error code is not ours to lose
    return r;
}

static int WINAPI hook_send(UINT_PTR s, const char* buf, int len, int flags)
{
    log_payload("send", s, buf, len);
    // Watch for the auth band's `USER x 8 * :<token>` on its way out. That token
    // is what POL hashes into the session id, and this process also opens the
    // FFXI lobby socket -- so this is the one place the two can be tied together
    // without inference. See poltoken.cpp.
    poltoken_note_send(buf, len);
    // Remember which product this socket's version check is for, so the RECV half
    // can scope its rewrite the same way this one does. Unconditional: it must
    // record NON-Viewer products too, since that is exactly what it has to be able
    // to recognise and leave alone.
    if (g_noupdate) noupd_note_request(s, buf, len);
    char pinned[512];
    if (noupdate_pin(buf, len, pinned, sizeof(pinned))) {
        logf("[patch] noupdate: Viewer version check pinned -- no auto-patch this launch");
        log_flush();
        return real_send(s, pinned, len, flags);
    }
    // Stamp FFXI lobby packets with that session id. The stamp lands in the
    // `identifer` field, which the bridge overwrites on the way to LSB anyway,
    // so it costs no bytes and LSB never sees it.
    char tagged[2048];
    if (len > 0 && len <= (int)sizeof(tagged) &&
        poltoken_tag(buf, len, tagged, (int)sizeof(tagged)))
        return real_send(s, tagged, len, flags);
    return real_send(s, buf, len, flags);
}

// --- [patch] noupdate (REPLY side): the decisive half ---------------------------------
// Pinning the SENT version (noupdate_pin) assumes the SERVER decides. Measured against
// our server it does NOT: the client compares its own installed version to whatever
// LATEST the patch server advertises in the POLP cmd8 reply, and ours always advertises
// its newest build. So we neutralise the reply instead -- find the advertised version
// token (8 date digits + '_' + a letter, e.g. "20110829_G") and zero the date to
// "00000000", which is older than any real install, so the client concludes it is
// current and never patches. Same-length edit; only the POLP checksum
// (u32_LE(MD5(frame[8:])[:4]) at offset 4) is recomputed. In-place on the recv buffer.
static void noupdate_reply_fix(UINT_PTR s, char* buf, int n)
{
    if (!g_noupdate || !buf || n < 0x1a) return;
    // SCOPED TO THE VIEWER, like noupdate_pin. Without this the knob rewrites
    // every title's version -- see noupd_note_request above for what that cost.
    if (!noupd_is_viewer_socket(s)) return;
    for (int i = 8; i + 8 <= n; i++) {
        if (memcmp(buf + i, "POLP", 4) != 0) continue;
        char* frame = buf + (i - 8);                       // "POLP" sits 8 bytes into a frame
        DWORD flen; memcpy(&flen, frame, 4);
        if (flen < 16 || (i - 8) + (int)flen > n) continue; // not a whole frame in this buffer
        DWORD cmd; memcpy(&cmd, frame + 12, 4);
        if (cmd != 8) continue;                            // version-check reply only
        bool changed = false;
        for (DWORD j = 16; j + 10 <= flen; j++) {
            const char* p = frame + j;
            bool datey = true;
            for (int k = 0; k < 8; k++) if (p[k] < '0' || p[k] > '9') { datey = false; break; }
            if (!datey || p[8] != '_') continue;
            char c = p[9];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) continue;
            if (memcmp(p, "00000000", 8) == 0) continue;   // already neutralised
            memcpy(frame + j, "00000000", 8);              // older than any install
            changed = true;
        }
        if (changed) {
            BYTE ck[4];
            if (md5_first4((const BYTE*)frame + 8, flen - 8, ck)) {
                memcpy(frame + 4, ck, 4);
                logf("[patch] noupdate: patch-check reply neutralised -- this box stays put");
                log_flush();
            }
        }
        return;                                            // one cmd8 per reply is enough
    }
}

static int WINAPI hook_recv(UINT_PTR s, char* buf, int len, int flags)
{
    int n = real_recv(s, buf, len, flags);       // log what actually arrived
    // Read the Winsock error BEFORE anything else can clobber it, and put it
    // back before returning: the caller's error code is not ours to disturb.
    DWORD werr = (n < 0) ? GetLastError() : 0;
    log_payload("recv", s, buf, n);              // (log the ORIGINAL, then neutralise in place)
    if (g_noupdate && n > 0) noupdate_reply_fix(s, buf, n);
    sessionwatch_note_recv(s, n, werr);
    if (n < 0) SetLastError(werr);
    return n;
}

static int WINAPI hook_sendto(UINT_PTR s, const char* buf, int len, int flags,
                              const void* to, int tolen)
{
    if (g_capture && to && tolen >= 8) {
        const BYTE* b = (const BYTE*)to;
        logf("[sendto] -> %u.%u.%u.%u:%u", b[4], b[5], b[6], b[7], (b[2] << 8) | b[3]);
    }
    log_payload("sendto", s, buf, len);
    return real_sendto(s, buf, len, flags, to, tolen);
}

static int WINAPI hook_recvfrom(UINT_PTR s, char* buf, int len, int flags,
                                void* from, int* fromlen)
{
    int n = real_recvfrom(s, buf, len, flags, from, fromlen);
    if (g_capture && n > 0 && from && fromlen && *fromlen >= 8) {
        const BYTE* b = (const BYTE*)from;
        logf("[recvfrom] <- %u.%u.%u.%u:%u", b[4], b[5], b[6], b[7], (b[2] << 8) | b[3]);
    }
    log_payload("recvfrom", s, buf, n);
    return n;
}

// --- startup-state override (usr/all/login_w.bin +0x6E) ----------------------
//
// The Viewer's first-run wizard is a state machine on a global that app.dll
// seeds from ONE PERSISTED BYTE: offset 0x6E of usr/all/login_w.bin, read by
// app.dll 0x049fe0e1 and written by 0x049fe067. State 0x19 makes
// GotoStartupState (0x04a0116e) enter state 0x1A, which builds
// CStartupWelcomeFrame carrying the `Startup_SignUpPlayOnline_Win` layout --
// the in-client account sign-up screen, which is otherwise unreachable once a
// client has finished its first run (a normal install sits at 0x1D).
//
// We serve the byte rather than write it. login_w.bin also holds the saved
// member list, so editing it on disk risks a 42KB file for a one-byte change,
// and a crash would leave the wizard latched on. Instead:
//
//   * ReadFile on that file -> patch the byte in the buffer the client sees,
//     remembering the true value.
//   * WriteFile on that file -> put the true value BACK, so a save during the
//     session cannot persist the forced state. This is what makes it temporary.
//
// Off unless [polshim] startup_state is set. Reads/writes that do not span
// offset 0x6E are untouched, and a write is only rewritten if we actually
// observed the original byte first -- otherwise we pass it through unchanged
// rather than guess.

#define LOGIN_W_STATE_OFF   0x6E

typedef HANDLE (WINAPI *PFN_CFW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                 DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *PFN_CFA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                 DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *PFN_RF)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *PFN_WF)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *PFN_CLH)(HANDLE);
typedef BOOL (WINAPI *PFN_MVA)(LPCSTR, LPCSTR);
typedef void (WINAPI *PFN_ODSA)(LPCSTR);
typedef void (WINAPI *PFN_ODSW)(LPCWSTR);
typedef int  (WINAPI *PFN_MBA)(HWND, LPCSTR, LPCSTR, UINT);
typedef int  (WINAPI *PFN_MBW)(HWND, LPCWSTR, LPCWSTR, UINT);
static PFN_MBA real_MessageBoxA = NULL;
static PFN_MBW real_MessageBoxW = NULL;

static PFN_CFW real_CreateFileW = NULL;
static PFN_CFA real_CreateFileA = NULL;
static PFN_RF  real_ReadFile    = NULL;
static PFN_WF  real_WriteFile   = NULL;
static PFN_CLH real_CloseHandle = NULL;
static PFN_MVA real_MoveFileA   = NULL;
static PFN_ODSA real_OutputDebugStringA = NULL;
static PFN_ODSW real_OutputDebugStringW = NULL;

// [polshim] dbgstring -- capture the CONTENT COMPONENTS' own diagnostics.
// FE_Client.dll (and FriendList.dll) ship their debug logging in the shipped
// build and talk through OutputDebugString, which normally needs an elevated
// DebugView to read. Routing it into polshim.log instead puts the component's
// own account of a failure on the SAME timeline as the COM/DirectX/table traces,
// which is the whole point -- a Fantasy Earth GameStart that returns without ever
// calling Direct3DCreate8 leaves no other evidence at all.
// Default ON: a component only calls this when it has something to say, so it is
// not a hot path, and a silent run is itself the finding.
int g_dbgstring = 1;

// The Viewer ships unicows.dll (MS Layer for Unicode) and app.dll/pol.exe import
// the file APIs THROUGH it, so their IAT slots hold unicows' thunk addresses --
// not kernel32's. By-value matching against kernel32 alone therefore misses every
// slot, which is exactly why the first startup_state run hooked nothing. Resolve
// the unicows aliases too and put them in the same map. unicows may not be loaded
// when init runs, so this is done lazily from patch_iat.
int g_startup_state = 0;                 // 0 = feature off (set from the ini)

static void* uni_CreateFileW = NULL;
static void* uni_CreateFileA = NULL;
static void* uni_ReadFile    = NULL;
static void* uni_WriteFile   = NULL;
static void* uni_CloseHandle = NULL;
static bool  g_uni_done      = false;

static void resolve_unicows(void)
{
    if (g_uni_done || !g_startup_state) return;
    HMODULE u = GetModuleHandleW(L"unicows.dll");
    if (!u) return;                       // not loaded yet; try again next module
    uni_CreateFileW = (void*)GetProcAddress(u, "CreateFileW");
    uni_CreateFileA = (void*)GetProcAddress(u, "CreateFileA");
    uni_ReadFile    = (void*)GetProcAddress(u, "ReadFile");
    uni_WriteFile   = (void*)GetProcAddress(u, "WriteFile");
    uni_CloseHandle = (void*)GetProcAddress(u, "CloseHandle");
    g_uni_done = true;
    logf("[startup] unicows.dll aliases resolved (CreateFileW=%p ReadFile=%p)",
         uni_CreateFileW, uni_ReadFile);
}

static int g_login_orig = -1;            // true byte, learned on first read
static HANDLE g_lw[16];                  // handles open on login_w.bin
static CRITICAL_SECTION g_lwlock;
static bool g_lwlock_ready = false;

static bool is_login_w_w(const wchar_t* p)
{
    if (!p) return false;
    size_t n = wcslen(p);
    const wchar_t* tail = L"login_w.bin";
    size_t t = wcslen(tail);
    return n >= t && _wcsicmp(p + n - t, tail) == 0;
}

static bool is_login_w_a(const char* p)
{
    if (!p) return false;
    size_t n = strlen(p), t = strlen("login_w.bin");
    return n >= t && _stricmp(p + n - t, "login_w.bin") == 0;
}

// Diagnostic. Two runs hooked CreateFileA in app.dll successfully and still never
// saw login_w.bin opened, yet the file's mtime advances during a session -- so the
// client reaches it some other way (app.dll imports CreateFileMappingA/MapViewOfFile
// and MoveFileA/DeleteFileA, so a mapped read or a write-temp-then-rename are both
// live candidates). Rather than guess a third time, log every .bin path that
// crosses the file APIs and let one run say which door it uses.
static bool ends_bin_a(const char* p)
{
    if (!p) return false;
    size_t n = strlen(p);
    return n >= 4 && _stricmp(p + n - 4, ".bin") == 0;
}

static bool ends_bin_w(const wchar_t* p)
{
    if (!p) return false;
    size_t n = wcslen(p);
    return n >= 4 && _wcsicmp(p + n - 4, L".bin") == 0;
}

static void lw_track(HANDLE h)
{
    if (h == INVALID_HANDLE_VALUE || !g_lwlock_ready) return;
    EnterCriticalSection(&g_lwlock);
    for (int i = 0; i < _countof(g_lw); i++)
        if (!g_lw[i]) { g_lw[i] = h; break; }
    LeaveCriticalSection(&g_lwlock);
    logf("[startup] login_w.bin opened (handle=%p)", h);
}

static void lw_forget(HANDLE h)
{
    if (!g_lwlock_ready) return;
    EnterCriticalSection(&g_lwlock);
    for (int i = 0; i < _countof(g_lw); i++)
        if (g_lw[i] == h) g_lw[i] = NULL;
    LeaveCriticalSection(&g_lwlock);
}

static bool lw_is_tracked(HANDLE h)
{
    if (!g_startup_state || !g_lwlock_ready) return false;
    bool hit = false;
    EnterCriticalSection(&g_lwlock);
    for (int i = 0; i < _countof(g_lw); i++)
        if (g_lw[i] == h) { hit = true; break; }
    LeaveCriticalSection(&g_lwlock);
    return hit;
}

// Absolute file offset this synchronous read/write starts at. Overlapped I/O
// carries its own offset; we only handle the synchronous case the client uses.
static bool lw_offset(HANDLE h, LPOVERLAPPED ov, LONGLONG* out)
{
    if (ov) { *out = ((LONGLONG)ov->OffsetHigh << 32) | ov->Offset; return true; }
    LARGE_INTEGER zero, cur;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) return false;
    *out = cur.QuadPart;
    return true;
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    HANDLE h = real_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
    if (g_startup_state && ends_bin_w(name))
        logf("[startup] CreateFileW(%S) acc=%08lX disp=%lu -> %p", name, acc, disp, h);
    if (g_startup_state && is_login_w_w(name)) lw_track(h);
    return h;
}

static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD acc, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    HANDLE h = real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    if (g_startup_state && ends_bin_a(name))
        logf("[startup] CreateFileA(%s) acc=%08lX disp=%lu -> %p", name, acc, disp, h);
    if (g_startup_state && is_login_w_a(name)) lw_track(h);
    return h;
}

static BOOL WINAPI hook_ReadFile(HANDLE h, LPVOID buf, DWORD len,
                                 LPDWORD got, LPOVERLAPPED ov)
{
    LONGLONG pos = 0;
    bool watch = lw_is_tracked(h);
    if (watch && !lw_offset(h, ov, &pos)) watch = false;

    BOOL r = real_ReadFile(h, buf, len, got, ov);
    if (!watch || !r || !buf || !got) return r;

    DWORD n = *got;
    if (pos <= LOGIN_W_STATE_OFF && (LONGLONG)(pos + n) > LOGIN_W_STATE_OFF) {
        BYTE* p = (BYTE*)buf + (LOGIN_W_STATE_OFF - pos);
        if (g_login_orig < 0) g_login_orig = *p;
        if (*p != (BYTE)g_startup_state) {
            logf("[startup] login_w.bin +0x%02x: 0x%02x -> 0x%02x (in-memory only)",
                 LOGIN_W_STATE_OFF, *p, (BYTE)g_startup_state);
            log_flush();
        }
        *p = (BYTE)g_startup_state;
    }
    return r;
}

static BOOL WINAPI hook_WriteFile(HANDLE h, LPCVOID buf, DWORD len,
                                  LPDWORD put, LPOVERLAPPED ov)
{
    LONGLONG pos = 0;
    // Only rewrite when we know the true byte; otherwise pass through untouched.
    if (g_login_orig >= 0 && lw_is_tracked(h) && buf && lw_offset(h, ov, &pos) &&
        pos <= LOGIN_W_STATE_OFF && (LONGLONG)(pos + len) > LOGIN_W_STATE_OFF) {
        BYTE* copy = (BYTE*)HeapAlloc(GetProcessHeap(), 0, len);
        if (copy) {
            memcpy(copy, buf, len);
            copy[LOGIN_W_STATE_OFF - pos] = (BYTE)g_login_orig;
            logf("[startup] login_w.bin save: restoring +0x%02x to 0x%02x",
                 LOGIN_W_STATE_OFF, g_login_orig);
            log_flush();
            BOOL r = real_WriteFile(h, copy, len, put, ov);
            HeapFree(GetProcessHeap(), 0, copy);
            return r;
        }
    }
    return real_WriteFile(h, buf, len, put, ov);
}

static BOOL WINAPI hook_CloseHandle(HANDLE h)
{
    lw_forget(h);
    return real_CloseHandle(h);
}

// Catches the write-temp-then-rename hypothesis: if login_w.bin is only ever
// *replaced*, no handle is ever opened under its own name and every hook above
// stays silent -- which is exactly the symptom we have.
static BOOL WINAPI hook_MoveFileA(LPCSTR from, LPCSTR to)
{
    if (g_startup_state && (ends_bin_a(from) || ends_bin_a(to)))
        logf("[startup] MoveFileA(%s -> %s)", from ? from : "(null)", to ? to : "(null)");
    return real_MoveFileA(from, to);
}

// Trailing CR/LF is stripped: these components end most lines with "\n", and a
// raw copy would break one log line into two and desynchronise it from the
// interleaved traces around it.
static void dbg_emit(const char* s)
{
    if (!s) return;
    char buf[1024];
    size_t n = 0;
    while (s[n] && n < sizeof(buf) - 1) { buf[n] = s[n]; n++; }
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    if (n) logf("[dbg] %s", buf);

    // KEY: THE ONE MOMENT WORTH A SNAPSHOT: Fantasy Earth's focus watchdog.
    //
    // FE compares its own window against GetForegroundWindow() and, on a mismatch,
    // releases the capture, calls ShowCursor/ClipCursor(NULL) and sets an input kill
    // switch at [obj+0x6C] that makes its mouse poll ZERO every axis and every
    // button (measured). The user sees the cursor move
    // and the clicks do nothing -- while DirectInput keeps delivering BUTTON events
    // that the game discards (measured 2026-08-25: 648 BTN0 events, all ignored).
    //
    // It announces the moment itself, so this is a free correlation point: record
    // what the foreground ACTUALLY is when FE claims to have lost it. Measured that
    // session, the foreground never left FE's window for 45 seconds (Watch-Focus.ps1)
    // and the watchdog fired 20 times anyway -- so the suspicion is that FE is
    // comparing against a window that is NOT the one it renders into. If this line
    // reports foreground == game window, that is proven by elimination and the hunt
    // moves into FE_Client.dll for whichever handle it cached.
    if (n && _strnicmp(buf, "window lost focus", 17) == 0) {
        HWND fg = GetForegroundWindow(), gw = d3d8_game_window();
        char cls[64] = "?";
        if (fg) GetClassNameA(fg, cls, sizeof(cls));
        logf("[foc] ^^ FE's focus watchdog fired. foreground=%p (class=%s)  "
             "game window=%p  -> %s. A mismatch here is a real focus loss; a MATCH "
             "means FE is comparing against some other handle, and its input kill "
             "switch is being set while its own window is genuinely in front.",
             (void*)fg, cls, (void*)gw,
             (fg && fg == gw) ? "MATCH -- FE is wrong about its own window"
                              : "differ -- FE really is not the foreground window");
    }
}

// The other half of "what did the title just tell the user". FE_Client emits
// nothing through OutputDebugString (its logging is compiled out of the shipped
// build), but it still raises real dialogs -- and on a machine without Japanese
// support those render as mojibake, so the user cannot read them either. Logging
// the raw text makes it translatable offline against the string set in the
// unpacked dump. Rides the same dbgstring switch.
// Translation rides HERE rather than in a hook of its own: one hook, one place
// the strings pass through, and the log line and the substitution can never
// disagree about what was shown. Text and caption are looked up independently --
// a dialog often has a translatable body and a caption that is just a file path.
static int WINAPI hook_MessageBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type)
{
    if (g_dbgstring)
        logf("[dbg] MessageBoxA cap=\"%s\" text=\"%s\" (type=%08X)",
             cap ? cap : "", text ? text : "", type);
    char xt[2048], xc[2048];
    if (msgxlate_enabled()) {
        if (text && msgxlate_a(text, xt, sizeof(xt), "text")) text = xt;
        if (cap  && msgxlate_a(cap,  xc, sizeof(xc), "caption")) cap = xc;
    }
    return real_MessageBoxA(h, text, cap, type);
}

static int WINAPI hook_MessageBoxW(HWND h, LPCWSTR text, LPCWSTR cap, UINT type)
{
    if (g_dbgstring) {
        char t[1024] = "", c[256] = "";
        if (text) WideCharToMultiByte(CP_ACP, 0, text, -1, t, sizeof(t), NULL, NULL);
        if (cap)  WideCharToMultiByte(CP_ACP, 0, cap,  -1, c, sizeof(c), NULL, NULL);
        logf("[dbg] MessageBoxW cap=\"%s\" text=\"%s\" (type=%08X)", c, t, type);
    }
    wchar_t xt[1024], xc[1024];
    if (msgxlate_enabled()) {
        if (text && msgxlate_w(text, xt, 1024, "text")) text = xt;
        if (cap  && msgxlate_w(cap,  xc, 1024, "caption")) cap = xc;
    }
    return real_MessageBoxW(h, text, cap, type);
}

static void arm_title_hooks_late(void);   // defined with the socket hook below

static void WINAPI hook_OutputDebugStringA(LPCSTR s)
{
    // EARLIEST reliable arming point for a title. FFXI emits "client start" long
    // before it opens a socket, and its world-session init -- where `-net` is
    // parsed and the world context allocated -- runs BEFORE the first socket()
    // too. Arming only from the socket hook therefore misses both, which is
    // exactly what happened on 2026-08-14: all three probes armed correctly but
    // the code they watch had already run.
    arm_title_hooks_late();
    if (g_dbgstring) dbg_emit(s);
    real_OutputDebugStringA(s);
}

static void WINAPI hook_OutputDebugStringW(LPCWSTR s)
{
    if (g_dbgstring && s) {
        // CP_ACP, not CP_UTF8: these are Japanese builds and the log is read as
        // Shift-JIS elsewhere in this tree, so keep one encoding throughout.
        char buf[1024];
        int n = WideCharToMultiByte(CP_ACP, 0, s, -1, buf, sizeof(buf), NULL, NULL);
        if (n > 0) dbg_emit(buf);
    }
    real_OutputDebugStringW(s);
}

// --- sign-up URL scheme downgrade (https: -> http:) --------------------------
//
// app.dll 0x049ff3a1 builds the in-client sign-up URL as
//     "https:" + POL_UCS_URL + POL_UCS_CGI + "?kinou_id=20&..."
// from a WIDE literal `L"https:%s%s"` in .rdata (RVA 0x3cf920 in 1.18.15e). The
// scheme is baked into that literal, so env.dat cannot change it -- and the
// client's TLS is genuine SSLv3, which no modern OpenSSL will answer (the server
// side logs VERSION_TOO_LOW; the client shows POL-1322).
//
// `L"http:%s%s"` is SHORTER than `L"https:%s%s"` (20 vs 22 bytes with the NUL),
// so it can be written in place with room to spare. That turns the whole SSLv3
// problem into a two-byte edit and sends the fetch to our plain-HTTP band
// handler instead.
//
// Found by SEARCH rather than a hardcoded RVA so a client update does not
// silently turn this into a wild write. Off unless [polshim] signup_http=1.

static int g_signup_http = 0;

static void patch_signup_scheme(HMODULE mod)
{
    if (!g_signup_http || !mod) return;
    char leaf[64];
    module_leaf(mod, leaf, sizeof(leaf));
    if (_stricmp(leaf, "app.dll") != 0) return;

    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    SIZE_T span = nt->OptionalHeader.SizeOfImage;

    static const wchar_t FROM[] = L"https:%s%s";
    static const wchar_t TO[]   = L"http:%s%s";
    const SIZE_T fromBytes = sizeof(FROM);          // includes the NUL
    int hits = 0;

    for (SIZE_T i = 0; i + fromBytes <= span; i += 2) {
        if (memcmp(base + i, FROM, fromBytes) != 0) continue;
        DWORD old;
        if (!VirtualProtect(base + i, fromBytes, PAGE_READWRITE, &old)) continue;
        memcpy(base + i, TO, sizeof(TO));           // shorter: NUL lands early
        // Blank the two trailing bytes the shorter string no longer covers, so
        // nothing reads a stale fragment past the new terminator.
        *(wchar_t*)(base + i + sizeof(TO)) = L'\0';
        VirtualProtect(base + i, fromBytes, old, &old);
        hits++;
        logf("[signup] %s+0x%06X: L\"https:%%s%%s\" -> L\"http:%%s%%s\"",
             leaf, (unsigned)i);
    }
    if (!hits)
        logf("[signup] %s: scheme literal NOT found -- client version changed?",
             leaf);
    log_flush();
}

static void inject_into(HANDLE hProcess)
{
    SIZE_T cb = (wcslen(g_self_path) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProcess, NULL, cb, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) return;
    if (WriteProcessMemory(hProcess, remote, g_self_path, cb, NULL)) {
        LPTHREAD_START_ROUTINE loader = (LPTHREAD_START_ROUTINE)
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(hProcess, NULL, 0, loader, remote, 0, NULL);
        if (th) { WaitForSingleObject(th, 10000); CloseHandle(th); }
    }
    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
}

// Log a spawned child's command line so a title's polboot.exe reveals the
// "/game <token>" shortcut argument it hands to its pol.exe child -- the token
// space is per-title and opaque to pol.exe (it copies the token verbatim into
// its shortcut buffer), so the ONLY way to learn each title's token is to read
// what its own boot exe passes. Low-noise: pol.exe spawns few children. `pid`
// is the freshly created child, for correlating with its own later log.
static void shortcut_log_child(const wchar_t* app, const wchar_t* cmd, DWORD pid)
{
    const wchar_t* a = app ? app : L"(null)";
    const wchar_t* c = cmd ? cmd : L"(inherited from app name)";
    logf("[shortcut] child pid=%lu app=\"%ls\" cmd=\"%ls\"", pid, a, c);
}

static BOOL WINAPI hook_CreateProcessW(LPCWSTR app, LPWSTR cmd,
    LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD flags,
    LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    BOOL wanted_suspended = (flags & CREATE_SUSPENDED) != 0;
    BOOL r = real_CreateProcessW(app, cmd, pa, ta, inh, flags | CREATE_SUSPENDED,
                                 env, cwd, si, pi);
    if (r) {
        shortcut_log_child(app, cmd, pi->dwProcessId);
        inject_into(pi->hProcess);              // child now hooks its own children too
        if (!wanted_suspended) ResumeThread(pi->hThread);
    }
    return r;
}

static BOOL WINAPI hook_CreateProcessA(LPCSTR app, LPSTR cmd,
    LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD flags,
    LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    BOOL wanted_suspended = (flags & CREATE_SUSPENDED) != 0;
    BOOL r = real_CreateProcessA(app, cmd, pa, ta, inh, flags | CREATE_SUSPENDED,
                                 env, cwd, si, pi);
    if (r) {
        // Widen the ANSI args just for the log so both hooks share one format.
        wchar_t wapp[MAX_PATH] = L"", wcmd[2048] = L"";
        if (app) MultiByteToWideChar(CP_ACP, 0, app, -1, wapp, MAX_PATH);
        if (cmd) MultiByteToWideChar(CP_ACP, 0, cmd, -1, wcmd, 2048);
        shortcut_log_child(app ? wapp : NULL, cmd ? wcmd : NULL, pi->dwProcessId);
        inject_into(pi->hProcess);
        if (!wanted_suspended) ResumeThread(pi->hThread);
    }
    return r;
}

// --- IAT patching (by value) ------------------------------------------------
//
// Swap any IAT slot whose value equals a known real function for our hook.
// By-value matching ignores which import descriptor or api-ms-win-* alias the
// slot came from, and ignores binding.

static void arm_title_hooks_late(void);   // defined with the socket hook

static void patch_iat(HMODULE mod)
{
    if (!mod || mod == g_self) return;
    resolve_unicows();          // no-op unless startup_state is on

    // IMPORTANT: ARM dbgstring HERE, NOT in apply_module_hooks -- corrected 2026-08-18 after the
    // first attempt logged nothing. apply_module_hooks only ever sees the shim's three
    // TARGETS (polcore.dll, app.dll, PolContents.dll); a title's DLL never reaches it.
    // patch_iat is the function that does see FE_Client.dll, which is exactly why the
    // "[iat] hooked ... in FE_Client.dll" lines come from here. Arming before the hook
    // loop below also means the title's own IAT is patched with the emit path already
    // live, so its very first OutputDebugString is captured rather than the second.
    {
        char leaf0[MAX_PATH] = "";
        if (GetModuleFileNameA(mod, leaf0, sizeof(leaf0))) {
            const char* lp = strrchr(leaf0, '\\');
            lp = lp ? lp + 1 : leaf0;
            if (!g_dbgstring && !g_dbgstring_explicit && is_title_module(lp)) {
                g_dbgstring = 1;
                logf("[dbg] %s is loaded -- capturing the title's OWN diagnostics from "
                     "here (auto; [polshim] dbgstring=0 to refuse, =1 from the start)", lp);
                log_flush();
            }
        }
    }
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;

    // `tag` is set on the entries whose swap is worth logging (the file-I/O and
    // DirectX ones), so an actual swap is visible. Without that, "hook never
    // installed" and "hook installed but the client never called it" look
    // identical in the log -- which is exactly the ambiguity that hid the
    // unicows problem on the first startup_state run.
    struct { void* from; void* to; const char* tag; } map[] = {
        { (void*)real_CreateProcessW, (void*)hook_CreateProcessW, NULL },
        { (void*)real_CreateProcessA, (void*)hook_CreateProcessA, NULL },
        { (void*)real_connect,        (void*)hook_connect,        NULL },
        // Tagged, unlike the other ws2_32 entries: the whole point of these two
        // is that their ABSENCE from the log is the finding, so "never installed"
        // must not be able to masquerade as "installed and never called".
        { (void*)real_socket,         (void*)hook_socket,         "ws2:socket" },
        { (void*)real_bind,           (void*)hook_bind,           "ws2:bind" },
        // Name resolution -- see netredir.cpp. Resolves to NULL unless
        // [redirect] enable=1, and a NULL `from` never matches, so a normal
        // private-server install never gets this hook. Tagged, because "the
        // redirect was never installed" and "the redirect installed but the
        // client resolved nothing" must not look alike in the log.
        { netredir_real_gethostbyname(), netredir_hook_gethostbyname(), "ws2:gethostbyname" },
        // Install-folder redirect -- see regredir.cpp. Re-roots the Viewer's own
        // registry-driven InstallFolder read at THIS copy, so a second install
        // stops writing to the primary tree. Both NULL unless [installdir]
        // enable=1, and a NULL `from` never matches, so a normal install is
        // untouched. Tagged: "the redirect was never installed" and "installed but
        // the client read nothing" must not look alike in the log.
        { installdir_real_RegQueryValueExA(), installdir_hook_RegQueryValueExA(), "advapi32:RegQueryValueExA" },
        { installdir_real_RegQueryValueExW(), installdir_hook_RegQueryValueExW(), "advapi32:RegQueryValueExW" },
        { (void*)real_send,           (void*)hook_send,           NULL },
        { (void*)real_recv,           (void*)hook_recv,           NULL },
        { (void*)real_sendto,         (void*)hook_sendto,         NULL },
        { (void*)real_recvfrom,       (void*)hook_recvfrom,       NULL },
        { (void*)real_wsasend,        (void*)hook_wsasend,        NULL },
        { (void*)real_wsarecv,        (void*)hook_wsarecv,        NULL },
        // DirectSound / DirectDraw factories -- see dxhook.cpp. Both resolve to
        // NULL unless [dx] enable=1, and a NULL `from` never matches, so the
        // presentation hooks stay out of every IAT in a normal capture run.
        // app.dll imports DSOUND by ORDINAL (#11); by-value matching does not
        // care, exactly as with the ws2_32 imports above.
        { dx_real_DirectSoundCreate8(), dx_hook_DirectSoundCreate8(), "dsound:DirectSoundCreate8" },
        { dx_real_DirectDrawCreateEx(), dx_hook_DirectDrawCreateEx(), "ddraw:DirectDrawCreateEx" },
        // COM activation -- see comtrace.cpp. Tagged so an installed-but-never-
        // called hook is distinguishable from one that never got installed;
        // that ambiguity is exactly what makes a silent COM failure unreadable.
        { com_real_CoCreateInstance(),   com_hook_CoCreateInstance(),   "ole32:CoCreateInstance" },
        { com_real_CoCreateInstanceEx(), com_hook_CoCreateInstanceEx(), "ole32:CoCreateInstanceEx" },
        { com_real_CoGetClassObject(),   com_hook_CoGetClassObject(),   "ole32:CoGetClassObject" },
        { com_real_CLSIDFromProgID(),    com_hook_CLSIDFromProgID(),    "ole32:CLSIDFromProgID" },
        // Display-mode trace -- who actually switches the monitor. Also NULL
        // unless [dx] enable=1, since dx_resolve() owns their resolution.
        { dx_real_ChangeDisplaySettingsA(),   dx_hook_ChangeDisplaySettingsA(),   "u32:ChangeDisplaySettingsA" },
        { dx_real_ChangeDisplaySettingsW(),   dx_hook_ChangeDisplaySettingsW(),   "u32:ChangeDisplaySettingsW" },
        { dx_real_ChangeDisplaySettingsExA(), dx_hook_ChangeDisplaySettingsExA(), "u32:ChangeDisplaySettingsExA" },
        { dx_real_ChangeDisplaySettingsExW(), dx_hook_ChangeDisplaySettingsExW(), "u32:ChangeDisplaySettingsExW" },
        // The GAMES' renderer. app.dll is DirectDraw, but TM.dll / FFXiMain.dll /
        // FE_Client.dll all import d3d8!Direct3DCreate8 -- so this, not anything
        // above, is the path a fullscreen mode switch actually takes.
        { d3d8_real_Direct3DCreate8(), d3d8_hook_Direct3DCreate8(), "d3d8:Direct3DCreate8" },
        // Cursor translation -- belt and braces alongside the EAT patch, since a
        // packed module re-resolving its imports is what defeats IAT patching.
        { d3d8_real_SetCursorPos(),    d3d8_hook_SetCursorPos(),    "u32:SetCursorPos" },
        { d3d8_real_GetCursorPos(),    d3d8_hook_GetCursorPos(),    "u32:GetCursorPos" },
        { d3d8_real_ClipCursor(),      d3d8_hook_ClipCursor(),      "u32:ClipCursor" },
        // ShowCursor: the display COUNT, not the shape. pol.exe imports it and so
        // does FE_Client.dll, and pol.exe's IAT is bound before our EAT patch lands,
        // so the IAT row is what actually catches the Viewer's own calls.
        { d3d8_real_ShowCursor(),      d3d8_hook_ShowCursor(),      "u32:ShowCursor" },
        // Mouse capture -- pure instrumentation. A captured window gets no
        // non-client hit-testing, so its caption stops being draggable; these say
        // who took the capture and never gave it back. IAT as well as EAT, since
        // the title modules are packed and re-resolve their imports.
        { d3d8_real_SetCapture(),      d3d8_hook_SetCapture(),      "u32:SetCapture" },
        { d3d8_real_ReleaseCapture(),  d3d8_hook_ReleaseCapture(),  "u32:ReleaseCapture" },
        // The games' real pointer source -- see dinputhook.cpp.
        { dinput_real_DirectInput8Create(), dinput_hook_DirectInput8Create(), "dinput8:DirectInput8Create" },
        // Focus grabs -- measuring who takes the window back.
        { d3d8_real_SetForegroundWindow(), d3d8_hook_SetForegroundWindow(), "u32:SetForegroundWindow" },
        { d3d8_real_BringWindowToTop(),    d3d8_hook_BringWindowToTop(),    "u32:BringWindowToTop" },
        { d3d8_real_SetActiveWindow(),     d3d8_hook_SetActiveWindow(),     "u32:SetActiveWindow" },
        { d3d8_real_GetSystemMetrics(),   d3d8_hook_GetSystemMetrics(),   "u32:GetSystemMetrics" },
        { d3d8_real_GetWindowRect(),      d3d8_hook_GetWindowRect(),      "u32:GetWindowRect" },
        { hookspy_real_SetWindowsHookExA(), hookspy_hook_SetWindowsHookExA(), "u32:SetWindowsHookExA" },
        { hookspy_real_SetWindowsHookExW(), hookspy_hook_SetWindowsHookExW(), "u32:SetWindowsHookExW" },
        // Resource GlobalLock heal -- see reslock.cpp. Wine NULLs a GlobalLock
        // on an unaligned resource pointer (it pokes the read-only .rsrc page
        // below it); Windows returns it unchanged. That silent NULL is what
        // left FMO's shader effect uncreated on the Deck and crashed game
        // entry. Tagged: "never installed" must not look like "never called".
        { reslock_real_GlobalLock(),   reslock_hook_GlobalLock(),   "k32:GlobalLock" },
        { reslock_real_GlobalUnlock(), reslock_hook_GlobalUnlock(), "k32:GlobalUnlock" },
        // Friend List window transparency -- see flwindow.cpp. NULL unless [dx]
        // fl_window_transparent=1, and only acts on the WInFriendListWindow class.
        // The white flash while a game loads -- see d3d8hook.cpp blackbg_swap. Acts
        // ONLY on a class registered by a profiled TITLE that asked for the white
        // system background, so pol.exe's and app.dll's own classes go past
        // untouched even though their imports are patched here too.
        { d3d8_real_RegisterClassA(),   d3d8_hook_RegisterClassA(),   "u32:RegisterClassA" },
        { d3d8_real_RegisterClassExA(), d3d8_hook_RegisterClassExA(), "u32:RegisterClassExA" },
        { flwindow_real_ShowWindow(),   flwindow_hook_ShowWindow(),   "u32:ShowWindow" },
        { flwindow_real_SetWindowRgn(), flwindow_hook_SetWindowRgn(), "u32:SetWindowRgn" },
        // File I/O -- only used by the startup_state override. Left unresolved
        // (NULL) when the feature is off, and a NULL `from` never matches.
        { (void*)real_CreateFileW,    (void*)hook_CreateFileW,    "k32:CreateFileW" },
        { (void*)real_CreateFileA,    (void*)hook_CreateFileA,    "k32:CreateFileA" },
        { (void*)real_ReadFile,       (void*)hook_ReadFile,       "k32:ReadFile" },
        { (void*)real_WriteFile,      (void*)hook_WriteFile,      "k32:WriteFile" },
        { (void*)real_CloseHandle,    (void*)hook_CloseHandle,    "k32:CloseHandle" },
        { (void*)real_MoveFileA,      (void*)hook_MoveFileA,      "k32:MoveFileA" },
        // ...and the same five as re-exported by unicows.dll (see resolve_unicows).
        { uni_CreateFileW,            (void*)hook_CreateFileW,    "uni:CreateFileW" },
        { uni_CreateFileA,            (void*)hook_CreateFileA,    "uni:CreateFileA" },
        { uni_ReadFile,               (void*)hook_ReadFile,       "uni:ReadFile" },
        { uni_WriteFile,              (void*)hook_WriteFile,      "uni:WriteFile" },
        { uni_CloseHandle,            (void*)hook_CloseHandle,    "uni:CloseHandle" },
        // The content components' own diagnostics -- see dbgstring above. Tagged
        // so "FE_Client never logs anything" is distinguishable from "the hook
        // never reached FE_Client's IAT", which are very different findings.
        { (void*)real_OutputDebugStringA, (void*)hook_OutputDebugStringA, "k32:OutputDebugStringA" },
        { (void*)real_OutputDebugStringW, (void*)hook_OutputDebugStringW, "k32:OutputDebugStringW" },
        { (void*)real_MessageBoxA,        (void*)hook_MessageBoxA,        "u32:MessageBoxA" },
        { (void*)real_MessageBoxW,        (void*)hook_MessageBoxW,        "u32:MessageBoxW" },
        // SEAM 5 of the focus gate -- see keystate.cpp. GetAsyncKeyState and
        // friends report the PHYSICAL keyboard and do not take a window, so a
        // title that polls them keeps playing while you type somewhere else;
        // nothing in this project hooked them before 2026-09-09, and
        // FE_Client.dll imports two of the three by name.
        //
        // Installed unconditionally rather than behind the gate's own switch:
        // [dx] key_focus_gate is re-read LIVE, and a hook that was never
        // installed cannot begin working when the setting changes. The hooks
        // consult the gate per call and are a pass-through while it is off.
        //
        // TAGGED, and that tag is load-bearing: "the title never polled the
        // keyboard" and "the swap never reached the title's imports" are
        // completely different findings, and the [key] summary explicitly tells
        // the reader to look for this line before choosing between them.
        { keystate_real_GetAsyncKeyState(), keystate_hook_GetAsyncKeyState(), "u32:GetAsyncKeyState" },
        { keystate_real_GetKeyState(),      keystate_hook_GetKeyState(),      "u32:GetKeyState" },
        { keystate_real_GetKeyboardState(), keystate_hook_GetKeyboardState(), "u32:GetKeyboardState" },
        // Self-termination -- name WHO decided to quit. A title that statically links the
        // MSVC CRT (FE_Client.dll) funnels abort()/_exit()/_invalid_parameter()/terminate()
        // through __crtExitProcess, which calls kernel32!TerminateProcess/ExitProcess via the
        // TITLE's own IAT -- so uitrace's EAT patch never sees it, VEH is bypassed (CRT
        // termination is not an exception), and the log stops mid-sentence with no crash dump.
        // That is Fantasy Earth's GameStart crash under Proton (2026-08-19). Swapping the IAT
        // by value is the path that actually fires; the hooks log a stack backtrace naming the
        // CRT caller. All NULL until uitrace arms (app.dll present), and a NULL `from` never
        // matches, so a normal capture run before app.dll is untouched.
        { uitrace_real_ExitProcess(),      uitrace_hook_ExitProcess(),      "k32:ExitProcess" },
        { uitrace_real_TerminateProcess(), uitrace_hook_TerminateProcess(), "k32:TerminateProcess" },
        { uitrace_real_FatalAppExitA(),    uitrace_hook_FatalAppExitA(),    "k32:FatalAppExitA" },
    };

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        void** iat = (void**)(base + imp->FirstThunk);
        for (; *iat; iat++) {
            for (int k = 0; k < _countof(map); k++) {
                if (*iat != map[k].from || !map[k].from) continue;
                DWORD old;
                if (VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &old)) {
                    *iat = map[k].to;
                    VirtualProtect(iat, sizeof(void*), old, &old);
                    if (map[k].tag) {
                        char leaf[64];
                        module_leaf(mod, leaf, sizeof(leaf));
                        logf("[iat] hooked %s in %s (%p)", map[k].tag, leaf, mod);
                    }
                }
            }
        }
    }

    // Retry arming a TITLE's late hooks on EVERY module load, not just the title's
    // own. A packed title's .text is still blank at its own load; the unpacker runs
    // in its DllMain, and several more modules load during its startup -- each of
    // those is another chance, and all of them land before its first
    // OutputDebugString.
    arm_title_hooks_late();
}

// --- module sweep + late-load notification ----------------------------------

typedef struct { ULONG Flags; const UNICODE_STRING* FullDllName;
                 const UNICODE_STRING* BaseDllName; PVOID DllBase; ULONG SizeOfImage; }
        LDR_DLL_NOTIFICATION_DATA;
typedef VOID (CALLBACK *PLDR_DLL_NOTIFICATION)(ULONG, const LDR_DLL_NOTIFICATION_DATA*, PVOID);
typedef NTSTATUS (NTAPI *PFN_LdrRegister)(ULONG, PLDR_DLL_NOTIFICATION, PVOID, PVOID*);
static PVOID g_cookie = NULL;

static void handle_module(HMODULE mod)
{
    patch_iat(mod);              // CreateProcess hooks into this module's imports
    eat_patch(mod);              // if it's a target component, hook its export
    patch_signup_scheme(mod);    // no-op unless signup_http=1
    // An FFXI add-on core (Ashita, Windower) is injected into this same pol.exe by
    // its own launcher, so it arrives as a module load like any other -- and this is
    // the only notification that sees it whether it landed before our registration
    // (the startup sweep calls here too) or after. Also the moment FFXiMain appears,
    // which is when [ffxi] plugins= are loaded.
    ffxiplug_on_module((void*)mod);
    // Same notification, different question: FFXiMain arriving is also the
    // moment FFXI's settings table becomes reachable, and that table is the
    // only place the 0000..0045 registry names are written down.
    ffxicfg_on_module((void*)mod);
}

void wndguard_module_unloading(HMODULE base);   // wndguard.cpp
void wndguard_configure(const wchar_t* ini);     // wndguard.cpp

static VOID CALLBACK on_dll_event(ULONG reason,
                                  const LDR_DLL_NOTIFICATION_DATA* d, PVOID)
{
    if (reason == 1 /*LOADED*/ && d && d->DllBase)
        handle_module((HMODULE)d->DllBase);
    // UNLOADED fires while the image is still mapped -- the only moment a window
    // still holding this module's WNDPROC can be repointed at something that will
    // survive. Miss it and the next message to that window is a call into freed
    // code (FE_Client.dll+0x7BE30; see wndguard.cpp).
    else if (reason == 2 /*UNLOADED*/ && d && d->DllBase)
        wndguard_module_unloading((HMODULE)d->DllBase);
}

static void sweep_loaded()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me = { sizeof(me) };
    if (Module32First(snap, &me)) {
        do { handle_module((HMODULE)me.modBaseAddr); } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}

// --- startup ----------------------------------------------------------------

static void marker(const char* what)
{
    wchar_t tmp[MAX_PATH], path[MAX_PATH], host[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    wcscpy_s(path, tmp); wcscat_s(path, L"polshim-attach.log");
    GetModuleFileNameW(NULL, host, MAX_PATH);
    char line[MAX_PATH * 2];
    int n = wsprintfA(line, "%s pid=%lu host=%S\r\n", what, GetCurrentProcessId(), host);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, line, n, &w, NULL); CloseHandle(h); }
}

// One-arm-per-process guard. The injector (polinject.dll) and the self-loading
// proxy (PolHook.dll) are the SAME shim built from one source set; if BOTH land in
// one process -- e.g. the user launches the injector shortcut on an install that
// also carries the proxy -- two copies would EAT-patch the same exports, install
// two VEHs and start duplicate threads, and collide into an access violation. So
// only the FIRST instance to reach here arms; any later one stands down. (The
// proxy's export FORWARDERS keep working regardless -- the loader resolves those,
// not DllMain.) The mutex name carries the pid, so two different processes (parent
// pol.exe + a child) never see each other's guard and each arms exactly once.
static HANDLE g_arm_guard  = NULL;
static bool   g_armed_here = false;

// Ghost fix (2026-08-21). On ExitProcess the OS terminates every worker thread
// FIRST, then runs DllMain(DETACH) on the caller. If a worker died mid-allocation
// or mid-WinINet call, it orphans the process HEAP / WinINet lock -- a lock the
// log-teardown fix (log_begin_teardown, which only frees the LOG's own locks) does
// not cover. The DETACH summary block then allocates, wedges on that orphaned lock,
// and pol.exe becomes an invisible ghost. The worst offender is a Tetra Master
// session: [tmlog]=1 floods the log, so the [logship] LIVE streamer thread (a
// fire-and-forget CreateThread with no stop flag and no retained handle -- it
// cannot be joined) is almost always mid-POST when the close happens. So on the
// exit path we SKIP the summaries by default -- they are only end-of-session totals
// -- keeping the path lock-free and alloc-free. exit_summaries=1 restores them,
// with per-step breadcrumbs, for a diagnostic run that names the wedging summary.
static int    g_exit_summaries = 0;

// Is the INJECTED copy already mapped into this process? Checked by module NAME so it
// works even against a version-skewed injector that predates the mutex guard (an old
// polinject.dll never creates the mutex, so the mutex ALONE would let both arm). Safe
// because the injector always loads -- and arms -- while pol.exe is suspended, before
// the static-import PolHook.dll maps, so the proxy is the one that sees it here and
// stands down. We check "polinject.dll" specifically, NOT "PolHook.dll": the proxy
// shares that name with SE's ORIGINAL PolHook.dll, so a name match there would be
// ambiguous (and would wrongly make the injector stand down on a stock install).
static bool injector_present()
{
    HMODULE m = GetModuleHandleW(L"polinject.dll");
    return m && m != g_self;
}

static bool claim_arm()
{
    if (injector_present())      // the other delivery path is already armed -- defer
        return false;

    wchar_t name[64];
    swprintf_s(name, L"Local\\PolShimArmed_%lu", GetCurrentProcessId());
    HANDLE h = CreateMutexW(NULL, FALSE, name);
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);          // the first instance holds its own; we bow out
        return false;
    }
    g_arm_guard  = h;            // NULL if creation failed -> fall through and arm anyway
    g_armed_here = true;
    return true;
}

static void startup()
{
    GetModuleFileNameW(g_self, g_self_path, MAX_PATH);

    // Stand down if another copy of the shim already armed THIS process. Do it
    // before log_open (the per-pid log name means a second open would truncate the
    // armed instance's log) and before any hook, so a duplicate load stays inert.
    if (!claim_arm()) {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[polshim] pid %lu: another shim copy is already active (both the "
                    "self-loading proxy and the injector loaded) -- standing down to "
                    "avoid a double load. Retire the injector on installs that carry "
                    "PolHook.dll; the proxy replaces it.\n",
                    GetCurrentProcessId());
        OutputDebugStringA(msg);
        return;
    }

    wchar_t ini[MAX_PATH], logname[MAX_PATH], logpath[MAX_PATH];
    wcscpy_s(ini, g_self_path);
    wchar_t* slash = wcsrchr(ini, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), L"polshim.ini");

    ini_str(L"polshim", L"log", L"polshim.log", logname, MAX_PATH, ini);
    if (wcschr(logname, L'\\')) {
        wcscpy_s(logpath, logname);
    } else {
        wcscpy_s(logpath, ini);
        wchar_t* s = wcsrchr(logpath, L'\\');
        if (s) wcscpy_s(s + 1, MAX_PATH - (s + 1 - logpath), logname);
    }
    // pol.exe fans out into several injected processes; a shared "w" log would
    // truncate itself to nothing. Give each process its own: name.<pid>.log.
    {
        wchar_t stamped[MAX_PATH];
        const wchar_t* dot = wcsrchr(logpath, L'.');
        const wchar_t* sl  = wcsrchr(logpath, L'\\');
        if (dot && (!sl || dot > sl)) {
            size_t stem = dot - logpath;
            wcsncpy_s(stamped, logpath, stem);
            swprintf_s(stamped + stem, MAX_PATH - stem, L".%lu%s", GetCurrentProcessId(), dot);
        } else {
            swprintf_s(stamped, L"%s.%lu", logpath, GetCurrentProcessId());
        }
        wcscpy_s(logpath, stamped);
    }

    // Prune old per-pid logs -- see logprune.cpp. Runs BEFORE our own file is created,
    // so it can never delete this session's log. [polshim] log_keep, 0 = keep all.
    {
        int keep = GetPrivateProfileIntW(L"polshim", L"log_keep", 12, ini);
        wchar_t dir[MAX_PATH]; wcscpy_s(dir, logpath);
        wchar_t* sl = wcsrchr(dir, L'\\');
        if (sl) { *(sl + 1) = 0; log_prune(dir, logname, keep, 0); }
    }

    // BEFORE every *_configure below: they pass trace_at(N) as their default.
    polshim_trace_configure(ini);
    // [polshim] minimal -- ROUTING ONLY, the "is it the shim or the game?" switch.
    // Keeps the network redirect, login, and logging that connect this client to
    // our server; turns OFF everything that touches the GAME -- Direct3D /
    // windowing / cursor / mask, DirectInput, the FMV paths, and every byte patch
    // (the shared manager AND fepatch). Read once; a change needs a restart.
    int minimal = GetPrivateProfileIntW(L"polshim", L"minimal", 0, ini);
    if (minimal)
        logf("[polshim] MINIMAL MODE -- routing/login/logging only; all game hooks "
             "(d3d, input, FMV, mask, byte-patches) are OFF. Diagnostic use.");
    g_verbose   = GetPrivateProfileIntW(L"polshim", L"verbose",   1,   ini);
    g_max_slots = GetPrivateProfileIntW(L"polshim", L"max_slots", 128, ini);
    if (g_max_slots > POLSHIM_MAX_SLOTS) g_max_slots = POLSHIM_MAX_SLOTS;
    // Read at CONFIG time, not at DETACH time: GetPrivateProfileInt opens a file and
    // may allocate, which is exactly what the exit path must not do. Default 0 = the
    // ghost-safe path (summaries skipped on process exit).
    g_exit_summaries = GetPrivateProfileIntW(L"polshim", L"exit_summaries", 0, ini);
    patches_set_enabled(minimal ? 0 : GetPrivateProfileIntW(L"polshim", L"patches", 0, ini));
    patches_set_optional(minimal ? 0 : GetPrivateProfileIntW(L"polshim", L"patches_optional", 0, ini));
    fepatch_set_enabled(minimal ? 0 : GetPrivateProfileIntW(L"dx", L"fe_teardown_guard", 0, ini));
    g_capture     = GetPrivateProfileIntW(L"polshim", L"capture",     0,   ini);
    g_noupdate    = GetPrivateProfileIntW(L"patch",   L"noupdate",    0,   ini);
    g_comproxy    = GetPrivateProfileIntW(L"polshim", L"comproxy",    0,   ini);
    g_capture_max = GetPrivateProfileIntW(L"polshim", L"capture_max", 256, ini);
    // Accepts decimal or 0x-prefixed hex (GetPrivateProfileInt is decimal-only).
    {
        wchar_t sv[32] = L"";
        ini_str(L"polshim", L"startup_state", L"0", sv,
                _countof(sv), ini);
        g_startup_state = (int)wcstol(sv, NULL, 0) & 0xFF;
    }
    g_signup_http = GetPrivateProfileIntW(L"polshim", L"signup_http", 0, ini);
    // [polshim] minimal skips this whole block -- every game-facing hook. The
    // subsystems stay at their compiled-off defaults, so none of them installs.
    // Routing (netredir/redirect) and login (authkey) are deliberately kept.
    if (!minimal) {
    dx_configure(ini);          // [dx] section -- DirectDraw/DirectSound layer
    d3d8_configure(ini);        // [dx] d3d_* -- the games' Direct3D 8 renderer
    vidfit_configure(ini);      // [dx] d3d_fitvideo -- the FMV, via the graph
    fmvskip_configure(ini);     // [dx] fmv_skip -- skip the FMV so Wine won't decode it
    fecfg_configure(ini);       // [fecfg] -- FE GLOBAL.INI vs its VirtualStore shadow
    vmrfix_configure(ini);      // [dx] d3d_vmr_mode -- FMO's VMR-9 FMV rendering mode
    d3d9_configure(ini);        // [dx] d3d9_* -- FMO, the one d3d9 title
    wake_configure(ini);        // [dx] wake_* -- rebuild a lost device after sleep
    dinput_configure(ini);      // [dx] dinput_* -- the games' mouse
    hookspy_configure(ini);     // [dx] hook_* -- the input hook chain
    maskguard_configure(ini);   // [dx] mask_guard -- keep the Viewer's mask window alive
    protondxvk_configure(ini);  // [proton] -- keep Proton's d3d8 on DXVK (heals for NEXT launch)
    }
    com_configure(ini);         // [polshim] comtrace -- ole32 activation trace

    char inia[MAX_PATH], mods[512];
    WideCharToMultiByte(CP_ACP, 0, ini, -1, inia, MAX_PATH, NULL, NULL);
    GetPrivateProfileStringA("polshim", "modules", "polcore.dll", mods, sizeof(mods), inia);
    for (char* tok = strtok(mods, ",; "); tok && g_ntargets < 8; tok = strtok(NULL, ",; "))
        strcpy_s(g_targets[g_ntargets++], sizeof(g_targets[0]), tok);

    // [replace] -- "{CLSID}=path\to\our.dll", one per line. A CLSID listed here
    // is served by our component instead of the genuine one. The module owning
    // that CLSID must also appear in `modules`, or its export is never hooked
    // and we never get asked.
    {
        char sect[4096] = "";
        GetPrivateProfileSectionA("replace", sect, sizeof(sect), inia);
        for (char* p = sect; *p && g_nrepl < _countof(g_repl); p += strlen(p) + 1) {
            char* eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            wchar_t wc[64];
            MultiByteToWideChar(CP_ACP, 0, p, -1, wc, _countof(wc));
            CLSID c;
            if (SUCCEEDED(CLSIDFromString(wc, &c))) {
                g_repl[g_nrepl].clsid = c;
                strcpy_s(g_repl[g_nrepl].dll, sizeof(g_repl[0].dll), eq + 1);
                g_repl[g_nrepl].mod = NULL;
                g_repl[g_nrepl].gco = NULL;
                g_repl[g_nrepl].failed = false;
                g_nrepl++;
            }
            *eq = '=';
        }
    }

    if (!log_open(logpath)) {
        wchar_t tmp[MAX_PATH], fb[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        swprintf_s(fb, L"%spolshim-%lu.log", tmp, GetCurrentProcessId());
        log_open(fb);
        marker("[warn] primary log unwritable, using TEMP");
    }
    // ===================================================================
    // THE MASTER BYPASS -- `[polshim] bypass=1`. Everything below this is a
    // shim function; none of it runs.
    //
    // WHY IT EARNS ITS PLACE. "Is the shim doing this?" is the question this
    // project answers wrong most often, and answering it used to mean pulling
    // the injector, which does NOT disable the self-loading COM proxy inside
    // the install tree -- so "I ran without the shim" was routinely false.
    // With this, the answer is one ini line and one log line.
    //
    // IT LOGS, AND THAT IS THE POINT. A silent bypass would be worse than no
    // bypass: this ledger is full of fixes that never ran and were reported as
    // if they had. The line below is the PROOF that the shim stood down, and
    // it carries the build number so a stale DLL cannot masquerade as a bypass.
    //
    // THE KEY IS `bypass`, NOT ANOTHER `enable`. Deliberate: a session was
    // lost to reading one section's `enable=0` as another's, so this name
    // exists nowhere else in the ini and cannot be confused with a subsystem
    // switch. Section-scoped to [polshim], the section that owns the DLL.
    //
    // Placed AFTER log_open and BEFORE autoupdate_boot_guard on purpose: the
    // guard rewrites the DLL on disk, which is a shim function acting on
    // itself and precisely the thing you want stopped while bisecting.
    //
    // ONE THING SURVIVES, DELIBERATELY: the settings watcher. Without it a
    // bypass ticked in the dialog could only be undone by hand-editing the ini
    // on a machine that may not have a keyboard in front of it -- a Deck in
    // Game Mode -- so the switch would be a trap rather than a tool. The
    // way back was asked for and it is the right call: an instrument
    // you cannot put down does not get picked up.
    //
    // WARNING: SO THE MARKER MUST NOT SAY "as if the shim were not installed" -- it
    // said exactly that until the watcher was kept, and it would have been a
    // lie by one thread. It names what is still running instead. What that
    // thread does is bounded and worth stating: `polsettings_start` resolves
    // XInput and polls the keyboard and pad every 60 ms on its own thread. It
    // installs NO hook, patches nothing, and touches no client memory; the pad
    // overlay it can open is self-guarding (`padoverlay_open` refuses unless
    // `padoverlay_configure` ran, and under bypass it did not).
    // ===================================================================
    if (GetPrivateProfileIntW(L"polshim", L"bypass", 0, ini)) {
        logf("[polshim] *** BYPASS: [polshim] bypass=1 -- the shim is standing "
             "down. No hooks, no probes, no patches, no redirect, no autoupdate, "
             "no automation, no COM interposition. STILL RUNNING: the settings "
             "watcher ONLY -- one polling thread, no hooks, no client memory "
             "touched -- so the dialog can turn this back off. "
             "(build %d, pid %lu) Untick it in Settings, or clear bypass= in "
             "polshim.ini; either way it re-arms on the NEXT launch.",
             POLSHIM_BUILD, GetCurrentProcessId());
        log_flush();
        polsettings_start(ini);
        return;
    }

    // The update boot guard, FIRST -- before proxy_init, every hook install and
    // every EAT patch, because those are exactly where a bad staged build would
    // crash, and the guard's whole job is to run before the crash does. If the
    // last session staged a DLL that keeps dying before confirming itself, this
    // restores the kept .old (see autoupdate.cpp). It cannot go any earlier:
    // it logs, so it needs log_open() above. A staged DLL the LOADER rejects
    // dies before ANY of our code -- that class stays uncoverable in-process.
    autoupdate_boot_guard(ini, g_self_path);
    proxy_init();
    InitializeCriticalSection(&g_mlock);
    InitializeCriticalSection(&g_lwlock);
    g_lwlock_ready = true;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    real_CreateProcessW = (PFN_CPW)GetProcAddress(k32, "CreateProcessW");
    real_CreateProcessA = (PFN_CPA)GetProcAddress(k32, "CreateProcessA");
    // FIRST line in every log, on purpose. The shim is loaded once at process
    // start, so relaunching only the GAME inside an already-running Viewer keeps
    // whatever build was injected then -- and a rebuild made meanwhile is simply
    // not there. That has now cost two full test cycles, each looking like "the
    // new hook produced no output" when the new hook was never loaded. With this,
    // one glance at the top of the log settles which build is actually running.
    logf("[polshim] PoL-Shim v%s  build %s %s", POLSHIM_VERSION, __DATE__, __TIME__);
    // This process's OWN command line. pol.exe's argv scan (pol.exe+0x14710) reads
    // "/game <token>" from here and copies the token into its shortcut buffer, so a
    // shortcut launched directly (or via a launcher wrapper) shows up on this line;
    // a title's polboot passes it to the pol.exe CHILD instead, logged as
    // "[shortcut] child" from the CreateProcess hooks. Together they capture every
    // real token before we synthesise one.
    {
        const wchar_t* cl = GetCommandLineW();
        logf("[shortcut] self pid=%lu cmd=\"%ls\"",
             GetCurrentProcessId(), cl ? cl : L"(null)");
    }
    // The DPI declaration had to happen up in dx_configure -- before any window
    // exists -- which is well before log_open() above, so it could not log its
    // own outcome. Replay it here. (Anything else that logs from the *_configure
    // block at the top of this function is being silently discarded for the same
    // reason; that block runs ~40 lines before the log is open.)
    dpi_log_result();
    fmvskip_log_result();   // ...and [dx] fmv_skip, gated off on Windows, for the same reason
    // FIRST thing after the banner, and deliberately before iniheal and every
    // other *_configure below: everything past this point installs hooks and
    // patches bytes, and a crash in any of it is exactly what we want reported.
    crashlog_arm(ini);
    iniheal(ini);               // add any missing user-facing options to polshim.ini (defaults)
    // ...and then repair the ones that are PRESENT but sitting on a default we
    // have since replaced. Strictly after iniheal: heal writes current defaults
    // for absent keys, so anything migrate still sees is a value the file really
    // carries. Once per install, stamped by [polshim] config_rev -- iniheal.cpp.
    inimigrate(ini);
    // dbgstring: remember whether the ini NAMES it, because the value alone cannot say
    // so and the auto-arm below must never override a deliberate 0.
    {
        wchar_t v[32] = L"";
        ini_str(L"polshim", L"dbgstring", L"", v, _countof(v), ini);
        g_dbgstring_explicit = (v[0] != 0);
        g_dbgstring = g_dbgstring_explicit ? _wtoi(v) : trace_at(2);
    }
    profiles_log_table();
    polfetch_configure(ini);
    fmokey_configure(ini);
    tmpathfix_init(ini);        // [dx] tm_pathfix -- TM gW path parser fix (default ON, no-op unless the site matches)
    reslock_init(ini);          // [polshim] reslock -- GlobalLock on a resource pointer returns it as-is (FMO Deck game-start fix)
    flwindow_init(ini);         // [dx] fl_window_transparent -- key out the Friend List window's black surround on the Deck
    fmoime_configure(ini);      // [dx] fmo_ime_direct -- FMO text fields start in direct input, not Japanese
    authkey_configure(ini);
    msgxlate_configure(ini);
    uitrace_config(ini);
    // IMPORTANT: RESOLVED UNCONDITIONALLY, and that is the whole trick. These four pointers are
    // the `from` side of the IAT hook table below, and a NULL `from` never matches -- so
    // leaving them unresolved when dbgstring is off does not merely silence the output,
    // it means the hook is NEVER INSTALLED and can never be switched on later. Resolving
    // is four GetProcAddress calls; the emit path is what g_dbgstring actually gates
    // (hook_OutputDebugStringA/W and dbg_emit all test it). That separation is what lets
    // a title's arrival arm this mid-session -- see apply_module_hooks.
    real_OutputDebugStringA = (PFN_ODSA)GetProcAddress(k32, "OutputDebugStringA");
    real_OutputDebugStringW = (PFN_ODSW)GetProcAddress(k32, "OutputDebugStringW");
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            real_MessageBoxA = (PFN_MBA)GetProcAddress(u32, "MessageBoxA");
            real_MessageBoxW = (PFN_MBW)GetProcAddress(u32, "MessageBoxW");
            // KEY: EAT AS WELL AS IAT -- otherwise a PACKED title is never caught.
            //
            // The IAT patch below matches by resolved address, which works for a
            // module whose imports are bound when we walk it. TM.dll is not one:
            // it is POL1/ASProtect-packed and re-resolves its own imports after
            // unpacking, so `[iat] hooked u32:MessageBoxA` has only ever named
            // FrontMissionOnline.dll. The consequence, reported 2026-09-08: Tetra
            // Master's startup error came up as mojibake ("‚Å‚«‚Ü‚¹‚ñ‚Å‚µ‚½B" is
            // CP932 "...できませんでした。" rendered in the system codepage) and
            // nothing from TM ever reached polshim-msg-missing.tsv, so there was
            // no string to translate and no record of what the error even said.
            //
            // Same answer d3d8hook already uses for the same module and the same
            // reason ("EAT patched d3d8!Direct3DCreate8 ... beats the packed TM.dll
            // re-resolving its imports, which the IAT patch alone loses to").
            if (real_MessageBoxA)
                dx_eat_patch_export(u32, "MessageBoxA", (void*)hook_MessageBoxA,
                                    "user32!MessageBoxA");
            if (real_MessageBoxW)
                dx_eat_patch_export(u32, "MessageBoxW", (void*)hook_MessageBoxW,
                                    "user32!MessageBoxW");
        }
    }
    if (g_startup_state) {
        // Resolved only when the override is on, so the file-I/O hooks stay out
        // of every IAT (and off every hot path) in a normal capture run.
        real_CreateFileW = (PFN_CFW)GetProcAddress(k32, "CreateFileW");
        real_CreateFileA = (PFN_CFA)GetProcAddress(k32, "CreateFileA");
        real_ReadFile    = (PFN_RF) GetProcAddress(k32, "ReadFile");
        real_WriteFile   = (PFN_WF) GetProcAddress(k32, "WriteFile");
        real_CloseHandle = (PFN_CLH)GetProcAddress(k32, "CloseHandle");
        real_MoveFileA   = (PFN_MVA)GetProcAddress(k32, "MoveFileA");
    }
    // polcore imports these by ORDINAL, but the IAT patch matches by resolved
    // address, so looking them up by name here is fine -- both routes end at
    // the same function pointer.
    //
    // GetModuleHandleW, NOT LoadLibraryW: this whole function runs under the
    // loader lock (DllMain -> startup()), and LoadLibraryW of a not-yet-loaded
    // module maps it and runs its DllMain under our held lock -- a recursive-load
    // deadlock, the ghost-pol.exe class. ws2_32 is statically imported by pol.exe,
    // so it is already mapped before any DllMain runs and GetModuleHandleW always
    // finds it (GetProcAddress works on a mapped module regardless of init order).
    // This also drops the reference we used to leak.
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (ws2) {
        real_connect  = (PFN_CONNECT) GetProcAddress(ws2, "connect");
        real_send     = (PFN_SEND)    GetProcAddress(ws2, "send");
        real_recv     = (PFN_RECV)    GetProcAddress(ws2, "recv");
        real_sendto   = (PFN_SENDTO)  GetProcAddress(ws2, "sendto");
        real_recvfrom = (PFN_RECVFROM)GetProcAddress(ws2, "recvfrom");
        real_wsasend  = (PFN_WSASEND) GetProcAddress(ws2, "WSASend");
        real_wsarecv  = (PFN_WSARECV) GetProcAddress(ws2, "WSARecv");
        real_socket   = (PFN_SOCKET)  GetProcAddress(ws2, "socket");
        real_bind     = (PFN_BIND)    GetProcAddress(ws2, "bind");
    }

    // Must precede sweep_loaded(): patch_iat matches by resolved address, so the
    // DirectX entry points have to be resolved before the first IAT is walked.
    dx_resolve();               // no-op unless [dx] enable=1
    d3d8_resolve();             // ditto, unless [dx] d3d_enable=1
    d3d9_resolve();             // ditto -- FMO's renderer, invisible to d3d8_resolve
    dinput_resolve();           // ditto, unless [dx] dinput_enable=1
    hookspy_resolve();          // must precede sweep_loaded() like the rest
    com_resolve();              // ditto -- must also precede sweep_loaded()
    tmpathfix_eat_patch();      // EAT-patch kernel32!FindFirstFileA: catch TM's gW glob to patch the parser pre-index
    netredir_init(ini);         // reads [redirect]; no-op unless enable=1
    netredir_resolve();         // ditto -- must precede sweep_loaded() as well
    installdir_init(ini);       // reads [installdir]; folder-aware install redirect, no-op unless enable=1
    swapconfirm_configure(ini); // reads [inputmode] swap_confirm; rides the RegQueryValueEx hooks
    regserve_configure(ini);    // reads [reg:...]; rides the SAME hooks, so it must precede installdir_resolve
    installdir_resolve();       // resolve real RegQueryValueEx; must precede sweep_loaded() too
    installdir_eat_patch();     // MUST follow installdir_resolve(): it rewrites advapi32's
                                // export table, and the pass-through needs the pre-patch
                                // pointers. polcore has no import directory at all (POL1
                                // packer), so the IAT route below can never reach it.
    regfix_run(ini);            // reads [regfix]; registers installed-but-unregistered titles, no-op unless enable=1
    fecfg_run(ini);             // reads [fecfg]; reconciles FE's GLOBAL.INI, no-op unless enable=1
    poltoken_configure(ini);   // [poltoken]; stamps the POL session id into FFXI's lobby packets
    ffxiplug_configure(ini);    // reads [ffxi]; Ashita/Windower coexistence + the plugin list
    ffxicfg_configure(ini);     // reads [ffxi] cfgdump; the FFXI settings-table dump (READ-ONLY)
    inputgate_configure(ini);   // reads [dx] mouse_focus_gate; withholds the mouse while another app is in front
    keystate_init(ini);         // resolves GetAsyncKeyState/GetKeyState/GetKeyboardState for the [dx] key_focus_gate swap below
    exitprompt_configure(ini);  // reads [dx] close_prompt; what the title bar X does
    ffxiplug_start();           // no-op unless load_at=startup
    sessionwatch_configure(ini);// reads [session]; logs a lost session socket
    inputmode_configure(ini);   // reads [inputmode]; no-op unless mode=force_gamepad|force_keyboard
    inputmode_apply();          // patch pol.exe's settings loader IN MEMORY, before its entry runs
    shortcut_configure(ini);    // reads [shortcut] game; hooks pol.exe GetCommandLineW before argv parse
    // MUST follow installdir_resolve(): padmap resolves each action's slot through
    // regredir, which reads the genuine registry via the saved RegQueryValueEx pointer.
    padmap_configure(ini);      // reads [inputmode] pad_remap/pad_bind; the live button map
    padoverlay_configure(ini);  // the SAME map, drawn on the shell's own DirectDraw surface
                                // -- the only feedback a Steam Deck can actually see
    wndguard_configure(ini);    // reads [polshim] wndproc_guard; neutralises a WNDPROC
                                // left behind by a DLL that unloads with a live window
    titletag_configure(ini);    // reads [polshim] titletag; builds the version tag string
    // BEFORE polsettings_start, which parses the report key and logs whether it is
    // armed -- that line reads polreport_armed(). Neither call sends anything: the
    // report key gathers and asks first, and only the player's Send posts it.
    logship_configure(ini, logpath);   // the report's log snapshot + redaction + endpoint
    polreport_configure(ini);   // reads [report]; the player's one-key bug report
    polsettings_start(ini);     // reads [settings]; watcher thread for the settings-dialog chord

    logf("[init] polshim in pid %lu; targets=%d verbose=%d capture=%d dx=%d comtrace=%d redirect=%d",
         GetCurrentProcessId(), g_ntargets, g_verbose, g_capture,
         dx_enabled(), comtrace_enabled(), netredir_enabled());
    if (g_startup_state)
        logf("[init] startup_state=0x%02x -- serving login_w.bin +0x%02x from "
             "memory (0x19 = in-client sign-up wizard)",
             g_startup_state, LOGIN_W_STATE_OFF);
    for (int i = 0; i < g_ntargets; i++) logf("[init] target: %s", g_targets[i]);
    // Create the profile mail directories the Viewer needs but never creates --
    // a missing usr\home<NN>\EMAIL is what the client reports as POL-0000.
    // Deliberately AFTER log_open(): called any earlier it still did the work
    // but its log lines went nowhere, which is exactly the kind of silent
    // success that wastes an afternoon later.
    mailstore_preflight(ini);
    for (int i = 0; i < g_nrepl; i++) {
        char c[64];
        logf("[init] replace: %s -> %s", iid_name(g_repl[i].clsid, c, sizeof(c)), g_repl[i].dll);
    }

    sweep_loaded();

    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PFN_LdrRegister reg = (PFN_LdrRegister)GetProcAddress(nt, "LdrRegisterDllNotification");
    if (reg && reg(0, on_dll_event, NULL, &g_cookie) == 0)
        logf("[init] DLL load notifications registered");
    else
        logf("[warn] LdrRegisterDllNotification unavailable; late modules may be missed");

    // Append the shim version to the Viewer window title so the running build is
    // visible (and thus whether the latest patch-channel update reached the user).
    titletag_start();

    // Guard pol.exe's PlayOnlineMask* window. Started HERE, at process init, and
    // not from the d3d path on purpose: d3d8hook's tame_mask_window() only runs
    // after a successful CreateDevice, so a title that dies during launch -- which
    // is what Fantasy Earth and FMO do under gamescope -- was never covered and
    // never even logged. Starting a thread from DllMain is safe here (the loader
    // lock releases before it runs; thread lib-calls are disabled).
    maskguard_start();

    // Self-update, last and on a delay: the Viewer's patch channel structurally
    // cannot replace this DLL on Windows (pol.exe imports it statically, so it is
    // mapped and polcore's overwrite fails), and renaming a mapped file is the one
    // operation Windows does allow. Off unless [autoupdate] enable=1. Same
    // thread-from-DllMain rule as titletag/maskguard.
    autoupdate_start(ini, g_self_path);

    log_flush();
}

// ---------------------------------------------------------------- live reload
//
// Called by the settings dialog's Save (polsettings.cpp) after it writes the ini,
// so a changed value reaches the RUNNING shim instead of waiting for a relaunch.
// This re-reads the same safe subset startup() reads into plain flag globals --
// values that are consumed per call, per poll or per title launch -- and then
// fans out to every module's *_reload(). What it deliberately does NOT do:
//
//   * probes / modules / [replace]: arming INT3 probes needs the VEH installed
//     first and the EAT-patch target list is walked at load time -- both are
//     startup()'s one-shot territory, and re-running them here could arm a
//     breakpoint with no handler behind it.
//   * anything *_resolve() or *_start(): hook installs and thread creation stay
//     one-shot. Where a module can safely restart its own worker (autoupdate,
//     polctl), its OWN reload does it behind a guard.
//
// polshim_trace_configure comes FIRST for the same reason it does in startup():
// the module reloads pass trace_at(N) as their defaults.
void shim_reload(const wchar_t* ini)
{
    // NAME THE FILE. There are two ini locations and picking the wrong one is
    // the oldest false negative in this project -- "the setting did nothing"
    // reads identically to "you edited the other ini". A Save that says which
    // file it re-read settles that in one line, and it is also the marker that
    // makes "did live-apply actually run?" answerable at all: no shipped log has
    // ever contained a [reload] line.
    logf("[reload] ---- SAVE: re-reading %ls ----", ini ? ini : L"(null)");
    polshim_trace_configure(ini);
    g_verbose   = GetPrivateProfileIntW(L"polshim", L"verbose",   1,   ini);
    g_max_slots = GetPrivateProfileIntW(L"polshim", L"max_slots", 128, ini);
    if (g_max_slots > POLSHIM_MAX_SLOTS) g_max_slots = POLSHIM_MAX_SLOTS;
    g_exit_summaries = GetPrivateProfileIntW(L"polshim", L"exit_summaries", 0, ini);
    patches_set_enabled(GetPrivateProfileIntW(L"polshim", L"patches", 0, ini));
    patches_set_optional(GetPrivateProfileIntW(L"polshim", L"patches_optional", 0, ini));
    fepatch_set_enabled(GetPrivateProfileIntW(L"dx", L"fe_teardown_guard", 0, ini));
    g_capture     = GetPrivateProfileIntW(L"polshim", L"capture",     0,   ini);
    g_noupdate    = GetPrivateProfileIntW(L"patch",   L"noupdate",    0,   ini);
    g_comproxy    = GetPrivateProfileIntW(L"polshim", L"comproxy",    0,   ini);
    g_capture_max = GetPrivateProfileIntW(L"polshim", L"capture_max", 256, ini);
    {
        wchar_t sv[32] = L"";
        ini_str(L"polshim", L"startup_state", L"0", sv, _countof(sv), ini);
        g_startup_state = (int)wcstol(sv, NULL, 0) & 0xFF;
    }
    g_signup_http = GetPrivateProfileIntW(L"polshim", L"signup_http", 0, ini);
    {
        wchar_t v[32] = L"";
        ini_str(L"polshim", L"dbgstring", L"", v, _countof(v), ini);
        g_dbgstring_explicit = (v[0] != 0);
        g_dbgstring = g_dbgstring_explicit ? _wtoi(v) : trace_at(2);
    }

    dx_reload(ini);
    d3d8_reload(ini);
    d3d9_reload(ini);
    wake_reload(ini);
    dinput_reload(ini);
    hookspy_reload(ini);
    inputgate_reload(ini);
    exitprompt_reload(ini);
    maskguard_reload(ini);
    fmvskip_reload(ini);
    vidfit_reload(ini);
    fmoime_reload(ini);
    com_reload(ini);
    polfetch_reload(ini);
    authkey_reload(ini);
    netredir_reload(ini);
    sessionwatch_reload(ini);
    autoupdate_reload(ini);
    padmap_reload(ini);
    padoverlay_reload(ini);
    titletag_reload(ini);
    ffxiplug_reload(ini);
    crashlog_reload(ini);

    logf("[reload] settings re-read; verbose=%d capture=%d/%d noupdate=%d comproxy=%d",
         g_verbose, g_capture, g_capture_max, g_noupdate, g_comproxy);
    log_flush();
}

// RE-READ THE SETTINGS WITH ONE TITLE IN SCOPE.
//
// Called from d3d8hook's
// d3d_title_boundary() as each title starts, and (later) from the settings dialog
// when Save happens while a title is running -- ONE function for both, so the
// "value applied at launch" and "value applied on Save" paths cannot drift into
// disagreeing about what a per-title key means.
//
// DELIBERATELY NARROW. shim_reload() fans out to 25 modules and prints 25 lines;
// running that per title launch would bury the log for no gain, because only the
// modules that own per-title keys can produce a different answer with a title in
// scope. Add a module here when, and only when, one of its keys is marked
// per_title.
//
// The scope is cleared on the way out even though it is thread-local: the title
// launch thread goes on to do other work, and a leaked scope would quietly make
// every later global read on that thread resolve per-title.
void shim_reload_for_title(const wchar_t* ini, const char* leaf)
{
    if (!ini || !*ini) return;

    // SAY WHAT IS IN FORCE. A per-title override that works silently is the same
    // support problem as one that silently does nothing -- "which settings is this
    // game running under?" is the question every incident actually
    // needed answered. One line per overridden key, once per launch, and nothing at
    // all for the overwhelmingly common case of no section.
    if (leaf && *leaf) {
        wchar_t sec[96], wleaf[80];
        if (MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf, _countof(wleaf)) > 0) {
            static const wchar_t* const kSections[] = { L"dx", L"polshim" };
            for (int s = 0; s < _countof(kSections); s++) {
                _snwprintf_s(sec, _countof(sec), _TRUNCATE, L"%s.%s", kSections[s], wleaf);
                wchar_t buf[2048] = L"";
                DWORD n = GetPrivateProfileSectionW(sec, buf, _countof(buf) - 2, ini);
                if (!n) continue;
                for (const wchar_t* e = buf; *e; e += wcslen(e) + 1)
                    logf("[title]   override [%ls] %ls", sec, e);
            }
        }
    }

    title_scope_set(leaf);
    d3d8_reload(ini);
    d3d9_reload(ini);
    dinput_reload(ini);
    vidfit_reload(ini);
    fmvskip_reload(ini);
    title_scope_set(NULL);
}

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hmod;
        DisableThreadLibraryCalls(hmod);
        marker("inject");
        startup();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!g_armed_here) return TRUE;      // a stood-down duplicate armed nothing

        // lpReserved != NULL means "the process is terminating", NULL means someone
        // called FreeLibrary -- the documented way to tell the two apart, and the
        // difference matters enormously here. On process termination ExitProcess has
        // ALREADY terminated every other thread, so:
        //
        //   * the *_stop() calls below have nothing left to stop. Each one signals a
        //     worker and waits for it; waiting on a thread that ExitProcess killed
        //     mid-critical-section is a deadlock with no timeout, under the loader
        //     lock, with no windows left on screen -- the invisible ghost pol.exe.
        //   * the log's locks now protect against nobody, and any of them may have
        //     been orphaned by a worker that died inside logf(). log_begin_teardown()
        //     drops them so the summary block below can never wedge.
        //
        // The summaries themselves are kept: they are the diagnostic record of the
        // session, and after log_begin_teardown() writing them is lock-free.
        const bool process_exiting = (lpReserved != NULL);
        if (process_exiting) {
            log_begin_teardown();
            // Ghost fix: keep this path lock-free AND alloc-free. The summary block
            // below allocates; on ExitProcess a worker (logship live during a TM
            // session) may have orphaned the heap/WinINet lock, so a summary that
            // allocates wedges there -> ghost. Skip them by default. See g_exit_summaries.
            logf("[teardown] process exiting -- lock-free path; summaries %s",
                 g_exit_summaries ? "ON (diagnostic run)" : "SKIPPED (ghost-safe default)");
            log_flush();
            // A clean exit is the cheap half of the update boot guard's
            // confirmation: reaching DETACH at all means this session did not
            // crash (WER terminates without it). Alloc-free by contract -- one
            // flag test, one tick compare, one DeleteFileW -- so it is safe on
            // this path. See autoupdate_boot_confirm in autoupdate.cpp.
            autoupdate_boot_confirm();
        } else {
            // FreeLibrary path (rare: this DLL is statically imported and
            // DllCanUnloadNow returns S_FALSE, so it never actually unmaps). We run
            // under the loader lock here, so every *_stop() below is signal-only and
            // MUST NOT join its worker -- waiting on a thread that needs the loader
            // lock to exit stalls or deadlocks under the lock. Each _stop() enforces
            // that itself; keep it that way if you touch them.
            if (g_arm_guard) CloseHandle(g_arm_guard);
            // Clean-exit confirmation for the update boot guard, BEFORE the stop
            // below latches g_stop. Reaching this path is as much proof of a
            // healthy session as process exit is.
            autoupdate_boot_confirm();
            titletag_stop();
            autoupdate_stop();
            maskguard_stop();
            fmokey_stop();          // signal-only daemon; safe under the loader lock
            fmoime_stop();          // ditto -- polls for FrontMissionOnline.dll
        }
        // On process exit the summaries are skipped (ghost-safe) unless
        // exit_summaries=1 turns them back on for a diagnostic run. On the
        // FreeLibrary path no worker was ExitProcess-killed, so they always run.
        // STEP() drops a lock-free breadcrumb before each one ONLY on the exit
        // path, so a diagnostic run that still ghosts names the wedging summary in
        // its last logged line.
        if (!process_exiting || g_exit_summaries) {
        #define STEP(n) do { if (process_exiting) { logf("[teardown] step: %s", n); log_flush(); } } while (0)
            STEP("maskguard"); maskguard_summary();
            STEP("dx");        dx_summary();
            STEP("d3d8");      d3d8_summary();
            STEP("d3d9");      d3d9_summary();
            STEP("wake");      wake_summary();
            STEP("dinput");    dinput_summary();
            STEP("hookspy");   hookspy_summary();
            STEP("com");       com_summary();
            STEP("polfetch");  polfetch_summary();
            STEP("fmokey");    fmokey_summary();
            STEP("fmoime");    fmoime_summary();
            STEP("authkey");   authkey_summary();
            STEP("msgxlate");  msgxlate_summary();
            STEP("uitrace");   uitrace_summary();
            STEP("ffxicfg");   ffxicfg_summary();
            STEP("inputgate"); inputgate_summary();
            STEP("keystate");  keystate_summary();
            STEP("exitprompt"); exitprompt_summary();
            STEP("regserve");  regserve_summary();
        #undef STEP
        }
        if (process_exiting) { logf("[teardown] step: log_close"); log_flush(); }
        log_close();
    }
    return TRUE;
}
