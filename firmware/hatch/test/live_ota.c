/*
 * The node's firmware update check against a real node backend, through the
 * firmware's own update code (vesper_ota.c + vesper_proto.c + vesper_claim.c):
 * the same headers, manifest parsing, version verdict and image URL. libcurl
 * stands in for esp_http_client, and a file for the update slot.
 *
 *   live_ota <base-url> <credential-file> <running-version> <image-out>
 *
 * Prints the verdict (vo_verdict_code) on the first line. For "newer" it
 * downloads the image to <image-out> the way ota.c does (same headers, no
 * redirects) and checks, as ota.c does before the boot partition switches,
 * that exactly the manifest's size arrived; the script then compares the
 * SHA-256 (ota.c does that on the device by reading the slot back).
 *
 * The node token comes from $VESPER_NODE_TOKEN. Neither it nor the
 * credential is ever printed. Use hatch/test/live_ota.sh, which starts a
 * throwaway backend with a temp registry and firmware store.
 */
#include "vesper_ota.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODE_ID "homelink-otatest"   /* not a real board's id */
#define IMAGE_MAX (8u * 1024 * 1024)
#define SLOT (4u * 1024 * 1024)

typedef struct {
    char *buf;
    size_t len, cap;
    long status;
} resp_t;

static size_t on_body(char *data, size_t size, size_t nitems, void *ud)
{
    resp_t *r = ud;
    size_t n = size * nitems;
    if (r->len + n > r->cap) {
        return 0;   /* too large: abort */
    }
    memcpy(r->buf + r->len, data, n);
    r->len += n;
    return n;
}

static int get(const char *url, const vp_header_t *h, int nh, resp_t *r)
{
    CURL *c = curl_easy_init();
    if (!c) {
        return -1;
    }
    struct curl_slist *list = NULL;
    char line[VP_AUTH_MAX + 64];
    for (int i = 0; i < nh; i++) {
        snprintf(line, sizeof(line), "%s: %s", h[i].name, h[i].value);
        list = curl_slist_append(list, line);
    }
    memset(line, 0, sizeof(line));
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);   /* ota.c: disable_auto_redirect */
    CURLcode rc = curl_easy_perform(c);
    r->status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->status);
    }
    curl_slist_free_all(list);   /* note: libcurl's copies; ours were wiped above */
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: live_ota <base-url> <credential-file> <running-version> <image-out>\n");
        return 2;
    }
    const char *token = getenv("VESPER_NODE_TOKEN");
    char cred[VC_CRED_MAX + 2] = { 0 };
    FILE *f = fopen(argv[2], "r");
    if (!f || !fgets(cred, sizeof(cred), f)) {
        fprintf(stderr, "no credential file\n");
        return 2;
    }
    fclose(f);
    cred[strcspn(cred, "\r\n")] = '\0';
    vp_url_t base;
    char url[VP_URL_MAX], auth[VP_AUTH_MAX];
    vp_header_t h[VO_HEADERS];
    if (!vp_url_parse(argv[1], &base) || !vo_manifest_url(&base, url, sizeof(url)) ||
        !vo_headers(token, NODE_ID, cred, argv[3], auth, h)) {
        fprintf(stderr, "bad base URL, token or credential\n");
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    char body[VO_BODY_MAX];
    resp_t r = { body, 0, sizeof(body) - 1, 0 };
    int rc = get(url, h, VO_HEADERS, &r);
    body[r.len] = '\0';
    vo_manifest_t m;
    vo_verdict_t v = vo_check_result(rc < 0 ? 0 : (int)r.status, body, r.len, argv[3], NULL, SLOT, &m);
    printf("%s\n", vo_verdict_code(v));
    printf("manifest: HTTP %ld, %s%s%s\n", r.status, vo_verdict_name(v), m.version[0] ? ", published " : "",
           m.version);
    int exit_code = 0;
    if (v == VO_INSTALL) {
        char *img = malloc(IMAGE_MAX);
        resp_t ir = { img, 0, IMAGE_MAX, 0 };
        if (!img || !vo_image_url(&base, &m, url, sizeof(url)) || get(url, h, VO_HEADERS, &ir) < 0 ||
            ir.status != 200) {
            printf("image: download failed (HTTP %ld)\n", ir.status);
            exit_code = 1;
        } else if (ir.len != m.size) {
            printf("image: %zu bytes, the manifest says %u: not installed\n", ir.len, (unsigned)m.size);
            exit_code = 1;
        } else {
            FILE *o = fopen(argv[4], "wb");
            if (!o || fwrite(img, 1, ir.len, o) != ir.len) {
                exit_code = 1;
            }
            if (o) {
                fclose(o);
            }
            printf("image: %zu bytes = the manifest's size; sha256 %s (to be checked)\n", ir.len, m.sha256_hex);
        }
        free(img);
    }
    memset(auth, 0, sizeof(auth));
    memset(cred, 0, sizeof(cred));
    curl_global_cleanup();
    return exit_code;
}
