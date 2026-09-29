"""Rank visible Wubi candidates with conservative corpus-based rules."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import sqlite3
import struct
import unicodedata
from typing import Dict, List, Optional, Sequence, Set, Tuple


VISIBLE_CANDIDATE_CAPACITY = 10
MIN_RELIABLE_GENERAL_FREQUENCY = 100000
MIN_RELIABLE_FOUR_CODE_CHARACTER_FREQUENCY = 1000000
MAX_PROMOTED_COMPLETION_LENGTH = 3
MAX_FOUR_CODE_PROMOTION_PERMILLE = 1
MIN_RARE_SINGLE_CHARACTER_BLOCKERS = 3
MIN_BLOCKED_COMPLETION_FREQUENCY = 500000
MAX_BLOCKED_COMPLETION_LENGTH = 4
MAX_BLOCKED_COMPLETION_PROMOTION_PERMILLE = 1
DictionaryEntry = Tuple[bytes, bytes, int]


def ranking_rules() -> Dict[str, int]:
    return {
        "visible_candidate_capacity": VISIBLE_CANDIDATE_CAPACITY,
        "min_reliable_general_frequency": MIN_RELIABLE_GENERAL_FREQUENCY,
        "min_reliable_four_code_character_frequency": (
            MIN_RELIABLE_FOUR_CODE_CHARACTER_FREQUENCY
        ),
        "max_promoted_completion_length": MAX_PROMOTED_COMPLETION_LENGTH,
        "max_four_code_promotion_permille": MAX_FOUR_CODE_PROMOTION_PERMILLE,
        "min_rare_single_character_blockers": MIN_RARE_SINGLE_CHARACTER_BLOCKERS,
        "min_blocked_completion_frequency": MIN_BLOCKED_COMPLETION_FREQUENCY,
        "max_blocked_completion_length": MAX_BLOCKED_COMPLETION_LENGTH,
        "max_blocked_completion_promotion_permille": (
            MAX_BLOCKED_COMPLETION_PROMOTION_PERMILLE
        ),
        "symbol_order_policy_version": 2,
    }


@dataclass(frozen=True)
class RankingOverride:
    input_code: bytes
    candidate_text: bytes
    candidate_code: bytes
    target_position: int
    reason: str


def _parse_wubi_code(value: object, field: str) -> bytes:
    if not isinstance(value, str):
        raise ValueError(f"Wubi ranking override {field} must be a string")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError(f"Wubi ranking override {field} must be ASCII") from error
    if (
        not encoded
        or len(encoded) > 4
        or any(ch < ord("a") or ch > ord("z") for ch in encoded)
    ):
        raise ValueError(f"invalid Wubi ranking override {field}: {value!r}")
    return encoded


def load_ranking_overrides(path: Optional[str]) -> Dict[bytes, List[RankingOverride]]:
    if path is None:
        return {}
    with open(path, "r", encoding="utf-8") as source:
        document = json.load(source)
    if (
        not isinstance(document, dict)
        or set(document) != {"format", "overrides"}
        or document.get("format") != 1
    ):
        raise ValueError(f"unsupported Wubi ranking overrides: {path}")
    records = document.get("overrides")
    if not isinstance(records, list):
        raise ValueError("Wubi ranking overrides must contain an overrides array")

    result: Dict[bytes, List[RankingOverride]] = {}
    identities = set()
    positions = set()
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("Wubi ranking override entries must be objects")
        required_fields = {
            "input_code",
            "candidate_text",
            "candidate_code",
            "target_position",
            "reason",
        }
        if set(record) != required_fields:
            raise ValueError("Wubi ranking override entry fields do not match format 1")
        input_code = _parse_wubi_code(record.get("input_code"), "input_code")
        candidate_code = _parse_wubi_code(record.get("candidate_code"), "candidate_code")
        candidate_text = record.get("candidate_text")
        target_position = record.get("target_position")
        reason = record.get("reason")
        if (
            not isinstance(candidate_text, str)
            or not candidate_text
            or "\t" in candidate_text
            or "\r" in candidate_text
            or "\n" in candidate_text
        ):
            raise ValueError("invalid Wubi ranking override candidate_text")
        if (
            not isinstance(target_position, int)
            or isinstance(target_position, bool)
            or target_position < 1
        ):
            raise ValueError("Wubi ranking override target_position must be a positive integer")
        if not isinstance(reason, str) or not reason.strip():
            raise ValueError("Wubi ranking override reason must not be empty")
        if not candidate_code.startswith(input_code):
            raise ValueError("Wubi ranking override candidate_code must start with input_code")

        identity = (input_code, candidate_text, candidate_code)
        position = (input_code, target_position)
        if identity in identities:
            raise ValueError("duplicate Wubi ranking override candidate")
        if position in positions:
            raise ValueError("duplicate Wubi ranking override target_position")
        identities.add(identity)
        positions.add(position)
        result.setdefault(input_code, []).append(
            RankingOverride(
                input_code=input_code,
                candidate_text=candidate_text.encode("utf-8"),
                candidate_code=candidate_code,
                target_position=target_position - 1,
                reason=reason.strip(),
            )
        )
    for overrides in result.values():
        overrides.sort(key=lambda item: item.target_position)
    return result


def ranking_overrides_fingerprint(
    overrides: Dict[bytes, List[RankingOverride]],
) -> str:
    digest = hashlib.sha256()
    for input_code in sorted(overrides):
        for item in overrides[input_code]:
            digest.update(
                struct.pack(
                    "<IIIII",
                    len(item.input_code),
                    len(item.candidate_text),
                    len(item.candidate_code),
                    item.target_position,
                    len(item.reason.encode("utf-8")),
                )
            )
            digest.update(item.input_code)
            digest.update(item.candidate_text)
            digest.update(item.candidate_code)
            digest.update(item.reason.encode("utf-8"))
    return digest.hexdigest()


def dictionary_fingerprint(entries: Sequence[DictionaryEntry]) -> str:
    digest = hashlib.sha256()
    for code, text, frequency in entries:
        digest.update(struct.pack("<IIi", len(code), len(text), frequency))
        digest.update(code)
        digest.update(text)
    return digest.hexdigest()


def frequency_fingerprint(frequencies: Dict[bytes, int]) -> str:
    digest = hashlib.sha256()
    for text in sorted(frequencies):
        digest.update(struct.pack("<II", len(text), frequencies[text]))
        digest.update(text)
    return digest.hexdigest()


def ranking_fingerprint(
    records: Sequence[Tuple[bytes, Sequence[int]]],
) -> str:
    digest = hashlib.sha256()
    for prefix, ranking in records:
        digest.update(struct.pack("<II", len(prefix), len(ranking)))
        digest.update(prefix)
        for entry_index in ranking:
            digest.update(struct.pack("<I", entry_index))
    return digest.hexdigest()


@dataclass
class RankingAudit:
    prefix_count: int = 0
    three_code_prefixes: int = 0
    four_code_prefixes: int = 0
    short_prefix_changes: int = 0
    short_symbol_repairs: int = 0  # Classified one- and two-code changes.
    visible_set_changes: int = 0
    unsafe_top_changes: int = 0
    three_code_top_changes: int = 0
    four_code_top_changes: int = 0
    safe_four_code_promotions: int = 0
    symbol_order_repairs: int = 0
    symbol_top_promotions: int = 0
    blocked_completion_promotions: int = 0
    ranking_override_changes: int = 0
    ranking_override_visible_set_changes: int = 0
    unknown_exact_demotions: int = 0
    visible_order_changes: int = 0
    visible_position_moves: int = 0
    internal_frequency_regressions: int = 0

    def validate(self) -> None:
        errors = []
        if self.short_prefix_changes != self.short_symbol_repairs:
            errors.append(
                "unclassified short prefix changes: "
                f"{self.short_prefix_changes - self.short_symbol_repairs}"
            )
        unreviewed_visible_set_changes = (
            self.visible_set_changes - self.ranking_override_visible_set_changes
        )
        if unreviewed_visible_set_changes:
            errors.append(
                "unreviewed visible candidate set changes: "
                f"{unreviewed_visible_set_changes}"
            )
        if self.internal_frequency_regressions:
            errors.append(
                "visible candidate frequency regressions: "
                f"{self.internal_frequency_regressions}"
            )
        if self.unsafe_top_changes:
            errors.append(f"unsafe top candidate changes: {self.unsafe_top_changes}")
        if self.three_code_top_changes > self.three_code_prefixes * 20 // 100:
            errors.append(
                "three-code top candidate changes exceed the 20% review budget: "
                f"{self.three_code_top_changes}/{self.three_code_prefixes}"
            )
        blocked_completion_budget = max(
            1,
            self.three_code_prefixes
            * MAX_BLOCKED_COMPLETION_PROMOTION_PERMILLE
            // 1000,
        )
        if self.blocked_completion_promotions > blocked_completion_budget:
            errors.append(
                "blocked completion promotions exceed the review budget: "
                f"{self.blocked_completion_promotions}/"
                f"{self.three_code_prefixes}"
            )
        classified_four_code = (
            self.safe_four_code_promotions + self.symbol_top_promotions
        )
        if self.four_code_top_changes != classified_four_code:
            errors.append(
                "unclassified four-code top candidate changes: "
                f"{self.four_code_top_changes - classified_four_code}"
            )
        four_code_promotion_budget = max(
            1,
            self.four_code_prefixes * MAX_FOUR_CODE_PROMOTION_PERMILLE // 1000,
        )
        if classified_four_code > four_code_promotion_budget:
            errors.append(
                "safe four-code promotions exceed the review budget: "
                f"{classified_four_code}/{self.four_code_prefixes}"
            )
        if self.unknown_exact_demotions > self.three_code_prefixes // 100:
            errors.append(
                "unknown exact demotions exceed the 1% review budget: "
                f"{self.unknown_exact_demotions}/{self.three_code_prefixes}"
            )
        if errors:
            raise ValueError("Wubi ranking audit failed: " + ", ".join(errors))


def load_general_frequencies(
    database_path: str,
    needed_texts: Set[bytes],
) -> Dict[bytes, int]:
    """Load the maximum corpus frequency for each text from the Pinyin source."""
    frequencies: Dict[bytes, int] = {}
    connection = sqlite3.connect(database_path)
    try:
        for text, frequency in connection.execute("SELECT text, frequency FROM dict"):
            encoded = text.encode("utf-8")
            if encoded not in needed_texts:
                continue
            value = max(int(frequency or 0), 0)
            if value > frequencies.get(encoded, -1):
                frequencies[encoded] = value
    finally:
        connection.close()
    return frequencies


def source_rank_key(
    entry_index: int,
    entries: Sequence[DictionaryEntry],
    prefix_length: int,
):
    code, text, frequency = entries[entry_index]
    return (
        0 if len(code) == prefix_length else 1,
        len(code),
        -frequency,
        code,
        len(text),
        text,
        entry_index,
    )


def unique_source_ranking(
    entry_indexes: Sequence[int],
    entries: Sequence[DictionaryEntry],
    prefix_length: int,
) -> List[int]:
    ranked = sorted(
        entry_indexes,
        key=lambda index: source_rank_key(index, entries, prefix_length),
    )
    unique = []
    seen_text = set()
    for entry_index in ranked:
        text = entries[entry_index][1]
        if text in seen_text:
            continue
        seen_text.add(text)
        unique.append(entry_index)
    return unique


def _text_length(text: bytes) -> int:
    return len(text.decode("utf-8"))


def _is_han_text(text: bytes) -> bool:
    decoded = text.decode("utf-8")
    return bool(decoded) and all(
        0x3400 <= ord(character) <= 0x4DBF
        or 0x4E00 <= ord(character) <= 0x9FFF
        or 0xF900 <= ord(character) <= 0xFAFF
        or 0x20000 <= ord(character) <= 0x3134F
        for character in decoded
    )


def _is_ranking_symbol(text: bytes) -> bool:
    # Freeze the policy across Python Unicode versions. Technical-symbol and
    # pictograph blocks include reserved positions; this is a ranking policy,
    # not a claim that every position has Unicode category S. In Unicode 3.2,
    # U+23B5 was Pe, and newer emoji such as U+1F9AD were unassigned.
    decoded = text.decode("utf-8")
    return bool(decoded) and all(
        unicodedata.ucd_3_2_0.category(character).startswith("S")
        or 0x2300 <= ord(character) <= 0x23FF
        or 0x1F300 <= ord(character) <= 0x1FAFF
        for character in decoded
    )


def _repair_symbol_blockers(
    visible, entries, general_frequencies, prefix_length
):
    reordered = list(visible)
    # Short-code exact matches and the corpus-ranked first candidate stay put.
    first_movable = 1 if prefix_length < 4 else 0
    for position in range(1, len(reordered)):
        _, entry_index = reordered[position]
        code, text, source_frequency = entries[entry_index]
        frequency = general_frequencies.get(text, 0)
        if not _is_han_text(text) or frequency <= 0:
            continue
        if prefix_length < 4 and len(code) <= prefix_length:
            continue
        while position > first_movable:
            _, prior_index = reordered[position - 1]
            prior_code, prior_text, prior_source_frequency = (
                entries[prior_index]
            )
            if (
                prior_code != code
                or prior_source_frequency != source_frequency
                or not _is_ranking_symbol(prior_text)
                or general_frequencies.get(prior_text, 0) > frequency
            ):
                break
            reordered[position - 1], reordered[position] = (
                reordered[position],
                reordered[position - 1],
            )
            position -= 1
    return reordered


def _completion_key(
    item: Tuple[int, int],
    entries: Sequence[DictionaryEntry],
    general_frequencies: Dict[bytes, int],
):
    source_position, entry_index = item
    text = entries[entry_index][1]
    return (
        -general_frequencies.get(text, 0),
        _text_length(text),
        source_position,
    )


def _safe_four_code_promotion(
    visible: Sequence[Tuple[int, int]],
    entries: Sequence[DictionaryEntry],
    general_frequencies: Dict[bytes, int],
) -> Tuple[int, int] | None:
    eligible = []
    for source_position, entry_index in visible[1:]:
        code, text, _ = entries[entry_index]
        frequency = general_frequencies.get(text, 0)
        if (
            len(code) != 4
            or _text_length(text) != 1
            or not _is_han_text(text)
            or frequency < MIN_RELIABLE_FOUR_CODE_CHARACTER_FREQUENCY
        ):
            continue
        prior_frequency = max(
            (
                general_frequencies.get(entries[prior_index][1], 0)
                for _, prior_index in visible[:source_position]
            ),
            default=0,
        )
        if frequency < prior_frequency:
            continue
        eligible.append((source_position, entry_index))

    if not eligible:
        return None
    return min(
        eligible,
        key=lambda item: (
            -general_frequencies.get(entries[item[1]][1], 0),
            item[0],
        ),
    )


def _repair_rare_single_character_blockers(
    visible: Sequence[Tuple[int, int]],
    entries: Sequence[DictionaryEntry],
    general_frequencies: Dict[bytes, int],
) -> List[Tuple[int, int]] | None:
    leading_blocker_count = 0
    for _, entry_index in visible:
        text = entries[entry_index][1]
        if (
            _text_length(text) != 1
            or not _is_han_text(text)
            or general_frequencies.get(text, 0) != 0
        ):
            break
        leading_blocker_count += 1
    if leading_blocker_count < MIN_RARE_SINGLE_CHARACTER_BLOCKERS:
        return None

    strong_completions = [
        item
        for item in visible[leading_blocker_count:]
        if (
            2
            <= _text_length(entries[item[1]][1])
            <= MAX_BLOCKED_COMPLETION_LENGTH
            and _is_han_text(entries[item[1]][1])
            and general_frequencies.get(entries[item[1]][1], 0)
            >= MIN_BLOCKED_COMPLETION_FREQUENCY
        )
    ]
    if not strong_completions:
        return None
    strong_completions.sort(
        key=lambda item: _completion_key(item, entries, general_frequencies)
    )
    rare_blockers = [
        item
        for item in visible
        if (
            _text_length(entries[item[1]][1]) == 1
            and _is_han_text(entries[item[1]][1])
            and general_frequencies.get(entries[item[1]][1], 0) == 0
        )
    ]
    meaningful = [
        item
        for item in visible
        if (
            item not in strong_completions
            and item not in rare_blockers
            and 2
            <= _text_length(entries[item[1]][1])
            <= MAX_BLOCKED_COMPLETION_LENGTH
            and _is_han_text(entries[item[1]][1])
        )
    ]
    other = [
        item
        for item in visible
        if (
            item not in strong_completions
            and item not in meaningful
            and item not in rare_blockers
        )
    ]
    return strong_completions + meaningful + rare_blockers + other


def _rerank_corpus_candidates(
    source_ranking: Sequence[int],
    entries: Sequence[DictionaryEntry],
    prefix_length: int,
    general_frequencies: Dict[bytes, int],
) -> List[int]:
    """Rerank only the existing visible set; deeper candidates remain untouched."""
    if prefix_length <= 2 or not source_ranking:
        return list(source_ranking)

    visible = list(enumerate(source_ranking[:VISIBLE_CANDIDATE_CAPACITY]))
    if prefix_length == 3:
        exact = []
        reliable_completions = []
        other_completions = []
        for item in visible:
            _, entry_index = item
            code, text, _ = entries[entry_index]
            general_frequency = general_frequencies.get(text, 0)
            if len(code) == prefix_length:
                exact.append(item)
            elif (
                general_frequency >= MIN_RELIABLE_GENERAL_FREQUENCY
                and _text_length(text) <= MAX_PROMOTED_COMPLETION_LENGTH
                and _is_han_text(text)
            ):
                reliable_completions.append(item)
            else:
                other_completions.append(item)

        if not reliable_completions:
            return list(source_ranking)
        if exact:
            reliable_completions.sort(
                key=lambda item: _completion_key(item, entries, general_frequencies)
            )
            exact_has_general_evidence = any(
                general_frequencies.get(entries[item[1]][1], 0) > 0
                for item in exact
            )
            exact_is_han = all(_is_han_text(entries[item[1]][1]) for item in exact)
            if not exact_has_general_evidence and exact_is_han:
                promoted = reliable_completions[:1]
                remaining = exact + reliable_completions[1:] + other_completions
                remaining.sort(key=lambda item: item[0])
                reordered = promoted + remaining
            else:
                reordered = exact + reliable_completions + other_completions
        else:
            source_top_length = _text_length(entries[visible[0][1]][1])
            source_top_frequency = general_frequencies.get(
                entries[visible[0][1]][1], 0
            )
            promotable = [
                item
                for item in reliable_completions
                if _text_length(entries[item[1]][1]) <= source_top_length
                and general_frequencies.get(entries[item[1]][1], 0)
                >= source_top_frequency
                and general_frequencies.get(entries[item[1]][1], 0)
                >= max(
                    (
                        general_frequencies.get(entries[other[1]][1], 0)
                        for other in visible[: item[0]]
                    ),
                    default=0,
                )
            ]
            if not promotable:
                repaired = _repair_rare_single_character_blockers(
                    visible, entries, general_frequencies
                )
                if repaired is None:
                    return list(source_ranking)
                reordered = repaired
            else:
                promotable.sort(
                    key=lambda item: _completion_key(
                        item, entries, general_frequencies
                    )
                )
                promoted_indexes = {item[1] for item in promotable}
                remaining = [
                    item for item in visible if item[1] not in promoted_indexes
                ]
                reordered = promotable + remaining
    elif prefix_length == 4:
        promoted = _safe_four_code_promotion(
            visible, entries, general_frequencies
        )
        reordered = (
            visible
            if promoted is None
            else [promoted] + [item for item in visible if item != promoted]
        )
    else:
        return list(source_ranking)

    return [entry_index for _, entry_index in reordered] + list(
        source_ranking[VISIBLE_CANDIDATE_CAPACITY:]
    )


def rerank_visible_candidates(
    source_ranking: Sequence[int],
    entries: Sequence[DictionaryEntry],
    prefix_length: int,
    general_frequencies: Dict[bytes, int],
) -> List[int]:
    ranked = _rerank_corpus_candidates(
        source_ranking, entries, prefix_length, general_frequencies
    )
    visible = list(enumerate(ranked[:VISIBLE_CANDIDATE_CAPACITY]))
    repaired = _repair_symbol_blockers(
        visible, entries, general_frequencies, prefix_length
    )
    return (
        [index for _, index in repaired]
        + ranked[VISIBLE_CANDIDATE_CAPACITY:]
    )


def apply_ranking_overrides(
    prefix: bytes,
    ranked: Sequence[int],
    entries: Sequence[DictionaryEntry],
    overrides: Dict[bytes, List[RankingOverride]],
) -> Tuple[List[int], bool]:
    selected = overrides.get(prefix)
    if not selected:
        return list(ranked), False

    entry_indexes = []
    for item in selected:
        matches = [
            entry_index
            for entry_index in ranked
            if entries[entry_index][0] == item.candidate_code
            and entries[entry_index][1] == item.candidate_text
        ]
        if len(matches) != 1:
            raise ValueError(
                "Wubi ranking override candidate must match exactly once: "
                f"{item.input_code.decode('ascii')} -> "
                f"{item.candidate_text.decode('utf-8')} ({item.candidate_code.decode('ascii')})"
            )
        if item.target_position >= len(ranked):
            raise ValueError("Wubi ranking override target_position exceeds candidate count")
        entry_indexes.append(matches[0])

    moved = set(entry_indexes)
    result = [entry_index for entry_index in ranked if entry_index not in moved]
    for item, entry_index in zip(selected, entry_indexes):
        result.insert(item.target_position, entry_index)
    if result == list(ranked):
        raise ValueError(
            "Wubi ranking override has no effect: "
            f"{prefix.decode('ascii')}"
        )
    return result, True


def validate_ranking_override_coverage(
    overrides: Dict[bytes, List[RankingOverride]],
    applied_prefixes: Set[bytes],
) -> None:
    missing = sorted(set(overrides) - applied_prefixes)
    if missing:
        raise ValueError(
            "Wubi ranking overrides did not match dictionary prefixes: "
            + ", ".join(prefix.decode("ascii") for prefix in missing)
        )


def audit_ranking_change(
    source_ranking: Sequence[int],
    ranked: Sequence[int],
    entries: Sequence[DictionaryEntry],
    prefix_length: int,
    general_frequencies: Dict[bytes, int],
    audit: RankingAudit,
    ranking_override_applied: bool = False,
) -> None:
    audit.prefix_count += 1
    if prefix_length == 3:
        audit.three_code_prefixes += 1
    elif prefix_length == 4:
        audit.four_code_prefixes += 1
    source_visible = source_ranking[:VISIBLE_CANDIDATE_CAPACITY]
    ranked_visible = ranked[:VISIBLE_CANDIDATE_CAPACITY]
    symbol_top_promoted = False
    symbol_repair_applied = False
    changed = list(source_ranking) != list(ranked)
    if changed and not ranking_override_applied:
        corpus_ranked = _rerank_corpus_candidates(
            source_ranking, entries, prefix_length, general_frequencies
        )
        before_symbols = list(
            enumerate(corpus_ranked[:VISIBLE_CANDIDATE_CAPACITY])
        )
        repaired = _repair_symbol_blockers(
            before_symbols, entries, general_frequencies, prefix_length
        )
        expected = (
            [index for _, index in repaired]
            + corpus_ranked[VISIBLE_CANDIDATE_CAPACITY:]
        )
        symbol_repair_applied = (
            repaired != before_symbols and expected == list(ranked)
        )
        if symbol_repair_applied:
            audit.symbol_order_repairs += 1
            symbol_top_promoted = (
                corpus_ranked[0] == source_ranking[0]
                and ranked[0] != source_ranking[0]
            )
    if prefix_length <= 2 and changed:
        audit.short_prefix_changes += 1
        if symbol_repair_applied:
            audit.short_symbol_repairs += 1
    if set(source_visible) != set(ranked_visible):
        audit.visible_set_changes += 1
        if ranking_override_applied:
            audit.ranking_override_visible_set_changes += 1
    if ranking_override_applied:
        audit.ranking_override_changes += 1
    if list(source_visible) != list(ranked_visible):
        audit.visible_order_changes += 1
        source_positions = {
            entry_index: position
            for position, entry_index in enumerate(source_visible)
        }
        ranked_positions = {
            entry_index: position
            for position, entry_index in enumerate(ranked_visible)
        }
        common_entries = set(source_positions) & set(ranked_positions)
        audit.visible_position_moves += sum(
            abs(source_positions[entry_index] - ranked_positions[entry_index])
            for entry_index in common_entries
        )
        for right_position, right_entry in enumerate(ranked_visible):
            right_source_position = source_positions.get(right_entry)
            if right_source_position is None:
                try:
                    right_source_position = source_ranking.index(right_entry)
                except ValueError:
                    continue
            right_frequency = general_frequencies.get(entries[right_entry][1], 0)
            for left_position, left_entry in enumerate(source_visible):
                if left_position >= right_source_position:
                    continue
                left_ranked_position = ranked_positions.get(left_entry)
                if (
                    (
                        left_ranked_position is None
                        or left_ranked_position > right_position
                    )
                    and right_frequency
                    < general_frequencies.get(entries[left_entry][1], 0)
                ):
                    audit.internal_frequency_regressions += 1
    if ranking_override_applied:
        if source_ranking and ranked and source_ranking[0] != ranked[0]:
            if prefix_length == 3:
                audit.three_code_top_changes += 1
            elif prefix_length == 4:
                audit.four_code_top_changes += 1
        return
    if not source_ranking or not ranked or source_ranking[0] == ranked[0]:
        return

    source_code, source_text, _ = entries[source_ranking[0]]
    _, ranked_text, _ = entries[ranked[0]]
    source_frequency = general_frequencies.get(source_text, 0)
    ranked_frequency = general_frequencies.get(ranked_text, 0)

    if prefix_length == 3:
        audit.three_code_top_changes += 1
        if len(source_code) == prefix_length:
            source_exact = [
                entry_index
                for entry_index in source_visible
                if len(entries[entry_index][0]) == prefix_length
            ]
            valid_unknown_exact_demotion = (
                source_frequency == 0
                and all(
                    general_frequencies.get(entries[entry_index][1], 0) == 0
                    and _is_han_text(entries[entry_index][1])
                    for entry_index in source_exact
                )
                and len(entries[ranked[0]][0]) > prefix_length
                and _text_length(ranked_text) <= MAX_PROMOTED_COMPLETION_LENGTH
                and ranked_frequency >= MIN_RELIABLE_GENERAL_FREQUENCY
                and list(ranked_visible[1 : 1 + len(source_exact)]) == source_exact
            )
            if valid_unknown_exact_demotion:
                audit.unknown_exact_demotions += 1
                return
            audit.unsafe_top_changes += 1
            return
        repaired = _repair_rare_single_character_blockers(
            list(enumerate(source_visible)), entries, general_frequencies
        )
        if repaired is not None and repaired[0][1] == ranked[0]:
            audit.blocked_completion_promotions += 1
            return
    elif prefix_length == 4:
        audit.four_code_top_changes += 1
        if symbol_top_promoted:
            audit.symbol_top_promotions += 1
            return
        visible = list(enumerate(source_visible))
        safe_promotion = _safe_four_code_promotion(
            visible, entries, general_frequencies
        )
        if safe_promotion is not None and safe_promotion[1] == ranked[0]:
            audit.safe_four_code_promotions += 1
        else:
            audit.unsafe_top_changes += 1
            return

    source_length = _text_length(source_text)
    ranked_length = _text_length(ranked_text)
    if ranked_length > source_length or ranked_frequency < source_frequency:
        audit.unsafe_top_changes += 1
