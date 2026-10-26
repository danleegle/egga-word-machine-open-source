#!/usr/bin/env python3
"""Translate Korean deck examples to Simplified Chinese with a restartable cache.

This tool intentionally keeps machine translations separate from the official
KRD-derived deck. The default provider is Google's unauthenticated GTX endpoint;
it is useful for local release preparation but is not a supported Google Cloud
API and must not be treated as an availability guarantee.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import random
import re
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import date
from pathlib import Path


ENDPOINT = "https://translate.googleapis.com/translate_a/single"
PROVIDER = "google-translate-gtx-unofficial"
MAX_TRANSLATION_BYTES = 255
HANGUL_RE = re.compile(r"[\uac00-\ud7af]")
HAN_RE = re.compile(r"[\u3400-\u9fff]")


def cache_key(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def load_cache(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    if not path.exists():
        return result
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            item = json.loads(line)
            if item.get("source") and item.get("translation"):
                result[cache_key(item["source"])] = item["translation"]
    return result


def translated_text(payload: object) -> str:
    if not isinstance(payload, list) or not payload or not isinstance(payload[0], list):
        raise ValueError("unexpected translation response")
    pieces: list[str] = []
    for segment in payload[0]:
        if isinstance(segment, list) and segment and isinstance(segment[0], str):
            pieces.append(segment[0])
    text = "".join(pieces).strip()
    if not text:
        raise ValueError("empty translation response")
    return text


def translate_batch(texts: list[str], timeout: int) -> list[str]:
    markers = [f"[[EGGA_SPLIT_{index:04d}]]" for index in range(1, len(texts))]
    joined_parts: list[str] = []
    for index, source in enumerate(texts):
        if index:
            joined_parts.append(markers[index - 1])
        joined_parts.append(source)
    query = urllib.parse.urlencode(
        {"client": "gtx", "sl": "ko", "tl": "zh-CN", "dt": "t", "q": "\n".join(joined_parts)}
    )
    request = urllib.request.Request(
        f"{ENDPOINT}?{query}",
        headers={"User-Agent": "EggaKoreanDeckTranslator/1.0"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    result = translated_text(payload)
    parts = [result]
    for marker in markers:
        next_parts: list[str] = []
        for part in parts:
            next_parts.extend(part.split(marker))
        parts = next_parts
    parts = [part.replace("\u200b", "").replace("\ufeff", "").strip() for part in parts]
    if len(parts) != len(texts) or any(not part for part in parts):
        raise ValueError(f"batch alignment failed: expected {len(texts)}, got {len(parts)}")
    return parts


def translate_single(text: str, source_language: str, target_language: str, timeout: int) -> str:
    query = urllib.parse.urlencode(
        {"client": "gtx", "sl": source_language, "tl": target_language, "dt": "t", "q": text}
    )
    request = urllib.request.Request(
        f"{ENDPOINT}?{query}",
        headers={"User-Agent": "EggaKoreanDeckTranslator/1.0"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return translated_text(json.loads(response.read().decode("utf-8")))


def translate_via_japanese(text: str, timeout: int) -> str:
    japanese = translate_single(text, "ko", "ja", timeout)
    return translate_single(japanese, "ja", "zh-CN", timeout)


def validate_pair(source: str, translation: str) -> None:
    if not translation or source == translation:
        raise ValueError("empty or untranslated result")
    if "[[EGGA_SPLIT_" in translation:
        raise ValueError("batch marker leaked into translation")
    if len(translation.encode("utf-8")) > MAX_TRANSLATION_BYTES:
        raise ValueError("translation exceeds device field capacity")
    if not HAN_RE.search(translation):
        raise ValueError("translation contains no Chinese character")


def read_deck(path: Path) -> tuple[list[str], list[list[str]]]:
    metadata: list[str] = []
    rows: list[list[str]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        for line in handle:
            if line.startswith("#"):
                metadata.append(line.rstrip("\r\n"))
                continue
            rows.extend(csv.reader([line], delimiter="\t"))
    if not rows or rows[0][:5] != ["word", "phonetic", "meaning", "example", "example_translation"]:
        raise ValueError("unsupported deck header")
    return metadata, rows


def write_deck(path: Path, metadata: list[str], rows: list[list[str]]) -> None:
    filtered = [line for line in metadata if not line.startswith("# example_translation_")]
    filtered.extend(
        [
            f"# example_translation_provider={PROVIDER}",
            "# example_translation_type=machine_translation",
            f"# example_translation_date={date.today().isoformat()}",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        for line in filtered:
            handle.write(line + "\n")
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, default=12)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--retries", type=int, default=5)
    parser.add_argument("--delay", type=float, default=0.25)
    parser.add_argument("--workers", type=int, default=1)
    args = parser.parse_args()
    if args.batch_size < 1 or args.batch_size > 25:
        parser.error("--batch-size must be between 1 and 25")
    if args.workers < 1 or args.workers > 6:
        parser.error("--workers must be between 1 and 6")

    metadata, rows = read_deck(args.input)
    cache = load_cache(args.cache)
    sources = list(dict.fromkeys(row[3] for row in rows[1:] if len(row) >= 5 and row[3]))
    missing = [source for source in sources if cache_key(source) not in cache]
    args.cache.parent.mkdir(parents=True, exist_ok=True)

    batches = [missing[start : start + args.batch_size] for start in range(0, len(missing), args.batch_size)]

    def translate_with_retries(batch: list[str]) -> tuple[list[str], list[str], list[str]]:
        error: Exception | None = None
        for attempt in range(args.retries):
            try:
                translations = translate_batch(batch, args.timeout)
                providers = [PROVIDER] * len(batch)
                for index, (source, translation) in enumerate(zip(batch, translations)):
                    try:
                        validate_pair(source, translation)
                    except ValueError as exc:
                        if str(exc) != "translation contains no Chinese character":
                            raise
                        translations[index] = translate_via_japanese(source, args.timeout)
                        providers[index] = PROVIDER + "-via-ja"
                        validate_pair(source, translations[index])
                time.sleep(args.delay)
                return batch, translations, providers
            except Exception as exc:  # network and response errors are retriable
                error = exc
                time.sleep(min(8.0, (2**attempt) + random.random()))
        raise RuntimeError(f"translation failed: {error}") from error

    completed = 0
    with args.cache.open("a", encoding="utf-8", newline="\n") as cache_handle:
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            for batch, translations, providers in executor.map(translate_with_retries, batches):
                for source, translation, provider in zip(batch, translations, providers):
                    cache[cache_key(source)] = translation
                    cache_handle.write(
                        json.dumps(
                            {"source": source, "translation": translation, "provider": provider},
                            ensure_ascii=False,
                        )
                        + "\n"
                    )
                cache_handle.flush()
                completed += len(batch)
                print(f"translated={completed}/{len(missing)} cached={len(sources) - len(missing)}", flush=True)

    translated = 0
    for row in rows[1:]:
        if len(row) < 5:
            row.extend([""] * (5 - len(row)))
        if not row[3]:
            row[4] = ""
            continue
        row[4] = cache[cache_key(row[3])]
        validate_pair(row[3], row[4])
        translated += 1
    write_deck(args.output, metadata, rows)
    print(json.dumps({"cards": len(rows) - 1, "examples_translated": translated, "output": str(args.output)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
