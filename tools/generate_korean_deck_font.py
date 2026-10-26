#!/usr/bin/env python3
"""Generate a compact LVGL font containing only Hangul used by Korean decks."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

from generate_vocabulary_font import CONVERTER_VERSION, generated_codepoints


def is_hangul(char: str) -> bool:
    codepoint = ord(char)
    return (
        0x1100 <= codepoint <= 0x11FF
        or 0x3130 <= codepoint <= 0x318F
        or 0xA960 <= codepoint <= 0xA97F
        or 0xAC00 <= codepoint <= 0xD7AF
        or 0xD7B0 <= codepoint <= 0xD7FF
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--deck", type=Path, action="append", required=True)
    parser.add_argument("--font-source", type=Path, required=True)
    parser.add_argument("--font-output", type=Path, required=True)
    parser.add_argument("--font-name", required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--pnpm", default="pnpm")
    parser.add_argument("--node", default="node")
    args = parser.parse_args()

    decks = [path.resolve() for path in args.deck]
    font_source = args.font_source.resolve()
    font_output = args.font_output.resolve()
    for path in [font_source, *decks]:
        if not path.is_file():
            raise SystemExit(f"missing input: {path}")

    # Scan every field, including the Korean pronunciation column. The generic
    # vocabulary font generator intentionally skips that column because English
    # phonetics use a separate IPA font, which is not valid for Korean decks.
    required = {
        char
        for deck in decks
        for char in deck.read_text(encoding="utf-8")
        if is_hangul(char)
    }
    if not required:
        raise SystemExit("the selected decks contain no Hangul")
    symbols = "".join(sorted(required, key=ord))
    print(f"Korean deck font request: decks={len(decks)} hangul={len(required)}")

    with tempfile.TemporaryDirectory(prefix="egga-korean-font-") as temp_dir:
        temp_path = Path(temp_dir)
        generated = temp_path / font_output.name
        subprocess.run(
            [
                args.pnpm,
                "add",
                "--dir",
                str(temp_path),
                "--ignore-scripts",
                f"lv_font_conv@{CONVERTER_VERSION}",
            ],
            check=True,
        )
        converter = temp_path / "node_modules" / "lv_font_conv" / "lv_font_conv.js"
        subprocess.run(
            [
                args.node,
                str(converter),
                "--font",
                str(font_source),
                "--range",
                "0x20-0x7E",
                "--symbols",
                symbols,
                "--size",
                str(args.size),
                "--bpp",
                "1",
                "--format",
                "lvgl",
                "--lv-include",
                "lvgl.h",
                "--lv-font-name",
                args.font_name,
                "--no-compress",
                "--no-kerning",
                "--output",
                str(generated),
            ],
            check=True,
        )
        generated_text = generated.read_text(encoding="utf-8")
        available = generated_codepoints(generated_text)
        missing = sorted(ord(char) for char in required if ord(char) not in available)
        if missing:
            preview = "".join(chr(codepoint) for codepoint in missing[:20])
            raise SystemExit(
                f"generated font is missing {len(missing)} Hangul glyphs: {preview}"
            )
        font_output.parent.mkdir(parents=True, exist_ok=True)
        font_output.write_bytes(generated.read_bytes())

    print(f"generated {font_output} ({font_output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
