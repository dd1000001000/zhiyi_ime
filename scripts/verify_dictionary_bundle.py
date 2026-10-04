#!/usr/bin/env python3
# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
#
# Verify data file integrity for release packaging.
#
# Checks:
#   1. Required files exist and size > 0
#   2. Magic bytes correct for each binary file
#   3. pinyin.topn.bin contains required Top-N keys
#   4. Dictionary binaries use the current runtime formats
#   5. dictionary_manifest.json describes the complete pinyin and wubi bundle
#
# Usage: python verify_dictionary_bundle.py --data-dir <dir>
#
# Exit codes:
#   0 = all checks passed
#   1 = verification failed

import argparse
import hashlib
import json
import os
import struct
import sys

from dictionary_bundle_layout import (
    PINYIN_SPELLING_FILES,
    REQUIRED_BUNDLE_FILES,
    REQUIRED_MANIFEST_ROLES,
)
from package_checks.reverse_index import check_reverse_index

REQUIRED_FILES = REQUIRED_BUNDLE_FILES

# Sample keys whose Top-N lists must be complete. ("sddf" is not complete with the compact
# ~400k-entry dictionary; such keys take the regular query path.)
REQUIRED_TOPN_KEYS = ["s", "sd", "sdf", "bj", "srf", "shrf"]

# Magic values (first 8 bytes of each binary file)
DICT_MAGIC_V2 = b"CXDIC\x02\x00\x00"
DICT_VERSION = 2
DICT_HEADER_FORMAT = "<8sIIIII"
DICT_HEADER_SIZE = struct.calcsize(DICT_HEADER_FORMAT)
DICT_ENTRY_FORMAT = "<IIIIi"
DICT_ENTRY_SIZE = struct.calcsize(DICT_ENTRY_FORMAT)
IDX_MAGIC = b"CXIDX\x00\x00\x00"
IDX_VERSION = 3
WUBI_INDEX_MAGIC = b"CXWIDX\x01\x00"
WUBI_INDEX_HEADER_FORMAT = "<8s10I"
WUBI_INDEX_HEADER_SIZE = struct.calcsize(WUBI_INDEX_HEADER_FORMAT)
WUBI_INDEX_KEY_FORMAT = "<III"
WUBI_INDEX_KEY_SIZE = struct.calcsize(WUBI_INDEX_KEY_FORMAT)
WUBI_INDEX_MAX_CODE_LENGTH = 4
SPELLINGS_MAGIC_V2 = b"CXSPL\x02\x00\x00"
SPELLINGS_VERSION = 2
TOPN_MAGIC = b"CXTOPN\x04\x00"
TOPN_VERSION = 4
TOPN_HEADER_FORMAT = "<8s11IQI"
TOPN_HEADER_SIZE = struct.calcsize(TOPN_HEADER_FORMAT)
TOPN_POSTING_LIST_SIZE = 4
TOPN_CANDIDATE_POSTING_SIZE = 8
TOPN_POSTING_OFFSET_MASK = 0x7FFFFFFF
TOPN_POSTING_PREFIX_COMPLETE = 0x80000000


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def check_file_exists(data_dir, filename, errors):
    """Check file exists and size > 0."""
    path = os.path.join(data_dir, filename)
    if not os.path.isfile(path):
        errors.append(f"{filename}: file not found")
        return False
    size = os.path.getsize(path)
    if size == 0:
        errors.append(f"{filename}: file is empty")
        return False
    return True


def check_dict_bin(data_dir, filename, errors):
    """Validate dict.bin magic and version."""
    path = os.path.join(data_dir, filename)
    with open(path, "rb") as f:
        data = f.read(28)  # DictHeader = 28 bytes
    if len(data) < 28:
        errors.append(f"{filename}: file too small for header")
        return False
    magic = data[0:8]
    if magic != DICT_MAGIC_V2:
        errors.append(f"{filename}: bad magic {magic!r}")
        return False
    version = struct.unpack_from("<I", data, 8)[0]
    if version != DICT_VERSION:
        errors.append(f"{filename}: bad version {version}")
        return False
    return True


def check_dict_idx(data_dir, filename, errors):
    """Validate dict.idx magic and version."""
    path = os.path.join(data_dir, filename)
    with open(path, "rb") as f:
        data = f.read(28)
    if len(data) < 28:
        errors.append(f"{filename}: file too small for header")
        return False
    magic = data[0:8]
    if magic != IDX_MAGIC:
        errors.append(f"{filename}: bad magic {magic!r}")
        return False
    # Header: magic(8) version(4) syl_count(4) syl_str_size(4) idx_count(4) idx_data_size(4)
    version = struct.unpack_from("<I", data, 8)[0]
    if version != IDX_VERSION:
        errors.append(f"{filename}: bad version {version}")
        return False
    return True


def check_wubi_prefix_index(data_dir, errors):
    """Validate the complete Wubi prefix posting index."""
    filename = "wubi86.dict.idx"
    path = os.path.join(data_dir, filename)
    with open(path, "rb") as source:
        data = source.read()
    if len(data) < WUBI_INDEX_HEADER_SIZE:
        errors.append(f"{filename}: file too small for header")
        return False

    (
        magic,
        version,
        header_size,
        file_size,
        dict_entry_count,
        key_count,
        posting_count,
        keys_offset,
        postings_offset,
        max_code_length,
        reserved,
    ) = struct.unpack_from(WUBI_INDEX_HEADER_FORMAT, data, 0)
    expected_postings_offset = keys_offset + key_count * WUBI_INDEX_KEY_SIZE
    if (
        magic != WUBI_INDEX_MAGIC
        or version != 1
        or header_size != WUBI_INDEX_HEADER_SIZE
        or file_size != len(data)
        or key_count == 0
        or posting_count == 0
        or keys_offset != WUBI_INDEX_HEADER_SIZE
        or postings_offset != expected_postings_offset
        or postings_offset + posting_count * 4 != len(data)
        or max_code_length != WUBI_INDEX_MAX_CODE_LENGTH
        or reserved != 0
    ):
        errors.append(f"{filename}: invalid header or section layout")
        return False

    with open(os.path.join(data_dir, "wubi86.dict.bin"), "rb") as source:
        dict_header = source.read(16)
    if len(dict_header) < 16 or struct.unpack_from("<I", dict_header, 12)[0] != dict_entry_count:
        errors.append(f"{filename}: dictionary entry count mismatch")
        return False

    previous_code = 0
    expected_posting_offset = 0
    for index in range(key_count):
        offset = keys_offset + index * WUBI_INDEX_KEY_SIZE
        packed_code, posting_offset, count = struct.unpack_from(
            WUBI_INDEX_KEY_FORMAT, data, offset
        )
        encoded = packed_code
        code_length = 0
        valid_code = encoded != 0
        while encoded:
            character = encoded & 0x1F
            code_length += 1
            if character == 0 or character > 26 or code_length > WUBI_INDEX_MAX_CODE_LENGTH:
                valid_code = False
                break
            encoded >>= 5
        if (
            not valid_code
            or packed_code <= previous_code
            or count == 0
            or posting_offset != expected_posting_offset
            or count > posting_count - posting_offset
        ):
            errors.append(f"{filename}: invalid key record at index {index}")
            return False
        previous_code = packed_code
        expected_posting_offset += count
    if expected_posting_offset != posting_count:
        errors.append(f"{filename}: posting lists do not cover the posting section")
        return False

    for index in range(posting_count):
        entry_index = struct.unpack_from("<I", data, postings_offset + index * 4)[0]
        if entry_index >= dict_entry_count:
            errors.append(f"{filename}: invalid posting at index {index}")
            return False
    return True


def check_spellings_bin(data_dir, filename, errors):
    """Validate a Pinyin spelling trie magic and version."""
    path = os.path.join(data_dir, filename)
    with open(path, "rb") as source:
        data = source.read(12)
    if len(data) < 12:
        errors.append(f"{filename}: file too small for header")
        return False
    magic = data[:8]
    if magic != SPELLINGS_MAGIC_V2:
        errors.append(f"{filename}: bad magic {magic!r}")
        return False
    version = struct.unpack_from("<I", data, 8)[0]
    if version != SPELLINGS_VERSION:
        errors.append(f"{filename}: bad version {version}")
        return False
    return True


def darts_offset(unit):
    """Decode a darts-clone unit offset."""
    return (unit >> 10) << ((unit & (1 << 9)) >> 6)


def find_topn_key(units, unit_count, key):
    """Return the posting-list index for an exact DAT key, or None."""
    node = 0
    unit = struct.unpack_from("<I", units, 0)[0]
    for label in key.encode("ascii"):
        node ^= darts_offset(unit) ^ label
        if node >= unit_count:
            return None
        unit = struct.unpack_from("<I", units, node * 4)[0]
        if (unit & ((1 << 31) | 0xFF)) != label:
            return None
    if ((unit >> 8) & 1) == 0:
        return None
    leaf = node ^ darts_offset(unit)
    if leaf >= unit_count:
        return None
    leaf_unit = struct.unpack_from("<I", units, leaf * 4)[0]
    if (leaf_unit & (1 << 31)) == 0:
        return None
    return leaf_unit & ((1 << 31) - 1)


def check_topn_candidate_postings(source, postings_offset, posting_count,
                                  dictionary_entry_count, errors):
    """Validate dictionary references in candidate postings."""
    source.seek(postings_offset)
    posting_index = 0
    while posting_index < posting_count:
        chunk_count = min(65536, posting_count - posting_index)
        postings = source.read(chunk_count * TOPN_CANDIDATE_POSTING_SIZE)
        if len(postings) != chunk_count * TOPN_CANDIDATE_POSTING_SIZE:
            errors.append("pinyin.topn.bin: truncated postings")
            return False
        for chunk_index in range(chunk_count):
            dictionary_entry_index = struct.unpack_from(
                "<I", postings, chunk_index * TOPN_CANDIDATE_POSTING_SIZE
            )[0]
            if dictionary_entry_index >= dictionary_entry_count:
                errors.append(
                    "pinyin.topn.bin: invalid candidate posting at index "
                    f"{posting_index + chunk_index}"
                )
                return False
        posting_index += chunk_count
    return True


def dictionary_candidate_store(data_dir, errors):
    """Validate the runtime dictionary and return its canonical candidate store."""
    path = os.path.join(data_dir, "pinyin.dict.bin")
    with open(path, "rb") as source:
        data = source.read()
    if len(data) < DICT_HEADER_SIZE:
        errors.append("pinyin.dict.bin: file too small for candidate store")
        return None
    magic, version, entry_count, string_size, entries_offset, strings_offset = (
        struct.unpack_from(DICT_HEADER_FORMAT, data, 0)
    )
    entries_size = entry_count * DICT_ENTRY_SIZE
    if (
        magic != DICT_MAGIC_V2
        or version != DICT_VERSION
        or entry_count == 0
        or entries_offset != DICT_HEADER_SIZE
        or strings_offset != entries_offset + entries_size
        or strings_offset + string_size != len(data)
    ):
        errors.append("pinyin.dict.bin: invalid candidate store layout")
        return None

    for entry_index in range(entry_count):
        entry_offset = entries_offset + entry_index * DICT_ENTRY_SIZE
        syllables_offset, text_offset, syllables_length, text_length, _ = (
            struct.unpack_from(DICT_ENTRY_FORMAT, data, entry_offset)
        )
        if (
            text_length == 0
            or syllables_length == 0
            or text_offset > string_size
            or text_length > string_size - text_offset
            or syllables_offset > string_size
            or syllables_length > string_size - syllables_offset
        ):
            errors.append(
                f"pinyin.dict.bin: invalid candidate entry at index {entry_index}"
            )
            return None

    fingerprint = 14695981039346656037
    for byte in struct.pack("<II", entry_count, string_size):
        fingerprint = ((fingerprint ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    for byte in data[entries_offset:]:
        fingerprint = ((fingerprint ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return entry_count, fingerprint


def check_topn_bin(data_dir, errors):
    """Validate the runtime shared-candidate Top-N index and required keys."""
    path = os.path.join(data_dir, "pinyin.topn.bin")
    with open(path, "rb") as f:
        header_data = f.read(TOPN_HEADER_SIZE)
    if len(header_data) < TOPN_HEADER_SIZE:
        errors.append("pinyin.topn.bin: file too small for header")
        return False

    header = struct.unpack(TOPN_HEADER_FORMAT, header_data)
    (magic, version, header_size, file_size, key_count, code_index_count,
     posting_list_count, posting_count, dictionary_entry_count, code_index_offset,
     posting_lists_offset, postings_offset, dictionary_fingerprint, reserved) = header
    actual_size = os.path.getsize(path)
    if magic != TOPN_MAGIC:
        errors.append(f"pinyin.topn.bin: bad magic {magic!r}")
        return False
    if version != TOPN_VERSION or header_size != TOPN_HEADER_SIZE:
        errors.append(
            "pinyin.topn.bin: unsupported header "
            f"(version={version}, size={header_size})"
        )
        return False
    if file_size != actual_size or reserved != 0:
        errors.append("pinyin.topn.bin: file size or reserved field is invalid")
        return False
    candidate_store = dictionary_candidate_store(data_dir, errors)
    if candidate_store is None:
        return False
    expected_entry_count, expected_fingerprint = candidate_store
    if (key_count == 0 or code_index_count == 0 or
            posting_list_count != key_count or
            dictionary_entry_count != expected_entry_count or
            dictionary_fingerprint != expected_fingerprint):
        errors.append("pinyin.topn.bin: invalid section counts or dictionary binding")
        return False

    cursor = TOPN_HEADER_SIZE
    sections = [
        (code_index_offset, code_index_count * 4),
        (posting_lists_offset, posting_list_count * TOPN_POSTING_LIST_SIZE),
        (postings_offset, posting_count * TOPN_CANDIDATE_POSTING_SIZE),
    ]
    for offset, size in sections:
        if cursor > actual_size or offset != cursor or size > actual_size - cursor:
            errors.append("pinyin.topn.bin: sections are not canonical")
            return False
        cursor += size
    if cursor != actual_size:
        errors.append("pinyin.topn.bin: sections do not cover the file")
        return False

    with open(path, "rb") as f:
        f.seek(code_index_offset)
        units = f.read(code_index_count * 4)
        if len(units) != code_index_count * 4:
            errors.append("pinyin.topn.bin: truncated Double-Array units")
            return False

        f.seek(posting_lists_offset)
        posting_lists = f.read(posting_list_count * TOPN_POSTING_LIST_SIZE)
        if len(posting_lists) != posting_list_count * TOPN_POSTING_LIST_SIZE:
            errors.append("pinyin.topn.bin: truncated posting lists")
            return False
        for index in range(posting_list_count):
            packed_list = struct.unpack_from(
                "<I", posting_lists, index * TOPN_POSTING_LIST_SIZE
            )[0]
            posting_offset = packed_list & TOPN_POSTING_OFFSET_MASK
            if index + 1 < posting_list_count:
                next_list = struct.unpack_from(
                    "<I", posting_lists, (index + 1) * TOPN_POSTING_LIST_SIZE
                )[0]
                posting_end = next_list & TOPN_POSTING_OFFSET_MASK
            else:
                posting_end = posting_count
            if ((index == 0 and posting_offset != 0) or
                    posting_offset > posting_end or posting_end > posting_count):
                errors.append(f"pinyin.topn.bin: invalid posting list at index {index}")
                return False

        if not check_topn_candidate_postings(
            f, postings_offset, posting_count, dictionary_entry_count, errors
        ):
            return False

        missing = []
        for key in REQUIRED_TOPN_KEYS:
            posting_list_index = find_topn_key(units, code_index_count, key)
            if posting_list_index is None or posting_list_index >= posting_list_count:
                missing.append(key)
                continue
            packed_list = struct.unpack_from(
                "<I", posting_lists,
                posting_list_index * TOPN_POSTING_LIST_SIZE
            )[0]
            if (packed_list & TOPN_POSTING_PREFIX_COMPLETE) == 0:
                errors.append(f"pinyin.topn.bin: required key is not prefix-complete: {key}")
                return False

    if missing:
        errors.append(f"pinyin.topn.bin: missing required keys: {', '.join(missing)}")
        return False
    return True


def is_safe_manifest_path(path):
    if not path:
        return False
    if path.startswith(("\\", "/")):
        return False
    if os.path.isabs(path):
        return False
    if ":" in path:
        return False
    parts = path.replace("\\", "/").split("/")
    return all(part and part not in (".", "..") for part in parts)


def check_dictionary_manifest(data_dir, errors):
    path = os.path.join(data_dir, "dictionary_manifest.json")
    try:
        with open(path, encoding="utf-8") as f:
            manifest = json.load(f)
    except Exception as e:
        errors.append(f"dictionary_manifest.json: cannot read manifest: {e}")
        return False

    if manifest.get("schema") != 1:
        errors.append("dictionary_manifest.json: unsupported schema")
        return False
    files = manifest.get("files")
    if not isinstance(files, list):
        errors.append("dictionary_manifest.json: files must be an array")
        return False

    roles = set()
    paths = set()
    for item in files:
        if not isinstance(item, dict):
            errors.append("dictionary_manifest.json: file entry must be object")
            return False
        role = item.get("role")
        rel = item.get("path")
        expected_size = item.get("size")
        expected_hash = item.get("sha256")
        if (not role or not rel or not isinstance(expected_size, int) or
                expected_size <= 0 or not expected_hash):
            errors.append("dictionary_manifest.json: file entry missing role/path/size/sha256")
            return False
        if role in roles:
            errors.append(f"dictionary_manifest.json: duplicate role: {role}")
            return False
        if rel.lower() in paths:
            errors.append(f"dictionary_manifest.json: duplicate path: {rel}")
            return False
        if not is_safe_manifest_path(rel):
            errors.append(f"dictionary_manifest.json: unsafe path: {rel}")
            return False
        if not isinstance(expected_hash, str) or len(expected_hash) != 64:
            errors.append(f"dictionary_manifest.json: invalid sha256 for {rel}")
            return False
        try:
            int(expected_hash, 16)
        except ValueError:
            errors.append(f"dictionary_manifest.json: invalid sha256 for {rel}")
            return False
        roles.add(role)
        paths.add(rel.lower())
        file_path = os.path.join(data_dir, rel)
        if not os.path.isfile(file_path):
            errors.append(f"dictionary_manifest.json: listed file missing: {rel}")
            return False
        actual_size = os.path.getsize(file_path)
        if actual_size != expected_size:
            errors.append(
                f"dictionary_manifest.json: size mismatch for {rel}: "
                f"expected {expected_size}, actual {actual_size}"
            )
            return False
        actual_hash = sha256_file(file_path)
        if actual_hash.lower() != expected_hash.lower():
            errors.append(f"dictionary_manifest.json: sha256 mismatch for {rel}")
            return False

    missing_roles = sorted(REQUIRED_MANIFEST_ROLES - roles)
    if missing_roles:
        errors.append(
            "dictionary_manifest.json: missing role(s): " + ", ".join(missing_roles)
        )
        return False
    return True


def check_symbols_json(data_dir, errors):
    path = os.path.join(data_dir, "symbols.json")
    try:
        with open(path, encoding="utf-8") as f:
            root = json.load(f)
    except Exception as e:
        errors.append(f"symbols.json: cannot read symbol table: {e}")
        return False

    if not isinstance(root, dict):
        errors.append("symbols.json: symbol table must be an object")
        return False
    categories = root.get("categories")
    if root.get("version") != 1 or not isinstance(categories, list) or not categories:
        errors.append("symbols.json: unsupported or empty symbol table")
        return False

    codes = set()
    for category in categories:
        code = category.get("code") if isinstance(category, dict) else None
        candidates = category.get("candidates") if isinstance(category, dict) else None
        if (
            not isinstance(code, str)
            or len(code) != 2
            or not code.isascii()
            or not code.islower()
            or not code.isalpha()
            or code in codes
        ):
            errors.append("symbols.json: invalid or duplicate category code")
            return False
        if (
            not isinstance(category.get("name"), str)
            or not category["name"]
            or not isinstance(candidates, list)
            or not candidates
            or any(not isinstance(value, str) or not value for value in candidates)
        ):
            errors.append(f"symbols.json: invalid category: {code}")
            return False
        codes.add(code)

    if "bd" not in codes:
        errors.append("symbols.json: required category missing: bd")
        return False
    return True


def verify(data_dir):
    """Run all checks. Return list of error strings (empty = pass)."""
    errors = []

    # 1. File existence and size
    for f in REQUIRED_FILES:
        check_file_exists(data_dir, f, errors)

    # If any files missing, can't proceed with format checks
    if errors:
        return errors

    # 2. Magic and version checks
    check_dictionary_manifest(data_dir, errors)
    check_dict_bin(data_dir, "pinyin.dict.bin", errors)
    check_reverse_index(
        data_dir, "pinyin.reverse.idx", "pinyin.dict.bin", errors
    )
    check_dict_idx(data_dir, "pinyin.dict.idx", errors)
    check_dict_bin(data_dir, "wubi86.dict.bin", errors)
    check_wubi_prefix_index(data_dir, errors)
    check_reverse_index(
        data_dir, "wubi86.reverse.idx", "wubi86.dict.bin", errors
    )
    for filename in PINYIN_SPELLING_FILES:
        check_spellings_bin(data_dir, filename, errors)
    check_topn_bin(data_dir, errors)
    check_symbols_json(data_dir, errors)

    return errors


def main():
    parser = argparse.ArgumentParser(description="Verify data file integrity")
    parser.add_argument("--data-dir", required=True, help="Data directory to verify")
    args = parser.parse_args()

    if not os.path.isdir(args.data_dir):
        print(f"ERROR: Data directory not found: {args.data_dir}", file=sys.stderr)
        return 1

    errors = verify(args.data_dir)
    if errors:
        print(f"FAILED: {len(errors)} error(s):", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1

    print("All data file checks PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
