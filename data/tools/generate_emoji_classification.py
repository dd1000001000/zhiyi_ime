#!/usr/bin/env python3
"""Freeze Unicode emoji sequences and properties for offline classification."""

import argparse
import hashlib
import json
from pathlib import Path

VERSION = "17.0"
DATA_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE_DIR = DATA_DIR / "unicode" / VERSION
DEFAULT_OUTPUT = Path(__file__).resolve().parent / "dict_builder/emoji_classification.json"
# Update these pins only after reviewing the official files for a new version.
SOURCES = (
    (
        "emoji-test.txt",
        "https://www.unicode.org/Public/17.0.0/emoji/emoji-test.txt",
        "1d8a944f88d7952f7ef7c5167fef3c67995bcae24543949710231b03a201acda",
    ),
    (
        "emoji-data.txt",
        "https://www.unicode.org/Public/17.0.0/ucd/emoji/emoji-data.txt",
        "2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b",
    ),
    (
        "UnicodeData.txt",
        "https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt",
        "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c",
    ),
)


def verify_source(content, source):
    name, _, expected = source
    actual = hashlib.sha256(content).hexdigest()
    if actual != expected:
        raise ValueError(
            f"SHA-256 mismatch for {name}: expected {expected}, got {actual}"
        )


def ranges(values):
    result = []
    for value in sorted(values):
        if result and value == result[-1][1] + 1:
            result[-1][1] = value
        else:
            result.append([value, value])
    return [
        f"{lo:X}" if lo == hi else f"{lo:X}..{hi:X}"
        for lo, hi in result
    ]


def generate(emoji_test, emoji_data, unicode_data, output):
    inputs = [Path(emoji_test), Path(emoji_data), Path(unicode_data)]
    if Path(output).resolve() in {path.resolve() for path in inputs}:
        raise ValueError("output must not overwrite Unicode sources")
    contents = []
    for path, source in zip(inputs, SOURCES):
        content = path.read_bytes()
        verify_source(content, source)
        contents.append(content.decode("utf-8"))
    if any(f"# Version: {VERSION}\n" not in text for text in contents[:2]):
        raise ValueError(f"expected Unicode emoji {VERSION} input files")

    groups = {}
    group = None
    for line in contents[0].splitlines():
        if line.startswith("# group: "):
            group = line.split(": ", 1)[1]
            groups[group] = []
        if line and not line.startswith("#"):
            fields = line.split("#", 1)[0]
            sequence = fields.split(";", 1)[0].strip()
            groups[group].append(sequence)

    properties = {
        name: set()
        for name in (
            "Emoji",
            "Emoji_Presentation",
            "Extended_Pictographic",
        )
    }
    for line in contents[1].splitlines():
        fields = line.split("#", 1)[0].strip()
        if not fields:
            continue
        span, name = (field.strip() for field in fields.split(";"))
        if name in properties:
            bounds = span.split("..")
            properties[name].update(
                range(
                    int(bounds[0], 16),
                    int(bounds[-1], 16) + 1,
                )
            )
    # UnicodeData has no version header; the pinned hash verifies its version.
    properties["Punctuation_Or_Symbol"] = {
        int(fields[0], 16)
        for fields in (line.split(";") for line in contents[2].splitlines())
        if fields[2][0] in "PS"
    }

    metadata = {
        "schema": 1,
        "unicode_version": VERSION,
        "license": "Unicode-3.0",
        "sources": [
            {
                "url": url,
                "sha256": digest,
            }
            for _, url, digest in SOURCES
        ],
    }
    # Wrap generated sequence data; do not depend on locale or Unicode tables.
    with Path(output).open("w", encoding="utf-8", newline="\n") as stream:
        header = json.dumps(metadata, indent=4)
        stream.write(header[:-2] + ',\n    "properties": {\n')
        for section_index, section in enumerate((
            {name: ranges(values) for name, values in properties.items()},
            groups,
        )):
            for index, (name, values) in enumerate(section.items()):
                stream.write(f"        {json.dumps(name)}: [\n")
                line = "            "
                for value_index, value in enumerate(values):
                    token = json.dumps(value)
                    if value_index + 1 < len(values):
                        token += ","
                    if len(line) + len(token) + 1 > 96:
                        stream.write(line.rstrip() + "\n")
                        line = "            "
                    line += token + " "
                if line.strip():
                    stream.write(line.rstrip() + "\n")
                suffix = "," if index + 1 < len(section) else ""
                stream.write("        ]" + suffix + "\n")
            if section_index == 0:
                stream.write('    },\n    "groups": {\n')
        stream.write("    }\n}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=DEFAULT_SOURCE_DIR,
        help=f"pinned Unicode input directory (default: {DEFAULT_SOURCE_DIR})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"generated classification JSON (default: {DEFAULT_OUTPUT})",
    )
    args = parser.parse_args()
    try:
        inputs = [args.source_dir / name for name, _, _ in SOURCES]
        generate(*inputs, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Generated {args.output}")


if __name__ == "__main__":
    main()
