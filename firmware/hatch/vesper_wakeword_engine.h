/*
 * Wake word (task 22): the "Hey Vesper" engine. 16 kHz mono audio in,
 * detections out: the TFLM micro_speech frontend (esphome/esp-micro-speech-
 * features, the same C code microWakeWord's training features come from),
 * vesper_mww.c's quantiser, frame stacker and decision rule, and the
 * streaming int8 model on TensorFlow Lite Micro (espressif/esp-tflite-micro).
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * No ESP-IDF calls in here: the same file runs on the device (through
 * vesper_wakeword.c) and on the host (firmware/wakeword/host/, which checks
 * that this pipeline gives the features and detections the model was scored
 * with). No allocation after vwe_init(), except the frontend's own state,
 * which vwe_init() creates once. One caller thread.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vesper_mww.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why vwe_init() failed (a constant string, for one log line). */
typedef const char *vwe_error_t;

/*
 * Starts the engine on a .tflite in memory (kept by the caller for good) with
 * the caller's tensor arena (16-byte aligned, kept for good), the sliding
 * window and the cutoff (permille). Checks the model's contract: a [1, 3, 40]
 * int8 input, a [1, 1] uint8 output, usable quantisation. NULL on success;
 * else why it failed (and the engine stays off).
 */
vwe_error_t vwe_init(const uint8_t *model, size_t model_len, uint8_t *arena, size_t arena_size, int window,
                     int cutoff_permille);

/* Bytes of the arena the model really uses (MicroInterpreter::arena_used_bytes), 0 before init. */
size_t vwe_arena_used(void);

/*
 * Audio (any length). Runs the frontend; every 3 frames (30 ms), one
 * inference and one step of the decision rule. *inferences (may be NULL)
 * gets how many inferences ran. True: "Hey Vesper" was just detected; the
 * rest of this call's audio is dropped (the caller resets after the listen).
 */
bool vwe_feed(const int16_t *pcm, size_t n, unsigned *inferences);

/* Forgets everything heard: the frontend's window and noise estimate, the
 * frames not yet used, the model's streaming state (a fresh interpreter, as
 * eval.py scores each track) and the sliding window, with a new cooldown. */
void vwe_reset(void);

/* A new cutoff from the next inference on. */
void vwe_set_cutoff(int cutoff_permille);

/* The decision rule's state (last outputs, peak since reset), for status. */
const vm_detect_t *vwe_detector(void);

/* The last inference's output (0-255) and how many ran since init. */
uint8_t vwe_last(void);
unsigned long vwe_inferences(void);
/* The last model input ([3][40] int8, oldest frame first): for the host check only. */
const int8_t *vwe_last_input(void);

#ifdef __cplusplus
}
#endif
