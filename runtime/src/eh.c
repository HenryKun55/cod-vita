/* MSVC C++ exception handling for the recompiled code.
 *
 * The game's functions keep MSVC's frame records on the guest stack (the chain
 * at fs:[0]): next record, handler (a stub `mov eax, FuncInfo; jmp
 * __CxxFrameHandler`, listed in g_eh_stubs) and the try state; the frame's ebp
 * is the record address + 12. A throw walks that chain like __CxxFrameHandler:
 * the first try block around the current state with a catch for the thrown
 * type wins. The frames above it and the target frame down to the try block are
 * unwound (destructor funclets), the catch funclet runs and returns the
 * continuation address, and the host jumps back into the function with the try
 * block (its wrapper in the generated code), which goes on there.
 *
 * Structures (MSVC 6, FuncInfo magic 0x19930520):
 *   FuncInfo      { magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap, ... }
 *   UnwindMap     { int toState; action }
 *   TryBlockMap   { tryLow, tryHigh, catchHigh, nCatches, pHandlerArray }
 *   HandlerType   { adjectives, pType, dispCatchObj, addressOfHandler }
 *   ThrowInfo     { attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray }
 *   CatchableType { properties, pType, PMD{mdisp, pdisp, vdisp}, size, copyFunction }
 *   TypeDescriptor{ vftable, spare, name[] } */
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>

__thread RtEhFrame *rt_eh_top;

static uint32_t stub_funcinfo(uint32_t handler)
{
    uint32_t lo = 0, hi = g_eh_stub_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (g_eh_stubs[mid].stub == handler)
            return g_eh_stubs[mid].funcinfo;
        if (g_eh_stubs[mid].stub < handler)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return 0;
}

static const char *type_name(uint32_t td)
{
    return td ? (const char *)g_mem + td + 8u : "...";
}

/* The CatchableType of the thrown object that a handler of type `htype` takes,
 * or 0. htype 0 is catch(...), which takes anything (returned as 1). */
static uint32_t match_type(uint32_t htype, uint32_t throw_info)
{
    if (!htype)
        return 1;
    uint32_t cta = RD32(throw_info + 12u);
    uint32_t n = RD32(cta);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ct = RD32(cta + 4u + 4u * i);
        uint32_t td = RD32(ct + 4u);
        if (td == htype || strcmp(type_name(td), type_name(htype)) == 0)
            return ct;
    }
    return 0;
}

/* Calls a funclet (unwind action or catch block) in the frame of `ebp`. */
static uint32_t call_funclet(Ctx *c, uint32_t fn, uint32_t ebp)
{
    uint32_t saved = c->r[5];
    c->r[5] = ebp;
    uint32_t r = guest_call(c, fn, 0, NULL);
    c->r[5] = saved;
    return r;
}

/* Runs the unwind actions of a frame from its current state down to `target`. */
static void unwind_frame(Ctx *c, uint32_t rec, uint32_t fi, int32_t target)
{
    uint32_t ebp = rec + 12u;
    uint32_t map = RD32(fi + 8u), max = RD32(fi + 4u);
    int32_t state = (int32_t)RD32(rec + 8u);
    while (state != target && state >= 0 && (uint32_t)state < max) {
        int32_t next = (int32_t)RD32(map + 8u * (uint32_t)state);
        uint32_t action = RD32(map + 8u * (uint32_t)state + 4u);
        WR32(rec + 8u, (uint32_t)next);
        if (action)
            call_funclet(c, action, ebp);
        state = next;
    }
}

/* Thiscall of fn with `this` and one stack argument (copy constructor). */
static void call_thiscall1(Ctx *c, uint32_t fn, uint32_t self, uint32_t arg)
{
    uint32_t saved = c->r[1];
    c->r[1] = self;
    uint32_t args[1] = { arg };
    guest_call(c, fn, 1, args);
    c->r[1] = saved;
}

static void call_thiscall0(Ctx *c, uint32_t fn, uint32_t self)
{
    uint32_t saved = c->r[1];
    c->r[1] = self;
    guest_call(c, fn, 0, NULL);
    c->r[1] = saved;
}

/* Initializes the catch's parameter from the thrown object. */
static void build_catch_object(Ctx *c, uint32_t handler, uint32_t ct, uint32_t obj, uint32_t ebp)
{
    uint32_t adjectives = RD32(handler), disp = RD32(handler + 8u);
    if (!disp || ct <= 1u || !RD32(handler + 4u))
        return;
    uint32_t dst = ebp + disp;
    uint32_t props = RD32(ct), mdisp = RD32(ct + 8u), size = RD32(ct + 20u), copy = RD32(ct + 24u);
    uint32_t src = obj + mdisp;
    if (adjectives & 8u)                       /* catch by reference */
        WR32(dst, src);
    else if ((props & 1u) || !copy)            /* simple type or no copy constructor */
        memmove(g_mem + dst, g_mem + src, size);
    else
        call_thiscall1(c, copy, dst, src);
}

uint64_t rt_eh_continue(Ctx *c, RtEhFrame *fr)
{
    uint32_t cont = fr->cont;
    uint32_t mask = g_guest_func_hash_mask;
    uint32_t i = GUEST_FUNC_HASH(cont) & mask;
    for (;;) {
        const GuestFunc *e = &g_guest_func_hash[i];
        if (e->addr == cont && e->fast)
            return e->fast(c, c->r[4], c->r[1], c->r[0]);
        if (!e->addr)
            break;
        i = (i + 1u) & mask;
    }
    rt_fatal(c, "C++ exception: no entry for the catch continuation %08X", cont);
}

void rt_eh_throw(Ctx *c, uint32_t obj, uint32_t info)
{
    const char *name = "?";
    uint32_t cta = info ? RD32(info + 12u) : 0;
    if (cta && RD32(cta))
        name = type_name(RD32(RD32(cta + 4u) + 4u));
    if (!info)
        rt_fatal(c, "C++ rethrow (throw;) is not supported");
    rt_log("C++ exception thrown: %s (return address %08X)", name, RD32(c->r[4]));

    uint32_t fs = c->fs_base;
    for (uint32_t rec = RD32(fs); rec != 0xFFFFFFFFu && rec; rec = RD32(rec)) {
        uint32_t fi = stub_funcinfo(RD32(rec + 4u));
        if (!fi)
            continue;
        int32_t state = (int32_t)RD32(rec + 8u);
        uint32_t ntb = RD32(fi + 12u), tbm = RD32(fi + 16u);
        for (uint32_t k = 0; k < ntb; k++) {
            uint32_t tb = tbm + 20u * k;
            int32_t lo = (int32_t)RD32(tb), hi = (int32_t)RD32(tb + 4u);
            if (state < lo || state > hi)
                continue;
            uint32_t nc = RD32(tb + 12u), ha = RD32(tb + 16u);
            for (uint32_t j = 0; j < nc; j++) {
                uint32_t h = ha + 16u * j;
                uint32_t ct = match_type(RD32(h + 4u), info);
                if (!ct)
                    continue;
                uint32_t ebp = rec + 12u;
                rt_log("  caught by the try block of the frame at ebp %08X (catch %s at %08X)", ebp,
                       type_name(RD32(h + 4u)), RD32(h + 12u));
                /* Unwind the frames above, then this one down to the try block. */
                for (uint32_t r = RD32(fs); r != rec; r = RD32(r)) {
                    uint32_t f = stub_funcinfo(RD32(r + 4u));
                    if (f)
                        unwind_frame(c, r, f, -1);
                }
                WR32(fs, rec);
                unwind_frame(c, rec, fi, lo - 1);
                build_catch_object(c, h, ct, obj, ebp);
                uint32_t cont = call_funclet(c, RD32(h + 12u), ebp);
                uint32_t dtor = RD32(info + 4u);
                if (dtor)
                    call_thiscall0(c, dtor, obj);
                /* Back into the frame: its esp as saved at the try ([ebp - 16]). */
                RtEhFrame *fr = rt_eh_top;
                while (fr && fr->entry_esp != ebp + 4u)
                    fr = fr->prev;
                if (!fr)
                    rt_fatal(c, "C++ exception: frame at ebp %08X has no host jump buffer", ebp);
                c->r[5] = ebp;
                c->r[4] = RD32(ebp - 16u);
                fr->cont = cont;
                longjmp(fr->jb, 1);
            }
        }
    }
    rt_fatal(c, "unhandled C++ exception: %s", name);
}
