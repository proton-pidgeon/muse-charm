/*
 * Wake word (task 22): see vesper_mww.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_mww.h"

#include <math.h>
#include <string.h>

/* ---- uint16 frontend feature -> int8 model input ---- */

int8_t vm_quant_direct(uint16_t feature, float scale, int zero_point)
{
    /* eval.py: np.round(u16.astype(float64) / 25.6 / scale) + zp, clipped. np.round rounds
     * half to even, as nearbyint() does in the default rounding mode. */
    double v = nearbyint((double)feature / VM_FEATURE_SCALE / (double)scale) + zero_point;
    if (v < -128.0) {
        return -128;
    }
    if (v > 127.0) {
        return 127;
    }
    return (int8_t)v;
}

bool vm_quant_init(vm_quant_t *q, float scale, int zero_point)
{
    if (!(scale > 0.0f) || !isfinite(scale) || zero_point < -128 || zero_point > 127) {
        return false;
    }
    for (int i = 0; i < VM_QUANT_LUT; i++) {
        q->lut[i] = vm_quant_direct((uint16_t)i, scale, zero_point);
    }
    /* Features grow monotonically into the input; the last entry stands for every larger
     * feature only if it is already saturated. */
    return q->lut[VM_QUANT_LUT - 1] == 127;
}

int8_t vm_quant(const vm_quant_t *q, uint16_t feature)
{
    return q->lut[feature < VM_QUANT_LUT ? feature : VM_QUANT_LUT - 1];
}

/* ---- 3 frames -> one model input ---- */

void vm_stack_reset(vm_stack_t *s)
{
    s->frames = 0;
}

bool vm_stack_push(vm_stack_t *s, const vm_quant_t *q, const uint16_t *features)
{
    if (s->frames >= VM_STRIDE) {
        s->frames = 0;   /* the last input was handed over: start the next */
    }
    int8_t *row = s->in + s->frames * VM_FEATURES;
    for (int i = 0; i < VM_FEATURES; i++) {
        row[i] = vm_quant(q, features[i]);
    }
    return ++s->frames == VM_STRIDE;
}

/* ---- The decision rule ---- */

int vm_cutoff_u8(int permille)
{
    if (permille < 0) {
        permille = 0;
    } else if (permille > 1000) {
        permille = 1000;
    }
    int x = permille * 255;   /* in 1/1000 */
    int base = x / 1000, rem = x % 1000;
    if (rem > 500 || (rem == 500 && (base & 1))) {
        base++;
    }
    return base;
}

void vm_detect_init(vm_detect_t *d, int window, int cutoff_permille)
{
    memset(d, 0, sizeof(*d));
    d->w = window < 1 ? 1 : window > VM_WINDOW_MAX ? VM_WINDOW_MAX : window;
    vm_detect_set_cutoff(d, cutoff_permille);
    vm_detect_reset(d);
}

void vm_detect_set_cutoff(vm_detect_t *d, int cutoff_permille)
{
    d->need = (unsigned)vm_cutoff_u8(cutoff_permille) * (unsigned)d->w;
}

void vm_detect_reset(vm_detect_t *d)
{
    memset(d->win, 0, sizeof(d->win));
    d->pos = 0;
    d->sum = 0;
    d->ignore = VM_COOLDOWN;
    d->n = 0;
    d->peak = 0;
    d->peak_sum = 0;
}

bool vm_detect_push(vm_detect_t *d, uint8_t p)
{
    d->sum = d->sum - d->win[d->pos] + p;
    d->win[d->pos] = p;
    d->pos = (d->pos + 1) % d->w;
    d->n++;
    if (p > d->peak) {
        d->peak = p;
    }
    /* The window keeps filling during the cooldown, as eval.py's moving sum does. */
    if (d->ignore > 0) {
        d->ignore--;
        return false;
    }
    if (d->sum > d->peak_sum) {
        d->peak_sum = d->sum;
    }
    if (d->sum > d->need) {
        d->ignore = VM_COOLDOWN;
        return true;
    }
    return false;
}
