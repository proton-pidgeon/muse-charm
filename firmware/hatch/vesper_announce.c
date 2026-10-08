/*
 * Announcements from the Vesper node backend (task 18). See vesper_announce.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_announce.h"

#include <string.h>

/* A text for the caption: control characters (newlines, tabs, ...) become spaces. */
static void clean_text(char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7f) {
            *s = ' ';
        }
    }
}

static bool blank(const char *s)
{
    for (; *s; s++) {
        if (*s != ' ') {
            return false;
        }
    }
    return true;
}

/* One element of the array into *it; false if it isn't a usable announcement. */
static bool parse_item(const char *elem, size_t len, const vp_url_t *base, vn_item_t *it, unsigned *audio_refused)
{
    it->text[0] = it->audio[0] = '\0';
    if (!len || elem[0] != '{') {
        return false;
    }
    if (vp_json_string(elem, len, "text", it->text, sizeof(it->text)) != VP_JSON_STRING ||
        strlen(it->text) > VN_TEXT_BYTES_MAX) {
        it->text[0] = '\0';
        return false;
    }
    clean_text(it->text);
    if (blank(it->text)) {
        it->text[0] = '\0';
        return false;
    }
    char ref[VP_URL_MAX];
    vp_json_kind_t k = vp_json_string(elem, len, "audio_url", ref, sizeof(ref));
    if (k == VP_JSON_BAD) {
        it->text[0] = '\0';
        return false;   /* the element isn't a well-formed object after all */
    }
    if (k == VP_JSON_STRING) {
        if (!base || !vp_resolve_audio_url(base, ref, it->audio, sizeof(it->audio))) {
            it->audio[0] = '\0';
            (*audio_refused)++;
        }
    } else if (k == VP_JSON_OTHER) {
        (*audio_refused)++;   /* a number, an object...: no speech, the caption still goes */
    }
    return true;
}

bool vn_parse(const char *body, size_t len, const vp_url_t *base, vn_list_t *out)
{
    out->n = 0;
    out->dropped = out->audio_refused = 0;
    if (!body || len >= VN_BODY_MAX || !vp_json_object_ok(body, len)) {
        return false;
    }
    vp_json_iter_t it;
    if (!vp_json_array(body, len, "announcements", &it)) {
        return false;
    }
    const char *elem;
    size_t elen;
    int seen = 0;
    while (vp_json_next(&it, &elem, &elen)) {
        if (seen++ >= VN_MAX) {
            out->dropped++;
            continue;
        }
        if (parse_item(elem, elen, base, &out->items[out->n], &out->audio_refused)) {
            out->n++;
        } else {
            out->dropped++;
        }
    }
    if (it.bad) {
        out->n = 0;
        out->dropped = out->audio_refused = 0;
        return false;
    }
    return true;
}

int vn_keep_from(vn_list_t *l, int from)
{
    if (from <= 0) {
        return l->n;
    }
    if (from >= l->n) {
        l->n = 0;
        return 0;
    }
    int keep = l->n - from;
    memmove(&l->items[0], &l->items[from], (size_t)keep * sizeof(l->items[0]));
    memset(&l->items[keep], 0, (size_t)from * sizeof(l->items[0]));
    l->n = keep;
    return keep;
}

bool vn_url(const vp_url_t *base, char *out, size_t cap)
{
    return vp_url_join(base, "/announcements", out, cap);
}

vn_verdict_t vn_verdict(int status, bool parsed)
{
    if (status == 200) {
        return parsed ? VN_OK : VN_BAD_BODY;
    }
    if (status == 429) {
        return VN_RATE_LIMITED;
    }
    if (status == 401 || status == 403) {
        return VN_REFUSED;
    }
    return VN_FAILED;
}

const char *vn_verdict_name(vn_verdict_t v)
{
    switch (v) {
    case VN_OK: return "ok";
    case VN_BAD_BODY: return "malformed reply";
    case VN_RATE_LIMITED: return "rate limited";
    case VN_REFUSED: return "refused";
    case VN_FAILED: return "failed";
    }
    return "?";
}

void vn_sched_init(vn_sched_t *s, int64_t now_ms)
{
    s->next_ms = now_ms + VN_POLL_MS;
    s->failures = 0;
}

bool vn_sched_due(const vn_sched_t *s, int64_t now_ms)
{
    return now_ms >= s->next_ms;
}

int64_t vn_sched_wait_ms(const vn_sched_t *s, int64_t now_ms)
{
    return s->next_ms > now_ms ? s->next_ms - now_ms : 0;
}

/* Retry-After as delta-seconds (digits only), in ms, capped; 0 if absent or not that. */
static int64_t retry_after_ms(const char *h)
{
    if (!h) {
        return 0;
    }
    while (*h == ' ') {
        h++;
    }
    int64_t v = 0;
    const char *start = h;
    while (*h >= '0' && *h <= '9') {
        v = v * 10 + (*h - '0');
        if (v * 1000 > VN_RETRY_AFTER_MAX_MS) {
            return VN_RETRY_AFTER_MAX_MS;
        }
        h++;
    }
    while (*h == ' ') {
        h++;
    }
    return h == start || *h ? 0 : v * 1000;
}

void vn_sched_done(vn_sched_t *s, vn_verdict_t v, const char *retry_after, int64_t now_ms)
{
    if (v == VN_OK) {
        s->failures = 0;
        s->next_ms = now_ms + VN_POLL_MS;
        return;
    }
    if (s->failures < 16) {
        s->failures++;
    }
    int64_t delay = VN_POLL_MS;
    for (uint32_t i = 1; i < s->failures && delay < VN_BACKOFF_MAX_MS; i++) {
        delay *= 2;
    }
    if (delay > VN_BACKOFF_MAX_MS) {
        delay = VN_BACKOFF_MAX_MS;
    }
    if (v == VN_RATE_LIMITED) {
        int64_t ra = retry_after_ms(retry_after);
        if (ra > delay) {
            delay = ra;
        }
    }
    s->next_ms = now_ms + delay;
}
