#!/usr/bin/env python3
# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
#
# Standalone dictionary preparation pipeline.
#
# Runs the full workflow: .zip extraction -> spelling algebra -> binary build.
# Supports both pinyin and wubi86 dictionaries.
#
# Usage:
#   python scripts/prepare_dictionary_bundle.py --data-dir data/ --output-dir dist/data/ \
#       --topn-builder build/tools/topn_index/Release/topn_builder.exe

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_TOOLS = os.path.join(ROOT, "data", "tools")
WUBI_RANKING_BASELINE = os.path.join(
    DATA_TOOLS, "dict_builder", "wubi_ranking_baseline.json"
)
WUBI_RANKING_OVERRIDES = os.path.join(ROOT, "data", "wubi_ranking_overrides.json")
SCHEMAS = os.path.join(ROOT, "data", "schemas")
SCRIPTS = os.path.join(ROOT, "scripts")
sys.path.insert(0, DATA_TOOLS)

from dictionary_bundle_layout import (
    MANIFEST_FILES,
    REQUIRED_MANIFEST_ROLES,
    SHUANGPIN_SCHEME_NAMES,
)
from dict_builder import build_reverse_index
from filter_dictionary_symbols import prepare_filtered_sources
from generate_symbols import generate as generate_symbols

TOPN_RUNTIME_HEADER_FORMAT = "<8s11IQI"
TOPN_RUNTIME_HEADER_SIZE = struct.calcsize(TOPN_RUNTIME_HEADER_FORMAT)


def find_source(data_dir: str, name: str) -> str | None:
    """Find .dict.db or .dict.db.zip for a dictionary name."""
    zip_path = os.path.join(data_dir, f"{name}.dict.db.zip")
    if os.path.isfile(zip_path):
        return zip_path
    db = os.path.join(data_dir, f"{name}.dict.db")
    if os.path.isfile(db):
        return db
    return None


def run_pinyin_spelling_generation(db_path: str, schema_name: str) -> None:
    """Regenerate spellings table from schema rules."""
    script = os.path.join(DATA_TOOLS, "generate_pinyin_spellings.py")
    schema = os.path.join(SCHEMAS, schema_name)
    print(f"  Generating spellings from {schema_name}...")
    subprocess.run(
        [sys.executable, script, db_path, schema],
        check=True,
        capture_output=False,
    )


def run_build_runtime_dictionary(
    db_path: str,
    output_prefix: str,
    skip_idx: bool = False,
    spellings_only: bool = False,
    dict_only: bool = False,
    wubi_prefix_index: bool = False,
    wubi_ranking_source: str | None = None,
    wubi_ranking_baseline: str | None = None,
    wubi_ranking_overrides: str | None = None,
) -> None:
    """Convert a SQLite dictionary to runtime files."""
    script = os.path.join(DATA_TOOLS, "build_runtime_dictionary.py")
    cmd = [sys.executable, script, "--input", db_path, "--output", output_prefix]
    if spellings_only:
        cmd.append("--spellings-only")
    if dict_only:
        cmd.append("--dict-only")
    if skip_idx:
        cmd.append("--skip-idx")
    if wubi_prefix_index:
        cmd.append("--wubi-prefix-index")
        if not wubi_ranking_source:
            raise RuntimeError("Wubi prefix index requires a ranking source")
        cmd.extend(["--wubi-ranking-source", wubi_ranking_source])
        if wubi_ranking_baseline:
            cmd.extend(["--wubi-ranking-baseline", wubi_ranking_baseline])
        if wubi_ranking_overrides:
            cmd.extend(["--wubi-ranking-overrides", wubi_ranking_overrides])
    print(f"  Building binary dicts: {os.path.basename(output_prefix)}.*")
    subprocess.run(cmd, check=True, capture_output=False)


def verify_generated_text_file(generated_path: str, expected_path: str) -> None:
    """Require generated UTF-8 text to match its reviewed repository copy."""
    if not os.path.isfile(expected_path):
        raise RuntimeError(f"Expected generated file not found: {expected_path}")
    with open(generated_path, "r", encoding="utf-8", newline=None) as generated, open(
        expected_path, "r", encoding="utf-8", newline=None
    ) as expected:
        if generated.read() != expected.read():
            raise RuntimeError(
                f"Generated {os.path.basename(generated_path)} does not match "
                f"{expected_path}; regenerate the repository copy"
            )


def run_build_pinyin_topn(db_path: str, output_path: str) -> None:
    """Build the intermediate representation consumed by topn_builder."""
    script = os.path.join(SCRIPTS, "build_pinyin_topn.py")
    print("  Building Top-N index intermediate...")
    subprocess.run(
        [sys.executable, script, "--input", db_path, "--output", output_path],
        check=True,
        capture_output=False,
    )


def finalize_topn_index(output_dir: str, topn_builder: str) -> str:
    """Convert the Top-N intermediate to the shared-candidate runtime format."""
    topn_path = os.path.join(output_dir, "pinyin.topn.bin")
    dictionary_path = os.path.join(output_dir, "pinyin.dict.bin")
    if not os.path.isfile(topn_path):
        raise RuntimeError(f"Top-N intermediate not found: {topn_path}")
    if not os.path.isfile(dictionary_path):
        raise RuntimeError(f"Runtime dictionary not found: {dictionary_path}")
    if not os.path.isfile(topn_builder):
        raise RuntimeError(f"topn_builder not found: {topn_builder}")

    with open(topn_path, "rb") as f:
        header = f.read(20)
    if len(header) < 20:
        raise RuntimeError("pinyin.topn.bin is too small")

    magic = header[:8]
    if magic == b"CXTOPN\x02\x00":
        print("  Converting Top-N index to the shared-candidate format...")
        subprocess.run(
            [
                topn_builder,
                "--input",
                topn_path,
                "--output",
                topn_path,
                "--dictionary",
                dictionary_path,
            ],
            check=True,
            capture_output=False,
        )
        with open(topn_path, "rb") as f:
            header = f.read(20)
        magic = header[:8]

    if magic != b"CXTOPN\x04\x00" or len(header) < 16:
        raise RuntimeError("pinyin.topn.bin is not a CXTOPN v4 file")
    version, header_size = struct.unpack_from("<II", header, 8)
    if version != 4 or header_size != TOPN_RUNTIME_HEADER_SIZE:
        raise RuntimeError(
            "pinyin.topn.bin has an unsupported runtime layout "
            f"(version={version}, header={header_size})"
        )
    return topn_path


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write_dictionary_manifest(output_dir: str) -> str:
    """Write dictionary_manifest.json last, after all large files are stable."""
    files = []
    for role, filename in MANIFEST_FILES:
        path = os.path.join(output_dir, filename)
        if not os.path.isfile(path):
            continue
        files.append({
            "role": role,
            "path": filename,
            "size": os.path.getsize(path),
            "sha256": sha256_file(path),
            "required": True,
        })

    roles = {item["role"] for item in files}
    missing = sorted(REQUIRED_MANIFEST_ROLES - roles)
    if missing:
        raise RuntimeError(
            "Missing required dictionary manifest role(s): " + ", ".join(missing)
        )

    manifest = {
        "schema": 1,
        "generation": datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z",
        "files": files,
    }

    path = os.path.join(output_dir, "dictionary_manifest.json")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(tmp, path)
    return path


def prepare_pinyin_dictionary(db_path: str, output_dir: str) -> list[str]:
    """Prepare pinyin binary dictionary files."""
    print("--- Pinyin dictionary ---")
    generated = []
    run_pinyin_spelling_generation(db_path, "pinyin.full-pinyin.schema.json")

    output_prefix = os.path.join(output_dir, "pinyin")
    run_build_runtime_dictionary(db_path, output_prefix)
    reverse_index_path = output_prefix + ".reverse.idx"
    build_reverse_index(output_prefix + ".dict.bin", reverse_index_path)
    generated.extend([
        output_prefix + ".dict.bin",
        output_prefix + ".dict.idx",
        output_prefix + ".spellings.bin",
        reverse_index_path,
    ])

    for scheme_name in SHUANGPIN_SCHEME_NAMES:
        schema_stem = f"{scheme_name}-shuangpin"
        run_pinyin_spelling_generation(db_path, f"pinyin.{schema_stem}.schema.json")
        scheme_prefix = os.path.join(output_dir, f"pinyin.{schema_stem}")
        run_build_runtime_dictionary(db_path, scheme_prefix, spellings_only=True)
        generated.append(scheme_prefix + ".spellings.bin")

    topn_path = os.path.join(output_dir, "pinyin.topn.bin")
    run_build_pinyin_topn(db_path, topn_path)
    generated.append(topn_path)

    return generated


def prepare_wubi_dictionary(
    db_path: str, output_dir: str, ranking_source: str,
) -> list[str]:
    """Prepare wubi86 binary dictionary files."""
    print("--- Wubi86 dictionary ---")
    output_prefix = os.path.join(output_dir, "wubi86")
    run_build_runtime_dictionary(
        db_path,
        output_prefix,
        dict_only=True,
        wubi_prefix_index=True,
        wubi_ranking_source=ranking_source,
        wubi_ranking_baseline=WUBI_RANKING_BASELINE,
        wubi_ranking_overrides=WUBI_RANKING_OVERRIDES,
    )
    reverse_index_path = output_prefix + ".reverse.idx"
    build_reverse_index(output_prefix + ".dict.bin", reverse_index_path)
    return [
        output_prefix + ".dict.bin",
        output_prefix + ".dict.idx",
        reverse_index_path,
    ]


# Zhiyi IME ships a compact pinyin dictionary (about 400k entries): every single character
# plus the words ranked above rime-ice's default weight of 100. The cut words (rare names,
# places and transliterations) can still be typed character by character and are then
# remembered by self-learning.
PINYIN_MIN_WORD_FREQUENCY = 101


def trim_pinyin_dictionary(db_path: str) -> dict:
    """Drops multi-character pinyin words below PINYIN_MIN_WORD_FREQUENCY."""
    db = sqlite3.connect(db_path)
    try:
        before = db.execute("SELECT COUNT(*) FROM dict").fetchone()[0]
        db.execute(
            "DELETE FROM dict WHERE length(text) > 1 AND frequency < ?",
            (PINYIN_MIN_WORD_FREQUENCY,),
        )
        db.commit()
        after = db.execute("SELECT COUNT(*) FROM dict").fetchone()[0]
        db.execute("VACUUM")
    finally:
        db.close()
    return {"entries_before": before, "entries_after": after}


def prepare_dictionary_bundle(
    data_dir: str,
    output_dir: str,
    workers: int = 2,
    topn_builder: str | None = None,
    defer_topn_conversion: bool = False,
) -> list[str]:
    """Run dictionary preparation, using separate workers for pinyin and wubi86."""
    os.makedirs(output_dir, exist_ok=True)
    workers = max(1, min(workers, 2))
    generated = []
    pinyin_source = find_source(data_dir, "pinyin")
    wubi_source = find_source(data_dir, "wubi86")
    if pinyin_source is None or wubi_source is None:
        raise RuntimeError("pinyin and wubi86 source dictionaries are required")
    with tempfile.TemporaryDirectory(prefix="zhiyi_prep_") as tmpdir:
        pinyin_db = os.path.join(tmpdir, "pinyin.dict.db")
        wubi_db = os.path.join(tmpdir, "wubi86.dict.db")
        symbols = os.path.join(tmpdir, "symbols.json")
        generate_symbols(os.path.join(data_dir, "symbol_catalog.json"), symbols)
        verify_generated_text_file(symbols, os.path.join(data_dir, "symbols.json"))
        stats = prepare_filtered_sources(
            wubi_source, pinyin_source, wubi_db, pinyin_db,
        )
        print("Dictionary symbol filtering: " + json.dumps(stats))
        print("Pinyin dictionary trimming: " + json.dumps(trim_pinyin_dictionary(pinyin_db)))
        tasks = [
            (prepare_pinyin_dictionary, (pinyin_db, output_dir)),
            (prepare_wubi_dictionary, (wubi_db, output_dir, pinyin_source)),
        ]
        if workers == 1:
            for task, args in tasks:
                generated.extend(task(*args))
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
                futures = [executor.submit(task, *args) for task, args in tasks]
                for future in futures:
                    generated.extend(future.result())
        symbols_output = os.path.join(output_dir, "symbols.json")
        shutil.copy2(symbols, symbols_output)
        generated.append(symbols_output)

    if defer_topn_conversion:
        return generated

    if not topn_builder:
        raise RuntimeError("topn_builder is required to produce the runtime Top-N index")
    finalize_topn_index(output_dir, os.path.abspath(topn_builder))

    manifest_path = write_dictionary_manifest(output_dir)
    generated.append(manifest_path)
    return generated


def main():
    parser = argparse.ArgumentParser(
        description="Prepare dictionary binaries for ZhiyiIME packaging"
    )
    parser.add_argument(
        "--data-dir", required=True,
        help="Source data directory (contains .dict.db or .dict.db.zip)",
    )
    parser.add_argument(
        "--output-dir", required=True,
        help="Output directory for binary dictionary files",
    )
    parser.add_argument(
        "--workers", type=int, default=min(2, os.cpu_count() or 1),
        help="Dictionary worker count (default: 2, capped by available dictionaries)",
    )
    parser.add_argument(
        "--topn-builder", required=True,
        help="Path to the x64 topn_builder executable",
    )
    args = parser.parse_args()

    data_dir = os.path.abspath(args.data_dir)
    output_dir = os.path.abspath(args.output_dir)

    if not os.path.isdir(data_dir):
        print(f"ERROR: data directory not found: {data_dir}", file=sys.stderr)
        return 1
    if args.workers < 1:
        print("ERROR: --workers must be >= 1", file=sys.stderr)
        return 1

    print(f"Data source: {data_dir}")
    print(f"Output:      {output_dir}")
    print()

    try:
        generated = prepare_dictionary_bundle(
            data_dir,
            output_dir,
            workers=args.workers,
            topn_builder=args.topn_builder,
        )
    except subprocess.CalledProcessError as e:
        print(f"ERROR: subprocess failed: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    print()
    print(f"Done. {len(generated)} file(s) generated:")
    for path in generated:
        size_mb = os.path.getsize(path) / (1024 * 1024)
        print(f"  {os.path.basename(path):30s} {size_mb:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
