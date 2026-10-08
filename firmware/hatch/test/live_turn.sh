#!/bin/bash
# One real turn against the running node backend through the firmware's
# protocol code. Makes a harmless spoken note with `say`, reads the node token
# from ~/.config/vesper-voice/node.env at runtime (never printed), and runs
# live_turn. Usage: live_turn.sh <live_turn binary> [base-url] [prompt]
set -euo pipefail
set +x
bin="$1"
base="${2:-${VESPER_NODE_URL:-http://[::1]:8796}}"
prompt="${3:-what is two plus two}"
envf="${VESPER_NODE_ENV:-$HOME/.config/vesper-voice/node.env}"
tmp="$(mktemp -d /tmp/vesper-live-turn.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
say -o "$tmp/note.aiff" "$prompt"
afconvert -f WAVE -d LEI16@16000 -c 1 "$tmp/note.aiff" "$tmp/note.wav"
[ -r "$envf" ] || { echo "no $envf" >&2; exit 2; }
VESPER_NODE_TOKEN="$(sed -n 's/^VESPER_NODE_TOKEN=//p' "$envf" | head -1 | tr -d "\"'")"
export VESPER_NODE_TOKEN
echo "prompt: \"$prompt\""
"$bin" "$base" "$tmp/note.wav"
