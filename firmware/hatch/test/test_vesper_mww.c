/*
 * Host tests for firmware/hatch/vesper_mww.c (the "Hey Vesper" model's
 * quantiser, frame stacker and decision rule, task 22). Run: make -C firmware test
 *
 * The decision rule is checked against a direct C port of train/eval.py's
 * count_detections() (moving sum with zeros before the start, the 25-inference
 * start and post-detection cooldowns) on fixed cases and 20,000 random streams.
 * The quantiser is checked against numpy's values for train/eval.py's formula
 * (np.round(u16 / 25.6 / scale) + zp, clipped): the expected checksums below were
 * computed with numpy from that exact expression.
 */
#include "vesper_mww.h"

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

/* The shipped model's input quantisation (firmware/wakeword/metrics.json). */
#define MODEL_SCALE 0.10196078568696976f
#define MODEL_ZP (-128)

static long lut_checksum(const vm_quant_t *q)
{
    long sum = 0;
    for (int i = 0; i < VM_QUANT_LUT; i++) {
        sum += (long)(i + 1) * (q->lut[i] + 128);
    }
    return sum;
}

static void test_quant(void)
{
    vm_quant_t q;
    CHECK(vm_quant_init(&q, MODEL_SCALE, MODEL_ZP));
    /* numpy: sum((i+1)*(lut[i]+128)) = 114910097; first values; saturation at 665. */
    CHECK(lut_checksum(&q) == 114910097L);
    static const int8_t first[8] = { -128, -128, -127, -127, -126, -126, -126, -125 };
    for (int i = 0; i < 8; i++) {
        CHECK(vm_quant(&q, (uint16_t)i) == first[i]);
    }
    static const struct {
        uint16_t f;
        int8_t q;
    } pts[] = { { 12, -123 }, { 13, -123 }, { 100, -90 }, { 333, 0 }, { 600, 102 }, { 664, 126 }, { 665, 127 } };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
        CHECK(vm_quant(&q, pts[i].f) == pts[i].q);
        CHECK(vm_quant_direct(pts[i].f, MODEL_SCALE, MODEL_ZP) == pts[i].q);
    }
    /* Beyond the table: saturated, like the direct value. */
    CHECK(vm_quant(&q, 1024) == 127 && vm_quant(&q, 65535) == 127);
    CHECK(vm_quant_direct(65535, MODEL_SCALE, MODEL_ZP) == 127);
    /* Monotone, and the table is the direct formula. */
    for (int i = 0; i < VM_QUANT_LUT; i++) {
        CHECK(q.lut[i] == vm_quant_direct((uint16_t)i, MODEL_SCALE, MODEL_ZP));
        if (i) {
            CHECK(q.lut[i] >= q.lut[i - 1]);
        }
    }
    /* Stock microWakeWord models' scale 26/256: numpy checksum 115057751 (the float formula;
     * ESPHome's integer form differs from it on 51 half-way entries, which is why the
     * firmware computes the table from the model's own parameters rather than hard-coding). */
    vm_quant_t s;
    CHECK(vm_quant_init(&s, 0.1015625f, -128));
    CHECK(lut_checksum(&s) == 115057751L);
    /* Unusable parameters. */
    CHECK(!vm_quant_init(&s, 0.0f, -128));
    CHECK(!vm_quant_init(&s, -0.1f, -128));
    CHECK(!vm_quant_init(&s, NAN, -128));
    CHECK(!vm_quant_init(&s, INFINITY, -128));
    CHECK(!vm_quant_init(&s, 0.1f, -129));
    CHECK(!vm_quant_init(&s, 0.1f, 128));
    CHECK(!vm_quant_init(&s, 1.0f, -128));     /* the table wouldn't reach saturation */
}

static void test_stack(void)
{
    vm_quant_t q;
    CHECK(vm_quant_init(&q, MODEL_SCALE, MODEL_ZP));
    vm_stack_t s;
    vm_stack_reset(&s);
    uint16_t frame[VM_FEATURES];
    int inputs = 0;
    for (int f = 0; f < 30; f++) {
        for (int i = 0; i < VM_FEATURES; i++) {
            frame[i] = (uint16_t)(f * 20 + i);   /* frame f's values identify it */
        }
        bool full = vm_stack_push(&s, &q, frame);
        CHECK(full == (f % VM_STRIDE == VM_STRIDE - 1));
        if (full) {
            inputs++;
            /* Oldest first: rows are frames f-2, f-1, f. */
            for (int r = 0; r < VM_STRIDE; r++) {
                int src = f - (VM_STRIDE - 1) + r;
                for (int i = 0; i < VM_FEATURES; i++) {
                    CHECK(s.in[r * VM_FEATURES + i] == vm_quant(&q, (uint16_t)(src * 20 + i)));
                }
            }
        }
    }
    CHECK(inputs == 10);
    /* A reset drops a partial input. */
    vm_stack_reset(&s);
    CHECK(!vm_stack_push(&s, &q, frame));
    vm_stack_reset(&s);
    CHECK(!vm_stack_push(&s, &q, frame) && !vm_stack_push(&s, &q, frame) && vm_stack_push(&s, &q, frame));
}

static void test_cutoff(void)
{
    /* Python round(cutoff * 255): banker's rounding on the exact halves. */
    CHECK(vm_cutoff_u8(650) == 166);   /* the shipped point: need 498 for W = 3 */
    CHECK(vm_cutoff_u8(500) == 128);   /* 127.5 -> 128 */
    CHECK(vm_cutoff_u8(700) == 178);   /* 178.5 -> 178 */
    CHECK(vm_cutoff_u8(900) == 230);   /* 229.5 -> 230 */
    CHECK(vm_cutoff_u8(100) == 26);    /* 25.5 -> 26 */
    CHECK(vm_cutoff_u8(300) == 76);    /* 76.5 -> 76 */
    CHECK(vm_cutoff_u8(990) == 252);
    CHECK(vm_cutoff_u8(0) == 0 && vm_cutoff_u8(1000) == 255);
    CHECK(vm_cutoff_u8(-5) == 0 && vm_cutoff_u8(5000) == 255);
    vm_detect_t d;
    vm_detect_init(&d, 3, 650);
    CHECK(d.need == 498 && d.w == 3);
    vm_detect_init(&d, 0, 650);
    CHECK(d.w == 1);
    vm_detect_init(&d, 99, 650);
    CHECK(d.w == VM_WINDOW_MAX);
}

/* train/eval.py count_detections(p, w, cutoff), ported: returns the count and
 * fills det[i] = 1 where a detection is counted. */
static int ref_count(const uint8_t *p, int n, int w, int permille, char *det)
{
    long need = (long)vm_cutoff_u8(permille) * w;
    int count = 0, next_ok = VM_COOLDOWN;
    for (int i = 0; i < n; i++) {
        long s = 0;
        for (int k = i - w + 1; k <= i; k++) {
            s += k >= 0 ? p[k] : 0;   /* zeros before the start */
        }
        det[i] = 0;
        if (s > need && i >= next_ok) {
            count++;
            det[i] = 1;
            next_ok = i + VM_COOLDOWN + 1;
        }
    }
    return count;
}

static int run_detect(const uint8_t *p, int n, int w, int permille, char *det)
{
    vm_detect_t d;
    vm_detect_init(&d, w, permille);
    int count = 0;
    for (int i = 0; i < n; i++) {
        det[i] = vm_detect_push(&d, p[i]);
        count += det[i];
    }
    return count;
}

static void test_detect_cases(void)
{
    uint8_t p[200];
    char a[200], b[200];
    /* A loud start is never a detection: the first 25 inferences are ignored. */
    memset(p, 255, sizeof(p));
    CHECK(run_detect(p, 25, 3, 650, a) == 0);
    /* The 26th inference (index 25) can be one; then every 26th. */
    CHECK(run_detect(p, 26, 3, 650, a) == 1 && a[25]);
    CHECK(run_detect(p, 200, 3, 650, a) == ref_count(p, 200, 3, 650, b) && !memcmp(a, b, 200));
    CHECK(a[25] && a[51] && !a[50] && !a[26]);
    /* Exactly at the threshold is not above it: 166 x 3 = 498 is not > 498. */
    memset(p, 166, sizeof(p));
    CHECK(run_detect(p, 200, 3, 650, a) == 0);
    memset(p, 167, sizeof(p));
    CHECK(run_detect(p, 200, 3, 650, a) == 7);   /* 25, 51, 77, 103, 129, 155, 181 */
    /* A single spike can't pass a W = 3 window; three in a row can. */
    memset(p, 0, sizeof(p));
    p[60] = 255;
    CHECK(run_detect(p, 200, 3, 650, a) == 0);
    p[61] = 255;
    CHECK(run_detect(p, 200, 3, 650, a) == 1 && a[61]);   /* 510 > 498: two at 255 already pass */
    p[61] = 200;
    CHECK(run_detect(p, 200, 3, 650, a) == 0);   /* 455 */
    p[62] = 100;
    CHECK(run_detect(p, 200, 3, 650, a) == 1 && a[62]);   /* 555 */
    /* The window keeps filling in the cooldown (eval.py's moving sum does). */
    memset(p, 0, sizeof(p));
    p[23] = p[24] = 255;   /* in the start cooldown */
    p[25] = 100;           /* 610 at index 25 */
    CHECK(run_detect(p, 200, 3, 650, a) == 1 && a[25]);
    CHECK(ref_count(p, 200, 3, 650, b) == 1 && b[25]);
}

static void test_detect_state(void)
{
    vm_detect_t d;
    vm_detect_init(&d, 3, 650);
    for (int i = 0; i < 30; i++) {
        vm_detect_push(&d, (uint8_t)(i == 27 ? 250 : 10));
    }
    CHECK(d.n == 30 && d.peak == 250);
    CHECK(d.peak_sum == 270);   /* 250 + 10 + 10, outside the cooldown */
    /* A reset empties the window and restarts the cooldown. */
    vm_detect_reset(&d);
    CHECK(d.n == 0 && d.peak == 0 && d.sum == 0 && d.ignore == VM_COOLDOWN);
    int fired = 0;
    for (int i = 0; i < 25; i++) {
        fired += vm_detect_push(&d, 255);
    }
    CHECK(fired == 0);
    CHECK(vm_detect_push(&d, 255));
    /* A new cutoff keeps the window: at 0.99 (need 756) a full window of 255 (765) still passes. */
    vm_detect_reset(&d);
    for (int i = 0; i < 25; i++) {
        vm_detect_push(&d, 252);
    }
    vm_detect_set_cutoff(&d, 990);
    CHECK(d.need == 756);
    CHECK(!vm_detect_push(&d, 252));   /* 756 is not > 756 */
    CHECK(vm_detect_push(&d, 253));    /* 757 */
}

static unsigned s_rng = 22u;
static unsigned rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 8) & 0xffffffu;
}

static void test_fuzz(void)
{
    enum { N = 400 };
    uint8_t p[N];
    char a[N], b[N];
    static const int ws[] = { 1, 2, 3, 5, 7, 10 };
    int mismatches = 0, detections = 0;
    for (int round = 0; round < 20000; round++) {
        int w = ws[rnd() % 6];
        int permille = 500 + (int)(rnd() % 500);
        int mode = (int)(rnd() % 3);
        for (int i = 0; i < N; i++) {
            /* quiet with bursts (wake-word-like), uniform noise, or near-threshold. */
            unsigned r = rnd();
            p[i] = mode == 0   ? (uint8_t)((r % 100) < 8 ? 200 + r % 56 : r % 40)
                   : mode == 1 ? (uint8_t)(r & 0xff)
                               : (uint8_t)(vm_cutoff_u8(permille) - 3 + (int)(r % 7));
        }
        int c1 = run_detect(p, N, w, permille, a);
        int c2 = ref_count(p, N, w, permille, b);
        detections += c1;
        if (c1 != c2 || memcmp(a, b, N)) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0);
    CHECK(detections > 1000);   /* the fuzz actually exercised detections */
}

int main(void)
{
    test_quant();
    test_stack();
    test_cutoff();
    test_detect_cases();
    test_detect_state();
    test_fuzz();
    if (s_fail) {
        fprintf(stderr, "test_vesper_mww: %d of %d checks failed\n", s_fail, s_checks);
        return 1;
    }
    printf("test_vesper_mww: all %d checks passed\n", s_checks);
    return 0;
}
