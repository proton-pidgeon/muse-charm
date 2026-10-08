/*
 * Reply-speech helpers for the Vesper hatch backend (task 10, F2). See
 * vesper_audio.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_audio.h"

#include <math.h>
#include <string.h>

#define RS_DELAY (VA_RS_TAPS / 2)

/* ---- Resampler ---- */

/* The table is built in single precision: the S3's FPU has no double, and Q15 needs far less. */
static const float PI_F = 3.14159265f;

static float blackman(float t)
{
    /* t in [-RS_DELAY, RS_DELAY] */
    float x = PI_F * t / RS_DELAY;
    return 0.42f + 0.5f * cosf(x) + 0.08f * cosf(2.0f * x);
}

void va_rs_reset(va_resampler_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
    r->hpos = 0;
    r->r = 0;
    r->lag = RS_DELAY + 1;   /* output 0 (time 0) needs input RS_DELAY as its newest sample */
}

bool va_rs_init(va_resampler_t *r, uint32_t in_rate, uint32_t out_rate)
{
    memset(r, 0, sizeof(*r));
    if (in_rate < VA_RS_RATE_MIN || in_rate > VA_RS_RATE_MAX || out_rate < VA_RS_RATE_MIN ||
        out_rate > VA_RS_RATE_MAX) {
        return false;
    }
    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->bypass = in_rate == out_rate;
    va_rs_reset(r);
    if (r->bypass) {
        return true;
    }
    /* cutoff in cycles per input sample: 0.45 of the lower rate's band */
    float fc = 0.45f * (float)(in_rate < out_rate ? in_rate : out_rate) / (float)in_rate;
    for (int p = 0; p < VA_RS_PHASES; p++) {
        float f = (p + 0.5f) / VA_RS_PHASES;   /* the middle of the phase's fractional delays */
        float g[VA_RS_TAPS], sum = 0;
        for (int m = 0; m < VA_RS_TAPS; m++) {
            /* tap m is input j - m; the output time is j - RS_DELAY + f, so their distance is m - RS_DELAY + f */
            float t = (float)(m - RS_DELAY) + f;
            float x = 2.0f * PI_F * fc * t;
            float sinc = fabsf(x) < 1e-6f ? 1.0f : sinf(x) / x;
            g[m] = 2.0f * fc * sinc * blackman(t);
            sum += g[m];
        }
        int32_t total = 0;
        int peak = 0;
        for (int m = 0; m < VA_RS_TAPS; m++) {
            float c = g[m] / sum * 32768.0f;
            int32_t q = (int32_t)(c < 0 ? c - 0.5f : c + 0.5f);
            r->coef[p][m] = (int16_t)q;
            total += q;
            if (q > r->coef[p][peak]) {
                peak = m;
            }
        }
        r->coef[p][peak] = (int16_t)(r->coef[p][peak] + (32768 - total));   /* unity gain at DC, exactly */
    }
    return true;
}

size_t va_rs_max_out(const va_resampler_t *r, size_t n)
{
    if (r->bypass) {
        return n;
    }
    size_t pushes = n ? n : RS_DELAY;
    return (size_t)(((uint64_t)pushes * r->out_rate + r->in_rate - 1) / r->in_rate) + 1;
}

static int16_t rs_emit(const va_resampler_t *r)
{
    unsigned p = (unsigned)((uint64_t)r->r * VA_RS_PHASES / r->out_rate);
    const int16_t *x = r->hist + r->hpos;
    const int16_t *c = r->coef[p];
    int64_t acc = 0;
    for (int m = 0; m < VA_RS_TAPS; m++) {
        acc += (int32_t)x[m] * c[m];
    }
    acc = acc >= 0 ? (acc + 16384) / 32768 : -((-acc + 16384) / 32768);
    return (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
}

static size_t rs_push(va_resampler_t *r, int16_t s, int16_t *out)
{
    size_t o = 0;
    r->hpos = r->hpos ? r->hpos - 1 : VA_RS_TAPS - 1;
    r->hist[r->hpos] = r->hist[r->hpos + VA_RS_TAPS] = s;
    r->lag--;
    while (r->lag == 0) {
        out[o++] = rs_emit(r);
        r->r += r->in_rate;               /* the next output's time, k * in / out input samples */
        r->lag += r->r / r->out_rate;
        r->r %= r->out_rate;
    }
    return o;
}

size_t va_rs_process(va_resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    if (r->bypass) {
        if (n) {
            memmove(out, in, n * sizeof(int16_t));
        }
        return n;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        o += rs_push(r, in[i], out + o);
    }
    return o;
}

size_t va_rs_flush(va_resampler_t *r, int16_t *out)
{
    if (r->bypass) {
        return 0;
    }
    size_t o = 0;
    for (int i = 0; i < RS_DELAY; i++) {
        o += rs_push(r, 0, out + o);
    }
    return o;
}

/* ---- MP3 buffer ---- */

void va_mp3_init(va_mp3buf_t *b, uint8_t *buf, size_t cap)
{
    b->buf = buf;
    b->cap = buf ? cap : 0;
    b->len = 0;
    b->ended = false;
}

size_t va_mp3_room(const va_mp3buf_t *b)
{
    return b->ended ? 0 : b->cap - b->len;
}

size_t va_mp3_put(va_mp3buf_t *b, const uint8_t *data, size_t n)
{
    size_t room = va_mp3_room(b);
    if (n > room) {
        n = room;
    }
    if (n) {
        memcpy(b->buf + b->len, data, n);
        b->len += n;
    }
    return n;
}

bool va_mp3_may_decode(const va_mp3buf_t *b, size_t off)
{
    if (off >= b->len) {
        return false;
    }
    return b->ended || b->len - off > VA_MP3_HOLD;
}

void va_mp3_consume(va_mp3buf_t *b, size_t off)
{
    if (off >= b->len) {
        b->len = 0;
        return;
    }
    if (off) {
        memmove(b->buf, b->buf + off, b->len - off);
        b->len -= off;
    }
}

bool va_mp3_drained(const va_mp3buf_t *b)
{
    return b->ended && b->len == 0;
}

/* ---- Timing ---- */

uint32_t va_speech_frames(uint32_t said, uint64_t said_bytes, uint64_t bytes_left)
{
    if (!said || !said_bytes) {
        return said;
    }
    if (bytes_left > ((uint64_t)1 << 31)) {
        bytes_left = (uint64_t)1 << 31;   /* far past any MP3 the node takes; keeps the product in range */
    }
    uint64_t total = (uint64_t)said + bytes_left * said / said_bytes;
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

uint32_t va_silent_frames(size_t chars, uint32_t out_rate, unsigned cps)
{
    if (!cps) {
        return 0;
    }
    uint64_t f = (uint64_t)chars * out_rate / cps;
    return f > UINT32_MAX ? UINT32_MAX : (uint32_t)f;
}

static bool prefix_ci(const char *s, const char *p, size_t *len)
{
    size_t i = 0;
    for (; p[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != p[i]) {
            return false;
        }
    }
    *len = i;
    return true;
}

bool va_tts_content_type_ok(const char *ct)
{
    if (!ct) {
        return true;
    }
    while (*ct == ' ' || *ct == '\t') {
        ct++;
    }
    if (!*ct) {
        return true;
    }
    static const char *const ok[] = { "audio/mpeg", "audio/mp3" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        size_t n;
        if (prefix_ci(ct, ok[i], &n) && (ct[n] == '\0' || ct[n] == ';' || ct[n] == ' ' || ct[n] == '\t')) {
            return true;
        }
    }
    return false;
}
