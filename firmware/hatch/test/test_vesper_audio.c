/*
 * Host tests for firmware/hatch/vesper_audio.c (task 10: the reply-speech
 * resampler, MP3 buffer accounting and caption timing). Run: make -C firmware test
 */
#include "vesper_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail, s_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        s_checks++;                                                              \
        if (!(cond)) {                                                           \
            s_fail++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static const double PI = 3.14159265358979323846;

/* xorshift: deterministic chunk sizes */
static uint32_t s_rng = 0x9e3779b9u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static va_resampler_t s_rs;   /* ~12 KB: static, not on the stack */

static int16_t *tone(size_t n, double hz, uint32_t rate, double amp)
{
    int16_t *x = malloc(n * sizeof(int16_t));
    for (size_t i = 0; i < n; i++) {
        x[i] = (int16_t)lrint(amp * sin(2 * PI * hz * (double)i / rate));
    }
    return x;
}

/* Resamples all of x in random-sized chunks (checking the max_out bound on each), then flushes. */
static size_t run(uint32_t in_rate, uint32_t out_rate, const int16_t *x, size_t n, int16_t *out, size_t cap,
                  bool random_chunks)
{
    if (!va_rs_init(&s_rs, in_rate, out_rate)) {
        return 0;
    }
    size_t o = 0, i = 0;
    while (i < n) {
        size_t c = random_chunks ? 1 + rnd() % 1200 : n - i;
        if (c > n - i) {
            c = n - i;
        }
        size_t bound = va_rs_max_out(&s_rs, c);
        if (o + bound > cap) {
            CHECK(!"output buffer too small for the test");
            return o;
        }
        size_t got = va_rs_process(&s_rs, x + i, c, out + o);
        CHECK(got <= bound);
        o += got;
        i += c;
    }
    size_t bound = va_rs_max_out(&s_rs, 0);
    if (o + bound <= cap) {
        size_t got = va_rs_flush(&s_rs, out + o);
        CHECK(got <= bound);
        o += got;
    }
    return o;
}

/* RMS error against the ideal tone at the output rate, over [skip, n - skip), in dB below the tone. */
static double snr_db(const int16_t *y, size_t n, double hz, uint32_t rate, double amp, size_t skip)
{
    double sig = 0, err = 0;
    for (size_t k = skip; k + skip < n; k++) {
        double ideal = amp * sin(2 * PI * hz * (double)k / rate);
        sig += ideal * ideal;
        err += (y[k] - ideal) * (y[k] - ideal);
    }
    return 10 * log10(sig / (err + 1e-9));
}

static double rms(const int16_t *y, size_t from, size_t to)
{
    double s = 0;
    for (size_t k = from; k < to; k++) {
        s += (double)y[k] * y[k];
    }
    return sqrt(s / (double)(to - from));
}

/* The stock bench self-test's linear interpolator (muse_chat_session.cpp), for
 * comparison; fed per MP3 frame (576 samples), as stock does, so its Q16 position can't wrap. */
static size_t linear(uint32_t in_rate, uint32_t out_rate, const int16_t *x, size_t total, int16_t *out)
{
    uint32_t step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate), pos = 0;
    int16_t prev = 0;
    size_t o = 0;
    for (size_t base = 0; base < total; base += 576) {
        const int16_t *in = x + base;
        size_t n = total - base < 576 ? total - base : 576;
        while ((pos >> 16) < n) {
            size_t i = pos >> 16;
            int32_t a = i ? in[i - 1] : prev;
            int32_t b = in[i];
            out[o++] = (int16_t)(a + (((b - a) * (int32_t)(pos & 0xffff)) >> 16));
            pos += step;
        }
        pos -= (uint32_t)(n << 16);
        prev = in[n - 1];
    }
    return o;
}

static void test_rates(void)
{
    CHECK(!va_rs_init(&s_rs, 0, 16000));
    CHECK(!va_rs_init(&s_rs, 7999, 16000));
    CHECK(!va_rs_init(&s_rs, 22050, 48001));
    CHECK(!va_rs_init(&s_rs, 96000, 16000));
    CHECK(va_rs_init(&s_rs, 8000, 16000));
    CHECK(va_rs_init(&s_rs, 48000, 16000));

    /* the coefficient table: every phase sums to exactly 1.0 (Q15), and the accumulator can't overflow int32 even */
    CHECK(va_rs_init(&s_rs, 22050, 16000));
    for (int p = 0; p < VA_RS_PHASES; p++) {
        int32_t sum = 0, abs_sum = 0;
        for (int m = 0; m < VA_RS_TAPS; m++) {
            sum += s_rs.coef[p][m];
            abs_sum += abs(s_rs.coef[p][m]);
        }
        CHECK(sum == 32768);
        CHECK(abs_sum < 65536);
    }
}

/* va_rs_reset starts a new stream: the same samples as a fresh init */
static void test_reset(void)
{
    const size_t N = 5000;
    int16_t *x = tone(N, 700, 22050, 9000);
    int16_t *a = malloc(2 * N * sizeof(int16_t)), *b = malloc(2 * N * sizeof(int16_t));
    CHECK(va_rs_init(&s_rs, 22050, 16000));
    size_t na = va_rs_process(&s_rs, x, N, a);
    na += va_rs_flush(&s_rs, a + na);
    va_rs_reset(&s_rs);
    size_t nb = va_rs_process(&s_rs, x, N, b);
    nb += va_rs_flush(&s_rs, b + nb);
    CHECK(na == nb && memcmp(a, b, na * sizeof(int16_t)) == 0);
    free(x);
    free(a);
    free(b);
}

static void test_bypass(void)
{
    int16_t x[500], y[500];
    for (int i = 0; i < 500; i++) {
        x[i] = (int16_t)(rnd() & 0xffff);
    }
    size_t n = run(16000, 16000, x, 500, y, 500, true);
    CHECK(n == 500);
    CHECK(memcmp(x, y, sizeof(x)) == 0);
    CHECK(va_rs_max_out(&s_rs, 0) == 0);
}

/* The backend's mp3_22050_32 to the speaker's 16 kHz: length, tones, aliasing, splits. */
static void test_22050_to_16000(void)
{
    const uint32_t IN = 22050, OUT = 16000;
    const size_t N = IN * 3;            /* 3 s */
    size_t cap = N + 1024;
    int16_t *y = malloc(cap * sizeof(int16_t)), *y2 = malloc(cap * sizeof(int16_t));

    /* duration preserved: 3 s in, 3 s out (no drift: the time base is exact) */
    int16_t *x = tone(N, 1000, IN, 16000);
    size_t n = run(IN, OUT, x, N, y, cap, true);
    CHECK(n >= OUT * 3 - 1 && n <= OUT * 3 + 1);

    /* 1 kHz passes: on time (sample k is the input at k / 16000 s), at full level */
    double s1 = snr_db(y, n, 1000, OUT, 16000, 64);
    CHECK(s1 > 40);
    double g = rms(y, 64, n - 64) / (16000 / sqrt(2.0));
    CHECK(g > 0.99 && g < 1.01);

    /* the same in one call: identical samples (state carries across calls) */
    size_t n2 = run(IN, OUT, x, N, y2, cap, false);
    CHECK(n2 == n);
    CHECK(memcmp(y, y2, n * sizeof(int16_t)) == 0);
    free(x);

    /* 3 kHz and 5 kHz (speech formants, sibilance) pass */
    x = tone(N, 3000, IN, 16000);
    n = run(IN, OUT, x, N, y, cap, true);
    CHECK(snr_db(y, n, 3000, OUT, 16000, 64) > 35);
    free(x);
    x = tone(N, 5000, IN, 16000);
    n = run(IN, OUT, x, N, y, cap, true);
    CHECK(snr_db(y, n, 5000, OUT, 16000, 64) > 30);
    free(x);

    /* above the new Nyquist: 10 kHz would fold to 6 kHz, 9 kHz to 7 kHz; both are filtered out */
    x = tone(N, 10000, IN, 16000);
    n = run(IN, OUT, x, N, y, cap, true);
    double a10 = 20 * log10(rms(y, 64, n - 64) / (16000 / sqrt(2.0)));
    CHECK(a10 < -50);
    size_t nl = linear(IN, OUT, x, N, y2);
    double l10 = 20 * log10(rms(y2, 64, nl - 64) / (16000 / sqrt(2.0)));
    CHECK(l10 > -20);   /* the stock linear interpolator lets the alias through */
    free(x);
    x = tone(N, 9000, IN, 16000);
    n = run(IN, OUT, x, N, y, cap, true);
    double a9 = 20 * log10(rms(y, 64, n - 64) / (16000 / sqrt(2.0)));
    CHECK(a9 < -40);
    free(x);
    printf("resampler 22050->16000: 1 kHz SNR %.1f dB, gain %.4f; 10 kHz alias %.1f dB (linear interp %.1f dB), 9 kHz %.1f dB\n",
           s1, g, a10, l10, a9);

    /* DC passes exactly */
    x = malloc(N * sizeof(int16_t));
    for (size_t i = 0; i < N; i++) {
        x[i] = 12000;
    }
    n = run(IN, OUT, x, N, y, cap, true);
    bool flat = true;
    for (size_t k = 64; k + 64 < n; k++) {
        flat = flat && abs(y[k] - 12000) <= 1;
    }
    CHECK(flat);

    /* full-scale square wave: overshoot saturates, no overflow (UBSan) */
    for (size_t i = 0; i < N; i++) {
        x[i] = (i / 11) % 2 ? 32767 : -32768;
    }
    n = run(IN, OUT, x, N, y, cap, true);
    CHECK(n >= OUT * 3 - 1);
    free(x);

    /* MP3-frame-sized pieces, as the decode path feeds it (576 samples per MPEG-2 layer III frame) */
    x = tone(N, 440, IN, 8000);
    CHECK(va_rs_init(&s_rs, IN, OUT));
    size_t o = 0;
    for (size_t i = 0; i < N; i += 576) {
        size_t c = N - i < 576 ? N - i : 576;
        o += va_rs_process(&s_rs, x + i, c, y + o);
    }
    o += va_rs_flush(&s_rs, y + o);
    CHECK(o >= OUT * 3 - 1 && o <= OUT * 3 + 1);
    CHECK(snr_db(y, o, 440, OUT, 8000, 64) > 40);
    free(x);
    free(y);
    free(y2);
}

static void test_other_rates(void)
{
    const size_t N = 24000;
    int16_t *y = malloc(4 * N * sizeof(int16_t));
    /* up: 8 kHz -> 16 kHz */
    int16_t *x = tone(N, 1000, 8000, 12000);
    size_t n = run(8000, 16000, x, N, y, 4 * N, true);
    CHECK(n >= 2 * N - 2 && n <= 2 * N + 2);
    CHECK(snr_db(y, n, 1000, 16000, 12000, 64) > 35);
    free(x);
    /* down: 44.1 kHz and 24 kHz (other ElevenLabs formats) */
    x = tone(N, 1500, 44100, 12000);
    n = run(44100, 16000, x, N, y, 4 * N, true);
    CHECK(n + 2 >= N * 16000 / 44100 && n <= N * 16000 / 44100 + 2);
    CHECK(snr_db(y, n, 1500, 16000, 12000, 64) > 40);
    free(x);
    x = tone(N, 1500, 24000, 12000);
    n = run(24000, 16000, x, N, y, 4 * N, true);
    CHECK(n + 2 >= N * 2 / 3 && n <= N * 2 / 3 + 2);
    CHECK(snr_db(y, n, 1500, 16000, 12000, 64) > 40);
    free(x);
    free(y);
}

static void test_mp3buf(void)
{
    uint8_t store[4000], data[5000];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }
    va_mp3buf_t b;
    va_mp3_init(&b, NULL, 100);
    CHECK(va_mp3_room(&b) == 0 && va_mp3_put(&b, data, 10) == 0);

    va_mp3_init(&b, store, sizeof(store));
    CHECK(va_mp3_room(&b) == 4000);
    CHECK(!va_mp3_may_decode(&b, 0));
    CHECK(va_mp3_put(&b, data, VA_MP3_HOLD) == VA_MP3_HOLD);
    CHECK(!va_mp3_may_decode(&b, 0));               /* exactly the hold: wait for more */
    CHECK(va_mp3_put(&b, data + VA_MP3_HOLD, 1) == 1);
    CHECK(va_mp3_may_decode(&b, 0));
    CHECK(!va_mp3_may_decode(&b, 1));
    /* full: takes what fits, no more */
    CHECK(va_mp3_put(&b, data, 5000) == 4000 - VA_MP3_HOLD - 1);
    CHECK(b.len == 4000 && va_mp3_room(&b) == 0);
    CHECK(memcmp(store, data, VA_MP3_HOLD + 1) == 0 && memcmp(store + VA_MP3_HOLD + 1, data, 10) == 0);
    /* consumed bytes move down */
    va_mp3_consume(&b, 1000);
    CHECK(b.len == 3000 && store[0] == data[1000]);
    CHECK(va_mp3_room(&b) == 1000);
    va_mp3_consume(&b, 0);
    CHECK(b.len == 3000);
    /* the end: the tail may be decoded, nothing more is taken */
    CHECK(!va_mp3_may_decode(&b, 3000 - VA_MP3_HOLD));
    b.ended = true;
    CHECK(va_mp3_may_decode(&b, 3000 - VA_MP3_HOLD));
    CHECK(va_mp3_may_decode(&b, 2999) && !va_mp3_may_decode(&b, 3000));
    CHECK(va_mp3_room(&b) == 0 && va_mp3_put(&b, data, 1) == 0);
    CHECK(!va_mp3_drained(&b));
    va_mp3_consume(&b, 5000);   /* over-consuming empties, never underflows */
    CHECK(b.len == 0 && va_mp3_drained(&b));
}

/*
 * The decode loop's buffer discipline with a stand-in decoder that, like
 * minimp3, refuses a frame unless it can see the next frame's start or the
 * stream has ended: bytes arriving in any split, through a small buffer,
 * come out as every frame exactly once.
 */
static void test_decode_discipline(void)
{
    enum { FRAME = 104, FRAMES = 600 };   /* 32 kbps at 22.05 kHz: ~104-byte frames, ~15.7 s */
    size_t total = FRAME * FRAMES;
    uint8_t *src = malloc(total);
    for (size_t i = 0; i < total; i++) {
        src[i] = (uint8_t)(i % FRAME == 0 ? 0xFF : (i / FRAME) & 0x7F);
    }
    for (int round = 0; round < 20; round++) {
        uint8_t store[8192];
        va_mp3buf_t b;
        va_mp3_init(&b, store, sizeof(store));
        size_t sent = 0, frames = 0, next = 0;
        bool ok = true;
        while (!va_mp3_drained(&b)) {
            if (sent < total) {
                size_t c = 1 + rnd() % 3000;
                c = c > total - sent ? total - sent : c;
                sent += va_mp3_put(&b, src + sent, c);
                if (sent == total) {
                    b.ended = true;
                }
            }
            size_t off = 0;
            int budget = 1 + (int)(rnd() % 8);   /* the reply buffer only takes so many frames at a time */
            while (budget-- && va_mp3_may_decode(&b, off)) {
                size_t avail = b.len - off;
                if (avail < FRAME || (!b.ended && avail < FRAME + 1)) {
                    break;   /* the stand-in: needs the frame and the next header */
                }
                ok = ok && b.buf[off] == 0xFF && b.buf[off + 1] == (uint8_t)(next & 0x7F);
                next++;
                frames++;
                off += FRAME;
            }
            va_mp3_consume(&b, off);
        }
        CHECK(ok);
        CHECK(frames == FRAMES);
    }
    free(src);
}

static void test_timing(void)
{
    /* 32 kbps at 16 kHz out: 4000 bytes made 16000 frames */
    CHECK(va_speech_frames(16000, 4000, 4000) == 32000);
    CHECK(va_speech_frames(16000, 4000, 0) == 16000);
    CHECK(va_speech_frames(8000, 2000, 6000) == 32000);
    /* nothing said yet: not known */
    CHECK(va_speech_frames(0, 0, 99999) == 0);
    CHECK(va_speech_frames(0, 500, 99999) == 0);
    CHECK(va_speech_frames(1234, 0, 99999) == 1234);
    /* the device's first turn: 5347 bytes, 1.31 s; after the first 2 frames (~230 bytes, 836 frames) */
    uint32_t est = va_speech_frames(836, 230, 5347 - 230);
    CHECK(est > 19000 && est < 22000);
    /* clamped, never wrapped */
    CHECK(va_speech_frames(UINT32_MAX - 1, 1, UINT64_MAX) == UINT32_MAX);
    CHECK(va_speech_frames(16000, 4000, UINT64_MAX) == UINT32_MAX);
    /* a 500-char reply's MP3 (~140 KB at 32 kbps) */
    CHECK(va_speech_frames(16000, 4000, 136000) == 560000);

    CHECK(va_silent_frames(32, 16000, 16) == 32000);
    CHECK(va_silent_frames(0, 16000, 16) == 0);
    CHECK(va_silent_frames(10, 16000, 0) == 0);
    CHECK(va_silent_frames(1023, 16000, 16) == 1023000);
}

static void test_content_type(void)
{
    CHECK(va_tts_content_type_ok(NULL));
    CHECK(va_tts_content_type_ok(""));
    CHECK(va_tts_content_type_ok("  "));
    CHECK(va_tts_content_type_ok("audio/mpeg"));
    CHECK(va_tts_content_type_ok("Audio/MPEG"));
    CHECK(va_tts_content_type_ok("audio/mpeg; charset=binary"));
    CHECK(va_tts_content_type_ok(" audio/mpeg;x"));
    CHECK(va_tts_content_type_ok("audio/mp3"));
    CHECK(!va_tts_content_type_ok("audio/mpegurl"));
    CHECK(!va_tts_content_type_ok("audio/mp"));
    CHECK(!va_tts_content_type_ok("application/json"));
    CHECK(!va_tts_content_type_ok("text/html; charset=utf-8"));
    CHECK(!va_tts_content_type_ok("audio/wav"));
}

int main(void)
{
    test_rates();
    test_bypass();
    test_reset();
    test_22050_to_16000();
    test_other_rates();
    test_mp3buf();
    test_decode_discipline();
    test_timing();
    test_content_type();
    printf("vesper_audio: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
