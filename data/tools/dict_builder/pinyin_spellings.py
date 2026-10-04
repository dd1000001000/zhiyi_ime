"""Build the Patricia trie used for Pinyin spelling lookup."""

from __future__ import annotations

import sqlite3
import struct
from collections import deque
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Tuple


MAGIC = b"CXSPL\x02\x00\x00"
HEADER_FORMAT = "<8sIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
# syllable offset, syllable length, type, fuzzy groups, credibility
SPELLING_FORMAT = "<IIbBf"
CHILD_FORMAT = "<B3xI"


@dataclass
class TrieNode:
    key: bytes = b""
    spellings: List[Tuple[bytes, int, float]] = field(default_factory=list)
    children: Dict[int, "TrieNode"] = field(default_factory=dict)


class PatriciaTrie:
    def __init__(self):
        self.root = TrieNode()

    def insert(self, key: str, syllable: str, spelling_type: int, credibility: float):
        self._insert(
            self.root,
            key.encode("utf-8"),
            syllable.encode("utf-8"),
            spelling_type,
            credibility,
        )

    def _insert(
        self,
        node: TrieNode,
        key: bytes,
        syllable: bytes,
        spelling_type: int,
        credibility: float,
    ):
        if not key:
            node.spellings.append((syllable, spelling_type, credibility))
            return

        first = key[0]
        child = node.children.get(first)
        if child is None:
            node.children[first] = TrieNode(
                key=key,
                spellings=[(syllable, spelling_type, credibility)],
            )
            return

        common = 0
        while (
            common < len(key)
            and common < len(child.key)
            and key[common] == child.key[common]
        ):
            common += 1

        if common == len(child.key):
            self._insert(
                child,
                key[common:],
                syllable,
                spelling_type,
                credibility,
            )
            return
        if common == 0:
            node.children[first] = TrieNode(
                key=key,
                spellings=[(syllable, spelling_type, credibility)],
            )
            return

        split = TrieNode(key=child.key[:common])
        node.children[first] = split
        old_suffix = child.key[common:]
        split.children[old_suffix[0]] = TrieNode(
            key=old_suffix,
            spellings=child.spellings,
            children=child.children,
        )
        new_suffix = key[common:]
        if new_suffix:
            split.children[new_suffix[0]] = TrieNode(
                key=new_suffix,
                spellings=[(syllable, spelling_type, credibility)],
            )
        else:
            split.spellings.append((syllable, spelling_type, credibility))

    def serialize(self) -> bytes:
        string_data = bytearray()
        string_offsets: Dict[bytes, int] = {}

        def intern(value: bytes) -> Tuple[int, int]:
            if value in string_offsets:
                return string_offsets[value], len(value)
            offset = len(string_data)
            string_data.extend(value)
            string_offsets[value] = offset
            return offset, len(value)

        queue: Deque[TrieNode] = deque([self.root])
        serialized = []
        node_indexes = {id(self.root): 0}
        while queue:
            node = queue.popleft()
            serialized.append(node)
            for first_character in sorted(node.children):
                child = node.children[first_character]
                node_indexes[id(child)] = len(node_indexes)
                queue.append(child)

        nodes_data = bytearray()
        for node in serialized:
            key_offset, key_length = intern(node.key)
            nodes_data.extend(
                struct.pack(
                    "<IIBB2x",
                    key_offset,
                    key_length,
                    len(node.spellings),
                    len(node.children),
                )
            )
            for syllable, spelling_type, credibility in node.spellings:
                syllable_offset, syllable_length = intern(syllable)
                nodes_data.extend(
                    struct.pack(
                        SPELLING_FORMAT,
                        syllable_offset,
                        syllable_length,
                        spelling_type & 0xFF,         # type in the low byte,
                        (spelling_type >> 8) & 0xFF,  # fuzzy groups in the next one
                        credibility,
                    )
                )
            for first_character in sorted(node.children):
                nodes_data.extend(
                    struct.pack(
                        CHILD_FORMAT,
                        first_character,
                        node_indexes[id(node.children[first_character])],
                    )
                )

        entries_offset = HEADER_SIZE
        strings_offset = entries_offset + len(nodes_data)
        header = struct.pack(
            HEADER_FORMAT,
            MAGIC,
            2,
            len(serialized),
            len(string_data),
            entries_offset,
            strings_offset,
        )
        return header + nodes_data + bytes(string_data)


def build(database_path: str, output_path: str) -> int:
    connection = sqlite3.connect(database_path)
    cursor = connection.cursor()
    cursor.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='spellings'")
    if not cursor.fetchone():
        print("  Warning: no spellings table, skipping")
        connection.close()
        return 0
    columns = {row[1] for row in cursor.execute("PRAGMA table_info(spellings)")}
    groups = "fuzzy_groups" if "fuzzy_groups" in columns else "0"
    cursor.execute(f"SELECT input, syllable, type, credibility, {groups} FROM spellings")
    rows = cursor.fetchall()
    connection.close()
    if not rows:
        print("  Warning: spellings empty, skipping")
        return 0

    trie = PatriciaTrie()
    for input_code, syllable, spelling_type, credibility, fuzzy_groups in rows:
        # The trie keeps (type | groups << 8) together; serialize() splits them again.
        trie.insert(input_code, syllable, spelling_type | (fuzzy_groups << 8), credibility)

    data = trie.serialize()
    with open(output_path, "wb") as output:
        output.write(data)
    node_count = struct.unpack_from(HEADER_FORMAT, data, 0)[2]
    print(f"  spellings.bin: {len(rows)} spellings, {node_count} trie nodes, {len(data)} bytes")
    return len(rows)
