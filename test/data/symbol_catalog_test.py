#!/usr/bin/env python3
"""Independent catalog reproducibility and runtime data boundaries."""

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "data/tools"))
from generate_symbols import generate, load_catalog
from generate_symbol_ranges import generate as generate_ranges


class CatalogTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cxxime-catalog-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.catalog = self.root / "catalog.json"
        self.output = self.root / "symbols.json"
        self.data = {
            "schema": 1, "unicode_version": "17.0",
            "sources": [{"source": "Unicode 17.0", "license": "Unicode-3.0"}],
            "categories": [
                {"code": "dw", "name": "单位",
                 "groups": [{"name": "温度", "range": ["2103", "2103"]}]},
                {"code": "ts", "name": "特殊符号",
                 "groups": [{"name": "完整文本", "codepoints": [
                     "2103", "1F926 1F3FB 200D 2642 FE0F",
                 ]}]},
            ],
        }

    def save(self):
        self.catalog.write_text(json.dumps(self.data), encoding="utf-8")

    def test_catalog_reproduces_runtime_table_without_dictionaries(self):
        generate(ROOT / "data/symbol_catalog.json", self.output)
        self.assertEqual(
            self.output.read_text(encoding="utf-8"),
            (ROOT / "data/symbols.json").read_text(encoding="utf-8"),
        )

    def test_desktop_catalog_has_only_traditional_categories(self):
        _, categories = load_catalog(ROOT / "data/symbol_catalog.json")
        self.assertEqual([c["code"] for c in categories], [
            "bd", "sz", "sx", "jt", "xl", "dw", "hb", "ts", "py", "pp",
        ])
        self.assertEqual(sum(len(c["candidates"]) for c in categories), 304)

    def test_runtime_symbol_ranges_match_frozen_offline_properties(self):
        generate_ranges(
            ROOT / "data/tools/dict_builder/emoji_classification.json",
            self.output,
        )
        self.assertEqual(
            self.output.read_text(encoding="utf-8"),
            (ROOT / "shared/src/symbol_ranges.inc").read_text(encoding="utf-8"),
        )

    def test_order_cross_category_entries_and_sequences_are_preserved(self):
        self.save()
        generate(self.catalog, self.output)
        table = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(table["version"], 1)
        self.assertEqual(
            [c["code"] for c in table["categories"]], ["dw", "ts"],
        )
        self.assertEqual(table["categories"][0]["candidates"], ["\u2103"])
        self.assertEqual(table["categories"][1]["candidates"], [
            "\u2103", "\U0001f926\U0001f3fb\u200d\u2642\ufe0f",
        ])

    def test_invalid_entries_do_not_overwrite_previous_runtime_table(self):
        for sequence in ("D800", "110000", "0000", "abcd", "1F600 " * 100):
            with self.subTest(sequence=sequence):
                self.data["categories"][0]["groups"] = [
                    {"name": "无效项", "codepoints": [sequence]},
                ]
                self.save()
                self.output.write_bytes(b"previous")
                with self.assertRaises(ValueError):
                    generate(self.catalog, self.output)
                self.assertEqual(self.output.read_bytes(), b"previous")

    def test_duplicate_categories_and_candidates_are_rejected(self):
        original = copy.deepcopy(self.data)
        self.data["categories"].append(
            copy.deepcopy(self.data["categories"][0]),
        )
        self.save()
        with self.assertRaises(ValueError):
            load_catalog(self.catalog)
        self.data = original
        self.data["categories"][0]["groups"].append(
            {"name": "与范围重叠", "codepoints": ["2103"]},
        )
        self.save()
        with self.assertRaises(ValueError):
            load_catalog(self.catalog)

    def test_groups_keep_order_and_ranges_do_not_fill_omitted_gaps(self):
        self.data["categories"] = [{
            "code": "xl", "name": "字母", "groups": [
                {"name": "较后范围", "range": ["03C3", "03C4"]},
                {"name": "显式列表", "codepoints": ["03C0"]},
                {"name": "较前范围", "range": ["03B1", "03B2"]},
            ],
        }]
        self.save()
        _, categories = load_catalog(self.catalog)
        self.assertEqual(categories[0]["candidates"], [
            "\u03c3", "\u03c4", "\u03c0", "\u03b1", "\u03b2",
        ])

    def test_invalid_group_rules_do_not_overwrite_previous_output(self):
        invalid = [
            {}, {"name": "空规则"},
            {"name": "两种规则", "range": ["2103", "2103"],
             "codepoints": ["2103"]},
            {"name": "空列表", "codepoints": []},
            {"name": "非列表", "codepoints": "2103"},
            {"name": "非字符串", "codepoints": [0x2103]},
            {"name": " ", "codepoints": ["2103"]},
            {"name": "未知字段", "codepoints": ["2103"], "sort": True},
        ]
        for bounds in ([], ["2103"], ["2103", "2102"], ["D7FF", "E000"],
                       ["D800", "D800"], ["10FFFF", "110000"],
                       ["0000", "0001"], ["03b1", "03B2"],
                       ["2103 2109", "2109"], [0x2103, 0x2109]):
            invalid.append({"name": "无效范围", "range": bounds})
        for group in invalid:
            with self.subTest(group=group):
                self.data["categories"][0]["groups"] = [group]
                self.save()
                self.output.write_bytes(b"previous")
                with self.assertRaises(ValueError):
                    generate(self.catalog, self.output)
                self.assertEqual(self.output.read_bytes(), b"previous")

    def test_cannot_overwrite_catalog(self):
        self.save()
        before = self.catalog.read_bytes()
        with self.assertRaises(ValueError):
            generate(self.catalog, self.catalog)
        self.assertEqual(before, self.catalog.read_bytes())

    def test_reviewed_additions_are_available_in_their_categories(self):
        _, categories = load_catalog(ROOT / "data/symbol_catalog.json")
        table = {c["code"]: c["candidates"] for c in categories}
        additions = {
            "sz": list(range(0x246A, 0x2474)) + [0x217A, 0x217B],
            "sx": [0x2202, 0x2205, 0x2209, 0x2282, 0x2283, 0x2286, 0x2287],
            "jt": [0x2194, 0x2195, 0x21D0, 0x21D2, 0x21D4],
            "dw": [0x33A5], "hb": [0x20AC],
        }
        for code, points in additions.items():
            for cp in points:
                self.assertIn(chr(cp), table[code])
        # Currency defaults use standard forms; fullwidth yuan stays last.
        self.assertEqual(table["hb"], [
            "\u00a5", "$", "\u20ac", "\u00a3", "\u00a2", "\uffe5",
        ])
        self.assertEqual(table["sz"][:20],
                         [chr(cp) for cp in range(0x2460, 0x2474)])
        for candidates in table.values():
            self.assertNotIn("\uf8ff", candidates)
            self.assertNotIn("\ue76c", candidates)


if __name__ == "__main__":
    unittest.main()
