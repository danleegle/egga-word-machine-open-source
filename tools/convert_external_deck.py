#!/usr/bin/env python3
"""Convert external vocabulary JSON/JSONL decks into Egga asset TSV decks."""

from __future__ import annotations

import argparse
import html
import json
import re
from pathlib import Path
from typing import Iterable


TAG_RE = re.compile(r"<[^>]+>")
WHITESPACE_RE = re.compile(r"\s+")


def strip_tags(value: str) -> str:
    return html.unescape(TAG_RE.sub("", value))


def normalize_field(value: str | None) -> str:
    cleaned = strip_tags(str(value or "")).replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return WHITESPACE_RE.sub(" ", cleaned).strip()


def first_non_empty(values: Iterable[str | None]) -> str:
    for value in values:
        text = normalize_field(value)
        if text:
            return text
    return ""


def format_pos(value: str | None) -> str:
    text = normalize_field(value).rstrip(".")
    if not text:
        return ""
    if re.fullmatch(r"[A-Za-z]+", text):
        return f"{text.lower()}."
    return text


def build_meaning_text(pos: str | None, translation: str | None) -> str:
    meaning = normalize_field(translation)
    if not meaning:
        return ""
    normalized_pos = format_pos(pos)
    return f"{normalized_pos} {meaning}" if normalized_pos else meaning


def load_records(path: Path) -> list[dict]:
    text = path.read_text(encoding="utf-8", errors="replace")
    if not text.strip():
        return []

    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        records: list[dict] = []
        for lineno, line in enumerate(text.splitlines(), start=1):
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"Invalid JSONL at line {lineno}: {exc}") from exc
            if isinstance(item, dict):
                records.append(item)
        return records

    if isinstance(parsed, list):
        return [item for item in parsed if isinstance(item, dict)]

    if isinstance(parsed, dict):
        for key in ("rows", "words", "data", "items", "list", "entries"):
            value = parsed.get(key)
            if isinstance(value, list):
                return [item for item in value if isinstance(item, dict)]
        return [parsed]

    raise ValueError(f"Unsupported deck root type: {type(parsed).__name__}")


def collect_meaning_items(items: list[dict] | None, limit: int = 3) -> str:
    if not isinstance(items, list):
        return ""

    meanings: list[str] = []
    for item in items:
        if not isinstance(item, dict):
            continue
        text = first_non_empty(
            (
                item.get("tranCn"),
                item.get("tran"),
                item.get("translation"),
                item.get("meaning"),
                item.get("definition"),
                item.get("gloss"),
            )
        )
        formatted = build_meaning_text(item.get("pos") or item.get("partOfSpeech"), text) or text
        if formatted and formatted not in meanings:
            meanings.append(formatted)
        if len(meanings) >= limit:
            break

    return " | ".join(meanings)


def collect_meaning_from_node(node: dict | None) -> str:
    if not isinstance(node, dict):
        return ""

    for collection in (
        node.get("trans"),
        node.get("translations"),
        node.get("definitions"),
        (node.get("syno") or {}).get("synos"),
    ):
        text = collect_meaning_items(collection)
        if text:
            return text

    text = first_non_empty(
        (
            node.get("meaning"),
            node.get("translation"),
            node.get("definition"),
            node.get("tranCn"),
            node.get("tran"),
            node.get("gloss"),
        )
    )
    return build_meaning_text(node.get("pos") or node.get("partOfSpeech"), text) or text


def collect_phonetic_from_node(node: dict | None) -> str:
    if not isinstance(node, dict):
        return ""
    return first_non_empty(
        (
            node.get("phonetic"),
            node.get("phone"),
            node.get("ukphone"),
            node.get("usphone"),
        )
    )


def first_example_from_list(items: list[dict] | None) -> tuple[str, str] | None:
    if not isinstance(items, list):
        return None

    fallback: tuple[str, str] | None = None
    for item in items:
        if not isinstance(item, dict):
            continue
        example = first_non_empty(
            (
                item.get("sContent"),
                item.get("content"),
                item.get("example"),
                item.get("sentence"),
                item.get("en"),
            )
        )
        example_translation = first_non_empty(
            (
                item.get("sCn"),
                item.get("translation"),
                item.get("exampleTranslation"),
                item.get("cn"),
                item.get("zh"),
            )
        )
        if not example:
            continue
        if example_translation:
            return example, example_translation
        if fallback is None:
            fallback = (example, "")

    return fallback


def collect_example_pair(nodes: Iterable[dict | None]) -> tuple[str, str]:
    for node in nodes:
        if not isinstance(node, dict):
            continue
        for collection in (
            ((node.get("sentence") or {}).get("sentences")),
            node.get("examples"),
            node.get("sentences"),
        ):
            pair = first_example_from_list(collection)
            if pair:
                return pair

    for node in nodes:
        if not isinstance(node, dict):
            continue
        pair = first_example_from_list(((node.get("realExamSentence") or {}).get("sentences")))
        if pair:
            return pair

    return "", ""


def extract_card(record: dict) -> dict[str, str] | None:
    content = record.get("content") or {}
    word_node = content.get("word") or record.get("word") or {}
    word_content = word_node.get("content") or {}
    candidates = [word_content, word_node, content, record]

    word = first_non_empty(
        (
            record.get("word"),
            record.get("headWord"),
            record.get("wordHead"),
            record.get("term"),
            word_node.get("word"),
            word_node.get("wordHead"),
        )
    )
    if not word:
        return None

    phonetic = next((text for text in (collect_phonetic_from_node(node) for node in candidates) if text), "")
    meaning = next((text for text in (collect_meaning_from_node(node) for node in candidates) if text), "")
    if not meaning:
        return None

    example, example_translation = collect_example_pair(candidates)
    return {
        "word": word,
        "phonetic": phonetic,
        "meaning": meaning,
        "example": example,
        "example_translation": example_translation,
    }


def slugify(value: str) -> str:
    text = value.strip().lower()
    text = re.sub(r"[^a-z0-9]+", "-", text)
    return text.strip("-") or "assets-current"


def write_tsv(
    records: list[dict[str, str]],
    output: Path,
    *,
    deck_id: str,
    name: str,
    language: str,
    version: int,
    output_format: str,
    include_metadata: bool,
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as handle:
        if include_metadata:
            handle.write(f"# deck_id={deck_id}\n")
            handle.write(f"# name={name}\n")
            handle.write(f"# language={language}\n")
            handle.write(f"# version={version}\n")

        for row in records:
            if output_format == "compact":
                handle.write(f"{row['word']}\t{row['meaning']}\n")
            elif output_format == "compact-phonetic":
                handle.write(f"{row['word']}\t{row['phonetic']}\t{row['meaning']}\n")
            else:
                handle.write(
                    "\t".join(
                        [
                            row["word"],
                            row["phonetic"],
                            row["meaning"],
                            row["example"],
                            row["example_translation"],
                        ]
                    )
                    + "\n"
                )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert external vocabulary JSON/JSONL decks into Egga asset TSV decks."
    )
    parser.add_argument("--source", type=Path, required=True, help="Path to the source JSON or JSONL deck.")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("assets/decks/current.tsv"),
        help="Output TSV path. Defaults to assets/decks/current.tsv.",
    )
    parser.add_argument("--deck-id", default=None, help="Optional deck id metadata.")
    parser.add_argument("--name", default=None, help="Optional deck name metadata.")
    parser.add_argument("--language", default="en-zh", help="Deck language metadata.")
    parser.add_argument("--version", type=int, default=1, help="Deck version metadata.")
    parser.add_argument(
        "--format",
        choices=("detailed", "compact", "compact-phonetic"),
        default="detailed",
        help="Output field set. Default is detailed: word, phonetic, meaning, example, example_translation.",
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="Write the smallest 2-column format: word<TAB>meaning.",
    )
    parser.add_argument(
        "--include-phonetic",
        action="store_true",
        help="Write the 3-column compact format: word<TAB>phonetic<TAB>meaning.",
    )
    parser.add_argument(
        "--omit-metadata",
        action="store_true",
        help="Do not write metadata header lines.",
    )
    args = parser.parse_args()

    output_format = args.format
    if args.compact:
        output_format = "compact"
    if args.include_phonetic:
        output_format = "compact-phonetic"

    source = args.source.resolve()
    output = args.output.resolve()
    records = load_records(source)

    seen_words: dict[str, dict[str, str]] = {}
    detailed_rows: list[dict[str, str]] = []
    for record in records:
        card = extract_card(record)
        if card is None:
            continue

        key = card["word"].casefold()
        existing = seen_words.get(key)
        if existing is None:
            seen_words[key] = card
            detailed_rows.append(card)
            continue

        for field in ("phonetic", "example", "example_translation"):
            if not existing[field] and card[field]:
                existing[field] = card[field]

    if not detailed_rows:
        raise SystemExit("No usable entries were found in the source deck.")

    deck_name = args.name or source.stem.replace("_", " ")
    deck_id = args.deck_id or slugify(source.stem)
    write_tsv(
        detailed_rows,
        output,
        deck_id=deck_id,
        name=deck_name,
        language=args.language,
        version=args.version,
        output_format=output_format,
        include_metadata=not args.omit_metadata,
    )

    size = output.stat().st_size
    column_count = {"compact": 2, "compact-phonetic": 3, "detailed": 5}[output_format]
    print(f"wrote {len(detailed_rows)} entries to {output}")
    print(f"columns={column_count} size_bytes={size}")


if __name__ == "__main__":
    main()
