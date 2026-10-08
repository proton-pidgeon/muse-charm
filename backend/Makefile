SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
# Homebrew uv first: ~/.local/bin/uv on this Mac is a stale 0.5.9 that corrupts uv.lock
export PATH := /opt/homebrew/bin:$(HOME)/.local/bin:$(PATH)

UV ?= uv
BUILD := .build
KEY_VARS := ELEVENLABS_API_KEY ELEVEN_API_KEY DEEPGRAM_API_KEY VESPER_BRAIN_TOKEN VESPER_NODE_TOKEN \
            VESPER_BRAIN_URL VESPER_PHONE_TTS_VOICE_ID VESPER_STT_PROVIDER VESPER_NODE_TTS \
            VESPER_NODE_ADMIN_TOKEN VESPER_NODE_REGISTRY_FILE
# keyless: strip every provider key and point both env files at nothing
KEYLESS := env $(foreach v,$(KEY_VARS),-u $(v)) VESPER_NODE_ENV_FILE=/nonexistent/node.env \
           VESPER_VOICE_ENV_FILE=/nonexistent/env
STUB_ARGS ?= --turns 5

.PHONY: help install lint fmt test plist-lint verify check-config serve verify-live \
        install-launchd uninstall-launchd clean

help:
	@echo "make verify          - keyless DoD: sync + ruff check + ruff format --check + pytest + plist lint"
	@echo "make verify-live     - stub client against a RUNNING backend (real STT/brain/TTS); STUB_ARGS=..."
	@echo "make check-config    - show settings by name (never values)"
	@echo "make serve           - run the backend in the foreground"
	@echo "make install-launchd - render + bootout/bootstrap com.vesper.node (run from the main checkout)"

install:
	$(UV) sync --locked --extra dev

lint:
	$(UV) run --locked ruff check src tests scripts
	$(UV) run --locked ruff format --check src tests scripts

fmt:
	$(UV) run --locked ruff check --fix src tests scripts
	$(UV) run --locked ruff format src tests scripts

test:
	$(KEYLESS) $(UV) run --locked pytest -q

plist-lint:
	plutil -lint launchd/com.vesper.node.plist
	bash scripts/install-launchd.sh --render-only $(BUILD)/launchd > /dev/null
	plutil -lint $(BUILD)/launchd/*.plist
	for s in scripts/*.sh; do bash -n "$$s"; done

verify: install lint test plist-lint
	@echo "verify: OK"

check-config:
	$(UV) run --locked vesper-node check-config

serve:
	$(UV) run --locked vesper-node serve

# Needs a running backend (make serve, or the launchd agent) and real keys in the env files.
verify-live:
	$(UV) run --locked python scripts/stub_client.py $(STUB_ARGS)

install-launchd:
	bash scripts/install-launchd.sh

uninstall-launchd:
	bash scripts/install-launchd.sh --uninstall

clean:
	rm -rf $(BUILD) .pytest_cache .ruff_cache
