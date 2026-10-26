# Korean Deck Source and Content Policy

The Korean-Chinese decks are generated only from the Korean Basic Dictionary
JSON published by the National Institute of Korean Language.

- Download page: <https://krdict.korean.go.kr/download/downloadPopup>
- Copyright policy: <https://krdict.korean.go.kr/eng/kboardPolicy/copyRightTermsInfo>
- Text license: CC BY-SA 2.0 KR
- Generator: `tools/generate_krdict_korean_decks.py`

## Conservative content policy

The generator does not translate, paraphrase, or synthesize dictionary content.

1. Cards are limited to entries marked beginner, intermediate, or advanced.
2. A card is emitted only when the source contains an official Chinese
   `Equivalent.lemma` value.
3. Meanings contain the source part of speech plus at most three official
   Chinese equivalent lemmas.
4. Pronunciation is copied from the source's written pronunciation field. Audio
   URLs and audio files are not copied.
5. Korean sentence examples are copied only from the same senses that supplied
   the Chinese equivalents.
6. Example translations remain empty because the source does not provide them.
7. Required fields that exceed firmware limits cause the entry to be skipped.
   Optional pronunciation or example fields are omitted rather than truncated.

Every generation produces `quality-report.json` and `quality-report.md` with
source hashes, completeness counts, exclusions, and the explicit no-generation
policy.

The first release candidate is the beginner deck only. Intermediate and
advanced outputs remain audit artifacts until device memory, navigation, and
font coverage have been validated at their larger sizes.

## Regeneration

Download the current full JSON ZIP from the official download page, then run:

```powershell
python tools/generate_krdict_korean_decks.py `
  --source <official-json-zip> `
  --output-dir ..\output\korean-decks `
  --expected-sha256 <verified-sha256>
```

Do not publish regenerated decks until their quality report, font coverage, and
firmware rendering have all been verified.

For a verified release candidate, extend the existing vocabulary font with the
exact characters used by the selected deck:

```powershell
python tools/generate_vocabulary_font.py `
  --deck ..\output\korean-decks\beginner.tsv `
  --font-source managed_components\lvgl__lvgl\scripts\built_in_font\SourceHanSansSC-Normal.otf `
  --font-output main\components\78__xiaozhi-fonts\src\SourceHanSansSC_Regular_slim.c `
  --font-name SourceHanSansSC_Regular_slim `
  --size 16
```

The font generator preserves the existing Chinese/UI glyph set, verifies every
rendered deck character against the generated cmap, and uses the pinned
`lv_font_conv` 1.5.3 package. On Windows it installs the pinned converter into a
temporary directory and invokes its JavaScript entry point directly, avoiding
the shorter `cmd.exe` argument limit for large CJK glyph sets.
