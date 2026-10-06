// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/spellings_index.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <windows.h>

#include <cxxime/logging.h>

static const char SPELLINGS_MAGIC_V2[] = "CXSPL\x02\x00\x00";
static const uint32_t NODE_HEADER_SIZE = 12;
static const uint32_t SPELLING_SIZE = 14;  // v2 per-node spelling
static const uint32_t CHILD_SIZE = 8;

namespace cxxime {

SpellingsIndex::~SpellingsIndex() {
    unload();
}

bool SpellingsIndex::load(const std::string& bin_path) {
    unload();
    CXXIME_LOG(L"SpellingsIndex::load path=%S", bin_path.c_str());

    if (!file_.open(bin_path)) {
        CXXIME_LOG(L"SpellingsIndex::load mapping FAILED");
        return false;
    }
    if (file_.size() < 28) {
        file_.close();
        return false;
    }
    data_size_ = file_.size();
    data_ = file_.data();

    auto* hdr = (const SpellingsHeader*)data_;
    if (std::memcmp(hdr->magic, SPELLINGS_MAGIC_V2, 8) != 0 || hdr->version != 2) {
        CXXIME_LOG(L"SpellingsIndex::load unsupported format");
        unload();
        return false;
    }

    node_count_ = hdr->node_count;
    nodes_ = data_ + hdr->nodes_offset;
    strings_ = data_ + hdr->strings_offset;

    // Build offset table
    node_offsets_ = std::make_unique<uint32_t[]>(node_count_);
    const char* p = nodes_;
    const char* const end = data_ + data_size_;
    for (uint32_t i = 0; i < node_count_; ++i) {
        if (p < data_ || end - p < (std::ptrdiff_t)NODE_HEADER_SIZE) {
            CXXIME_LOG(L"SpellingsIndex::load node table out of bounds");
            unload();
            return false;
        }
        node_offsets_[i] = (uint32_t)(p - nodes_);
        uint8_t ns = *(const uint8_t*)(p + 8);
        uint8_t nc = *(const uint8_t*)(p + 9);
        has_spelling_entries_ = has_spelling_entries_ || ns != 0;
        p += NODE_HEADER_SIZE + ns * SPELLING_SIZE + nc * CHILD_SIZE;
        if (p > end) {
            CXXIME_LOG(L"SpellingsIndex::load node table out of bounds");
            unload();
            return false;
        }
    }

    CXXIME_LOG(L"SpellingsIndex::load v2 trie nodes=%u", node_count_);
    return node_count_ > 0;
}

void SpellingsIndex::unload() {
    file_.close();
    data_ = nullptr;
    nodes_ = nullptr;
    strings_ = nullptr;
    node_count_ = 0;
    has_spelling_entries_ = false;
    data_size_ = 0;
    node_offsets_.reset();
}

// A spelling is usable when it is not fuzzy (or fuzzy pinyin is on) and every fuzzy group it
// needs is enabled. `allowed_groups` is 0 when fuzzy pinyin is off.
static bool spelling_allowed(const char* spelling, bool enable_fuzzy, uint8_t allowed_groups) {
    const uint8_t type = *(const uint8_t*)(spelling + 8);
    const uint8_t needed = *(const uint8_t*)(spelling + 9);
    if (!enable_fuzzy && type == kFuzzySpelling) {
        return false;
    }
    return (needed & ~allowed_groups) == 0;
}

// v2 trie prefix search: O(k) walk
static std::vector<SpellingMatch> trie_prefix_search(
    const char* nodes, const char* strings, const uint32_t* node_offsets,
    uint32_t node_count, std::string_view prefix, bool enable_fuzzy, uint8_t allowed_groups) {

    std::vector<SpellingMatch> results;
    const uint32_t prefix_len = (uint32_t)prefix.size();
    const char* prefix_data = prefix.data();
    uint32_t prefix_pos = 0;
    uint32_t current_node = 0;

    while (current_node < node_count) {
        const char* node = nodes + node_offsets[current_node];
        uint32_t key_len = *(const uint32_t*)(node + 4);
        uint8_t ns = *(const uint8_t*)(node + 8);
        uint8_t nc = *(const uint8_t*)(node + 9);

        if (prefix_pos + key_len > prefix_len)
            break;

        const char* key = strings + *(const uint32_t*)(node);
        if (key_len > 0 && std::memcmp(key, prefix_data + prefix_pos, key_len) != 0)
            break;

        prefix_pos += key_len;

        // Collect spellings
        const char* sp = node + NODE_HEADER_SIZE;
        for (uint8_t i = 0; i < ns; ++i) {
            const int type = *(const uint8_t*)(sp + 8);
            if (!spelling_allowed(sp, enable_fuzzy, allowed_groups)) {
                sp += SPELLING_SIZE;
                continue;
            }
            SpellingMatch m;
            m.syllable.assign(strings + *(const uint32_t*)(sp),
                              *(const uint32_t*)(sp + 4));
            m.type = type;
            m.credibility = *(const float*)(sp + 10);
            m.input_key_len = prefix_pos;
            results.push_back(std::move(m));
            sp += SPELLING_SIZE;
        }

        if (prefix_pos >= prefix_len)
            break;

        // Find child
        uint8_t next_char = (uint8_t)prefix_data[prefix_pos];
        const char* ch = sp;
        bool found = false;
        for (uint8_t i = 0; i < nc; ++i) {
            if (*(const uint8_t*)(ch) == next_char) {
                current_node = *(const uint32_t*)(ch + 4);
                found = true;
                break;
            }
            ch += CHILD_SIZE;
        }
        if (!found)
            break;
    }

    return results;
}

static void append_trie_completions(
    const char* nodes, const char* strings, const uint32_t* node_offsets,
    uint32_t node_count, uint32_t start_node, uint32_t start_key_length,
    uint32_t prefix_length, bool enable_fuzzy, uint8_t allowed_groups,
    std::vector<SpellingMatch>* results) {
    struct PendingNode {
        uint32_t index;
        uint32_t key_length;
    };

    std::vector<PendingNode> pending = {{start_node, start_key_length}};
    while (!pending.empty()) {
        const PendingNode current = pending.back();
        pending.pop_back();
        if (current.index >= node_count) {
            continue;
        }

        const char* node = nodes + node_offsets[current.index];
        const uint8_t spelling_count = *(const uint8_t*)(node + 8);
        const uint8_t child_count = *(const uint8_t*)(node + 9);
        const char* spelling = node + NODE_HEADER_SIZE;
        if (current.key_length > prefix_length) {
            for (uint8_t i = 0; i < spelling_count; ++i) {
                const int type = *(const uint8_t*)(spelling + 8);
                if (!spelling_allowed(spelling, enable_fuzzy, allowed_groups)) {
                    spelling += SPELLING_SIZE;
                    continue;
                }
                SpellingMatch match;
                match.syllable.assign(strings + *(const uint32_t*)(spelling),
                                      *(const uint32_t*)(spelling + 4));
                match.type = type;
                match.credibility = *(const float*)(spelling + 10);
                match.input_key_len = current.key_length;
                results->push_back(std::move(match));
                spelling += SPELLING_SIZE;
            }
        } else {
            spelling += spelling_count * SPELLING_SIZE;
        }

        const char* child = spelling;
        for (uint8_t i = 0; i < child_count; ++i) {
            const uint32_t child_index = *(const uint32_t*)(child + 4);
            if (child_index < node_count) {
                const char* child_node = nodes + node_offsets[child_index];
                const uint32_t child_key_length = *(const uint32_t*)(child_node + 4);
                pending.push_back({child_index, current.key_length + child_key_length});
            }
            child += CHILD_SIZE;
        }
    }
}

static std::vector<SpellingMatch> trie_completion_search(
    const char* nodes, const char* strings, const uint32_t* node_offsets,
    uint32_t node_count, std::string_view prefix, bool enable_fuzzy, uint8_t allowed_groups) {
    std::vector<SpellingMatch> results;
    const uint32_t prefix_length = (uint32_t)prefix.size();
    uint32_t prefix_position = 0;
    uint32_t current_node = 0;

    while (current_node < node_count) {
        const char* node = nodes + node_offsets[current_node];
        const uint32_t key_length = *(const uint32_t*)(node + 4);
        const uint8_t spelling_count = *(const uint8_t*)(node + 8);
        const uint8_t child_count = *(const uint8_t*)(node + 9);
        const uint32_t remaining = prefix_length - prefix_position;
        const uint32_t compare_length = std::min(key_length, remaining);
        const char* key = strings + *(const uint32_t*)(node);
        if (compare_length > 0 &&
            std::memcmp(key, prefix.data() + prefix_position, compare_length) != 0) {
            return results;
        }

        const uint32_t full_key_length = prefix_position + key_length;
        if (remaining <= key_length) {
            append_trie_completions(nodes, strings, node_offsets, node_count,
                current_node, full_key_length, prefix_length, enable_fuzzy,
                allowed_groups, &results);
            return results;
        }
        prefix_position = full_key_length;

        const char* child = node + NODE_HEADER_SIZE + spelling_count * SPELLING_SIZE;
        const uint8_t next_character = (uint8_t)prefix[prefix_position];
        bool found = false;
        for (uint8_t i = 0; i < child_count; ++i) {
            if (*(const uint8_t*)child == next_character) {
                current_node = *(const uint32_t*)(child + 4);
                found = true;
                break;
            }
            child += CHILD_SIZE;
        }
        if (!found) {
            return results;
        }
    }
    return results;
}

std::vector<SpellingMatch> SpellingsIndex::prefix_search(std::string_view prefix,
                                                         bool enable_fuzzy,
                                                         uint8_t fuzzy_groups) const {
    if (!data_ || prefix.empty())
        return {};

    std::vector<SpellingMatch> results =
        trie_prefix_search(nodes_, strings_, node_offsets_.get(), node_count_, prefix,
                           enable_fuzzy, enable_fuzzy ? fuzzy_groups : 0);

    return results;
}

std::vector<SpellingMatch> SpellingsIndex::completion_search(std::string_view prefix,
                                                             bool enable_fuzzy,
                                                             uint8_t fuzzy_groups) const {
    if (!data_ || prefix.empty())
        return {};

    std::vector<SpellingMatch> results =
        trie_completion_search(nodes_, strings_, node_offsets_.get(), node_count_, prefix,
                               enable_fuzzy, enable_fuzzy ? fuzzy_groups : 0);
    return results;
}

bool SpellingsIndex::create_test_trie(const std::string& path,
    const std::vector<std::tuple<std::string, std::string, int, float>>& entries) {

    // Build a simple trie in memory
    struct Node {
        std::string key;
        std::vector<std::tuple<std::string, int, float>> spellings;
        std::vector<std::pair<uint8_t, size_t>> children;  // (first_char, child_index)
    };
    std::vector<Node> nodes;
    nodes.push_back(Node{});  // root

    for (auto& [input, syll, stype, cred] : entries) {
        size_t cur = 0;
        size_t pos = 0;
        while (pos < input.size()) {
            uint8_t fc = (uint8_t)input[pos];
            bool found = false;
            for (auto& [c, ci] : nodes[cur].children) {
                if (c == fc) {
                    // Check how much of the node's key matches
                    Node child = nodes[ci];
                    size_t match = 0;
                    while (pos + match < input.size() && match < child.key.size() &&
                           input[pos + match] == child.key[match])
                        ++match;

                    if (match == child.key.size()) {
                        // Fully consumed node key
                        pos += match;
                        cur = ci;
                        found = true;
                        break;
                    } else if (match > 0) {
                        // Partial match: split node
                        size_t split_idx = nodes.size();
                        nodes.push_back(Node{child.key.substr(0, match)});

                        // Old child becomes sub-child
                        size_t old_idx = nodes.size();
                        nodes.push_back(Node{child.key.substr(match), child.spellings, child.children});
                        nodes[split_idx].children.push_back({(uint8_t)child.key[match], old_idx});

                        // Replace old child with split
                        for (auto& [cc, ci2] : nodes[cur].children) {
                            if (ci2 == ci) { ci2 = split_idx; break; }
                        }

                        pos += match;
                        cur = split_idx;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                // New child
                size_t new_idx = nodes.size();
                nodes.push_back(Node{input.substr(pos)});
                nodes[cur].children.push_back({fc, new_idx});
                cur = new_idx;
                pos = input.size();
            }
        }
        nodes[cur].spellings.push_back({syll, stype, cred});
    }

    // Serialize
    std::string string_data;
    auto intern = [&string_data](const std::string& s) -> std::pair<uint32_t, uint32_t> {
        uint32_t off = (uint32_t)string_data.size();
        string_data += s;
        return {off, (uint32_t)s.size()};
    };

    // Assign indices via BFS
    std::vector<size_t> order;
    std::vector<size_t> q = {0};
    while (!q.empty()) {
        size_t n = q.front(); q.erase(q.begin());
        order.push_back(n);
        for (auto& [fc, ci] : nodes[n].children)
            q.push_back(ci);
    }

    std::vector<uint32_t> node_idx(nodes.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        node_idx[order[i]] = i;

    // Write binary
    std::string node_data;
    for (size_t idx : order) {
        auto& n = nodes[idx];
        auto [ko, kl] = intern(n.key);
        uint8_t ns = (uint8_t)n.spellings.size();
        uint8_t nc = (uint8_t)n.children.size();

        // Header
        node_data.append((const char*)&ko, 4);
        node_data.append((const char*)&kl, 4);
        node_data += (char)ns;
        node_data += (char)nc;
        node_data += '\0';
        node_data += '\0';

        // Spellings
        for (auto& [syll, stype, cred] : n.spellings) {
            auto [so, sl] = intern(syll);
            node_data.append((const char*)&so, 4);
            node_data.append((const char*)&sl, 4);
            node_data += (char)(uint8_t)(stype & 0xFF);         // type
            node_data += (char)(uint8_t)((stype >> 8) & 0xFF);  // fuzzy groups
            node_data.append((const char*)&cred, 4);
        }

        // Children
        for (auto& [fc, ci] : n.children) {
            uint32_t ni = node_idx[ci];
            node_data += (char)fc;
            node_data += '\0';
            node_data += '\0';
            node_data += '\0';
            node_data.append((const char*)&ni, 4);
        }
    }

    uint32_t entries_offset = 28;  // header size
    uint32_t strings_offset = entries_offset + (uint32_t)node_data.size();

    MappedFile::rename_away(path);  // a loaded trie is mapped: move it aside, do not truncate
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD written;
    char hdr[28];
    memcpy(hdr, SPELLINGS_MAGIC_V2, 8);
    uint32_t ver = 2;
    memcpy(hdr + 8, &ver, 4);
    // Only reachable nodes are serialized (splitting leaves the replaced node orphaned).
    uint32_t nc = (uint32_t)order.size();
    uint32_t sdsize = (uint32_t)string_data.size();
    memcpy(hdr + 12, &nc, 4);
    memcpy(hdr + 16, &sdsize, 4);
    memcpy(hdr + 20, &entries_offset, 4);
    memcpy(hdr + 24, &strings_offset, 4);
    WriteFile(hFile, hdr, 28, &written, nullptr);
    WriteFile(hFile, node_data.data(), (DWORD)node_data.size(), &written, nullptr);
    WriteFile(hFile, string_data.data(), (DWORD)string_data.size(), &written, nullptr);
    CloseHandle(hFile);
    return true;
}

} // namespace cxxime
