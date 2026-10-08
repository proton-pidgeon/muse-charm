#!/usr/bin/env bash
# Turn a pristine muse-gadget-sdk checkout (b1a3822) into the Vesper node
# firmware. Idempotent: re-running on an already-applied tree changes nothing.
#
#   firmware/apply-sdk.sh <SDK_DIR>
#
# Steps:
#   1. delete the Meta transport (firmware/sdk-patches/delete.txt)
#   2. apply the patch series (firmware/sdk-patches/*.patch, in order)
#   3. install the Vesper hatch backend (firmware/hatch/*.{c,h}: the backend,
#      the protocol core, the reply-speech helpers) into
#      <SDK>/esp32/components/muse/vesper/ (an ignored path in the SDK)
#   4. install the Vesper avatar (firmware/avatar/install.sh)
#   5. check that esp32/main/voice.c is still byte-identical to b1a3822
#
# Then: cd <SDK>/esp32 && tools/muse/board.sh build aipi
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk="${1:?usage: firmware/apply-sdk.sh <SDK_DIR>}"
sdk="$(cd "$sdk" && pwd)"
base=b1a3822
[ -d "$sdk/esp32/components/muse" ] || { echo "not a muse-gadget-sdk checkout: $sdk" >&2; exit 1; }
git -C "$sdk" rev-parse --git-dir >/dev/null 2>&1 || { echo "$sdk is not a git checkout" >&2; exit 1; }
head="$(git -C "$sdk" rev-parse HEAD)"
case "$head" in
    "$base"*) ;;
    *) echo "warning: $sdk is at ${head:0:7}, the patch set targets $base" >&2 ;;
esac

# 1. deletions
deleted=0
while IFS= read -r p; do
    case "$p" in ''|'#'*) continue ;; esac
    if [ -e "$sdk/$p" ]; then
        rm -rf "${sdk:?}/$p"
        deleted=$((deleted + 1))
    fi
done < "$here/sdk-patches/delete.txt"
echo "deleted: $deleted path(s)"

# 2. patches
for patch in "$here"/sdk-patches/*.patch; do
    name="$(basename "$patch")"
    if git -C "$sdk" apply --check "$patch" 2>/dev/null; then
        git -C "$sdk" apply "$patch"
        echo "applied: $name"
    elif git -C "$sdk" apply --reverse --check "$patch" 2>/dev/null; then
        echo "already applied: $name"
    else
        echo "error: $name neither applies nor is applied (is $sdk pristine $base?)" >&2
        git -C "$sdk" apply --check "$patch" || true
        exit 1
    fi
done

# 3. the Vesper hatch backend
dest="$sdk/esp32/components/muse/vesper"
mkdir -p "$dest"
for f in vesper_proto.c vesper_proto.h vesper_audio.c vesper_audio.h muse_chat_vesper.c muse_chat_vesper.h; do
    if ! cmp -s "$here/hatch/$f" "$dest/$f"; then
        cp "$here/hatch/$f" "$dest/$f"
        echo "installed: components/muse/vesper/$f"
    fi
done

# 4. the avatar
"$here/avatar/install.sh" "$sdk"

# 5. voice.c must not change
if ! git -C "$sdk" diff --quiet "$base" -- esp32/main/voice.c; then
    echo "error: esp32/main/voice.c differs from $base" >&2
    exit 1
fi
echo "ok: esp32/main/voice.c unchanged from $base"
