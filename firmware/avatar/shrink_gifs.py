#!/usr/bin/env python3
"""Shrink the 320 px make_gifs.py output to 192 px (3x the 64 px grid) so the committed previews stay small.

    python3 shrink_gifs.py SRC_DIR DST_DIR
"""
import os
import sys

from PIL import Image, ImageSequence

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
for name in sorted(os.listdir(src)):
    if not name.endswith(".gif"):
        continue
    im = Image.open(os.path.join(src, name))
    frames = [f.convert("RGB").resize((192, 192), Image.NEAREST) for f in ImageSequence.Iterator(im)]
    frames = [f.quantize(colors=64, dither=Image.Dither.NONE) for f in frames]
    frames[0].save(os.path.join(dst, name), save_all=True, append_images=frames[1:], duration=40, loop=0, optimize=True)
    print(name, os.path.getsize(os.path.join(dst, name)) // 1024, "KiB")
