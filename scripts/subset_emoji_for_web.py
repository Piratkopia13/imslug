#!/usr/bin/env python3
"""Subset the fetched Noto Color Emoji for the web demo (COLR v1 only).

scripts/build_web.* runs this on tests/fonts/emoji/NotoColorEmoji-Regular.ttf
and embeds the result into the browser build - the page has no filesystem and
the full font is ~25 MB. imslug renders COLR/CPAL layers only, so the subset
keeps the paint graphs and drops the SVG/bitmap tables the renderer never
reads (dropping SVG also avoids fonttools' lxml dependency).

The codepoint list is exactly the text the emoji scene draws
(example/common/example_app.cpp, `SceneEmoji`); keep them in sync. The output
is deterministic for a given fonttools version and input bytes (it is a build
artifact, not committed).

Requires fonttools (`pip install fonttools`).

usage: python scripts/subset_emoji_for_web.py --input <font.ttf> --output <out.ttf>
"""

import argparse
import os
import sys

# The emoji scene's sample set (example/common/example_app.cpp, SceneEmoji).
# The last two are the non-BMP imslug mascot: the web build defines
# IMGUI_USE_WCHAR32 (IMSLUG_WCHAR32, on by default), so 32-bit codepoints are
# addressable; keep this list and the scene text in sync.
EMOJI_CODEPOINTS = [
    0x2600,  # ☀  black sun
    0x26C5,  # ⛅ sun behind cloud
    0x2764,  # ❤  heavy heart (also the zoom glyph)
    0x2B50,  # ⭐ star
    0x26A1,  # ⚡ high voltage
    0x2728,  # ✨ sparkles
    0x26C4,  # ⛄ snowman
    0x2614,  # ☔ umbrella with rain
    0x26BD,  # ⚽ soccer ball
    0x2702,  # ✂  black scissors
    0x264B,  # ♋  cancer zodiac
    0x2693,  # ⚓ anchor
    0x1F40C,  # 🐌 snail (mascot)
    0x1F41A,  # 🐚 spiral shell (mascot)
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Subset Noto Color Emoji for the imslug web demo."
    )
    parser.add_argument(
        "--input", required=True, help="path to NotoColorEmoji-Regular.ttf"
    )
    parser.add_argument(
        "--output", required=True, help="path of the subset TTF to write"
    )
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        print(
            "subset_emoji_for_web: fonttools is required (pip install fonttools)",
            file=sys.stderr,
        )
        return 1

    try:
        font = TTFont(args.input)
    except OSError as exc:
        print("subset_emoji_for_web: %s" % exc, file=sys.stderr)
        return 1

    options = subset.Options()
    options.hinting = False
    options.drop_tables += ["SVG"]  # imslug renders COLR/CPAL only

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=EMOJI_CODEPOINTS)
    subsetter.subset(font)

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    font.save(args.output)

    print(
        "subset_emoji_for_web: wrote %s (%d glyphs, %d bytes)"
        % (args.output, font["maxp"].numGlyphs, os.path.getsize(args.output))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
