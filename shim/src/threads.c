/* kernel32.dll: threads, events, critical sections and waits.
 *
 * Every guest thread is a host thread with its own guest context, guest stack
 * and thread information block. The generated code only shares guest memory
 * between them, as the game's x86 threads did. Thread and event objects live
 * as long as the process (a handle closed while its thread still runs keeps it
 * valid for the thread). */
#include "shim.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu

#define GUEST_THREAD_STACK (1024u * 1024u)
/* Host stack of a guest thread: the generated code nests one C frame per guest
 * call. The Vita has little memory outside the newlib heap for thread stacks. */
#if defined(__vita__)
#define HOST_THREAD_STACK (1024u * 1024u)
#else
#define HOST_THREAD_STACK (4u * 1024u * 1024u)
#endif

/* A waitable object: a thread (signaled when it ends) or an event. */
typedef struct Waitable {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int signaled;
    int manual_reset;
} Waitable;

typedef struct GuestThread {
    Waitable w;              /* first: the handle's ptr is used as a Waitable */
    Ctx ctx;
    uint32_t proc, param;
    uint32_t stack;
    uint32_t exit_code;
    uint32_t id;
} GuestThread;

static void waitable_init(Waitable *w, int manual, int signaled)
{
    pthread_mutex_init(&w->m, NULL);
    pthread_cond_init(&w->cv, NULL);
    w->manual_reset = manual;
    w->signaled = signaled;
}

static void waitable_set(Waitable *w)
{
    pthread_mutex_lock(&w->m);
    w->signaled = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->m);
}

/* Returns WAIT_OBJECT_0 or WAIT_TIMEOUT. */
static uint32_t waitable_wait(Waitable *w, uint32_t ms)
{
    struct timespec until;
    if (ms != INFINITE) {
        struct timeval now;
        gettimeofday(&now, NULL);
        uint64_t ns = (uint64_t)now.tv_usec * 1000ull + (uint64_t)ms * 1000000ull;
        until.tv_sec = now.tv_sec + (time_t)(ns / 1000000000ull);
        until.tv_nsec = (long)(ns % 1000000000ull);
    }
    pthread_mutex_lock(&w->m);
    while (!w->signaled) {
        if (ms == 0) {
            pthread_mutex_unlock(&w->m);
            return WAIT_TIMEOUT;
        }
        int r = ms == INFINITE ? pthread_cond_wait(&w->cv, &w->m) : pthread_cond_timedwait(&w->cv, &w->m, &until);
        if (r == ETIMEDOUT && !w->signaled) {
            pthread_mutex_unlock(&w->m);
            return WAIT_TIMEOUT;
        }
    }
    if (!w->manual_reset)
        w->signaled = 0;
    pthread_mutex_unlock(&w->m);
    return WAIT_OBJECT_0;
}

/* ---- threads ------------------------------------------------------------------- */

static uint32_t next_thread_id = 0x200u;
static __thread uint32_t current_thread_id = 0x100u;   /* the main thread's */

uint32_t thread_setup_tib(Ctx *c, uint32_t stack_lo, uint32_t stack_hi);

static void *thread_main(void *arg)
{
    GuestThread *t = arg;
    current_thread_id = t->id;
    uint32_t args[1] = { t->param };
    rt_log("thread %X: start %08X(%08X)", t->id, t->proc, t->param);
    t->exit_code = guest_call(&t->ctx, t->proc, 1, args);
    rt_log("thread %X: exit %u", t->id, t->exit_code);
    waitable_set(&t->w);
    return NULL;
}

/* CreateThread(attributes, stack size, proc, param, flags, &id) */
SHIM(kernel32, CreateThread)
{
    uint32_t proc = ARG(2), param = ARG(3), flags = ARG(4), idp = ARG(5);
    if (flags & 4u)
        rt_log("CreateThread: CREATE_SUSPENDED is not supported (starting the thread)");
    GuestThread *t = calloc(1, sizeof(*t));
    waitable_init(&t->w, 1, 0);
    t->proc = proc;
    t->param = param;
    t->id = __atomic_fetch_add(&next_thread_id, 4u, __ATOMIC_RELAXED);
    t->stack = heap_alloc(GUEST_THREAD_STACK);
    if (!t->stack)
        rt_fatal(c, "CreateThread: no memory for the guest stack");
    rt_fninit(&t->ctx);
#ifdef COD_PROFILE
    t->ctx.prof = calloc(1, sizeof(RtProf));   /* not sampled: the profile is the main thread's */
#endif
    t->ctx.fcw = c->fcw;
    t->ctx.r[4] = t->stack + GUEST_THREAD_STACK - 16u;
    thread_setup_tib(&t->ctx, t->stack, t->stack + GUEST_THREAD_STACK);

    uint32_t h = handle_new(H_THREAD);
    handle_get(h, H_THREAD)->ptr = t;
    if (idp)
        WR32(idp, t->id);

    pthread_t pt;
    int err = -1;
    for (uint32_t stack = HOST_THREAD_STACK; err && stack >= 256u * 1024u; stack /= 2u) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stack);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        err = pthread_create(&pt, &attr, thread_main, t);
        pthread_attr_destroy(&attr);
        if (err) {
            rt_log("CreateThread: pthread_create with a %u KiB stack failed (%d)", stack >> 10, err);
            mem_report("thread creation failed");
        }
    }
    if (err)
        rt_fatal(c, "CreateThread: no host thread for %08X", proc);
    stdcall_ret(c, h, 6);
}

SHIM(kernel32, GetCurrentThread) { stdcall_ret(c, 0xFFFFFFFEu, 0); }
SHIM(kernel32, GetCurrentThreadId) { stdcall_ret(c, current_thread_id, 0); }
SHIM(kernel32, SetThreadPriority) { stdcall_ret(c, 1, 2); }
SHIM(kernel32, SetThreadPriorityBoost) { stdcall_ret(c, 1, 2); }

/* ---- events ------------------------------------------------------------------- */

/* CreateEventA(attributes, manual reset, initial state, name) */
SHIM(kernel32, CreateEventA)
{
    Waitable *w = calloc(1, sizeof(*w));
    waitable_init(w, ARG(1) != 0, ARG(2) != 0);
    uint32_t h = handle_new(H_EVENT);
    handle_get(h, H_EVENT)->ptr = w;
    stdcall_ret(c, h, 4);
}

SHIM(kernel32, SetEvent)
{
    HandleObj *o = handle_get(ARG(0), H_EVENT);
    if (o)
        waitable_set(o->ptr);
    stdcall_ret(c, o != NULL, 1);
}

SHIM(kernel32, ResetEvent)
{
    HandleObj *o = handle_get(ARG(0), H_EVENT);
    if (o) {
        Waitable *w = o->ptr;
        pthread_mutex_lock(&w->m);
        w->signaled = 0;
        pthread_mutex_unlock(&w->m);
    }
    stdcall_ret(c, o != NULL, 1);
}

SHIM(kernel32, WaitForSingleObject)
{
    uint32_t h = ARG(0), ms = ARG(1);
    HandleObj *o = handle_get(h, H_FREE);
    uint32_t r = WAIT_OBJECT_0;
    if (o && (o->type == H_EVENT || o->type == H_THREAD))
        r = waitable_wait(o->ptr, ms);
    else if (!o)
        r = WAIT_FAILED;
    stdcall_ret(c, r, 2);
}

/* ---- critical sections -----------------------------------------------------------
 * The guest CRITICAL_SECTION (24 bytes) holds the index of a host recursive mutex
 * in its LockSemaphore field (offset 16), plus the magic below in SpinCount. */

#define CS_MAGIC 0xC5EC7104u
#define MAX_CS 1024u

static pthread_mutex_t cs_mutex[MAX_CS];
static uint32_t cs_count;
static pthread_mutex_t cs_table_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t cs_create_locked(void)
{
    if (cs_count == MAX_CS)
        rt_fatal(NULL, "too many critical sections");
    uint32_t i = cs_count++;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs_mutex[i], &a);
    pthread_mutexattr_destroy(&a);
    return i;
}

static uint32_t cs_create(void)
{
    pthread_mutex_lock(&cs_table_lock);
    uint32_t i = cs_create_locked();
    pthread_mutex_unlock(&cs_table_lock);
    return i;
}

static pthread_mutex_t *cs_of(uint32_t cs)
{
    if (__atomic_load_n((uint32_t *)(g_mem + cs + 20u), __ATOMIC_ACQUIRE) != CS_MAGIC) {
        /* Not initialized through InitializeCriticalSection (zeroed static). */
        pthread_mutex_lock(&cs_table_lock);
        if (RD32(cs + 20u) != CS_MAGIC) {
            WR32(cs + 16u, cs_create_locked());
            __atomic_store_n((uint32_t *)(g_mem + cs + 20u), CS_MAGIC, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&cs_table_lock);
    }
    return &cs_mutex[RD32(cs + 16u) % MAX_CS];
}

SHIM(kernel32, InitializeCriticalSection)
{
    uint32_t cs = ARG(0);
    memset(g_mem + cs, 0, 24);
    WR32(cs + 4u, 0xFFFFFFFFu);      /* LockCount */
    WR32(cs + 16u, cs_create());
    WR32(cs + 20u, CS_MAGIC);
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, DeleteCriticalSection) { stdcall_ret(c, 0, 1); }

SHIM(kernel32, EnterCriticalSection)
{
    pthread_mutex_lock(cs_of(ARG(0)));
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, LeaveCriticalSection)
{
    pthread_mutex_unlock(cs_of(ARG(0)));
    stdcall_ret(c, 0, 1);
}
