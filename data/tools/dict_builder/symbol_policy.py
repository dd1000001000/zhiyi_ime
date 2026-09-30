"""Identify system symbols to omit before building ordinary dictionaries."""

import json
from pathlib import Path


def expand_ranges(values):
    result = set()
    for value in values:
        bounds = value.split("..")
        result.update(
            chr(cp)
            for cp in range(
                int(bounds[0], 16),
                int(bounds[-1], 16) + 1,
            )
        )
    return result


class SymbolPolicy:
    def __init__(self):
        path = Path(__file__).with_name("emoji_classification.json")
        data = json.loads(path.read_text(encoding="utf-8"))
        if data["schema"] != 1 or data["unicode_version"] != "17.0":
            raise ValueError("unsupported emoji classification data")
        self.forms = {}
        for group, sequences in data["groups"].items():
            for sequence in sequences:
                text = "".join(chr(int(cp, 16)) for cp in sequence.split())
                self.forms[text] = group
        props = data["properties"]
        self.presentation = expand_ranges(props["Emoji_Presentation"])
        self.symbols = expand_ranges(props["Punctuation_Or_Symbol"])
        self.pictographic = expand_ranges(props["Extended_Pictographic"])
        # Accept characters of possible sequences only for review detection,
        # never as a substitute for complete sequence membership.
        components = (
            set("\u200d\ufe0e\ufe0f\u20e3") |
            {chr(cp) for cp in range(0xE0020, 0xE0080)}
        )
        self.sequence_chars = (
            self.symbols | self.pictographic | components |
            (expand_ranges(props["Emoji"]) - set("0123456789"))
        )
        self.markers = self.presentation | self.pictographic | components

    def _emoji_group(self, text):
        group = self.forms.get(text)
        if (group is None and len(text) == 2 and text[1] == "\ufe0f"
                and text[0] in self.presentation):
            group = self.forms.get(text[0])
        return group

    def _is_graphic_sequence(self, text):
        offset = 0
        while offset < len(text):
            char = text[offset]
            offset += 1
            # As in the runtime policy, digits are text unless part of a keycap.
            if "0" <= char <= "9":
                end = offset
                if end < len(text) and text[end] == "\ufe0f":
                    end += 1
                if end < len(text) and text[end] == "\u20e3":
                    offset = end + 1
                    continue
            if char not in self.sequence_chars:
                return False
        return bool(text)

    def classify(self, text):
        """Return emoji/symbol/None independently of catalog membership."""
        group = self._emoji_group(text)
        if group is not None and group != "Component":
            if group == "Symbols" and len(text) == 1:
                return "symbol"
            return "emoji"
        # Isolated modifiers are pure symbols, not complete emoji candidates.
        pure_symbols = text and all(ch in self.symbols for ch in text)
        if pure_symbols and len(text) == 1:
            return "symbol"
        if (self._is_graphic_sequence(text)
                and any(ch in self.markers for ch in text)):
            raise ValueError(f"unreviewed emoji sequence: {text!r}")
        if pure_symbols:
            return "symbol"
        return None
