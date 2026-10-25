/* SSE and MMX for the generated code: the XMM and MMX registers live in the
 * context (Ctx.xmm, Ctx.mm); tools/recomp/gen.py turns each instruction into one
 * of these helpers. Packed single precision arithmetic uses NEON on the Vita
 * (flush-to-zero for denormals, otherwise the IEEE results SSE gives). The MMX
 * registers are kept apart from the x87 stack they alias on a real CPU: the game
 * separates the two with EMMS, as it must. Included by recomp.h. */
#ifndef COD_SIMD_H
#define COD_SIMD_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define COD_NEON 1
#endif

typedef union Xmm {
    float f[4];
    uint32_t u[4];
    uint64_t q[2];
} Xmm;

static inline Xmm rt_xload(const uint8_t *p)
{
    Xmm v;
    memcpy(&v, p, 16);
    return v;
}

#if defined(COD_NEON)
#define RT_PS_OP(name, neon, expr)                                                     \
    static inline void name(Xmm *d, const Xmm *s)                                      \
    {                                                                                  \
        vst1q_f32(d->f, neon(vld1q_f32(d->f), vld1q_f32(s->f)));                       \
    }
#else
#define RT_PS_OP(name, neon, expr)                                                     \
    static inline void name(Xmm *d, const Xmm *s)                                      \
    {                                                                                  \
        for (int i = 0; i < 4; i++) {                                                  \
            float a = d->f[i], b = s->f[i];                                            \
            d->f[i] = (expr);                                                          \
        }                                                                              \
    }
#endif
RT_PS_OP(rt_mulps, vmulq_f32, a * b)
RT_PS_OP(rt_addps, vaddq_f32, a + b)
RT_PS_OP(rt_subps, vsubq_f32, a - b)
#undef RT_PS_OP

/* MINPS/MAXPS: the second operand when either is NaN or both are zero. */
static inline void rt_minps(Xmm *d, const Xmm *s)
{
    for (int i = 0; i < 4; i++)
        d->f[i] = d->f[i] < s->f[i] ? d->f[i] : s->f[i];
}

static inline void rt_maxps(Xmm *d, const Xmm *s)
{
    for (int i = 0; i < 4; i++)
        d->f[i] = d->f[i] > s->f[i] ? d->f[i] : s->f[i];
}

static inline void rt_andps(Xmm *d, const Xmm *s)
{
    d->q[0] &= s->q[0];
    d->q[1] &= s->q[1];
}

static inline void rt_shufps(Xmm *d, const Xmm *s, unsigned imm)
{
    Xmm r;
    r.u[0] = d->u[imm & 3u];
    r.u[1] = d->u[(imm >> 2) & 3u];
    r.u[2] = s->u[(imm >> 4) & 3u];
    r.u[3] = s->u[(imm >> 6) & 3u];
    *d = r;
}

static inline void rt_unpcklps(Xmm *d, const Xmm *s)
{
    Xmm r = { .u = { d->u[0], s->u[0], d->u[1], s->u[1] } };
    *d = r;
}

static inline void rt_unpckhps(Xmm *d, const Xmm *s)
{
    Xmm r = { .u = { d->u[2], s->u[2], d->u[3], s->u[3] } };
    *d = r;
}

/* RCPPS/RSQRTPS are approximations on x86 (12 bits); the exact values are within them. */
static inline void rt_rcpps(Xmm *d, const Xmm *s)
{
    Xmm r;
    for (int i = 0; i < 4; i++)
        r.f[i] = 1.0f / s->f[i];
    *d = r;
}

static inline void rt_rsqrtps(Xmm *d, const Xmm *s)
{
    Xmm r;
    for (int i = 0; i < 4; i++)
        r.f[i] = 1.0f / sqrtf(s->f[i]);
    *d = r;
}

/* CVTPI2PS: two signed 32-bit integers to the low two floats (the high two stay). */
static inline void rt_cvtpi2ps(Xmm *d, uint64_t v)
{
    d->f[0] = (float)(int32_t)(uint32_t)v;
    d->f[1] = (float)(int32_t)(uint32_t)(v >> 32);
}

/* CVTTPS2PI: truncation, the integer indefinite 0x80000000 when out of range or NaN. */
static inline uint32_t rt_cvtt_ss(float f)
{
    if (!(f > -2147483904.0f && f < 2147483648.0f))
        return 0x80000000u;
    return (uint32_t)(int32_t)f;
}

static inline uint64_t rt_cvttps2pi(float lo, float hi)
{
    return (uint64_t)rt_cvtt_ss(lo) | ((uint64_t)rt_cvtt_ss(hi) << 32);
}

/* ---- MMX (64-bit registers as 8 bytes / 4 words / 2 dwords) ---- */

typedef union Mmx {
    uint64_t q;
    uint32_t d[2];
    uint16_t w[4];
    int16_t sw[4];
    uint8_t b[8];
} Mmx;

static inline uint64_t rt_psllw(uint64_t a, uint64_t n)
{
    Mmx v = { .q = a };
    for (int i = 0; i < 4; i++)
        v.w[i] = n > 15u ? 0u : (uint16_t)(v.w[i] << n);
    return v.q;
}

static inline uint64_t rt_psraw(uint64_t a, uint64_t n)
{
    Mmx v = { .q = a };
    unsigned k = n > 15u ? 15u : (unsigned)n;
    for (int i = 0; i < 4; i++)
        v.sw[i] = (int16_t)(v.sw[i] >> k);
    return v.q;
}

static inline uint64_t rt_pmulhw(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b };
    for (int i = 0; i < 4; i++)
        x.w[i] = (uint16_t)(((int32_t)x.sw[i] * (int32_t)y.sw[i]) >> 16);
    return x.q;
}

static inline uint64_t rt_paddw(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b };
    for (int i = 0; i < 4; i++)
        x.w[i] = (uint16_t)(x.w[i] + y.w[i]);
    return x.q;
}

static inline uint64_t rt_psubw(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b };
    for (int i = 0; i < 4; i++)
        x.w[i] = (uint16_t)(x.w[i] - y.w[i]);
    return x.q;
}

static inline uint64_t rt_psubusw(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b };
    for (int i = 0; i < 4; i++)
        x.w[i] = x.w[i] > y.w[i] ? (uint16_t)(x.w[i] - y.w[i]) : 0u;
    return x.q;
}

static inline uint64_t rt_punpcklbw(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b }, r;
    for (int i = 0; i < 4; i++) {
        r.b[2 * i] = x.b[i];
        r.b[2 * i + 1] = y.b[i];
    }
    return r.q;
}

static inline uint64_t rt_punpcklwd(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b }, r;
    r.w[0] = x.w[0];
    r.w[1] = y.w[0];
    r.w[2] = x.w[1];
    r.w[3] = y.w[1];
    return r.q;
}

static inline uint64_t rt_punpckhwd(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b }, r;
    r.w[0] = x.w[2];
    r.w[1] = y.w[2];
    r.w[2] = x.w[3];
    r.w[3] = y.w[3];
    return r.q;
}

static inline uint8_t rt_sat_ub(int16_t v) { return v < 0 ? 0u : v > 255 ? 255u : (uint8_t)v; }

static inline uint64_t rt_packuswb(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b }, r;
    for (int i = 0; i < 4; i++) {
        r.b[i] = rt_sat_ub(x.sw[i]);
        r.b[4 + i] = rt_sat_ub(y.sw[i]);
    }
    return r.q;
}

static inline int16_t rt_sat_sw(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

static inline uint64_t rt_packssdw(uint64_t a, uint64_t b)
{
    Mmx x = { .q = a }, y = { .q = b }, r;
    r.sw[0] = rt_sat_sw((int32_t)x.d[0]);
    r.sw[1] = rt_sat_sw((int32_t)x.d[1]);
    r.sw[2] = rt_sat_sw((int32_t)y.d[0]);
    r.sw[3] = rt_sat_sw((int32_t)y.d[1]);
    return r.q;
}

#endif
