/* The Win32 calls Call of Duty and its statically linked C runtime make beyond
 * the common set in kernel32.c / user32.c: TLS, locale probing, system and memory
 * information, the clipboard, joystick and MIDI (none here), the DirectDraw
 * video-memory probe of the auto-configuration, and a few GDI/shell calls. */
#include "shim.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_CALL_NOT_IMPLEMENTED 120u

/* ---- thread-local storage ----------------------------------------------- */

#define TLS_SLOTS 64u
static uint8_t tls_used[TLS_SLOTS];

/* Each host thread's slots, found by rt_thread_tag (__thread is not per thread
 * on the Vita, and the render thread needs its own C runtime data). The game
 * thread is nearly always the first entry. */
#define TLS_THREADS 32u
static struct {
    uintptr_t tag;
    uint32_t v[TLS_SLOTS];
} tls_threads[TLS_THREADS];
static uint32_t tls_nthreads;
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t *tls_slots(void)
{
    uintptr_t t = rt_thread_tag();
    uint32_t n = __atomic_load_n(&tls_nthreads, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++)
        if (tls_threads[i].tag == t)
            return tls_threads[i].v;
    pthread_mutex_lock(&tls_lock);
    uint32_t i = tls_nthreads;
    if (i == TLS_THREADS)
        rt_fatal(NULL, "TLS: more than %u threads", TLS_THREADS);
    tls_threads[i].tag = t;
    __atomic_store_n(&tls_nthreads, i + 1u, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&tls_lock);
    return tls_threads[i].v;
}

SHIM(kernel32, TlsAlloc)
{
    for (uint32_t i = 0; i < TLS_SLOTS; i++)
        if (!tls_used[i]) {
            tls_used[i] = 1;
            stdcall_ret(c, i, 0);
            return;
        }
    stdcall_ret(c, 0xFFFFFFFFu, 0);
}

SHIM(kernel32, TlsFree)
{
    uint32_t i = ARG(0);
    if (i < TLS_SLOTS)
        tls_used[i] = 0;
    stdcall_ret(c, i < TLS_SLOTS, 1);
}

SHIM(kernel32, TlsGetValue)
{
    uint32_t i = ARG(0);
    win32_set_last_error(0);
    stdcall_ret(c, i < TLS_SLOTS ? tls_slots()[i] : 0u, 1);
}

SHIM(kernel32, TlsSetValue)
{
    uint32_t i = ARG(0);
    if (i < TLS_SLOTS)
        tls_slots()[i] = ARG(1);
    stdcall_ret(c, i < TLS_SLOTS, 2);
}

/* ---- process, system, memory -------------------------------------------- */

SHIM(kernel32, SetLastError)
{
    win32_set_last_error(ARG(0));
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, GetCurrentProcessId) { stdcall_ret(c, 0x1000u, 0); }
SHIM(kernel32, OpenProcess) { stdcall_ret(c, 0, 3); }
SHIM(kernel32, CreateProcessA) { win32_set_last_error(ERROR_CALL_NOT_IMPLEMENTED); stdcall_ret(c, 0, 10); }
SHIM(kernel32, CreateToolhelp32Snapshot) { stdcall_ret(c, 0xFFFFFFFFu, 2); }
SHIM(kernel32, Module32First) { stdcall_ret(c, 0, 2); }
SHIM(kernel32, Module32Next) { stdcall_ret(c, 0, 2); }
SHIM(kernel32, SetUnhandledExceptionFilter) { stdcall_ret(c, 0, 1); }
SHIM(kernel32, SetErrorMode) { stdcall_ret(c, 0, 1); }
SHIM(kernel32, SetConsoleCtrlHandler) { stdcall_ret(c, 1, 2); }
SHIM(kernel32, SetThreadExecutionState) { stdcall_ret(c, 0x80000000u, 1); }
SHIM(kernel32, IsBadCodePtr) { stdcall_ret(c, 0, 1); }
SHIM(kernel32, IsBadWritePtr) { stdcall_ret(c, 0, 2); }
SHIM(kernel32, SetEnvironmentVariableA) { stdcall_ret(c, 1, 2); }

SHIM(kernel32, FatalAppExitA)
{
    rt_fatal(c, "FatalAppExit: %s", ARG(1) ? gstr(ARG(1)) : "");
}

SHIM(kernel32, QueryPerformanceFrequency)
{
    /* QueryPerformanceCounter (kernel32.c) counts microseconds. */
    WR32(ARG(0), 1000000u);
    WR32(ARG(0) + 4u, 0);
    stdcall_ret(c, 1, 1);
}

/* MEMORYSTATUS: what the auto-configuration and com_hunkMegs checks see. */
SHIM(kernel32, GlobalMemoryStatus)
{
    uint32_t p = ARG(0);
    WR32(p + 0u, 32);
    WR32(p + 4u, 30);                 /* load % */
    WR32(p + 8u, 512u << 20);         /* total physical */
    WR32(p + 12u, 384u << 20);        /* available physical */
    WR32(p + 16u, 1024u << 20);       /* page file */
    WR32(p + 20u, 896u << 20);
    WR32(p + 24u, 0x7FFE0000u);       /* virtual */
    WR32(p + 28u, 0x70000000u);
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, GetSystemInfo)
{
    uint32_t p = ARG(0);
    memset(gptr(p), 0, 36);
    WR32(p + 4u, 4096);               /* page size */
    WR32(p + 8u, 0x00010000u);
    WR32(p + 12u, 0x7FFEFFFFu);
    WR32(p + 16u, 1);                 /* active processor mask */
    WR32(p + 20u, 1);                 /* processors */
    WR32(p + 24u, 586);               /* PROCESSOR_INTEL_PENTIUM */
    WR32(p + 28u, 0x00010000u);       /* allocation granularity */
    WR16(p + 32u, 6);                 /* level */
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, HeapSize) { stdcall_ret(c, heap_size(ARG(2)), 3); }

SHIM(kernel32, VirtualProtect)
{
    if (ARG(3))
        WR32(ARG(3), 0x04u);          /* PAGE_READWRITE */
    stdcall_ret(c, 1, 4);
}

/* GlobalAlloc memory is plain heap memory (GMEM_FIXED handles are pointers). */
SHIM(kernel32, GlobalLock) { stdcall_ret(c, ARG(0), 1); }
SHIM(kernel32, GlobalUnlock) { stdcall_ret(c, 1, 1); }
SHIM(kernel32, GlobalSize) { stdcall_ret(c, ARG(0) ? heap_size(ARG(0)) : 0u, 1); }

SHIM(kernel32, MulDiv)
{
    int32_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), d = (int32_t)ARG(2);
    int64_t p = (int64_t)a * b;
    int32_t r = d ? (int32_t)((p + (p >= 0 ? d / 2 : -d / 2)) / d) : -1;
    stdcall_ret(c, (uint32_t)r, 3);
}

/* ---- paths ---------------------------------------------------------------- */

static void put_str(uint32_t buf, uint32_t size, const char *s, uint32_t *len)
{
    uint32_t n = (uint32_t)strlen(s);
    *len = n;
    if (n + 1u > size)
        return;
    memcpy(gptr(buf), s, n + 1u);
}

SHIM(kernel32, GetTempPathA)
{
    uint32_t n;
    put_str(ARG(1), ARG(0), WIN_INSTALL_DIR "\\", &n);
    stdcall_ret(c, n, 2);
}

SHIM(kernel32, GetTempFileNameA)
{
    static uint32_t counter = 1;
    char out[300];
    const char *dir = gstr(ARG(0));
    const char *pre = ARG(1) ? gstr(ARG(1)) : "";
    uint32_t u = ARG(2) ? ARG(2) : counter++;
    snprintf(out, sizeof(out), "%s\\%.3s%04X.tmp", dir, pre, u & 0xFFFFu);
    memcpy(gptr(ARG(3)), out, strlen(out) + 1u);
    stdcall_ret(c, u, 4);
}

SHIM(kernel32, GetSystemDirectoryA)
{
    uint32_t n;
    put_str(ARG(0), ARG(1), "C:\\WINDOWS\\SYSTEM32", &n);
    stdcall_ret(c, n, 2);
}

SHIM(kernel32, GetFileAttributesA)
{
    char host[1100];
    win_path_to_host(gstr(ARG(0)), host, sizeof(host));
    struct stat st;
    uint32_t r;
    if (stat(host, &st) != 0) {
        win32_set_last_error(ERROR_FILE_NOT_FOUND);
        r = 0xFFFFFFFFu;
    } else {
        r = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
    }
    stdcall_ret(c, r, 1);
}

SHIM(kernel32, GetFullPathNameA)
{
    const char *name = gstr(ARG(0));
    uint32_t size = ARG(1), buf = ARG(2), part = ARG(3);
    char full[600];
    if (name[0] && name[1] == ':')
        snprintf(full, sizeof(full), "%s", name);
    else if (name[0] == '\\' || name[0] == '/')
        snprintf(full, sizeof(full), "C:%s", name);
    else
        snprintf(full, sizeof(full), WIN_INSTALL_DIR "\\%s", name[0] == '.' && (name[1] == '\\' || name[1] == '/') ? name + 2 : name);
    for (char *p = full; *p; p++)
        if (*p == '/')
            *p = '\\';
    uint32_t n = (uint32_t)strlen(full);
    if (n + 1u <= size) {
        memcpy(gptr(buf), full, n + 1u);
        if (part) {
            const char *slash = strrchr(full, '\\');
            WR32(part, buf + (uint32_t)(slash ? slash + 1 - full : 0));
        }
        stdcall_ret(c, n, 4);
    } else {
        stdcall_ret(c, n + 1u, 4);
    }
}

/* ---- locale ----------------------------------------------------------------
 * The C runtime tries the wide versions first and falls back to the ANSI ones
 * when they report ERROR_CALL_NOT_IMPLEMENTED (as on Windows 9x). */

SHIM(kernel32, CompareStringW) { win32_set_last_error(ERROR_CALL_NOT_IMPLEMENTED); stdcall_ret(c, 0, 6); }
SHIM(kernel32, GetLocaleInfoW) { win32_set_last_error(ERROR_CALL_NOT_IMPLEMENTED); stdcall_ret(c, 0, 4); }
SHIM(kernel32, SetEnvironmentVariableW) { win32_set_last_error(ERROR_CALL_NOT_IMPLEMENTED); stdcall_ret(c, 0, 2); }

SHIM(kernel32, CompareStringA)
{
    uint32_t flags = ARG(1), s1 = ARG(2), s2 = ARG(4);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    if (n1 < 0)
        n1 = (int32_t)strlen(gstr(s1));
    if (n2 < 0)
        n2 = (int32_t)strlen(gstr(s2));
    int r = 0;
    for (int32_t i = 0; !r && i < n1 && i < n2; i++) {
        int a = RD8(s1 + (uint32_t)i), b = RD8(s2 + (uint32_t)i);
        if (flags & 1u) {
            a = tolower(a);
            b = tolower(b);
        }
        r = a - b;
    }
    if (!r)
        r = n1 - n2;
    stdcall_ret(c, r < 0 ? 1u : r > 0 ? 3u : 2u, 6);
}

SHIM(kernel32, EnumSystemLocalesA) { stdcall_ret(c, 1, 2); }
SHIM(kernel32, IsValidCodePage) { stdcall_ret(c, 1, 1); }
SHIM(kernel32, IsValidLocale) { stdcall_ret(c, 1, 2); }
SHIM(kernel32, GetUserDefaultLCID) { stdcall_ret(c, 0x0409u, 0); }

SHIM(kernel32, GetTimeZoneInformation)
{
    memset(gptr(ARG(0)), 0, 172);
    stdcall_ret(c, 0, 1);
}

/* SYSTEMTIME (or now) as MM/dd/yy and HH:mm:ss; the format argument is ignored. */
static void systime(uint32_t st, struct tm *out)
{
    if (st) {
        memset(out, 0, sizeof(*out));
        out->tm_year = (int)RD16(st) - 1900;
        out->tm_mon = (int)RD16(st + 2u) - 1;
        out->tm_mday = (int)RD16(st + 6u);
        out->tm_hour = (int)RD16(st + 8u);
        out->tm_min = (int)RD16(st + 10u);
        out->tm_sec = (int)RD16(st + 12u);
    } else {
        time_t t = time(NULL);
        localtime_r(&t, out);
    }
}

static void format_out(Ctx *c, const char *s, uint32_t buf, uint32_t size)
{
    uint32_t n = (uint32_t)strlen(s) + 1u;
    if (size && n <= size)
        memcpy(gptr(buf), s, n);
    stdcall_ret(c, size && n > size ? 0u : n, 6);
}

SHIM(kernel32, GetDateFormatA)
{
    struct tm t;
    char s[32];
    systime(ARG(2), &t);
    strftime(s, sizeof(s), "%m/%d/%y", &t);
    format_out(c, s, ARG(4), ARG(5));
}

SHIM(kernel32, GetTimeFormatA)
{
    struct tm t;
    char s[32];
    systime(ARG(2), &t);
    strftime(s, sizeof(s), "%H:%M:%S", &t);
    format_out(c, s, ARG(4), ARG(5));
}

/* ---- user32 --------------------------------------------------------------- */

SHIM(user32, CallWindowProcA)
{
    uint32_t args[4] = { ARG(1), ARG(2), ARG(3), ARG(4) };
    /* The console's edit controls are subclassed over a window procedure of 0. */
    uint32_t r = ARG(0) ? guest_call(c, ARG(0), 4, args) : 0u;
    stdcall_ret(c, r, 5);
}

SHIM(user32, ChangeDisplaySettingsA) { stdcall_ret(c, 0, 2); }   /* DISP_CHANGE_SUCCESSFUL */
SHIM(user32, OpenClipboard) { stdcall_ret(c, 0, 1); }
SHIM(user32, GetClipboardData) { stdcall_ret(c, 0, 1); }
SHIM(user32, CloseClipboard) { stdcall_ret(c, 1, 0); }
SHIM(user32, CloseWindow) { stdcall_ret(c, 1, 1); }
SHIM(user32, LoadImageA) { stdcall_ret(c, 0x00070001u, 6); }
SHIM(user32, RegisterHotKey) { stdcall_ret(c, 1, 4); }
SHIM(user32, UnregisterHotKey) { stdcall_ret(c, 1, 2); }
SHIM(user32, SetTimer) { stdcall_ret(c, 1, 4); }

SHIM(user32, GetWindowTextA)
{
    if (ARG(2))
        WR8(ARG(1), 0);
    stdcall_ret(c, 0, 3);
}

SHIM(user32, SystemParametersInfoA)
{
    if (ARG(2))
        WR32(ARG(2), 0);
    stdcall_ret(c, 1, 4);
}

/* ---- winmm: time, no joystick, no MIDI ---------------------------------- */

SHIM(winmm, timeGetTime) { stdcall_ret(c, win32_tick_count(), 0); }
SHIM(winmm, timeBeginPeriod) { stdcall_ret(c, 0, 1); }
SHIM(winmm, timeEndPeriod) { stdcall_ret(c, 0, 1); }
SHIM(winmm, joyGetNumDevs) { stdcall_ret(c, 0, 0); }
SHIM(winmm, joyGetDevCapsA) { stdcall_ret(c, 167u, 3); }   /* JOYERR_UNPLUGGED */
SHIM(winmm, joyGetPosEx) { stdcall_ret(c, 167u, 2); }
SHIM(winmm, midiInGetNumDevs) { stdcall_ret(c, 0, 0); }
SHIM(winmm, midiInGetDevCapsA) { stdcall_ret(c, 2u, 3); }  /* MMSYSERR_BADDEVICEID */
SHIM(winmm, midiInOpen) { stdcall_ret(c, 2u, 5); }
SHIM(winmm, midiInStart) { stdcall_ret(c, 5u, 1); }        /* MMSYSERR_INVALHANDLE */
SHIM(winmm, midiInClose) { stdcall_ret(c, 5u, 1); }

/* ---- advapi32, shell32 ------------------------------------------------------ */

/* The default player name. */
SHIM(advapi32, GetUserNameA)
{
    const char *name = "Player";
    uint32_t buf = ARG(0), psize = ARG(1);
    uint32_t n = (uint32_t)strlen(name) + 1u;
    if (RD32(psize) < n) {
        WR32(psize, n);
        win32_set_last_error(122u);   /* ERROR_INSUFFICIENT_BUFFER */
        stdcall_ret(c, 0, 2);
        return;
    }
    memcpy(gptr(buf), name, n);
    WR32(psize, n);
    stdcall_ret(c, 1, 2);
}

SHIM(shell32, ShellExecuteA) { stdcall_ret(c, 2u, 6); }   /* <= 32: failed (no browser) */

/* ---- ddraw: the auto-configuration's video memory probe ------------------- */

SHIM(ddraw, DirectDrawEnumerateExA) { stdcall_ret(c, 0, 3); }   /* no devices listed */
SHIM(ddraw, DirectDrawCreateEx) { stdcall_ret(c, 0x80004005u, 4); }   /* E_FAIL */

/* ---- gdi32 ---------------------------------------------------------------- */

SHIM(gdi32, CreateFontA) { stdcall_ret(c, 0x000A0001u, 14); }
SHIM(gdi32, CreateSolidBrush) { stdcall_ret(c, 0x000B0001u, 1); }
SHIM(gdi32, SetBkColor) { stdcall_ret(c, 0, 2); }
SHIM(gdi32, SetTextColor) { stdcall_ret(c, 0, 2); }
