#!/usr/bin/env python3
"""Repair the syllables of the rime-ice pinyin dictionary.

The SQLite source was converted from rime-ice with the syllable spaces stripped ("xi an" ->
"xian") and the syllables re-derived by greedy longest match, so about 15k words carry wrong
syllables (西安 "xian", 办公室 "bang:o:n:g:shi"). The rime-ice tencent words have no pinyin
column at all; their weight landed in the code column ("西安站", code "100").

This pass rebuilds the syllables the way Rime reads its dictionaries:
  - a word with a code is re-segmented so that each character takes one of its own readings;
  - a word without a code is encoded from known words it contains (银行卡 takes 银行 "yin:hang")
    and, for the remaining characters, every reading that carries at least 5% of the character's
    weight (Rime's word-making rule), at most kMaxEncodings combinations;
  - tencent words are kept only with 3 to 4 characters, at rime-ice's default weight.

Usage: python repair_pinyin_dictionary.py <pinyin.dict.db>
"""

import itertools
import re
import sqlite3
import sys
from collections import defaultdict

HAN_CHARS = "〇㐀-䶿一-鿿\U00020000-\U0003134f"
HAN = re.compile(f"^[{HAN_CHARS}]+$")
# Names written with separators (比尔·盖茨, 特里斯坦-达库尼亚): the separators carry no syllable.
SEPARATORS = re.compile(r"[·・•\-－—]")
SEPARATED_HAN = re.compile(f"^[{HAN_CHARS}·・•\\-－—]+$")
UNENCODED_CODE = re.compile(r"^[0-9]+$")
MIN_READING_SHARE = 0.05  # librime EntryCollector::TranslateWord
MAX_ENCODINGS = 4
UNENCODED_MIN_CHARS = 3
UNENCODED_MAX_CHARS = 4
UNENCODED_WEIGHT = 100  # rime-ice's default weight; the tencent rows carry it as their code
MAX_KNOWN_WORD_CHARS = 4


def load_readings(db):
    """Single characters: {char: {syllable: weight}}. A character's code is one syllable, even
    when the greedy split broke a rare one apart (覅 fiao -> "f:i:ao", 𰻝 biang -> "bian:g")."""
    readings = defaultdict(dict)
    for text, code, weight in db.execute(
            "SELECT text, code, frequency FROM dict WHERE length(text) = 1"):
        if code and not UNENCODED_CODE.match(code):
            readings[text][code] = max(weight, readings[text].get(code, 0))
    return readings


def segment(text, code, readings):
    """All ways to split code so that each character takes one of its readings."""
    results = []

    def walk(i, pos, acc):
        if len(results) > 16:
            return
        if i == len(text):
            if pos == len(code):
                results.append(list(acc))
            return
        for syllable in readings.get(text[i], ()):
            if code.startswith(syllable, pos):
                acc.append(syllable)
                walk(i + 1, pos + len(syllable), acc)
                acc.pop()

    walk(0, 0, [])
    return results


def reading_score(text, syllables, readings):
    score = 0.0
    for ch, syllable in zip(text, syllables):
        table = readings[ch]
        total = sum(table.values()) or 1
        score += table.get(syllable, 0) / total
    return score


def word_making_readings(ch, readings):
    table = readings.get(ch)
    if not table:
        return []
    total = sum(table.values())
    if total <= 0:
        return sorted(table)
    return sorted(s for s, w in table.items() if w >= total * MIN_READING_SHARE)


def encode(text, readings, known):
    """Encodings of a word without pinyin: known words first, then character readings."""
    n = len(text)
    # best[i]: (characters covered by known words, -pieces, pieces) for text[:i]
    best = [None] * (n + 1)
    best[0] = (0, 0, [])
    for i in range(n):
        if best[i] is None:
            continue
        covered, neg_pieces, pieces = best[i]
        options = []
        for j in range(min(n, i + MAX_KNOWN_WORD_CHARS), i + 1, -1):
            if text[i:j] in known:
                options.append((j, j - i, [known[text[i:j]]]))
        chars = word_making_readings(text[i], readings)
        if chars:
            options.append((i + 1, 0, [[s] for s in chars]))
        for j, gained, choices in options:
            candidate = (covered + gained, neg_pieces - 1, pieces + [choices])
            if best[j] is None or candidate[:2] > best[j][:2]:
                best[j] = candidate
    if best[n] is None:
        return []
    encodings = []
    for combo in itertools.product(*best[n][2]):
        encodings.append([s for piece in combo for s in piece])
        if len(encodings) >= MAX_ENCODINGS:
            break
    return encodings


def repair(db_path):
    db = sqlite3.connect(db_path)
    readings = load_readings(db)
    stats = defaultdict(int)

    single = [(code, row_id) for row_id, code, syllable_ids in db.execute(
        "SELECT id, code, syllable_ids FROM dict WHERE length(text) = 1")
        if code != syllable_ids and not UNENCODED_CODE.match(code)]
    db.executemany("UPDATE dict SET syllable_ids = ? WHERE id = ?", single)
    stats["single_characters"] = len(single)

    rows = db.execute("SELECT id, text, code, frequency, syllable_ids FROM dict "
                      "WHERE length(text) > 1").fetchall()
    syllabary = {syllable for table in readings.values() for syllable in table}
    broken = []
    known = {}  # text -> syllables of the most frequent reading, for encoding
    known_weight = {}
    updates = []
    unencoded = []
    for row_id, text, code, weight, syllable_ids in rows:
        if UNENCODED_CODE.match(code):
            unencoded.append((row_id, text))
            continue
        if not SEPARATED_HAN.match(text):
            continue
        han = SEPARATORS.sub("", text)
        if not han:
            continue
        old = syllable_ids.split(":")
        options = segment(han, code, readings)
        if not options:
            # A reading no character table knows (是咯 "shi:l:o"): keep it only when its
            # syllables are real ones, so no stray "l" / "o" enters the syllabary.
            if all(syllable in syllabary for syllable in old):
                stats["unsegmentable_kept"] += 1
            else:
                broken.append((row_id,))
            continue
        if old in options:
            chosen = old
        else:
            chosen = max(options, key=lambda s: reading_score(han, s, readings))
            updates.append((":".join(chosen), row_id))
        if han == text and len(chosen) == len(text) and weight > known_weight.get(text, -1):
            known[text] = chosen
            known_weight[text] = weight
    db.executemany("UPDATE dict SET syllable_ids = ? WHERE id = ?", updates)
    db.executemany("DELETE FROM dict WHERE id = ?", broken)
    stats["resegmented"] = len(updates)
    stats["unsegmentable_dropped"] = len(broken)

    existing = set(db.execute("SELECT text, syllable_ids FROM dict WHERE length(text) > 1"))
    inserts = []
    for row_id, text in unencoded:
        if (UNENCODED_MIN_CHARS <= len(text) <= UNENCODED_MAX_CHARS and HAN.match(text)):
            encodings = encode(text, readings, known)
            if not encodings:
                stats["unencodable"] += 1
            for syllables in encodings:
                key = (text, ":".join(syllables))
                if key in existing:
                    stats["encoded_duplicate"] += 1
                    continue
                existing.add(key)
                inserts.append((text, "".join(syllables), UNENCODED_WEIGHT, key[1]))
            stats["encoded_words"] += bool(encodings)
        else:
            stats["unencoded_dropped"] += 1
    db.executemany("DELETE FROM dict WHERE id = ?", [(row_id,) for row_id, _ in unencoded])
    db.executemany("INSERT INTO dict (text, code, frequency, syllable_ids) VALUES (?, ?, ?, ?)",
                   inserts)
    stats["encoded_entries"] = len(inserts)
    db.commit()
    db.close()
    return dict(stats)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    print(repair(sys.argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
