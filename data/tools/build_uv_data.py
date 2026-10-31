#!/usr/bin/env python3
"""Build data/uv_chars.tsv, the character table of U mode (docs/uv-mode.md).

One line a character: its stroke codes (h s p n z), readings (with and without tones), a
frequency and how it splits into components. Sources, downloaded separately (pinned below):

  stroke.dict.yaml  https://github.com/rime/rime-stroke (LGPL-3.0)
  chaizi-jt.txt     https://github.com/kfcd/chaizi (CC BY 3.0)
  pinyin.txt        https://github.com/mozillazg/pinyin-data (MIT)

and the frequencies of single characters in the runtime pinyin dictionary (pinyin.dict.db).

  python data/tools/build_uv_data.py SOURCE_DIR data/pinyin.dict.db data/uv_chars.tsv
"""

import argparse
import collections
import hashlib
from pathlib import Path
import sqlite3
import unicodedata

SOURCES = {
    "stroke.dict.yaml": (
        "https://raw.githubusercontent.com/rime/rime-stroke/1e8fff9b9494ddec23b0cbc526bcfd8171a6fd48/stroke.dict.yaml",
        "b3e93dce89c185f45c3d6e189b86b3a8626913352cc85e1094c786579a665791",
    ),
    "chaizi-jt.txt": (
        "https://raw.githubusercontent.com/kfcd/chaizi/e177ab54ce4edf255315a60593ffecbd8167e96b/chaizi-jt.txt",
        "c341a99fc6a4a4f3169279c451ca55057a22603a5d7a520094378f65e817ad26",
    ),
    "pinyin.txt": (
        "https://raw.githubusercontent.com/mozillazg/pinyin-data/9d05c9e29cb6a7e5c79f2f5279f5c1014246a19a/pinyin.txt",
        "90c11bd680ccfe7f2617a6e25abdb1fb87b5540df3e6f011ca50aaf3c7263a89",
    ),
}

# Names read for components that are not characters of their own (or are read otherwise when
# naming a part): 氵 is 三点水, typed "shui".
COMPONENT_NAMES = {
    "氵": "shuǐ", "冫": "bīng", "扌": "shǒu", "亻": "rén", "彳": "chì", "艹": "cǎo", "⺮": "zhú",
    "讠": "yán", "訁": "yán", "钅": "jīn", "釒": "jīn", "饣": "shí", "飠": "shí", "纟": "sī",
    "糹": "sī", "忄": "xīn", "㣺": "xīn", "阝": "fù,ěr", "刂": "dāo", "礻": "shì", "衤": "yī",
    "犭": "quǎn", "辶": "chuò", "廴": "yǐn", "冖": "mì", "宀": "mián", "疒": "bìng", "攵": "wén",
    "灬": "huǒ", "罒": "wǎng", "勹": "bāo", "卩": "jié", "丨": "shù", "丿": "piě", "丶": "diǎn",
    "乛": "zhé", "亠": "tóu", "丷": "bā", "厶": "sī", "囗": "wéi", "癶": "bō", "爫": "zhǎo",
    "覀": "xī", "耂": "lǎo", "虍": "hǔ", "牜": "niú", "丬": "qiáng", "王": "wáng", "⺌": "xiǎo",
    "龰": "zhǐ", "𠂉": "pié", "⺈": "dāo", "𤣩": "wáng", "⻊": "zú", "𧾷": "zú", "⺶": "yáng",
    "⺤": "zhǎo", "⺊": "bǔ", "⺆": "jiōng", "⺄": "yǐ", "⺧": "niú", "⺩": "wáng", "⺪": "shū",
    "⺳": "wǎng", "⺷": "yáng", "⻌": "chuò", "⻍": "chuò", "⻏": "yì", "⻖": "fù",
}

TONELESS = str.maketrans({"ü": "v"})


def read_source(directory, name):
    url, expected = SOURCES[name]
    content = (Path(directory) / name).read_bytes()
    actual = hashlib.sha256(content).hexdigest()
    if actual != expected:
        raise SystemExit(f"{name}: SHA-256 {actual}, expected {expected} ({url})")
    return content.decode("utf-8")


def toneless(reading):
    # "lǜ" -> "lv", "shào" -> "shao"
    plain = "".join(c for c in unicodedata.normalize("NFD", reading)
                    if unicodedata.category(c) != "Mn" or c == "̈")
    return unicodedata.normalize("NFC", plain).translate(TONELESS)


def strokes_of(text):
    codes = collections.defaultdict(list)
    body = False
    for line in text.splitlines():
        if line == "...":
            body = True
            continue
        if not body or not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 2 or len(parts[0]) != 1 or not parts[1] or set(parts[1]) - set("hspnz"):
            continue
        if parts[1] not in codes[parts[0]]:
            codes[parts[0]].append(parts[1])
    return codes


def readings_of(text):
    readings = {}
    for line in text.splitlines():
        if not line.startswith("U+"):
            continue
        code, _, rest = line.partition(":")
        values = rest.split("#")[0].strip()
        char = chr(int(code[2:], 16))
        readings[char] = [r for r in values.split(",") if r]
    # The part's name first, then its readings as a character.
    for char, names in COMPONENT_NAMES.items():
        merged = names.split(",")
        merged += [r for r in readings.get(char, []) if r not in merged]
        readings[char] = merged
    return readings


def splits_of(text):
    splits = {}
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or len(parts[0]) != 1:
            continue
        found = []
        for split in parts[1:]:
            components = split.split()
            if len(components) >= 2 and components not in found:
                found.append(components)
        if found:
            splits[parts[0]] = found
    return splits


def frequencies_of(db):
    connection = sqlite3.connect(db)
    try:
        rows = connection.execute(
            "SELECT text, MAX(frequency) FROM dict WHERE length(text) = 1 GROUP BY text")
        return {text: frequency for text, frequency in rows}
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sources", help="folder with stroke.dict.yaml, chaizi-jt.txt, pinyin.txt")
    parser.add_argument("dictionary", help="the runtime pinyin dictionary, pinyin.dict.db")
    parser.add_argument("output")
    args = parser.parse_args()

    strokes = strokes_of(read_source(args.sources, "stroke.dict.yaml"))
    readings = readings_of(read_source(args.sources, "pinyin.txt"))
    splits = splits_of(read_source(args.sources, "chaizi-jt.txt"))
    frequencies = frequencies_of(args.dictionary)

    # Every character with strokes, and the components of the splits.
    chars = set(strokes)
    for components in splits.values():
        for split in components:
            chars.update(split)
    lines = [
        "# U mode characters (docs/uv-mode.md), generated by data/tools/build_uv_data.py.",
        "# char<TAB>strokes<TAB>readings<TAB>toneless readings<TAB>frequency<TAB>splits",
        "# Strokes: rime-stroke (LGPL-3.0), " + SOURCES["stroke.dict.yaml"][0],
        "# Splits: 漢語拆字字典 by 開放詞典網 (CC BY 3.0), " + SOURCES["chaizi-jt.txt"][0],
        "# Readings: pinyin-data (MIT), " + SOURCES["pinyin.txt"][0],
    ]
    for char in sorted(chars, key=ord):
        toned = readings.get(char, [])
        plain = []
        for reading in toned:
            value = toneless(reading)
            if value not in plain:
                plain.append(value)
        lines.append("\t".join([
            char,
            ",".join(strokes.get(char, [])),
            ",".join(toned),
            ",".join(plain),
            str(frequencies.get(char, 0)),
            ";".join(" ".join(split) for split in splits.get(char, [])),
        ]))
    Path(args.output).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{args.output}: {len(lines) - 5} characters")


if __name__ == "__main__":
    main()
