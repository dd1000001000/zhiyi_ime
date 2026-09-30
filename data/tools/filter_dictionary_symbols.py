#!/usr/bin/env python3
"""Filter system symbols from source copies before building runtime files."""

import argparse
import json
from pathlib import Path
import shutil
import sqlite3
import tempfile

from dict_builder.source_archive import copy_database
from dict_builder.symbol_policy import SymbolPolicy

# Legacy Wubi extension codes include letters and radicals, not only P/S.
# Keep this source-specific rule separate from the independent symbol catalog.
WUBI_EXTENSION_CODES = frozenset((
    "cobd", "coy", "coys", "cof", "coks", "cod", "cods", "coo", "coos",
    "codl", "coxl", "cosx", "coj", "cojt", "coxx", "codx", "coxe", "code",
    "copj", "cojp", "coap", "cocm", "cokg", "cosc", "cohc", "cocc", "codw",
    "cohb", "cots", "cooy", "copy", "copa", "cope", "copi", "copo", "copu",
    "copv", "copp",
))
ORDINARY_CO_CODES = frozenset(("coaw", "cogw", "coqh"))


def filter_database(path, policy, wubi=False):
    """Filter only a caller-owned temporary DB; never populate the catalog."""
    connection = sqlite3.connect(path)
    removed = []
    stats = {
        "input_rows": 0,
        "extension_rows": 0,
        "emoji_rows": 0,
        "symbol_rows": 0,
    }
    try:
        for row_id, text, code in connection.execute(
            "SELECT id, text, code FROM dict ORDER BY id"
        ):
            stats["input_rows"] += 1
            if wubi and code in WUBI_EXTENSION_CODES:
                kind = "extension"
            else:
                if (wubi and code.startswith("co")
                        and code not in ORDINARY_CO_CODES):
                    raise ValueError(f"unreviewed upstream co* code: {code}")
                kind = policy.classify(text)
            if kind is not None:
                stats[kind + "_rows"] += 1
                removed.append((row_id,))
        if removed:
            cursor = connection.executemany(
                "DELETE FROM dict WHERE id = ?", removed,
            )
            if cursor.rowcount != len(removed):
                raise RuntimeError("symbol row count mismatch")
            connection.commit()
            connection.execute("VACUUM")
    finally:
        connection.close()
    return stats


def prepare_filtered_sources(wubi_input, pinyin_input,
                             wubi_output, pinyin_output):
    inputs = {Path(path).resolve() for path in (wubi_input, pinyin_input)}
    outputs = [Path(path).resolve() for path in (wubi_output, pinyin_output)]
    if inputs.intersection(outputs):
        raise ValueError("outputs must not overwrite dictionary sources")
    if len(set(outputs)) != len(outputs):
        raise ValueError("filtered outputs must use different paths")
    if any(path.suffix.lower() == ".zip" for path in outputs):
        raise ValueError("filtered outputs must be SQLite databases")
    policy = SymbolPolicy()
    with tempfile.TemporaryDirectory(prefix="cxxime-symbol-filter-") as temp:
        stats = {}
        prepared = []
        for name, source in (("wubi86", wubi_input), ("pinyin", pinyin_input)):
            database = Path(temp) / (name + ".dict.db")
            copy_database(str(source), str(database))
            stats[name] = filter_database(
                database, policy, wubi=name == "wubi86",
            )
            prepared.append(database)
        # Do not publish either source until both have been validated.
        for source, output in zip(prepared, outputs):
            shutil.copy2(source, output)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wubi-input", required=True)
    parser.add_argument("--pinyin-input", required=True)
    parser.add_argument("--wubi-output", required=True)
    parser.add_argument("--pinyin-output", required=True)
    args = parser.parse_args()
    stats = prepare_filtered_sources(
        args.wubi_input, args.pinyin_input,
        args.wubi_output, args.pinyin_output,
    )
    print(json.dumps(stats, indent=2))


if __name__ == "__main__":
    main()
