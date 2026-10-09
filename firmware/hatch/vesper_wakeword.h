/*
 * Wake word (task 22): "Hey Vesper" on the AIPI's idle mic feed. The engine
 * (frontend, model, decision rule) is vesper_wakeword_engine.cc; the
 * decisions around a wake (noise floor, the listen after it, the settings)
 * are vesper_wake.c's, host-tested. This file is the firmware's side: the
 * model in the image, the arena, NVS, the counters, the status JSON.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * The model (firmware/wakeword/hey-vesper.tflite, the task 21 microWakeWord
 * model) is linked into the app image at build time
 * (components/muse/CMakeLists.txt, vesper_hv_model.S), so an OTA update
 * delivers it and the partition table is unchanged. If it can't be started
 * the wake word is off, says so once in the log and in >status, and push to
 * talk is unaffected.
 *
 * Threads: vesper_wakeword_init() and the setters run on tasks with internal
 * RAM stacks (they touch NVS); vesper_wakeword_feed()/_reset() run on the
 * voice task only. The setters hand their values over through atomics, which
 * the voice task applies before its next feed: the engine is only ever
 * called from one task.
 *
 * NVS namespace "muse": wake_on (u8, 0/1, as in 1.0.3), wake_hv_thr (u16,
 * permille; a new key, so 1.0.3's WakeNet threshold, wake_thr, never applies
 * to this model). Serial: >wake=on|off, >wake.threshold=0.65, >wake (show).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the engine on the packed model (boot, before the voice task starts).
 * False: the wake word is unavailable (logged); everything else works. */
bool vesper_wakeword_init(void);

/* Started and switched on. */
bool vesper_wakeword_on(void);

/* One idle 20 ms mic chunk (any length) and its level in dBFS. Updates the
 * noise floor and runs the engine (an inference every 30 ms of audio).
 * True: "Hey Vesper" was just heard. Voice task only, idle only. */
bool vesper_wakeword_feed(const int16_t *pcm, size_t n, float chunk_db);

/* Forgets what the engine has heard so far (the frontend, the model's
 * streaming state, the sliding window): after Muse played anything, and after
 * every turn or listen. */
void vesper_wakeword_reset(void);

/* The idle noise floor (dBFS) that a listen after a wake is measured against. */
float vesper_wakeword_floor_db(void);

/* How a listen after a wake ended, for >status. */
typedef enum {
    VESPER_WAKE_TURN,        /* speech followed: a turn ran */
    VESPER_WAKE_NO_SPEECH,   /* nobody spoke: back to idle, nothing sent */
    VESPER_WAKE_PRESSED,     /* the talk button took over */
    VESPER_WAKE_NOT_READY,   /* not set up, or no room for the note: nothing recorded */
} vesper_wake_outcome_t;
void vesper_wakeword_count(vesper_wake_outcome_t outcome);

/* Console (serial task). They save to NVS and print nothing themselves. */
bool vesper_wakeword_set_on(bool on);
bool vesper_wakeword_set_threshold(int permille);
int vesper_wakeword_threshold(void);

/* {"state":"on|off|no_model|failed|not_started","model":"hey_vesper","threshold":0.650,"window":3,
 *  "wakes":N,"turns":N,"no_speech":N,"pressed":N,"not_ready":N,"detect_us":{"avg":N,"max":N},
 *  "score_max":0.123,"arena":N,"floor_db":-62.0,"stack_free":N}
 * for >status: counts and settings only, never audio or text. score_max is the highest sliding-
 * window score (0-1, compare it with the threshold) since the last reset. Integer formatting only
 * (no floating-point printf: it runs on the serial task's small stack). VESPER_WAKE_JSON_MAX is
 * room for the longest. */
#define VESPER_WAKE_JSON_MAX 384
int vesper_wakeword_status_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
