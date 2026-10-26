import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "translate_korean_deck_examples.py"
SPEC = importlib.util.spec_from_file_location("translate_korean_deck_examples", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class TranslationTests(unittest.TestCase):
    def test_segmented_payload_is_joined(self):
        payload = [[["你好", "안녕"], ["。", "."]], None, "ko"]
        self.assertEqual(MODULE.translated_text(payload), "你好。")

    def test_valid_translation_must_fit_device(self):
        MODULE.validate_pair("오늘 날씨가 좋다.", "今天天气很好。")
        with self.assertRaises(ValueError):
            MODULE.validate_pair("오늘 날씨가 좋다.", "오늘 날씨가 좋다.")
        with self.assertRaises(ValueError):
            MODULE.validate_pair("짧다.", "长" * 86)


if __name__ == "__main__":
    unittest.main()
