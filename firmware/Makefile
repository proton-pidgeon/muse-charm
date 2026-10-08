# Host-side checks for the Vesper firmware pieces. No ESP-IDF needed.
#   make -C firmware test        protocol core, claim flow, reply-speech and OTA unit tests (ASan + UBSan,
#                                -Wall -Wextra -Werror), plus the log-hygiene check (no secret reaches a log)
#   make -C firmware live-turn   one real turn against the running node backend (see hatch/test/live_turn.c)
#   make -C firmware live-claim  the claim ceremony against a throwaway backend (see hatch/test/live_claim.sh)
#   make -C firmware live-ota    the update check + download against a throwaway backend (hatch/test/live_ota.sh;
#                                OTA_IMAGE=<build>/muse-gadget.bin publishes a real build, else a synthetic image)
CC ?= cc
BUILD ?= /tmp/muse-charm-firmware-test
WARN = -std=c11 -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

.PHONY: test log-hygiene live-turn live-claim live-ota clean

test: $(BUILD)/test_vesper_proto $(BUILD)/test_vesper_claim $(BUILD)/test_vesper_audio $(BUILD)/test_vesper_ota log-hygiene
	$(BUILD)/test_vesper_proto
	$(BUILD)/test_vesper_claim
	$(BUILD)/test_vesper_audio
	$(BUILD)/test_vesper_ota

log-hygiene:
	python3 hatch/test/check_log_hygiene.py hatch sdk-patches

$(BUILD)/test_vesper_proto: hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/test_vesper_proto.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_proto.c hatch/test/test_vesper_proto.c -o $@

$(BUILD)/test_vesper_claim: hatch/vesper_claim.c hatch/vesper_claim.h hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/test_vesper_claim.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_claim.c hatch/vesper_proto.c hatch/test/test_vesper_claim.c -o $@

$(BUILD)/test_vesper_audio: hatch/vesper_audio.c hatch/vesper_audio.h hatch/test/test_vesper_audio.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_audio.c hatch/test/test_vesper_audio.c -lm -o $@

$(BUILD)/test_vesper_ota: hatch/vesper_ota.c hatch/vesper_ota.h hatch/vesper_claim.c hatch/vesper_claim.h hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/test_vesper_ota.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_ota.c hatch/vesper_claim.c hatch/vesper_proto.c hatch/test/test_vesper_ota.c -o $@

live-turn: $(BUILD)/live_turn
	hatch/test/live_turn.sh $(BUILD)/live_turn

$(BUILD)/live_turn: hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/live_turn.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_proto.c hatch/test/live_turn.c -lcurl -o $@

live-claim: $(BUILD)/live_claim
	hatch/test/live_claim.sh $(BUILD)/live_claim

$(BUILD)/live_claim: hatch/vesper_claim.c hatch/vesper_claim.h hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/live_claim.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_claim.c hatch/vesper_proto.c hatch/test/live_claim.c -lcurl -o $@

live-ota: $(BUILD)/live_ota
	hatch/test/live_ota.sh $(BUILD)/live_ota $(OTA_IMAGE)

$(BUILD)/live_ota: hatch/vesper_ota.c hatch/vesper_ota.h hatch/vesper_claim.c hatch/vesper_claim.h hatch/vesper_proto.c hatch/vesper_proto.h hatch/test/live_ota.c
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 -Ihatch hatch/vesper_ota.c hatch/vesper_claim.c hatch/vesper_proto.c hatch/test/live_ota.c -lcurl -o $@

clean:
	rm -rf $(BUILD)
