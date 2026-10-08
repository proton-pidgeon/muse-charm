#!/usr/bin/env bash
# Turn a pristine muse-gadget-sdk checkout (b1a3822) into the Vesper node
# firmware. Idempotent: re-running on an already-applied tree changes nothing.
#
#   firmware/apply-sdk.sh <SDK_DIR>
#
# Steps:
#   1. delete the Meta transport (firmware/sdk-patches/delete.txt)
#   2. apply the patch series (firmware/sdk-patches/*.patch, in order; a tree
#      that already has the whole series is detected as such)
#   3. install the Vesper hatch backend (firmware/hatch/*.{c,h}: the backend,
#      the protocol core, the reply-speech helpers, the claim flow, the node
#      credential store, the BLE host, the update check and the announcement
#      poll; and VERSION, the firmware version the build stamps into the
#      image) into
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
# A later patch may edit lines an earlier one added (0004 edits 0002's app.c),
# so "is patch N applied?" can't be asked of patch N alone once N+1 is on top.
# Ask it of a prefix of the series instead: on a scratch copy of the files the
# first k patches touch, reverse them last to first. The longest prefix that
# reverses cleanly is what the tree already has (all of it: fully patched;
# none: pristine); the rest of the series is applied on top, in order. So a
# tree patched by an older checkout of this repo picks up just the new patches.
patches=()
while IFS= read -r p; do patches+=("$p"); done < <(ls "$here"/sdk-patches/*.patch | sort)
prefix_applied() {
    local k="$1" scratch rc=0 f i
    [ "$k" -gt 0 ] || return 0
    scratch="$(mktemp -d)"
    for f in $(sed -n 's|^+++ b/||p' "${patches[@]:0:$k}" | sort -u); do
        [ -e "$sdk/$f" ] || { rm -rf "$scratch"; return 1; }
        mkdir -p "$scratch/$(dirname "$f")"
        cp "$sdk/$f" "$scratch/$f"
    done
    for ((i = k - 1; i >= 0; i--)); do
        (cd "$scratch" && git apply --reverse "${patches[$i]}" 2>/dev/null) || { rc=1; break; }
    done
    rm -rf "$scratch"
    return $rc
}
have=${#patches[@]}
while [ "$have" -gt 0 ] && ! prefix_applied "$have"; do have=$((have - 1)); done
for ((i = 0; i < ${#patches[@]}; i++)); do
    patch="${patches[$i]}"
    name="$(basename "$patch")"
    if [ "$i" -lt "$have" ]; then
        echo "already applied: $name"
    elif git -C "$sdk" apply --check "$patch" 2>/dev/null; then
        git -C "$sdk" apply "$patch"
        echo "applied: $name"
    else
        echo "error: $name doesn't apply on top of the ${i} before it (is $sdk pristine $base?)" >&2
        git -C "$sdk" apply --check "$patch" || true
        exit 1
    fi
done

# 3. the Vesper hatch backend
dest="$sdk/esp32/components/muse/vesper"
mkdir -p "$dest"
for f in vesper_proto.c vesper_proto.h vesper_audio.c vesper_audio.h muse_chat_vesper.c muse_chat_vesper.h \
         vesper_claim.c vesper_claim.h vesper_cred.c vesper_cred.h vesper_ble.c vesper_ble.h \
         vesper_ota.c vesper_ota.h vesper_announce.c vesper_announce.h VERSION; do
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
