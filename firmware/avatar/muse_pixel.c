// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * VESPER -- "Iconic": a severe cyborg face.
 *
 * Front-facing woman, reduced to hard graphic shapes: silver-gray hair cut in
 * angular swept planes, a cool gray face with a flat-line mouth and one natural
 * blue eye under a straight, heavy brow. The other eye is the focal point: a
 * large square cybernetic eye -- concentric square rings in a metal housing
 * with gold contact pins -- set into a metal plate on that temple. A circuit
 * trace runs from the eye down the cheek. Dark navy collar with gold accents,
 * solid black background.
 *
 * The cybernetic eye, its halo and the circuit traces take the colour of the
 * current mode: gold idle, cyan listening, magenta thinking, mint speaking,
 * red error (white/blue boot, dusk violet off). Listening: the eye brightens
 * and a scan line sweeps it. Thinking: pulses run along the traces and the
 * rings step inward. Speaking: the mouth parts with `level` and the eye core
 * flares with it. Error: red, the eye glitches. Boot: the face resolves out of
 * the dark and the eye ignites from the core outward. Off: the eye collapses to
 * its core and goes dark, the natural eye closes. The happy overlay is a brief
 * acknowledge flare (square pulse off the eye, a slight narrowing of the
 * natural eye) -- no smile.
 *
 * This replaces the evening-star owl (task 04), which is retired. Original art
 * for the Vesper node: only the muse_pixel.h contract and the generic
 * set_size/scale blitter follow the SDK's default renderer (Apache-2.0, Meta
 * Platforms); the character itself does not.
 *
 * Cost: the static face (everything that does not animate) is rasterised once
 * into a 4 KiB base plane on the first frame; each frame is a memcpy of that
 * plane plus a few hundred pixels of eye, traces, mouth and overlays. Per-pixel
 * glow and dither work is Q12 fixed point; floats are used per frame only.
 * Static memory only (3 x 4 KiB planes + palettes + a 512-byte map), no
 * allocation, no bitmaps: every shape is a polygon or a rule below.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_OUT,       /* outline / deepest shadow */
    C_SK0,       /* skin, dark ... (contiguous ramp) */
    C_SK1,
    C_SK2,
    C_SK3,       /* ... skin, lit */
    C_HR0,       /* hair, dark ... (contiguous ramp) */
    C_HR1,
    C_HR2,
    C_HR3,       /* ... hair, lit */
    C_MT0,       /* metal, dark ... (contiguous ramp) */
    C_MT1,
    C_MT2,
    C_MT3,       /* ... metal, lit */
    C_SCL,       /* natural eye: sclera */
    C_IRIS,
    C_IRISL,
    C_PUPIL,
    C_BROW,
    C_LIP,
    C_NV0,       /* collar navy, dark ... */
    C_NV1,
    C_NV2,       /* ... lit */
    C_GD,        /* gold */
    C_GDD,       /* gold, dark */
    C_G3,        /* mode glow ramp, deep ... (contiguous: G3 < G2 < G1 < G0) */
    C_G2,
    C_G1,
    C_G0,        /* ... white-hot */
    C_ACC,       /* mode accent */
    C_GSK,       /* glow spilling on skin */
    C_GMT,       /* glow spilling on metal */
    C_COUNT,
};

typedef struct {
    float r, g, b;
} rgb_t;

typedef struct {
    uint32_t f[4];
    uint32_t acc;
} scheme_t;

/* The accent scheme the owl used, unchanged: f[0] bright ... f[3] deep. */
static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xcfe0ff, 0x8fa8ff, 0x5a5fe0 }, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { { 0xfff6d2, 0xffd96b, 0xffb42e, 0xb86a10 }, 0xffc94d },  /* gold */
    [MUSE_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { { 0xffe6ff, 0xff9cf0, 0xd35bff, 0x7a2bd9 }, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [MUSE_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [MUSE_MODE_OFF]       = { { 0xd8d4ff, 0x8f86d9, 0x5a4fb0, 0x2e2870 }, 0x7c72d0 },
};

static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x07080c,
    [C_SK0] = 0x38363a,
    [C_SK1] = 0x5c595c,
    [C_SK2] = 0x85817f,
    [C_SK3] = 0xa7a29e,
    [C_HR0] = 0x4a4e57,
    [C_HR1] = 0x81868f,
    [C_HR2] = 0xb3b8c0,
    [C_HR3] = 0xdfe2e7,
    [C_MT0] = 0x15171c,
    [C_MT1] = 0x353943,
    [C_MT2] = 0x5a606c,
    [C_MT3] = 0x99a1ae,
    [C_SCL] = 0xa9adb5,
    [C_IRIS] = 0x2a9ad0,
    [C_IRISL] = 0x8fe2ff,
    [C_PUPIL] = 0x081018,
    [C_BROW] = 0x1d1e23,
    [C_LIP] = 0x2b2a2e,
    [C_NV0] = 0x0c1426,
    [C_NV1] = 0x16223f,
    [C_NV2] = 0x2a3d6a,
    [C_GD] = 0xc89b4e,
    [C_GDD] = 0x7d5f2c,
};

static rgb_t s_scheme[5];
static bool s_scheme_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint8_t s_fb[W * H];     /* the frame */
static uint8_t s_base[W * H];   /* the static face, rasterised once */
static uint8_t s_reg[W * H];    /* region id per pixel (what the base pixel is) */
static bool s_base_init;

enum { R_BG = 0, R_FACE, R_HAIR, R_PLATE, R_EAR, R_NECK, R_COLLAR, R_JACKET, R_EYE };

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

/* true with probability v (Q12, 0..ONE) on an ordered-dither pattern */
static inline bool dith(int32_t v, int x, int y)
{
    return ((v * 16) >> Q) > bayer(x, y);
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
    pal[C_GSK] = mix(pal[C_SK2], acc, 0.42f);
    pal[C_GMT] = mix(pal[C_MT1], acc, 0.38f);

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

static inline void bpx(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_base[y * W + x] = c;
    }
}

static inline uint8_t reg_at(int x, int y)
{
    return ((unsigned)x < W && (unsigned)y < H) ? s_reg[y * W + x] : R_BG;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fracf(float v)
{
    return v - floorf(v);
}

static inline int32_t clampq(int32_t v)
{
    return v < 0 ? 0 : (v > ONE ? ONE : v);
}

static uint32_t s_rng = 0x9e3779b9u;
static float frand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)(s_rng >> 8) / 16777216.0f;
}

static inline int32_t hash8(int x, int y)
{
    uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return (int32_t)((h >> 16) & 255);
}

/*
 * Even-odd scanline polygon fill into the region plane, sampled at pixel centres.
 * Integer only: vertices are whole grid units, rows are sampled at 2x scale
 * (centre of row y is 2y+1), and crossings are kept as exact fractions.
 */
typedef struct {
    int8_t x, y;
} pt_t;

#define POLY_MAX 32

static void fill_poly(const pt_t *v, int n, uint8_t region)
{
    int ymin = H, ymax = -1;
    for (int i = 0; i < n; i++) {
        ymin = v[i].y < ymin ? v[i].y : ymin;
        ymax = v[i].y > ymax ? v[i].y : ymax;
    }
    ymin = ymin < 0 ? 0 : ymin;
    ymax = ymax > H ? H : ymax;
    for (int y = ymin; y < ymax; y++) {
        int py = 2 * y + 1;
        int xs[POLY_MAX];   /* first pixel column right of each crossing */
        int nx = 0;
        for (int i = 0, j = n - 1; i < n && nx < POLY_MAX; j = i++) {
            int yi = 2 * v[i].y, yj = 2 * v[j].y;
            if ((yi > py) == (yj > py)) {
                continue;
            }
            /* crossing at x2 = 2*xi + (2*xj - 2*xi) * (py - yi) / (yj - yi), in 2x units;
             * pixel x is right of it when 2x+1 >= x2, i.e. x >= ceil((x2 - 1) / 2) */
            int num = 2 * v[i].x * (yj - yi) + (2 * v[j].x - 2 * v[i].x) * (py - yi) - (yj - yi);
            int den = 2 * (yj - yi);
            if (den < 0) {
                num = -num;
                den = -den;
            }
            int c = num >= 0 ? (num + den - 1) / den : -((-num) / den);
            /* insertion sort: a handful of crossings per row */
            int k = nx++;
            while (k > 0 && xs[k - 1] > c) {
                xs[k] = xs[k - 1];
                k--;
            }
            xs[k] = c;
        }
        for (int k = 0; k + 1 < nx; k += 2) {
            int a = xs[k] < 0 ? 0 : xs[k], b = xs[k + 1] > W ? W : xs[k + 1];
            for (int x = a; x < b; x++) {
                s_reg[y * W + x] = region;
            }
        }
    }
}

/* Bresenham line into the base plane. */
static void bline(int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        bpx(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Geometry (grid units; the face is a fixed front-facing bust)
 * ------------------------------------------------------------------------- */

/* Cybernetic eye: square housing centred on (EX, EY), Chebyshev radius 0..ER. */
#define EX 23
#define EY 27
#define ER 6

/* Natural eye */
#define NX 39
#define NY 28

static const pt_t P_JACKET[] = {
    { 0, 64 }, { 0, 56 }, { 9, 52 }, { 20, 50 }, { 44, 50 }, { 55, 52 }, { 64, 56 }, { 64, 64 },
};
static const pt_t P_NECK[] = {
    { 27, 40 }, { 37, 40 }, { 37, 52 }, { 34, 64 }, { 30, 64 }, { 27, 52 },
};
static const pt_t P_COLLAR_L[] = {
    { 14, 44 }, { 25, 43 }, { 28, 52 }, { 31, 64 }, { 4, 64 }, { 6, 56 },
};
static const pt_t P_COLLAR_R[] = {
    { 39, 43 }, { 50, 44 }, { 58, 56 }, { 60, 64 }, { 33, 64 }, { 36, 52 },
};
static const pt_t P_FACE[] = {
    { 19, 10 }, { 45, 10 }, { 45, 31 }, { 43, 37 }, { 37, 46 }, { 34, 47 }, { 30, 47 }, { 27, 46 }, { 21, 37 }, { 19, 31 },
};
static const pt_t P_EAR_R[] = {
    { 45, 25 }, { 48, 25 }, { 49, 29 }, { 48, 34 }, { 45, 36 },
};
static const pt_t P_EAR_L[] = {
    { 16, 26 }, { 19, 26 }, { 19, 36 }, { 17, 35 }, { 15, 30 },
};
static const pt_t P_HAIR[] = {
    /* angular swept silhouette: a few hard spikes along the crown, a heavy fall on the right */
    { 12, 18 }, { 10, 12 }, { 13, 10 }, { 12, 6 }, { 16, 5 }, { 17, 2 }, { 21, 2 }, { 24, 0 }, { 29, 1 },
    { 33, 0 }, { 39, 1 }, { 43, 0 }, { 46, 3 }, { 50, 4 }, { 50, 8 }, { 53, 11 }, { 52, 15 }, { 54, 19 },
    { 51, 21 }, { 52, 26 }, { 49, 28 }, { 48, 33 }, { 46, 29 }, { 46, 21 }, { 44, 17 }, { 40, 14 },
    { 36, 13 }, { 31, 13 }, { 27, 14 }, { 22, 16 }, { 16, 17 },
};
static const pt_t P_PLATE[] = {
    { 12, 15 }, { 16, 10 }, { 22, 8 }, { 26, 10 }, { 27, 15 }, { 26, 19 }, { 21, 20 }, { 17, 22 },
    { 16, 30 }, { 17, 36 }, { 14, 36 }, { 12, 30 }, { 11, 22 },
};

/* Circuit traces in the accent colour, as polylines. Pulses run along them in order. */
static const pt_t T_CHEEK[] = { { 19, 34 }, { 19, 36 }, { 21, 38 }, { 21, 40 }, { 24, 43 }, { 24, 45 }, { 26, 47 }, { 26, 49 } };
static const pt_t T_PLATE1[] = { { 16, 27 }, { 14, 25 }, { 14, 20 }, { 17, 17 }, { 22, 17 } };
static const pt_t T_PLATE2[] = { { 16, 12 }, { 20, 12 }, { 22, 14 }, { 25, 14 } };

#define TRACE_MAX 48
typedef struct {
    int8_t x[TRACE_MAX], y[TRACE_MAX];
    int n;
} trace_t;

static trace_t s_tr[3];

static void trace_build(trace_t *t, const pt_t *v, int n)
{
    t->n = 0;
    for (int i = 0; i + 1 < n; i++) {
        int x0 = v[i].x, y0 = v[i].y, x1 = v[i + 1].x, y1 = v[i + 1].y;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            bool dup = t->n > 0 && t->x[t->n - 1] == x0 && t->y[t->n - 1] == y0;
            if (!dup && t->n < TRACE_MAX) {
                t->x[t->n] = (int8_t)x0;
                t->y[t->n] = (int8_t)y0;
                t->n++;
            }
            if (x0 == x1 && y0 == y1) {
                break;
            }
            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

#define NPTS(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---------------------------------------------------------------------------
 * The static base: rasterised once
 * ------------------------------------------------------------------------- */

static uint8_t ramp(uint8_t c0, int32_t t)
{
    /* t in Q12, 0..ONE -> c0 + 0..3, rounded: flat planes, no dither noise */
    t = clampq(t);
    int idx = (t * 3 + ONE / 2) >> Q;
    return (uint8_t)(c0 + (idx > 3 ? 3 : idx));
}

static uint8_t shade_face(int x, int y)
{
    /* Flat planes, lit from the upper right; the implant side sits in shadow. */
    int32_t t = QF(0.62f);
    if (x < 24 && y > 33) {
        t -= QF(0.22f);   /* cheek under the implant */
    }
    if (x >= 35 && y >= 31 && x + y >= 70 && x + y <= 76) {
        t += QF(0.25f);   /* right cheekbone: a lit diagonal plane above the hollow */
    }
    /* hollow under the right cheekbone: a hard diagonal plane */
    if (x + y >= 77 && x + y <= 79 && y >= 33 && y <= 39) {
        t -= QF(0.30f);
    }
    /* jaw planes: darken the outer two columns of the jaw */
    if (y > 36) {
        uint8_t l2 = reg_at(x - 2, y), r2 = reg_at(x + 2, y);
        if (l2 != R_FACE || r2 != R_FACE) {
            t -= QF(0.26f);
        }
    }
    if (y >= 45) {
        t -= QF(0.20f);   /* chin underside */
    }
    return ramp(C_SK0, t);
}

static uint8_t shade_hair(int x, int y)
{
    /* Angular swept planes: bands perpendicular to a sweep up and to the right. */
    static const int8_t PLANE[8] = { 2, 3, 1, 3, 2, 1, 3, 2 };   /* tone per plane, neighbours differ */
    /*
     * A hard part runs from the crown (31,2) down to the right temple (46,19). Left of it
     * the planes sweep up and to the right (slope 1/2: clean 2-px stair steps); right of it
     * they are swept back parallel to the part and fall down the side.
     */
    int side = 17 * (x - 31) - 15 * (y - 2);
    int p, bw, band;
    if (side > 0) {
        p = 17 * x - 15 * y + 1024;
        bw = 66;              /* ~3 px bands */
        band = p / bw + 3;
    } else {
        p = x + 2 * y;
        bw = 7;
        band = p / bw;
    }
    int tone = PLANE[band & 7];
    if (y <= 4 && tone < 3) {
        tone++;           /* crown catches the light */
    }
    if (side > 0 && y > 16 && tone > 1) {
        tone--;           /* side fall in shadow */
    }
    if (side <= 0 ? (p % bw == 0 && (band & 1)) : (p % bw < 22 && (band & 1))) {
        return C_HR0;     /* hard edge between planes */
    }
    return (uint8_t)(C_HR0 + tone);
}

static uint8_t shade_plate(int x, int y)
{
    /* two facets split by a seam running down-left from the top edge */
    int s = 3 * x - 2 * y;   /* seam at s == 30 */
    if (s >= 29 && s <= 31) {
        return C_MT0;
    }
    uint8_t up = reg_at(x, y - 1), rt = reg_at(x + 1, y);
    if ((up != R_PLATE && up != R_EYE) || (s > 31 && rt != R_PLATE && rt != R_EYE)) {
        return C_MT3;     /* lit top/right edge */
    }
    if (s > 31) {
        return C_MT2;
    }
    return y < 25 ? C_MT1 : C_MT2;
}

static void build_base(void)
{
    memset(s_reg, R_BG, sizeof(s_reg));
    fill_poly(P_JACKET, NPTS(P_JACKET), R_JACKET);
    fill_poly(P_NECK, NPTS(P_NECK), R_NECK);
    fill_poly(P_COLLAR_L, NPTS(P_COLLAR_L), R_COLLAR);
    fill_poly(P_COLLAR_R, NPTS(P_COLLAR_R), R_COLLAR);
    fill_poly(P_EAR_R, NPTS(P_EAR_R), R_EAR);
    fill_poly(P_EAR_L, NPTS(P_EAR_L), R_EAR);
    fill_poly(P_FACE, NPTS(P_FACE), R_FACE);
    fill_poly(P_HAIR, NPTS(P_HAIR), R_HAIR);
    fill_poly(P_PLATE, NPTS(P_PLATE), R_PLATE);
    for (int y = EY - ER; y <= EY + ER; y++) {
        for (int x = EX - ER; x <= EX + ER; x++) {
            s_reg[y * W + x] = R_EYE;
        }
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t c = C_BG;
            switch (s_reg[y * W + x]) {
            case R_FACE:
                c = shade_face(x, y);
                break;
            case R_HAIR:
                c = shade_hair(x, y);
                break;
            case R_PLATE:
                c = shade_plate(x, y);
                break;
            case R_EAR:
                c = x > 32 ? (x >= 47 ? C_SK2 : C_SK1) : C_SK0;
                break;
            case R_NECK: {
                int32_t t = QF(0.40f) - (y < 49 ? QF(0.30f) : 0);   /* shadow under the jaw */
                if (x <= 28 || x >= 36) {
                    t -= QF(0.20f);
                }
                c = ramp(C_SK0, t);
                break;
            }
            case R_COLLAR:
                c = C_NV1;
                break;
            case R_JACKET:
                c = C_NV0;
                break;
            case R_EYE:
                c = C_MT0;
                break;
            default:
                break;
            }
            s_base[y * W + x] = c;
        }
    }

    /* hair casts a hard shadow on the forehead; face edges darken next to background */
    for (int y = 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t r = s_reg[y * W + x];
            if (r == R_FACE && (s_reg[(y - 1) * W + x] == R_HAIR || s_reg[(y - 1) * W + x] == R_PLATE)) {
                s_base[y * W + x] = C_SK0;
            }
            if (r == R_NECK && s_reg[(y - 1) * W + x] == R_FACE) {
                s_base[y * W + x] = C_OUT;
            }
        }
    }

    /* collar: lit inner edge and a gold piping along its top */
    for (int y = 40; y < H; y++) {
        for (int x = 1; x < W - 1; x++) {
            int i = y * W + x;
            if (s_reg[i] != R_COLLAR) {
                continue;
            }
            bool inner = (x < 32 && s_reg[i + 1] == R_NECK) || (x > 32 && s_reg[i - 1] == R_NECK);
            bool top = s_reg[i - W] != R_COLLAR;
            if (top) {
                s_base[i] = (x & 1) ? C_GD : C_GDD;
            } else if (inner) {
                s_base[i] = C_NV2;
            } else if (y < 50 && ((x < 32 && s_reg[i - 1] != R_COLLAR) || (x > 32 && s_reg[i + 1] != R_COLLAR))) {
                s_base[i] = C_NV2;
            }
        }
    }
    /* jacket: gold clasps / strap hardware on both shoulders */
    for (int k = 0; k < 3; k++) {
        int y = 56 + k * 3;
        bpx(9, y, C_GD); bpx(10, y, C_GD); bpx(11, y, C_GDD);
        bpx(54, y, C_GD); bpx(53, y, C_GD); bpx(52, y, C_GDD);
    }
    bline(13, 54, 13, 63, C_MT1);
    bline(50, 54, 50, 63, C_MT1);

    /* outline: background pixels touching the figure go to OUT (keeps the silhouette crisp) */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (s_reg[y * W + x] != R_BG) {
                continue;
            }
            bool n = reg_at(x - 1, y) != R_BG || reg_at(x + 1, y) != R_BG || reg_at(x, y - 1) != R_BG || reg_at(x, y + 1) != R_BG;
            if (n) {
                s_base[y * W + x] = C_OUT;
            }
        }
    }

    /* hair part: a dark sweep from the crown down to the right temple */
    bline(31, 2, 46, 19, C_HR0);

    /* plate: gold traces and a bolt */
    bline(24, 11, 27, 11, C_GD);
    bline(27, 11, 27, 13, C_GD);
    bline(13, 32, 15, 34, C_GDD);
    bpx(19, 10, C_MT3);
    bpx(14, 17, C_MT3);

    /* ear: a hard inner shadow and a metal stud */
    bline(46, 27, 46, 32, C_SK0);
    bpx(46, 35, C_MT3);

    /* brow: straight and heavy, low at the inner end (severe, not surprised) */
    bline(NX - 4, NY - 3, NX - 2, NY - 3, C_BROW);
    bline(NX - 2, NY - 4, NX + 5, NY - 4, C_BROW);
    bline(NX - 1, NY - 5, NX + 4, NY - 5, C_SK1);
    bpx(NX - 4, NY - 4, C_BROW);
    /* eye socket shadow */
    bline(NX - 3, NY - 2, NX + 4, NY - 2, C_SK1);

    /* nose: lit bridge, shaded side, hard underside */
    bline(32, 30, 32, 35, C_SK3);
    bline(33, 31, 33, 35, C_SK1);
    bline(30, 37, 34, 37, C_SK0);
    bpx(31, 36, C_SK1);
    bpx(34, 36, C_SK1);
    bpx(29, 36, C_SK1);
    /* philtrum + chin */
    bpx(32, 38, C_SK1);
    bline(31, 44, 33, 44, C_SK3);

    /* cybernetic-eye housing (static parts): metal rim, gold contact pins */
    for (int y = EY - ER; y <= EY + ER; y++) {
        for (int x = EX - ER; x <= EX + ER; x++) {
            int d = abs(x - EX) > abs(y - EY) ? abs(x - EX) : abs(y - EY);
            if (d == ER) {
                s_base[y * W + x] = C_OUT;
            } else if (d == ER - 1) {
                bool lit = (y - EY) < 0 || (x - EX) > 0;
                s_base[y * W + x] = lit ? C_MT3 : C_MT2;
            }
        }
    }
    for (int k = -3; k <= 3; k += 2) {
        bpx(EX + ER + 1, EY + k, C_GD);   /* pins toward the nose */
        bpx(EX + ER + 2, EY + k, C_GDD);
        bpx(EX + k, EY + ER + 1, C_GDD);  /* pins along the bottom */
    }
    bpx(EX - ER - 1, EY - 2, C_GD);
    bpx(EX - ER - 1, EY + 2, C_GD);

    trace_build(&s_tr[0], T_CHEEK, NPTS(T_CHEEK));
    trace_build(&s_tr[1], T_PLATE1, NPTS(T_PLATE1));
    trace_build(&s_tr[2], T_PLATE2, NPTS(T_PLATE2));
    s_base_init = true;
}

/* ---------------------------------------------------------------------------
 * Behaviour state (blink, gaze)
 * ------------------------------------------------------------------------- */

static struct {
    float last_t;
    float blink_cd, blink_ph;
    float gaze, gaze_tgt, gaze_cd;
} s_st = { .blink_cd = 3.0f, .blink_ph = -1.0f, .gaze_cd = 2.5f };

static float state_update(muse_mode_t mode, float dt)
{
    float blink = 0;
    s_st.blink_cd -= dt;
    if (s_st.blink_ph < 0 && s_st.blink_cd <= 0) {
        s_st.blink_ph = 0;
    }
    if (s_st.blink_ph >= 0) {
        s_st.blink_ph += dt / 0.14f;
        if (s_st.blink_ph >= 1.0f) {
            s_st.blink_ph = -1.0f;
            s_st.blink_cd = 3.5f + frand() * 4.0f;   /* slow, deliberate */
        } else {
            blink = 1.0f;
        }
    }
    s_st.gaze_cd -= dt;
    if (s_st.gaze_cd <= 0) {
        s_st.gaze_cd = 2.5f + frand() * 3.5f;
        float r = frand();
        s_st.gaze_tgt = r < 0.6f ? 0.0f : (r < 0.8f ? -1.0f : 1.0f);
    }
    float tgt = s_st.gaze_tgt;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING || mode == MUSE_MODE_ERROR) {
        tgt = 0;   /* locked on */
    }
    s_st.gaze += (tgt - s_st.gaze) * (1.0f - expf(-dt * 12.0f));
    return blink;
}

/* ---------------------------------------------------------------------------
 * Animated parts
 * ------------------------------------------------------------------------- */

/* Glow ramp colour for level 0..4 (0 = dark housing, 4 = white-hot). */
static const uint8_t GLOW[5] = { C_MT0, C_G3, C_G2, C_G1, C_G0 };

typedef struct {
    int32_t glow;     /* Q12 overall brightness 0..ONE */
    int lit_r;        /* rings with Chebyshev radius > lit_r stay dark (boot ignite / off collapse) */
    int scan_y;       /* listening scan row, or -1 */
    int step;         /* thinking: ring phase */
    int core;         /* core radius (0 or 1) */
    bool glitch;      /* error */
    bool white;       /* happy flare */
} eye_t;

static void draw_cyber_eye(const eye_t *e, float t)
{
    /* ring levels from the outside (d = ER - 2) in to the core */
    static const uint8_t RINGS[ER - 1] = { 4, 3, 1, 3, 2 };   /* d = 0 .. 4: core, inner, dark gap, bright ring, outer */
    uint32_t gl = (uint32_t)(((t * 977.0f) > 0 ? (uint32_t)(t * 977.0f) : 0));
    for (int y = EY - ER + 2; y <= EY + ER - 2; y++) {
        int shift = 0;
        if (e->glitch) {
            int h = hash8(y, (int)(gl >> 6)) ;
            shift = h < 40 ? -2 : (h < 80 ? 1 : (h > 230 ? 2 : 0));
        }
        for (int x = EX - ER + 2; x <= EX + ER - 2; x++) {
            int sx = x - shift;
            int d = abs(sx - EX) > abs(y - EY) ? abs(sx - EX) : abs(y - EY);
            int lvl;
            if (d > ER - 2) {
                lvl = 0;
            } else if (d <= e->core) {
                lvl = 4;
            } else {
                int k = d;
                if (e->step >= 0) {
                    k = (d + e->step) % (ER - 1);
                }
                lvl = RINGS[k];
            }
            if (d > e->lit_r) {
                lvl = 0;
            }
            if (e->white && lvl > 0) {
                lvl = lvl < 3 ? 3 : 4;
            }
            if (y == e->scan_y && d <= ER - 2) {
                lvl = 4;
            } else if (e->scan_y >= 0 && abs(y - e->scan_y) == 1 && lvl > 0 && lvl < 3) {
                lvl = 3;
            }
            /* scale by glow, Q12, rounded: crisp flat rings */
            int idx = (lvl * e->glow + ONE / 2) >> Q;
            px(x, y, GLOW[idx > 4 ? 4 : idx]);
        }
    }
    /* halo on the surrounding skin and plate: a solid ring, plus a dithered outer one when bright */
    int32_t hs = e->white ? ONE : e->glow;
    for (int y = EY - ER - 2; y <= EY + ER + 2; y++) {
        for (int x = EX - ER - 2; x <= EX + ER + 2; x++) {
            int d = abs(x - EX) > abs(y - EY) ? abs(x - EX) : abs(y - EY);
            if (d <= ER || (unsigned)x >= W || (unsigned)y >= H) {
                continue;
            }
            uint8_t r = s_reg[y * W + x];
            int32_t v = d == ER + 1 ? (hs > QF(0.4f) ? ONE : 0) : clampq((hs - QF(0.85f)) * 5);
            if (!dith(v, x, y)) {
                continue;
            }
            if (r == R_FACE && s_fb[y * W + x] >= C_SK1 && s_fb[y * W + x] <= C_SK3) {
                s_fb[y * W + x] = C_GSK;
            } else if (r == R_PLATE && s_fb[y * W + x] >= C_MT1 && s_fb[y * W + x] <= C_MT3) {
                s_fb[y * W + x] = C_GMT;
            }
        }
    }
}

/*
 * Draw a trace: base colour G3/G2 scaled by `on` (Q12), plus up to two pulses
 * at positions p (0..1 along the trace, <0 = none) with a short tail.
 */
static void draw_trace(const trace_t *tr, int32_t on, float p1, float p2, int lit_n)
{
    int h1 = p1 >= 0 ? (int)(p1 * (float)tr->n) : -100;
    int h2 = p2 >= 0 ? (int)(p2 * (float)tr->n) : -100;
    for (int i = 0; i < tr->n && i < lit_n; i++) {
        int x = tr->x[i], y = tr->y[i];
        int d1 = h1 - i, d2 = h2 - i;
        uint8_t c;
        if (d1 == 0 || d2 == 0) {
            c = C_G0;
        } else if ((d1 > 0 && d1 <= 2) || (d2 > 0 && d2 <= 2)) {
            c = C_G1;
        } else {
            int32_t v = on * 2;   /* 0..2 in Q12 */
            int base = v >> Q;
            int frac = (v & (ONE - 1)) >> 8;
            int idx = base + (frac > bayer(x, y) ? 1 : 0);
            if (idx <= 0) {
                continue;   /* trace dark: leave the skin/metal */
            }
            c = idx >= 2 ? C_G2 : C_G3;
        }
        px(x, y, c);
    }
}

static void draw_natural_eye(float open, float gaze, bool narrow)
{
    int gx = (int)floorf(gaze + 0.5f);
    if (open < 0.5f) {
        /* closed: a single hard lid line */
        for (int x = NX - 3; x <= NX + 4; x++) {
            px(x, NY + 1, C_BROW);
        }
        for (int x = NX - 2; x <= NX + 3; x++) {
            px(x, NY, C_SK1);
        }
        px(NX + 5, NY, C_BROW);
        return;
    }
    /* upper lid / lash line with a sharp outer flick */
    for (int x = NX - 3; x <= NX + 4; x++) {
        px(x, NY - 1, C_BROW);
    }
    px(NX + 5, NY - 2, C_BROW);
    px(NX + 5, NY - 1, C_BROW);
    int y0 = narrow ? NY + 1 : NY;
    if (narrow) {
        for (int x = NX - 2; x <= NX + 3; x++) {
            px(x, NY, C_BROW);
        }
    }
    for (int y = y0; y <= NY + 1; y++) {
        for (int x = NX - 2; x <= NX + 3; x++) {
            int ix = x - gx;
            uint8_t c = C_SCL;
            if (ix >= NX - 1 && ix <= NX + 2) {
                c = C_IRIS;
                if ((ix == NX || ix == NX + 1) && y == NY) {
                    c = C_PUPIL;
                }
                if (ix == NX + 2 && y == NY + 1) {
                    c = C_IRISL;
                }
            }
            px(x, y, c);
        }
    }
    /* lower lid */
    for (int x = NX - 2; x <= NX + 3; x++) {
        px(x, NY + 2, C_SK1);
    }
}

static void draw_mouth(float open)
{
    /* flat line; when speaking it parts into a narrow dark slit (never a round "o") */
    int gap = open > 0.25f ? 1 : 0;
    for (int x = 28; x <= 36; x++) {
        px(x, 40, (x == 28 || x == 36) ? C_SK1 : C_LIP);
    }
    if (gap) {
        int hw = open > 0.7f ? 3 : 2;
        for (int x = 32 - hw; x <= 32 + hw; x++) {
            px(x, 41, C_OUT);
        }
    }
    for (int x = 30; x <= 34; x++) {
        px(x, 41 + gap, C_SK3);    /* lower lip catch light */
        px(x, 42 + gap, C_SK1);    /* shadow under the lip */
    }
}

/* Happy overlay: a square pulse expands off the cybernetic eye across the face. */
static void draw_ack(float ph, float amount)
{
    int r = ER + 2 + (int)(ph * 12.0f);
    int32_t a = clampq((int32_t)((1.0f - ph) * amount * (float)ONE));
    for (int y = EY - r; y <= EY + r; y++) {
        for (int x = EX - r; x <= EX + r; x++) {
            int d = abs(x - EX) > abs(y - EY) ? abs(x - EX) : abs(y - EY);
            if (d != r || (unsigned)x >= W || (unsigned)y >= H) {
                continue;
            }
            uint8_t reg = s_reg[y * W + x];
            if (reg != R_FACE && reg != R_PLATE && reg != R_HAIR) {
                continue;
            }
            if (dith(a, x, y)) {
                s_fb[y * W + x] = reg == R_FACE ? C_GSK : C_G2;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Blitter
 * ------------------------------------------------------------------------- */

#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px_size)
{
    s_size = px_size < 0 ? 0 : px_size < MAP_MAX ? px_size : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    /* Callers pass in-range strips; refuse anything else rather than read past s_map. */
    if (!dst || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 || x1 >= s_size || y1 >= s_size) {
        return;
    }
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
    if (!p || !isfinite(p->t) || !isfinite(p->mode_t)) {
        return; /* keep the last frame rather than drive geometry from garbage */
    }
    if (!s_base_init) {
        build_base();
    }
    float dt = s_st.last_t > 0 ? clampf(p->t - s_st.last_t, 0, 0.2f) : 0.04f;
    s_st.last_t = p->t;

    muse_mode_t mode = (unsigned)p->mode < MUSE_MODE_COUNT ? p->mode : MUSE_MODE_IDLE;
    float mt = clampf(p->mode_t, 0, 1e6f);
    float happy = mode == MUSE_MODE_ERROR || !(p->happy > 0) ? 0.0f : clampf(p->happy, 0, 1);
    float level = !(p->level > 0) ? 0.0f : clampf(p->level, 0, 1);
    float t = fmodf(p->t, 3600.0f);   /* phases only; keeps float precision for long uptimes */

    update_palette(&SCHEMES[mode], dt);
    float blink = state_update(mode, dt);

    memcpy(s_fb, s_base, sizeof(s_fb));

    /* ---- boot: the face resolves out of the dark (dissolve), then the eye ignites ---- */
    float reveal = mode == MUSE_MODE_BOOT ? clampf((mt - 0.2f) / 0.7f, 0, 1) : 1.0f;

    eye_t e = { .glow = ONE, .lit_r = ER, .scan_y = -1, .step = -1, .core = 0 };
    float open = blink > 0 ? 0.0f : 1.0f;
    float mouth = 0;
    int32_t trace_on = QF(0.75f);
    float p1 = -1, p2 = -1;
    int trace_lit = TRACE_MAX;

    switch (mode) {
    case MUSE_MODE_BOOT: {
        float ig = clampf((mt - 0.8f) / 0.6f, 0, 1);          /* ignite 0.8 .. 1.4 s */
        e.lit_r = mt < 0.8f ? -1 : (int)(ig * (float)(ER - 1) + 0.5f);
        e.glow = QF(0.6f + 0.4f * ig);
        if (mt > 1.4f && mt < 1.7f) {
            e.white = true;   /* a single hard flash as it locks on */
        }
        open = mt < 1.0f ? 0.0f : open;
        trace_on = mt < 1.3f ? 0 : QF(0.75f);
        trace_lit = mt < 1.3f ? 0 : (int)((mt - 1.3f) / 0.4f * (float)TRACE_MAX);
        if (mt >= 1.3f && mt < 1.9f) {
            p1 = (mt - 1.3f) / 0.6f;
        }
        break;
    }
    case MUSE_MODE_IDLE:
        e.glow = QF(0.80f + 0.12f * sinf(t * 1.4f));
        trace_on = QF(0.55f);
        break;
    case MUSE_MODE_LISTENING: {
        e.glow = QF(clampf(0.95f + level * 0.2f, 0, 1));
        e.core = level > 0.45f ? 1 : 0;
        float sc = fracf(t * 0.8f);
        e.scan_y = EY - (ER - 2) + (int)(sc * (float)(2 * (ER - 2) + 1));
        trace_on = QF(0.85f);
        break;
    }
    case MUSE_MODE_THINKING:
        e.glow = QF(0.9f);
        e.step = (int)(t * 6.0f) % (ER - 1);
        trace_on = QF(0.45f);
        p1 = fracf(t * 0.9f);
        p2 = fracf(t * 0.9f + 0.5f);
        break;
    case MUSE_MODE_SPEAKING:
        e.glow = QF(clampf(0.78f + level * 0.3f, 0, 1));
        e.core = level > 0.35f ? 1 : 0;
        mouth = level * 1.2f;
        trace_on = QF(clampf(0.5f + level * 0.5f, 0, 1));
        break;
    case MUSE_MODE_ERROR:
        e.glitch = mt < 0.6f || fracf(t * 0.7f) < 0.12f;
        e.glow = (fracf(t * 2.5f) < 0.5f) ? ONE : QF(0.55f);
        trace_on = e.glow;
        break;
    case MUSE_MODE_OFF: {
        float f = clampf(mt / 1.1f, 0, 1);                 /* collapse outside-in */
        e.lit_r = (int)((1.0f - f) * (float)(ER - 1) + 0.5f) - (f >= 1.0f ? 1 : 0);
        e.glow = QF(clampf(1.0f - mt / 1.3f, 0, 1));
        open = mt < 0.6f ? open : 0.0f;
        trace_on = QF(clampf(0.6f - mt / 0.8f, 0, 1));
        break;
    }
    default:
        break;
    }

    bool narrow = false;
    float ack_ph = -1;
    if (happy > 0) {
        e.white = true;
        e.glow = ONE;
        e.core = 1;
        narrow = happy > 0.3f;
        ack_ph = fracf(t * 1.1f);
        trace_on = ONE;
        p1 = fracf(t * 1.6f);
    }

    draw_trace(&s_tr[0], trace_on, p1, p2, trace_lit);
    draw_trace(&s_tr[1], trace_on, p2 >= 0 ? p2 : p1, -1, trace_lit);
    draw_trace(&s_tr[2], trace_on, p1, -1, trace_lit);
    draw_cyber_eye(&e, t);
    draw_natural_eye(open, s_st.gaze, narrow);
    draw_mouth(mouth);
    if (ack_ph >= 0) {
        draw_ack(ack_ph, happy);
    }

    if (reveal < 1.0f) {
        /* a hard scan line draws the face top to bottom; below it is dark */
        int sy = (int)(reveal * (float)(H + 1)) - 1;
        for (int y = sy < 0 ? 0 : sy; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (y == sy && s_reg[y * W + x] != R_BG) {
                    s_fb[y * W + x] = C_G1;
                } else if (y > sy) {
                    s_fb[y * W + x] = C_BG;
                }
            }
        }
    }
}
