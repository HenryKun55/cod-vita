/* opengl32.dll: wgl*, the GDI pixel-format calls and the gl* functions that need
 * more than passing their arguments on (the rest: opengl32_gen.c).
 *
 * GL runs on the game thread, straight into the host context: guest pointers
 * (client vertex arrays included) are host pointers into guest memory, which
 * stays put. */
#include "opengl32.h"

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

void user32_window_size(int *w, int *h);

/* ---- entry points ------------------------------------------------------- */

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
            if (e == GL_EXTENSIONS && getenv("COD_NO_S3TC"))
                s = "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_ARB_texture_env_add GL_EXT_texture_env_add "
                    "GL_ARB_texture_env_combine GL_EXT_draw_range_elements GL_ARB_texture_cube_map";
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

/* ---- textures ---------------------------------------------------------------
 * The Vita gets no compressed textures: compressed uploads to vitaGL corrupted
 * memory while a level loaded (the Freedom Fighters port saw the same). DXT
 * images are decoded to RGBA here, and a compressed internal format asked of
 * glTexImage2D becomes the plain one. Elsewhere the driver takes them as they are. */

#define GL_DXT1_RGB 0x83F0u
#define GL_DXT1_RGBA 0x83F1u
#define GL_DXT3 0x83F2u
#define GL_DXT5 0x83F3u

typedef void (*pfn_teximage)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*pfn_ctex)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (*pfn_csubtex)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);

static void *gl_host(const char *name)
{
    void *p = platform_gl_proc(name);
    if (!p) {
        size_t n = strlen(name);
        char core[96];
        if (n > 3 && n < sizeof(core)) {
            snprintf(core, sizeof(core), "%.*s", (int)(n - 3), name);
            p = platform_gl_proc(core);
        }
    }
    return p;
}

static uint32_t rgb565_rgba(uint32_t c)
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
}

static uint32_t mix_rgba(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t d)
{
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8)
        out |= ((((a >> sh) & 0xFFu) * wa + ((b >> sh) & 0xFFu) * wb) / d) << sh;
    return out;
}

/* The 16 colors of a DXT color block (4 bytes of endpoints, 4 of indices). */
static void dxt_colors(const uint8_t *b, int dxt1, uint32_t *px)
{
    uint32_t c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8), c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    uint32_t pal[4];
    pal[0] = rgb565_rgba(c0);
    pal[1] = rgb565_rgba(c1);
    if (c0 > c1 || !dxt1) {
        pal[2] = mix_rgba(pal[0], pal[1], 2, 1, 3);
        pal[3] = mix_rgba(pal[0], pal[1], 1, 2, 3);
    } else {
        pal[2] = mix_rgba(pal[0], pal[1], 1, 1, 2);
        pal[3] = 0;                      /* transparent black */
    }
    uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++)
        px[i] = pal[(bits >> (2 * i)) & 3u];
}

static void dxt5_alpha(const uint8_t *b, uint32_t *px)
{
    uint32_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; i++)
            a[i + 1] = ((7u - (uint32_t)i) * a[0] + (uint32_t)i * a[1]) / 7u;
    } else {
        for (int i = 1; i < 5; i++)
            a[i + 1] = ((5u - (uint32_t)i) * a[0] + (uint32_t)i * a[1]) / 5u;
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
        px[i] = (px[i] & 0x00FFFFFFu) | (a[(bits >> (3 * i)) & 7u] << 24);
}

/* A w x h DXT image (format fmt) to RGBA8, or NULL for another format. */
static uint32_t *dxt_decode(GLenum fmt, const uint8_t *src, uint32_t w, uint32_t h)
{
    if (fmt < GL_DXT1_RGB || fmt > GL_DXT5 || !src || !w || !h)
        return NULL;
    uint32_t *out = malloc((size_t)w * h * 4u);
    if (!out)
        return NULL;
    uint32_t bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
    uint32_t bsize = fmt <= GL_DXT1_RGBA ? 8u : 16u;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + (by * bw + bx) * bsize;
            uint32_t px[16];
            if (bsize == 8u) {
                dxt_colors(b, 1, px);
            } else {
                dxt_colors(b + 8, 0, px);
                if (fmt == GL_DXT3) {
                    for (int i = 0; i < 16; i++) {
                        uint32_t a4 = (b[i / 2] >> (4 * (i & 1))) & 15u;
                        px[i] = (px[i] & 0x00FFFFFFu) | ((a4 * 17u) << 24);
                    }
                } else {
                    dxt5_alpha(b, px);
                }
            }
            for (uint32_t y = 0; y < 4u && by * 4u + y < h; y++)
                for (uint32_t x = 0; x < 4u && bx * 4u + x < w; x++)
                    out[(size_t)(by * 4u + y) * w + bx * 4u + x] = px[y * 4u + x];
        }
    }
    return out;
}

#if defined(__vita__)
static GLint plain_format(GLint f)
{
    switch ((uint32_t)f) {
    case GL_DXT1_RGB: case 0x84EDu: /* GL_COMPRESSED_RGB_ARB */ return GL_RGB;
    case GL_DXT1_RGBA: case GL_DXT3: case GL_DXT5: case 0x84EEu: /* GL_COMPRESSED_RGBA_ARB */ return GL_RGBA;
    default: return f;
    }
}
#endif

void glm_glTexImage2D(Ctx *c)
{
    static pfn_teximage fn;
    if (!fn)
        fn = (pfn_teximage)gl_host("glTexImage2D");
    GLint ifmt = (GLint)ARG(2);
#if defined(__vita__)
    ifmt = plain_format(ifmt);
#endif
    fn((GLenum)ARG(0), (GLint)ARG(1), ifmt, (GLsizei)ARG(3), (GLsizei)ARG(4), (GLint)ARG(5), (GLenum)ARG(6),
       (GLenum)ARG(7), gptr(ARG(8)));
    stdcall_ret(c, c->r[0], 9);
}

static void compressed_image(Ctx *c, const char *name)
{
    GLenum target = (GLenum)ARG(0), fmt = (GLenum)ARG(2);
    GLint level = (GLint)ARG(1), border = (GLint)ARG(5);
    GLsizei w = (GLsizei)ARG(3), h = (GLsizei)ARG(4), size = (GLsizei)ARG(6);
    const void *data = gptr(ARG(7));
#if defined(__vita__)
    (void)name, (void)size;
    uint32_t *rgba = dxt_decode(fmt, data, (uint32_t)w, (uint32_t)h);
    if (rgba) {
        glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    } else {
        rt_log("glCompressedTexImage2D: format %04X not decoded", fmt);
    }
#else
    static pfn_ctex fn;
    if (!fn)
        fn = (pfn_ctex)gl_host(name);
    fn(target, level, fmt, w, h, border, size, data);
#endif
    stdcall_ret(c, c->r[0], 8);
}

static void compressed_subimage(Ctx *c, const char *name)
{
    GLenum target = (GLenum)ARG(0), fmt = (GLenum)ARG(6);
    GLint level = (GLint)ARG(1), x = (GLint)ARG(2), y = (GLint)ARG(3);
    GLsizei w = (GLsizei)ARG(4), h = (GLsizei)ARG(5), size = (GLsizei)ARG(7);
    const void *data = gptr(ARG(8));
#if defined(__vita__)
    (void)name, (void)size;
    uint32_t *rgba = dxt_decode(fmt, data, (uint32_t)w, (uint32_t)h);
    if (rgba) {
        glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    }
#else
    static pfn_csubtex fn;
    if (!fn)
        fn = (pfn_csubtex)gl_host(name);
    fn(target, level, x, y, w, h, fmt, size, data);
#endif
    stdcall_ret(c, c->r[0], 9);
}

void glm_glCompressedTexImage2D(Ctx *c) { compressed_image(c, "glCompressedTexImage2D"); }
void glm_glCompressedTexImage2DARB(Ctx *c) { compressed_image(c, "glCompressedTexImage2DARB"); }
void glm_glCompressedTexSubImage2D(Ctx *c) { compressed_subimage(c, "glCompressedTexSubImage2D"); }
void glm_glCompressedTexSubImage2DARB(Ctx *c) { compressed_subimage(c, "glCompressedTexSubImage2DARB"); }

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

/* Vsync stays on whatever the game asks (r_swapInterval 0 is its default): at
 * hundreds of frames a second its movement code, which counts whole milliseconds,
 * steps unevenly (crouching, view height). COD_VSYNC=0 turns it off. */
static void wgl_swap_interval(Ctx *c)
{
    const char *vs = getenv("COD_VSYNC");
    platform_set_swap_interval(!(vs && vs[0] == '0'));
    stdcall_ret(c, 1, 1);
}

static void wgl_get_swap_interval(Ctx *c) { stdcall_ret(c, 1, 0); }

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

static uint32_t gl_missing(void) { return 0; }

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
            if (!p) {
                /* The game looks up the whole GL 1.1 set and calls some functions
                 * without checking (vitaGL lacks a few, e.g. glLightModeli): they do
                 * nothing. Calling a function without parameters with arguments is
                 * harmless in the host ABIs (the caller owns them). */
                rt_log("GL: %s not in the driver: does nothing", name);
                p = (void *)gl_missing;
            }
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

#if defined(__vita__)
/* The Vita build is shot from the Vita3K window by tools/qa_vita3k.sh, which
 * watches for this file. */
static void save_shot(void)
{
    FILE *f = fopen("ux0:data/cod1/shot_now.txt", "w");
    if (f) {
        fputs(shot_name, f);
        fclose(f);
    }
    rt_log("screenshot %s requested", shot_name);
}
#else
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
#endif

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
