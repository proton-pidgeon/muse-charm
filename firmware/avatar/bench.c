/* Host benchmark: per-frame cost of muse_pixel_render() + a full 128x128 muse_pixel_scale().
 *
 *   cc -O2 -I $SDK/esp32/components/muse bench.c muse_pixel.c -lm -o /tmp/vesper_bench && /tmp/vesper_bench
 *
 * Prints min/mean/max microseconds per mode. -O2 matches the firmware's default optimisation closely
 * enough for a proxy; the S3 estimate lives in firmware/README.md.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "muse_pixel.h"

#define SIZE 128
#define FRAMES 2000

static uint16_t buf[SIZE * SIZE];

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(void)
{
    static const char *const NAMES[MUSE_MODE_COUNT] = { "boot", "idle", "listening", "thinking", "speaking", "error", "off" };
    muse_pixel_set_size(SIZE);
    {
        /* The first frame also rasterises the static face (once per boot). */
        muse_pose_t p = { .mode = MUSE_MODE_BOOT, .t = 0.04f, .mode_t = 0 };
        double a = now_us();
        muse_pixel_render(&p);
        printf("first frame (builds the static base once): %.1f us\n", now_us() - a);
    }
    double worst_total = 0;
    for (int m = 0; m < MUSE_MODE_COUNT; m++) {
        for (int pet = 0; pet < 2; pet++) {
            double rmin = 1e9, rsum = 0, rmax = 0, smin = 1e9, ssum = 0, smax = 0;
            for (int i = 0; i < FRAMES; i++) {
                float t = 10.0f + i * 0.04f;
                muse_pose_t p = { .mode = (muse_mode_t)m, .t = t, .mode_t = (i % 100) * 0.04f,
                                  .level = 0.5f + 0.5f * sinf(t * 6.3f), .happy = pet ? 1.0f : 0.0f };
                double a = now_us();
                muse_pixel_render(&p);
                double b = now_us();
                muse_pixel_scale(buf, SIZE, 0, SIZE - 1, 0, SIZE - 1);
                double c = now_us();
                double r = b - a, s = c - b;
                if (i < 50) {
                    continue;   /* warm-up: caches, palette blend */
                }
                rsum += r; ssum += s;
                if (r < rmin) rmin = r;
                if (r > rmax) rmax = r;
                if (s < smin) smin = s;
                if (s > smax) smax = s;
                if (r + s > worst_total) worst_total = r + s;
            }
            int n = FRAMES - 50;
            printf("%-10s%-6s render us min/mean/max %6.1f %6.1f %6.1f | scale128 us %5.1f %5.1f %5.1f\n",
                   NAMES[m], pet ? "+pet" : "", rmin, rsum / n, rmax, smin, ssum / n, smax);
        }
    }
    printf("worst single frame render+scale: %.1f us\n", worst_total);
    return 0;
}
