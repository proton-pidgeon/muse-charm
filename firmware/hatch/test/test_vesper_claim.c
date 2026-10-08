/*
 * Host tests for firmware/hatch/vesper_claim.c (the node side of the claim
 * flow). Run: make -C firmware test
 */
#include "vesper_claim.h"

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

/* Shapes exactly as backend/src/vesper_node/registry.py makes them (token_urlsafe(32): 43 chars). */
#define SECRET "vcs_Q2hhbmdlTWVQbGVhc2VfdGhpc19pc19hX3Rlc3QxMjM"
#define CRED "vnc_bm90X2FfcmVhbF9jcmVkZW50aWFsX2p1c3RfYV9abc1"
#define START_OK                                                                                     \
    "{\"status\": \"pending\", \"claim_code\": \"K7M2-QX9P\", \"claim_secret\": \"" SECRET "\", " \
    "\"expires_in\": 600, \"poll_interval\": 3}"
#define POLL_OK "{\"status\": \"claimed\", \"node_id\": \"homelink-c86320\", \"room\": \"kitchen\", \"credential\": \"" CRED "\"}"
#define PENDING "{\"status\": \"pending\", \"expires_in\": 597, \"poll_interval\": 3}"
#define NOT_FOUND "{\"error\": \"claim_not_found\"}"

static vc_event_t start(vc_claim_t *c, int status, const char *body, const char *ra, int64_t now)
{
    return vc_start_result(c, status, body, body ? strlen(body) : 0, ra, now);
}

static vc_event_t poll(vc_claim_t *c, int status, const char *body, const char *ra, int64_t now)
{
    return vc_poll_result(c, status, body, body ? strlen(body) : 0, ra, now);
}

static bool all_zero(const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i]) {
            return false;
        }
    }
    return true;
}

static void test_validation(void)
{
    CHECK(sizeof(SECRET) - 1 == 47 && sizeof(CRED) - 1 == 47);
    CHECK(vc_valid_code("K7M2-QX9P"));
    CHECK(vc_valid_code("2345-6789"));
    CHECK(vc_valid_code("WXYZ-ABCD"));
    CHECK(!vc_valid_code("k7m2-qx9p"));      /* the node shows what the backend sent: upper case */
    CHECK(!vc_valid_code("K7M2QX9P"));
    CHECK(!vc_valid_code("K7M2-QX9"));
    CHECK(!vc_valid_code("K7M2-QX9PP"));
    CHECK(!vc_valid_code("K7M2_QX9P"));
    CHECK(!vc_valid_code("O7M2-QX9P"));      /* 0 O 1 I L U are not in the alphabet */
    CHECK(!vc_valid_code("K0M2-QX9P"));
    CHECK(!vc_valid_code("K7M1-QX9P"));
    CHECK(!vc_valid_code("K7MI-QX9P"));
    CHECK(!vc_valid_code("K7ML-QX9P"));
    CHECK(!vc_valid_code("K7MU-QX9P"));
    CHECK(!vc_valid_code(""));
    CHECK(!vc_valid_code(NULL));

    CHECK(vc_valid_secret(SECRET));
    CHECK(vc_valid_secret("vcs_abcdefghijklmnop"));          /* 16 characters after the prefix */
    CHECK(!vc_valid_secret("vcs_abcdefghijklmno"));          /* 15 */
    CHECK(!vc_valid_secret("vnc_abcdefghijklmnop"));         /* a credential is not a secret */
    CHECK(!vc_valid_secret("VCS_abcdefghijklmnop"));
    CHECK(!vc_valid_secret("vcs_abcdefghijklmnop\r\nX-Evil: 1"));   /* header injection */
    CHECK(!vc_valid_secret("vcs_abcdefgh ijklmnop"));
    CHECK(!vc_valid_secret("vcs_abcdefghijklmnop+/="));      /* url-safe base64 only */
    CHECK(!vc_valid_secret(NULL));
    char longest[VC_SECRET_MAX + 2];
    memset(longest, 'a', sizeof(longest));
    memcpy(longest, "vcs_", 4);
    longest[VC_SECRET_MAX] = '\0';
    CHECK(vc_valid_secret(longest));
    longest[VC_SECRET_MAX] = 'a';
    longest[VC_SECRET_MAX + 1] = '\0';
    CHECK(!vc_valid_secret(longest));

    CHECK(vc_valid_credential(CRED));
    CHECK(!vc_valid_credential(SECRET));
    CHECK(!vc_valid_credential("vnc_short"));
    CHECK(!vc_valid_credential("vnc_abcdefghijklmnop\n"));
    CHECK(!vc_valid_credential(""));

    CHECK(vc_valid_room("kitchen"));
    CHECK(vc_valid_room("Living Room 2"));
    CHECK(vc_valid_room("a"));
    CHECK(vc_valid_room("bed_room-1"));
    CHECK(!vc_valid_room(""));
    CHECK(!vc_valid_room(" kitchen"));
    CHECK(!vc_valid_room("kitchen "));
    CHECK(!vc_valid_room("kitchen]"));
    CHECK(!vc_valid_room("1234567890123456789012345678901234567890X"));   /* 41 */
    CHECK(vc_valid_room("1234567890123456789012345678901234567890"));     /* 40 */
}

static void test_headers(void)
{
    char auth[VP_AUTH_MAX];
    vp_header_t h[VC_CLAIM_HEADERS];
    const char *token = "abcdefghijklmnopqrstuvwxyz0123456789";
    CHECK(vc_claim_headers(token, "homelink-c86320", NULL, auth, h) == 4);
    CHECK_STR(h[0].name, "Authorization");
    CHECK(!strncmp(h[0].value, "Bearer ", 7) && !strcmp(h[0].value + 7, token));
    CHECK_STR(h[1].name, "X-Node-Id");
    CHECK_STR(h[1].value, "homelink-c86320");
    CHECK_STR(h[2].name, "X-Vesper-Node-Protocol");
    CHECK_STR(h[2].value, "1");
    CHECK_STR(h[3].name, "Accept");
    CHECK_STR(h[3].value, "application/json");
    CHECK(vc_claim_headers(token, "homelink-c86320", SECRET, auth, h) == 5);
    CHECK_STR(h[4].name, "X-Claim-Secret");
    CHECK_STR(h[4].value, SECRET);
    CHECK(vc_claim_headers(token, "homelink-c86320", "vcs_bad\r\n", auth, h) == 0);
    CHECK(vc_claim_headers("", "homelink-c86320", NULL, auth, h) == 0);
    CHECK(vc_claim_headers("tok en", "homelink-c86320", NULL, auth, h) == 0);
    CHECK(vc_claim_headers(token, "homelink c86320", NULL, auth, h) == 0);
    CHECK(vc_claim_headers(token, NULL, NULL, auth, h) == 0);

    vp_header_t ch;
    CHECK(vc_credential_header(CRED, &ch));
    CHECK_STR(ch.name, "X-Node-Credential");
    CHECK_STR(ch.value, CRED);
    CHECK(!vc_credential_header("vnc_x\r\nAuthorization: evil", &ch));
    CHECK(!vc_credential_header(NULL, &ch));

    CHECK(vc_needs_claim(403, "node_unauthorized"));
    CHECK(!vc_needs_claim(403, NULL));
    CHECK(!vc_needs_claim(403, "forbidden"));
    CHECK(!vc_needs_claim(401, "node_unauthorized"));
    CHECK(!vc_needs_claim(200, NULL));

    CHECK(vc_retry_after_s("5", 9) == 5);
    CHECK(vc_retry_after_s(" 60", 9) == 60);
    CHECK(vc_retry_after_s("0", 9) == 1);
    CHECK(vc_retry_after_s("99999999999999999999", 9) == VC_RETRY_AFTER_MAX_S);
    CHECK(vc_retry_after_s("Wed, 21 Oct 2026 07:28:00 GMT", 9) == 9);
    CHECK(vc_retry_after_s(NULL, 9) == 9);
    CHECK(vc_retry_after_s("", 0) == 1);
}

static void test_happy_path(void)
{
    vc_claim_t c;
    char cap[48], cred[VC_CRED_MAX + 1];
    int64_t t = 1000;
    vc_init(&c, false, t);
    CHECK(c.state == VC_START);
    CHECK(vc_due(&c, t) == VC_ACT_START);
    CHECK(vc_wait_ms(&c, t) == 0);
    CHECK_STR(vc_state_name(&c), "starting");
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "GETTING A CLAIM CODE");

    CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_CODE);
    CHECK(c.state == VC_POLL);
    CHECK_STR(c.code, "K7M2-QX9P");
    CHECK_STR(c.secret, SECRET);
    CHECK(c.codes == 1);
    CHECK_STR(vc_state_name(&c), "pending");
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "CLAIM CODE K7M2-QX9P");
    CHECK(strlen(cap) <= 20);   /* two lines of the AIPI's 16 columns: "CLAIM CODE" / "K7M2-QX9P" */
    CHECK(!strstr(cap, "vcs_"));

    /* polls every 3 s */
    CHECK(vc_due(&c, t + 2999) == VC_ACT_NONE);
    CHECK(vc_wait_ms(&c, t + 1000) == 2000);
    CHECK(vc_due(&c, t + 3000) == VC_ACT_POLL);
    t += 3000;
    CHECK(poll(&c, 202, PENDING, NULL, t) == VC_EV_PENDING);
    CHECK(c.polls == 1);
    CHECK(vc_due(&c, t + 3000) == VC_ACT_POLL);
    t += 3000;
    CHECK(poll(&c, 202, PENDING, NULL, t) == VC_EV_PENDING);
    t += 3000;
    /* results for the wrong state are ignored */
    CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_NONE);
    CHECK(poll(&c, 200, POLL_OK, NULL, t) == VC_EV_CLAIMED);
    CHECK(c.state == VC_CLAIMED);
    CHECK_STR(c.room, "kitchen");
    CHECK(all_zero(c.secret, sizeof(c.secret)));   /* the secret is gone with the claim */
    CHECK(!c.code[0]);
    CHECK(vc_due(&c, t + 100000) == VC_ACT_NONE);
    CHECK(vc_wait_ms(&c, t) == -1);
    CHECK_STR(vc_state_name(&c), "claimed");
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "");
    CHECK(vc_take_credential(&c, cred));
    CHECK_STR(cred, CRED);
    CHECK(all_zero(c.credential, sizeof(c.credential)));
    CHECK(!vc_take_credential(&c, cred));
    CHECK(poll(&c, 200, POLL_OK, NULL, t) == VC_EV_NONE);   /* claimed: nothing more */
}

static void test_start_problems(void)
{
    vc_claim_t c;
    char cap[48];
    int64_t t = 0;
    vc_init(&c, false, t);

    /* no response: back off 5, 10, 20, 40, 60, 60 s */
    int64_t expect[] = { 5000, 10000, 20000, 40000, 60000, 60000 };
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        CHECK(start(&c, 0, NULL, NULL, t) == VC_EV_PROBLEM);
        CHECK(c.due_ms == t + expect[i]);
        CHECK(!c.started);
        t = c.due_ms;
    }
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "CAN'T REACH VESPER");
    /* success resets the backoff */
    CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_CODE);
    CHECK(c.backoff_ms == VC_BACKOFF_MIN_MS);

    vc_init(&c, false, t);
    CHECK(start(&c, 429, "{\"error\": \"rate_limited\"}", "5", t) == VC_EV_PROBLEM);
    CHECK(c.due_ms == t + 5000 && c.state == VC_START);
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "VESPER IS BUSY");
    CHECK(start(&c, 429, "{\"error\": \"too_many_pending\"}", "60", t) == VC_EV_PROBLEM);
    CHECK(c.due_ms == t + 60000);
    CHECK(start(&c, 429, NULL, NULL, t) == VC_EV_PROBLEM);
    CHECK(c.due_ms == t + 5000);
    CHECK(start(&c, 503, "{\"error\": \"registry_unavailable\"}", NULL, t) == VC_EV_PROBLEM);
    CHECK(c.due_ms == t + 5000);

    CHECK(start(&c, 401, "{\"error\": \"unauthorized\"}", NULL, t) == VC_EV_PROBLEM);
    CHECK(c.due_ms == t + VC_REFUSED_MS);
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "TOKEN REFUSED");
    CHECK(start(&c, 400, "{\"error\": \"bad_node_id\"}", NULL, t) == VC_EV_PROBLEM);
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "CLAIM REFUSED");
    CHECK(start(&c, 404, "{\"error\": \"not_found\"}", NULL, t) == VC_EV_PROBLEM);   /* a pre-task-08 backend */
    CHECK(c.due_ms == t + VC_REFUSED_MS);
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "CHECK THE SERVER URL");

    /* malformed 200s are server errors, and nothing of them is kept */
    const char *bad[] = {
        "",
        "not json",
        "{\"status\": \"pending\", \"claim_code\": \"K7M2-QX9P\"}",
        "{\"status\": \"pending\", \"claim_secret\": \"" SECRET "\"}",
        "{\"status\": \"claimed\", \"claim_code\": \"K7M2-QX9P\", \"claim_secret\": \"" SECRET "\"}",
        "{\"status\": \"pending\", \"claim_code\": \"K7M2-QX9O\", \"claim_secret\": \"" SECRET "\"}",
        "{\"status\": \"pending\", \"claim_code\": \"K7M2-QX9P\", \"claim_secret\": \"vcs_has space in it ok\"}",
        "{\"status\": \"pending\", \"claim_code\": \"K7M2-QX9P\", \"claim_secret\": 5}",
        "{\"status\": \"pending\", \"claim_code\": \"K7M2-QX9P\", \"claim_secret\": \"vcs_\\r\\nX: abcdefghijklmnop\"}",
        "{\"x\": {\"status\": \"pending\", \"claim_code\": \"K7M2-QX9P\", \"claim_secret\": \"" SECRET "\"}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        vc_init(&c, false, t);
        CHECK(start(&c, 200, bad[i], NULL, t) == VC_EV_PROBLEM);
        CHECK(c.state == VC_START && !c.code[0] && all_zero(c.secret, sizeof(c.secret)));
        vc_caption(&c, cap, sizeof(cap));
        CHECK_STR(cap, "VESPER SERVER ERROR");
    }
    /* a secret longer than fits is cut by the JSON decoder and then refused, never used cut */
    char big[VC_BODY_MAX];
    char sec[VC_SECRET_MAX + 10];
    memset(sec, 'a', sizeof(sec));
    memcpy(sec, "vcs_", 4);
    sec[sizeof(sec) - 1] = '\0';
    snprintf(big, sizeof(big), "{\"status\":\"pending\",\"claim_code\":\"K7M2-QX9P\",\"claim_secret\":\"%s\"}", sec);
    vc_init(&c, false, t);
    CHECK(start(&c, 200, big, NULL, t) == VC_EV_PROBLEM);
    /* JSON escapes decode before validation */
    vc_init(&c, false, t);
    CHECK(start(&c, 200, "{\"status\":\"pending\",\"claim_code\":\"K7M2\\u002dQX9P\",\"claim_secret\":\"" SECRET "\"}",
                NULL, t) == VC_EV_CODE);
    CHECK_STR(c.code, "K7M2-QX9P");
}

static void test_poll_problems(void)
{
    vc_claim_t c;
    char cap[48];
    int64_t t = 50000;
    vc_init(&c, false, t);
    CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_CODE);
    int64_t started = t;

    /* expired / replaced / wrong secret: start over, but not inside the backend's 5 s restart interval */
    t += 3000;
    CHECK(poll(&c, 404, NOT_FOUND, NULL, t) == VC_EV_RESTART);
    CHECK(c.state == VC_START && !c.code[0] && all_zero(c.secret, sizeof(c.secret)));
    CHECK(c.due_ms == started + VC_RESTART_MS);
    CHECK(vc_due(&c, t) == VC_ACT_NONE);
    CHECK(vc_due(&c, started + VC_RESTART_MS) == VC_ACT_START);
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "GETTING A NEW CODE");
    /* a later code replaces the screen's */
    t = started + VC_RESTART_MS;
    CHECK(start(&c, 200, "{\"status\":\"pending\",\"claim_code\":\"WXYZ-2345\",\"claim_secret\":\"" SECRET "\"}", NULL,
                t) == VC_EV_CODE);
    CHECK_STR(c.code, "WXYZ-2345");
    CHECK(c.codes == 2);
    /* a 404 long after the start restarts at once */
    t += 600000;
    CHECK(poll(&c, 404, NOT_FOUND, NULL, t) == VC_EV_RESTART);
    CHECK(c.due_ms == t);

    /* transient problems keep the code on the screen and keep polling */
    vc_init(&c, false, t);
    CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_CODE);
    t += 3000;
    CHECK(poll(&c, 0, NULL, NULL, t) == VC_EV_PROBLEM);
    CHECK(c.state == VC_POLL && c.due_ms == t + 5000);
    vc_caption(&c, cap, sizeof(cap));
    CHECK_STR(cap, "CLAIM CODE K7M2-QX9P");
    CHECK_STR(c.secret, SECRET);
    t = c.due_ms;
    CHECK(poll(&c, 503, "{\"error\": \"registry_unavailable\"}", NULL, t) == VC_EV_PROBLEM);
    CHECK(c.due_ms == t + 10000);   /* backing off */
    t = c.due_ms;
    CHECK(poll(&c, 202, PENDING, NULL, t) == VC_EV_PENDING);
    CHECK(c.due_ms == t + VC_POLL_MS && c.backoff_ms == VC_BACKOFF_MIN_MS);
    CHECK(poll(&c, 429, NULL, "7", t) == VC_EV_PROBLEM);
    CHECK(c.due_ms == t + 7000 && c.state == VC_POLL);
    CHECK(poll(&c, 401, "{\"error\": \"unauthorized\"}", NULL, t) == VC_EV_PROBLEM);
    CHECK(c.state == VC_POLL && c.due_ms == t + VC_REFUSED_MS);

    /* a 200 that doesn't parse: the one delivery is lost; start over */
    const char *bad[] = {
        "{\"status\": \"claimed\", \"room\": \"kitchen\"}",
        "{\"status\": \"claimed\", \"credential\": \"" SECRET "\"}",
        "{\"status\": \"pending\", \"credential\": \"" CRED "\"}",
        "{\"status\": \"claimed\", \"credential\": \"vnc_bad\\nX: abcdefghijklmnopqrstu\"}",
        "garbage",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        vc_init(&c, false, t);
        CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_CODE);
        CHECK(poll(&c, 200, bad[i], NULL, t + 3000) == VC_EV_RESTART);
        CHECK(c.state == VC_START && !c.credential[0] && all_zero(c.secret, sizeof(c.secret)));
        vc_caption(&c, cap, sizeof(cap));
        CHECK_STR(cap, "VESPER SERVER ERROR");
    }
    /* a bad room doesn't lose a good credential (the room is for the screen only) */
    vc_init(&c, false, t);
    CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_CODE);
    CHECK(poll(&c, 200, "{\"status\":\"claimed\",\"room\":\"<script>\",\"credential\":\"" CRED "\"}", NULL, t) ==
          VC_EV_CLAIMED);
    CHECK_STR(c.room, "");
    CHECK_STR(c.credential, CRED);
    vc_wipe(&c);
    CHECK(all_zero(c.credential, sizeof(c.credential)));
}

static void test_reclaim_and_config(void)
{
    vc_claim_t c;
    int64_t t = 7;
    vc_init(&c, true, t);
    CHECK(c.state == VC_CLAIMED && vc_due(&c, t) == VC_ACT_NONE);
    vc_config_changed(&c, t);   /* claimed stays claimed */
    CHECK(c.state == VC_CLAIMED);
    /* 403 node_unauthorized on /turn: claim again now */
    vc_reclaim(&c, t);
    CHECK(c.state == VC_START && vc_due(&c, t) == VC_ACT_START);
    CHECK(start(&c, 200, START_OK, NULL, t) == VC_EV_CODE);
    /* reclaim while already claiming changes nothing */
    vc_reclaim(&c, t + 1);
    CHECK(c.state == VC_POLL && !strcmp(c.code, "K7M2-QX9P"));
    /* new server URL or token: the claim starts over (not before the restart interval) */
    vc_config_changed(&c, t + 1000);
    CHECK(c.state == VC_START && !c.code[0] && all_zero(c.secret, sizeof(c.secret)));
    CHECK(c.due_ms == t + VC_RESTART_MS);
    /* a claimed node that is refused later reclaims after the restart interval of its last start */
    CHECK(start(&c, 200, START_OK, NULL, t + VC_RESTART_MS) == VC_EV_CODE);
    CHECK(poll(&c, 200, POLL_OK, NULL, t + VC_RESTART_MS + 100) == VC_EV_CLAIMED);
    vc_reclaim(&c, t + VC_RESTART_MS + 200);
    CHECK(all_zero(c.credential, sizeof(c.credential)));
    CHECK(c.due_ms == t + 2 * VC_RESTART_MS);
}

/* Arbitrary bytes as bodies and headers: no crash, no overrun (ASan/UBSan), never claimed. */
static void test_fuzz(void)
{
    vc_claim_t c;
    char body[VC_BODY_MAX];
    srand(11);
    int claimed = 0;
    for (int i = 0; i < 20000; i++) {
        size_t n = (size_t)(rand() % (int)sizeof(body));
        for (size_t k = 0; k < n; k++) {
            /* JSON-ish bytes, so the parser gets deep */
            static const char pool[] = "{}[]\":,\\u0aZ vcs_nc_K7M2-QX9P statusclaimedpending credential\x80\xff";
            body[k] = rand() % 4 ? pool[rand() % (int)(sizeof(pool) - 1)] : (char)rand();
        }
        vc_init(&c, false, 0);
        int st = (int[]){ 0, 200, 202, 404, 429, 500 }[rand() % 6];
        vc_start_result(&c, st, body, n, NULL, 0);
        c.state = VC_POLL;
        claimed += vc_poll_result(&c, st, body, n, NULL, 1) == VC_EV_CLAIMED;
    }
    CHECK(claimed == 0);
    /* exact shapes still claim after all that */
    vc_init(&c, false, 0);
    CHECK(start(&c, 200, START_OK, NULL, 0) == VC_EV_CODE);
    CHECK(poll(&c, 200, POLL_OK, NULL, 3000) == VC_EV_CLAIMED);
}

int main(void)
{
    test_validation();
    test_headers();
    test_happy_path();
    test_start_problems();
    test_poll_problems();
    test_reclaim_and_config();
    test_fuzz();
    printf("vesper_claim: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
