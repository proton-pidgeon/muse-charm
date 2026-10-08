// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * VESPER -- the evening-star owl.
 *
 * Vesper is a small, plump night owl: a deep indigo body with a pale moon-
 * coloured face disc, two pointed ear tufts, big dark eyes with a glint, a
 * little gold beak and gold feet. On its forehead sits the evening star (the
 * planet Vesper): a four-point sparkle that takes the colour of the current
 * mode -- warm gold when idle, cyan when listening, magenta when thinking,
 * mint when speaking, red on error -- and flares when Vesper is petted.
 *
 * This is original art for the Vesper node. Only the muse_pixel.h contract and
 * the generic set_size/scale blitter follow the SDK's default renderer
 * (Apache-2.0, Meta Platforms); the character itself does not.
 *
 * Cost: per-pixel work is Q12 fixed point inside the body/wing/aura bounding
 * boxes; floats, sqrtf, sinf and expf are used per frame or per part only.
 * Static memory only (2 x 4 KiB planes + palettes), no allocation.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_OUT,       /* outline */
    C_OUT2,      /* soft ring around the face disc */
    C_BD,        /* feather dark ... (contiguous: tone ramp) */
    C_BM,
    C_BL,
    C_BH,        /* ... feather highlight */
    C_RIM,       /* mode-tinted rim light */
    C_FACED,     /* face disc shade */
    C_FACE,
    C_FACEL,
    C_IRIS,      /* eyes */
    C_BROW,
    C_BLUSH,
    C_BLUSHD,
    C_BEAK,
    C_BEAKD,
    C_MOUTH,
    C_TONGUE,
    C_EARIN,
    C_G0,        /* mode glow ramp, bright ... */
    C_G1,
    C_G2,
    C_G3,        /* ... deep */
    C_AURA1,
    C_AURA2,
    C_SPK,
    C_ACC,
    C_SHADOW,
    C_HEART,
    C_WHITE,
    C_COUNT,
};

typedef struct {
    float r, g, b;
} rgb_t;

typedef struct {
    uint32_t f[4];
    uint32_t acc;
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xcfe0ff, 0x8fa8ff, 0x5a5fe0 }, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { { 0xfff6d2, 0xffd96b, 0xffb42e, 0xb86a10 }, 0xffc94d },  /* evening-star gold */
    [MUSE_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { { 0xffe6ff, 0xff9cf0, 0xd35bff, 0x7a2bd9 }, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [MUSE_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [MUSE_MODE_OFF]       = { { 0xd8d4ff, 0x8f86d9, 0x5a4fb0, 0x2e2870 }, 0x7c72d0 },
};

static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x0b0820,
    [C_OUT2] = 0x8f87c0,
    [C_BD] = 0x2b2358,
    [C_BM] = 0x42388a,
    [C_BL] = 0x5f52b4,
    [C_BH] = 0x8c7fdc,
    [C_FACED] = 0xc2bce6,
    [C_FACE] = 0xe3dffa,
    [C_FACEL] = 0xfbf9ff,
    [C_IRIS] = 0x140d33,
    [C_BROW] = 0x3a2f78,
    [C_BLUSH] = 0xf9a6c8,
    [C_BLUSHD] = 0xe987b4,
    [C_BEAK] = 0xffc83c,
    [C_BEAKD] = 0xd68a24,
    [C_MOUTH] = 0x3d0f2c,
    [C_TONGUE] = 0xff6b8b,
    [C_EARIN] = 0xb585dc,
    [C_SHADOW] = 0x120c26,
    [C_HEART] = 0xff4f8b,
    [C_WHITE] = 0xffffff,
};

static rgb_t s_scheme[5];
static bool s_scheme_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint8_t s_fb[W * H];
static uint8_t s_mask[W * H];

static const uint8_t BAYER4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

#define Q 12
#define ONE (1 << Q)
#define QF(v) ((int32_t)((v) * ONE))

static inline int bayer(int x, int y)
{
    return BAYER4[y & 3][x & 3];
}

static inline rgb_t hex_rgb(uint32_t c)
{
    return (rgb_t){ (float)((c >> 16) & 255), (float)((c >> 8) & 255), (float)(c & 255) };
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline rgb_t scale_rgb(rgb_t a, float k)
{
    return (rgb_t){ a.r * k, a.g * k, a.b * k };
}

static inline uint16_t to565(rgb_t c)
{
    int r = (int)(c.r + 0.5f), g = (int)(c.g + 0.5f), b = (int)(c.b + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[(unsigned)mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

static void update_palette(const scheme_t *target, float dt)
{
    rgb_t tgt[5];
    for (int i = 0; i < 4; i++) {
        tgt[i] = hex_rgb(target->f[i]);
    }
    tgt[4] = hex_rgb(target->acc);

    float k = s_scheme_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    for (int i = 0; i < 5; i++) {
        s_scheme[i] = mix(s_scheme[i], tgt[i], k);
    }
    s_scheme_init = true;

    rgb_t pal[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) {
        pal[i] = hex_rgb(FIXED[i]);
    }
    rgb_t acc = s_scheme[4];
    pal[C_G0] = s_scheme[0];
    pal[C_G1] = s_scheme[1];
    pal[C_G2] = s_scheme[2];
    pal[C_G3] = s_scheme[3];
    pal[C_ACC] = acc;
    pal[C_RIM] = mix(pal[C_BH], acc, 0.55f);
    pal[C_AURA1] = scale_rgb(acc, 0.14f);
    pal[C_AURA2] = scale_rgb(acc, 0.30f);
    pal[C_SPK] = mix(acc, pal[C_WHITE], 0.5f);

    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(pal[i]);
        s_pal_dim[i] = to565(scale_rgb(pal[i], 0.72f));
    }
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static inline int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fracf(float v)
{
    return v - floorf(v);
}

static uint32_t s_rng = 0x9e3779b9u;
static float frand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)(s_rng >> 8) / 16777216.0f;
}

/* Stable per-cell hash (feather texture that does not shimmer). */
static inline int32_t hash8(int x, int y)
{
    uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return (int32_t)((h >> 16) & 255);
}

/* Small filled ellipse in grid coordinates (eyes, blush, feet). Float is fine: tiny bounding box. */
static void fill_ellipse(float cx, float cy, float rx, float ry, uint8_t col, bool dither, bool mark)
{
    int x0 = (int)floorf(cx - rx), x1 = (int)ceilf(cx + rx);
    int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
    float irx = 1.0f / rx, iry = 1.0f / ry;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float u = ((float)x + 0.5f - cx) * irx, v = ((float)y + 0.5f - cy) * iry;
            float r2 = u * u + v * v;
            if (r2 > 1.0f) {
                continue;
            }
            if (dither && (int)((1.0f - r2) * 16.0f) < bayer(x, y)) {
                continue;
            }
            px(x, y, col);
            if (mark && (unsigned)x < W && (unsigned)y < H) {
                s_mask[y * W + x] = 1;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Time-varying behaviour state (blink, gaze, ear flicks)
 * ------------------------------------------------------------------------- */

static struct {
    float last_t;
    float blink_cd, blink_ph;
    bool again, second;
    float gx, gy, tx, ty, gaze_cd;
    float flick, flick_cd;
    int flick_side;
} s_eyes = { .blink_cd = 2.0f, .blink_ph = -1.0f, .gaze_cd = 1.5f, .flick_cd = 3.0f };

static float eyes_update(const muse_pose_t *p, float dt)
{
    float blink = 0;
    s_eyes.blink_cd -= dt;
    if (s_eyes.blink_ph < 0 && s_eyes.blink_cd <= 0) {
        s_eyes.blink_ph = 0;
        s_eyes.again = s_eyes.second ? false : frand() < 0.25f;
        s_eyes.second = false;
    }
    if (s_eyes.blink_ph >= 0) {
        s_eyes.blink_ph += dt / 0.16f;
        if (s_eyes.blink_ph >= 1.0f) {
            s_eyes.blink_ph = -1.0f;
            if (s_eyes.again) {
                s_eyes.second = true;
                s_eyes.blink_cd = 0.1f;
            } else {
                s_eyes.blink_cd = 2.2f + frand() * 3.0f;
            }
        } else {
            blink = sinf(s_eyes.blink_ph * 3.1416f);
        }
    }

    s_eyes.gaze_cd -= dt;
    if (s_eyes.gaze_cd <= 0) {
        s_eyes.gaze_cd = 1.2f + frand() * 2.4f;
        s_eyes.tx = (frand() - 0.5f) * 2.0f;
        s_eyes.ty = (frand() - 0.5f) * 1.2f;
    }
    float tx = s_eyes.tx, ty = s_eyes.ty;
    if (p->mode == MUSE_MODE_LISTENING || p->mode == MUSE_MODE_ERROR) {
        tx = 0;
        ty = 0;
    } else if (p->mode == MUSE_MODE_THINKING) {
        int phase = (int)(p->mode_t / 0.9f) & 3;
        tx = (phase == 1 || phase == 3) ? 1.0f : -1.0f;
        ty = -1.0f;
        if (phase == 0) {
            tx = 0.0f;
        }
    }
    float k = 1.0f - expf(-dt * 14.0f);
    s_eyes.gx += (tx - s_eyes.gx) * k;
    s_eyes.gy += (ty - s_eyes.gy) * k;

    s_eyes.flick_cd -= dt;
    if (s_eyes.flick_cd <= 0) {
        s_eyes.flick_cd = 3.0f + frand() * 4.0f;
        s_eyes.flick = 1.0f;
        s_eyes.flick_side = frand() < 0.5f ? -1 : 1;
    }
    s_eyes.flick = s_eyes.flick > 0 ? s_eyes.flick - dt * 3.5f : 0;
    return blink;
}

/* ---------------------------------------------------------------------------
 * Background layers
 * ------------------------------------------------------------------------- */

static void draw_aura(float cx, float cy, float radius, float strength)
{
    if (strength <= 0.01f) {
        return;
    }
    int icx = iround(cx), icy = iround(cy), R = (int)radius;
    int32_t R2 = R * R;
    int32_t inv = (1 << 20) / R2;
    int32_t str = QF(clampf(strength, 0, 1.4f));
    int y0 = icy - R < 0 ? 0 : icy - R, y1 = icy + R >= H ? H - 1 : icy + R;
    int x0 = icx - R < 0 ? 0 : icx - R, x1 = icx + R >= W ? W - 1 : icx + R;
    for (int y = y0; y <= y1; y++) {
        int dy = y - icy;
        for (int x = x0; x <= x1; x++) {
            int dx = x - icx;
            int32_t d2 = dx * dx + dy * dy;
            if (d2 >= R2 || s_fb[y * W + x] != C_BG) {
                continue;
            }
            int32_t f = ONE - ((d2 * inv) >> (20 - Q));
            int32_t v = (((f * f) >> Q) * str) >> Q;
            if (((v * 16) >> Q) > bayer(x, y)) {
                s_fb[y * W + x] = v > QF(0.55f) ? C_AURA2 : C_AURA1;
            }
        }
    }
}

static float s_dir[32][2];
static bool s_dir_init;

static void draw_rings(float cx, float cy, float t, float level, float speed)
{
    for (int k = 0; k < 3; k++) {
        float ph = fracf(t * speed + (float)k / 3.0f);
        float r = 14.0f + ph * 19.0f + level * 3.0f;
        int alpha = (int)((1.0f - ph) * 17.0f);
        for (int i = 0; i < 32; i++) {
            if (((i + k) & 1) || alpha <= bayer(i, k * 3)) {
                continue;
            }
            px(iround(cx + s_dir[i][0] * r), iround(cy + s_dir[i][1] * r * 0.82f), ph < 0.35f ? C_ACC : C_AURA2);
        }
    }
}

static void draw_shadow(float cx, float y, float half_w)
{
    int iy = iround(y);
    for (int dy = -1; dy <= 1; dy++) {
        float hw = half_w * (dy == 0 ? 1.0f : 0.72f);
        for (int x = iround(cx - hw); x <= iround(cx + hw); x++) {
            if (s_fb[(iy + dy) * W + x] != C_BG) {
                continue;
            }
            if (dy == 0 || bayer(x, iy + dy) < 8) {
                px(x, iy + dy, C_SHADOW);
            }
        }
    }
}

static void draw_sparkle(int x, int y, int tw, bool big)
{
    px(x, y, C_WHITE);
    if (tw > 0 || big) {
        px(x - 1, y, C_SPK);
        px(x + 1, y, C_SPK);
        px(x, y - 1, C_SPK);
        px(x, y + 1, C_SPK);
    }
}

static void draw_sparkles(const muse_pose_t *p, float cx, float cy, bool front, float speed, int count)
{
    for (int i = 0; i < count && i < 8; i++) {
        bool is_front = (i & 1) != 0;
        if (is_front != front) {
            continue;
        }
        float ph = (float)i * 0.7391f + p->t * 0.35f * speed;
        float ang = ph * TAU;
        float x = cx + cosf(ang) * (24.0f + (float)(i % 3) * 2.0f);
        float y = cy - 4.0f + sinf(ang * 1.0f) * (19.0f + (float)(i % 2) * 3.0f);
        int tw = (int)(2.0f * (0.5f + 0.5f * sinf(p->t * 4.0f + (float)i * 1.9f)));
        draw_sparkle(iround(x), iround(y), tw, false);
    }
}

/* ---------------------------------------------------------------------------
 * Body
 * ------------------------------------------------------------------------- */

typedef struct {
    float cx, cy, a, b;   /* body: centre, half-width, half-height */
    float fx, fy, fa;     /* face disc: centre, half-width */
    float top;
} owl_t;

/* Tone index 0..3 from a Q12 brightness, Bayer-dithered. */
static inline uint8_t tone(int32_t t, int x, int y)
{
    if (t < 0) {
        t = 0;
    }
    int32_t s = t * 3;
    int base = s >> Q;
    int frac = (s & (ONE - 1)) >> 8;
    int idx = base + (frac > bayer(x, y) ? 1 : 0);
    return (uint8_t)(C_BD + (idx > 3 ? 3 : idx));
}

static void draw_ear(const owl_t *o, int side, float perk, float droop, float flick)
{
    float apx = o->cx + side * (o->a * 0.58f + droop * 3.5f + flick * 0.8f);
    float apy = o->top - 4.5f - perk * 3.5f + droop * 4.5f + flick * 1.2f;
    float bx0 = o->cx + side * (o->a * 0.20f);
    float bx1 = o->cx + side * (o->a * 0.90f);
    float by = o->top + 6.0f;
    int ya = iround(apy), yb = iround(by);
    for (int y = ya; y <= yb; y++) {
        float t = (float)(y - ya + 0.5f) / (float)(yb - ya + 1);
        float xa = apx + (bx0 - apx) * t, xb = apx + (bx1 - apx) * t;
        int xl = iround(xa < xb ? xa : xb), xr = iround(xa < xb ? xb : xa);
        for (int x = xl; x <= xr; x++) {
            float u = (xr == xl) ? 0.5f : (float)(x - xl) / (float)(xr - xl);
            float outer = side < 0 ? u : 1.0f - u;   /* 0 near apex-side centre, 1 outer */
            uint8_t c = C_BM;
            if (t > 0.30f && u > 0.28f && u < 0.72f) {
                c = C_EARIN;
            } else if (outer > 0.8f || t < 0.12f) {
                c = C_BD;
            } else if (outer < 0.2f) {
                c = C_BL;
            }
            px(x, y, c);
            if ((unsigned)x < W && (unsigned)y < H) {
                s_mask[y * W + x] = 1;
            }
        }
    }
}

static void draw_body(const owl_t *o)
{
    int y0 = (int)floorf(o->cy - o->b), y1 = (int)ceilf(o->cy + o->b);
    int belly_y = iround(o->fy + o->fa * 0.72f);
    int32_t ib = QF(1.0f / o->b);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= H) {
            continue;
        }
        float v = ((float)y + 0.5f - o->cy) / o->b;
        float v2 = v * v;
        float w4 = 1.0f - v2 * v2;
        if (w4 <= 0.0f) {
            continue;
        }
        float hw = o->a * sqrtf(w4) * (1.0f + 0.09f * v);   /* plump egg: wider below */
        if (hw < 0.8f) {
            continue;
        }
        int xl = (int)floorf(o->cx - hw + 0.5f), xr = (int)floorf(o->cx + hw - 0.5f);
        int32_t ihw = (int32_t)((float)ONE / hw);
        int32_t vq = (int32_t)(((float)y + 0.5f - o->cy) * (float)ib);   /* Q12 */
        int32_t vterm = (vq * QF(0.36f)) >> Q;
        int32_t dxq = (int32_t)(((float)xl + 0.5f - o->cx) * (float)ONE);
        for (int x = xl; x <= xr; x++, dxq += ONE) {
            if ((unsigned)x >= W) {
                continue;
            }
            int32_t u = (dxq * ihw) >> Q;
            int32_t u2 = (u * u) >> Q;
            int32_t u4 = (u2 * u2) >> Q;
            int32_t t = QF(0.46f) - ((u * QF(0.32f)) >> Q) - vterm - ((u4 * QF(0.28f)) >> Q);
            bool belly = y >= belly_y;
            if (belly) {
                t += QF(0.16f);
                /* feather scallops: little arcs every 3 rows */
                int p = (x + (((y - belly_y) / 3) & 1) * 2) & 3;
                if (((y - belly_y) % 3) == 2 && (p == 1 || p == 2)) {
                    t -= QF(0.30f);
                }
            }
            t += (hash8(x, y) - 128) * 4;   /* +-0.03 texture */
            s_fb[y * W + x] = tone(t, x, y);
            s_mask[y * W + x] = 1;
        }
    }
}

static void draw_face_disc(const owl_t *o)
{
    float ry = o->fa * 0.73f;
    int y0 = (int)floorf(o->fy - ry), y1 = (int)ceilf(o->fy + ry);
    int32_t irx = (int32_t)((float)ONE / o->fa), iry = (int32_t)((float)ONE / ry);
    for (int y = y0; y <= y1; y++) {
        if ((unsigned)y >= H) {
            continue;
        }
        int32_t vq = (int32_t)(((float)y + 0.5f - o->fy) * (float)iry);  /* Q12 */
        int32_t v2 = (vq * vq) >> Q;
        int x0 = (int)floorf(o->fx - o->fa), x1 = (int)ceilf(o->fx + o->fa);
        int32_t uq0 = (int32_t)(((float)x0 + 0.5f - o->fx) * (float)irx), uq_step = irx;
        for (int x = x0; x <= x1; x++, uq0 += uq_step) {
            if ((unsigned)x >= W) {
                continue;
            }
            int32_t uq = uq0;
            int32_t r2 = ((uq * uq) >> Q) + v2;
            if (r2 > ONE) {
                continue;
            }
            /* heart-ish top: a shallow notch between the brows */
            if (vq < -QF(0.55f) && uq > -QF(0.14f) && uq < QF(0.14f) && vq < -QF(0.85f)) {
                continue;
            }
            uint8_t c;
            if (r2 > QF(0.80f)) {
                c = C_OUT2;
            } else {
                int32_t t = QF(0.62f) - ((uq * QF(0.12f)) >> Q) - ((vq * QF(0.30f)) >> Q);
                int32_t s = t * 2;
                int base = s >> Q;
                int frac = (s & (ONE - 1)) >> 8;
                int idx = base + (frac > bayer(x, y) ? 1 : 0);
                c = (uint8_t)(C_FACED + (idx > 2 ? 2 : (idx < 0 ? 0 : idx)));
            }
            s_fb[y * W + x] = c;
        }
    }
}

typedef struct {
    float sx, sy, dx, dy;   /* shoulder, unit direction */
} wing_t;

static wing_t wing_ang(const owl_t *o, int side, float ang, float dy_off)
{
    wing_t w;
    w.sx = o->cx + side * (o->a - 1.5f);
    w.sy = o->cy + dy_off;
    w.dx = side * sinf(ang);
    w.dy = cosf(ang);
    return w;
}

static void draw_wing(const wing_t *w, float rx, float ry)
{
    float ccx = w->sx + w->dx * ry * 0.85f, ccy = w->sy + w->dy * ry * 0.85f;
    int32_t dxq = QF(w->dx), dyq = QF(w->dy);
    int32_t irx = (int32_t)((float)ONE / rx), iry = (int32_t)((float)ONE / ry);
    int ext = (int)ceilf(ry) + 1;
    int x0 = iround(ccx) - ext, x1 = iround(ccx) + ext;
    int y0 = iround(ccy) - ext, y1 = iround(ccy) + ext;
    int32_t xs = (int32_t)(((float)x0 + 0.5f - ccx) * (float)ONE);
    int32_t ys = (int32_t)(((float)y0 + 0.5f - ccy) * (float)ONE);
    int32_t iry_s = QF(0.5f) / (int32_t)ry;
    for (int y = y0; y <= y1; y++, ys += ONE) {
        int32_t X = xs;
        for (int x = x0; x <= x1; x++, X += ONE) {
            if ((unsigned)x >= W || (unsigned)y >= H) {
                continue;
            }
            int32_t along = (int32_t)(((int64_t)X * dxq + (int64_t)ys * dyq) >> Q);
            int32_t perp = (int32_t)(((int64_t)-X * dyq + (int64_t)ys * dxq) >> Q);
            int32_t ux = (int32_t)(((int64_t)perp * irx) >> Q);
            int32_t uy = (int32_t)(((int64_t)along * iry) >> Q);
            int32_t r2 = ((ux * ux) >> Q) + ((uy * uy) >> Q);
            if (r2 > ONE) {
                continue;
            }
            uint8_t c;
            if (r2 > QF(0.60f)) {
                c = s_mask[y * W + x] ? C_OUT : C_BD;
            } else {
                int32_t lit = QF(0.58f) - (int32_t)((((int64_t)X * QF(0.55f) >> Q) + ((int64_t)ys * QF(0.7f) >> Q)) * iry_s >> Q);
                /* feather ribs near the tip */
                if (uy > QF(0.20f) && ((((perp >> Q) + 16) & 3) == 0)) {
                    lit -= QF(0.30f);
                }
                c = tone(lit + QF(0.30f), x, y);
            }
            s_fb[y * W + x] = c;
            s_mask[y * W + x] = 1;
        }
    }
}

static void draw_foot(float x, float y)
{
    fill_ellipse(x, y, 3.3f, 1.8f, C_BEAK, false, true);
    px(iround(x - 2), iround(y + 1), C_BEAKD);
    px(iround(x), iround(y + 1), C_BEAKD);
    px(iround(x + 2), iround(y + 1), C_BEAKD);
}

/* Outline pass: 1 px dark border around the silhouette plus a tinted rim on its lit side. */
static void outline_pass(void)
{
    static uint8_t out[W * H];
    memset(out, 0, sizeof(out));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (s_mask[i]) {
                continue;
            }
            bool n = (y > 0 && s_mask[i - W]) || (y < H - 1 && s_mask[i + W]) ||
                     (x > 0 && s_mask[i - 1]) || (x < W - 1 && s_mask[i + 1]);
            if (n) {
                out[i] = 1;
            }
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (out[i]) {
                s_fb[i] = C_OUT;
            } else if (s_mask[i] && x > 0 && y > 0 && !s_mask[i - W - 1] && s_mask[i] &&
                       s_fb[i] >= C_BL && s_fb[i] <= C_BH && ((x + y) % 3) == 0) {
                s_fb[i] = C_RIM;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Face
 * ------------------------------------------------------------------------- */

typedef enum { EYES_NORMAL, EYES_WIDE, EYES_HAPPY, EYES_X } eye_style_t;
typedef enum { BEAK_CLOSED, BEAK_O, BEAK_HMM, BEAK_TALK, BEAK_FLAT, BEAK_GRIN } beak_t;

static void draw_eye(float ex, float ey, float open, eye_style_t style, float gx, float gy)
{
    int x = iround(ex), y = iround(ey);
    if (style == EYES_HAPPY) {
        px(x - 2, y + 1, C_IRIS); px(x - 1, y, C_IRIS); px(x, y - 1, C_IRIS);
        px(x + 1, y, C_IRIS); px(x + 2, y + 1, C_IRIS);
        px(x - 1, y + 1, C_IRIS); px(x + 1, y + 1, C_IRIS);
        return;
    }
    if (style == EYES_X) {
        for (int i = -2; i <= 2; i++) {
            px(x + i, y + i, C_IRIS);
            px(x + i, y - i, C_IRIS);
        }
        px(x - 1, y, C_IRIS); px(x + 1, y, C_IRIS);
        return;
    }
    float rx = style == EYES_WIDE ? 3.2f : 2.5f;
    float ry = (style == EYES_WIDE ? 3.7f : 3.0f) * open;
    float cx = ex + gx * 0.9f, cy = ey + gy * 0.7f;
    if (ry < 0.7f) {
        for (int i = -2; i <= 2; i++) {
            px(x + i, y + 1, C_IRIS);
        }
        return;
    }
    fill_ellipse(cx, cy, rx, ry, C_IRIS, false, false);
    if (ry > 1.8f) {
        px(iround(cx - 0.9f), iround(cy - ry * 0.45f), C_WHITE);
        if (rx > 3.0f) {
            px(iround(cx - 0.9f) + 1, iround(cy - ry * 0.45f), C_WHITE);
        }
        px(iround(cx + 0.8f), iround(cy + ry * 0.55f), C_G1);
    }
}

static void draw_blush(int x, int y, float strength)
{
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int d = abs(dx) + abs(dy) * 2;
            if (d > 3) {
                continue;
            }
            if ((int)((1.0f - (float)d / 4.0f) * 16.0f * strength) > bayer(x + dx, y + dy)) {
                px(x + dx, y + dy, d < 2 ? C_BLUSHD : C_BLUSH);
            }
        }
    }
}

static void draw_beak(int x, int y, beak_t m, float open)
{
    switch (m) {
    case BEAK_FLAT:
        for (int i = -2; i <= 2; i++) {
            px(x + i, y + 1, C_BEAKD);
        }
        px(x - 1, y, C_BEAK); px(x, y, C_BEAK); px(x + 1, y, C_BEAK);
        return;
    case BEAK_HMM:
        x += 1;
        px(x - 1, y, C_BEAK); px(x, y, C_BEAK); px(x + 1, y, C_BEAK);
        px(x, y + 1, C_BEAKD);
        px(x + 2, y, C_BEAKD);
        return;
    default:
        break;
    }
    int gap = 0;
    if (m == BEAK_O) {
        gap = 1;
    } else if (m == BEAK_TALK) {
        gap = (int)(open * 3.0f + 0.5f);
        if (gap > 3) {
            gap = 3;
        }
    } else if (m == BEAK_GRIN) {
        gap = 2;
    }
    px(x - 1, y, C_BEAK); px(x, y, C_BEAK); px(x + 1, y, C_BEAK);
    px(x - 1, y, C_BEAK);
    if (m == BEAK_GRIN || gap >= 2) {
        px(x - 2, y, C_BEAKD); px(x + 2, y, C_BEAKD);
    }
    int ly = y + 1;
    for (int g = 0; g < gap; g++, ly++) {
        int hw = (gap >= 2) ? 1 : 0;
        for (int i = -hw; i <= hw; i++) {
            px(x + i, ly, (g == gap - 1 && gap >= 2 && i == 0) ? C_TONGUE : C_MOUTH);
        }
    }
    px(x, ly, C_BEAKD);
    if (gap >= 2) {
        px(x - 1, ly, C_BEAKD); px(x + 1, ly, C_BEAKD);
    }
}

static void draw_brow(int x, int y, int side, int tilt)
{
    /* feather tuft: three pixels sloping toward the outer edge */
    px(x - side, y + (tilt > 0 ? 0 : 0), C_BROW);
    px(x, y - (tilt > 0 ? 1 : 0), C_BROW);
    px(x + side, y - tilt, C_BROW);
}

/* The evening star on the forehead. size 0..3. */
static void draw_star(int cx, int cy, float size, const owl_t *o)
{
    int s = (int)(size + 0.5f);
    /* soft halo on the feathers */
    if (size > 0.4f) {
        int rad = 3 + s;
        for (int dy = -rad; dy <= rad; dy++) {
            for (int dx = -rad; dx <= rad; dx++) {
                int d2 = dx * dx + dy * dy;
                int x = cx + dx, y = cy + dy;
                if (d2 > rad * rad || (unsigned)x >= W || (unsigned)y >= H || !s_mask[y * W + x]) {
                    continue;
                }
                if (s_fb[y * W + x] > C_BH || s_fb[y * W + x] < C_BD) {
                    continue;
                }
                if ((int)((1.0f - (float)d2 / (float)(rad * rad)) * 12.0f) > bayer(x, y)) {
                    px(x, y, C_G3);
                }
            }
        }
    }
    (void)o;
    px(cx, cy, C_WHITE);
    if (s >= 1) {
        px(cx - 1, cy, C_G0); px(cx + 1, cy, C_G0); px(cx, cy - 1, C_G0); px(cx, cy + 1, C_G0);
    }
    if (s >= 2) {
        px(cx - 2, cy, C_G1); px(cx + 2, cy, C_G1); px(cx, cy - 2, C_G1); px(cx, cy + 2, C_G1);
        px(cx - 1, cy - 1, C_G2); px(cx + 1, cy - 1, C_G2); px(cx - 1, cy + 1, C_G2); px(cx + 1, cy + 1, C_G2);
    }
    if (s >= 3) {
        px(cx - 3, cy, C_G2); px(cx + 3, cy, C_G2); px(cx, cy - 3, C_G2); px(cx, cy + 3, C_G2);
    }
}

static const char *const HEART[] = { ".#.#.", "#####", ".###.", "..#.." };

static void draw_heart(int x, int y, float fade)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 5; c++) {
            if (HEART[r][c] == '#' && (int)(fade * 16.0f) > bayer(x + c, y + r)) {
                px(x + c - 2, y + r, (r == 0 && c == 1) ? C_WHITE : C_HEART);
            }
        }
    }
}

static void draw_hearts(float cx, float top, float t, float amount)
{
    for (int i = 0; i < 2; i++) {
        float ph = fracf(t * 0.8f + (float)i * 0.5f);
        float x = cx + (i ? 17.0f : -17.0f) + sinf(t * 3.0f + (float)i * 2.0f) * 1.5f;
        draw_heart(iround(x), iround(top + 14.0f - ph * 16.0f), (ph < 0.7f ? 1.0f : (1.0f - ph) / 0.3f) * clampf(amount * 2.0f, 0, 1));
    }
}

static void draw_waves(float cx, float cy, float body_a, float level, float t)
{
    int n = 1 + (int)(level * 3.0f);
    if (n > 3) {
        n = 3;
    }
    for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k < n; k++) {
            float r = body_a + 4.0f + (float)k * 3.0f + 0.6f * sinf(t * 9.0f + (float)k);
            int hh = 2 + k;
            for (int dy = -hh; dy <= hh; dy++) {
                float xx = sqrtf(r * r + (float)(dy * dy) * 2.0f) ;
                px(iround(cx + side * xx), iround(cy + dy), k == 0 ? C_ACC : C_AURA2);
            }
        }
    }
}

static void draw_thought_dots(float x, float y, float t)
{
    int active = (int)(t * 2.5f) % 4;
    for (int i = 0; i < 3; i++) {
        int dx = iround(x + (float)i * 3.5f), dy = iround(y - (float)i * 3.5f);
        bool on = active > i;
        int sz = i == 2 ? 2 : 1;
        for (int yy = 0; yy < sz + 1; yy++) {
            for (int xx = 0; xx < sz + 1; xx++) {
                px(dx + xx, dy + yy, on ? (active == i + 1 ? C_WHITE : C_ACC) : C_AURA2);
            }
        }
    }
}

static void draw_alert(int x, int y)
{
    for (int yy = 0; yy < 5; yy++) {
        px(x, y + yy, C_ACC);
        px(x + 1, y + yy, C_ACC);
    }
    px(x, y + 6, C_ACC);
    px(x + 1, y + 6, C_ACC);
    px(x, y, C_WHITE);
}

/* ---------------------------------------------------------------------------
 * Blitter
 * ------------------------------------------------------------------------- */

#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px_size)
{
    s_size = px_size < MAP_MAX ? px_size : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, (size_t)n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

/* ---------------------------------------------------------------------------
 * Frame
 * ------------------------------------------------------------------------- */

void muse_pixel_render(const muse_pose_t *p)
{
    if (!s_dir_init) {
        for (int i = 0; i < 32; i++) {
            s_dir[i][0] = cosf((float)i * TAU / 32.0f);
            s_dir[i][1] = sinf((float)i * TAU / 32.0f);
        }
        s_dir_init = true;
    }
    float dt = s_eyes.last_t > 0 ? clampf(p->t - s_eyes.last_t, 0, 0.2f) : 0.04f;
    s_eyes.last_t = p->t;

    muse_mode_t mode = p->mode;
    float fade = mode == MUSE_MODE_OFF ? clampf(1.0f - p->mode_t / 1.3f, 0, 1) : 1.0f;
    float happy = mode == MUSE_MODE_ERROR ? 0.0f : p->happy;
    float level = clampf(p->level, 0, 1);
    float t = p->t;

    update_palette(&SCHEMES[(unsigned)mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE], dt);
    float blink = eyes_update(p, dt);

    memset(s_fb, C_BG, sizeof(s_fb));
    memset(s_mask, 0, sizeof(s_mask));

    /* ---- body motion ---- */
    float bob, lean = 0, hop = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        bob = sinf(t * 3.0f) * 0.5f;
        lean = 0.0f;
        break;
    case MUSE_MODE_THINKING:
        bob = sinf(t * 2.4f) * 0.7f;
        lean = sinf(t * 1.3f) * 1.3f;
        break;
    case MUSE_MODE_SPEAKING:
        bob = sinf(t * 5.0f) * 0.6f - level * 1.6f;
        break;
    case MUSE_MODE_ERROR:
        bob = 1.0f;
        lean = sinf(t * 18.0f) * (p->mode_t < 0.6f ? 1.4f : 0.0f);
        break;
    default:
        bob = sinf(t * 1.8f) * 1.0f;
        break;
    }
    if (happy > 0) {
        hop = fabsf(sinf(t * 9.0f)) * 3.0f * happy;
    }

    float boot = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.4f, 0, 1) : 1.0f;
    float pop = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 0.6f, 0, 1) : 1.0f;
    float squash = 1.0f - (1.0f - pop) * 0.35f + sinf(pop * 3.1416f) * 0.06f;
    float breathe = sinf(t * 2.0f + 1.0f) * 0.03f;

    owl_t o;
    o.a = 15.0f * (1 + breathe) * (2.0f - squash) + level * 0.7f;
    o.b = 22.0f * (1 - breathe) * squash;
    o.cx = 32.0f + lean;
    o.cy = 56.0f - o.b + bob * 0.5f - hop;
    o.top = o.cy - o.b;
    o.fa = o.a * 0.80f;
    o.fx = o.cx + lean * 0.25f;
    o.fy = o.top + o.b * 0.80f;

    /* ---- background ---- */
    float aura_r = 29.0f + level * 4.0f + sinf(t * 1.5f) * 1.0f;
    draw_aura(o.cx, o.cy - 3, aura_r, (0.75f * boot + level * 0.4f) * fade);
    if (mode == MUSE_MODE_LISTENING) {
        draw_rings(o.cx, o.fy + 2, t, level, 0.9f);
    } else if (mode == MUSE_MODE_SPEAKING) {
        draw_rings(o.cx, o.fy + 2, t, level, 0.6f);
    }
    draw_shadow(o.cx, 58.5f, 13.0f - hop * 0.8f);

    float spk_speed = mode == MUSE_MODE_THINKING ? 2.8f : mode == MUSE_MODE_LISTENING ? 1.2f
                    : mode == MUSE_MODE_SPEAKING ? 1.5f : 0.6f;
    int spk_count = mode == MUSE_MODE_BOOT ? (int)(boot * 8) : (int)(8 * fade);
    draw_sparkles(p, o.cx, o.cy, false, spk_speed, spk_count);

    /* ---- character ---- */
    float base = o.cy + o.b;
    float step = mode == MUSE_MODE_SPEAKING ? sinf(t * 5.0f) * 0.6f : 0.0f;
    float hf = happy > 0 ? hop * 0.3f : 0.0f;
    draw_foot(o.cx - 6.0f, base - 0.2f + (happy > 0 ? hf : step));
    draw_foot(o.cx + 6.0f, base - 0.2f + (happy > 0 ? hf : -step));

    /* ears */
    float perk = 0.0f, droop = 0.0f;
    if (mode == MUSE_MODE_LISTENING) {
        perk = 1.0f;
    } else if (mode == MUSE_MODE_ERROR) {
        droop = 1.0f;
    } else if (mode == MUSE_MODE_OFF) {
        droop = 1.0f - fade;
    } else if (mode == MUSE_MODE_THINKING) {
        perk = 0.3f;
    }
    if (happy > 0) {
        perk = 0.8f * happy;
    }
    draw_ear(&o, -1, perk, droop, s_eyes.flick_side < 0 ? s_eyes.flick : 0.0f);
    draw_ear(&o, 1, perk + (mode == MUSE_MODE_THINKING ? 0.6f : 0.0f), droop, s_eyes.flick_side > 0 ? s_eyes.flick : 0.0f);

    draw_body(&o);
    draw_face_disc(&o);

    /* wings */
    float sway = sinf(t * 1.8f + 0.6f) * 0.08f;
    wing_t wl, wr;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        wl = wing_ang(&o, -1, 2.55f + sinf(t * 3.0f) * 0.05f, 2.0f);
        wr = wing_ang(&o, 1, 2.55f - sinf(t * 3.0f) * 0.05f, 2.0f);
        break;
    case MUSE_MODE_THINKING: {
        wl = wing_ang(&o, -1, 0.25f + sway, 2.0f);
        /* right wing folds across the chest up to the chin */
        wr = wing_ang(&o, 1, 0.0f, 2.0f);
        float tx = o.cx + 3.0f - wr.sx, ty = o.fy + o.fa * 0.78f - wr.sy;
        float len = sqrtf(tx * tx + ty * ty);
        wr.dx = tx / len;
        wr.dy = ty / len;
        break;
    }
    case MUSE_MODE_SPEAKING: {
        float w = sinf(t * 7.0f) * (0.2f + level * 0.5f);
        wl = wing_ang(&o, -1, 0.55f + w, 2.0f);
        wr = wing_ang(&o, 1, 0.55f - w, 2.0f);
        break;
    }
    case MUSE_MODE_OFF: {
        float w = sinf(t * 12.0f) * 0.35f * fade;
        wl = wing_ang(&o, -1, 0.25f, 2.0f);
        wr = wing_ang(&o, 1, 2.4f + w, 2.0f);
        break;
    }
    case MUSE_MODE_ERROR:
        wl = wing_ang(&o, -1, 0.10f, 2.0f);
        wr = wing_ang(&o, 1, 0.10f, 2.0f);
        break;
    default:
        if (happy > 0) {
            float wig = sinf(t * 14.0f) * 0.25f;
            wl = wing_ang(&o, -1, 2.6f + wig, 2.0f);
            wr = wing_ang(&o, 1, 2.6f + wig, 2.0f);
        } else {
            wl = wing_ang(&o, -1, 0.22f + sway, 2.0f);
            wr = wing_ang(&o, 1, 0.22f - sway, 2.0f);
        }
        break;
    }
    draw_wing(&wl, 4.2f, 9.5f);
    draw_wing(&wr, 4.2f, 9.5f);

    outline_pass();

    /* ---- face ---- */
    float eye_y = o.fy - 0.5f;
    float eye_dx = o.fa * 0.46f;
    eye_style_t style = EYES_NORMAL;
    float open = 1.0f - blink;
    beak_t beak = BEAK_CLOSED;
    float beak_open = 0;

    switch (mode) {
    case MUSE_MODE_BOOT:
        open = p->mode_t < 0.9f ? 0.0f : clampf((p->mode_t - 0.9f) / 0.3f, 0, 1);
        break;
    case MUSE_MODE_LISTENING:
        style = EYES_WIDE;
        beak = BEAK_O;
        break;
    case MUSE_MODE_THINKING:
        open *= 0.85f;
        beak = BEAK_HMM;
        break;
    case MUSE_MODE_SPEAKING:
        beak = BEAK_TALK;
        beak_open = level * 1.3f + 0.12f * (0.5f + 0.5f * sinf(t * 22.0f));
        break;
    case MUSE_MODE_ERROR:
        style = EYES_X;
        beak = BEAK_FLAT;
        break;
    case MUSE_MODE_OFF:
        open = clampf((1.0f - p->mode_t / 1.0f) * 1.5f, 0, 1);
        break;
    default:
        break;
    }
    if (happy > 0.2f) {
        style = EYES_HAPPY;
        beak = BEAK_GRIN;
    }

    draw_eye(o.fx - eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);
    draw_eye(o.fx + eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);

    int bl = iround(o.fx - eye_dx), br = iround(o.fx + eye_dx), by = iround(eye_y) - 5;
    if (mode == MUSE_MODE_THINKING) {
        draw_brow(bl, by + 1, -1, 0);
        draw_brow(br, by - 1, 1, 1);
    } else if (mode == MUSE_MODE_LISTENING) {
        draw_brow(bl, by - 1, -1, 1);
        draw_brow(br, by - 1, 1, 1);
    } else if (mode == MUSE_MODE_ERROR) {
        draw_brow(bl, by, -1, -1);
        draw_brow(br, by, 1, -1);
    } else {
        draw_brow(bl, by + 1, -1, 0);
        draw_brow(br, by + 1, 1, 0);
    }

    float blush = 0.5f + happy * 0.5f + (mode == MUSE_MODE_SPEAKING ? 0.15f : 0.0f) + (mode == MUSE_MODE_ERROR ? 0.2f : 0.0f);
    draw_blush(iround(o.fx - o.fa * 0.74f), iround(eye_y + 3), blush);
    draw_blush(iround(o.fx + o.fa * 0.74f), iround(eye_y + 3), blush);
    draw_beak(iround(o.fx), iround(eye_y + 3.6f), beak, beak_open);

    /* ---- forehead star ---- */
    float star = 1.6f + 0.9f * sinf(t * 2.0f);
    if (mode == MUSE_MODE_THINKING) {
        star = 1.5f + 1.4f * sinf(t * 7.0f);
    } else if (mode == MUSE_MODE_LISTENING) {
        star = 1.8f + level * 1.2f + 0.4f * sinf(t * 3.0f);
    } else if (mode == MUSE_MODE_SPEAKING) {
        star = 1.5f + level * 2.0f;
    } else if (mode == MUSE_MODE_ERROR) {
        star = p->mode_t < 0.6f ? 2.0f : 0.8f + (fracf(t * 2.5f) < 0.5f ? 0.0f : 0.8f);
    } else if (mode == MUSE_MODE_BOOT) {
        star = p->mode_t < 1.1f ? 0.0f : clampf((p->mode_t - 1.1f) * 5.0f, 0, 1) * (2.0f + sinf(p->mode_t * 9.0f));
    } else if (mode == MUSE_MODE_OFF) {
        star *= fade;
    }
    if (happy > 0) {
        star = 2.0f + happy * 1.6f;
    }
    draw_star(iround(o.cx), (int)(o.top + 6.0f), clampf(star, 0, 3.4f), &o);

    /* ---- foreground ---- */
    draw_sparkles(p, o.cx, o.cy, true, spk_speed, spk_count);
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_waves(o.cx, o.fy + 1, o.a, level, t);
    }
    if (mode == MUSE_MODE_THINKING) {
        draw_thought_dots(o.cx + 16.0f, o.top + 4.0f, t);
    }
    if (happy > 0) {
        draw_hearts(o.cx, o.top + 1.0f, t, happy);
    }
    if (mode == MUSE_MODE_ERROR) {
        draw_alert(iround(o.cx + 19.0f), iround(o.top - 1.0f));
    }
}
