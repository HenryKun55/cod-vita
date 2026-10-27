/* The renderer's back end on its own core (the Vita; COD_ASYNC_RENDER=0 turns it off,
 * =1 turns it on elsewhere, =2 keeps only the waits, for diagnostics).
 *
 * The renderer builds a frame's command list and draw lists in one buffer,
 * backEndData (a pointer at BACKEND_DATA), and RE_EndFrame ends the frame by
 * running the commands with RB_ExecuteRenderCommands (the CPU side of every draw:
 * tessellation, skinning and the GL calls). Quake III could run that on a second
 * thread with two such buffers (r_smp); Call of Duty kept one. Here the end of a
 * frame hands its commands to a render thread and points backEndData at a second
 * buffer, so the next frame's game logic (the server frame: AI, physics,
 * collision) runs while the render thread draws this one. Every access to
 * backEndData goes through the pointer, which only R_Init writes, and the buffer
 * holds no pointers into itself (its polygons are addressed from its start).
 *
 * The back end also reads renderer globals the front end rewrites for every scene
 * (tr.refdef's pointers into backEndData, among others: Call of Duty's back end
 * does not keep to its own copies as Quake III's did for r_smp), so the front end
 * must not run while it draws: every renderer entry point (the refexport table's
 * functions, RENDER_WAIT in tools/recomp/gen.py) first waits for the render
 * thread. The other callers of RB_ExecuteRenderCommands (before a texture upload,
 * a video frame or a screenshot) need the commands run at once: they wait too and
 * run them on the game thread. GL from the game thread waits as well (the GL
 * thunks are gated), so vitaGL is never used by both threads at once.
 *
 * The threads hand frames over with semaphores: a condition variable broadcast
 * was sometimes lost on the Vita (the game thread slept until a timeout). */
#include "shim.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

#define BACKEND_DATA 0x01549698u          /* backEndData_t * */
#define BACKEND_DATA_SIZE 0x0028D704u     /* without the polygons */
#define BACKEND_CMDS_USED 0x0028D700u     /* commands.used */
#define MAX_POLYS 0x01534504u
#define MAX_POLYVERTS 0x015344F8u
#define EXECUTE_COMMANDS 0x004CEA70u      /* RB_ExecuteRenderCommands(eax = list) */
#define END_FRAME_RETURN 0x004D4AC1u      /* RE_EndFrame, after its call */
#define RENDER_STACK (1024u * 1024u)

uint32_t thread_setup_tib(Ctx *c, uint32_t stack_lo, uint32_t stack_hi);

static int enabled = -1;
static SDL_sem *job_sem, *done_sem;
static int busy;                 /* the render thread draws (diagnostics: render_async_foreign) */
static int outstanding;          /* a frame was handed over and its end not waited for (game thread only) */
static uint32_t job_cmds;
static Ctx rctx;
static uintptr_t render_tag;     /* the render thread's rt_thread_tag (0 until it runs) */
static uint32_t buf_a, buf_b, buf_size;
static uint64_t frames, frame_waits, call_waits, wait_us, draw_us;
#ifdef COD_PROFILE
RtProf rt_prof_render_thread;
#endif

static uint64_t now_us(void) { return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency(); }

static int on_render_thread(void)
{
    uintptr_t t = __atomic_load_n(&render_tag, __ATOMIC_ACQUIRE);
    return t && t == rt_thread_tag();
}

static int render_main(void *arg)
{
    (void)arg;
    __atomic_store_n(&render_tag, rt_thread_tag(), __ATOMIC_RELEASE);
#if defined(__vita__)
    /* The game thread runs on the first core; this one takes the second. */
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
#endif
    for (;;) {
        SDL_SemWait(job_sem);
        uint64_t t0 = now_us();
        rctx.r[0] = job_cmds;
        guest_call(&rctx, EXECUTE_COMMANDS, 0, NULL);
        draw_us += now_us() - t0;
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        SDL_SemPost(done_sem);
    }
    return 0;
}

/* Until the render thread has drawn the frame handed over last (a no-op on the
 * render thread and when nothing is outstanding: one load). */
static void render_wait(uint64_t *count)
{
    if (!outstanding || on_render_thread())
        return;
    uint64_t w0 = now_us();
    if (__atomic_load_n(&busy, __ATOMIC_ACQUIRE) && count)
        (*count)++;
    SDL_SemWait(done_sem);
    outstanding = 0;
    wait_us += now_us() - w0;
}

static void render_gate(void)
{
    render_wait(&call_waits);
}

static int start(Ctx *c)
{
    job_sem = SDL_CreateSemaphore(0);
    done_sem = SDL_CreateSemaphore(0);
    uint32_t stack = heap_alloc(RENDER_STACK);
    if (!job_sem || !done_sem || !stack)
        return 0;
    rt_fninit(&rctx);
    rctx.fcw = c->fcw;
    rctx.r[4] = stack + RENDER_STACK - 16u;
    thread_setup_tib(&rctx, stack, stack + RENDER_STACK);
#ifdef COD_PROFILE
    rctx.prof = &rt_prof_render_thread;
#if defined(__vita__)
    {   /* vitaGL's draw phases are the render thread's now */
        extern volatile uint32_t *vgl_prof_slot __attribute__((weak));
        if (&vgl_prof_slot)
            vgl_prof_slot = &rt_prof_render_thread.cur;
    }
#endif
#endif
    SDL_Thread *t = SDL_CreateThreadWithStackSize(render_main, "cod-render", 1024u * 1024u, NULL);
    if (!t) {
        rt_log("render thread: not started (%s)", SDL_GetError());
        return 0;
    }
    SDL_DetachThread(t);
    rt_thunk_gate = render_gate;
    rt_log("render thread: started");
    return 1;
}

/* The buffer the next frame is built in: the other one of the pair, the second
 * made (as a copy of the first) when the renderer allocates a new first. */
static uint32_t other_buffer(uint32_t cur)
{
    if (cur != buf_a && cur != buf_b) {
        uint32_t size = BACKEND_DATA_SIZE + (RD32(MAX_POLYS) + RD32(MAX_POLYVERTS) * 2u) * 16u;
        if (size != buf_size || !buf_b) {
            if (buf_b && buf_b != cur)
                heap_free(buf_b);
            buf_b = heap_alloc(size);
            buf_size = buf_b ? size : 0;
            rt_log("render thread: second frame buffer %08X (%u KB)", buf_b, size >> 10);
        }
        buf_a = cur;
        if (buf_b)
            memcpy(g_mem + buf_b, g_mem + buf_a, buf_size);
    }
    return cur == buf_a ? buf_b : buf_a;
}

/* Entry hook of every renderer entry point (RENDER_WAIT in tools/recomp/gen.py). */
#ifdef COD_PROFILE
/* Which entry points wait (the function's id is the profiler's current code). */
static struct { uint32_t fn; uint32_t n; } waiters[32];
#endif

int cod_render_wait_entry(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)c, (void)esp, (void)ecx, (void)eax, (void)ret;
#ifdef COD_PROFILE
    if (outstanding && !on_render_thread()) {
        uint32_t fn = c->prof->cur, i;
        for (i = 0; i < 32u && waiters[i].fn && waiters[i].fn != fn; i++)
            ;
        if (i < 32u) {
            waiters[i].fn = fn;
            waiters[i].n++;
        }
    }
#endif
    render_wait(&call_waits);
    return 0;
}

/* Entry hook of RB_ExecuteRenderCommands. */
int cod_render_execute(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)ecx;
    if (on_render_thread())
        return 0;
    if (enabled < 0) {
        const char *e = getenv("COD_ASYNC_RENDER");
#if defined(__vita__)
        enabled = !(e && e[0] == '0');
#else
        enabled = e && e[0] == '1';
#endif
        if (enabled && e && e[0] == '2')
            enabled = start(c) ? 2 : 0;
        else if (enabled)
            enabled = start(c);
    }
    int end_frame = RD32(esp) == END_FRAME_RETURN;
    render_wait(end_frame ? &frame_waits : &call_waits);
    if (enabled != 1 || !end_frame)
        return 0;   /* a sync point (or diagnostics): run the commands now, here */
    uint32_t next = other_buffer(RD32(BACKEND_DATA));
    if (!next)
        return 0;
    WR32(BACKEND_DATA, next);
    WR32(next + BACKEND_CMDS_USED, 0);
    if (++frames % 600u == 0) {
        rt_log("render thread: %llu frames; per frame the game thread waited %.1f ms, drawing took %.1f ms "
               "(waits at frame ends %llu, at renderer calls and GL %llu)",
               (unsigned long long)frames, wait_us / 600000.0, draw_us / 600000.0,
               (unsigned long long)frame_waits, (unsigned long long)call_waits);
        wait_us = draw_us = 0;
#ifdef COD_PROFILE
        char line[400];
        size_t n = 0;
        for (uint32_t i = 0; i < 32u && waiters[i].fn && n < sizeof(line) - 24; i++)
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %08X:%u", waiters[i].fn, waiters[i].n);
        line[n] = 0;
        rt_log("render thread: entry points that waited:%s", line);
        memset(waiters, 0, sizeof(waiters));
#endif
    }
    job_cmds = eax;
    outstanding = 1;
    __atomic_store_n(&busy, 1, __ATOMIC_RELEASE);
    SDL_SemPost(job_sem);
    *ret = ((uint64_t)(esp + 4u) << 32) | eax;
    return 1;
}

/* The renderer's font lookup (ri callback 0040E430: VM_Call(uivm, 13, name, size *
 * scale), the UI module registers the fonts): text drawn by the back end looks its
 * font up there, and VM_Call switches the global current VM, which the game
 * thread's own module calls read at the same time. The answers are cached per UI
 * module, font and size: the game thread fills the cache (it asks first, laying
 * the text out), the render thread reads it. */
#define FONT_CACHE 64u
#define UI_VM 0x014BD648u
static struct {
    uint32_t vm, name, size, font;
} font_cache[FONT_CACHE];
static uint32_t font_n;
static int font_lookup_running;

int cod_render_font(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)ecx, (void)eax;
    if (font_lookup_running)
        return 0;   /* the call below: the guest code */
    uint32_t vm = RD32(UI_VM), name = RD32(esp + 4u), size = RD32(esp + 8u);
    uint32_t n = __atomic_load_n(&font_n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++)
        if (font_cache[i].vm == vm && font_cache[i].name == name && font_cache[i].size == size) {
            *ret = ((uint64_t)(esp + 4u) << 32) | font_cache[i].font;
            return 1;
        }
    if (on_render_thread()) {
        static unsigned misses;
        if (misses++ < 8u)
            rt_log("render thread: font %08X size %08X not looked up by the game thread first", name, size);
        return 0;
    }
    font_lookup_running = 1;
    c->r[4] = esp;
    uint32_t args[2] = { name, size };
    uint32_t font = guest_call(c, 0x0040E430u, 2, args);
    font_lookup_running = 0;
    if (n == FONT_CACHE || (n && font_cache[0].vm != vm))
        n = 0;   /* full, or a new UI module: start over */
    font_cache[n].vm = vm;
    font_cache[n].name = name;
    font_cache[n].size = size;
    font_cache[n].font = font;
    __atomic_store_n(&font_n, n + 1u, __ATOMIC_RELEASE);
    *ret = ((uint64_t)(esp + 4u) << 32) | font;
    return 1;
}

/* 1 while the render thread draws and the caller is another thread (diagnostics). */
int render_async_foreign(void)
{
    return __atomic_load_n(&busy, __ATOMIC_ACQUIRE) && !on_render_thread();
}

int render_async_busy(void)
{
    return __atomic_load_n(&busy, __ATOMIC_ACQUIRE);
}

/* Before the process ends. */
void render_async_sync(void)
{
    render_wait(NULL);
}
