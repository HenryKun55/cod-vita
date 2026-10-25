/* opengl32.dll: wgl*, the GDI pixel-format calls and the gl* functions that need
 * more than passing their arguments on (the rest: opengl32_gen.c).
 *
 * GL runs on the game thread, straight into the host context: guest pointers
 * (client vertex arrays included) are host pointers into guest memory, which
 * stays put. */
#include "opengl32.h"

#include "platform.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

void user32_window_size(int *w, int *h);

/* ---- entry points ------------------------------------------------------- */

static int gl_ready;

void *platform_gl_proc(const char *name)
{
    if (!gl_ready) {
        SDL_GL_LoadLibrary(NULL);
        gl_ready = 1;
    }
    return SDL_GL_GetProcAddress(name);
}

/* ---- hand-written gl* --------------------------------------------------- */

/* Extensions the game is told about: the ones its renderer can use here. */
static const char *const gl_extensions =
    "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_ARB_texture_env_add GL_EXT_texture_env_add "
    "GL_ARB_texture_env_combine GL_EXT_texture_filter_anisotropic GL_ARB_texture_compression "
    "GL_EXT_texture_compression_s3tc GL_EXT_draw_range_elements GL_ARB_texture_cube_map";

/* glGetString returns a pointer the guest reads: a copy in guest memory per name. */
void glm_glGetString(Ctx *c)
{
    static uint32_t cache[8];
    static const GLenum names[] = { GL_VENDOR, GL_RENDERER, GL_VERSION, GL_EXTENSIONS };
    GLenum e = (GLenum)ARG(0);
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        if (names[i] != e)
            continue;
        if (!cache[i]) {
            const char *s = e == GL_EXTENSIONS ? gl_extensions : (const char *)glGetString(e);
            if (e == GL_VERSION)
                s = "1.3.0";
            cache[i] = scratch_strdup(s ? s : "");
            rt_log("glGetString(%04X) = %s", e, s ? s : "");
        }
        r = cache[i];
    }
    stdcall_ret(c, r, 1);
}

/* The game sizes viewports for its resolution; the drawable may be bigger (HiDPI
 * or a scaled window). */
static void to_drawable(GLint *x, GLint *y, GLsizei *w, GLsizei *h)
{
    int mw, mh, dw, dh;
    platform_get_video_mode(&mw, &mh);
    platform_get_drawable_size(&dw, &dh);
    if (mw <= 0 || mh <= 0 || (mw == dw && mh == dh))
        return;
    *x = (GLint)((int64_t)*x * dw / mw);
    *y = (GLint)((int64_t)*y * dh / mh);
    *w = (GLsizei)((int64_t)*w * dw / mw);
    *h = (GLsizei)((int64_t)*h * dh / mh);
}

void glm_glViewport(Ctx *c)
{
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    to_drawable(&x, &y, &w, &h);
    glViewport(x, y, w, h);
    stdcall_ret(c, c->r[0], 4);
}

void glm_glScissor(Ctx *c)
{
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    to_drawable(&x, &y, &w, &h);
    glScissor(x, y, w, h);
    stdcall_ret(c, c->r[0], 4);
}

/* Screenshots read the game's resolution: read the drawable and scale down. */
void glm_glReadPixels(Ctx *c)
{
    glReadPixels((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), (GLenum)ARG(4), (GLenum)ARG(5),
                 gptr(ARG(6)));
    stdcall_ret(c, c->r[0], 7);
}

/* Compiled vertex arrays are only a hint. */
void glm_glLockArraysEXT(Ctx *c) { stdcall_ret(c, c->r[0], 2); }
void glm_glUnlockArraysEXT(Ctx *c) { stdcall_ret(c, c->r[0], 0); }

/* ---- wgl ---------------------------------------------------------------- */

#define HGLRC_FAKE 0x00C0FFEEu

static void wgl_create_context(Ctx *c)
{
    int w, h;
    user32_window_size(&w, &h);
    platform_set_video_mode(w, h);
    platform_gl_init();
    rt_log("wglCreateContext: %dx%d", w, h);
    stdcall_ret(c, HGLRC_FAKE, 1);
}

static void wgl_make_current(Ctx *c)
{
    if (ARG(1))
        platform_gl_init();
    stdcall_ret(c, 1, 2);
}

static void wgl_delete_context(Ctx *c) { stdcall_ret(c, 1, 1); }
static void wgl_get_current_context(Ctx *c) { stdcall_ret(c, HGLRC_FAKE, 0); }
static void wgl_get_current_dc(Ctx *c) { stdcall_ret(c, 0x00DC0001u, 0); }
static void wgl_share_lists(Ctx *c) { stdcall_ret(c, 1, 2); }

static void wgl_get_proc_address(Ctx *c)
{
    const char *name = gstr(ARG(0));
    uint32_t r = name ? opengl32_get_proc(name) : 0;
    rt_log("wglGetProcAddress(%s) -> %08X", name ? name : "", r);
    stdcall_ret(c, r, 1);
}

static void present(void);

static void wgl_swap_buffers(Ctx *c)
{
    present();
    stdcall_ret(c, 1, 1);
}

static void wgl_swap_interval(Ctx *c)
{
    const char *vs = getenv("COD_VSYNC");
    SDL_GL_SetSwapInterval(vs && vs[0] == '0' ? 0 : (int)ARG(0));
    stdcall_ret(c, 1, 1);
}

static void wgl_get_swap_interval(Ctx *c) { stdcall_ret(c, (uint32_t)SDL_GL_GetSwapInterval(), 0); }

static void wgl_get_extensions_string(Ctx *c)
{
    static uint32_t s;
    if (!s)
        s = scratch_strdup("WGL_EXT_swap_control WGL_EXT_extensions_string");
    stdcall_ret(c, s, 0);
}

static void wgl_get_extensions_string_arb(Ctx *c)
{
    static uint32_t s;
    if (!s)
        s = scratch_strdup("WGL_EXT_swap_control WGL_EXT_extensions_string");
    stdcall_ret(c, s, 1);
}

/* PIXELFORMATDESCRIPTOR of the one format offered: RGBA 32, depth 24, stencil 8,
 * double-buffered, accelerated. */
static void fill_pfd(uint32_t p, uint32_t size)
{
    if (!p || size < 40u)
        return;
    memset(gptr(p), 0, 40);
    WR16(p + 0u, 40);
    WR16(p + 2u, 1);
    WR32(p + 4u, 0x00000004u | 0x00000020u | 0x00000001u);   /* DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER */
    WR8(p + 8u, 0);    /* PFD_TYPE_RGBA */
    WR8(p + 9u, 32);   /* cColorBits */
    WR8(p + 10u, 8);
    WR8(p + 11u, 16);
    WR8(p + 12u, 8);
    WR8(p + 13u, 8);
    WR8(p + 14u, 8);
    WR8(p + 15u, 0);
    WR8(p + 16u, 8);   /* alpha */
    WR8(p + 17u, 24);
    WR8(p + 23u, 24);  /* cDepthBits */
    WR8(p + 24u, 8);   /* cStencilBits */
}

static void wgl_describe_pixel_format(Ctx *c)
{
    fill_pfd(ARG(3), ARG(2));
    stdcall_ret(c, 1, 4);
}

static void wgl_choose_pixel_format(Ctx *c) { stdcall_ret(c, 1, 2); }
static void wgl_set_pixel_format(Ctx *c) { stdcall_ret(c, 1, 3); }
static void wgl_get_pixel_format(Ctx *c) { stdcall_ret(c, 1, 1); }

static const struct {
    const char *name;
    GuestFn fn;
} wgl_entries[] = {
    { "wglCreateContext", wgl_create_context },
    { "wglMakeCurrent", wgl_make_current },
    { "wglDeleteContext", wgl_delete_context },
    { "wglGetCurrentContext", wgl_get_current_context },
    { "wglGetCurrentDC", wgl_get_current_dc },
    { "wglShareLists", wgl_share_lists },
    { "wglGetProcAddress", wgl_get_proc_address },
    { "wglSwapBuffers", wgl_swap_buffers },
    { "wglSwapIntervalEXT", wgl_swap_interval },
    { "wglGetSwapIntervalEXT", wgl_get_swap_interval },
    { "wglGetExtensionsStringEXT", wgl_get_extensions_string },
    { "wglGetExtensionsStringARB", wgl_get_extensions_string_arb },
    { "wglDescribePixelFormat", wgl_describe_pixel_format },
    { "wglChoosePixelFormat", wgl_choose_pixel_format },
    { "wglSetPixelFormat", wgl_set_pixel_format },
    { "wglGetPixelFormat", wgl_get_pixel_format },
};
static const unsigned wgl_entry_count = sizeof(wgl_entries) / sizeof(wgl_entries[0]);

uint32_t opengl32_get_proc(const char *name)
{
    for (unsigned i = 0; i < wgl_entry_count; i++)
        if (strcmp(wgl_entries[i].name, name) == 0)
            return thunk_register(wgl_entries[i].fn, wgl_entries[i].name);
    for (unsigned i = 0; i < g_gl_entry_count; i++) {
        const GlEntry *e = &g_gl_entries[i];
        if (strcmp(e->name, name) != 0)
            continue;
        if (e->host && !*e->host) {
            void *p = platform_gl_proc(name);
            size_t n = strlen(name);
            if (!p && n > 3 && (!strcmp(name + n - 3, "ARB") || !strcmp(name + n - 3, "EXT"))) {
                char core[96];
                snprintf(core, sizeof(core), "%.*s", (int)(n - 3), name);
                p = platform_gl_proc(core);
            }
            if (!p)
                return 0;
            *e->host = p;
        }
        return thunk_register(e->fn, e->name);
    }
    return 0;
}


/* ---- frames: screenshots for the QA scripts (COD_INPUT "shot=name") ------- */

static char shot_name[256];
static int shot_pending;

void render_shot_request(const char *name)
{
    snprintf(shot_name, sizeof(shot_name), "%s", name);
    shot_pending = 1;
}

void render_diag_set(int mode) { (void)mode; }
void render_shutdown(void) {}

/* The back buffer as a 24-bit BMP (bottom-up rows, as GL reads them). */
static void save_shot(void)
{
    int w, h;
    platform_get_drawable_size(&w, &h);
    int stride = (w * 3 + 3) & ~3;
    uint8_t *px = malloc((size_t)stride * h);
    if (!px)
        return;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px);
    char path[300];
    snprintf(path, sizeof(path), "%s.bmp", shot_name);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t size = 54u + (uint32_t)(stride * h);
        uint8_t hd[54] = { 'B', 'M' };
        memcpy(hd + 2, &size, 4);
        hd[10] = 54;
        hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        memcpy(hd + 22, &h, 4);
        hd[26] = 1;
        hd[28] = 24;
        fwrite(hd, 1, 54, f);
        fwrite(px, 1, (size_t)stride * h, f);
        fclose(f);
        rt_log("screenshot %s (%dx%d)", path, w, h);
    }
    free(px);
}

int g_gl_tracing;
extern int g_trace_next_frame;

void gl_trace_call(Ctx *c, const char *name)
{
    static uint32_t vptr, cptr, vsize = 3, vstride, cstride;
    rt_log("gl %s(%08X %08X %08X %08X %08X %08X)", name, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5));
    if (!strcmp(name, "glVertexPointer")) {
        vsize = ARG(0);
        vstride = ARG(2) ? ARG(2) : vsize * 4u;
        vptr = ARG(3);
    } else if (!strcmp(name, "glColorPointer")) {
        cstride = ARG(2) ? ARG(2) : 4u;
        cptr = ARG(3);
    } else if (!strcmp(name, "glDrawElements") && vptr) {
        /* the first vertices drawn (unsigned int indices) */
        uint32_t n = ARG(1) < 4u ? ARG(1) : 4u, idx = ARG(3);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t i = ARG(2) == 0x1405u ? RD32(idx + 4u * k) : ARG(2) == 0x1403u ? RD16(idx + 2u * k) : RD8(idx + k);
            float x, y, z;
            uint32_t a = vptr + i * vstride;
            memcpy(&x, gptr(a), 4);
            memcpy(&y, gptr(a + 4u), 4);
            memcpy(&z, gptr(a + 8u), 4);
            rt_log("    v%u: %.1f %.1f %.1f  rgba %08X", i, x, y, z, cptr ? RD32(cptr + i * cstride) : 0u);
        }
    }
}

static void present(void)
{
    if (g_gl_tracing) {
        g_gl_tracing = 0;
        g_trace_next_frame = 0;
        rt_log("gl trace: end of frame");
    } else if (g_trace_next_frame) {
        g_gl_tracing = 1;
        rt_log("gl trace: next frame");
    }
    if (shot_pending) {
        shot_pending = 0;
        save_shot();
    }
    platform_swap();
}

/* ---- gdi32 pixel formats, buffers and gamma ------------------------------ */

SHIM(gdi32, ChoosePixelFormat) { stdcall_ret(c, 1, 2); }
SHIM(gdi32, SetPixelFormat) { stdcall_ret(c, 1, 3); }

SHIM(gdi32, DescribePixelFormat)
{
    fill_pfd(ARG(3), ARG(2));
    stdcall_ret(c, 1, 4);   /* the number of formats */
}

SHIM(gdi32, SwapBuffers)
{
    present();
    stdcall_ret(c, 1, 1);
}

/* Gamma ramps: 3 x 256 WORDs. The game's brightness setting (r_gamma) goes
 * through them; identity until the host side applies them. */
SHIM(gdi32, GetDeviceGammaRamp)
{
    uint32_t p = ARG(1);
    for (uint32_t k = 0; k < 3u; k++)
        for (uint32_t i = 0; i < 256u; i++)
            WR16(p + (k * 256u + i) * 2u, i * 257u);
    stdcall_ret(c, 1, 2);
}

SHIM(gdi32, SetDeviceGammaRamp) { stdcall_ret(c, 1, 2); }
