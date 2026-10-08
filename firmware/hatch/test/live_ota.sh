#!/bin/bash
# The firmware update path end to end on the host, through the firmware's own
# update code (live_ota.c), against a THROWAWAY node backend started from this
# checkout's backend/ (same isolation as live_claim.sh):
#   - a free loopback port (default 18896; never the production :8796),
#   - a temp registry and a temp firmware store under a 700 temp dir,
#   - dummy tokens generated here, env files pointed at nothing,
#   - only the PIDs started here are killed.
# Steps: register a test node with a credential in the temp registry; publish
# an image with `vesper-node firmware publish`; then, as the node:
#   running older  -> "newer": download, exact size, SHA-256 = the manifest's
#   running equal  -> "up_to_date";  running newer -> "older" (no downgrade)
#   wrong credential -> "refused";  withdrawn -> "up_to_date" (204)
# and grep everything for leaked secrets.
# Usage: live_ota.sh <live_ota binary> [image.bin]
#   image.bin: a real build (build-muse-aipi/muse-gadget.bin); default: a
#   synthetic app image (version 1.0.0) made here.
set -euo pipefail
set +x
export PATH=/opt/homebrew/bin:$HOME/.local/bin:$PATH
bin="$1"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
backend="$(cd "$here/../../../backend" && pwd)"
port="${VESPER_LIVE_OTA_PORT:-18896}"
[ "$port" != 8796 ] || { echo "refusing the production port 8796" >&2; exit 2; }
while lsof -nP -iTCP:"$port" >/dev/null 2>&1; do port=$((port + 1)); done
tmp="$(mktemp -d /tmp/vesper-live-ota.XXXXXX)"
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
py="$backend/.venv/bin/python"
gen() { python3 -c 'import secrets; print(secrets.token_urlsafe(32))'; }
VESPER_NODE_TOKEN="$(gen)"
export VESPER_NODE_TOKEN
benv=(env -u VESPER_NODE_ADMIN_TOKEN -u DEEPGRAM_API_KEY -u ELEVEN_API_KEY
      VESPER_NODE_ENV_FILE=/nonexistent/node.env VESPER_VOICE_ENV_FILE=/nonexistent/env
      VESPER_NODE_REGISTRY_FILE="$tmp/registry/nodes.json"
      VESPER_NODE_FIRMWARE_DIR="$tmp/firmware"
      VESPER_NODE_TOKEN="$VESPER_NODE_TOKEN" VESPER_BRAIN_TOKEN="$(gen)" VESPER_BRAIN_URL="http://[::1]:9"
      ELEVENLABS_API_KEY=dummy-never-called VESPER_NODE_TTS=off
      VESPER_NODE_HOST=::1 VESPER_NODE_PORT="$port")

# The test node and its credential (as a completed claim would leave them).
umask 077
echo "vnc_$(gen)" > "$tmp/cred"
"${benv[@]}" "$py" - "$tmp/registry/nodes.json" "$tmp/cred" <<'EOF'
import sys
from vesper_node.registry import Registry
Registry(sys.argv[1]).add_node("homelink-otatest", "test", credential=open(sys.argv[2]).read().strip())
EOF
echo "vnc_$(gen)" > "$tmp/wrong"

# The image to publish.
image="${2:-}"
if [ -z "$image" ]; then
    image="$tmp/synthetic.bin"
    "$py" - "$image" <<'EOF'
import struct, sys
h = bytes([0xE9, 1, 2, 0x2F]) + struct.pack("<I", 0x40370000) + bytes([0xEE, 0, 0, 0])
h += struct.pack("<H", 9) + bytes(9) + bytes([1])
d = struct.pack("<IIII", 0xABCD5432, 0, 0, 0) + b"1.0.0".ljust(32, b"\0") + b"muse-gadget".ljust(32, b"\0")
body = h + struct.pack("<II", 0x3C000020, 256) + d
open(sys.argv[1], "wb").write(body + bytes(65536 - len(body)))
EOF
fi
(cd "$backend" && "${benv[@]}" .venv/bin/vesper-node firmware publish "$image")
version="$("$py" -c 'import sys; d=open(sys.argv[1],"rb").read(); print(d[48:80].split(b"\0")[0].decode())' "$image")"
IFS=. read -r maj min pat <<<"$version"
older="$maj.$min.$((pat > 0 ? pat - 1 : 0))"
[ "$pat" -gt 0 ] || older="$((maj > 0 ? maj - 1 : 0)).99.99"
newer="$maj.$min.$((pat + 1))"

"${benv[@]}" "$backend/.venv/bin/vesper-node" serve >"$tmp/backend.log" 2>&1 &
pid=$!
echo "throwaway backend: pid $pid on [::1]:$port"
for _ in $(seq 1 100); do
    curl -sf "http://[::1]:$port/healthz" >/dev/null 2>&1 && break
    kill -0 "$pid" 2>/dev/null || { echo "backend exited:" >&2; cat "$tmp/backend.log" >&2; exit 1; }
    sleep 0.1
done
base="http://[::1]:$port"

expect() {   # expect <verdict> <cred-file> <running>
    local out
    out="$("$bin" "$base" "$2" "$3" "$tmp/got.bin" || true)"
    echo "$out" | sed "s/^/  [running $3] /" | tee -a "$tmp/node.out"
    [ "$(echo "$out" | head -1)" = "$1" ] || { echo "expected $1" >&2; exit 1; }
}

echo "published $version; the node checks:"
expect newer "$tmp/cred" "$older"
want="$(shasum -a 256 "$image" | cut -d' ' -f1)"
got="$(shasum -a 256 "$tmp/got.bin" | cut -d' ' -f1)"
[ "$want" = "$got" ] || { echo "downloaded image SHA-256 differs" >&2; exit 1; }
echo "  downloaded image SHA-256 = the published file's ($want)"
expect up_to_date "$tmp/cred" "$version"
expect older "$tmp/cred" "$newer"
expect refused "$tmp/wrong" "$older"
(cd "$backend" && "${benv[@]}" .venv/bin/vesper-node firmware withdraw)
expect up_to_date "$tmp/cred" "$older"

cred="$(cat "$tmp/cred")"
for f in "$tmp/node.out" "$tmp/backend.log"; do
    if grep -qF -- "$cred" "$f" || grep -qF -- "$VESPER_NODE_TOKEN" "$f" || grep -qE 'v(nc|cs)_[A-Za-z0-9_-]{20,}' "$f"; then
        echo "SECRET LEAKED into $(basename "$f")" >&2
        exit 1
    fi
done
echo "no credential or token in the node output or the backend log"
echo "backend log (firmware lines):"
grep -E "firmware|rejected" "$tmp/backend.log" | sed 's/^/  /'
echo "live ota: OK"
