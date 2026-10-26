#!/usr/bin/env python3
"""Validate translated Korean decks and prepare device-sized release files."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path


HEADER = ["word", "phonetic", "meaning", "example", "example_translation"]
MAX_LINE_BYTES = 4096
MAX_MEANING_BYTES = 191
MAX_TRANSLATION_BYTES = 255
HAN_RE = re.compile(r"[\u3400-\u9fff]")
PLACEHOLDER = "(无对应词汇)"
SUBSCRIPT_DIGITS = str.maketrans("₀₁₂₃₄₅₆₇₈₉", "0123456789")


def normalize_render_text(value: str) -> str:
    return value.replace("\u200b", "").replace("\ufeff", "").translate(SUBSCRIPT_DIGITS)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_deck(path: Path) -> tuple[list[str], list[list[str]]]:
    metadata: list[str] = []
    rows: list[list[str]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        for line in handle:
            if line.startswith("#"):
                metadata.append(line.rstrip("\r\n"))
            else:
                rows.extend(csv.reader([line], delimiter="\t"))
    if not rows or rows[0] != HEADER:
        raise ValueError(f"bad header: {path}")
    return metadata, rows[1:]


def replace_metadata(metadata: list[str], key: str, value: str) -> list[str]:
    prefix = f"# {key}="
    result = [line for line in metadata if not line.startswith(prefix)]
    result.append(prefix + value)
    return result


def validate_rows(rows: list[list[str]], label: str) -> dict[str, int]:
    words: set[str] = set()
    examples = 0
    translations = 0
    for index, row in enumerate(rows, 1):
        if len(row) != 5:
            raise ValueError(f"{label} row {index}: expected 5 fields")
        row[:] = [normalize_render_text(value) for value in row]
        word, _, meaning, example, translation = row
        if not word or not meaning or word in words:
            raise ValueError(f"{label} row {index}: empty or duplicate word {word!r}")
        words.add(word)
        if PLACEHOLDER in meaning:
            raise ValueError(f"{label} row {index}: placeholder meaning")
        if len(meaning.encode("utf-8")) > MAX_MEANING_BYTES:
            raise ValueError(f"{label} row {index}: meaning too long")
        if example:
            examples += 1
            if not translation:
                raise ValueError(f"{label} row {index}: missing translation")
        elif translation:
            raise ValueError(f"{label} row {index}: translation without example")
        if translation:
            translations += 1
            if len(translation.encode("utf-8")) > MAX_TRANSLATION_BYTES:
                raise ValueError(f"{label} row {index}: translation too long")
            if not HAN_RE.search(translation):
                raise ValueError(f"{label} row {index}: translation has no Chinese character")
        line = "\t".join(row)
        if len(line.encode("utf-8")) > MAX_LINE_BYTES:
            raise ValueError(f"{label} row {index}: line too long")
    return {"cards": len(rows), "uniqueWords": len(words), "examples": examples, "translations": translations}


def write_deck(path: Path, metadata: list[str], rows: list[list[str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        for line in metadata:
            handle.write(line + "\n")
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(HEADER)
        writer.writerows(rows)


def split_by_size(rows: list[list[str]]) -> tuple[list[list[str]], list[list[str]]]:
    sizes = [len(("\t".join(row) + "\n").encode("utf-8")) for row in rows]
    target = sum(sizes) // 2
    total = 0
    split = 1
    for index, size in enumerate(sizes, 1):
        total += size
        if total >= target:
            split = index
            break
    return rows[:split], rows[split:]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    release_specs: list[tuple[str, str, list[str], list[list[str]]]] = []
    for level, name in (
        ("beginner", "韩语基础词汇（初级）"),
        ("intermediate", "韩语基础词汇（中级）"),
    ):
        metadata, rows = read_deck(args.input_dir / f"{level}-translated.tsv")
        release_specs.append((f"krdict-ko-zh-{level}", name, metadata, rows))

    advanced_metadata, advanced_rows = read_deck(args.input_dir / "advanced-translated.tsv")
    upper, lower = split_by_size(advanced_rows)
    release_specs.extend(
        [
            ("krdict-ko-zh-advanced-1", "韩语基础词汇（高级·上）", advanced_metadata, upper),
            ("krdict-ko-zh-advanced-2", "韩语基础词汇（高级·下）", advanced_metadata, lower),
        ]
    )

    report: dict[str, object] = {
        "machineTranslation": True,
        "provider": "google-translate-gtx-unofficial",
        "cardPolicy": "one written word per card; multiple official meanings; at most one example",
        "decks": {},
    }
    for deck_id, name, metadata, rows in release_specs:
        metadata = replace_metadata(metadata, "deck_id", deck_id)
        metadata = replace_metadata(metadata, "name", name)
        stats = validate_rows(rows, deck_id)
        output = args.output_dir / f"{deck_id}.tsv"
        write_deck(output, metadata, rows)
        stats.update({"name": name, "size": output.stat().st_size, "sha256": digest(output)})
        report["decks"][deck_id] = stats

    report_path = args.output_dir / "release-quality-report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
