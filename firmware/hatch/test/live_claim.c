/*
 * The claim ceremony against a real node backend, through the firmware's own
 * claim code (vesper_claim.c + vesper_proto.c): the same headers, the same
 * response parsing, the same state machine and poll verdicts. libcurl stands
 * in for esp_http_client and a 600-mode file for NVS.
 *
 *   live_claim claim <base-url> <code-file> <nvs-file>
 *       runs the claim flow as a fresh node: /claim/start, writes the claim
 *       code to <code-file> (for the approver; it is never printed), polls
 *       every 3 s until the credential arrives, stores it in <nvs-file>
 *   live_claim auth <base-url> <nvs-file>
 *       a "reboot": reads the stored credential and checks that /turn and
 *       /audio accept it, and that without it (or with a wrong one) the
 *       backend says 403 node_unauthorized, which the firmware takes as
 *       "claim again"
 *
 * The node token comes from $VESPER_NODE_TOKEN. Neither it, the claim secret
 * nor the credential is ever printed. Use hatch/test/live_claim.sh, which
 * starts a throwaway backend and approves the code with the CLI.
 */
#include "vesper_claim.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NODE_ID "homelink-claimtest"   /* not a real board's id */
#define MAX_POLLS 60                   /* 3 minutes for the approver */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int64_t ms)
{
    if (ms > 0) {
        struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
}

typedef struct {
    char body[VC_BODY_MAX];
    size_t len;
    char retry_after[16];
    long status;
} resp_t;

static size_t on_header(char *buf, size_t size, size_t nitems, void *ud)
{
    resp_t *r = ud;
    size_t n = size * nitems;
    if (n > 12 && !strncasecmp(buf, "retry-after:", 12)) {
        size_t i = 12, o = 0;
        while (i < n && buf[i] == ' ') {
            i++;
        }
        while (i < n && o + 1 < sizeof(r->retry_after) && buf[i] != '\r' && buf[i] != '\n') {
            r->retry_after[o++] = buf[i++];
        }
        r->retry_after[o] = '\0';
    }
    return n;
}

static size_t on_body(char *buf, size_t size, size_t nitems, void *ud)
{
    resp_t *r = ud;
    size_t n = size * nitems;
    size_t k = n < sizeof(r->body) - 1 - r->len ? n : sizeof(r->body) - 1 - r->len;
    memcpy(r->body + r->len, buf, k);
    r->len += k;
    r->body[r->len] = '\0';
    return n;
}

/* One request; extra headers are "Name: value" lines. Returns the status (0: no response). */
static int request(const char *method, const char *url, const vp_header_t *h, int nh, const char *body,
                   const char *content_type, resp_t *r)
{
    memset(r, 0, sizeof(*r));
    struct curl_slist *list = NULL;
    char line[VP_AUTH_MAX + 64];
    for (int i = 0; i < nh; i++) {
        snprintf(line, sizeof(line), "%s: %s", h[i].name, h[i].value);
        list = curl_slist_append(list, line);
    }
    if (content_type) {
        snprintf(line, sizeof(line), "Content-Type: %s", content_type);
        list = curl_slist_append(list, line);
    }
    list = curl_slist_append(list, "Expect:");
    memset(line, 0, sizeof(line));
    CURL *c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (!strcmp(method, "POST")) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    CURLcode res = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->status);
    curl_easy_cleanup(c);
    curl_slist_free_all(list);
    return res == CURLE_OK ? (int)r->status : 0;
}

static bool write_private(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return false;
    }
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t)n;
    return close(fd) == 0 && ok;
}

static const char *event_name(vc_event_t e)
{
    static const char *const names[] = { "none", "code", "pending", "claimed", "restart", "problem" };
    return names[e];
}

static int run_claim(const char *token, const vp_url_t *base, const char *code_file, const char *nvs_file)
{
    char start_url[VP_URL_MAX], poll_url[VP_URL_MAX];
    vp_url_join(base, "/claim/start", start_url, sizeof(start_url));
    vp_url_join(base, "/claim/poll", poll_url, sizeof(poll_url));
    static vc_claim_t c;
    vc_init(&c, false, now_ms());
    char auth[VP_AUTH_MAX], cap[48];
    vp_header_t h[VC_CLAIM_HEADERS];
    resp_t r;
    uint32_t codes_seen = 0;
    for (int steps = 0; steps < MAX_POLLS + 10; steps++) {
        int64_t wait = vc_wait_ms(&c, now_ms());
        sleep_ms(wait);
        vc_action_t a = vc_due(&c, now_ms());
        if (a == VC_ACT_START) {
            int nh = vc_claim_headers(token, NODE_ID, NULL, auth, h);
            int st = request("POST", start_url, h, nh, NULL, NULL, &r);
            vc_event_t ev = vc_start_result(&c, st, r.body, r.len, r.retry_after, now_ms());
            vc_caption(&c, cap, sizeof(cap));
            /* the caption carries the code: print the state, never the caption */
            printf("POST /claim/start -> HTTP %d: %s (state %s)\n", st, event_name(ev), vc_state_name(&c));
            if (ev == VC_EV_CODE && c.codes != codes_seen) {
                codes_seen = c.codes;
                if (!write_private(code_file, c.code)) {
                    fprintf(stderr, "can't write the code file\n");
                    return 1;
                }
                printf("  claim code %d written for the approver (%zu chars, secret held in RAM only)\n",
                       (int)c.codes, strlen(c.code));
            }
        } else if (a == VC_ACT_POLL) {
            int nh = vc_claim_headers(token, NODE_ID, c.secret, auth, h);
            int st = request("POST", poll_url, h, nh, NULL, NULL, &r);
            unsigned n = (unsigned)c.polls + 1;
            vc_event_t ev = vc_poll_result(&c, st, r.body, r.len, r.retry_after, now_ms());
            printf("POST /claim/poll  -> HTTP %d: %s (poll %u)\n", st, event_name(ev), n);
            memset(&r, 0, sizeof(r));   /* the 200 body held the credential */
            if (ev == VC_EV_CLAIMED) {
                char cred[VC_CRED_MAX + 1];
                vc_take_credential(&c, cred);
                bool stored = write_private(nvs_file, cred);
                printf("claimed: room \"%s\"; credential (%zu chars, %s...) stored in the NVS stand-in: %s\n", c.room,
                       strlen(cred), VC_CRED_PREFIX, stored ? "ok" : "FAILED");
                memset(cred, 0, sizeof(cred));
                memset(auth, 0, sizeof(auth));
                vc_wipe(&c);
                return stored ? 0 : 1;
            }
            if (c.polls >= MAX_POLLS) {
                break;
            }
        }
    }
    memset(auth, 0, sizeof(auth));
    vc_wipe(&c);
    fprintf(stderr, "not claimed in time\n");
    return 1;
}

/* The "error" field of a JSON error body, into err (VP_CODE_MAX); "" if none. */
static const char *err_of(const resp_t *r, char *err)
{
    if (vp_json_string(r->body, r->len, "error", err, VP_CODE_MAX) != VP_JSON_STRING) {
        err[0] = '\0';
    }
    return err;
}

static bool expect(const char *what, int got, int want, const char *err, const char *want_err)
{
    bool ok = got == want && (!want_err || (err && !strcmp(err, want_err)));
    printf("  %-58s HTTP %d %s  %s\n", what, got, err && err[0] ? err : "-", ok ? "ok" : "UNEXPECTED");
    return ok;
}

static int run_auth(const char *token, const vp_url_t *base, const char *nvs_file)
{
    char cred[VC_CRED_MAX + 2] = "";
    FILE *f = fopen(nvs_file, "r");
    if (!f || !fgets(cred, sizeof(cred), f)) {
        fprintf(stderr, "no stored credential\n");
        if (f) {
            fclose(f);
        }
        return 1;
    }
    fclose(f);
    cred[strcspn(cred, "\r\n")] = '\0';
    printf("after a reboot: stored credential %s\n", vc_valid_credential(cred) ? "present and well-formed" : "MALFORMED");
    if (!vc_valid_credential(cred)) {
        return 1;
    }
    char turn_url[VP_URL_MAX], audio_url[VP_URL_MAX], auth[VP_AUTH_MAX];
    vp_url_join(base, "/turn", turn_url, sizeof(turn_url));
    vp_url_join(base, "/audio/AAAAAAAAAAAAAAAAAAAAAAAA.mp3", audio_url, sizeof(audio_url));
    /* the firmware's /turn headers: vp_turn_headers + X-Node-Credential */
    vp_header_t h[VP_TURN_HEADERS + 1];
    if (!vp_turn_headers(token, NODE_ID, auth, h) || !vc_credential_header(cred, &h[VP_TURN_HEADERS])) {
        return 1;
    }
    /* Content-Type is in h already (audio/wav); a junk body passes auth and fails WAV checks */
    const char *junk = "RIFF\x04\x00\x00\x00WAVE";
    resp_t r;
    char err[VP_CODE_MAX];
    bool ok = true;
    char cap[48];
    int retry_ms;

    int st = request("POST", turn_url, h, VP_TURN_HEADERS + 1, junk, NULL, &r);
    ok &= expect("POST /turn with the credential (junk note)", st, 422, err_of(&r, err), "bad_audio");

    vp_header_t audio_h[3] = { h[0], h[1], h[VP_TURN_HEADERS] };
    st = request("GET", audio_url, audio_h, 3, NULL, NULL, &r);
    ok &= expect("GET /audio/<unknown id> with the credential", st, 404, err_of(&r, err), "not_found");

    st = request("POST", turn_url, h, VP_TURN_HEADERS, junk, NULL, &r);
    ok &= expect("POST /turn without the credential", st, 403, err_of(&r, err), "node_unauthorized");
    bool reclaim = vc_needs_claim(st, err);
    vp_http_verdict(st, NULL, err, NULL, 0, &retry_ms, cap, sizeof(cap));
    printf("    firmware verdict: caption \"%s\", claim again: %s\n", cap, reclaim ? "yes" : "NO");
    ok &= reclaim;
    if (reclaim) {
        static vc_claim_t c;
        vc_init(&c, true, now_ms());
        vc_reclaim(&c, now_ms());
        printf("    claim state after the 403: %s, next action %s\n", vc_state_name(&c),
               vc_due(&c, now_ms()) == VC_ACT_START ? "POST /claim/start" : "?");
        ok &= c.state == VC_START;
    }

    char wrong[VC_CRED_MAX + 1];
    snprintf(wrong, sizeof(wrong), "%s", cred);
    wrong[strlen(wrong) - 1] = wrong[strlen(wrong) - 1] == 'A' ? 'B' : 'A';
    vp_header_t wh[VP_TURN_HEADERS + 1];
    memcpy(wh, h, sizeof(h));
    vc_credential_header(wrong, &wh[VP_TURN_HEADERS]);
    st = request("POST", turn_url, wh, VP_TURN_HEADERS + 1, junk, NULL, &r);
    ok &= expect("POST /turn with a wrong credential", st, 403, err_of(&r, err), "node_unauthorized");

    vp_header_t wa[3] = { wh[0], wh[1], wh[VP_TURN_HEADERS] };
    st = request("GET", audio_url, wa, 3, NULL, NULL, &r);
    ok &= expect("GET /audio/<unknown id> with a wrong credential", st, 403, err_of(&r, err), "node_unauthorized");

    memset(cred, 0, sizeof(cred));
    memset(wrong, 0, sizeof(wrong));
    memset(auth, 0, sizeof(auth));
    printf("%s\n", ok ? "auth: OK" : "auth: FAILED");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool claim = argc == 5 && !strcmp(argv[1], "claim");
    bool auth = argc == 4 && !strcmp(argv[1], "auth");
    if (!claim && !auth) {
        fprintf(stderr, "usage: %s claim <base-url> <code-file> <nvs-file> | auth <base-url> <nvs-file>\n", argv[0]);
        return 2;
    }
    const char *token = getenv("VESPER_NODE_TOKEN");
    vp_url_t base;
    if (!vp_url_parse(argv[2], &base)) {
        fprintf(stderr, "bad base url\n");
        return 2;
    }
    if (!vp_valid_token(token)) {
        fprintf(stderr, "VESPER_NODE_TOKEN missing or malformed\n");
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc = claim ? run_claim(token, &base, argv[3], argv[4]) : run_auth(token, &base, argv[3]);
    curl_global_cleanup();
    return rc;
}
