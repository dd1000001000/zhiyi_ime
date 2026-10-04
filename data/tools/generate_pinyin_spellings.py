#!/usr/bin/env python3
"""Spelling Algebra engine for CxxIME — port of librime's calculus.cc + algebra.cc.

Applies configurable regex rules to expand a syllabary into a Script
(map<input_str, list[Spelling]>), supporting abbreviation, fuzzy matching,
derivation, transformation, erasion, and transliteration.

Usage:
    from generate_pinyin_spellings import SpellingAlgebra, Script, parse_rules_from_json
    script = Script()
    for s in syllabary: script.add_syllable(s)
    rules = parse_rules_from_json("schema.json")
    SpellingAlgebra(rules).apply(script)
"""

import json
import re
import sys
from dataclasses import dataclass, field

# SpellingType — matches librime algo/spelling.h
K_NORMAL = 0
K_FUZZY = 1
K_ABBREV = 2

# Credibility penalties — matches librime algo/calculus.cc
K_ABBREV_PENALTY = -0.6931471805599453   # log(0.5)
K_FUZZY_PENALTY = -0.6931471805599453    # log(0.5)

_SCHEME_ID_PATTERN = re.compile(r"^[a-z][a-z0-9_]*$")
_SUPPORTED_SHUANGPIN_ALPHABET = frozenset("abcdefghijklmnopqrstuvwxyz;")
_ROOT_FIELDS = {"scheme_id", "speller"}
_FULL_PINYIN_FIELDS = {"type", "algebra", "fuzzy_groups"}
_SHUANGPIN_FIELDS = {
    "type",
    "alphabet",
    "initials",
    "finals",
    "zero_initials",
    "final_aliases_by_initial",
    "passthrough_syllables",
    "ignored_syllables",
    "expected_collisions",
    "fuzzy_algebra",
}


@dataclass
class Spelling:
    """A possible syllable interpretation — matches librime Spelling."""
    syllable: str
    type: int = K_NORMAL
    credibility: float = 0.0
    # Fuzzy pinyin groups (bits) this spelling needs; 0 when no fuzzy rule was involved.
    groups: int = 0

    def __eq__(self, other):
        return isinstance(other, Spelling) and self.syllable == other.syllable

    def __hash__(self):
        return hash(self.syllable)


class Script(dict):
    """map<input_str, list[Spelling]> — matches librime Script (algebra.h).

    Each key is a possible user input string, each value is the list of
    syllables it can produce with their type and credibility.
    """

    def add_syllable(self, syllable):
        """Add a normal spelling for a syllable — matches Script::AddSyllable."""
        if syllable not in self:
            self[syllable] = [Spelling(syllable, K_NORMAL, 0.0)]

    def merge(self, key, rule_type, rule_credibility, spellings, rule_groups=0):
        """Merge spellings under a new key — matches Script::Merge.

        For each spelling in the source list:
        - type = max(rule_type, spelling.type) — worse type wins
        - credibility = spelling.credibility + rule_credibility
        If key already exists, deduplicate: keep better type, higher credibility.
        """
        if key not in self:
            self[key] = []
        target = self[key]
        for sp in spellings:
            new_type = max(rule_type, sp.type)
            new_cred = sp.credibility + rule_credibility
            new_groups = sp.groups | rule_groups
            # Deduplicate: find existing entry with same syllable
            existing = None
            for e in target:
                if e.syllable == sp.syllable:
                    existing = e
                    break
            if existing:
                # Keep better (lower) type and higher credibility; of two derivations keep the
                # one that needs fewer fuzzy groups (a better type wins outright).
                if new_type < existing.type or (
                        new_type == existing.type and
                        bin(new_groups).count("1") < bin(existing.groups).count("1")):
                    existing.groups = new_groups
                existing.type = min(existing.type, new_type)
                existing.credibility = max(existing.credibility, new_cred)
            else:
                target.append(Spelling(sp.syllable, new_type, new_cred, new_groups))


class SpellingRule:
    """Base class for spelling rules — matches librime Calculation (calculus.h)."""

    def apply(self, text):
        """Try to transform text. Return (new_text, True) or (text, False)."""
        raise NotImplementedError

    def addition(self):
        """Whether the transformed form is added to the Script."""
        return True

    def deletion(self):
        """Whether the original form is removed from the Script."""
        return True

    def rule_type(self):
        """SpellingType produced by this rule."""
        return K_NORMAL

    def credibility_delta(self):
        """Credibility penalty added by this rule."""
        return 0.0


class TransformationRule(SpellingRule):
    """xform/ptn/rep/ — regex replacement.
    Matches librime Transformation (calculus.cc:98-119).
    addition=True, deletion=True (replaces original).
    """

    def __init__(self, pattern, replacement):
        self.pattern = re.compile(pattern)
        self.replacement = replacement

    def apply(self, text):
        result = self.pattern.sub(self.replacement, text)
        if result != text:
            return result, True
        return text, False


class ErasionRule(SpellingRule):
    """erase/ptn/ — remove entries matching pattern.
    Matches librime Erasion (calculus.cc:123-141).
    addition=False (removes the entry entirely).
    """

    def __init__(self, pattern):
        self.pattern = re.compile(pattern)

    def apply(self, text):
        if self.pattern.fullmatch(text):
            return "", True
        return text, False

    def addition(self):
        return False


class DerivationRule(TransformationRule):
    """derive/ptn/rep/ — keep original + add transformed.
    Matches librime Derivation (calculus.cc:145-156).
    addition=True, deletion=False (keeps original).
    """

    def deletion(self):
        return False


class FuzzyDerivationRule(DerivationRule):
    """A derive rule that is semantically fuzzy (e.g. eng↔en, zh↔z).
    Produces type=kFuzzySpelling with penalty, like FuzzingRule,
    but parsed from a derive/ definition.
    """

    def rule_type(self):
        return K_FUZZY

    def credibility_delta(self):
        return K_FUZZY_PENALTY


# Known fuzzy derive patterns: (raw_regex, raw_replacement)
_FUZZY_DERIVE_PATTERNS = {
    (r"^([zcs])h", "$1"),       # zh↔z, ch↔c, sh↔s
    (r"^(.*)eng$", "$1en"),     # eng↔en
}


class FuzzingRule(DerivationRule):
    """fuzz/ptn/rep/ — fuzzy spelling variant.
    Matches librime Fuzzing (calculus.cc:160-180).
    Like Derivation but sets type=kFuzzySpelling, penalty=-0.693.
    """

    def rule_type(self):
        return K_FUZZY

    def credibility_delta(self):
        return K_FUZZY_PENALTY


class AbbreviationRule(DerivationRule):
    """abbrev/ptn/rep/ — abbreviation.
    Matches librime Abbreviation (calculus.cc:184-204).
    Like Derivation but sets type=kAbbreviation, penalty=-0.693.
    """

    def rule_type(self):
        return K_ABBREV

    def credibility_delta(self):
        return K_ABBREV_PENALTY


class TransliterationRule(SpellingRule):
    """xlit/abc/ABC/ — character-by-character mapping.
    Matches librime Transliteration (calculus.cc:48-94).
    """

    def __init__(self, char_map):
        self.char_map = char_map  # dict{from_char: to_char}

    def apply(self, text):
        result = []
        modified = False
        for ch in text:
            if ch in self.char_map:
                result.append(self.char_map[ch])
                modified = True
            else:
                result.append(ch)
        if modified:
            return "".join(result), True
        return text, False


class SpellingAlgebra:
    """Applies a list of rules to a Script — matches librime Projection::Apply(Script*).

    Source: librime algo/algebra.cc:117-150
    """

    def __init__(self, rules):
        self.rules = rules

    def apply(self, script):
        """Apply all rules sequentially, expanding the Script.

        For each rule, for each key in the Script:
        - Try regex replacement on the key
        - If matched:
            if !deletion: keep original key (merge its spellings)
            if addition: add transformed key (merge with rule type/penalty)
        - If not matched: keep original key unchanged
        """
        for rule in self.rules:
            temp = Script()
            for key, spellings in script.items():
                new_key, applied = rule.apply(key)
                if applied:
                    if not rule.deletion():
                        # Keep original key
                        temp.merge(key, K_NORMAL, 0.0, spellings)
                    if rule.addition() and new_key:
                        # Add transformed key with rule's type, penalty and fuzzy group
                        temp.merge(new_key, rule.rule_type(),
                                   rule.credibility_delta(), spellings,
                                   getattr(rule, "groups", 0))
                else:
                    # Rule didn't match — keep unchanged
                    temp.merge(key, K_NORMAL, 0.0, spellings)
            script.clear()
            script.update(temp)
        return script


def _convert_boost_replacement(replacement):
    """Convert boost::regex $N backreferences to Python \\N format."""
    return re.sub(r'\$(\d+)', r'\\\1', replacement)


def _parse_rule(definition):
    """Parse a single rule definition string — matches librime Calculus::Parse.

    Format: token/regex/replacement/ or token/regex/
    Separator is the first non-lowercase-alpha character.
    """
    # Find separator (first char not in [a-z])
    sep = None
    for i, ch in enumerate(definition):
        if not ch.isalpha() or ch.isupper():
            sep = ch
            break
    if sep is None:
        return None

    parts = definition.split(sep)
    if not parts:
        return None

    token = parts[0]
    args = parts[1:]  # split produces empty string at end for trailing sep

    if token == "xform":
        if len(args) >= 2:
            return TransformationRule(args[0], _convert_boost_replacement(args[1]))
    elif token == "derive":
        if len(args) >= 2:
            if (args[0], args[1]) in _FUZZY_DERIVE_PATTERNS:
                return FuzzyDerivationRule(args[0], _convert_boost_replacement(args[1]))
            return DerivationRule(args[0], _convert_boost_replacement(args[1]))
    elif token == "fuzz":
        if len(args) >= 2:
            return FuzzingRule(args[0], _convert_boost_replacement(args[1]))
    elif token == "abbrev":
        if len(args) >= 2:
            return AbbreviationRule(args[0], _convert_boost_replacement(args[1]))
    elif token == "erase":
        if len(args) >= 1:
            return ErasionRule(args[0])
    elif token == "xlit":
        if len(args) >= 2:
            left, right = args[0], args[1]
            if len(left) == len(right):
                char_map = dict(zip(left, right))
                return TransliterationRule(char_map)

    return None


def _require_exact_fields(mapping, expected, field_name):
    actual = set(mapping)
    missing = sorted(expected - actual)
    unknown = sorted(actual - expected)
    if missing or unknown:
        raise ValueError(
            f"{field_name} fields differ from the schema contract: "
            f"missing={missing}, unknown={unknown}"
        )


def validate_schema(schema):
    if not isinstance(schema, dict):
        raise ValueError("schema root must be an object")
    _require_exact_fields(schema, _ROOT_FIELDS, "schema")

    scheme_id = schema["scheme_id"]
    if not isinstance(scheme_id, str) or not _SCHEME_ID_PATTERN.fullmatch(scheme_id):
        raise ValueError("scheme_id must be a stable snake_case identifier")

    speller = schema["speller"]
    if not isinstance(speller, dict):
        raise ValueError("schema must contain a speller object")
    speller_type = speller.get("type")
    if speller_type == "full_pinyin":
        _require_exact_fields(speller, _FULL_PINYIN_FIELDS, "speller")
        definitions = speller["algebra"]
        if not isinstance(definitions, list) or not all(
            isinstance(definition, str) for definition in definitions
        ):
            raise ValueError("speller.algebra must be a string array")
        groups = speller["fuzzy_groups"]
        bits = [group.get("bit") for group in groups if isinstance(group, dict)]
        if (not isinstance(groups, list) or len(bits) != len(groups) or
                any(not isinstance(bit, int) or bit <= 0 or bit > 0x80 or bit & (bit - 1)
                    for bit in bits) or len(set(bits)) != len(bits) or
                any(not isinstance(group.get("rules"), list) or not group["rules"]
                    for group in groups)):
            raise ValueError("speller.fuzzy_groups must list {name, bit, rules} with "
                             "distinct single-bit values")
    elif speller_type == "shuangpin":
        _require_exact_fields(speller, _SHUANGPIN_FIELDS, "speller")
    else:
        raise ValueError(f"unsupported speller.type: {speller_type}")
    return schema


def parse_rules_from_json(json_path):
    """Parse spelling algebra rules from a CxxIME JSON schema file."""
    schema = load_schema(json_path)
    if schema["speller"]["type"] != "full_pinyin":
        raise ValueError("speller.type must be full_pinyin for spelling algebra")
    definitions = schema["speller"]["algebra"]

    rules = []
    for definition in definitions:
        if not isinstance(definition, str):
            raise ValueError("speller.algebra entries must be strings")
        rule = _parse_rule(definition)
        if rule is None:
            raise ValueError(f"failed to parse spelling rule: {definition}")
        rules.append(rule)
    # Fuzzy pinyin pairs: each group's rules tag their spellings with the group's bit, so the
    # engine can enable the pairs one by one (applied after the algebra above).
    for group in schema["speller"]["fuzzy_groups"]:
        for definition in group["rules"]:
            rule = _parse_rule(definition)
            if rule is None or rule.rule_type() != K_FUZZY:
                raise ValueError(f"fuzzy group rules must be fuzz rules: {definition}")
            rule.groups = group["bit"]
            rules.append(rule)
    return rules


def parse_rules_simple(rules_list):
    """Parse rules from a list of definition strings (for testing)."""
    rules = []
    for definition in rules_list:
        rule = _parse_rule(definition)
        if rule:
            rules.append(rule)
    return rules


def _mapping_values(mapping, key, field_name):
    value = mapping.get(key)
    if isinstance(value, str):
        values = [value]
    elif isinstance(value, list) and all(isinstance(item, str) for item in value):
        values = value
    else:
        raise ValueError(f"{field_name}.{key} must be a string or string array")
    if not values or any(not item for item in values):
        raise ValueError(f"{field_name}.{key} must not be empty")
    if len(values) != len(set(values)):
        raise ValueError(f"{field_name}.{key} contains duplicate codes")
    return values


def _shuangpin_encoder(schema, usage=None):
    validate_schema(schema)
    speller = schema.get("speller")
    if speller["type"] != "shuangpin":
        raise ValueError("schema must contain a shuangpin speller")

    alphabet = speller.get("alphabet")
    initials = speller.get("initials")
    finals = speller.get("finals")
    zero_initials = speller.get("zero_initials")
    aliases = speller.get("final_aliases_by_initial", {})
    passthrough_syllables = speller["passthrough_syllables"]
    ignored_syllables = speller["ignored_syllables"]
    if not isinstance(alphabet, str) or not alphabet:
        raise ValueError("speller.alphabet must be a non-empty string")
    if len(alphabet) != len(set(alphabet)):
        raise ValueError("speller.alphabet must not contain duplicate characters")
    unsupported_characters = sorted(set(alphabet) - _SUPPORTED_SHUANGPIN_ALPHABET)
    if unsupported_characters:
        raise ValueError(
            "speller.alphabet contains characters unsupported by the input processor: "
            f"{unsupported_characters}"
        )
    if not all(isinstance(value, dict) for value in (initials, finals, zero_initials, aliases)):
        raise ValueError("shuangpin mapping fields must be objects")
    if not isinstance(passthrough_syllables, list) or not all(
        isinstance(item, str) and item for item in passthrough_syllables
    ):
        raise ValueError("speller.passthrough_syllables must be a string array")
    if not isinstance(ignored_syllables, list) or not all(
        isinstance(item, str) and item for item in ignored_syllables
    ):
        raise ValueError("speller.ignored_syllables must be a string array")

    initial_names = sorted(initials, key=len, reverse=True)
    passthrough_set = set(passthrough_syllables)
    ignored_set = set(ignored_syllables)

    def record(field, key):
        if usage is not None:
            usage.setdefault(field, set()).add(key)

    def validate_codes(codes, syllable):
        for code in codes:
            if any(character not in alphabet for character in code):
                raise ValueError(f"{syllable}: code contains a character outside alphabet: {code}")
            if code.startswith(";"):
                raise ValueError(f"{syllable}: code cannot start with semicolon")
            if len(code) > 2:
                raise ValueError(f"{syllable}: shuangpin code is longer than two keys: {code}")
        return codes


    def encode(syllable):
        if syllable in ignored_set:
            record("ignored_syllables", syllable)
            return []
        if syllable in passthrough_set:
            record("passthrough_syllables", syllable)
            return validate_codes([syllable], syllable)
        if syllable in zero_initials:
            record("zero_initials", syllable)
            return validate_codes(
                _mapping_values(zero_initials, syllable, "speller.zero_initials"),
                syllable,
            )

        initial = next((name for name in initial_names if syllable.startswith(name)), None)
        if initial is None:
            raise ValueError(f"{syllable}: no shuangpin initial mapping")
        source_final = syllable[len(initial):]
        initial_aliases = aliases.get(initial, {})
        if not isinstance(initial_aliases, dict):
            raise ValueError(f"speller.final_aliases_by_initial.{initial} must be an object")
        final = initial_aliases.get(source_final, source_final)
        if not isinstance(final, str) or not final:
            raise ValueError(f"{syllable}: invalid final alias")
        initial_key = initials[initial]
        if not isinstance(initial_key, str) or len(initial_key) != 1:
            raise ValueError(f"speller.initials.{initial} must be one key")
        record("initials", initial)
        record("finals", final)
        if final != source_final:
            record("final_aliases_by_initial", f"{initial}:{source_final}")
        final_keys = _mapping_values(finals, final, "speller.finals")
        return validate_codes([initial_key + key for key in final_keys], syllable)

    return encode


def build_shuangpin_script(syllabary, schema):
    """Build a spelling script from an explicit, build-time Shuangpin table."""
    usage = {}
    encode = _shuangpin_encoder(schema, usage)
    speller = schema["speller"]
    script = Script()
    normal_by_code = {}
    for syllable in sorted(syllabary):
        codes = encode(syllable)
        for code in codes:
            script.merge(code, K_NORMAL, 0.0, [Spelling(syllable)])
            normal_by_code.setdefault(code, set()).add(syllable)

    declared_keys = {
        "initials": set(speller["initials"]),
        "finals": set(speller["finals"]),
        "zero_initials": set(speller["zero_initials"]),
        "passthrough_syllables": set(speller["passthrough_syllables"]),
        "ignored_syllables": set(speller["ignored_syllables"]),
        "final_aliases_by_initial": {
            f"{initial}:{source_final}"
            for initial, final_aliases in speller.get("final_aliases_by_initial", {}).items()
            for source_final in final_aliases
        },
    }
    unreachable = {
        field: sorted(keys - usage.get(field, set()))
        for field, keys in declared_keys.items()
        if keys - usage.get(field, set())
    }
    if unreachable:
        raise ValueError(f"unreachable shuangpin mappings: {unreachable}")

    declared_collisions = speller["expected_collisions"]
    if not isinstance(declared_collisions, dict):
        raise ValueError("speller.expected_collisions must be an object")
    actual_collisions = {
        code: sorted(syllables)
        for code, syllables in normal_by_code.items()
        if len(syllables) > 1
    }
    expected_collisions = {}
    for code, syllables in declared_collisions.items():
        if not isinstance(syllables, list) or not all(
            isinstance(syllable, str) and syllable for syllable in syllables
        ):
            raise ValueError(f"speller.expected_collisions.{code} must be a string array")
        expected_collisions[code] = sorted(set(syllables))
    if actual_collisions != expected_collisions:
        raise ValueError(
            "shuangpin collisions differ from declaration: "
            f"actual={actual_collisions}, expected={expected_collisions}"
        )

    fuzzy_definitions = speller.get("fuzzy_algebra", [])
    if not isinstance(fuzzy_definitions, list):
        raise ValueError("speller.fuzzy_algebra must be an array")
    fuzzy_rules = parse_rules_simple(fuzzy_definitions)
    if len(fuzzy_rules) != len(fuzzy_definitions) or any(
        rule.rule_type() != K_FUZZY for rule in fuzzy_rules
    ):
        raise ValueError("speller.fuzzy_algebra may contain only fuzzy rules")
    for syllable in sorted(syllabary):
        for rule in fuzzy_rules:
            alias, applied = rule.apply(syllable)
            if not applied or not alias:
                continue
            for code in encode(alias):
                script.merge(
                    code,
                    K_FUZZY,
                    rule.credibility_delta(),
                    [Spelling(syllable)],
                )
    return script


def load_schema(json_path):
    with open(json_path, "r", encoding="utf-8") as source:
        schema = json.load(source)
    return validate_schema(schema)


# Self-test
if __name__ == "__main__":
    import os

    # CLI mode: generate_pinyin_spellings.py <dict.db> [schema.json]
    if len(sys.argv) >= 2:
        db_path = sys.argv[1]
        schema_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
            os.path.dirname(__file__), "..", "schemas", "pinyin.full-pinyin.schema.json")

        if not os.path.exists(schema_path):
            print(f"Schema not found: {schema_path}", file=sys.stderr)
            sys.exit(1)

        # Load distinct syllables from dict table
        import sqlite3
        conn = sqlite3.connect(db_path)
        cur = conn.cursor()
        cur.execute("SELECT DISTINCT syllable_ids FROM dict")
        syllabary = set()
        for (sid,) in cur.fetchall():
            if sid:
                for s in sid.split(":"):
                    if s:
                        syllabary.add(s)
        conn.close()
        print(f"Loaded {len(syllabary)} unique syllables")

        schema = load_schema(schema_path)
        if schema["speller"]["type"] == "shuangpin":
            script = build_shuangpin_script(syllabary, schema)
            normal_codes = {
                input_code
                for input_code, spellings in script.items()
                if any(spelling.type == K_NORMAL for spelling in spellings)
            }
            collision_count = len(schema["speller"]["expected_collisions"])
            print(
                f"Loaded Shuangpin scheme {schema['scheme_id']}: "
                f"{len(normal_codes)} normal codes, {collision_count} declared collisions"
            )
        else:
            script = Script()
            for s in sorted(syllabary):
                script.add_syllable(s)
            rules = parse_rules_from_json(schema_path)
            print(f"Loaded {len(rules)} rules from {schema_path}")
            SpellingAlgebra(rules).apply(script)

        # Write spellings table to SQLite
        conn = sqlite3.connect(db_path)
        cur = conn.cursor()
        cur.execute("DROP TABLE IF EXISTS spellings")
        cur.execute("""
            CREATE TABLE spellings (
                input TEXT NOT NULL,
                syllable TEXT NOT NULL,
                type INTEGER NOT NULL,
                credibility REAL NOT NULL,
                fuzzy_groups INTEGER NOT NULL DEFAULT 0
            )
        """)
        rows = []
        for input_str, spellings_list in sorted(script.items()):
            for sp in spellings_list:
                rows.append((input_str, sp.syllable, sp.type, sp.credibility, sp.groups))
        cur.executemany(
            "INSERT INTO spellings (input, syllable, type, credibility, fuzzy_groups) "
            "VALUES (?, ?, ?, ?, ?)",
            rows)
        conn.commit()
        print(f"Wrote {len(rows)} spellings entries to {db_path}")
        conn.close()
        sys.exit(0)

    print("=== Spelling Algebra Self-Test ===")

    # Test 1: Abbreviation
    script = Script()
    for s in ["ni", "hao", "da", "di"]:
        script.add_syllable(s)
    rules = parse_rules_simple(["abbrev/^(.+).$/$1/"])
    SpellingAlgebra(rules).apply(script)
    assert "d" in script, f"'d' not in script: {list(script.keys())}"
    d_spellings = {s.syllable: s for s in script["d"]}
    assert "da" in d_spellings and d_spellings["da"].type == K_ABBREV
    assert "di" in d_spellings and d_spellings["di"].type == K_ABBREV
    assert "n" in script
    print("  [OK] abbrev: 'd' → da,di (type=abbrev)")

    # Test 2: Derivation (fuzzy zh→z)
    script = Script()
    for s in ["ca", "cha"]:
        script.add_syllable(s)
    rules = parse_rules_simple(["derive/^([zcs])h/$1/"])
    SpellingAlgebra(rules).apply(script)
    assert "ca" in script
    ca_spellings = {s.syllable: s for s in script["ca"]}
    assert "ca" in ca_spellings and ca_spellings["ca"].type == K_NORMAL
    assert "cha" in ca_spellings and ca_spellings["cha"].type == K_FUZZY  # fuzzy derive
    print("  [OK] derive: 'ca' → ca(normal), cha(fuzzy)")

    # Test 3: Fuzzing (n↔l)
    script = Script()
    for s in ["na", "la"]:
        script.add_syllable(s)
    rules = parse_rules_simple(["fuzz/^n(.*)/l$1/", "fuzz/^l(.*)/n$1/"])
    SpellingAlgebra(rules).apply(script)
    assert "na" in script
    na_spellings = {s.syllable: s for s in script["na"]}
    assert "na" in na_spellings
    assert "la" in na_spellings and na_spellings["la"].type == K_FUZZY
    print("  [OK] fuzz: 'na' → na(normal), la(fuzzy)")

    # Test 4: Multi-rule (abbrev + derive)
    script = Script()
    for s in ["ni", "hao", "da", "di", "chu", "ca"]:
        script.add_syllable(s)
    rules = parse_rules_simple([
        "abbrev/^(.+).$/$1/",
        "derive/^([zcs])h/$1/",
    ])
    SpellingAlgebra(rules).apply(script)
    # abbrev: "da"→"d", "di"→"d" → "d" maps to [da, di]
    assert "d" in script
    d_spellings = {s.syllable: s for s in script["d"]}
    assert "da" in d_spellings and "di" in d_spellings
    # derive: "chu"→"cu" (ch→c), "ca" unchanged
    assert "cu" in script
    cu_spellings = {s.syllable: s for s in script["cu"]}
    assert "chu" in cu_spellings  # derive keeps original
    print(f"  [OK] multi-rule: 'd' → {[s.syllable for s in script['d']]}, 'cu' → {[s.syllable for s in script['cu']]}")

    # Test 5: Schema JSON parsing
    import os
    schema_path = os.path.join(
        os.path.dirname(__file__), "..", "schemas", "pinyin.full-pinyin.schema.json"
    )
    if os.path.exists(schema_path):
        rules = parse_rules_from_json(schema_path)
        print(f"  [OK] schema JSON: loaded {len(rules)} rules")
        for r in rules:
            print(f"       {r.__class__.__name__}")
    else:
        print(f"  [SKIP] schema JSON not found: {schema_path}")

    print("\n=== All tests passed ===")
