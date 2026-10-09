/*
 * Wake word (task 22): the pure-C pieces around the "Hey Vesper"
 * microWakeWord model, shared by the firmware (vesper_wakeword_engine.cc)
 * and the host tests.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * The model's contract (firmware/wakeword/README.md, "Integration note";
 * firmware/wakeword/metrics.json, "decision_rule"):
 *
 *   - the TFLM micro_speech frontend turns 16 kHz mono audio into one frame
 *     of 40 uint16 features every 10 ms (30 ms window);
 *   - vm_quant_*  turns a uint16 feature into the model's int8 input exactly
 *                 as train/eval.py scored it: q = round(f / 25.6 / scale) + zp,
 *                 rounded half to even in double precision, clamped to int8;
 *   - vm_stack_*  groups 3 frames (the model's stride) into one [1, 3, 40]
 *                 input, oldest first: one inference every 30 ms;
 *   - vm_detect_* the decision rule: keep the last W uint8 outputs and detect
 *                 when their sum > round(cutoff * 255) * W. The first 25
 *                 inferences after a reset never detect (the frontend and the
 *                 streaming state settle), nor do the 25 after a detection.
 *                 This is train/eval.py's count_detections(), step by step.
 *
 * Like vesper_wake.c this does no I/O, keeps no clock and never allocates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_FEATURES 40        /* mel channels per 10 ms frame */
#define VM_STRIDE 3           /* frames per inference (the model's first-layer stride) */
#define VM_COOLDOWN 25        /* inferences ignored after a reset and after a detection (0.75 s) */
#define VM_WINDOW_MAX 10      /* the longest sliding window vm_detect_init accepts */
#define VM_FEATURE_SCALE 25.6 /* frontend uint16 = float feature * 25.6 (microWakeWord's convention) */

/* ---- uint16 frontend feature -> int8 model input ---- */

#define VM_QUANT_LUT 1024   /* features at or above this all map to lut[VM_QUANT_LUT - 1] */

typedef struct {
    int8_t lut[VM_QUANT_LUT];
} vm_quant_t;

/* From the model input tensor's quantisation (scale > 0, zero point in int8).
 * False if the parameters are unusable: a non-positive or non-finite scale,
 * a zero point outside int8, or a scale so small that the table doesn't reach
 * saturation (then it can't stand for every uint16). */
bool vm_quant_init(vm_quant_t *q, float scale, int zero_point);
int8_t vm_quant(const vm_quant_t *q, uint16_t feature);
/* The same value computed directly (for the tests; the firmware uses the table). */
int8_t vm_quant_direct(uint16_t feature, float scale, int zero_point);

/* ---- 3 frames -> one model input ---- */

typedef struct {
    int8_t in[VM_STRIDE * VM_FEATURES];   /* row-major [frame][feature], oldest frame first */
    int frames;
} vm_stack_t;

void vm_stack_reset(vm_stack_t *s);
/* One frame of VM_FEATURES uint16 features. True when it completed an input
 * (s->in is ready for the model); the next push starts a new one. */
bool vm_stack_push(vm_stack_t *s, const vm_quant_t *q, const uint16_t *features);

/* ---- The decision rule ---- */

typedef struct {
    uint8_t win[VM_WINDOW_MAX];
    int w;            /* window length */
    int pos;          /* next slot to overwrite */
    unsigned sum;     /* sum of win[0..w) */
    unsigned need;    /* detect when sum > need: round(cutoff * 255) * w */
    int ignore;       /* inferences still ignored (cooldown) */
    unsigned n;       /* inferences since the reset */
    uint8_t peak;     /* highest single output since the reset */
    unsigned peak_sum;/* highest window sum since the reset (outside the cooldown) */
} vm_detect_t;

/* round(permille / 1000 * 255), half to even (Python's round(), as eval.py's thr_u8). */
int vm_cutoff_u8(int permille);
/* window in [1, VM_WINDOW_MAX] (clamped); cutoff in permille (any int, clamped to [0, 1000]). */
void vm_detect_init(vm_detect_t *d, int window, int cutoff_permille);
/* A new cutoff, keeping the window's contents. */
void vm_detect_set_cutoff(vm_detect_t *d, int cutoff_permille);
/* Empty window (zeros, as eval.py's moving sum before a track starts) and a fresh cooldown. */
void vm_detect_reset(vm_detect_t *d);
/* One inference's uint8 output. True: the wake word was detected. */
bool vm_detect_push(vm_detect_t *d, uint8_t p);

#ifdef __cplusplus
}
#endif
