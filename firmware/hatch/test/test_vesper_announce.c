/*
 * Host tests for firmware/hatch/vesper_announce.c (GET /announcements, task 18)
 * and the JSON array helpers in vesper_proto.c. Run: make -C firmware test
 */
#include "vesper_announce.h"

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

#define CHECK_STR(a, b)                                                                     \
    do {                                                                                    \
        s_checks++;                                                                         \
        const char *a_ = (a), *b_ = (b);                                                    \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                            \
            s_fail++;                                                                       \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", \
                    b_ ? b_ : "(null)");                                                    \
        }                                                                                   \
    } while (0)

#define AID "AbCdEfGhIjKlMnOpQrStUvWx"
#define BASE "https://peggy.fly.dev/vesper-node"

static vn_list_t s_list;   /* ~6 KB: off the stack */
static vp_url_t s_base;

static bool parse(const char *body)
{
    return vn_parse(body, strlen(body), &s_base, &s_list);
}

static void test_json_arrays(void)
{
    vp_json_iter_t it;
    const char *e;
    size_t n;
    const char *j = "{\"a\": 1, \"list\": [ {\"x\": \"]\"} , 2, \"s,\", [3, [4]], null ], \"z\": {}}";
    CHECK(vp_json_object_ok(j, strlen(j)));
    CHECK(vp_json_array(j, strlen(j), "list", &it));
    CHECK(vp_json_next(&it, &e, &n) && n == 10 && !memcmp(e, "{\"x\": \"]\"}", 10));
    CHECK(vp_json_next(&it, &e, &n) && n == 1 && e[0] == '2');
    CHECK(vp_json_next(&it, &e, &n) && n == 4 && !memcmp(e, "\"s,\"", 4));
    CHECK(vp_json_next(&it, &e, &n) && n == 8 && !memcmp(e, "[3, [4]]", 8));
    CHECK(vp_json_next(&it, &e, &n) && n == 4 && !memcmp(e, "null", 4));
    CHECK(!vp_json_next(&it, &e, &n) && it.done && !it.bad);
    CHECK(!vp_json_next(&it, &e, &n));   /* stays at the end */

    const char *empty = "{\"list\":[]}";
    CHECK(vp_json_array(empty, strlen(empty), "list", &it));
    CHECK(!vp_json_next(&it, &e, &n) && it.done && !it.bad);

    /* not arrays / not there */
    CHECK(!vp_json_array("{\"list\":{}}", 11, "list", &it));
    CHECK(!vp_json_array("{\"list\":\"[]\"}", 13, "list", &it));
    CHECK(!vp_json_array("{\"other\":[]}", 12, "list", &it));
    CHECK(!vp_json_array("[1,2]", 5, "list", &it));
    CHECK(!vp_json_array("{\"x\":{\"list\":[]}}", 17, "list", &it));   /* nested never matches */

    /* malformed arrays */
    const char *bad[] = { "{\"l\":[1,]}", "{\"l\":[,1]}", "{\"l\":[1 2]}", "{\"l\":[1", "{\"l\":[", "{\"l\":[\"x]}", NULL };
    for (int i = 0; bad[i]; i++) {
        CHECK(vp_json_array(bad[i], strlen(bad[i]), "l", &it));
        int guard = 0;
        while (vp_json_next(&it, &e, &n) && guard++ < 10) {
        }
        CHECK(it.bad);
    }

    /* whole-object check */
    CHECK(vp_json_object_ok(" {\"a\":[1]} \n", 12));
    CHECK(!vp_json_object_ok("{\"a\":[1]} x", 11));
    CHECK(!vp_json_object_ok("{\"a\":[1]", 8));
    CHECK(!vp_json_object_ok("[1]", 3));
    CHECK(!vp_json_object_ok("", 0));
    CHECK(!vp_json_object_ok("{\"a\":[1]}{}", 11));
}

static void test_valid(void)
{
    /* The shape backend/src/vesper_node/app.py sends (json.dumps). */
    CHECK(parse("{\"announcements\": [{\"text\": \"Your ten minute timer is done.\", \"audio_url\": \"audio/" AID
                ".mp3\"}, {\"text\": \"Reminder: call the plumber.\", \"audio_url\": null}]}"));
    CHECK(s_list.n == 2 && s_list.dropped == 0 && s_list.audio_refused == 0);
    CHECK_STR(s_list.items[0].text, "Your ten minute timer is done.");
    CHECK_STR(s_list.items[0].audio, BASE "/audio/" AID ".mp3");
    CHECK_STR(s_list.items[1].text, "Reminder: call the plumber.");
    CHECK_STR(s_list.items[1].audio, "");

    /* origin-relative reference, missing audio_url, unknown fields, escapes */
    CHECK(parse("{\"x\":1,\"announcements\":[{\"audio_url\":\"/audio/" AID ".mp3\",\"text\":\"caf\\u00e9\\nnow\","
                "\"kind\":\"timer\"},{\"text\":\"b\"}],\"y\":[1,{}]}"));
    CHECK(s_list.n == 2);
    CHECK_STR(s_list.items[0].text, "caf\xc3\xa9 now");   /* control characters become spaces */
    CHECK_STR(s_list.items[0].audio, "https://peggy.fly.dev/audio/" AID ".mp3");
    CHECK_STR(s_list.items[1].audio, "");

    /* exactly at the text limit */
    char body[2048];
    char text[VN_TEXT_BYTES_MAX + 1];
    memset(text, 'a', VN_TEXT_BYTES_MAX);
    text[VN_TEXT_BYTES_MAX] = '\0';
    snprintf(body, sizeof(body), "{\"announcements\":[{\"text\":\"%s\",\"audio_url\":null}]}", text);
    CHECK(parse(body) && s_list.n == 1 && strlen(s_list.items[0].text) == VN_TEXT_BYTES_MAX);
}

static void test_empty(void)
{
    CHECK(parse("{\"announcements\": []}") && s_list.n == 0 && s_list.dropped == 0);
    CHECK(parse(" \n{ \"announcements\" : [ ] }\n") && s_list.n == 0);
}

static void test_malformed(void)
{
    const char *bad[] = {
        "",
        "null",
        "[]",
        "{}",
        "{\"announcements\": null}",
        "{\"announcements\": {}}",
        "{\"announcements\": \"[]\"}",
        "{\"announcements\": [}",
        "{\"announcements\": [{\"text\": \"a\"}",                   /* truncated */
        "{\"announcements\": [{\"text\": \"a\"}]",                  /* truncated */
        "{\"announcements\": [{\"text\": \"a\"},]}",
        "{\"announcements\": [{\"text\": \"a\"}]} trailing",
        "{\"announcements\": [{\"text\": \"a\"}]}{}",
        "{\"announcements\": [{\"text\": \"a\\q\"}]",
        "{\"error\": \"rate_limited\"}",
        "{\"nested\": {\"announcements\": [{\"text\": \"a\"}]}}",
        NULL,
    };
    for (int i = 0; bad[i]; i++) {
        s_list.n = 99;
        CHECK(!parse(bad[i]));
        CHECK(s_list.n == 0);
    }
    CHECK(!vn_parse(NULL, 0, &s_base, &s_list) && s_list.n == 0);
}

static void test_bad_items_dropped(void)
{
    CHECK(parse("{\"announcements\": [\"a string\", 5, null, [\"x\"], {\"text\": 5}, {\"text\": \"\"},"
                "{\"text\": \"  \\n \"}, {\"kind\": \"timer\"}, {\"text\": null}]}"));
    /* only the first VN_MAX elements are looked at: 5 dropped, 4 ignored (also dropped) */
    CHECK(s_list.n == 0 && s_list.dropped == 9);

    CHECK(parse("{\"announcements\": [{\"text\": 5}, {\"text\": \"ok\"}]}"));
    CHECK(s_list.n == 1 && s_list.dropped == 1);
    CHECK_STR(s_list.items[0].text, "ok");
}

static void test_oversize(void)
{
    /* a text over VN_TEXT_BYTES_MAX is dropped, the rest kept */
    static char body[VN_BODY_MAX + 64];
    char text[VN_TEXT_BYTES_MAX + 2];
    memset(text, 'b', VN_TEXT_BYTES_MAX + 1);
    text[VN_TEXT_BYTES_MAX + 1] = '\0';
    snprintf(body, sizeof(body), "{\"announcements\":[{\"text\":\"%s\"},{\"text\":\"short\"}]}", text);
    CHECK(parse(body) && s_list.n == 1 && s_list.dropped == 1);
    CHECK_STR(s_list.items[0].text, "short");

    /* a text far over the decode buffer (cut there) is dropped too */
    static char huge[VN_BODY_MAX];
    int o = snprintf(huge, sizeof(huge), "{\"announcements\":[{\"text\":\"");
    memset(huge + o, 'c', 3000);
    o += 3000;
    snprintf(huge + o, sizeof(huge) - (size_t)o, "\"}]}");
    CHECK(parse(huge) && s_list.n == 0 && s_list.dropped == 1);

    /* more than VN_MAX items: the first VN_MAX only */
    CHECK(parse("{\"announcements\":[{\"text\":\"1\"},{\"text\":\"2\"},{\"text\":\"3\"},{\"text\":\"4\"},"
                "{\"text\":\"5\"},{\"text\":\"6\"},{\"text\":\"7\"}]}"));
    CHECK(s_list.n == VN_MAX && s_list.dropped == 2);
    CHECK_STR(s_list.items[4].text, "5");

    /* a body of VN_BODY_MAX bytes or more is refused whole, even if well formed */
    static char big[VN_BODY_MAX + 1];
    o = snprintf(big, sizeof(big), "{\"announcements\":[{\"text\":\"ok\"}],\"pad\":\"");
    memset(big + o, 'p', sizeof(big) - (size_t)o);
    memcpy(big + VN_BODY_MAX - 2, "\"}", 2);
    CHECK(vp_json_object_ok(big, VN_BODY_MAX));
    s_list.n = 99;
    CHECK(!vn_parse(big, VN_BODY_MAX, &s_base, &s_list) && s_list.n == 0);
    CHECK(vn_parse(big, VN_BODY_MAX - 1, &s_base, &s_list) == false);   /* now truncated: malformed */
}

static void test_bad_audio_url(void)
{
    /* the message_done.audio_url rules: anything that could leave the configured server is refused */
    const char *refs[] = {
        "https://evil.example/audio/" AID ".mp3",
        "//evil.example/audio/" AID ".mp3",
        "http:audio/x.mp3",
        "audio/../../x.mp3",
        "./audio/" AID ".mp3",
        "audio\\\\" AID ".mp3",
        "audio/%2e%2e/x.mp3",
        "audio/" AID ".mp3?x=1",
        "audio/" AID ".mp3#f",
        "audio/ " AID ".mp3",
        "audio/\\u0000x.mp3",
        "audio/\\r\\nX-Evil: 1",
        "",
        NULL,
    };
    char body[512];
    for (int i = 0; refs[i]; i++) {
        snprintf(body, sizeof(body), "{\"announcements\":[{\"text\":\"t\",\"audio_url\":\"%s\"}]}", refs[i]);
        CHECK(parse(body));
        CHECK(s_list.n == 1 && s_list.audio_refused == 1);
        CHECK_STR(s_list.items[0].audio, "");
        CHECK_STR(s_list.items[0].text, "t");   /* the caption still goes */
    }
    /* audio_url of the wrong type: caption only */
    CHECK(parse("{\"announcements\":[{\"text\":\"t\",\"audio_url\":5}]}") && s_list.n == 1 && s_list.audio_refused == 1);
    CHECK(parse("{\"announcements\":[{\"text\":\"t\",\"audio_url\":{\"u\":\"audio/x.mp3\"}}]}") && s_list.n == 1 &&
          !s_list.items[0].audio[0]);
    /* no base: nothing resolves */
    snprintf(body, sizeof(body), "{\"announcements\":[{\"text\":\"t\",\"audio_url\":\"audio/" AID ".mp3\"}]}");
    CHECK(vn_parse(body, strlen(body), NULL, &s_list) && s_list.n == 1 && !s_list.items[0].audio[0]);
    /* a URL too long for VP_URL_MAX */
    char longref[400];
    memset(longref, 'a', sizeof(longref) - 1);
    longref[sizeof(longref) - 1] = '\0';
    memcpy(longref, "audio/", 6);
    char body2[700];
    snprintf(body2, sizeof(body2), "{\"announcements\":[{\"text\":\"t\",\"audio_url\":\"%s\"}]}", longref);
    CHECK(parse(body2) && s_list.n == 1 && !s_list.items[0].audio[0]);
}

static void test_url(void)
{
    char url[VP_URL_MAX];
    CHECK(vn_url(&s_base, url, sizeof(url)));
    CHECK_STR(url, BASE "/announcements");
    vp_url_t lan;
    CHECK(vp_url_parse("http://[::1]:8796", &lan) && vn_url(&lan, url, sizeof(url)));
    CHECK_STR(url, "http://[::1]:8796/announcements");
    CHECK(!vn_url(&s_base, url, 20));
}

static void test_verdicts(void)
{
    CHECK(vn_verdict(200, true) == VN_OK);
    CHECK(vn_verdict(200, false) == VN_BAD_BODY);
    CHECK(vn_verdict(429, false) == VN_RATE_LIMITED);
    CHECK(vn_verdict(401, false) == VN_REFUSED);
    CHECK(vn_verdict(403, false) == VN_REFUSED);
    CHECK(vn_verdict(0, false) == VN_FAILED);
    CHECK(vn_verdict(500, false) == VN_FAILED);
    CHECK(vn_verdict(204, false) == VN_FAILED);
    CHECK(vn_verdict(404, false) == VN_FAILED);   /* a backend from before task 18 */
    for (int v = VN_OK; v <= VN_FAILED; v++) {
        CHECK(vn_verdict_name((vn_verdict_t)v)[0] != '?');
    }
}

static void test_schedule(void)
{
    vn_sched_t s;
    vn_sched_init(&s, 1000);
    CHECK(!vn_sched_due(&s, 1000) && vn_sched_wait_ms(&s, 1000) == VN_POLL_MS);
    CHECK(vn_sched_due(&s, 1000 + VN_POLL_MS) && vn_sched_wait_ms(&s, 1000 + VN_POLL_MS) == 0);
    int64_t t = 100000;
    vn_sched_done(&s, VN_OK, NULL, t);
    CHECK(s.next_ms == t + 15000 && s.failures == 0);
    /* errors back off 15 -> 30 -> 60 s, capped */
    vn_sched_done(&s, VN_FAILED, NULL, t);
    CHECK(s.next_ms == t + 15000);
    vn_sched_done(&s, VN_BAD_BODY, NULL, t);
    CHECK(s.next_ms == t + 30000);
    vn_sched_done(&s, VN_REFUSED, NULL, t);
    CHECK(s.next_ms == t + 60000);
    for (int i = 0; i < 40; i++) {
        vn_sched_done(&s, VN_FAILED, NULL, t);
    }
    CHECK(s.next_ms == t + VN_BACKOFF_MAX_MS);
    vn_sched_done(&s, VN_OK, NULL, t);
    CHECK(s.next_ms == t + VN_POLL_MS && s.failures == 0);
    /* 429: at least Retry-After, at least the backoff, Retry-After capped */
    vn_sched_done(&s, VN_RATE_LIMITED, "3", t);
    CHECK(s.next_ms == t + 15000);
    vn_sched_done(&s, VN_RATE_LIMITED, "120", t);
    CHECK(s.next_ms == t + 120000);
    vn_sched_done(&s, VN_RATE_LIMITED, "999999999999999999999", t);
    CHECK(s.next_ms == t + VN_RETRY_AFTER_MAX_MS);
    vn_sched_done(&s, VN_OK, NULL, t);
    vn_sched_done(&s, VN_RATE_LIMITED, "Wed, 21 Oct 2015 07:28:00 GMT", t);   /* not delta-seconds: ignored */
    CHECK(s.next_ms == t + 15000);
    vn_sched_done(&s, VN_RATE_LIMITED, "-5", t);
    CHECK(s.next_ms == t + 30000);
    vn_sched_done(&s, VN_RATE_LIMITED, " 90 ", t);
    CHECK(s.next_ms == t + 90000);
}

/* Bounded, crash-free on arbitrary input (ASan/UBSan watch). */
static void test_fuzz(void)
{
    const char *seed = "{\"announcements\":[{\"text\":\"Your timer is done.\",\"audio_url\":\"audio/" AID
                       ".mp3\"},{\"text\":\"b\\u00e9\",\"audio_url\":null}]}";
    size_t n = strlen(seed);
    static char buf[512];
    uint32_t x = 12345;
    for (int round = 0; round < 20000; round++) {
        memcpy(buf, seed, n);
        int flips = 1 + round % 4;
        for (int k = 0; k < flips; k++) {
            x = x * 1103515245u + 12345u;
            size_t at = (x >> 8) % n;
            x = x * 1103515245u + 12345u;
            buf[at] = (char)(x >> 16);
        }
        x = x * 1103515245u + 12345u;
        size_t len = round % 3 == 0 ? (x >> 8) % (n + 1) : n;
        if (vn_parse(buf, len, &s_base, &s_list)) {
            CHECK(s_list.n >= 0 && s_list.n <= VN_MAX);
            for (int i = 0; i < s_list.n; i++) {
                CHECK(s_list.items[i].text[0] && strlen(s_list.items[i].text) <= VN_TEXT_BYTES_MAX);
                CHECK(!s_list.items[i].audio[0] || !strncmp(s_list.items[i].audio, BASE "/audio/", strlen(BASE "/audio/")) ||
                      !strncmp(s_list.items[i].audio, "https://peggy.fly.dev/", 22));
            }
        } else {
            CHECK(s_list.n == 0);
        }
    }
}

int main(void)
{
    if (!vp_url_parse(BASE, &s_base)) {
        fprintf(stderr, "base URL didn't parse\n");
        return 1;
    }
    test_json_arrays();
    test_valid();
    test_empty();
    test_malformed();
    test_bad_items_dropped();
    test_oversize();
    test_bad_audio_url();
    test_url();
    test_verdicts();
    test_schedule();
    test_fuzz();
    if (s_fail) {
        fprintf(stderr, "test_vesper_announce: %d of %d checks FAILED\n", s_fail, s_checks);
        return 1;
    }
    printf("test_vesper_announce: %d checks passed\n", s_checks);
    return 0;
}
