"""Build the runtime English word list (data/english.words.tsv).

Sources:
  data/english/en.dict.yaml, en_ext.dict.yaml  word lists from rime-ice (GPL-3.0-only),
                                              commit recorded in data/english/RIME_ICE_COMMIT
  wordfreq (pip)                              word frequencies (data CC-BY-SA 4.0), build time only

Output (UTF-8, one entry per line, sorted by key then by descending score):
  key<TAB>text<TAB>score
    key    lowercase input code (what the user types), e.g. "losangeles", "readme.md"
    text   word as committed, case preserved, e.g. "Los Angeles", "README.md"
    score  100 * wordfreq Zipf frequency; entries wordfreq does not know get a small floor

Usage: python data/tools/build_english_dictionary.py [--output data/english.words.tsv]
"""
from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES = [("en.dict.yaml", 0), ("en_ext.dict.yaml", -50)]  # (file, score adjustment)
UNKNOWN_SCORE = 100  # Zipf 1.0: rarer than anything wordfreq lists as common


def read_rime_dict(path: str):
    in_body = False
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not in_body:
                in_body = line.strip() == "..."
                continue
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            text = parts[0].strip()
            code = parts[1].strip() if len(parts) > 1 and parts[1].strip() else text.replace(" ", "")
            if text and code:
                yield text, code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", default=os.path.join(ROOT, "data", "english.words.tsv"))
    args = parser.parse_args()
    try:
        from wordfreq import zipf_frequency
    except ImportError:
        print("ERROR: pip install wordfreq", file=sys.stderr)
        return 1

    entries: dict[tuple[str, str], int] = {}
    for name, adjust in SOURCES:
        path = os.path.join(ROOT, "data", "english", name)
        if not os.path.exists(path):
            print(f"ERROR: missing {path}", file=sys.stderr)
            return 1
        for text, code in read_rime_dict(path):
            key = code.lower()
            if not key.isascii():
                continue
            zipf = zipf_frequency(text, "en")
            score = int(round(zipf * 100)) if zipf > 0 else UNKNOWN_SCORE
            score = max(1, score + adjust)
            entries[(key, text)] = max(entries.get((key, text), 0), score)

    rows = sorted(entries.items(), key=lambda kv: (kv[0][0], -kv[1], kv[0][1]))
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        for (key, text), score in rows:
            f.write(f"{key}\t{text}\t{score}\n")
    print(f"wrote {len(rows)} entries -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
