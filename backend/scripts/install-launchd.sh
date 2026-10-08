#!/bin/bash
# Render launchd/com.vesper.node.plist and (re)load it as a user LaunchAgent.
#
#   scripts/install-launchd.sh                      # install + load com.vesper.node (port 8796)
#   scripts/install-launchd.sh --label L --port P   # e.g. a throwaway self-test agent
#   scripts/install-launchd.sh --uninstall [--label L]
#   scripts/install-launchd.sh --render-only DIR    # render + plutil -lint into DIR, load nothing
#
# Reload is bootout + bootstrap (NOT `launchctl kickstart -k`, which does not re-read the plist).
# Keys are never rendered into the plist; the service reads the mode-600 env files itself.
# Run from the MAIN checkout: the plist points at this directory's .venv.
set -euo pipefail

BACKEND_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="com.vesper.node"
PORT="8796"
RENDER_ONLY=""
UNINSTALL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --render-only) RENDER_ONLY="${2:?--render-only needs a directory}"; shift 2 ;;
    --label) LABEL="${2:?--label needs a value}"; shift 2 ;;
    --port) PORT="${2:?--port needs a value}"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done

if [[ ! "$LABEL" =~ ^com\.vesper\.node(\.[a-z0-9-]+)?$ ]]; then
  echo "install-launchd: label must be com.vesper.node[.suffix]" >&2; exit 64
fi
if [[ ! "$PORT" =~ ^[0-9]{2,5}$ ]]; then
  echo "install-launchd: --port must be a number" >&2; exit 64
fi

DOMAIN="gui/$(id -u)"
AGENTS_DIR="$HOME/Library/LaunchAgents"

if [[ -n "$UNINSTALL" ]]; then
  launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
  rm -f "$AGENTS_DIR/$LABEL.plist"
  echo "unloaded + removed $LABEL"
  exit 0
fi

render() {  # render <dest-dir>
  local out="$1/$LABEL.plist"
  sed -e "s#__LABEL__#${LABEL}#g" -e "s#__BACKEND_DIR__#${BACKEND_DIR}#g" \
      -e "s#__HOME__#${HOME}#g" -e "s#__PORT__#${PORT}#g" \
      "$BACKEND_DIR/launchd/com.vesper.node.plist" > "$out"
  if grep -q '__[A-Z_]*__' "$out"; then
    echo "install-launchd: unrendered placeholder left in $out" >&2
    exit 1
  fi
  plutil -lint "$out" >/dev/null
  echo "$out"
}

if [[ -n "$RENDER_ONLY" ]]; then
  mkdir -p "$RENDER_ONLY"
  render "$RENDER_ONLY"
  exit 0
fi

for f in "${VESPER_NODE_ENV_FILE:-$HOME/.config/vesper-voice/node.env}" \
         "${VESPER_VOICE_ENV_FILE:-$HOME/.config/vesper-voice/env}"; do
  if [[ ! -f "$f" ]]; then
    echo "install-launchd: $f does not exist; the service would exit 78" >&2
    exit 1
  fi
  mode="$(stat -f '%Lp' "$f")"
  if [[ "$mode" != "600" && "$mode" != "400" ]]; then
    echo "install-launchd: $f is mode $mode; fix with: chmod 600 $f" >&2
    exit 1
  fi
done

if [[ ! -x "$BACKEND_DIR/.venv/bin/vesper-node" ]]; then
  echo "install-launchd: $BACKEND_DIR/.venv/bin/vesper-node missing; run: make -C $BACKEND_DIR install" >&2
  exit 1
fi

if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1 \
   && ! launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
  echo "install-launchd: port $PORT is already in use by something other than $LABEL" >&2
  exit 1
fi

mkdir -p "$AGENTS_DIR" "$HOME/Library/Logs/vesper-node"
plist="$(render "$AGENTS_DIR")"
launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
launchctl bootstrap "$DOMAIN" "$plist"
launchctl enable "$DOMAIN/$LABEL"
echo "loaded $LABEL ($plist) on port $PORT"
echo "status: launchctl print $DOMAIN/$LABEL | grep -E 'state|pid|last exit'"
echo "health: curl -s http://[::1]:$PORT/healthz"
