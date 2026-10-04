# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

# Zhiyi IME supports full pinyin only (initials included); no shuangpin spelling tables.
SHUANGPIN_SCHEME_NAMES = ()

SHUANGPIN_MANIFEST_FILES = tuple(
    (
        f"pinyin_spellings_{name}_shuangpin",
        f"pinyin.{name}-shuangpin.spellings.bin",
    )
    for name in SHUANGPIN_SCHEME_NAMES
)

MANIFEST_FILES = (
    ("pinyin_dict", "pinyin.dict.bin"),
    ("pinyin_idx", "pinyin.dict.idx"),
    ("pinyin_spellings", "pinyin.spellings.bin"),
    *SHUANGPIN_MANIFEST_FILES,
    ("pinyin_topn", "pinyin.topn.bin"),
    ("pinyin_reverse_index", "pinyin.reverse.idx"),
    ("wubi_dict", "wubi86.dict.bin"),
    ("wubi_prefix_index", "wubi86.dict.idx"),
    ("wubi_reverse_index", "wubi86.reverse.idx"),
)

REQUIRED_MANIFEST_ROLES = frozenset(role for role, _ in MANIFEST_FILES)
PINYIN_SPELLING_FILES = tuple(
    filename for role, filename in MANIFEST_FILES if role.startswith("pinyin_spellings")
)
REQUIRED_BUNDLE_FILES = (
    "dictionary_manifest.json",
    *(filename for _, filename in MANIFEST_FILES),
    "default.json",
    "settings_presets.json",
    "punctuation.json",
    "symbols.json",
)
