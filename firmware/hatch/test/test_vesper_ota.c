/*
 * Host tests for firmware/hatch/vesper_ota.c (firmware updates from the node
 * backend, task 13) and vp_json_uint. Run: make -C firmware test
 */
#include "vesper_ota.h"

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

#define SHA "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
/* The shape backend/src/vesper_node/firmware.py sends (json.dumps of Manifest.wire()). */
#define MANIFEST(ver, size) \
    "{\"version\": \"" ver "\", \"sha256\": \"" SHA "\", \"size\": " size ", \"published_at\": 1791500000}"
#define CRED "vnc_bm90X2FfcmVhbF9jcmVkZW50aWFsX2p1c3RfYV9abc1"
#define TOKEN "node-token-nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"
#define SLOT (4u * 1024 * 1024)

static vo_verdict_t check(int status, const char *body, const char *running, const char *rejected, vo_manifest_t *m)
{
    return vo_check_result(status, body, body ? strlen(body) : 0, running, rejected, SLOT, m);
}

static void test_versions(void)
{
    vo_version_t v, w;
    CHECK(vo_version_parse("1.0.1", &v) && v.major == 1 && v.minor == 0 && v.patch == 1);
    CHECK(vo_version_parse("0.0.0", &v));
    CHECK(vo_version_parse("65535.65535.65535", &v) && v.major == 65535 && v.patch == 65535);
    const char *bad[] = { "",       "1",          "1.0",     "1.0.0.0", "v1.0.0",  "01.0.0",  "1.00.0", "1.0.0-dirty",
                          "1.0.0 ", " 1.0.0",     "65536.0.0", "1..0",  "1.0.",    "-1.0.0",  "+1.0.0", "1.0.0\n",
                          "999999.0.0", "1.0.0\r\nX-Evil: 1", "a.b.c", "1.0.0.", "0x1.0.0", NULL };
    for (int i = 0; bad[i]; i++) {
        CHECK(!vo_version_parse(bad[i], &v));
    }
    CHECK(!vo_version_parse(NULL, &v));
    CHECK(!vo_version_parse("1.0.0000000000000000000000000001", &v));   /* longer than 31 */
    vo_version_parse("1.2.3", &v);
    vo_version_parse("1.2.4", &w);
    CHECK(vo_version_cmp(&v, &w) < 0 && vo_version_cmp(&w, &v) > 0 && vo_version_cmp(&v, &v) == 0);
    vo_version_parse("1.10.0", &v);
    vo_version_parse("1.9.99", &w);
    CHECK(vo_version_cmp(&v, &w) > 0);   /* numeric, not lexical */
    vo_version_parse("2.0.0", &v);
    vo_version_parse("1.65535.65535", &w);
    CHECK(vo_version_cmp(&v, &w) > 0);
}

static void test_json_uint(void)
{
    uint32_t n;
    CHECK(vp_json_uint("{\"size\": 2035712}", 17, "size", &n) && n == 2035712);
    CHECK(vp_json_uint("{\"a\":\"x\",\"size\":0}", 18, "size", &n) && n == 0);
    CHECK(vp_json_uint("{\"size\":4294967295}", 19, "size", &n) && n == 4294967295u);
    const char *bad[] = { "{\"size\":4294967296}", "{\"size\":-1}",   "{\"size\":1.5}",   "{\"size\":1e6}",
                          "{\"size\":\"12\"}",     "{\"size\":012}",  "{\"size\":null}",  "{\"size\":true}",
                          "{\"size\":}",           "{\"x\":1}",       "[1]",              "{\"size\":12abc}",
                          "{\"n\":{\"size\":1}}",  "{\"size\":99999999999999999999}", NULL };
    for (int i = 0; bad[i]; i++) {
        n = 7;
        CHECK(!vp_json_uint(bad[i], strlen(bad[i]), "size", &n) && n == 0);
    }
}

static void test_manifest(void)
{
    vo_manifest_t m;
    const char *ok = MANIFEST("1.0.1", "2035712");
    CHECK(vo_manifest_parse(ok, strlen(ok), &m));
    CHECK_STR(m.version, "1.0.1");
    CHECK_STR(m.sha256_hex, SHA);
    CHECK(m.size == 2035712 && m.v.major == 1 && m.v.patch == 1);
    CHECK(m.sha256[0] == 0x9f && m.sha256[31] == 0x08);

    const char *bad[] = {
        MANIFEST("1.0", "2035712"),
        MANIFEST("1.0.1-dirty", "2035712"),
        MANIFEST("1.0.1", "0"),
        MANIFEST("1.0.1", "-5"),
        MANIFEST("1.0.1", "\"2035712\""),
        MANIFEST("1.0.1", "4294967296"),
        "{\"version\": \"1.0.1\", \"sha256\": \"" SHA "0\", \"size\": 10}",
        "{\"version\": \"1.0.1\", \"sha256\": \"9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08\", \"size\": 10}",
        "{\"version\": \"1.0.1\", \"sha256\": \"zz86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08\", \"size\": 10}",
        "{\"version\": \"1.0.1\", \"size\": 10}",
        "{\"sha256\": \"" SHA "\", \"size\": 10}",
        "{\"version\": 101, \"sha256\": \"" SHA "\", \"size\": 10}",
        "not json",
        "",
        NULL,
    };
    for (int i = 0; bad[i]; i++) {
        memset(&m, 0x5a, sizeof(m));
        CHECK(!vo_manifest_parse(bad[i], strlen(bad[i]), &m));
        CHECK(m.size == 0 && m.version[0] == '\0' && m.sha256[0] == 0);
    }
    CHECK(!vo_manifest_parse(NULL, 0, &m));
    /* A body cut short by the reader (VO_BODY_MAX) doesn't parse. */
    CHECK(!vo_manifest_parse(ok, strlen(ok) - 30, &m));
}

static void test_urls(void)
{
    vp_url_t base;
    vo_manifest_t m;
    char url[VP_URL_MAX];
    const char *ok = MANIFEST("1.0.1", "100");
    CHECK(vo_manifest_parse(ok, strlen(ok), &m));
    CHECK(vp_url_parse("https://peggy.fly.dev/vesper-node", &base));
    CHECK(vo_manifest_url(&base, url, sizeof(url)));
    CHECK_STR(url, "https://peggy.fly.dev/vesper-node/firmware/manifest");
    CHECK(vo_image_url(&base, &m, url, sizeof(url)));
    CHECK_STR(url, "https://peggy.fly.dev/vesper-node/firmware/" SHA ".bin");
    CHECK(vp_url_parse("http://[::1]:8796", &base));
    CHECK(vo_image_url(&base, &m, url, sizeof(url)));
    CHECK_STR(url, "http://[::1]:8796/firmware/" SHA ".bin");
    CHECK(!vo_image_url(&base, &m, url, 40));   /* doesn't fit */
    /* The URL is built from the parsed hash only: a tampered hex copy is refused. */
    m.sha256_hex[0] = '/';
    CHECK(!vo_image_url(&base, &m, url, sizeof(url)));
}

static void test_headers(void)
{
    char auth[VP_AUTH_MAX];
    vp_header_t h[VO_HEADERS];
    CHECK(vo_headers(TOKEN, "homelink-c86320", CRED, "1.0.0", auth, h));
    CHECK_STR(h[0].name, "Authorization");
    CHECK_STR(h[0].value, "Bearer " TOKEN);
    CHECK_STR(h[1].name, "X-Node-Id");
    CHECK_STR(h[1].value, "homelink-c86320");
    CHECK_STR(h[2].name, "X-Node-Credential");
    CHECK_STR(h[2].value, CRED);
    CHECK_STR(h[3].name, "X-Node-Firmware");
    CHECK_STR(h[3].value, "1.0.0");
    /* Anything that could break a header, or is missing, gives nothing usable. */
    CHECK(!vo_headers("", "homelink-c86320", CRED, "1.0.0", auth, h) && auth[0] == '\0' && !h[0].name);
    CHECK(!vo_headers(TOKEN "\r\nX: y", "homelink-c86320", CRED, "1.0.0", auth, h));
    CHECK(!vo_headers(TOKEN, "homelink c86320", CRED, "1.0.0", auth, h));
    CHECK(!vo_headers(TOKEN, "homelink-c86320", "", "1.0.0", auth, h));
    CHECK(!vo_headers(TOKEN, "homelink-c86320", "vnc_short", "1.0.0", auth, h));
    CHECK(!vo_headers(TOKEN, "homelink-c86320", CRED "\r\n", "1.0.0", auth, h));
    /* A running version that isn't MAJOR.MINOR.PATCH (or could break the header) goes as "unknown". */
    CHECK(vo_headers(TOKEN, "homelink-c86320", CRED, "999.0.0-dirty", auth, h));
    CHECK_STR(h[3].value, "unknown");
    CHECK(vo_headers(TOKEN, "homelink-c86320", CRED, "1.0.0\r\nX: y", auth, h));
    CHECK_STR(h[3].value, "unknown");
    CHECK(vo_headers(TOKEN, "homelink-c86320", CRED, NULL, auth, h));
    CHECK_STR(h[3].value, "unknown");
}

static void test_verdicts(void)
{
    vo_manifest_t m;
    const char *newer = MANIFEST("1.0.1", "2035712");
    CHECK(check(204, NULL, "1.0.0", NULL, &m) == VO_UP_TO_DATE);
    CHECK(check(200, newer, "1.0.0", NULL, &m) == VO_INSTALL && m.size == 2035712);
    CHECK(check(200, newer, "1.0.0", "", &m) == VO_INSTALL);
    CHECK(check(200, newer, "1.0.0", "1.0.2", &m) == VO_INSTALL);
    CHECK(check(200, newer, "1.0.1", NULL, &m) == VO_UP_TO_DATE);    /* the version it runs */
    CHECK(check(200, newer, "1.0.2", NULL, &m) == VO_SKIP_OLDER);    /* never a downgrade */
    CHECK(check(200, newer, "2.0.0", NULL, &m) == VO_SKIP_OLDER);
    CHECK(check(200, newer, "1.0.0", "1.0.1", &m) == VO_SKIP_REJECTED);   /* rolled back here once */
    CHECK(check(200, newer, "999.0.0-dirty", NULL, &m) == VO_SKIP_UNVERSIONED);
    CHECK(check(200, newer, "", NULL, &m) == VO_SKIP_UNVERSIONED);
    const char *huge = MANIFEST("1.0.1", "4194305");
    CHECK(check(200, huge, "1.0.0", NULL, &m) == VO_SKIP_TOO_BIG);
    const char *exact = MANIFEST("1.0.1", "4194304");
    CHECK(check(200, exact, "1.0.0", NULL, &m) == VO_INSTALL);
    CHECK(check(200, "{\"error\":\"x\"}", "1.0.0", NULL, &m) == VO_BAD_MANIFEST);
    CHECK(check(200, "", "1.0.0", NULL, &m) == VO_BAD_MANIFEST);
    CHECK(check(401, "{\"error\":\"unauthorized\"}", "1.0.0", NULL, &m) == VO_REFUSED);
    CHECK(check(403, "{\"error\":\"node_unauthorized\"}", "1.0.0", NULL, &m) == VO_REFUSED);
    CHECK(check(0, NULL, "1.0.0", NULL, &m) == VO_FAILED);
    CHECK(check(503, "{\"error\":\"firmware_unavailable\"}", "1.0.0", NULL, &m) == VO_FAILED);
    CHECK(check(404, "{\"error\":\"not_found\"}", "1.0.0", NULL, &m) == VO_FAILED);   /* a backend before task 13 */
    CHECK(check(302, NULL, "1.0.0", NULL, &m) == VO_FAILED);
    CHECK(check(-1, NULL, "1.0.0", NULL, &m) == VO_FAILED);

    /* The channel a future update comes through works: what a new image must see to be kept. */
    CHECK(vo_channel_ok(VO_UP_TO_DATE) && vo_channel_ok(VO_INSTALL) && vo_channel_ok(VO_SKIP_OLDER));
    CHECK(vo_channel_ok(VO_SKIP_REJECTED) && vo_channel_ok(VO_SKIP_TOO_BIG) && vo_channel_ok(VO_SKIP_UNVERSIONED));
    CHECK(!vo_channel_ok(VO_REFUSED) && !vo_channel_ok(VO_FAILED) && !vo_channel_ok(VO_BAD_MANIFEST));
    for (int v = VO_UP_TO_DATE; v <= VO_FAILED; v++) {
        CHECK(strcmp(vo_verdict_name((vo_verdict_t)v), "?") != 0);
        CHECK(strcmp(vo_verdict_code((vo_verdict_t)v), "?") != 0 && !strchr(vo_verdict_code((vo_verdict_t)v), '"'));
    }
}

static void test_schedule(void)
{
    vo_sched_t s;
    const char *id = "homelink-c86320";
    vo_sched_init(&s, 1000);
    CHECK(!vo_sched_due(&s, 1000) && vo_sched_wait_ms(&s, 1000) == VO_FIRST_CHECK_MS);
    CHECK(vo_sched_due(&s, 1000 + VO_FIRST_CHECK_MS) && vo_sched_wait_ms(&s, 1000 + VO_FIRST_CHECK_MS) == 0);

    int64_t j = vo_jitter_ms(id);
    CHECK(j >= 0 && j < VO_JITTER_MS && j % 1000 == 0);
    CHECK(vo_jitter_ms(id) == j);   /* fixed per node */
    /* Spread: 50 node ids land on many different seconds. */
    int distinct = 0;
    int64_t seen[50];
    for (int i = 0; i < 50; i++) {
        char nid[32];
        snprintf(nid, sizeof(nid), "homelink-%06x", (unsigned)(0xc86320 + i * 7919));
        int64_t v = vo_jitter_ms(nid);
        CHECK(v >= 0 && v < VO_JITTER_MS);
        bool dup = false;
        for (int k = 0; k < distinct; k++) {
            dup |= seen[k] == v;
        }
        if (!dup) {
            seen[distinct++] = v;
        }
    }
    CHECK(distinct > 40);

    int64_t now = 50000;
    vo_sched_done(&s, true, id, now);
    CHECK(s.failures == 0 && s.next_ms == now + VO_PERIOD_MS + j);
    /* Failures back off from a minute, doubling, capped at the period. */
    int64_t expect = VO_RETRY_MIN_MS;
    for (int i = 0; i < 12; i++) {
        vo_sched_done(&s, false, id, now);
        CHECK(s.next_ms - now == expect);
        expect = expect * 2 > VO_PERIOD_MS ? VO_PERIOD_MS : expect * 2;
    }
    CHECK(s.next_ms - now == VO_PERIOD_MS || s.next_ms - now == expect);
    s.failures = UINT32_MAX;
    vo_sched_done(&s, false, id, now);
    CHECK(s.failures == UINT32_MAX && s.next_ms - now == VO_PERIOD_MS);
    vo_sched_done(&s, true, id, now);
    CHECK(s.failures == 0);
    vo_sched_now(&s, now + 5);
    CHECK(vo_sched_due(&s, now + 5));
}

/* Random bodies never yield an install, and never read out of bounds (ASan). */
static void test_fuzz(void)
{
    srand(13);
    char body[VO_BODY_MAX];
    const char *pieces[] = { "{", "}", "\"version\"", "\"sha256\"", "\"size\"", ":", ",", "\"1.0.1\"", "\"" SHA "\"",
                             "123", "-", "\"", "\\", "[", "]", "null", " ", "4294967296", "0", "\\u0000" };
    int installs = 0;
    for (int i = 0; i < 20000; i++) {
        size_t n = 0;
        int parts = rand() % 14;
        for (int k = 0; k < parts; k++) {
            const char *p = pieces[rand() % (int)(sizeof(pieces) / sizeof(pieces[0]))];
            size_t l = strlen(p);
            if (n + l >= sizeof(body)) {
                break;
            }
            memcpy(body + n, p, l);
            n += l;
        }
        vo_manifest_t m;
        vo_verdict_t v = vo_check_result(200, body, n, "1.0.0", NULL, SLOT, &m);
        if (v == VO_INSTALL) {
            /* Possible only for a body that really is a full manifest; check it is one. */
            installs++;
            CHECK(!strcmp(m.version, "1.0.1") && !strcmp(m.sha256_hex, SHA) && m.size > 0 && m.size <= SLOT);
        }
    }
    (void)installs;
}

int main(void)
{
    test_versions();
    test_json_uint();
    test_manifest();
    test_urls();
    test_headers();
    test_verdicts();
    test_schedule();
    test_fuzz();
    if (s_fail) {
        fprintf(stderr, "test_vesper_ota: %d of %d checks FAILED\n", s_fail, s_checks);
        return 1;
    }
    printf("test_vesper_ota: %d checks passed\n", s_checks);
    return 0;
}
