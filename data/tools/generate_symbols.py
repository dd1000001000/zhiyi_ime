#!/usr/bin/env python3
"""Generate the runtime symbol table from the reviewed independent catalog."""

import argparse
import json
from pathlib import Path
import re

CODEPOINTS = re.compile(r"[0-9A-F]{4,6}(?: [0-9A-F]{4,6})*\Z")
CATEGORY_CODE = re.compile(r"[a-z]{2}\Z")
CANDIDATES_PER_LINE = 10


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
    raise ValueError("symbol group needs exactly one range or codepoints")


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
    return root, categories


def generate(catalog_path, output_path):
    if Path(catalog_path).resolve() == Path(output_path).resolve():
        raise ValueError("output must not overwrite symbol catalog")
    catalog, categories = load_catalog(catalog_path)
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
        stream.write("    ]\n}\n")
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
