/*
 * Wake word (task 22): the "Hey Vesper" engine. See vesper_wakeword_engine.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources. The structure
 * (frontend settings, streaming op set, resource variables) follows ESPHome's
 * micro_wake_word component, the reference consumer of microWakeWord models.
 */
#include "vesper_wakeword_engine.h"

#include <new>
#include <string.h>

#include "frontend.h"
#include "frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
/* The engine's state (about 4 KB) in PSRAM, like the arena: internal RAM is what Wi-Fi, BLE
 * and the display share. Plain .bss on the host. */
#define VWE_BSS EXT_RAM_BSS_ATTR
#else
#define VWE_BSS
#endif

namespace {

constexpr int kSampleRate = 16000;
constexpr int kOps = 13;
constexpr int kMaxVariables = 20;
/* The resource variables' bookkeeping (not their buffers: those come from the
 * tensor arena). ESPHome uses 1 KB; 64-bit hosts need a little more. */
constexpr size_t kVarArena = 2048;

using Resolver = tflite::MicroMutableOpResolver<kOps>;

bool s_ready;
bool s_frontend_up;
const tflite::Model *s_model;
uint8_t *s_arena;
size_t s_arena_size;
alignas(16) uint8_t s_var_arena[kVarArena] VWE_BSS;
alignas(Resolver) uint8_t s_resolver_mem[sizeof(Resolver)] VWE_BSS;
Resolver *s_resolver;
alignas(tflite::MicroInterpreter) uint8_t s_interp_mem[sizeof(tflite::MicroInterpreter)] VWE_BSS;
tflite::MicroInterpreter *s_interp;
struct FrontendState s_frontend VWE_BSS;
vm_quant_t s_quant VWE_BSS;
vm_stack_t s_stack VWE_BSS;
vm_detect_t s_detect VWE_BSS;
size_t s_arena_used;
uint8_t s_last;
int8_t s_last_in[VM_STRIDE * VM_FEATURES] VWE_BSS;   /* what the last inference was given */
unsigned long s_inferences;

bool add_ops(Resolver *r)
{
    /* The 13 ops of a microWakeWord streaming model (firmware/wakeword/README.md). */
    return r->AddCallOnce() == kTfLiteOk && r->AddVarHandle() == kTfLiteOk && r->AddReadVariable() == kTfLiteOk &&
           r->AddAssignVariable() == kTfLiteOk && r->AddConv2D() == kTfLiteOk &&
           r->AddDepthwiseConv2D() == kTfLiteOk && r->AddFullyConnected() == kTfLiteOk &&
           r->AddLogistic() == kTfLiteOk && r->AddReshape() == kTfLiteOk && r->AddStridedSlice() == kTfLiteOk &&
           r->AddSplitV() == kTfLiteOk && r->AddConcatenation() == kTfLiteOk && r->AddQuantize() == kTfLiteOk;
}

void drop_interpreter()
{
    if (s_interp) {
        s_interp->~MicroInterpreter();
        s_interp = nullptr;
    }
}

/* A fresh interpreter: fresh streaming state (the model's ring buffers start at their
 * zero point and its CALL_ONCE initialiser runs again), as eval.py scores every track. */
vwe_error_t build_interpreter()
{
    drop_interpreter();
    tflite::MicroAllocator *va = tflite::MicroAllocator::Create(s_var_arena, sizeof(s_var_arena));
    tflite::MicroResourceVariables *vars = va ? tflite::MicroResourceVariables::Create(va, kMaxVariables) : nullptr;
    if (!vars) {
        return "no room for the resource variables";
    }
    s_interp = new (s_interp_mem) tflite::MicroInterpreter(s_model, *s_resolver, s_arena, s_arena_size, vars);
    if (s_interp->AllocateTensors() != kTfLiteOk) {
        drop_interpreter();
        return "the tensor arena is too small or the model needs an op this build lacks";
    }
    s_arena_used = s_interp->arena_used_bytes();
    return nullptr;
}

vwe_error_t check_contract()
{
    TfLiteTensor *in = s_interp->input(0);
    TfLiteTensor *out = s_interp->output(0);
    if (s_interp->inputs_size() != 1 || s_interp->outputs_size() != 1 || !in || !out) {
        return "the model doesn't have one input and one output";
    }
    if (in->type != kTfLiteInt8 || !in->dims || in->dims->size != 3 || in->dims->data[0] != 1 ||
        in->dims->data[1] != VM_STRIDE || in->dims->data[2] != VM_FEATURES) {
        return "the model's input isn't int8 [1, 3, 40]";
    }
    if (out->type != kTfLiteUInt8 || !out->dims || out->dims->size != 2 || out->dims->data[0] != 1 ||
        out->dims->data[1] != 1) {
        return "the model's output isn't uint8 [1, 1]";
    }
    if (!vm_quant_init(&s_quant, in->params.scale, in->params.zero_point)) {
        return "the model's input quantisation is unusable";
    }
    return nullptr;
}

bool start_frontend()
{
    /* microWakeWord's training features (pymicro_features) and ESPHome's micro_wake_word:
     * 30 ms window, 10 ms step, 40 channels, 125-7500 Hz, noise reduction (smoothing
     * bits 10, even 0.025, odd 0.06, min signal 0.05), PCAN on (0.95, 80, 21 bits), log
     * scale shift 6. Everything else at the TFLM defaults, as there. */
    struct FrontendConfig c;
    FrontendFillConfigWithDefaults(&c);
    c.window.size_ms = 30;
    c.window.step_size_ms = 10;
    c.filterbank.num_channels = VM_FEATURES;
    c.filterbank.lower_band_limit = 125.0f;
    c.filterbank.upper_band_limit = 7500.0f;
    c.noise_reduction.smoothing_bits = 10;
    c.noise_reduction.even_smoothing = 0.025f;
    c.noise_reduction.odd_smoothing = 0.06f;
    c.noise_reduction.min_signal_remaining = 0.05f;
    c.pcan_gain_control.enable_pcan = 1;
    c.pcan_gain_control.strength = 0.95f;
    c.pcan_gain_control.offset = 80.0f;
    c.pcan_gain_control.gain_bits = 21;
    c.log_scale.enable_log = 1;
    c.log_scale.scale_shift = 6;
    return FrontendPopulateState(&c, &s_frontend, kSampleRate) != 0;
}

}  // namespace

extern "C" vwe_error_t vwe_init(const uint8_t *model, size_t model_len, uint8_t *arena, size_t arena_size, int window,
                                int cutoff_permille)
{
    if (s_ready) {
        return "already started";
    }
    if (!model || model_len < 16 || !arena || arena_size < 1024) {
        return "no model or no arena";
    }
    s_model = tflite::GetModel(model);
    if (!s_model || s_model->version() != TFLITE_SCHEMA_VERSION) {
        return "the model isn't a TFLite flatbuffer of this schema version";
    }
    s_arena = arena;
    s_arena_size = arena_size;
    if (!s_resolver) {
        s_resolver = new (s_resolver_mem) Resolver();
        if (!add_ops(s_resolver)) {
            return "the op resolver is full";
        }
    }
    vwe_error_t err = build_interpreter();
    if (!err) {
        err = check_contract();
    }
    if (!err && !s_frontend_up) {
        if (start_frontend()) {
            s_frontend_up = true;
        } else {
            err = "the audio frontend didn't start (memory)";
        }
    }
    if (err) {
        drop_interpreter();
        return err;
    }
    vm_stack_reset(&s_stack);
    vm_detect_init(&s_detect, window, cutoff_permille);
    s_ready = true;
    return nullptr;
}

extern "C" size_t vwe_arena_used(void)
{
    return s_ready ? s_arena_used : 0;
}

extern "C" bool vwe_feed(const int16_t *pcm, size_t n, unsigned *inferences)
{
    unsigned ran = 0;
    bool heard = false;
    while (s_ready && n > 0 && !heard) {
        size_t used = 0;
        struct FrontendOutput f = FrontendProcessSamples(&s_frontend, pcm, n, &used);
        if (used == 0 || used > n) {
            break;   /* can't happen with this frontend; never spin on it */
        }
        pcm += used;
        n -= used;
        if (f.size != VM_FEATURES || !f.values) {
            continue;   /* the window isn't full yet */
        }
        if (!vm_stack_push(&s_stack, &s_quant, f.values)) {
            continue;
        }
        TfLiteTensor *in = s_interp->input(0);
        memcpy(in->data.int8, s_stack.in, sizeof(s_stack.in));
        memcpy(s_last_in, s_stack.in, sizeof(s_last_in));
        if (s_interp->Invoke() != kTfLiteOk) {
            break;   /* leave the state as it is; the next reset rebuilds it */
        }
        ran++;
        s_inferences++;
        s_last = s_interp->output(0)->data.uint8[0];
        heard = vm_detect_push(&s_detect, s_last);
    }
    if (inferences) {
        *inferences = ran;
    }
    return heard;
}

extern "C" void vwe_reset(void)
{
    if (!s_ready) {
        return;
    }
    FrontendReset(&s_frontend);
    vm_stack_reset(&s_stack);
    vm_detect_reset(&s_detect);
    s_last = 0;
    if (build_interpreter()) {
        s_ready = false;   /* it allocated before with the same arena: can't happen */
    }
}

extern "C" void vwe_set_cutoff(int cutoff_permille)
{
    vm_detect_set_cutoff(&s_detect, cutoff_permille);
}

extern "C" const vm_detect_t *vwe_detector(void)
{
    return &s_detect;
}

extern "C" uint8_t vwe_last(void)
{
    return s_last;
}

extern "C" unsigned long vwe_inferences(void)
{
    return s_inferences;
}

extern "C" const int8_t *vwe_last_input(void)
{
    return s_last_in;
}
