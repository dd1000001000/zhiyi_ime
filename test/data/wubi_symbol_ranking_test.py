#!/usr/bin/env python3
"""Symbol ordering and short-code protections in the Wubi prefix index."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "data/tools"))
from dict_builder import wubi_ranking as RANKING


def test_four_code_words_precede_same_frequency_symbols():
    for code, symbol, word, source_frequency, general_frequency in [
        (b"pwst", "⎵", "空格", 10, 102170),
        (b"cykg", "×", "叉号", 0, 1000),
    ]:
        entries = [
            (code, symbol.encode("utf-8"), source_frequency),
            (code, word.encode("utf-8"), source_frequency),
        ]
        frequencies = {entries[1][1]: general_frequency}
        source = RANKING.unique_source_ranking([0, 1], entries, 4)
        assert source == [0, 1]
        ranked = RANKING.rerank_visible_candidates(
            source, entries, 4, frequencies
        )
        # Keep the symbol reachable at the original code.
        assert ranked == [1, 0]
        audit = RANKING.RankingAudit()
        RANKING.audit_ranking_change(
            source, ranked, entries, 4, frequencies, audit
        )
        assert audit.symbol_order_repairs == 1
        assert audit.symbol_top_promotions == 1
        assert audit.safe_four_code_promotions == 0
        assert audit.unsafe_top_changes == 0
        audit.validate()
        for prefix_length in (1, 2, 3):
            assert RANKING.rerank_visible_candidates(
                source, entries, prefix_length, frequencies
            ) == source


def test_symbol_promotion_preserves_mixed_text_and_requires_evidence():
    for (
        first,
        second,
        first_frequency,
        second_frequency,
        first_general,
        second_general,
    ) in [
        ("U盘", "头重脚轻", 20, 10, 0, 102170),
        ("SD卡", "顶上", 20, 10, 0, 102170),
        ("SDK", "开发工具", 10, 10, 0, 102170),
        ("Apple", "劳军", 10, 10, 0, 102170),
        ("×汉", "叉号", 0, 0, 0, 1000),
        ("，", "逗号", 0, 0, 0, 1000),
        ("×", "叉号", 0, 0, 0, 0),
        ("×", "叉号", 10, 0, 0, 1000),
        ("×", "叉号", 0, 0, 2000, 1000),
    ]:
        entries = [
            (b"aaaa", first.encode("utf-8"), first_frequency),
            (b"aaaa", second.encode("utf-8"), second_frequency),
        ]
        frequencies = {
            entries[0][1]: first_general,
            entries[1][1]: second_general,
        }
        assert RANKING.rerank_visible_candidates(
            [0, 1], entries, 4, frequencies
        ) == [0, 1]

    entries = [(b"aaaa", "×".encode("utf-8"), 0)] * 10
    entries.append((b"aaaa", "叉号".encode("utf-8"), 0))
    # Do not import candidates from outside the visible set.
    assert RANKING.rerank_visible_candidates(
        list(range(11)), entries, 4, {entries[10][1]: 1000}
    ) == list(range(11))

    entries = [
        (b"aaaa", "×".encode("utf-8"), 10),
        (b"aaaa", "原词".encode("utf-8"), 10),
        (b"aaaa", "叉号".encode("utf-8"), 10),
    ]
    assert RANKING.rerank_visible_candidates(
        [0, 1, 2], entries, 4, {entries[2][1]: 1000}
    ) == [0, 1, 2]  # A promotion must not cross another ordinary word.


def test_symbol_repair_and_classification():
    for symbol in (
        "\u23b5", "\u00d7", "\U0001f42c",
        "\U0001f9ad", "\U0001f98d", "\U0001f9a7",
    ):
        assert RANKING._is_ranking_symbol(symbol.encode("utf-8"))
    for word in ("U盘", "SD卡", "SDK", "1", "\uff0c", "\u00d7汉", ""):
        assert not RANKING._is_ranking_symbol(word.encode("utf-8"))
    entries = [
        (b"itee", text.encode("utf-8"), 0)
        for text in ("海豚", "海豹", "\U0001f42c", "\U0001f9ad", "海月")
    ]
    frequencies = {entries[4][1]: 1}
    source = list(range(5))
    ranked = RANKING.rerank_visible_candidates(
        source, entries, 4, frequencies
    )
    assert ranked == [0, 1, 4, 2, 3]
    assert RANKING.rerank_visible_candidates(
        ranked, entries, 4, frequencies
    ) == ranked
    audit = RANKING.RankingAudit()
    RANKING.audit_ranking_change(
        source, ranked, entries, 4, frequencies, audit
    )
    assert audit.symbol_order_repairs == 1
    assert audit.symbol_top_promotions == audit.four_code_top_changes == 0
    audit.validate()


def test_short_completion_symbol_order():
    for prefix_length in (1, 2, 3):
        entries = [
            (b"cyk"[:prefix_length], "骧".encode("utf-8"), 10),
            (b"cykg", "\u00d7".encode("utf-8"), 0),
            (b"cykg", "叉号".encode("utf-8"), 0),
        ]
        frequencies = {entries[0][1]: 2552, entries[2][1]: 1000}
        source = [0, 1, 2]
        ranked = RANKING.rerank_visible_candidates(
            source, entries, prefix_length, frequencies
        )
        assert ranked == [0, 2, 1]
        audit = RANKING.RankingAudit()
        RANKING.audit_ranking_change(
            source, ranked, entries, prefix_length, frequencies, audit
        )
        assert audit.symbol_order_repairs == 1
        assert audit.short_symbol_repairs == int(prefix_length <= 2)
        assert audit.short_prefix_changes == audit.short_symbol_repairs
        assert audit.three_code_top_changes == 0
        audit.validate()


def test_three_code_corpus_and_symbol_order():
    # Both early-return paths and a successful corpus promotion must converge.
    for exact_code in (b"cyk", b"cyka"):
        for reliable_frequency in (0, 200000):
            entries = [
                (exact_code, "原词".encode("utf-8"), 10),
                (b"cykg", "\u00d7".encode("utf-8"), 0),
                (b"cykg", "叉号".encode("utf-8"), 0),
                (b"cykh", "常用".encode("utf-8"), 0),
            ]
            frequencies = {
                entries[0][1]: 1000,
                entries[2][1]: 1000,
                entries[3][1]: reliable_frequency,
            }
            source = [0, 1, 2, 3]
            expected = [0, 2, 1, 3]
            if reliable_frequency:
                expected = (
                    [0, 3, 2, 1] if exact_code == b"cyk" else [3, 0, 2, 1]
                )
            ranked = RANKING.rerank_visible_candidates(
                source, entries, 3, frequencies
            )
            assert ranked == expected
            audit = RANKING.RankingAudit()
            # The existing top-change budget requires a representative corpus.
            audit.three_code_prefixes = 10
            RANKING.audit_ranking_change(
                source, ranked, entries, 3, frequencies, audit
            )
            assert audit.symbol_order_repairs == 1
            assert audit.short_symbol_repairs == 0
            audit.validate()


def test_short_symbol_guards():
    for prefix_length in (1, 2, 3):
        prefix = b"aaa"[:prefix_length]
        for symbol_code, word_code, word_frequency in (
            (prefix, prefix, 0),  # Preserve exact short-code order.
            (prefix, b"aaaa", 0),  # Do not cross exact matches.
            (b"aaaa", b"aaab", 0),  # Do not cross full codes.
            (b"aaaa", b"aaaa", 1),  # Do not cross source frequencies.
        ):
            entries = [
                (prefix, "首选".encode("utf-8"), 10),
                (symbol_code, b"+", 0),
                (word_code, "加号".encode("utf-8"), word_frequency),
            ]
            assert RANKING.rerank_visible_candidates(
                [0, 1, 2], entries, prefix_length, {entries[2][1]: 1000}
            ) == [0, 1, 2]
        entries = [
            (b"aaaa", b"+", 0),
            (b"aaaa", "加号".encode("utf-8"), 0),
        ]
        assert RANKING.rerank_visible_candidates(
            [0, 1], entries, prefix_length, {entries[1][1]: 1000}
        ) == [0, 1]  # Preserve the corpus-ranked first candidate.


def test_short_window_preserves_symbol_order():
    for prefix_length in (1, 2, 3):
        for word_position in (9, 10):
            entries = [
                (b"aaa"[:prefix_length], "首选".encode("utf-8"), 10),
            ] + [
                (b"aaaa", chr(0x2200 + i).encode("utf-8"), 0)
                for i in range(10)
            ]
            entries[word_position] = (b"aaaa", "词语".encode("utf-8"), 0)
            source = list(range(11))
            ranked = RANKING.rerank_visible_candidates(
                source, entries, prefix_length,
                {entries[word_position][1]: 1000},
            )
            expected = (
                [0, 9] + list(range(1, 9)) + [10]
                if word_position == 9 else source
            )
            assert ranked == expected


def test_short_audit_rejects_unrelated_changes():
    for prefix_length in (1, 2):
        entries = [
            (b"aaa"[:prefix_length], "首选".encode("utf-8"), 10),
            (b"aaaa", b"+", 0),
            (b"aaaa", "加号".encode("utf-8"), 0),
        ] + [(b"aaab", str(i).encode(), 0) for i in range(9)]
        source = list(range(12))
        frequencies = {entries[2][1]: 1000}
        for ranked in (
            [0, 2, 1] + list(range(3, 10)) + [11, 10],  # Tail changed.
            [0, 2, 1, 4, 3] + list(range(5, 12)),  # Unrelated order changed.
            [2, 0, 1] + list(range(3, 12)),  # First candidate changed.
        ):
            audit = RANKING.RankingAudit()
            RANKING.audit_ranking_change(
                source, ranked, entries, prefix_length, frequencies, audit
            )
            assert audit.short_prefix_changes == 1
            assert audit.short_symbol_repairs == 0
            try:
                audit.validate()
            except ValueError:
                pass
            else:
                raise AssertionError("unclassified short-code change passed")


if __name__ == "__main__":
    test_four_code_words_precede_same_frequency_symbols()
    test_symbol_promotion_preserves_mixed_text_and_requires_evidence()
    test_symbol_repair_and_classification()
    test_short_completion_symbol_order()
    test_three_code_corpus_and_symbol_order()
    test_short_symbol_guards()
    test_short_window_preserves_symbol_order()
    test_short_audit_rejects_unrelated_changes()
