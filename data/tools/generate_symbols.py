#!/usr/bin/env python3
"""Generate the runtime symbol table from the reviewed independent catalog."""

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re

CODEPOINTS = re.compile(r"[0-9A-F]{4,6}(?: [0-9A-F]{4,6})*\Z")
CATEGORY_CODE = re.compile(r"[a-z]{2}\Z")
CANDIDATES_PER_LINE = 10

UNICODE_DIR = Path(__file__).resolve().parents[1] / "unicode"
# Pinned like generate_emoji_classification.py; update only after reviewing a new version.
EMOJI_TEST = (
    UNICODE_DIR / "17.0/emoji-test.txt",
    "1d8a944f88d7952f7ef7c5167fef3c67995bcae24543949710231b03a201acda",
)
# https://raw.githubusercontent.com/unicode-org/cldr-json/48.0.0/cldr-json/
# cldr-annotations-full/annotations/zh/annotations.json
EMOJI_ANNOTATIONS = (
    UNICODE_DIR / "cldr48/annotations.zh.json",
    "8d2dfe9bf41894bd17c9c1a1906a31bf0ccc06204bf4b77c2ab2b0d8f79a5fdf",
)
# Emoji 15.0 and earlier, which the Windows 11 emoji font draws. Skin-tone variants are
# left out (the base emoji stays); flags are not cataloged, Windows draws them as letters.
EMOJI_MAX_VERSION = (15, 0)
SKIN_TONES = range(0x1F3FB, 0x1F400)
EMOJI_PER_WORD = 3
CHINESE_WORD = re.compile("[\u3400-\u9fff\U00020000-\U0003134f]+\\Z")


def read_pinned(source):
    path, expected = source
    content = path.read_bytes()
    actual = hashlib.sha256(content).hexdigest()
    if actual != expected:
        raise ValueError(f"SHA-256 mismatch for {path.name}: {actual}")
    return content.decode("utf-8")


def emoji_groups():
    """Fully-qualified emoji per emoji-test.txt group, in Unicode's order."""
    groups, group = {}, None
    for line in read_pinned(EMOJI_TEST).splitlines():
        if line.startswith("# group: "):
            group = groups.setdefault(line.split(": ", 1)[1], [])
            continue
        match = re.match(
            r"([0-9A-F ]+?)\s*; fully-qualified\s*# \S+ E(\d+)\.(\d+) ", line,
        )
        if not match:
            continue
        sequence = match.group(1)
        points = [int(cp, 16) for cp in sequence.split()]
        version = (int(match.group(2)), int(match.group(3)))
        if version <= EMOJI_MAX_VERSION and not any(cp in SKIN_TONES for cp in points):
            group.append(sequence)
    return groups


def expand_group(group):
    """Expand one ordered group without inferring Unicode block membership."""
    if (
        not isinstance(group, dict)
        or not isinstance(group.get("name"), str)
        or not group["name"].strip()
    ):
        raise ValueError("symbol group needs a descriptive name")
    if set(group) == {"name", "range"}:
        bounds = group["range"]
        if (
            not isinstance(bounds, list)
            or len(bounds) != 2
            or any(
                not isinstance(cp, str)
                or not re.fullmatch(r"[0-9A-F]{4,6}", cp)
                for cp in bounds
            )
        ):
            raise ValueError("symbol range needs two codepoints")
        first, last = (int(cp, 16) for cp in bounds)
        if (
            first > last
            or last > 0x10FFFF
            or first <= 0xDFFF and last >= 0xD800
        ):
            raise ValueError("invalid symbol range")
        return (f"{cp:04X}" for cp in range(first, last + 1))
    if set(group) == {"name", "codepoints"}:
        values = group["codepoints"]
        if not isinstance(values, list) or not values:
            raise ValueError("symbol codepoints must be a nonempty list")
        return values
    if set(group) == {"name", "emoji_group"}:
        values = emoji_groups().get(group["emoji_group"])
        if not values:
            raise ValueError(f"unknown emoji group: {group['emoji_group']!r}")
        return values
    raise ValueError("symbol group needs exactly one range, codepoints or emoji_group")


def candidate_text(sequence):
    if not isinstance(sequence, str) or not CODEPOINTS.fullmatch(sequence):
        raise ValueError(f"invalid catalog entry: {sequence!r}")
    points = [int(cp, 16) for cp in sequence.split()]
    if any(cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF for cp in points):
        raise ValueError(f"invalid Unicode scalar: {sequence}")
    text = "".join(chr(cp) for cp in points)
    if "\0" in text or len(text.encode("utf-8")) >= 256:
        raise ValueError(f"unusable candidate: {sequence}")
    return text


def load_catalog(path):
    root = json.loads(Path(path).read_text(encoding="utf-8"))
    if root.get("schema") != 1 or root.get("unicode_version") != "17.0":
        raise ValueError("unsupported symbol catalog version")
    if not root.get("sources") or not root.get("categories"):
        raise ValueError("symbol catalog needs sources and categories")
    codes, categories = set(), []
    for category in root["categories"]:
        code, name = category["code"], category["name"]
        if not CATEGORY_CODE.fullmatch(code) or code in codes or not name:
            raise ValueError(f"invalid or duplicate category: {code!r}")
        codes.add(code)
        candidates, seen = [], set()
        groups = category.get("groups")
        if not isinstance(groups, list) or not groups:
            raise ValueError(f"symbol category needs groups: {code}")
        for group in groups:
            for sequence in expand_group(group):
                text = candidate_text(sequence)
                if text in seen:
                    raise ValueError(f"duplicate candidate: {sequence}")
                seen.add(text)
                candidates.append(text)
        if not candidates:
            raise ValueError(f"empty symbol category: {code}")
        categories.append(dict(code=code, name=name, candidates=candidates))
    emoji_code = root.get("emoji_words")
    if emoji_code is not None and emoji_code not in codes:
        raise ValueError(f"emoji_words names no category: {emoji_code!r}")
    return root, categories


def emoji_words(emoji):
    """Chinese word -> up to EMOJI_PER_WORD of `emoji`, from the CLDR zh annotations.

    An emoji named by the word comes first, then one whose name contains it (shorter names
    first), then one with the word among its keywords (in Unicode's emoji order; CLDR lists
    keywords alphabetically, not by relevance). A single character, typed all the time, only
    gets emoji named with it alone or with one more character that also have it as a keyword
    (笑 gets 😄 大笑, 小 not ▪️ 黑色小方块, 好 not every emoji keyworded with it).
    """
    order = {text: index for index, text in enumerate(emoji)}
    by_bare = {text.replace("\ufe0f", ""): text for text in emoji}
    root = json.loads(read_pinned(EMOJI_ANNOTATIONS))
    hits = defaultdict(list)
    for key, entry in root["annotations"]["annotations"].items():
        text = by_bare.get(key.replace("\ufe0f", ""))
        if text is None:
            continue
        names = entry.get("tts", [])
        name = names[0] if names else ""
        keywords = set(entry.get("default", []))
        for word in keywords | set(names):
            if not CHINESE_WORD.fullmatch(word):
                continue
            if word == name:
                rank = (0, 0)
            elif len(word) > 1:
                rank = (1, len(name)) if word in name else (2, 0)
            elif word in keywords and word in name and len(name) == 2:
                rank = (1, len(name))
            else:
                continue
            hits[word].append((rank, order[text], text))
    return {
        word: [text for _, _, text in sorted(found)[:EMOJI_PER_WORD]]
        for word, found in sorted(hits.items())
    }


def generate(catalog_path, output_path):
    if Path(catalog_path).resolve() == Path(output_path).resolve():
        raise ValueError("output must not overwrite symbol catalog")
    catalog, categories = load_catalog(catalog_path)
    words = None
    if catalog.get("emoji_words"):
        words = emoji_words(next(c["candidates"] for c in categories
                                 if c["code"] == catalog["emoji_words"]))
    with Path(output_path).open("w", encoding="utf-8", newline="\n") as stream:
        stream.write('{\n    "version": 1,\n')
        stream.write('    "unicode_version": "17.0",\n')
        stream.write('    "sources": ')
        stream.write(json.dumps(catalog["sources"], ensure_ascii=False))
        stream.write(',\n    "categories": [\n')
        for index, category in enumerate(categories):
            suffix = ",\n" if index + 1 < len(categories) else "\n"
            stream.write("        {")
            for field in ("code", "name"):
                value = json.dumps(category[field], ensure_ascii=False)
                stream.write(f'"{field}": {value}, ')
            stream.write('"candidates": [\n')
            candidates = category["candidates"]
            for offset in range(0, len(candidates), CANDIDATES_PER_LINE):
                chunk = candidates[offset:offset + CANDIDATES_PER_LINE]
                stream.write("            ")
                stream.write(", ".join(
                    json.dumps(value, ensure_ascii=False) for value in chunk
                ))
                stream.write(
                    ",\n" if offset + len(chunk) < len(candidates) else "\n"
                )
            stream.write("        ]}" + suffix)
        stream.write("    ]")
        if words is not None:
            stream.write(',\n    "emoji_words": {\n')
            for index, (word, texts) in enumerate(words.items()):
                stream.write(f"        {json.dumps(word, ensure_ascii=False)}: ")
                stream.write(json.dumps(texts, ensure_ascii=False))
                stream.write(",\n" if index + 1 < len(words) else "\n")
            stream.write("    }")
        stream.write("\n}\n")
    return categories


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    categories = generate(args.catalog, args.output)
    count = sum(len(category["candidates"]) for category in categories)
    print(f"Generated {len(categories)} categories, {count} candidates")


if __name__ == "__main__":
    main()
