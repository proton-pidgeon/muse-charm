/*
 * Reply-speech helpers for the Vesper hatch backend (task 10, F2): the
 * resampler that takes the backend's MP3 (mp3_22050_32: 22.05 kHz mono) to
 * the speaker's 16 kHz, the MP3 buffer accounting of the decode path, and the
 * caption timing numbers. muse_chat_vesper.c does the I/O (the HTTP GET,
 * minimp3, the reply stream buffer); this file is the arithmetic.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * Pure C: only <stddef.h>, <stdint.h>, <stdbool.h>, <string.h>, <math.h>
 * (the resampler's table is built with sinf/cosf once per sample rate), so the
 * same code runs on the ESP32-S3 and in the host tests
 * (firmware/hatch/test/test_vesper_audio.c). No allocation, no recursion.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Resampler ----
 *
 * Polyphase windowed-sinc (Blackman), exact rational time base (no drift
 * however long the reply), VA_RS_TAPS input samples per output, the filter's
 * fractional delay quantised to 1/(2*VA_RS_PHASES) of an input sample. The
 * low-pass cutoff is 0.45 of the lower of the two rates, so going down to
 * 16 kHz nothing above ~7.2 kHz is folded back into the speech (a linear
 * interpolator, as in the stock bench self-test, aliases 8-11 kHz down to
 * 5-8 kHz). Output sample k is the input signal at time k / out_rate; the
 * filter lags VA_RS_TAPS / 2 input samples, which va_rs_flush() drains at the
 * end of a stream. Equal rates pass samples through unchanged.
 */
#define VA_RS_TAPS 48
#define VA_RS_PHASES 128
#define VA_RS_RATE_MIN 8000
#define VA_RS_RATE_MAX 48000

typedef struct {
    uint32_t in_rate, out_rate;
    bool bypass;
    uint32_t r;              /* remainder of the next output's time, in 1/out_rate input samples */
    uint32_t lag;            /* inputs still to push before the next output is due */
    unsigned hpos;           /* newest sample in hist (written twice, so the window is contiguous) */
    int16_t hist[2 * VA_RS_TAPS];
    int16_t coef[VA_RS_PHASES][VA_RS_TAPS];   /* Q15, each phase sums to exactly 32768 */
} va_resampler_t;

/* False (and r unusable) unless both rates are in [VA_RS_RATE_MIN, VA_RS_RATE_MAX]. */
bool va_rs_init(va_resampler_t *r, uint32_t in_rate, uint32_t out_rate);

/* Starts a new stream at the same rates, keeping the coefficient table
 * (va_rs_init builds it: ~6k sinf/cosf, a few ms on the S3). */
void va_rs_reset(va_resampler_t *r);

/* The most samples va_rs_process can write for n inputs (and va_rs_flush for n = 0). */
size_t va_rs_max_out(const va_resampler_t *r, size_t n);

/* Resamples n inputs into out (room for va_rs_max_out(r, n)); state carries
 * across calls, so any split of a stream gives the same samples. */
size_t va_rs_process(va_resampler_t *r, const int16_t *in, size_t n, int16_t *out);

/* End of stream: pushes the filter's lag of silence, so the last input
 * samples come out. out needs va_rs_max_out(r, 0). The resampler is then
 * spent; va_rs_init it again for another stream. */
size_t va_rs_flush(va_resampler_t *r, int16_t *out);

/* ---- MP3 buffer (the bytes fetched but not yet decoded) ---- */

/* minimp3 only takes a frame once it can see the next one's header; given
 * less it resyncs and skips what it has, which drops speech and clicks. So
 * until the stream has ended, the decoder leaves this much for more to come:
 * the largest MP3 frame and the next header (as the stock decode path). */
#define VA_MP3_HOLD (1441 + 4)

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool ended;              /* no more bytes will come (finished, or cut short) */
} va_mp3buf_t;

void va_mp3_init(va_mp3buf_t *b, uint8_t *buf, size_t cap);
size_t va_mp3_room(const va_mp3buf_t *b);
/* Takes at most va_mp3_room() bytes; returns how many it took. */
size_t va_mp3_put(va_mp3buf_t *b, const uint8_t *data, size_t n);
/* Whether the decoder may look at buf[off..len) now (see VA_MP3_HOLD). */
bool va_mp3_may_decode(const va_mp3buf_t *b, size_t off);
/* Drops the first off bytes (decoded or skipped). */
void va_mp3_consume(va_mp3buf_t *b, size_t off);
/* All bytes are in and decoded. */
bool va_mp3_drained(const va_mp3buf_t *b);

/* ---- Timing (caption sync) ---- */

/*
 * How long the message's speech will be, in out_rate frames: what's decoded
 * so far plus what the bitrate says the rest holds (bytes_left: buffered
 * plus still to download). kbps <= 0 (no frame decoded yet) gives just the
 * decoded part. Never 0 once anything is known, so a caption timed by it
 * never falls back to the reading-pace guess mid-speech.
 */
uint32_t va_speech_frames(uint32_t decoded, uint64_t bytes_left, int kbps, uint32_t out_rate);

/* A message's caption time with no speech (the stock pace): chars at cps. */
uint32_t va_silent_frames(size_t chars, uint32_t out_rate, unsigned cps);

/* The TTS GET's Content-Type: audio/mpeg (or audio/mp3, or none at all) is
 * accepted; anything else (a JSON 404 body, an HTML error page) is not. */
bool va_tts_content_type_ok(const char *content_type);

#ifdef __cplusplus
}
#endif
