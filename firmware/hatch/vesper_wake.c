/*
 * Wake word (tasks 20, 22): see vesper_wake.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_wake.h"

#include <string.h>

/* ---- Noise floor ---- */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void vw_floor_init(vw_floor_t *f)
{
    f->db = VW_FLOOR_INIT_DB;
}

void vw_floor_update(vw_floor_t *f, float chunk_db)
{
    /* NaN compares false both ways: ignore it rather than poison the floor. */
    if (!(chunk_db == chunk_db)) {
        return;
    }
    if (chunk_db < f->db) {
        f->db += (chunk_db - f->db) * VW_FLOOR_FALL;   /* quieter: follow it down quickly */
    } else {
        f->db += VW_FLOOR_RISE_DB;                     /* louder: creep up, so talk isn't "the floor" */
        if (f->db > chunk_db) {
            f->db = chunk_db;
        }
    }
    f->db = clampf(f->db, VW_FLOOR_MIN_DB, VW_FLOOR_MAX_DB);
}

/* ---- The listen after a wake ---- */

void vw_config_default(vw_config_t *c)
{
    *c = (vw_config_t){
        .onset_margin_db = 10.0f,
        .min_speech_db = -60.0f,
        .silence_margin_db = 6.0f,
        .guard_chunks = 200 / VW_CHUNK_MS,
        .onset_chunks = 80 / VW_CHUNK_MS,
        .no_speech_chunks = 5000 / VW_CHUNK_MS,
        .end_silence_chunks = 1000 / VW_CHUNK_MS,
        .lead_chunks = 300 / VW_CHUNK_MS,
        .max_chunks = 15000 / VW_CHUNK_MS,
    };
}

bool vw_config_ok(const vw_config_t *c)
{
    return c->guard_chunks >= 0 && c->onset_chunks > 0 && c->no_speech_chunks > c->guard_chunks &&
           c->end_silence_chunks > 0 && c->lead_chunks >= 0 && c->max_chunks > 0 &&
           c->no_speech_chunks <= c->max_chunks && c->lead_chunks + c->onset_chunks < c->max_chunks &&
           c->silence_margin_db <= c->onset_margin_db;
}

void vw_listen_start(vw_listen_t *l, const vw_config_t *cfg, float floor_db)
{
    memset(l, 0, sizeof(*l));
    l->cfg = *cfg;
    if (!(floor_db == floor_db)) {
        floor_db = VW_FLOOR_INIT_DB;
    }
    floor_db = clampf(floor_db, VW_FLOOR_MIN_DB, VW_FLOOR_MAX_DB);
    l->speech_db = floor_db + cfg->onset_margin_db;
    if (l->speech_db < cfg->min_speech_db) {
        l->speech_db = cfg->min_speech_db;
    }
    l->silence_db = floor_db + cfg->silence_margin_db;
    l->onset_at = -1;
    l->lead = -1;
    l->last = VW_WAITING;
}

static vw_verdict_t end(vw_listen_t *l, vw_verdict_t v)
{
    l->ended = true;
    return l->last = v;
}

vw_verdict_t vw_listen_chunk(vw_listen_t *l, float chunk_db)
{
    if (l->ended) {
        return l->last;
    }
    if (!(chunk_db == chunk_db)) {
        chunk_db = -100.0f;   /* NaN: count it as silence */
    }
    const vw_config_t *c = &l->cfg;
    int idx = l->chunks++;
    if (l->lead < 0) {
        if (idx < c->guard_chunks) {
            l->run = 0;   /* the wake word's own tail: never an onset */
        } else if (chunk_db >= l->speech_db) {
            if (++l->run >= c->onset_chunks) {
                l->onset_at = idx - l->run + 1;
                l->lead = l->onset_at - c->lead_chunks;
                if (l->lead < 0) {
                    l->lead = 0;   /* never before the detection */
                }
                l->quiet = 0;
                return l->last = VW_ONSET;
            }
        } else {
            l->run = 0;
        }
        if (l->chunks >= c->no_speech_chunks) {
            return end(l, VW_NO_SPEECH);
        }
        return l->last = VW_WAITING;
    }
    l->quiet = chunk_db < l->silence_db ? l->quiet + 1 : 0;
    if (l->quiet >= c->end_silence_chunks) {
        return end(l, VW_END_SILENCE);
    }
    if (l->chunks - l->lead >= c->max_chunks) {
        return end(l, VW_END_MAX);
    }
    return l->last = VW_SPEAKING;
}

bool vw_listen_may_contact_backend(const vw_listen_t *l)
{
    return l->lead >= 0;
}

int vw_listen_lead(const vw_listen_t *l)
{
    return l->lead;
}

/* ---- Settings ---- */

int vw_clamp_threshold(int permille)
{
    return permille < VW_THRESHOLD_MIN ? VW_THRESHOLD_MIN : permille > VW_THRESHOLD_MAX ? VW_THRESHOLD_MAX : permille;
}

vw_parse_t vw_parse_threshold(const char *s, int *permille)
{
    if (!s || !*s) {
        return VW_PARSE_INVALID;
    }
    /* [d] [. d{1,4}] with at least one digit somewhere; value in units of 1e-4. */
    long whole = 0, frac = 0;
    int whole_digits = 0, frac_digits = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        if (++whole_digits > 1) {
            return VW_PARSE_INVALID;   /* 10, 65, 650: not a fraction (no percent or permille forms) */
        }
        whole = *p++ - '0';
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (++frac_digits > 4) {
                return VW_PARSE_INVALID;
            }
            frac = frac * 10 + (*p++ - '0');
        }
    }
    if (*p || (!whole_digits && !frac_digits)) {
        return VW_PARSE_INVALID;
    }
    for (int i = frac_digits; i < 4; i++) {
        frac *= 10;
    }
    long v = whole * 10000 + frac;   /* 1e-4 units */
    if (v > 10000) {
        return VW_PARSE_INVALID;     /* above 1: not a probability */
    }
    int pm = (int)((v + 5) / 10);    /* to permille, rounded */
    int clamped = vw_clamp_threshold(pm);
    *permille = clamped;
    return clamped == pm ? VW_PARSE_OK : VW_PARSE_CLAMPED;
}

bool vw_parse_onoff(const char *s, bool *on)
{
    if (s && !strcmp(s, "on")) {
        *on = true;
        return true;
    }
    if (s && !strcmp(s, "off")) {
        *on = false;
        return true;
    }
    return false;
}
