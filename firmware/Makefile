# Host-side checks for the Vesper firmware pieces. No ESP-IDF needed.
#   make -C firmware test        protocol core and reply-speech unit tests (ASan + UBSan, -Wall -Wextra -Werror)
#   make -C firmware live-turn   one real turn against the running node backend (see hatch/test/live_turn.c)
CC ?= cc
BUILD ?= /tmp/muse-charm-firmware-test
WARN = -std=c11 -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

.PHONY: test live-turn clean

test: $(BUILD)/test_vesper_proto $(BUILD)/test_vesper_audio
	$(BUILD)/test_vesper_proto
	$(BUILD)/test_vesper_audio

$(BUILD)/test_vesper_proto: hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/test_vesper_proto.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_proto.c hatch/test/test_vesper_proto.c -o $@

$(BUILD)/test_vesper_audio: hatch/vesper_audio.c hatch/vesper_audio.h hatch/test/test_vesper_audio.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_audio.c hatch/test/test_vesper_audio.c -lm -o $@

live-turn: $(BUILD)/live_turn
	hatch/test/live_turn.sh $(BUILD)/live_turn

$(BUILD)/live_turn: hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/live_turn.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_proto.c hatch/test/live_turn.c -lcurl -o $@

clean:
	rm -rf $(BUILD)
