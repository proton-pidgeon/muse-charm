#!/usr/bin/env bash
# Install the Vesper avatar into a Muse gadget SDK clone (idempotent).
# Usage: firmware/avatar/install.sh [SDK_DIR]   (default: ~/builds/muse-charm/scratch/muse-gadget-sdk)
# Copies muse_pixel.c to <SDK>/esp32/components/muse/avatar/ (an untracked, ignored dir in the SDK),
# where the SDK build glob and tools/muse/make_gifs.py pick it up. No tracked SDK file is touched.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk="${1:-$HOME/builds/muse-charm/scratch/muse-gadget-sdk}"
dest="$sdk/esp32/components/muse/avatar"
[ -d "$sdk/esp32/components/muse" ] || { echo "not an SDK clone: $sdk" >&2; exit 1; }
mkdir -p "$dest"
if ! cmp -s "$here/muse_pixel.c" "$dest/muse_pixel.c"; then
  cp "$here/muse_pixel.c" "$dest/muse_pixel.c"
  echo "installed: $dest/muse_pixel.c"
else
  echo "up to date: $dest/muse_pixel.c"
fi
