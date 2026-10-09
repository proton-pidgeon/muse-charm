/*
 * Wake word (tasks 20, 22): the pure-C decisions around the wake word
 * detector, shared by the firmware (muse_voice.c through vesper_wakeword.c)
 * and the host tests.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * The detector (since task 22 the "Hey Vesper" microWakeWord model,
 * vesper_wakeword_engine.cc; task 20's ESP-SR WakeNet "Computer" is gone)
 * answers one question: was the wake word just said? Everything after that is
 * decided here, so that it can be tested without a board:
 *
 *   - vw_floor_*    the room's noise floor, tracked over the idle mic feed,
 *                   so "speech" means "louder than this room";
 *   - vw_listen_*   the listen that follows a wake: the board records
 *                   locally and may contact the backend only once speech
 *                   starts (onset), ends on trailing silence or the length
 *                   cap, and gives up quietly if nobody speaks;
 *   - vw_parse_*    the console's >wake=on|off and >wake.threshold=0.65 (the
 *                   model's probability cutoff).
 *
 * The privacy and no-phantom-turn rules are properties of vw_listen_t:
 *
 *   1. Audio from before the wake is never part of a wake turn: the turn's
 *      audio is counted in chunks since the detection, and its start
 *      (vw_listen_lead) is never negative.
 *   2. vw_listen_may_contact_backend() is false until onset. The firmware
 *      calls muse_hatch_turn_begin() (the first network step of a turn) only
 *      on the VW_ONSET verdict.
 *   3. A listen with no onset ends with VW_NO_SPEECH after
 *      no_speech_chunks, and VW_NO_SPEECH never follows an onset: a silent
 *      wake costs zero backend calls.
 *
 * Like vesper_proto.c this does no I/O, keeps no clock (time is counted in
 * the 20 ms mic chunks the caller feeds) and never allocates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VW_CHUNK_MS 20   /* one mic chunk (MUSE_AUDIO_CHUNK at 16 kHz) */

/* ---- Noise floor ---- */

#define VW_FLOOR_INIT_DB (-60.0f)   /* before the first chunk */
#define VW_FLOOR_MIN_DB (-85.0f)    /* a dead-silent or glitched chunk can't pull it lower */
#define VW_FLOOR_MAX_DB (-25.0f)    /* nor a long loud stretch push it higher */
#define VW_FLOOR_RISE_DB 0.05f      /* per chunk while louder: 2.5 dB/s, so speech barely moves it */
#define VW_FLOOR_FALL 0.25f         /* fraction of the gap closed per chunk while quieter */

typedef struct {
    float db;
} vw_floor_t;

void vw_floor_init(vw_floor_t *f);
/* One idle chunk's level (dBFS). Not called for chunks of Muse's own playback tail. */
void vw_floor_update(vw_floor_t *f, float chunk_db);

/* ---- The listen after a wake ---- */

typedef struct {
    float onset_margin_db;     /* speech: at least this far above the floor ... */
    float min_speech_db;       /* ... and at least this loud */
    float silence_margin_db;   /* after onset, quieter than floor + this counts as silence */
    int guard_chunks;          /* right after the detection, no onset: the wake word's own tail */
    int onset_chunks;          /* this many speech chunks in a row make an onset (a click doesn't) */
    int no_speech_chunks;      /* no onset by then: back to idle, nothing sent */
    int end_silence_chunks;    /* after onset, this much silence in a row ends the turn */
    int lead_chunks;           /* the turn starts this long before the onset (never before the wake) */
    int max_chunks;            /* the turn's length cap, from its start (the note buffer) */
} vw_config_t;

/* 10 dB / -60 dBFS / 6 dB, 200 ms guard, 80 ms onset, 5 s no-speech,
 * 1.0 s end silence, 300 ms lead, 15 s cap. */
void vw_config_default(vw_config_t *c);
/* The config is usable: positive counts, a lead and a no-speech wait that fit the cap. */
bool vw_config_ok(const vw_config_t *c);

typedef enum {
    VW_WAITING = 0,   /* no speech yet: keep recording locally, contact nothing */
    VW_ONSET,         /* speech started on this chunk: from now the backend may be contacted */
    VW_SPEAKING,      /* after onset: keep streaming */
    VW_END_SILENCE,   /* the speaker stopped: end the turn (like a button release) */
    VW_END_MAX,       /* the length cap: end the turn */
    VW_NO_SPEECH,     /* nobody spoke after the wake: back to idle quietly, no backend call */
} vw_verdict_t;

typedef struct {
    vw_config_t cfg;
    float speech_db;    /* floor + onset margin, at least min_speech_db */
    float silence_db;   /* floor + silence margin */
    int chunks;         /* chunks since the detection, this one included */
    int run;            /* speech chunks in a row (before onset) */
    int quiet;          /* silent chunks in a row (after onset) */
    int onset_at;       /* chunk index (0-based) where the onset's run began, or -1 */
    int lead;           /* chunk index the turn's audio starts at, or -1 before onset */
    bool ended;
    vw_verdict_t last;
} vw_listen_t;

/* Starts the listen at the moment of detection, against the idle floor. */
void vw_listen_start(vw_listen_t *l, const vw_config_t *cfg, float floor_db);
/* One 20 ms chunk recorded since the detection (its level in dBFS). After an
 * end verdict (END_SILENCE, END_MAX, NO_SPEECH) it keeps returning that verdict. */
vw_verdict_t vw_listen_chunk(vw_listen_t *l, float chunk_db);
/* True from the onset on: the only time a wake turn may reach the backend. */
bool vw_listen_may_contact_backend(const vw_listen_t *l);
/* After onset: the first chunk (counted from the detection, >= 0) the turn's
 * audio includes; -1 before onset. Chunks before it are dropped unsent. */
int vw_listen_lead(const vw_listen_t *l);

/* ---- Settings ---- */

#define VW_THRESHOLD_MIN 500    /* permille: 0.50; the model's test curve starts there (0.74 false wakes/h) */
#define VW_THRESHOLD_MAX 990    /* 0.99; above this it hardly fires at all (FRR 26 % at 0.99) */

typedef enum {
    VW_PARSE_OK = 0,
    VW_PARSE_CLAMPED,   /* a valid number outside [MIN, MAX]: *out holds the nearest bound */
    VW_PARSE_INVALID,   /* not a decimal in [0, 1]: *out unchanged */
} vw_parse_t;

/* "0.65", ".7", "0.655", "1" -> permille (rounded to nearest). At most 4
 * decimals, no sign, no spaces, no exponent. */
vw_parse_t vw_parse_threshold(const char *s, int *permille);
/* "on" / "off" -> *on. */
bool vw_parse_onoff(const char *s, bool *on);
/* Brings a stored or configured value into range (NVS may hold anything). */
int vw_clamp_threshold(int permille);

#ifdef __cplusplus
}
#endif
