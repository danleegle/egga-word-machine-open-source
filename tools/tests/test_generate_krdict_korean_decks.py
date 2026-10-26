import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "generate_krdict_korean_decks.py"
SPEC = importlib.util.spec_from_file_location("generate_krdict_korean_decks", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def feat(att, val):
    return {"att": att, "val": val}


def entry(*, chinese=True, example="한국어 예문입니다.", word="사과"):
    sense = {
        "Equivalent": [{"feat": [feat("language", "중국어"), feat("lemma", "苹果")]}]
        if chinese
        else [],
        "SenseExample": [{"feat": [feat("type", "문장"), feat("example", example)]}],
    }
    return {
        "Lemma": {"feat": feat("writtenForm", word)},
        "Sense": [sense],
        "WordForm": {"feat": [feat("type", "발음"), feat("pronunciation", "사과")]},
        "feat": [
            feat("vocabularyLevel", "초급"),
            feat("lexicalUnit", "단어"),
            feat("partOfSpeech", "명사"),
        ],
    }


class GeneratorTests(unittest.TestCase):
    def test_official_fields_are_copied_without_translation(self):
        skipped = MODULE.Counter()
        level, card = MODULE.build_card(entry(), skipped)
        self.assertEqual(level, "초급")
        self.assertEqual(card.word, "사과")
        self.assertEqual(card.phonetic, "[사과]")
        self.assertEqual(card.meaning, "[명사] 苹果")
        self.assertEqual(card.example, "한국어 예문입니다.")
        self.assertEqual(card.example_translation, "")
        self.assertFalse(skipped)

    def test_missing_chinese_equivalent_is_rejected(self):
        skipped = MODULE.Counter()
        self.assertIsNone(MODULE.build_card(entry(chinese=False), skipped))
        self.assertEqual(skipped["missing_chinese_equivalent"], 1)

    def test_placeholder_chinese_equivalent_is_rejected(self):
        placeholder = entry()
        placeholder["Sense"][0]["Equivalent"][0]["feat"][1]["val"] = "(无对应词汇)"
        skipped = MODULE.Counter()
        self.assertIsNone(MODULE.build_card(placeholder, skipped))
        self.assertEqual(skipped["placeholder_chinese_equivalent_omitted"], 1)
        self.assertEqual(skipped["missing_chinese_equivalent"], 1)

    def test_long_optional_example_is_omitted_not_truncated(self):
        skipped = MODULE.Counter()
        _, card = MODULE.build_card(entry(example="가" * 200), skipped)
        self.assertEqual(card.example, "")

    def test_long_required_word_is_rejected(self):
        skipped = MODULE.Counter()
        self.assertIsNone(MODULE.build_card(entry(word="가" * 16), skipped))
        self.assertEqual(skipped["word_too_long"], 1)

    def test_duplicate_written_words_become_one_card_and_one_example(self):
        cards = [
            MODULE.Card("gada", "[gada]", "[verb] meaning-a", "example-a"),
            MODULE.Card("gada", "[gada]", "[verb] meaning-b", "example-b"),
        ]
        skipped = MODULE.Counter()
        merged = MODULE.merge_cards_by_word(cards, skipped)
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0].example, "example-a")
        self.assertIn("meaning-a", merged[0].meaning)
        self.assertIn("meaning-b", merged[0].meaning)
        self.assertEqual(skipped["duplicate_word_cards_merged"], 1)


if __name__ == "__main__":
    unittest.main()
