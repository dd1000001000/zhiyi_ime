#!/usr/bin/env python3
# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
# Build the Pinyin CXTOPN v2 intermediate consumed by topn_builder.
#
# This script owns key generation and ranking. Runtime packages must convert its
# output to the shared-candidate runtime format before writing dictionary_manifest.json.

import argparse
import functools
import math
import os
import sqlite3
import struct
import sys
import zipfile
from collections import defaultdict

# Binary format constants (must match short_code_cache_format.h)
TOPN_MAGIC = b"CXTOPN\x02\x00"
HEADER_FMT = "<8sIIIIIII"  # 36 bytes
HEADER_SIZE = struct.calcsize(HEADER_FMT)
KEY_FMT = "<IIIHH"         # 16 bytes (candidate_offset, candidate_count, key_offset, key_len, flags)
KEY_SIZE = struct.calcsize(KEY_FMT)
# 24 bytes: text_off, text_len, syllables_off, syllables_len, frequency, score.
CAND_FMT = "<IIIIii"
CAND_SIZE = struct.calcsize(CAND_FMT)

MAX_MATERIALIZED_PREFIX_LENGTH = 6
MAX_MIXED_KEY_LENGTH = 16
MAX_MIXED_KEYS_PER_ENTRY = 8
MAX_CANDIDATES_PER_KEY = 64

# Ranking, as Rime ranks phrases. Three groups:
#   COMPLETE    every syllable of the word is matched, each in full or by its initial;
#   COMPLETION  the last syllable is unfinished ("ni" -> 年), or an initial is used although the
#               key also reads as whole syllables ("zhou" -> 最后 as z+hou): Rime only completes
#               input it cannot syllabify and drops abbreviations when whole syllables fit;
#   EXTENSION   the word is longer than the key (先占领 for "xianzhan").
# Within a group the score is ln(frequency) plus the spelling credibility, ln(0.5) for each
# abbreviated or unfinished syllable ("qianm": 前面 before 千米 by frequency).
# User dictionary matches start at 120,000,000 and stay above every group.
# Keep in sync with rank_fallback_candidate (engine/src/pinyin_translator.cc).
COMPLETE_BASE = 60_000_000
COMPLETION_BASE = 45_000_000
EXTENSION_BASE = 30_000_000
LOG_SCALE = 500_000
LOG_OFFSET = 10.0                # ln-scores from -10 to 19.9 stay inside each group
MAX_LOG_SCORE = 14_999_999
ABBREVIATION_CREDIBILITY = -0.6931471805599453  # ln(0.5), as the abbrev spellings
# A character the dictionary has hardly seen used (frequency up to 100) goes below every word,
# at a hundredth of its log score (under 150,000).
RARE_CHARACTER_FREQUENCY = 100
RARE_CHARACTER_SCALE = 100
COMPLETION_CREDIBILITY = -2.995732273553991    # ln(0.05), as Rime reads a completed syllable
# Erhua: a trailing r after a syllable spells 儿 in full (花儿 "huar", 哪儿 "nar"); the r-initial
# words (华人, 纳入) then read the key as whole syllables with an initial and rank as completions.
ERHUA_CREDIBILITY = 0.0

# Key flags
SHORT_KEY_EXACT = 0x01
SHORT_KEY_ABBR = 0x02
SHORT_KEY_MIXED = 0x04
SHORT_KEY_PREFIX = 0x08
SHORT_KEY_PREFIX_COMPLETE = 0x10


def resolve_input(path):
    """Resolve input path, auto-extracting .zip if needed."""
    # Check .zip/.db.zip first — must extract before returning .db path
    if path.endswith(".db.zip") and os.path.isfile(path):
        extract_dir = os.path.dirname(path) or "."
        with zipfile.ZipFile(path) as zf:
            zf.extractall(extract_dir)
        db_path = path[:-4]  # remove .zip -> .db
        if os.path.isfile(db_path):
            return db_path
    if path.endswith(".zip") and os.path.isfile(path):
        extract_dir = os.path.dirname(path) or "."
        with zipfile.ZipFile(path) as zf:
            zf.extractall(extract_dir)
        db_path = path[:-4]  # remove .zip
        if os.path.isfile(db_path):
            return db_path
    if os.path.isfile(path):
        return path
    return path


def key_quality(key, syllables, erhua=False):
    """(complete, unfinished, credibility) of reading key as the start of these syllables, or None.

    Each syllable is typed in full (credibility 0), as its initial (z / zh, ln 0.5) or, for the
    last typed one, as an unfinished prefix longer than its initial (ln 0.5). complete: every
    syllable is touched; unfinished: the last one is cut. Best: complete, finished, credible.
    erhua: the word ends in 儿, whose syllable "er" is also typed as "r".
    """
    best = None
    n = len(syllables)
    frontier = {(0, 0, False): 0.0}  # (syllable index, key position, unfinished) -> credibility
    while frontier:
        nxt = {}
        for (i, p, cut), cred in frontier.items():
            if p == len(key):
                if i > 0:
                    cand = (i == n, not cut, cred)
                    if best is None or cand > best:
                        best = cand
                continue
            if i == n:
                continue
            s = syllables[i]
            steps = []
            if key.startswith(s, p):
                steps.append((p + len(s), 0.0, False))
            initials = [s[:2]] if s[:2] in ("zh", "ch", "sh") else []
            initials.append(s[0])
            for ini in initials:
                if len(ini) < len(s) and key.startswith(ini, p):
                    steps.append((p + len(ini), ABBREVIATION_CREDIBILITY, False))
            rest = len(key) - p
            if 0 < rest < len(s) and s.startswith(key[p:]) and key[p:] not in initials:
                steps.append((len(key), COMPLETION_CREDIBILITY, True))
            if erhua and i == n - 1 and i > 0 and s == "er" and key[p] == "r":
                steps.append((p + 1, ERHUA_CREDIBILITY, False))
            for q, c, unfinished in steps:
                state = (i + 1, q, unfinished)
                if state not in nxt or nxt[state] < cred + c:
                    nxt[state] = cred + c
        frontier = nxt
    if best is None:
        return None
    return best[0], not best[1], best[2]


def reads_as_syllables(key, syllabary):
    """Whether the key splits into whole syllables (zhou, xian), as Rime's normal spelling path."""
    ok = [False] * (len(key) + 1)
    ok[0] = True
    for i in range(len(key)):
        if ok[i]:
            for j in range(i + 1, min(len(key), i + 6) + 1):
                if key[i:j] in syllabary:
                    ok[j] = True
            if i > 0 and key[i] == "r":  # erhua: r after a syllable spells 儿
                ok[i + 1] = True
    return ok[len(key)]


def key_score(key, syllables, frequency, key_is_syllables, rare_character=False, erhua=False):
    quality = key_quality(key, syllables, erhua)
    if quality is None:
        return None
    complete, unfinished, credibility = quality
    if rare_character:
        return round(LOG_SCALE * (credibility + LOG_OFFSET)) // RARE_CHARACTER_SCALE
    abbreviated = credibility < (COMPLETION_CREDIBILITY if unfinished else 0.0) - 1e-9
    if not complete:
        base = EXTENSION_BASE
    elif unfinished or (abbreviated and key_is_syllables):
        base = COMPLETION_BASE
    else:
        base = COMPLETE_BASE
    log_score = round(LOG_SCALE * (math.log(max(frequency, 0) + 1) + credibility + LOG_OFFSET))
    return base + min(max(log_score, 0), MAX_LOG_SCORE)


def generate_keys(syllable_ids, text, frequency, key_is_syllables=lambda key: False):
    """Generate (key, score, flags, has_complete_match) tuples for a single dict entry.

    syllable_ids: colon-separated syllable string, e.g. "shu:ru:fa"
    """
    syllables = syllable_ids.split(":")
    if not syllables or not all(syllables):
        return []

    results = {}
    n = len(syllables)
    rare_character = len(text) == 1 and frequency <= RARE_CHARACTER_FREQUENCY
    erhua = n >= 2 and syllables[-1] == "er" and text.endswith("儿")

    def offer(key, flags, has_complete_match=False):
        existing = results.get(key)
        if existing is None:
            score = key_score(key, syllables, frequency, key_is_syllables(key), rare_character,
                              erhua)
            if score is None:
                return
            results[key] = (score, flags, has_complete_match)
        else:
            results[key] = (existing[0], flags | existing[1], has_complete_match or existing[2])

    # exact_code: full concatenation, e.g. "shurufa"
    exact = "".join(syllables)
    complete_keys = [(exact, SHORT_KEY_EXACT)]

    # abbr_code: first letters, e.g. "srf"
    abbr = "".join(s[0] for s in syllables)
    if abbr != exact:
        complete_keys.append((abbr, SHORT_KEY_ABBR))

    # mixed_code: combinations of full/abbr per syllable (limit to MAX_MIXED_KEYS_PER_ENTRY)
    if n > 1:
        mixed_list = _generate_mixed(syllables)
        for m in mixed_list[:MAX_MIXED_KEYS_PER_ENTRY]:
            if m != exact and len(m) <= MAX_MIXED_KEY_LENGTH:
                complete_keys.append((m, SHORT_KEY_MIXED))

    # erhua_code: the final 儿 typed as r, e.g. "huar", "nar" (and "nr" from the mixed key "ner")
    if erhua:
        complete_keys += [(key[:-2] + "r", flags | SHORT_KEY_MIXED)
                          for key, flags in list(complete_keys)
                          if key.endswith("er") and len(key) > 2]

    for key, flags in complete_keys:
        offer(key, flags, has_complete_match=True)

    # Prefixes keep their source match type in the flags; the score comes from the key itself.
    for key, flags in complete_keys:
        max_prefix_length = min(len(key), MAX_MATERIALIZED_PREFIX_LENGTH)
        for plen in range(1, max_prefix_length + 1):
            prefix = key[:plen]
            if prefix != key:
                offer(prefix, flags | SHORT_KEY_PREFIX)

    return [
        (key, score, flags, has_complete_match)
        for key, (score, flags, has_complete_match) in results.items()
    ]


def _generate_mixed(syllables):
    """Generate mixed-code keys targeting common abbreviation patterns.

    Patterns:
      1. Enhanced initial: zh/ch/sh -> two-letter, others -> first letter  (shu:ru:fa -> shrf)
      2. Long phrase head (5+ syls): first letter of each syllable         (zhong:hua:ren:min:gong:he:guo -> zhrmghg)
      B: first syllable full + rest first letter    (shu:ru:fa -> shurf)
      C: first 2 full + rest first letter            (bei:jing:da:xue -> beijidx)
      D: first letter + second full + rest first     (bei:jing:da:xue -> bjingdx)
      E: first 2 chars + rest first letter           (shu:ru:fa -> shrf)
    """
    n = len(syllables)
    if n <= 1:
        return []

    results = set()

    # Pattern 1: Enhanced initial (声母增强简拼)
    # For zh/ch/sh syllables, use two-letter initial; otherwise first letter only.
    enhanced = ""
    for s in syllables:
        if len(s) >= 2 and s[:2] in ("zh", "ch", "sh"):
            enhanced += s[:2]
        else:
            enhanced += s[0]
    results.add(enhanced)

    # Pattern 2: Long phrase head (长词首字母码, 5+ syllables)
    if n >= 5:
        results.add("".join(s[0] for s in syllables))

    rest_first = "".join(s[0] for s in syllables[1:])

    # Mode B: first syllable full + rest first letter
    results.add(syllables[0] + rest_first)

    rest_first_from_3 = "".join(s[0] for s in syllables[2:])

    # Mode C: first 2 syllables full + rest first letter (3+ syllables)
    if n >= 3:
        results.add(syllables[0] + syllables[1] + rest_first_from_3)

    # Mode D: first letter + second syllable full + rest first letter (2+ syllables)
    results.add(syllables[0][0] + syllables[1] + rest_first_from_3)

    # Mode E: first 2 chars of first syllable + rest first letter
    if len(syllables[0]) >= 2:
        results.add(syllables[0][:2] + rest_first)

    # Remove exact (handled by exact index)
    # Keep abbr in mixed — design doc requires srf/shrf/shurf all in mixed generator
    exact = "".join(syllables)
    results.discard(exact)
    results.discard("")

    return sorted(results)


def build_cache(db_path, min_word_frequency=-1):
    """Read dict entries from SQLite and build the key -> candidates mapping."""
    conn = sqlite3.connect(db_path)
    cursor = conn.execute("SELECT text, code, frequency, syllable_ids FROM dict")

    # key -> list of (text, syllables, frequency, score, flags, has_complete_match)
    key_candidates = defaultdict(list)
    seen_keys_text = defaultdict(dict)  # key -> text -> index (for dedup)

    syllabary = {
        syllable
        for (ids,) in conn.execute("SELECT DISTINCT syllable_ids FROM dict WHERE length(text) = 1")
        for syllable in ids.split(":") if syllable
    }
    key_is_syllables = functools.lru_cache(maxsize=None)(
        lambda key: reads_as_syllables(key, syllabary))

    count = 0
    for text, code, frequency, syllable_ids in cursor:
        if not syllable_ids or not text:
            continue
        # Long-tail words (rime-ice's default weight) stay out of the index: they rank last
        # for any short key, and the runtime lookup still finds them for full pinyin.
        if len(text) > 1 and frequency <= min_word_frequency:
            continue

        keys = generate_keys(syllable_ids, text, frequency, key_is_syllables)
        for key, score, flags, has_complete_match in keys:
            if len(key) == 0:
                continue
            # One reading per word and key: the best-scoring one (银行 yin:hang over yin:xing).
            item = (text, syllable_ids, frequency, score, flags, has_complete_match)
            index = seen_keys_text[key].get(text)
            if index is None:
                seen_keys_text[key][text] = len(key_candidates[key])
                key_candidates[key].append(item)
            elif score > key_candidates[key][index][3]:
                key_candidates[key][index] = item

        count += 1
        if count % 100000 == 0:
            print(f"  Processed {count} entries...", file=sys.stderr)

    conn.close()

    # Trim each key to top MAX_CANDIDATES_PER_KEY by score
    for key in key_candidates:
        cands = key_candidates[key]
        cands.sort(key=lambda x: (-x[3], -x[2], len(x[0]), x[0]))  # score desc, freq desc, len asc, lex asc
        key_candidates[key] = cands[:MAX_CANDIDATES_PER_KEY]

    print(f"  Total entries: {count}", file=sys.stderr)
    print(f"  Unique keys: {len(key_candidates)}", file=sys.stderr)
    total_cands = sum(len(v) for v in key_candidates.values())
    print(f"  Total candidates: {total_cands}", file=sys.stderr)
    long_keys = sum(1 for key in key_candidates if len(key) > MAX_MATERIALIZED_PREFIX_LENGTH)
    long_candidates = sum(
        len(candidates)
        for key, candidates in key_candidates.items()
        if len(key) > MAX_MATERIALIZED_PREFIX_LENGTH
    )
    max_key_length = max((len(key) for key in key_candidates), default=0)
    print(
        f"  Keys beyond materialized prefixes: {long_keys}, "
        f"candidates: {long_candidates}, max length: {max_key_length}",
        file=sys.stderr,
    )

    return key_candidates


def serialize(key_candidates, output_path):
    """Write the binary cache file."""
    # Sort keys by byte order
    sorted_keys = sorted(key_candidates.keys())

    # Build string data buffer and collect entries
    strings = bytearray()
    key_entries = []   # (key_offset, key_len, flags, cand_start_idx, cand_count)
    cand_entries = []  # (text_off, text_len, syllables_off, syllables_len, freq, score)

    def intern_str(s):
        off = len(strings)
        strings.extend(s.encode("utf-8"))
        return off, len(s.encode("utf-8"))

    for key in sorted_keys:
        cands = key_candidates[key]
        if len(key.encode("utf-8")) > 0xFFFF:
            raise ValueError(f"Top-N key exceeds uint16 length: {key[:64]!r}")
        key_off, key_len = intern_str(key)

        flags = 0
        has_complete_candidate = False
        for candidate in cands:
            candidate_flags = candidate[4]
            flags |= candidate_flags
            has_complete_candidate = has_complete_candidate or candidate[5]
        # Cached prefix candidates still seed runtime lookup. A materialized short
        # key may bypass syllabification only when it also has a complete candidate.
        if len(key) <= MAX_MATERIALIZED_PREFIX_LENGTH and has_complete_candidate:
            flags |= SHORT_KEY_PREFIX_COMPLETE

        cand_start = len(cand_entries)
        for text, syllables, freq, score, _, _ in cands:
            text_off, text_len = intern_str(text)
            syllables_off, syllables_len = intern_str(syllables)
            cand_entries.append(
                (text_off, text_len, syllables_off, syllables_len, freq, score)
            )

        key_entries.append((key_off, key_len, flags, cand_start, len(cands)))

    key_count = len(key_entries)
    cand_count = len(cand_entries)
    string_data_size = len(strings)

    keys_offset = HEADER_SIZE
    cand_offset = keys_offset + key_count * KEY_SIZE
    string_offset = cand_offset + cand_count * CAND_SIZE

    with open(output_path, "wb") as f:
        # Header
        hdr = struct.pack(HEADER_FMT,
                          TOPN_MAGIC, 2, key_count, cand_count,
                          string_data_size, keys_offset, cand_offset, string_offset)
        f.write(hdr)

        # Key entries
        for key_off, key_len, flags, cand_start, cand_count_entry in key_entries:
            f.write(struct.pack(KEY_FMT, cand_start, cand_count_entry, key_off, key_len, flags))

        # Candidate entries
        for text_off, text_len, syllables_off, syllables_len, freq, score in cand_entries:
            f.write(
                struct.pack(
                    CAND_FMT,
                    text_off,
                    text_len,
                    syllables_off,
                    syllables_len,
                    freq,
                    score,
                )
            )

        # String data
        f.write(bytes(strings))

    file_size = string_offset + string_data_size
    print(f"  Written {file_size} bytes to {output_path}", file=sys.stderr)
    print(f"  Keys: {key_count}, Candidates: {cand_count}, Strings: {string_data_size}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Build the pinyin Top-N index intermediate")
    parser.add_argument("--input", required=True, help="Input .dict.db or .dict.db.zip path")
    parser.add_argument("--output", required=True, help="Output .topn.bin path")
    parser.add_argument("--no-verify", action="store_true", help="Skip required keys verification")
    parser.add_argument("--min-word-frequency", type=int, default=-1,
                        help="Leave words (2+ characters) at or below this frequency out of the index")
    args = parser.parse_args()

    db_path = resolve_input(args.input)
    if not os.path.isfile(db_path):
        print(f"ERROR: Input file not found: {db_path}", file=sys.stderr)
        sys.exit(1)

    print(f"Building Top-N index intermediate from {db_path}...", file=sys.stderr)
    key_candidates = build_cache(db_path, args.min_word_frequency)

    if not key_candidates:
        print("WARNING: No keys generated. Check input data.", file=sys.stderr)

    serialize(key_candidates, args.output)

    # Verify required keys are present (unless --no-verify)
    if args.no_verify:
        print("Done (verification skipped).", file=sys.stderr)
        return
    required_keys = ["s", "sd", "sdf", "sddf", "bj", "srf", "shrf", "zguo", "nihao"]
    missing = [k for k in required_keys if k not in key_candidates]
    if missing:
        print(f"ERROR: Missing required keys in topn.bin: {missing}", file=sys.stderr)
        sys.exit(1)
    else:
        print(f"  Verified: all {len(required_keys)} required keys present", file=sys.stderr)

    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
