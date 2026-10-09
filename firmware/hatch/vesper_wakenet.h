/*
 * Wake word (task 20): ESP-SR WakeNet on the AIPI's idle mic feed. The
 * decisions around it (noise floor, the listen after a wake, WakeNet's chunk
 * size, the settings) are vesper_wake.c's, host-tested.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * The model (CONFIG_VESPER_WAKE_MODEL, "wn9_computer_tts": "Computer") is
 * packed into the app image at build time (components/muse/CMakeLists.txt)
 * and loaded from there with esp-sr's srmodel_load(), so an OTA update
 * delivers it and the partition table is unchanged. If it can't be loaded
 * the wake word is off, says so once in the log and in >status, and push to
 * talk is unaffected.
 *
 * Threads: vesper_wakenet_init() and the setters run on tasks with internal
 * RAM stacks (they touch NVS); vesper_wakenet_feed()/_reset() run on the
 * voice task only. The setters hand their values over through atomics, which
 * the voice task applies before its next detection: WakeNet itself is only
 * ever called from one task.
 *
 * NVS namespace "muse": wake_on (u8, 0/1), wake_thr (u16, permille). Serial:
 * >wake=on|off, >wake.threshold=0.65, >wake (show).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the model and creates WakeNet (boot, before the voice task starts).
 * False: the wake word is unavailable (logged); everything else works. */
bool vesper_wakenet_init(void);

/* Loaded and switched on. */
bool vesper_wakenet_on(void);

/* One idle 20 ms mic chunk (any length) and its level in dBFS. Updates the
 * noise floor and runs WakeNet whenever a block of its chunk size is full.
 * True: the wake word was just heard. Voice task only, idle only. */
bool vesper_wakenet_feed(const int16_t *pcm, size_t n, float chunk_db);

/* Forgets what WakeNet has heard so far (a partial block, its internal
 * window): after Muse played anything, and after every turn or listen. */
void vesper_wakenet_reset(void);

/* The idle noise floor (dBFS) that a listen after a wake is measured against. */
float vesper_wakenet_floor_db(void);

/* How a listen after a wake ended, for >status. */
typedef enum {
    VESPER_WAKE_TURN,        /* speech followed: a turn ran */
    VESPER_WAKE_NO_SPEECH,   /* nobody spoke: back to idle, nothing sent */
    VESPER_WAKE_PRESSED,     /* the talk button took over */
    VESPER_WAKE_NOT_READY,   /* not set up, or no room for the note: nothing recorded */
} vesper_wake_outcome_t;
void vesper_wakenet_count(vesper_wake_outcome_t outcome);

/* Console (serial task). They save to NVS and print nothing themselves. */
bool vesper_wakenet_set_on(bool on);
bool vesper_wakenet_set_threshold(int permille);
int vesper_wakenet_threshold(void);

/* {"state":"on|off|no_model|failed|disabled","model":"...","threshold":0.650,"chunk":512,
 *  "wakes":N,"turns":N,"no_speech":N,"pressed":N,"detect_us":{"avg":N,"max":N},"floor_db":-62.0}
 * for >status: counts and settings only, never audio or text. */
int vesper_wakenet_status_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
