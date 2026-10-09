/*
 * Host run of the firmware's "Hey Vesper" engine (hatch/vesper_wakeword_engine.cc
 * + hatch/vesper_mww.c, unchanged, with TFLite Micro's reference kernels and
 * the same microfrontend sources the firmware links).
 *
 *   vwe_host [--chunk N] [--cutoff PERMILLE] [--window W] [--dump DIR] model.tflite a.wav [b.wav ...]
 *
 * Each WAV (16 kHz mono int16) starts from a reset engine, as eval.py scores
 * every clip from a fresh interpreter, and is fed in N-sample chunks (320 =
 * the firmware's 20 ms mic chunk). One JSON line per file: inferences,
 * detections, the first detection's inference index, the peak output. As in
 * the firmware, a detection drops the rest of its chunk, so with 20 ms chunks
 * only the audio up to the first detection is comparable with eval.py. With --dump, DIR/<k>.in (int8 [n][3][40]) and
 * DIR/<k>.p (uint8 [n]) for check.py to compare with the training pipeline.
 * The last line: arena bytes used and the mean time per inference on this host.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this project.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "vesper_wakeword_engine.h"

static bool read_file(const char *path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(f);
    return true;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool read_wav(const char *path, std::vector<int16_t> &pcm)
{
    std::vector<uint8_t> b;
    if (!read_file(path, b) || b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) {
        return false;
    }
    bool fmt_ok = false;
    for (size_t off = 12; off + 8 <= b.size();) {
        uint32_t len = le32(&b[off + 4]);
        const uint8_t *d = &b[off + 8];
        if (off + 8 + len > b.size()) {
            len = (uint32_t)(b.size() - off - 8);
        }
        if (!memcmp(&b[off], "fmt ", 4) && len >= 16) {
            unsigned fmt = d[0] | d[1] << 8, ch = d[2] | d[3] << 8, bits = d[14] | d[15] << 8;
            fmt_ok = fmt == 1 && ch == 1 && le32(d + 4) == 16000 && bits == 16;
        } else if (!memcmp(&b[off], "data", 4)) {
            if (!fmt_ok) {
                return false;
            }
            pcm.resize(len / 2);
            memcpy(pcm.data(), d, pcm.size() * 2);
            return true;
        }
        off += 8 + len + (len & 1);
    }
    return false;
}

int main(int argc, char **argv)
{
    size_t chunk = 320;
    int cutoff = 650, window = 3;
    const char *dump = nullptr;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i += 2) {
        if (i + 1 >= argc) {
            return 2;
        }
        if (!strcmp(argv[i], "--chunk")) {
            chunk = (size_t)atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "--cutoff")) {
            cutoff = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "--window")) {
            window = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "--dump")) {
            dump = argv[i + 1];
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (argc - i < 2 || chunk == 0) {
        fprintf(stderr, "usage: vwe_host [--chunk N] [--cutoff PERMILLE] [--window W] [--dump DIR] model.tflite a.wav...\n");
        return 2;
    }
    std::vector<uint8_t> model;
    if (!read_file(argv[i], model)) {
        fprintf(stderr, "can't read %s\n", argv[i]);
        return 1;
    }
    alignas(16) static uint8_t arena[96 * 1024];
    vwe_error_t err = vwe_init(model.data(), model.size(), arena, sizeof(arena), window, cutoff);
    if (err) {
        fprintf(stderr, "vwe_init: %s\n", err);
        return 1;
    }
    double secs = 0;
    unsigned long total = 0;
    for (int k = 0, a = i + 1; a < argc; a++, k++) {
        std::vector<int16_t> pcm;
        if (!read_wav(argv[a], pcm)) {
            fprintf(stderr, "not a 16 kHz mono 16-bit WAV: %s\n", argv[a]);
            return 1;
        }
        vwe_reset();
        FILE *fin = nullptr, *fp = nullptr;
        if (dump) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/%d.in", dump, k);
            fin = fopen(path, "wb");
            snprintf(path, sizeof(path), "%s/%d.p", dump, k);
            fp = fopen(path, "wb");
            if (!fin || !fp) {
                fprintf(stderr, "can't write to %s\n", dump);
                return 1;
            }
        }
        unsigned long inf = 0, det = 0;
        long first = -1;   /* the inference (0-based in this file) of the first detection */
        for (size_t off = 0; off < pcm.size(); off += chunk) {
            size_t n = pcm.size() - off < chunk ? pcm.size() - off : chunk;
            unsigned ran = 0;
            auto t0 = std::chrono::steady_clock::now();
            bool heard = vwe_feed(pcm.data() + off, n, &ran);
            secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            inf += ran;
            if (ran && fin) {
                fwrite(vwe_last_input(), 1, VM_STRIDE * VM_FEATURES, fin);
                uint8_t p = vwe_last();
                fwrite(&p, 1, 1, fp);
            }
            if (heard && first < 0) {
                first = (long)inf - 1;
            }
            det += heard;
        }
        total += inf;
        if (fin) {
            fclose(fin);
            fclose(fp);
        }
        printf("{\"file\":\"%s\",\"inferences\":%lu,\"detections\":%lu,\"first_detection\":%ld,\"peak\":%u,"
               "\"peak_sum\":%u}\n",
               argv[a], inf, det, first, vwe_detector()->peak, vwe_detector()->peak_sum);
    }
    printf("{\"arena_used\":%zu,\"inferences\":%lu,\"us_per_feed_with_inference\":%.1f}\n", vwe_arena_used(), total,
           total ? secs * 1e6 / (double)total : 0.0);
    return 0;
}
