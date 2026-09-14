#include "polshim.h"
#include "com_allow.h"   // typed engine: IID -> exact vtable slot count

// The IClassFactory IID, defined locally so proxy.cpp needn't link uuid.lib.
static const GUID k_IClassFactory =
    { 0x00000001, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };

// ---------------------------------------------------------------------------
// The trick that makes signature-free interposition work on x86.
//
// COM interface methods are __stdcall: every argument is on the stack and the
// CALLEE cleans up. We don't know the argument count, so we can't rebuild the
// frame. Instead we leave the frame exactly as the caller built it, swap the
// `this` slot for the real interface pointer, and replace the RETURN ADDRESS
// with our own trampoline. The real method then runs untouched and cleans its
// own arguments with `ret N`; control lands in our trampoline with the return
// value still in EAX:EDX and ESP already correct. We log, then jump to the
// caller's original return address.
//
// Per-thread shadow stack keeps the real return addresses (and anything we
// stashed at entry) so nesting and recursion behave.
// ---------------------------------------------------------------------------

struct SlotCtx {
    Proxy* proxy;
    int    slot;
};

struct Frame {
    void*     retaddr;
    Proxy*    proxy;
    int       slot;
    void**    out_ppv;     // QueryInterface / CreateInstance out-param, if any
    int       wrap_kind;   // what to wrap *out_ppv as on the way back (legacy engine)
    GUID      req_iid;     // the requested IID of that out-param (typed engine)
};

struct ThreadState {
    int   depth;
    Frame frames[POLSHIM_MAX_DEPTH];
};

static DWORD g_tls = TLS_OUT_OF_INDEXES;

static ThreadState* tls_get()
{
    ThreadState* ts = (ThreadState*)TlsGetValue(g_tls);
    if (!ts) {
        ts = (ThreadState*)VirtualAlloc(NULL, sizeof(ThreadState),
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        // Only store a successful allocation. On failure return NULL and let the
        // caller degrade to a transparent pass-through (polshim_enter leaves the
        // return address intact when no frame can be pushed) -- storing NULL here
        // would be harmless but retrying next call is the intent.
        if (ts) TlsSetValue(g_tls, ts);
    }
    return ts;
}

// --- proxy registry --------------------------------------------------------

static CRITICAL_SECTION g_lock;
static Proxy*  g_proxies[1024];
static int     g_nproxies = 0;
static BYTE*   g_stub_pool = NULL;
static size_t  g_stub_used = 0;
static size_t  g_stub_cap  = 0;

extern "C" void __stdcall polshim_dispatch();
extern "C" void __stdcall polshim_onreturn();

// --- vtable probing ---------------------------------------------------------
//
// Walk the vtable until an entry stops looking like a function pointer inside a
// mapped image. Cheap, and reliable enough in practice because the compiler
// packs vtables contiguously and whatever follows is rarely a valid code page.

static bool is_code_ptr(void* p)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Type != MEM_IMAGE && mbi.Type != MEM_PRIVATE) return false;
    DWORD x = mbi.Protect & 0xFF;
    return x == PAGE_EXECUTE || x == PAGE_EXECUTE_READ ||
           x == PAGE_EXECUTE_READWRITE || x == PAGE_EXECUTE_WRITECOPY;
}

static int probe_vtable_len(void** vt)
{
    int n = 0;
    for (; n < g_max_slots; n++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(&vt[n], &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            break;
        if (!is_code_ptr(vt[n]))
            break;
    }
    return n;
}

static void describe_vtable(void** vt, Proxy* p)
{
    p->vtbl_rva = 0;
    strcpy_s(p->module, "?");
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(vt, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
        char path[MAX_PATH];
        if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) {
            const char* base = strrchr(path, '\\');
            strcpy_s(p->module, base ? base + 1 : path);
        }
        p->vtbl_rva = (DWORD)((BYTE*)vt - (BYTE*)mbi.AllocationBase);
    }
}

// --- stub emission ----------------------------------------------------------
//
//   68 <ctx>     push   <SlotCtx*>
//   E9 <rel32>   jmp    polshim_dispatch

static void* emit_stubs(Proxy* p, int nslots)
{
    size_t need = (size_t)nslots * 10 + (size_t)nslots * sizeof(SlotCtx);
    if (!g_stub_pool || g_stub_used + need > g_stub_cap) {
        g_stub_cap  = 1 << 20;
        g_stub_pool = (BYTE*)VirtualAlloc(NULL, g_stub_cap, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
        g_stub_used = 0;
        if (!g_stub_pool) return NULL;
    }

    BYTE*    code = g_stub_pool + g_stub_used;
    SlotCtx* ctxs = (SlotCtx*)(code + nslots * 10);
    g_stub_used += need;

    for (int i = 0; i < nslots; i++) {
        ctxs[i].proxy = p;
        ctxs[i].slot  = i;
        BYTE* s = code + i * 10;
        s[0] = 0x68;                                        // push imm32
        *(DWORD*)(s + 1) = (DWORD)(UINT_PTR)&ctxs[i];
        s[5] = 0xE9;                                        // jmp rel32
        *(DWORD*)(s + 6) = (DWORD)((BYTE*)polshim_dispatch - (s + 10));
    }
    return code;
}

// --- wrapping ---------------------------------------------------------------

bool proxy_init()
{
    g_tls = TlsAlloc();
    InitializeCriticalSection(&g_lock);
    return g_tls != TLS_OUT_OF_INDEXES;
}

void* proxy_wrap(void* iface, int kind, const char* origin)
{
    if (!iface) return NULL;

    EnterCriticalSection(&g_lock);

    // COM identity: the same interface pointer must always yield the same
    // proxy, or the host's pointer comparisons start lying.
    for (int i = 0; i < g_nproxies; i++) {
        if (g_proxies[i]->real == iface) {
            void* r = g_proxies[i];
            LeaveCriticalSection(&g_lock);
            return r;
        }
    }
    // Already one of ours coming back around? Don't double-wrap.
    for (int i = 0; i < g_nproxies; i++) {
        if (g_proxies[i] == iface) {
            LeaveCriticalSection(&g_lock);
            return iface;
        }
    }

    if (g_nproxies >= _countof(g_proxies)) {
        LeaveCriticalSection(&g_lock);
        return iface;
    }

    void** vt = *(void***)iface;
    int n = probe_vtable_len(vt);
    if (n < 3) {                       // not a plausible COM interface
        LeaveCriticalSection(&g_lock);
        return iface;
    }

    Proxy* p = (Proxy*)VirtualAlloc(NULL, sizeof(Proxy), MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    memset(p, 0, sizeof(*p));
    p->real      = iface;
    p->real_vtbl = vt;
    p->nslots    = n;
    p->kind      = kind;
    p->id        = g_nproxies;
    describe_vtable(vt, p);

    void* code = emit_stubs(p, n);
    if (!code) {
        LeaveCriticalSection(&g_lock);
        return iface;
    }

    void** fake = (void**)VirtualAlloc(NULL, sizeof(void*) * n,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < n; i++)
        fake[i] = (BYTE*)code + i * 10;
    p->fake_vtbl = fake;
    p->stubs     = code;

    g_proxies[g_nproxies++] = p;
    LeaveCriticalSection(&g_lock);

    logf("[wrap] #%d %s+0x%06X slots=%d kind=%d via=%s real=%p",
         p->id, p->module, p->vtbl_rva, n, kind, origin, iface);
    return p;
}

// --- the TYPED engine -------------------------------------------------------
//
// Same stub/dispatch machinery as proxy_wrap, but three policy changes that
// together make it safe where the heuristic wrapper was not:
//   * IID-scoped   -- wrap ONLY interfaces on the com_allow.h allow-list;
//                     everything else (transient system COM) passes through.
//   * exact size   -- the vtable slot count comes from the typelib, never from
//                     probe_vtable_len, so real_vtbl[slot] can't run off the end.
//   * lifetime     -- a Release that drops the real object to 0 RETIRES the proxy
//                     (see typed_retire in polshim_leave), so the registry and
//                     identity cache never hold a dangling real_vtbl.

int proxy_count() { EnterCriticalSection(&g_lock); int n = g_nproxies; LeaveCriticalSection(&g_lock); return n; }

static void typed_retire(Proxy* p)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_nproxies; i++)
        if (g_proxies[i] == p) { g_proxies[i] = g_proxies[--g_nproxies]; break; }
    LeaveCriticalSection(&g_lock);
    if (g_verbose) logf("[typed] #%d retired (real released to 0)", p->id);
    // DO NOT free p or p->fake_vtbl. fake_vtbl IS the interface pointer the host was
    // handed, and the shared stub pool still holds SlotCtx pointers back into p. A host
    // that over-releases, or another thread racing this final Release on the same
    // interface, would then dispatch through freed memory -- a hard use-after-free.
    // Removing p from the registry above is all that is needed to keep the identity
    // cache and the summary off the dead real object; the Proxy and its fake vtable are
    // leaked DELIBERATELY (like the stub pool), a bounded few KB per created-and-dropped
    // interface. The stub bytes stay in the shared pool regardless.
}

void* typed_wrap(void* iface, REFIID iid)
{
    if (!iface) return NULL;
    int slots = com_allow_slots(iid);
    if (slots == 0) return iface;              // NOT allow-listed -> pass through

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_nproxies; i++)       // same real -> same proxy (identity)
        if (g_proxies[i]->real == iface) { void* r = g_proxies[i]; LeaveCriticalSection(&g_lock); return r; }
    for (int i = 0; i < g_nproxies; i++)       // already one of ours coming back?
        if (g_proxies[i] == iface) { LeaveCriticalSection(&g_lock); return iface; }
    if (g_nproxies >= _countof(g_proxies)) { LeaveCriticalSection(&g_lock); return iface; }

    Proxy* p = (Proxy*)VirtualAlloc(NULL, sizeof(Proxy), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memset(p, 0, sizeof(*p));
    p->real      = iface;
    p->real_vtbl = *(void***)iface;
    p->nslots    = slots;                      // EXACT, from com_allow.h
    p->kind      = IsEqualGUID(iid, k_IClassFactory) ? KIND_CLASSFACTORY : KIND_UNKNOWN;
    p->id        = g_nproxies;
    p->typed     = true;
    p->iid       = iid;
    describe_vtable(p->real_vtbl, p);

    // Size the stubs and fake vtable to a generous cap, not just `slots`. The counts
    // in com_allow.h are hand-maintained (see the IFantasyEarthCom override, a typelib
    // that under-reported), and if one is too LOW the host calls a real method at
    // slot >= slots. Against an exact-sized fake vtable that read runs off the end
    // (a zero entry -> jump to NULL -> crash); with the cap every reachable slot has a
    // valid forwarding stub, so an under-count degrades to "forwarded + logged" rather
    // than a crash. 64 clears the largest real interface here (IPOLCoreCom, 32) with
    // wide headroom. p->nslots stays the reported count so a call beyond it is flagged
    // (see polshim_enter's tripwire).
    const int cap = slots < 64 ? 64 : slots;
    void* code = emit_stubs(p, cap);
    if (!code) { VirtualFree(p, 0, MEM_RELEASE); LeaveCriticalSection(&g_lock); return iface; }
    void** fake = (void**)VirtualAlloc(NULL, sizeof(void*) * cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!fake) { VirtualFree(p, 0, MEM_RELEASE); LeaveCriticalSection(&g_lock); return iface; }
    for (int i = 0; i < cap; i++) fake[i] = (BYTE*)code + i * 10;
    p->fake_vtbl = fake;
    p->stubs     = code;
    g_proxies[g_nproxies++] = p;
    LeaveCriticalSection(&g_lock);

    char iidbuf[64];
    logf("[typed] #%d %s+0x%06X slots=%d iid=%s real=%p",
         p->id, p->module, p->vtbl_rva, slots, iid_name(iid, iidbuf, sizeof(iidbuf)), iface);
    return p;
}

// --- the hooks --------------------------------------------------------------

extern "C" void* __fastcall polshim_enter(SlotCtx* ctx, void** frame)
{
    // frame[0] = caller's return address, frame[1] = `this`, frame[2..] = args
    Proxy* p = ctx->proxy;
    LONG ncall = InterlockedIncrement(&p->calls[ctx->slot]);
    // Under-count tripwire: com_allow.h reported fewer slots than the host actually
    // calls for this typed interface. The cap-sized fake vtable forwards it safely,
    // but flag it once per slot so the stale count gets corrected instead of masked.
    if (p->typed && ctx->slot >= p->nslots && ncall == 1)
        logf("[typed] #%d %s+0x%06X slot %d called, but com_allow.h lists only %d slots "
             "-- forwarding; update the slot count for this iid",
             p->id, p->module, p->vtbl_rva, ctx->slot, p->nslots);

    ThreadState* ts = tls_get();
    Frame* f = NULL;
    if (ts && ts->depth < POLSHIM_MAX_DEPTH) {
        f = &ts->frames[ts->depth++];
        f->retaddr   = frame[0];
        f->proxy     = p;
        f->slot      = ctx->slot;
        f->out_ppv   = NULL;
        f->wrap_kind = KIND_UNKNOWN;
        memset(&f->req_iid, 0, sizeof(GUID));
    }

    if (g_verbose) {
        char iidbuf[64];
        if (ctx->slot == 0) {
            // QueryInterface(this, REFIID, void**) -- the IID is the payload
            // we care about most; it names an interface we've never seen.
            const GUID* iid = (const GUID*)frame[2];
            logf("+ t%04X d%d #%d slot=0 QueryInterface iid=%s",
                 GetCurrentThreadId(), ts ? ts->depth : 0, p->id,
                 iid ? iid_name(*iid, iidbuf, sizeof(iidbuf)) : "(null)");
        } else if (ctx->slot == 1 || ctx->slot == 2) {
            if (g_verbose > 1)
                logf("+ t%04X d%d #%d slot=%d %s",
                     GetCurrentThreadId(), ts ? ts->depth : 0, p->id, ctx->slot,
                     ctx->slot == 1 ? "AddRef" : "Release");
        } else {
            logf("+ t%04X d%d #%d %s+0x%06X slot=%3d args=%08X %08X %08X %08X",
                 GetCurrentThreadId(), ts ? ts->depth : 0, p->id,
                 p->module, p->vtbl_rva, ctx->slot,
                 (DWORD)(UINT_PTR)frame[2], (DWORD)(UINT_PTR)frame[3],
                 (DWORD)(UINT_PTR)frame[4], (DWORD)(UINT_PTR)frame[5]);
        }
    }

    // Remember out-params we intend to wrap on the way back out.
    if (f) {
        if (ctx->slot == 0) {                              // IUnknown::QueryInterface(riid, ppv)
            f->out_ppv   = (void**)frame[3];
            f->wrap_kind = KIND_UNKNOWN;
            if (p->typed && frame[2]) f->req_iid = *(const GUID*)frame[2];
        } else if (p->kind == KIND_CLASSFACTORY && ctx->slot == 3) {
            // CreateInstance(pUnkOuter, riid, ppv) -> frame[2], frame[3], frame[4].
            f->out_ppv   = (void**)frame[4];
            f->wrap_kind = KIND_UNKNOWN;
            if (p->typed && frame[3]) f->req_iid = *(const GUID*)frame[3];
        }
    }

    // The real method always needs the genuine `this`.
    frame[1] = p->real;
    // Hijack the return ONLY if we actually pushed a shadow frame. If tls_get()
    // failed (out of memory) or the shadow stack is at POLSHIM_MAX_DEPTH, f is NULL
    // and there is no frame for polshim_leave to pop -- routing the return through
    // polshim_onreturn anyway made leave return NULL (jump to 0 -> crash) or pop an
    // outer call's frame (stack desync). Leaving frame[0] as the caller's real return
    // address makes the real method return straight to the caller: we lose out-param
    // wrapping and the leave-side log for this one call, but never corrupt the stack.
    if (f)
        frame[0] = (void*)polshim_onreturn;
    return p->real_vtbl[ctx->slot];
}

extern "C" void* __fastcall polshim_leave(DWORD eax, DWORD edx)
{
    ThreadState* ts = tls_get();
    if (!ts || ts->depth <= 0) return NULL;                // cannot happen; fail loud-ish
    Frame* f = &ts->frames[--ts->depth];

    if (f->out_ppv && eax == 0 /*S_OK*/) {
        void* inner = *f->out_ppv;
        if (inner) {
            // Typed: wrap the QI/CreateInstance result only if its IID is
            // allow-listed (typed_wrap passes it through otherwise) -- so the
            // recursive wrap never reaches system COM. Legacy: wrap everything.
            void* wrapped = f->proxy->typed ? typed_wrap(inner, f->req_iid)
                                            : proxy_wrap(inner, f->wrap_kind, "out-param");
            *f->out_ppv = wrapped;
        }
    }

    if (g_verbose && f->slot != 1 && f->slot != 2)
        logf("- t%04X d%d #%d slot=%3d ret=%08X:%08X",
             GetCurrentThreadId(), ts->depth, f->proxy->id, f->slot, eax, edx);

    // Typed lifetime: a Release that dropped the real object's refcount to 0
    // retires the proxy -- so the summary and identity cache never touch a dead
    // object (the stale real_vtbl the generic engine crashed on). Done LAST:
    // typed_retire frees f->proxy, so nothing below may use it.
    if (f->proxy->typed && f->slot == 2 /*Release*/ && eax == 0)
        typed_retire(f->proxy);

    return f->retaddr;
}

// --- naked trampolines ------------------------------------------------------
//
// Called through globals rather than by symbol so decoration can't bite us.

static void* (__fastcall *g_enter)(SlotCtx*, void**) = polshim_enter;
static void* (__fastcall *g_leave)(DWORD, DWORD)     = polshim_leave;

__declspec(naked) void __stdcall polshim_dispatch()
{
    __asm {
        pop  ecx                  // ecx = SlotCtx*, pushed by the stub
        pushad                    // [esp+32] = saved EAX
        pushfd
        lea  edx, [esp + 36]      // edx = &frame[0] (caller's return address)
        call [g_enter]            // -> real method address
        mov  [esp + 32], eax      // smuggle it out through the saved-EAX slot
        popfd
        popad                     // eax = real method
        jmp  eax                  // frame untouched; callee cleans its own args
    }
}

__declspec(naked) void __stdcall polshim_onreturn()
{
    __asm {
        sub  esp, 4               // slot for the caller's real return address
        pushad
        pushfd
        mov  ecx, [esp + 32]      // saved EAX = return value low
        mov  edx, [esp + 24]      // saved EDX = return value high
        call [g_leave]            // -> caller's original return address
        mov  [esp + 36], eax
        popfd
        popad                     // EAX/EDX restored: the callee's return value
        ret                       // jump to the real caller, ESP now correct
    }
}

// --- summary ----------------------------------------------------------------

// Every line goes out through emit() rather than fprintf. This runs from log_close()
// inside DllMain(DLL_PROCESS_DETACH), where a plain fprintf would take the CRT's
// per-FILE lock -- the one a worker thread ExitProcess killed mid-write may still
// "own", in which case this banner is the last thing pol.exe ever tries to do and it
// hangs there forever. See log_begin_teardown() in log.cpp.
static void emit(FILE* f, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (log_teardown_active()) log_write_raw(buf);
    else                       fputs(buf, f);
}

void proxy_summary(FILE* f)
{
    emit(f, "\n==== polshim summary: %d interfaces ====\n", g_nproxies);
    for (int i = 0; i < g_nproxies; i++) {
        Proxy* p = g_proxies[i];
        int used = 0;
        for (int s = 0; s < p->nslots; s++) if (p->calls[s]) used++;
        emit(f, "\n#%d  %s+0x%06X  slots=%d  exercised=%d  kind=%d\n",
             p->id, p->module, p->vtbl_rva, p->nslots, used, p->kind);
        for (int s = 0; s < p->nslots; s++) {
            if (!p->calls[s]) continue;
            emit(f, "    slot %3d  %8ld calls   target=%p\n",
                 s, p->calls[s], p->real_vtbl[s]);
        }
    }
}
