#!/bin/bash
# The claim ceremony end to end on the host, through the firmware's own claim
# code (live_claim.c), against a THROWAWAY node backend started from this
# checkout's backend/:
#   - a free loopback port (default 18796; never the production :8796),
#   - a temp registry (VESPER_NODE_REGISTRY_FILE under a 700 temp dir),
#   - dummy tokens generated here, env files pointed at nothing, so neither
#     ~/.config/vesper-voice/*.env nor the real registry is ever read,
#   - only the PIDs started here are killed.
# Steps: start the backend; `live_claim claim` (start, poll); approve the code
# with `vesper-node claim <code> --room test` against the temp registry; the
# poll collects the credential into an NVS stand-in file; then `live_claim
# auth` (a "reboot") checks authenticated and refused requests.
# Usage: live_claim.sh <live_claim binary>
set -euo pipefail
set +x
export PATH=/opt/homebrew/bin:$HOME/.local/bin:$PATH
bin="$1"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
backend="$(cd "$here/../../../backend" && pwd)"
port="${VESPER_LIVE_CLAIM_PORT:-18796}"
[ "$port" != 8796 ] || { echo "refusing the production port 8796" >&2; exit 2; }
while lsof -nP -iTCP:"$port" >/dev/null 2>&1; do port=$((port + 1)); done
tmp="$(mktemp -d /tmp/vesper-live-claim.XXXXXX)"
chmod 700 "$tmp"
pid=""
cleanup() {
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT

[ -x "$backend/.venv/bin/vesper-node" ] || (cd "$backend" && uv sync --locked --extra dev >/dev/null)
gen() { python3 -c 'import secrets; print(secrets.token_urlsafe(32))'; }
VESPER_NODE_TOKEN="$(gen)"
export VESPER_NODE_TOKEN
# Everything the backend reads, set here; nothing from the real env files.
benv=(env -u VESPER_NODE_ADMIN_TOKEN -u DEEPGRAM_API_KEY -u ELEVEN_API_KEY
      VESPER_NODE_ENV_FILE=/nonexistent/node.env VESPER_VOICE_ENV_FILE=/nonexistent/env
      VESPER_NODE_REGISTRY_FILE="$tmp/registry/nodes.json"
      VESPER_NODE_TOKEN="$VESPER_NODE_TOKEN" VESPER_BRAIN_TOKEN="$(gen)" VESPER_BRAIN_URL="http://[::1]:9"
      ELEVENLABS_API_KEY=dummy-never-called VESPER_NODE_TTS=off
      VESPER_NODE_HOST=::1 VESPER_NODE_PORT="$port")

"${benv[@]}" "$backend/.venv/bin/vesper-node" serve >"$tmp/backend.log" 2>&1 &
pid=$!
echo "throwaway backend: pid $pid on [::1]:$port, registry $tmp/registry/nodes.json"
for _ in $(seq 1 100); do
    curl -sf "http://[::1]:$port/healthz" >/dev/null 2>&1 && break
    kill -0 "$pid" 2>/dev/null || { echo "backend exited:" >&2; cat "$tmp/backend.log" >&2; exit 1; }
    sleep 0.1
done
curl -sf "http://[::1]:$port/healthz" >/dev/null || { echo "backend not up" >&2; exit 1; }
base="http://[::1]:$port"

# 1. the node: claim/start, then polls (in the background while Kevin approves)
"$bin" claim "$base" "$tmp/code" "$tmp/nvs" >"$tmp/node.out" 2>&1 &
node=$!
for _ in $(seq 1 100); do [ -s "$tmp/code" ] && break; sleep 0.1; done
[ -s "$tmp/code" ] || { cat "$tmp/node.out"; echo "no claim code" >&2; exit 1; }
sleep 4   # let it poll once or twice (202) before the approval

# 2. Kevin: approve the code shown on the screen (CLI, temp registry)
(cd "$backend" && "${benv[@]}" .venv/bin/vesper-node claim "$(cat "$tmp/code")" --room test)

# 3. the node collects the credential on its next poll
wait "$node" || { cat "$tmp/node.out"; echo "claim failed" >&2; exit 1; }
cat "$tmp/node.out"
(cd "$backend" && "${benv[@]}" .venv/bin/vesper-node nodes list)

# 4. "reboot": the stored credential authenticates /turn and /audio
"$bin" auth "$base" "$tmp/nvs" | tee "$tmp/auth.out"

# 5. no secret anywhere in what the node printed or the backend logged
cred="$(cat "$tmp/nvs")"
for f in "$tmp/node.out" "$tmp/auth.out" "$tmp/backend.log"; do
    if grep -qF -- "$cred" "$f" || grep -qF -- "$VESPER_NODE_TOKEN" "$f" || grep -qE 'v(nc|cs)_[A-Za-z0-9_-]{20,}' "$f"; then
        echo "SECRET LEAKED into $(basename "$f")" >&2
        exit 1
    fi
done
echo "no credential, claim secret or token in the node output or the backend log"
echo "backend log (claim lines):"
grep -E "claim|credential|rejected" "$tmp/backend.log" | sed 's/^/  /'
echo "live claim: OK"
