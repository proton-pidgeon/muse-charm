/*
 * Host tests for firmware/hatch/vesper_wake.c (the wake word's decisions,
 * task 20). Run: make -C firmware test
 *
 * The privacy and no-phantom-turn properties are tested on a small model of
 * the firmware's wake listen (run_wake below, the same steps muse_voice.c's
 * wake_record takes): it counts the backend calls the firmware would make and
 * records which chunks the turn would send.
 */
#include "vesper_wake.h"

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

#define FLOOR (-62.0f)
#define QUIET (-63.0f)
#define SPEECH (-30.0f)

/* ---- A model of the firmware's wake listen ---- */

#define MAXC 2000

typedef struct {
    int backend_calls;     /* muse_hatch_turn_begin() + every chunk handed to Hatch + turn_end */
    int begun;             /* turn_begin calls */
    int sent[MAXC];        /* chunk ids (counted from the detection) the turn sent, in order */
    int nsent;
    vw_verdict_t end;
    int chunks;            /* chunks recorded after the detection */
    int calls_before_onset;
    bool pressed;          /* the listen ended because the talk button was pressed */
    int cancels;           /* muse_hatch_turn_cancel() calls a press caused */
} run_t;

/*
 * levels[0..n): the mic's chunk levels after the detection. The firmware:
 * records every chunk locally (ids from 0 at the detection), asks
 * vw_listen_chunk, and on VW_ONSET drops the chunks before vw_listen_lead and
 * goes live (turn_begin, then the kept chunks, then each new one). Pre-wake
 * audio has negative ids and is never in the buffer.
 */
/*
 * press_at >= 0: the talk button is pressed while chunk press_at is being
 * recorded. wake_record() checks press_waiting() at the top of every chunk and
 * again right before each go_live(), so a press on the onset chunk ends the
 * listen before any turn is begun.
 */
static void run_wake_press(const float *levels, int n, const vw_config_t *cfg, float floor_db, int press_at, run_t *r)
{
    memset(r, 0, sizeof(*r));
    vw_listen_t l;
    vw_listen_start(&l, cfg, floor_db);
    int buf[MAXC], nbuf = 0;
    bool live = false;
    r->end = VW_WAITING;
    for (int i = 0; i < n; i++) {
        if (press_at >= 0 && i > press_at) {   /* the check at the top of the chunk */
            r->pressed = true;
            r->cancels += live;
            r->end = l.last;
            return;
        }
        buf[nbuf++] = i;   /* recorded locally */
        r->chunks++;
        vw_verdict_t v = vw_listen_chunk(&l, levels[i]);
        if (!live && vw_listen_may_contact_backend(&l) != (v == VW_ONSET || v == VW_SPEAKING ||
                                                            v == VW_END_SILENCE || v == VW_END_MAX)) {
            CHECK(!"may_contact_backend disagrees with the verdict");
        }
        if (v == VW_ONSET && press_at == i) {   /* the check right before go_live() */
            r->pressed = true;
            r->end = v;
            return;
        }
        if (v == VW_ONSET) {
            CHECK(!live);
            int lead = vw_listen_lead(&l);
            CHECK(lead >= 0 && lead <= i);
            int drop = 0;
            while (drop < nbuf && buf[drop] < lead) {
                drop++;
            }
            memmove(buf, buf + drop, (nbuf - drop) * sizeof(int));
            nbuf -= drop;
            r->calls_before_onset = r->backend_calls;
            r->begun++;
            r->backend_calls++;   /* turn_begin */
            live = true;
        }
        if (live) {
            for (int k = 0; k < nbuf; k++) {
                r->sent[r->nsent++] = buf[k];
                r->backend_calls++;
            }
            nbuf = 0;
        } else {
            CHECK(!vw_listen_may_contact_backend(&l));
        }
        if (v == VW_END_SILENCE || v == VW_END_MAX || v == VW_NO_SPEECH) {
            r->end = v;
            if (live) {
                r->backend_calls++;   /* turn_end */
            }
            return;
        }
    }
    r->end = l.last;
}

static void fill(float *lv, int from, int to, float db)
{
    for (int i = from; i < to; i++) {
        lv[i] = db;
    }
}

static void run_wake(const float *levels, int n, const vw_config_t *cfg, float floor_db, run_t *r)
{
    run_wake_press(levels, n, cfg, floor_db, -1, r);
}

/* A press during the listen: before the onset, or on the onset chunk itself, no turn is ever begun. */
static void test_press_during_listen(void)
{
    vw_config_t c;
    vw_config_default(&c);
    static float lv[MAXC];
    fill(lv, 0, MAXC, QUIET);
    int speak_from = 60;
    fill(lv, speak_from, speak_from + 100, SPEECH);
    int onset = speak_from + c.onset_chunks - 1;   /* the chunk VW_ONSET comes on */
    run_t r;
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.begun == 1);   /* without a press this is a turn */

    run_wake_press(lv, MAXC, &c, FLOOR, onset, &r);
    CHECK(r.pressed && r.end == VW_ONSET);
    CHECK(r.begun == 0 && r.backend_calls == 0 && r.cancels == 0);   /* no begin, so nothing to cancel */

    for (int at = 0; at < onset; at++) {
        run_wake_press(lv, MAXC, &c, FLOOR, at, &r);
        CHECK(r.pressed && r.begun == 0 && r.backend_calls == 0 && r.cancels == 0);
    }
    /* After the onset the turn was live: the press cancels it (the press then records as its own turn). */
    run_wake_press(lv, MAXC, &c, FLOOR, onset + 5, &r);
    CHECK(r.pressed && r.begun == 1 && r.cancels == 1);
}

/* ---- Tests ---- */

static void test_config(void)
{
    vw_config_t c;
    vw_config_default(&c);
    CHECK(vw_config_ok(&c));
    CHECK(c.no_speech_chunks == 250);     /* 5 s */
    CHECK(c.end_silence_chunks == 50);    /* 1.0 s */
    CHECK(c.max_chunks == 750);           /* 15 s, muse_voice.c's MAX_SECS */
    CHECK(c.guard_chunks == 10 && c.onset_chunks == 4 && c.lead_chunks == 15);
    vw_config_t b = c;
    b.onset_chunks = 0;
    CHECK(!vw_config_ok(&b));
    b = c;
    b.no_speech_chunks = c.max_chunks + 1;   /* the pre-onset wait must fit the note buffer */
    CHECK(!vw_config_ok(&b));
    b = c;
    b.silence_margin_db = c.onset_margin_db + 1;
    CHECK(!vw_config_ok(&b));
    b = c;
    b.no_speech_chunks = b.guard_chunks;
    CHECK(!vw_config_ok(&b));
}

/* A silent listen after a wake: no onset, zero backend calls, ends in exactly 5 s. */
static void test_silent_wake_no_backend(void)
{
    vw_config_t c;
    vw_config_default(&c);
    static float lv[MAXC];
    fill(lv, 0, MAXC, QUIET);
    run_t r;
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_NO_SPEECH);
    CHECK(r.backend_calls == 0);
    CHECK(r.begun == 0 && r.nsent == 0);
    CHECK(r.chunks == c.no_speech_chunks);

    /* The wake word's own tail right after the detection (inside the guard) is not speech. */
    fill(lv, 0, c.guard_chunks, SPEECH);
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_NO_SPEECH && r.backend_calls == 0);

    /* Clicks and short noises (shorter than the onset run) never start a turn. */
    fill(lv, 0, MAXC, QUIET);
    for (int i = c.guard_chunks; i < c.no_speech_chunks; i += 7) {
        for (int k = 0; k < c.onset_chunks - 1 && i + k < MAXC; k++) {
            lv[i + k] = -5.0f;
        }
    }
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_NO_SPEECH && r.backend_calls == 0);

    /* Steady room noise at the floor, even a loud floor, is not speech. */
    fill(lv, 0, MAXC, -35.0f);
    run_wake(lv, MAXC, &c, -36.0f, &r);
    CHECK(r.end == VW_NO_SPEECH && r.backend_calls == 0);

    /* Sound just under the speech level (floor + 9.9 dB) for the whole wait: nothing sent. */
    fill(lv, 0, MAXC, FLOOR + c.onset_margin_db - 0.1f);
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_NO_SPEECH && r.backend_calls == 0);

    /* Below the absolute minimum, however far above a very quiet floor: nothing sent. */
    fill(lv, 0, MAXC, -61.0f);
    run_wake(lv, MAXC, &c, -84.0f, &r);
    CHECK(r.end == VW_NO_SPEECH && r.backend_calls == 0);

    /* NaN levels count as silence. */
    fill(lv, 0, MAXC, NAN);
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_NO_SPEECH && r.backend_calls == 0);
}

/* "Computer ... (pause) ... what time is it": onset, lead-in, end on trailing silence. */
static void test_wake_turn(void)
{
    vw_config_t c;
    vw_config_default(&c);
    static float lv[MAXC];
    fill(lv, 0, MAXC, QUIET);
    int speak_from = 60, speak_to = 160;   /* 1.2 s after the wake, 2 s of speech */
    fill(lv, speak_from, speak_to, SPEECH);
    run_t r;
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_END_SILENCE);
    CHECK(r.begun == 1);
    CHECK(r.calls_before_onset == 0);                      /* nothing before speech */
    CHECK(r.nsent > 0 && r.sent[0] == speak_from - c.lead_chunks);   /* 300 ms lead-in */
    for (int k = 1; k < r.nsent; k++) {
        CHECK(r.sent[k] == r.sent[k - 1] + 1);            /* contiguous, in order */
    }
    CHECK(r.sent[r.nsent - 1] == speak_to - 1 + c.end_silence_chunks);   /* ends 1 s after the last word */
    CHECK(r.chunks == speak_to + c.end_silence_chunks);

    /* Speech straight after the wake word: the lead never reaches before the detection. */
    fill(lv, 0, MAXC, QUIET);
    fill(lv, 0, 100, SPEECH);
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_END_SILENCE && r.begun == 1);
    CHECK(r.sent[0] == 0);   /* from the detection: nothing earlier exists in the turn */
    for (int k = 0; k < r.nsent; k++) {
        CHECK(r.sent[k] >= 0);
    }

    /* Onset on the very last chunk of the wait still counts (speech wins the tie). */
    fill(lv, 0, MAXC, QUIET);
    fill(lv, c.no_speech_chunks - c.onset_chunks, c.no_speech_chunks + 20, SPEECH);
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.begun == 1 && r.end == VW_END_SILENCE);
    /* One chunk later is too late. */
    fill(lv, 0, MAXC, QUIET);
    fill(lv, c.no_speech_chunks - c.onset_chunks + 1, c.no_speech_chunks + 20, SPEECH);
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.begun == 0 && r.end == VW_NO_SPEECH && r.backend_calls == 0);

    /* Pauses shorter than the end silence don't end the turn. */
    fill(lv, 0, MAXC, QUIET);
    fill(lv, 30, 80, SPEECH);
    fill(lv, 80 + c.end_silence_chunks - 1, 140, SPEECH);   /* a 0.98 s pause */
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_END_SILENCE && r.sent[r.nsent - 1] == 139 + c.end_silence_chunks);

    /* Talking without pause hits the 15 s cap, counted from the turn's start. */
    fill(lv, 0, MAXC, SPEECH);
    fill(lv, 0, 40, QUIET);
    run_wake(lv, MAXC, &c, FLOOR, &r);
    CHECK(r.end == VW_END_MAX);
    CHECK(r.nsent == c.max_chunks);
    CHECK(r.sent[0] == 40 - c.lead_chunks);
}

static void test_verdict_sequence(void)
{
    vw_config_t c;
    vw_config_default(&c);
    vw_listen_t l;
    vw_listen_start(&l, &c, FLOOR);
    CHECK(!vw_listen_may_contact_backend(&l) && vw_listen_lead(&l) == -1);
    for (int i = 0; i < c.guard_chunks; i++) {
        CHECK(vw_listen_chunk(&l, SPEECH) == VW_WAITING);
    }
    for (int i = 0; i < c.onset_chunks - 1; i++) {
        CHECK(vw_listen_chunk(&l, SPEECH) == VW_WAITING);
        CHECK(!vw_listen_may_contact_backend(&l));
    }
    CHECK(vw_listen_chunk(&l, SPEECH) == VW_ONSET);
    CHECK(vw_listen_may_contact_backend(&l));
    CHECK(vw_listen_lead(&l) == 0);   /* guard 10 - lead 15 < 0: clamped to the detection */
    CHECK(vw_listen_chunk(&l, SPEECH) == VW_SPEAKING);
    /* Between silence and speech levels: neither ends nor counts as silence. */
    for (int i = 0; i < 200; i++) {
        CHECK(vw_listen_chunk(&l, FLOOR + 8.0f) == VW_SPEAKING);
    }
    for (int i = 0; i < c.end_silence_chunks - 1; i++) {
        CHECK(vw_listen_chunk(&l, QUIET) == VW_SPEAKING);
    }
    CHECK(vw_listen_chunk(&l, QUIET) == VW_END_SILENCE);
    /* Ended stays ended, whatever comes. */
    CHECK(vw_listen_chunk(&l, SPEECH) == VW_END_SILENCE);
    CHECK(vw_listen_may_contact_backend(&l));

    /* A NaN floor is treated as the initial floor; a crazy one is clamped. */
    vw_listen_start(&l, &c, NAN);
    CHECK(l.speech_db == VW_FLOOR_INIT_DB + c.onset_margin_db);
    vw_listen_start(&l, &c, 50.0f);
    CHECK(l.speech_db == VW_FLOOR_MAX_DB + c.onset_margin_db);
}

static void test_floor(void)
{
    vw_floor_t f;
    vw_floor_init(&f);
    CHECK(f.db == VW_FLOOR_INIT_DB);
    for (int i = 0; i < 200; i++) {
        vw_floor_update(&f, -70.0f);
    }
    CHECK(fabsf(f.db - -70.0f) < 0.01f);
    /* 1 s of speech at idle (someone talking, or the wake word) moves it < 3 dB. */
    for (int i = 0; i < 50; i++) {
        vw_floor_update(&f, -25.0f);
    }
    CHECK(f.db < -67.4f && f.db > -70.0f);
    /* It comes back down fast. */
    for (int i = 0; i < 20; i++) {
        vw_floor_update(&f, -70.0f);
    }
    CHECK(fabsf(f.db - -70.0f) < 0.1f);
    /* A louder room is learned within ~10 s. */
    for (int i = 0; i < 500; i++) {
        vw_floor_update(&f, -50.0f);
    }
    CHECK(fabsf(f.db - -50.0f) < 0.01f);
    /* Never above the chunk that raised it. */
    vw_floor_init(&f);
    vw_floor_update(&f, -59.99f);
    CHECK(f.db <= -59.99f);
    /* Clamped: digital silence or a glitch can't drag it to -inf; NaN is ignored. */
    for (int i = 0; i < 100; i++) {
        vw_floor_update(&f, -150.0f);
    }
    CHECK(f.db == VW_FLOOR_MIN_DB);
    vw_floor_update(&f, NAN);
    CHECK(f.db == VW_FLOOR_MIN_DB);
    for (int i = 0; i < 5000; i++) {
        vw_floor_update(&f, 0.0f);
    }
    CHECK(f.db == VW_FLOOR_MAX_DB);
}

static void test_reblock(void)
{
    /* 320-sample mic chunks into 512-sample WakeNet chunks (and other sizes). */
    const size_t sizes[] = { 512, 480, 320, 160, 1, 1024 };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t size = sizes[s];
        int16_t *blk = calloc(size, sizeof(int16_t));
        vw_reblock_t r;
        vw_reblock_init(&r, blk, size);
        int16_t mic[320];
        int next_in = 0, next_out = 0, blocks = 0;
        for (int chunk = 0; chunk < 64; chunk++) {
            for (int i = 0; i < 320; i++) {
                mic[i] = (int16_t)(next_in++ & 0x7fff);
            }
            size_t off = 0;
            while (off < 320) {
                size_t took = vw_reblock_push(&r, mic + off, 320 - off);
                off += took;
                if (vw_reblock_full(&r)) {
                    for (size_t i = 0; i < size; i++) {
                        CHECK(blk[i] == (int16_t)(next_out++ & 0x7fff));
                    }
                    blocks++;
                    vw_reblock_clear(&r);
                } else {
                    CHECK(off == 320);   /* only stops early when the block is full */
                }
            }
        }
        CHECK(blocks == (int)(64 * 320 / size));
        CHECK(r.fill == (64 * 320) % size);
        CHECK(vw_reblock_push(&r, mic, 0) == 0);
        free(blk);
    }
    vw_reblock_t z;
    vw_reblock_init(&z, NULL, 0);
    CHECK(!vw_reblock_full(&z));
    int16_t one = 1;
    CHECK(vw_reblock_push(&z, &one, 1) == 0);
}

static void test_parse(void)
{
    int pm = -1;
    CHECK(vw_parse_threshold("0.65", &pm) == VW_PARSE_OK && pm == 650);
    CHECK(vw_parse_threshold(".7", &pm) == VW_PARSE_OK && pm == 700);
    CHECK(vw_parse_threshold("0.655", &pm) == VW_PARSE_OK && pm == 655);
    CHECK(vw_parse_threshold("0.6555", &pm) == VW_PARSE_OK && pm == 656);   /* rounded */
    CHECK(vw_parse_threshold("0.5", &pm) == VW_PARSE_OK && pm == 500);
    CHECK(vw_parse_threshold("0.99", &pm) == VW_PARSE_OK && pm == 990);
    CHECK(vw_parse_threshold("0.995", &pm) == VW_PARSE_CLAMPED && pm == 990);
    CHECK(vw_parse_threshold("1", &pm) == VW_PARSE_CLAMPED && pm == 990);
    CHECK(vw_parse_threshold("1.0", &pm) == VW_PARSE_CLAMPED && pm == 990);
    CHECK(vw_parse_threshold("0.4", &pm) == VW_PARSE_CLAMPED && pm == 500);
    CHECK(vw_parse_threshold("0", &pm) == VW_PARSE_CLAMPED && pm == 500);
    CHECK(vw_parse_threshold("0.", &pm) == VW_PARSE_CLAMPED && pm == 500);
    pm = 123;
    const char *bad[] = { "", ".", "-0.6", "+0.6", "0.6 ", " 0.6", "0,6", "65", "650", "1.0001", "1.5",
                          "0.65555", "0.6.5", "abc", "0x1", "6e-1", "nan", "inf", "0.6a", "00.6" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (vw_parse_threshold(bad[i], &pm) != VW_PARSE_INVALID) {
            fprintf(stderr, "accepted bad threshold \"%s\"\n", bad[i]);
            CHECK(0);
        }
    }
    CHECK(vw_parse_threshold(NULL, &pm) == VW_PARSE_INVALID);
    CHECK(pm == 123);   /* untouched on invalid input */

    CHECK(vw_clamp_threshold(0) == VW_THRESHOLD_MIN);
    CHECK(vw_clamp_threshold(-5) == VW_THRESHOLD_MIN);
    CHECK(vw_clamp_threshold(65535) == VW_THRESHOLD_MAX);
    CHECK(vw_clamp_threshold(650) == 650);

    bool on = false;
    CHECK(vw_parse_onoff("on", &on) && on);
    CHECK(vw_parse_onoff("off", &on) && !on);
    on = true;
    CHECK(!vw_parse_onoff("ON", &on) && on);
    CHECK(!vw_parse_onoff("", &on));
    CHECK(!vw_parse_onoff("1", &on));
    CHECK(!vw_parse_onoff("onn", &on));
    CHECK(!vw_parse_onoff(NULL, &on));
}

static unsigned next_rand(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (*seed >> 16) & 0x7fff;
}

/* Random level sequences: whatever the room does, the properties hold. */
static void test_fuzz(void)
{
    vw_config_t c;
    vw_config_default(&c);
    static float lv[MAXC];
    unsigned seed = 12345;
    int turns = 0, silent = 0;
    for (int round = 0; round < 20000; round++) {
        float floor_db = -80.0f + (float)((int)next_rand(&seed) % 50);
        int n = 1 + (int)next_rand(&seed) % MAXC;
        float p_speech = (float)((int)next_rand(&seed) % 100) / 100.0f;
        for (int i = 0; i < n; i++) {
            float u = (float)((int)next_rand(&seed) % 1000) / 1000.0f;
            lv[i] = u < p_speech ? floor_db + (float)((int)next_rand(&seed) % 40)
                                 : floor_db - 5.0f + (float)((int)next_rand(&seed) % 8);
        }
        run_t r;
        run_wake(lv, n, &c, floor_db, &r);
        CHECK(r.calls_before_onset == 0);
        CHECK(r.begun <= 1);
        if (r.end == VW_NO_SPEECH) {
            CHECK(r.backend_calls == 0 && r.nsent == 0);
            CHECK(r.chunks == c.no_speech_chunks);
            silent++;
        }
        if (r.begun) {
            turns++;
            CHECK(r.sent[0] >= 0);
            CHECK(r.nsent <= c.max_chunks);
            CHECK(r.end != VW_NO_SPEECH);
        } else {
            CHECK(r.backend_calls == 0);
        }
    }
    CHECK(turns > 1000 && silent > 1000);   /* both paths were exercised */
}

int main(void)
{
    test_config();
    test_silent_wake_no_backend();
    test_wake_turn();
    test_verdict_sequence();
    test_press_during_listen();
    test_floor();
    test_reblock();
    test_parse();
    test_fuzz();
    if (s_fail) {
        fprintf(stderr, "test_vesper_wake: %d of %d checks failed\n", s_fail, s_checks);
        return 1;
    }
    printf("test_vesper_wake: all %d checks passed\n", s_checks);
    return 0;
}
