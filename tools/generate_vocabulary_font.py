#!/usr/bin/env python3
"""Extend the current Egga vocabulary font with glyphs used by verified decks."""

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
from pathlib import Path


CONVERTER_VERSION = "1.5.3"
SYMBOL_PATTERN = re.compile(r"--symbols (.*?) --size \d+", re.DOTALL)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--deck", type=Path, action="append", required=True)
    parser.add_argument("--font-source", type=Path, required=True)
    parser.add_argument("--font-output", type=Path, required=True)
    parser.add_argument("--font-name", required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--pnpm", default="pnpm")
    parser.add_argument("--node", default="node")
    return parser.parse_args()


def existing_symbols(font_output: Path) -> set[str]:
    text = font_output.read_text(encoding="utf-8")
    match = SYMBOL_PATTERN.search(text)
    if not match:
        raise SystemExit(f"cannot recover existing --symbols from {font_output}")
    return set(match.group(1))


def deck_symbols(paths: list[Path]) -> set[str]:
    result: set[str] = set()
    for path in paths:
        text = path.read_text(encoding="utf-8")
        rendered_text: list[str] = []
        for line in text.splitlines():
            if line.startswith("# name="):
                rendered_text.append(line.removeprefix("# name="))
                continue
            if not line or line.startswith("#") or line.startswith("word\t"):
                continue
            fields = line.split("\t")
            for index in (0, 2, 3, 4):
                if index < len(fields):
                    rendered_text.append(fields[index])
        for char in "".join(rendered_text):
            codepoint = ord(char)
            if codepoint > 0x7E and not char.isspace():
                result.add(char)
    return result


def generated_codepoints(source: str) -> set[int]:
    codepoints: set[int] = set()
    unicode_lists = {
        name: [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)", body)]
        for name, body in re.findall(
            r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};",
            source,
            re.DOTALL,
        )
    }
    cmap_match = re.search(
        r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};",
        source,
        re.DOTALL,
    )
    if not cmap_match:
        raise SystemExit("cannot inspect the generated font cmaps")
    entries = re.findall(r"\{(.*?)\}", cmap_match.group(1), re.DOTALL)
    for entry in entries:
        start_match = re.search(r"\.range_start = (\d+)", entry)
        length_match = re.search(r"\.range_length = (\d+)", entry)
        list_name_match = re.search(r"\.unicode_list = (unicode_list_\d+)", entry)
        if not start_match or not length_match:
            continue
        range_start = int(start_match.group(1))
        range_length = int(length_match.group(1))
        if list_name_match:
            offsets = unicode_lists.get(list_name_match.group(1))
            if offsets is None:
                raise SystemExit(f"missing {list_name_match.group(1)} data")
            codepoints.update(range_start + offset for offset in offsets)
        elif ".unicode_list = NULL" in entry:
            codepoints.update(range(range_start, range_start + range_length))
    if not codepoints:
        raise SystemExit("generated font cmap contains no codepoints")
    return codepoints


def main() -> int:
    args = parse_args()
    font_source = args.font_source.resolve()
    font_output = args.font_output.resolve()
    decks = [path.resolve() for path in args.deck]
    for path in [font_source, font_output, *decks]:
        if not path.is_file():
            raise SystemExit(f"missing input: {path}")

    previous = existing_symbols(font_output)
    required = deck_symbols(decks)
    symbols = "".join(sorted(previous | required, key=ord))
    print(
        f"font glyph request: previous={len(previous)} "
        f"deck={len(required)} union={len(symbols)} added={len((previous | required) - previous)}"
    )

    with tempfile.TemporaryDirectory(prefix="egga-vocab-font-") as temp_dir:
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
        if not converter.is_file():
            raise SystemExit(f"font converter was not installed at {converter}")
        command = [
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
        ]
        subprocess.run(command, check=True)
        generated_text = generated.read_text(encoding="utf-8")
        if args.font_name not in generated_text:
            raise SystemExit("generated font does not contain the expected LVGL symbol")
        missing = sorted(ord(char) for char in required if ord(char) not in generated_codepoints(generated_text))
        if missing:
            preview = "".join(chr(codepoint) for codepoint in missing[:20])
            raise SystemExit(
                f"generated font is missing {len(missing)} required deck glyphs: {preview}"
            )
        font_output.write_bytes(generated.read_bytes())

    print(f"generated {font_output} ({font_output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
