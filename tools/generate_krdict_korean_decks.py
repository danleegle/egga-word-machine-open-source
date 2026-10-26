#!/usr/bin/env python3
"""Generate conservative Korean-Chinese Egga decks from the official KRD JSON ZIP.

The generator never translates or synthesizes dictionary content. Card meanings
come only from Chinese ``Equivalent.lemma`` values in the downloaded Korean
Basic Dictionary data. Korean examples and written pronunciations are copied
only when they fit the firmware fields without truncation.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
import sys
import zipfile
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable


SOURCE_URL = "https://krdict.korean.go.kr/download/downloadPopup"
LICENSE_URL = "https://krdict.korean.go.kr/eng/kboardPolicy/copyRightTermsInfo"
LICENSE_NAME = "CC BY-SA 2.0 KR"

MAX_WORD_BYTES = 47
MAX_PHONETIC_BYTES = 63
MAX_MEANING_BYTES = 191
MAX_EXAMPLE_BYTES = 447
MAX_LINE_BYTES = 4096
MEANING_RE = re.compile(r"^\[([^\]]+)\]\s*(.*)$")

# The official export uses this literal marker when a sense has no usable
# Chinese equivalent. It is provenance, not a learner-facing definition.
UNUSABLE_EQUIVALENT_LEMMAS = {"(无对应词汇)"}

LEVELS = {
    "초급": ("beginner", "韩语基础词汇（初级）"),
    "중급": ("intermediate", "韩语基础词汇（中级）"),
    "고급": ("advanced", "韩语基础词汇（高级）"),
}


@dataclass(frozen=True)
class Card:
    word: str
    phonetic: str
    meaning: str
    example: str
    example_translation: str = ""


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    return value if isinstance(value, list) else [value]


def normalize_text(value: Any) -> str:
    text = html.unescape(str(value or ""))
    text = text.replace("\u200b", "").replace("\ufeff", "")
    text = re.sub(r"[\t\r\n]+", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def byte_len(value: str) -> int:
    return len(value.encode("utf-8"))


def feat_map(node: Any) -> dict[str, list[str]]:
    if not isinstance(node, dict):
        return {}
    result: dict[str, list[str]] = {}
    for feat in as_list(node.get("feat")):
        if not isinstance(feat, dict):
            continue
        key = normalize_text(feat.get("att"))
        value = normalize_text(feat.get("val"))
        if key and value:
            result.setdefault(key, []).append(value)
    return result


def first(values: dict[str, list[str]], key: str) -> str:
    items = values.get(key, [])
    return items[0] if items else ""


def lexical_entries(payload: dict[str, Any]) -> Iterable[dict[str, Any]]:
    resource = payload.get("LexicalResource", {})
    for lexicon in as_list(resource.get("Lexicon")):
        if not isinstance(lexicon, dict):
            continue
        for entry in as_list(lexicon.get("LexicalEntry")):
            if isinstance(entry, dict):
                yield entry


def source_creation_date(payload: dict[str, Any]) -> str:
    resource = payload.get("LexicalResource", {})
    information = resource.get("GlobalInformation", {})
    return first(feat_map(information), "creationDate")


def entry_word(entry: dict[str, Any]) -> str:
    lemma = entry.get("Lemma", {})
    return first(feat_map(lemma), "writtenForm")


def chinese_lemmas(sense: dict[str, Any], skipped: Counter[str]) -> list[str]:
    lemmas: list[str] = []
    for equivalent in as_list(sense.get("Equivalent")):
        features = feat_map(equivalent)
        if first(features, "language") != "중국어":
            continue
        lemma = first(features, "lemma")
        if lemma in UNUSABLE_EQUIVALENT_LEMMAS:
            skipped["placeholder_chinese_equivalent_omitted"] += 1
            continue
        if lemma and lemma not in lemmas:
            lemmas.append(lemma)
    return lemmas


def written_pronunciation(entry: dict[str, Any]) -> str:
    for word_form in as_list(entry.get("WordForm")):
        features = feat_map(word_form)
        if first(features, "type") != "발음":
            continue
        pronunciation = first(features, "pronunciation")
        if pronunciation:
            return pronunciation
    return ""


def sentence_example(senses: list[dict[str, Any]], included_sense_ids: set[int]) -> str:
    for index, sense in enumerate(senses):
        if index not in included_sense_ids:
            continue
        for example_node in as_list(sense.get("SenseExample")):
            features = feat_map(example_node)
            if first(features, "type") != "문장":
                continue
            examples = features.get("example", [])
            if len(examples) != 1:
                continue
            example = examples[0]
            if example and byte_len(example) <= MAX_EXAMPLE_BYTES:
                return example
    return ""


def build_card(entry: dict[str, Any], skipped: Counter[str]) -> tuple[str, Card] | None:
    features = feat_map(entry)
    level = first(features, "vocabularyLevel")
    if level not in LEVELS:
        skipped["not_graded"] += 1
        return None
    if first(features, "lexicalUnit") != "단어":
        skipped["not_word_unit"] += 1
        return None

    word = entry_word(entry)
    if not word:
        skipped["missing_word"] += 1
        return None
    if byte_len(word) > MAX_WORD_BYTES:
        skipped["word_too_long"] += 1
        return None

    senses = [sense for sense in as_list(entry.get("Sense")) if isinstance(sense, dict)]
    lemmas: list[str] = []
    included_sense_ids: set[int] = set()
    for index, sense in enumerate(senses):
        values = chinese_lemmas(sense, skipped)
        if not values:
            continue
        for value in values:
            if value not in lemmas:
                lemmas.append(value)
        included_sense_ids.add(index)

    if not lemmas:
        skipped["missing_chinese_equivalent"] += 1
        return None

    pos = first(features, "partOfSpeech")
    prefix = f"[{pos}] " if pos else ""
    selected: list[str] = []
    for lemma in lemmas:
        candidate = prefix + "；".join([*selected, lemma])
        if byte_len(candidate) > MAX_MEANING_BYTES:
            break
        selected.append(lemma)
        if len(selected) == 3:
            break
    if not selected:
        skipped["meaning_too_long"] += 1
        return None
    meaning = prefix + "；".join(selected)

    pronunciation = written_pronunciation(entry)
    if byte_len(pronunciation) > MAX_PHONETIC_BYTES:
        pronunciation = ""
        skipped["pronunciation_omitted_too_long"] += 1
    elif pronunciation:
        pronunciation = f"[{pronunciation}]"
        if byte_len(pronunciation) > MAX_PHONETIC_BYTES:
            pronunciation = ""
            skipped["pronunciation_omitted_too_long"] += 1

    example = sentence_example(senses, included_sense_ids)
    card = Card(
        word=word,
        phonetic=pronunciation,
        meaning=meaning,
        example=example,
    )
    line = "\t".join(asdict(card).values())
    if byte_len(line) > MAX_LINE_BYTES:
        skipped["line_too_long"] += 1
        return None
    return level, card


def merge_meanings(meanings: list[str], skipped: Counter[str]) -> str:
    groups: list[tuple[str, list[str]]] = []

    def render(candidate_groups: list[tuple[str, list[str]]]) -> str:
        parts: list[str] = []
        for pos, values in candidate_groups:
            body = "；".join(values)
            parts.append(f"[{pos}] {body}" if pos else body)
        return "；".join(parts)

    for meaning in meanings:
        match = MEANING_RE.match(meaning)
        pos, body = (match.group(1), match.group(2)) if match else ("", meaning)
        target = next((values for group_pos, values in groups if group_pos == pos), None)
        if target is not None and body in target:
            continue
        candidate = [(group_pos, list(values)) for group_pos, values in groups]
        candidate_target = next((values for group_pos, values in candidate if group_pos == pos), None)
        if candidate_target is None:
            candidate.append((pos, [body]))
        else:
            candidate_target.append(body)
        if byte_len(render(candidate)) > MAX_MEANING_BYTES:
            skipped["merged_meaning_omitted_too_long"] += 1
            continue
        groups = candidate
    return render(groups)


def merge_cards_by_word(cards: list[Card], skipped: Counter[str]) -> list[Card]:
    grouped: dict[str, list[Card]] = {}
    for card in cards:
        grouped.setdefault(card.word, []).append(card)

    merged: list[Card] = []
    for word, group in grouped.items():
        # Keep example, pronunciation and primary meaning paired with one
        # official source entry. Stable input order breaks equal-quality ties.
        primary = max(
            group,
            key=lambda card: (bool(card.example), bool(card.phonetic), byte_len(card.meaning)),
        )
        meanings = [primary.meaning]
        for card in group:
            if card.meaning in meanings:
                continue
            meanings.append(card.meaning)
        skipped["duplicate_word_cards_merged"] += len(group) - 1
        merged.append(
            Card(
                word=word,
                phonetic=primary.phonetic,
                meaning=merge_meanings(meanings, skipped),
                example=primary.example,
            )
        )
    return merged


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_deck(
    output: Path,
    level_key: str,
    name: str,
    cards: list[Card],
    source_date: str,
    source_hash: str,
) -> None:
    deck_id = f"krdict-ko-zh-{level_key}"
    lines = [
        f"# deck_id={deck_id}",
        f"# name={name}",
        "# language=ko-zh",
        "# version=1",
        f"# source={SOURCE_URL}",
        f"# source_date={source_date}",
        f"# source_sha256={source_hash}",
        f"# license={LICENSE_NAME}",
        f"# license_url={LICENSE_URL}",
        "word\tphonetic\tmeaning\texample\texample_translation",
    ]
    for card in cards:
        lines.append("\t".join(asdict(card).values()))
    output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def build_report(
    source: Path,
    source_hash: str,
    source_dates: set[str],
    source_entries: int,
    decks: dict[str, list[Card]],
    skipped: Counter[str],
    duplicate_cards: int,
) -> dict[str, Any]:
    deck_report: dict[str, Any] = {}
    for level, cards in decks.items():
        level_key, name = LEVELS[level]
        deck_report[level_key] = {
            "name": name,
            "cards": len(cards),
            "with_pronunciation": sum(bool(card.phonetic) for card in cards),
            "with_korean_example": sum(bool(card.example) for card in cards),
            "with_chinese_equivalent": sum(bool(card.meaning) for card in cards),
            "with_example_translation": 0,
            "unique_words": len({card.word for card in cards}),
            "max_word_bytes": max((byte_len(card.word) for card in cards), default=0),
            "max_meaning_bytes": max((byte_len(card.meaning) for card in cards), default=0),
            "max_example_bytes": max((byte_len(card.example) for card in cards), default=0),
        }
    return {
        "policy": {
            "translations_generated": False,
            "examples_generated": False,
            "truncated_fields": False,
            "meaning_source": "Chinese Equivalent.lemma from the official KRD JSON",
            "example_translation_policy": "left empty because the source does not provide it",
            "card_policy": "one card per written word; at most one official example",
        },
        "source": {
            "path": str(source),
            "url": SOURCE_URL,
            "sha256": source_hash,
            "creation_dates": sorted(source_dates),
            "license": LICENSE_NAME,
            "license_url": LICENSE_URL,
            "entries_scanned": source_entries,
        },
        "decks": deck_report,
        "skipped": dict(sorted(skipped.items())),
        "exact_duplicate_cards_removed": duplicate_cards,
    }


def write_markdown_report(path: Path, report: dict[str, Any]) -> None:
    lines = [
        "# 韩语词库数据质量报告",
        "",
        "## 结论",
        "",
        "- 所有中文词义均直接来自韩国国立国语院 JSON 的中文对译词字段。",
        "- 未生成、猜测或机器翻译任何释义和例句。",
        "- 官方数据没有例句中文翻译，因此该字段全部留空。",
        "- 超出固件字段长度的数据不会截断，会省略对应可选字段或整条剔除。",
        "",
        "## 生成结果",
        "",
        "| 词库 | 卡片数 | 唯一词形 | 有书面发音 | 有韩语例句 |",
        "|---|---:|---:|---:|---:|",
    ]
    for item in report["decks"].values():
        lines.append(
            f"| {item['name']} | {item['cards']} | {item['unique_words']} | "
            f"{item['with_pronunciation']} | {item['with_korean_example']} |"
        )
    lines.extend([
        "",
        "## 来源",
        "",
        f"- 下载页：{report['source']['url']}",
        f"- 数据日期：{', '.join(report['source']['creation_dates'])}",
        f"- SHA-256：`{report['source']['sha256']}`",
        f"- 授权：{report['source']['license']}",
        f"- 授权说明：{report['source']['license_url']}",
        "",
        "## 剔除与降级统计",
        "",
    ])
    for reason, count in report["skipped"].items():
        lines.append(f"- `{reason}`：{count}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True, help="Official KRD JSON ZIP")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--expected-sha256", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source = args.source.resolve()
    output_dir = args.output_dir.resolve()
    if not source.is_file() or not zipfile.is_zipfile(source):
        raise SystemExit(f"not a ZIP file: {source}")

    source_hash = sha256_file(source)
    if args.expected_sha256 and source_hash.lower() != args.expected_sha256.lower():
        raise SystemExit(
            f"source SHA-256 mismatch: expected {args.expected_sha256}, got {source_hash}"
        )

    decks: dict[str, list[Card]] = {level: [] for level in LEVELS}
    seen: dict[str, set[Card]] = {level: set() for level in LEVELS}
    skipped: Counter[str] = Counter()
    source_dates: set[str] = set()
    source_entries = 0
    duplicate_cards = 0

    with zipfile.ZipFile(source) as archive:
        json_names = sorted(name for name in archive.namelist() if name.lower().endswith(".json"))
        if not json_names:
            raise SystemExit("source ZIP contains no JSON files")
        for name in json_names:
            payload = json.loads(archive.read(name))
            date = source_creation_date(payload)
            if date:
                source_dates.add(date)
            for entry in lexical_entries(payload):
                source_entries += 1
                built = build_card(entry, skipped)
                if built is None:
                    continue
                level, card = built
                if card in seen[level]:
                    duplicate_cards += 1
                    continue
                seen[level].add(card)
                decks[level].append(card)

    if not source_dates:
        raise SystemExit("source JSON has no creationDate")
    if any(not cards for cards in decks.values()):
        raise SystemExit("one or more graded decks are empty")

    for level, cards in decks.items():
        decks[level] = merge_cards_by_word(cards, skipped)
        decks[level].sort(key=lambda card: card.word)

    output_dir.mkdir(parents=True, exist_ok=True)
    source_date = ",".join(sorted(source_dates))
    for level, cards in decks.items():
        level_key, name = LEVELS[level]
        write_deck(
            output_dir / f"{level_key}.tsv",
            level_key,
            name,
            cards,
            source_date,
            source_hash,
        )

    report = build_report(
        source,
        source_hash,
        source_dates,
        source_entries,
        decks,
        skipped,
        duplicate_cards,
    )
    (output_dir / "quality-report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    write_markdown_report(output_dir / "quality-report.md", report)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
