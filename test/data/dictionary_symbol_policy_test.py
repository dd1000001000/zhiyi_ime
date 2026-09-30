#!/usr/bin/env python3
"""Filtering is shared by dictionaries and independent of symbol navigation."""

from contextlib import closing
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "data/tools"))
sys.path.insert(0, str(ROOT / "scripts"))

from dict_builder.symbol_policy import SymbolPolicy
from filter_dictionary_symbols import prepare_filtered_sources
import prepare_dictionary_bundle as bundle


UNREVIEWED_GRAPHIC_TEXTS = (
    "\u2103\ufe0f",  # unit + variation selector
    "\u2764\ufe0f!",  # emoji + punctuation
    "\U0001f600\u200d!",  # incomplete ZWJ sequence + punctuation
    "1\ufe0f\u20e3!", "1\u20e3!",  # keycaps + punctuation
    "\U000e0061",  # isolated tag component
)


def write_database(path, entries):
    # The connection context manages transactions; closing owns the handle.
    with closing(sqlite3.connect(path)) as connection:
        with connection:
            connection.execute(
                "CREATE TABLE dict (id INTEGER PRIMARY KEY, text TEXT NOT NULL, "
                "code TEXT NOT NULL, frequency INTEGER, syllable_ids TEXT)"
            )
            connection.execute("CREATE INDEX idx_code ON dict(code)")
            connection.executemany(
                "INSERT INTO dict (text, code, frequency, syllable_ids) "
                "VALUES (?, ?, ?, ?)",
                [(text, code, freq, code) for text, code, freq in entries],
            )


def read_rows(path):
    with closing(sqlite3.connect(path)) as connection:
        return connection.execute("SELECT * FROM dict ORDER BY id").fetchall()


class ClassificationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = SymbolPolicy()

    def test_complete_emoji_including_groups_not_in_catalog(self):
        for text in (
            "\U0001f600",  # grinning face
            "\U0001f926\U0001f3fb\u200d\u2642\ufe0f",  # skin tone + ZWJ
            "\U0001f578", "\U0001f578\ufe0f",  # qualification variants
            "\U0001f336",  # text-style hot pepper
            "\u26bd\ufe0f",  # legacy VS16 form
            "\U0001f4a1",  # Objects: not in the product catalog
            "\U0001f1e8\U0001f1f3",  # Flags: not in the product catalog
        ):
            with self.subTest(text=text):
                self.assertEqual(self.policy.classify(text), "emoji")

    def test_all_pure_symbols_leave_ordinary_candidates(self):
        symbols = (
            "\u2103\u2109\u00b0\u33a1"  # units
            "\u00d7\u00f7\u2248\u221a\u2030"  # math
            "\u2713\u00a9\u00ae"  # check, copyright, registered
            "\u2669\u266a\u266b\u266c\u266d\u266e\u23b5"  # music/space
            "\U0001f3fb"  # isolated modifier is a symbol, not a word
        )
        for text in symbols:
            with self.subTest(text=text):
                self.assertEqual(self.policy.classify(text), "symbol")
        self.assertEqual(self.policy.classify("+-"), "symbol")

    def test_words_and_mixed_text_are_retained(self):
        for text in ("空格", "童稚", "音符", "U盘", "SDK", "SD卡",
                     "艾伦\u00b7图灵", "昂格鲁-撒克逊人", "1", "abc",
                     "20\u2103", "开心\U0001f600", "1\U0001f600",
                     "20\u2103\ufe0f", "1\ufe0f", ""):
            with self.subTest(text=text):
                self.assertIsNone(self.policy.classify(text))
        for text in UNREVIEWED_GRAPHIC_TEXTS:
            for prefix in ("说明", "2"):
                with self.subTest(text=text, prefix=prefix):
                    self.assertIsNone(self.policy.classify(prefix + text))

    def test_unreviewed_sequences_require_audit(self):
        for text in (
            "\U0001f600\u200d",
            "\U0001f600\U0001f600",
            "\ufe0f",
            *UNREVIEWED_GRAPHIC_TEXTS,
        ):
            with self.subTest(text=text):
                with self.assertRaisesRegex(ValueError, "unreviewed"):
                    self.policy.classify(text)


class FilteringTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cxxime-filter-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.wubi = self.root / "wubi86.dict.db"
        self.pinyin = self.root / "pinyin.dict.db"
        self.wubi_out = self.root / "wubi.filtered.db"
        self.pinyin_out = self.root / "pinyin.filtered.db"

    def run_filter(self):
        return prepare_filtered_sources(
            self.wubi, self.pinyin, self.wubi_out, self.pinyin_out)

    def test_database_helpers_close_connections_on_success_and_error(self):
        connections = []
        connect = sqlite3.connect

        def retained_connection(*args, **kwargs):
            connection = connect(*args, **kwargs)
            connections.append(connection)
            return connection

        try:
            with mock.patch.object(sqlite3, "connect", side_effect=retained_connection):
                write_database(self.wubi, [("词", "a", 1)])
                self.assertEqual(read_rows(self.wubi)[0][1], "词")
                with self.assertRaises(sqlite3.OperationalError):
                    write_database(self.wubi, [])
                with self.assertRaises(sqlite3.OperationalError):
                    read_rows(self.pinyin)
            # Retain references so garbage collection cannot hide missing close().
            self.assertEqual(len(connections), 4)
            for connection in connections:
                with self.assertRaises(sqlite3.ProgrammingError):
                    connection.execute("SELECT 1")
            self.wubi.unlink()
            self.pinyin.unlink()
        finally:
            for connection in connections:
                connection.close()

    def test_filter_preserves_all_columns_ids_order_and_sources(self):
        write_database(self.wubi, [
            ("难苦荬", "coaw", 100), ("a", "copy", 10),
            ("\u2103", "ab", 10), ("\U0001f600", "abc", 20),
            ("童稚", "ujtw", 10),
        ])
        write_database(self.pinyin, [
            ("\u2103", "she'shi'du", 30), ("\U0001f4a1", "deng", 20),
            ("艾伦\u00b7图灵", "ai'lun'tu'ling", 40),
        ])
        expected_wubi = [read_rows(self.wubi)[i] for i in (0, 4)]
        expected_pinyin = read_rows(self.pinyin)[2:]
        before = [p.read_bytes() for p in (self.wubi, self.pinyin)]
        stats = self.run_filter()
        self.assertEqual(
            before,
            [p.read_bytes() for p in (self.wubi, self.pinyin)],
        )
        self.assertEqual(read_rows(self.wubi_out), expected_wubi)
        self.assertEqual(read_rows(self.pinyin_out), expected_pinyin)
        self.assertEqual(stats["wubi86"]["extension_rows"], 1)
        self.assertEqual(stats["wubi86"]["symbol_rows"], 1)
        self.assertEqual(stats["pinyin"]["emoji_rows"], 1)

    def test_zip_source_and_no_matches_are_unchanged(self):
        write_database(self.wubi, [("普通词", "a", 30)])
        write_database(self.pinyin, [("普通词", "pu'tong'ci", 30)])
        archive = self.root / "wubi86.dict.db.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.write(self.wubi, "wubi86.dict.db")
        original = archive.read_bytes()
        prepare_filtered_sources(archive, self.pinyin,
                                 self.wubi_out, self.pinyin_out)
        self.assertEqual(original, archive.read_bytes())
        for source, output in ((self.wubi, self.wubi_out),
                               (self.pinyin, self.pinyin_out)):
            self.assertEqual(source.read_bytes(), output.read_bytes())

    def test_bad_sequences_and_unknown_extension_do_not_publish(self):
        cases = [("cozz", "词", "词"), ("abc", "词", "\U0001f600\u200d")]
        for text in UNREVIEWED_GRAPHIC_TEXTS:
            cases.extend((("abc", text, "词"), ("abc", "词", text)))
        for wubi_code, wubi_text, pinyin_text in cases:
            for path in (self.wubi, self.pinyin):
                path.unlink(missing_ok=True)
            write_database(self.wubi, [(wubi_text, wubi_code, 1)])
            write_database(self.pinyin, [(pinyin_text, "a", 1)])
            for path in (self.wubi_out, self.pinyin_out):
                path.write_bytes(b"previous")
            with self.assertRaises(ValueError):
                self.run_filter()
            for path in (self.wubi_out, self.pinyin_out):
                self.assertEqual(path.read_bytes(), b"previous")

    def test_source_and_output_aliases_are_rejected(self):
        for target in (self.wubi, self.pinyin, self.pinyin_out):
            with self.subTest(target=target):
                with self.assertRaises(ValueError):
                    prepare_filtered_sources(self.wubi, self.pinyin,
                                             target, self.pinyin_out)

    def test_workers_use_filtered_sources_without_growing_catalog(self):
        write_database(self.wubi, [("词", "a", 1), ("\u2103", "b", 1)])
        write_database(self.pinyin, [("词", "ci", 1),
                                     ("\U0001f4a1", "deng", 1)])
        for name in ("symbols.json", "symbol_catalog.json"):
            shutil.copy2(ROOT / "data" / name, self.root / name)

        def pinyin_worker(db, output):
            self.assertEqual([r[1] for r in read_rows(db)], ["词"])
            return []

        def wubi_worker(db, output, ranking_source):
            self.assertEqual([r[1] for r in read_rows(db)], ["词"])
            self.assertEqual(Path(ranking_source), self.pinyin)
            return []

        for workers in (1, 2):
            with mock.patch.object(
                bundle,
                "prepare_pinyin_dictionary",
                side_effect=pinyin_worker,
            ):
                with mock.patch.object(
                    bundle,
                    "prepare_wubi_dictionary",
                    side_effect=wubi_worker,
                ):
                    output = self.root / f"out{workers}"
                    bundle.prepare_dictionary_bundle(
                        str(self.root), str(output), workers=workers,
                        defer_topn_conversion=True,
                    )
                    self.assertEqual(
                        (self.root / "symbols.json").read_bytes(),
                        (output / "symbols.json").read_bytes(),
                    )


if __name__ == "__main__":
    unittest.main()
