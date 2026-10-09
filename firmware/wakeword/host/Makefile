# Host check of the firmware's "Hey Vesper" pipeline (task 22). Needs network on the first
# run (it fetches the two pinned ESP-IDF components the firmware uses, checked by sha256) and
# the task-21 training venv + generated audio for check.py.
#
#   make -C firmware wake-host-check                 build vwe_host and run check.py
#   make -C firmware/wakeword/host vwe_host          just the binary
#
# vwe_host links the firmware's own hatch/vesper_wakeword_engine.cc and hatch/vesper_mww.c with
# TFLite Micro (espressif/esp-tflite-micro 1.4.1, reference kernels: esp-nn's are Xtensa-only)
# and the microfrontend (esphome/esp-micro-speech-features 1.2.3), the versions patch 0008 pins.
CXX ?= c++
CC ?= cc
BUILD ?= /tmp/muse-charm-wakeword-host
HATCH := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../hatch)
HERE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REG := https://components-file.espressif.com/components
TFLM_ZIP := espressif/esp-tflite-micro/1.4.1/espressif__esp-tflite-micro-v1.4.1.zip
TFLM_SHA := e62d1c6dfbe4801ef3627e3e87a314196f6a81c23a1ecf4fb8272cdcd097a497
MSF_ZIP := esphome/esp-micro-speech-features/1.2.3/esphome__esp-micro-speech-features-v1.2.3.zip
MSF_SHA := 052a8f32e7a82f49593bfdc067638a8cee6542443726d1a7e5b01b3bd9d67eac
TFLM := $(BUILD)/esp-tflite-micro
MSF := $(BUILD)/esp-micro-speech-features
WW_ROOT ?= $(HOME)/builds/muse-charm/scratch/wakeword-21
PY ?= $(WW_ROOT)/venv-mww/bin/python

DEFS := -DTF_LITE_STATIC_MEMORY -DTF_LITE_DISABLE_X86_NEON -DNDEBUG
INC := -I$(TFLM) -I$(TFLM)/third_party/gemmlowp -I$(TFLM)/third_party/flatbuffers/include \
       -I$(TFLM)/third_party/ruy -I$(TFLM)/third_party/kissfft -I$(MSF)/include -I$(MSF)/src
CXXFLAGS := -std=gnu++17 -O2 -fno-rtti -fno-exceptions -w $(DEFS) $(INC)
CFLAGS := -std=gnu11 -O2 -w $(DEFS) $(INC)

.PHONY: check vwe_host clean
check: $(BUILD)/vwe_host
	WW_ROOT=$(WW_ROOT) $(PY) $(HERE)/check.py $(BUILD)/vwe_host $(HERE)/../hey-vesper.tflite

vwe_host: $(BUILD)/vwe_host

$(BUILD)/%.ok:
	@mkdir -p $(BUILD)
	curl -sSfL -o $(BUILD)/$*.zip $(REG)/$(if $(filter tflm,$*),$(TFLM_ZIP),$(MSF_ZIP))
	echo "$(if $(filter tflm,$*),$(TFLM_SHA),$(MSF_SHA))  $(BUILD)/$*.zip" | shasum -a 256 -c -
	rm -rf $(BUILD)/$*.dir && mkdir -p $(BUILD)/$*.dir && cd $(BUILD)/$*.dir && unzip -q ../$*.zip
	rm -rf $(if $(filter tflm,$*),$(TFLM),$(MSF)) && mv $(BUILD)/$*.dir $(if $(filter tflm,$*),$(TFLM),$(MSF))
	touch $@

# The esp-tflite-micro CMakeLists' source list, minus the ESP-only pieces (esp/micro_time.cc,
# esp_nn kernels, the signal library the model doesn't use), plus the reference kernels.
$(BUILD)/srcs.mk: $(BUILD)/tflm.ok $(BUILD)/msf.ok
	cd $(TFLM) && { \
	  printf 'TFLM_SRCS :='; \
	  for f in debug_log flatbuffer_utils memory_helpers micro_allocation_info micro_allocator micro_context \
	           micro_interpreter_context micro_interpreter_graph micro_interpreter micro_log micro_op_resolver \
	           micro_profiler micro_resource_variable micro_time micro_utils recording_micro_allocator system_setup; do \
	    printf ' tensorflow/lite/micro/%s.cc' $$f; done; \
	  ls tensorflow/lite/micro/tflite_bridge/*.cc tensorflow/lite/micro/kernels/*.cc | sed 's/^/ /' | tr -d '\n'; \
	  printf ' %s' tensorflow/lite/kernels/kernel_util.cc \
	    tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc \
	    tensorflow/lite/micro/memory_planner/linear_memory_planner.cc \
	    tensorflow/lite/micro/arena_allocator/non_persistent_arena_buffer_allocator.cc \
	    tensorflow/lite/micro/arena_allocator/persistent_arena_buffer_allocator.cc \
	    tensorflow/lite/micro/arena_allocator/recording_single_arena_buffer_allocator.cc \
	    tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.cc \
	    tensorflow/lite/core/c/common.cc tensorflow/lite/core/api/flatbuffer_conversions.cc \
	    tensorflow/lite/core/api/tensor_utils.cc tensorflow/lite/kernels/internal/common.cc \
	    tensorflow/lite/kernels/internal/quantization_util.cc \
	    tensorflow/lite/kernels/internal/portable_tensor_utils.cc tensorflow/lite/kernels/internal/tensor_utils.cc \
	    tensorflow/lite/kernels/internal/tensor_ctypes.cc \
	    tensorflow/lite/kernels/internal/reference/portable_tensor_utils.cc \
	    tensorflow/compiler/mlir/lite/core/api/error_reporter.cc tensorflow/compiler/mlir/lite/schema/schema_utils.cc; \
	  echo; } > $@
	cd $(MSF) && { printf 'MSF_SRCS :='; ls src/*.c | sed 's/^/ /' | tr -d '\n'; echo; } >> $@

-include $(BUILD)/srcs.mk
TFLM_OBJS := $(patsubst %.cc,$(BUILD)/obj/tflm/%.o,$(TFLM_SRCS))
MSF_OBJS := $(patsubst %.c,$(BUILD)/obj/msf/%.o,$(MSF_SRCS))

$(BUILD)/obj/tflm/%.o: $(TFLM)/%.cc $(BUILD)/srcs.mk
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/msf/%.o: $(MSF)/%.c $(BUILD)/srcs.mk
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libtflm.a: $(TFLM_OBJS) $(MSF_OBJS)
	rm -f $@ && ar rcs $@ $^

# The firmware's own files: warnings are errors here, as in the firmware build.
$(BUILD)/vwe_host: $(BUILD)/libtflm.a $(HATCH)/vesper_wakeword_engine.cc $(HATCH)/vesper_wakeword_engine.h \
                   $(HATCH)/vesper_mww.c $(HATCH)/vesper_mww.h $(HERE)/vwe_host.cc
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -iquote $(HATCH) -c $(HATCH)/vesper_mww.c -o $(BUILD)/vesper_mww.o
	$(CXX) -std=gnu++17 -O2 -fno-rtti -fno-exceptions -Wall -Wextra -Werror $(DEFS) -iquote $(HATCH) \
	    $(patsubst -I%,-isystem %,$(INC)) \
	    $(HATCH)/vesper_wakeword_engine.cc $(HERE)/vwe_host.cc $(BUILD)/vesper_mww.o $(BUILD)/libtflm.a -lm -o $@

clean:
	rm -rf $(BUILD)
