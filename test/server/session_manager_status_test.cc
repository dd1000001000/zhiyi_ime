// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <windows.h>

#include <cxxime/data_path.h>
#include <cxxime/dictionary_manifest.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/short_code_cache.h>
#include <cxxime/spellings_index.h>

#include "session_manager.h"
#include "support/testutil.h"
#include "support/topn_test_data.h"
#include "support/wubi_index_test_data.h"

static char temp_path[MAX_PATH] = {};

using TestDictEntry = std::tuple<std::string, std::string, int>;

static bool _status_init = []() {
    GetTempPathA(MAX_PATH, temp_path);
    return true;
}();

static std::string make_temp_path(const char* name) {
    return std::string(temp_path) + "\\" + name;
}

static void setup_test_paths() {
    static std::once_flag once;
    std::call_once(once, []() {
        cxxime::set_data_dir(CXXIME_DATA_DIR);
        std::string user_data = std::string(temp_path) + "zhiyi-session-status-" +
                                std::to_string(GetCurrentProcessId());
        CreateDirectoryA(user_data.c_str(), nullptr);
        cxxime::set_user_data_dir(user_data);
    });
}

struct ManifestEntry {
    const char* role;
    std::string path;
};

static std::string basename_of(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

static std::vector<std::string> split_syllables(const std::string& code) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= code.size()) {
        size_t end = code.find(':', start);
        std::string item = end == std::string::npos
            ? code.substr(start)
            : code.substr(start, end - start);
        if (!item.empty())
            result.push_back(item);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return result;
}

static std::string input_key_for_code(const std::string& code) {
    std::string key;
    key.reserve(code.size());
    for (char c : code) {
        if (c != ':')
            key.push_back(c);
    }
    return key;
}

static void write_u32(std::ofstream& out, uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

static void write_test_id_index(const std::string& path,
                                const std::vector<TestDictEntry>& entries) {
    auto sorted = entries;
    std::sort(sorted.begin(), sorted.end());

    std::vector<std::string> syllabary;
    std::vector<std::pair<std::vector<uint32_t>, uint32_t>> id_entries;
    for (uint32_t entry_index = 0; entry_index < sorted.size(); ++entry_index) {
        const auto& code = std::get<0>(sorted[entry_index]);
        std::vector<uint32_t> ids;
        for (const auto& syllable : split_syllables(code)) {
            auto it = std::find(syllabary.begin(), syllabary.end(), syllable);
            if (it == syllabary.end()) {
                syllabary.push_back(syllable);
                ids.push_back(static_cast<uint32_t>(syllabary.size() - 1));
            } else {
                ids.push_back(static_cast<uint32_t>(it - syllabary.begin()));
            }
        }
        if (!ids.empty())
            id_entries.push_back({ids, entry_index});
    }
    std::sort(id_entries.begin(), id_entries.end());

    std::string syllable_data;
    std::vector<uint32_t> syllable_offsets;
    for (const auto& syllable : syllabary) {
        syllable_offsets.push_back(static_cast<uint32_t>(syllable_data.size()));
        syllable_data.append(syllable);
        syllable_data.push_back('\0');
    }

    std::string id_data;
    std::vector<uint32_t> id_offsets;
    for (const auto& entry : id_entries) {
        id_offsets.push_back(static_cast<uint32_t>(id_data.size()));
        uint32_t count = static_cast<uint32_t>(entry.first.size());
        id_data.append(reinterpret_cast<const char*>(&count), sizeof(count));
        for (uint32_t id : entry.first)
            id_data.append(reinterpret_cast<const char*>(&id), sizeof(id));
        id_data.append(reinterpret_cast<const char*>(&entry.second), sizeof(entry.second));
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write("CXIDX\0\0\0", 8);
    write_u32(out, 3);
    write_u32(out, static_cast<uint32_t>(syllabary.size()));
    write_u32(out, static_cast<uint32_t>(syllable_data.size()));
    write_u32(out, static_cast<uint32_t>(id_entries.size()));
    write_u32(out, static_cast<uint32_t>(id_data.size()));
    for (uint32_t offset : syllable_offsets)
        write_u32(out, offset);
    out.write(syllable_data.data(), static_cast<std::streamsize>(syllable_data.size()));
    for (uint32_t offset : id_offsets)
        write_u32(out, offset);
    out.write(id_data.data(), static_cast<std::streamsize>(id_data.size()));
    out.close();
    ASSERT_TRUE(out.good());
}

static void write_manifest_for_dicts(const std::string& dict_path,
                                     const std::vector<ManifestEntry>& entries) {
    std::string manifest_path = cxxime::dictionary_manifest_path_for_dict(dict_path);
    std::string tmp_path = manifest_path + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"generation\": \"test\",\n"
        << "  \"files\": [\n";

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        std::string hash;
        ASSERT_TRUE(cxxime::compute_file_sha256(entry.path, hash));

        WIN32_FILE_ATTRIBUTE_DATA data = {};
        ASSERT_TRUE(GetFileAttributesExA(entry.path.c_str(), GetFileExInfoStandard, &data));
        uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;

        out << "    {\"role\":\"" << entry.role << "\",\"path\":\"" << basename_of(entry.path)
            << "\",\"size\":" << size << ",\"sha256\":\"" << hash
            << "\",\"required\":true}";
        if (i + 1 < entries.size())
            out << ",";
        out << "\n";
    }

    out << "  ]\n"
        << "}\n";
    out.close();
    ASSERT_TRUE(out.good());
    ASSERT_TRUE(MoveFileExA(tmp_path.c_str(), manifest_path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}

static void create_pinyin_bundle_files(const std::string& dict_path,
                                       const std::vector<TestDictEntry>& entries) {
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dict_path, entries));
    write_test_id_index(dict_path + ".idx", entries);

    std::vector<std::tuple<std::string, std::string, int, float>> spellings;
    std::vector<std::pair<std::string, std::vector<cxxime::Candidate>>> topn;
    for (const auto& entry : entries) {
        std::string key = input_key_for_code(std::get<0>(entry));
        spellings.push_back({key, std::get<0>(entry), cxxime::kNormalSpelling, 0.0f});

        cxxime::Candidate candidate;
        candidate.text = std::get<1>(entry);
        candidate.frequency = std::get<2>(entry);
        candidate.code = std::get<0>(entry);
        candidate.syllables = std::get<0>(entry);
        topn.push_back({key, {candidate}});
    }
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(dict_path + ".spellings.bin",
                                                          spellings));
    ASSERT_TRUE(cxxime::test::create_test_topn(
        dict_path + ".topn.bin", dict_path, topn));
}

static std::string setup_test_dict() {
    setup_test_paths();
    std::string dict_path = make_temp_path("test_status_dict.bin");
    std::string wubi_path = make_temp_path("test_status_wubi.bin");
    create_pinyin_bundle_files(dict_path, {
        {"ni", "你", 1000},
        {"hao", "好", 800},
    });
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {
        {"aaaa", "工", 1000},
    }));
    ASSERT_TRUE(cxxime::test::create_test_wubi_index(
        wubi_path + ".idx", {{"aaaa", "工", 1000}}));
    write_manifest_for_dicts(dict_path, {
        {"pinyin_dict", dict_path},
        {"pinyin_idx", dict_path + ".idx"},
        {"pinyin_spellings", dict_path + ".spellings.bin"},
        {"pinyin_topn", dict_path + ".topn.bin"},
        {"wubi_dict", wubi_path},
        {"wubi_prefix_index", wubi_path + ".idx"},
    });
    return dict_path;
}

static std::shared_ptr<const cxxime::Config> setup_capslock_config() {
    auto config = std::make_shared<cxxime::Config>();
    config->ascii_switch_key["Caps_Lock"] = "clear";
    return config;
}

// ============================================================
// Toggle Tests
// ============================================================

TEST(SessionStatus, toggles_update_flags_and_revision) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    auto [st0, s0] = mgr.get_ime_status(id);
    ASSERT_EQ(st0, cxxime::IPCStatus::OK);
    ASSERT_EQ(s0.chinese_mode(), true);
    ASSERT_EQ(s0.revision, (uint64_t)0);

    auto [st1, s1] = mgr.toggle_chinese(id);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(s1.chinese_mode(), false);
    ASSERT_EQ(s1.revision, (uint64_t)1);

    auto [st2, s2] = mgr.toggle_chinese(id);
    ASSERT_EQ(s2.chinese_mode(), true);
    ASSERT_EQ(s2.revision, (uint64_t)2);

    auto [shape_status, shape] = mgr.toggle_shape(id);
    ASSERT_EQ(shape_status, cxxime::IPCStatus::OK);
    ASSERT_TRUE(shape.full_shape());
    ASSERT_EQ(shape.revision, (uint64_t)3);

    auto [punct_status, punct] = mgr.toggle_punct(id);
    ASSERT_EQ(punct_status, cxxime::IPCStatus::OK);
    ASSERT_TRUE(!punct.chinese_punct());
    ASSERT_EQ(punct.revision, (uint64_t)4);
}

TEST(SessionStatus, set_chinese_mode_is_idempotent) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();

    mgr.toggle_shape(id);
    mgr.toggle_punct(id);
    mgr.switch_input_mode(id, cxxime::InputMode::WUBI);
    auto [before_status, before] = mgr.get_ime_status(id);
    ASSERT_EQ(before_status, cxxime::IPCStatus::OK);

    auto first = mgr.set_chinese_mode(id, false);
    ASSERT_EQ(first.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(first.ime_status.chinese_mode(), false);
    ASSERT_EQ(first.ime_status.full_shape(), true);
    ASSERT_EQ(first.ime_status.chinese_punct(), false);
    ASSERT_EQ(first.ime_status.input_mode, cxxime::InputMode::WUBI);
    ASSERT_EQ(first.ime_status.revision, before.revision + 1);

    auto second = mgr.set_chinese_mode(id, false);
    ASSERT_EQ(second.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(second.ime_status.chinese_mode(), false);
    ASSERT_EQ(second.ime_status.revision, first.ime_status.revision);

    auto third = mgr.set_chinese_mode(id, true);
    ASSERT_EQ(third.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(third.ime_status.chinese_mode(), true);
    ASSERT_EQ(third.ime_status.revision, first.ime_status.revision + 1);
}

TEST(SessionStatus, switch_input_mode_sets_target) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    auto [st0, s0] = mgr.get_ime_status(id);
    ASSERT_EQ(s0.input_mode, cxxime::InputMode::PINYIN);

    auto [st1, s1] = mgr.switch_input_mode(id, cxxime::InputMode::WUBI);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(s1.input_mode, cxxime::InputMode::WUBI);
    ASSERT_EQ(s1.revision, (uint64_t)1);

    // Mixed input was removed: a request for it (old clients) lands on pinyin.
    auto [st2, s2] = mgr.switch_input_mode(id, cxxime::InputMode::MIXED);
    ASSERT_EQ(s2.input_mode, cxxime::InputMode::PINYIN);
    ASSERT_EQ(s2.revision, (uint64_t)2);

    auto [st3, s3] = mgr.switch_input_mode(id, cxxime::InputMode::WUBI);
    ASSERT_EQ(s3.input_mode, cxxime::InputMode::WUBI);
    ASSERT_EQ(s3.revision, (uint64_t)3);
}

TEST(SessionStatus, style_shortcut_toggles_shared_pinyin_style_once_per_press) {
    auto config = std::make_shared<cxxime::Config>();
    SessionManager mgr;
    ASSERT_TRUE(mgr.initialize(setup_test_dict(), config));
    uint32_t id = mgr.create_session();
    uint32_t other = mgr.create_session();

    // Sessions start in Chinese pinyin mode, where the style shortcut (default Ctrl+Space)
    // switches full pinyin / initials.
    cxxime::KeyEvent shortcut;
    shortcut.keycode = VK_SPACE;
    shortcut.set_ctrl();
    auto first = mgr.process_key(id, shortcut);
    ASSERT_EQ(first.result, cxxime::ProcessResult::TOGGLE_PINYIN_STYLE);
    ASSERT_TRUE(first.ime_status.pinyin_initials());

    auto repeated = mgr.process_key(id, shortcut);
    ASSERT_EQ(repeated.result, cxxime::ProcessResult::INPUT_MODE_SHORTCUT_HANDLED);
    ASSERT_TRUE(repeated.ime_status.pinyin_initials());

    shortcut.is_key_up = true;
    shortcut.modifiers = 0;
    auto released = mgr.process_key(id, shortcut);
    ASSERT_EQ(released.result, cxxime::ProcessResult::INPUT_MODE_SHORTCUT_HANDLED);

    // The style is shared by all sessions.
    cxxime::KeyEvent letter;
    letter.keycode = 'N';
    letter.is_key_up = true;
    auto other_status = mgr.process_key(other, letter);
    ASSERT_TRUE(other_status.ime_status.pinyin_initials());

    shortcut.is_key_up = false;
    shortcut.set_ctrl();
    auto second = mgr.process_key(id, shortcut);
    ASSERT_EQ(second.result, cxxime::ProcessResult::TOGGLE_PINYIN_STYLE);
    ASSERT_TRUE(!second.ime_status.pinyin_initials());
}

TEST(SessionStatus, input_mode_shortcut_is_disabled_by_default) {
    SessionManager mgr;
    ASSERT_TRUE(mgr.initialize(setup_test_dict()));
    uint32_t id = mgr.create_session();

    cxxime::KeyEvent shortcut;
    shortcut.keycode = 'M';
    shortcut.set_ctrl();
    shortcut.set_shift();
    auto result = mgr.process_key(id, shortcut);
    ASSERT_EQ(result.result, cxxime::ProcessResult::REJECTED);
    ASSERT_EQ(result.ime_status.input_mode, cxxime::InputMode::PINYIN);
}

TEST(SessionStatus, unrelated_config_update_preserves_pending_input_mode) {
    auto initial_config = std::make_shared<cxxime::Config>();
    SessionManager mgr;
    ASSERT_TRUE(mgr.initialize(setup_test_dict(), initial_config));
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    auto [switch_status, switched] = mgr.switch_input_mode(id, cxxime::InputMode::WUBI);
    ASSERT_EQ(switch_status, cxxime::IPCStatus::OK);
    ASSERT_EQ(switched.input_mode, cxxime::InputMode::WUBI);

    auto unrelated_config = std::make_shared<cxxime::Config>(*initial_config);
    unrelated_config->status_window.x = 123;
    mgr.apply_config(unrelated_config);

    auto [unrelated_status, after_unrelated] = mgr.get_ime_status(id);
    ASSERT_EQ(unrelated_status, cxxime::IPCStatus::OK);
    ASSERT_EQ(after_unrelated.input_mode, cxxime::InputMode::WUBI);
}

TEST(SessionStatus, sync_caps_lock_sets_current_state) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    auto [st0, s0] = mgr.get_ime_status(id);
    ASSERT_EQ(s0.caps_lock(), false);

    auto [st1, caps_on] = mgr.sync_caps_lock(id, true);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(caps_on.caps_lock(), true);
    auto [st2, s1] = mgr.get_ime_status(id);
    ASSERT_EQ(s1.caps_lock(), true);

    mgr.sync_caps_lock(id, true);
    auto [st3, s2] = mgr.get_ime_status(id);
    ASSERT_EQ(s2.caps_lock(), true);

    mgr.sync_caps_lock(id, false);
    auto [st4, s3] = mgr.get_ime_status(id);
    ASSERT_EQ(s3.caps_lock(), false);
}

TEST(SessionStatus, sync_caps_lock_enables_ascii_overlay) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict(), setup_capslock_config());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    auto [st1, s1] = mgr.sync_caps_lock(id, true);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(s1.caps_lock(), true);
    ASSERT_EQ(s1.chinese_mode(), false);

    cxxime::KeyEvent letter;
    letter.keycode = 'N';
    letter.set_caps_lock();
    auto r = mgr.process_key(id, letter);
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r.result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(r.commit_text, "N");
    ASSERT_TRUE(!r.composing);

    auto [st2, s2] = mgr.sync_caps_lock(id, false);
    ASSERT_EQ(st2, cxxime::IPCStatus::OK);
    ASSERT_EQ(s2.caps_lock(), false);
    ASSERT_EQ(s2.chinese_mode(), true);
}

TEST(SessionStatus, first_key_with_caps_lock_on_enables_ascii_overlay) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict(), setup_capslock_config());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    cxxime::KeyEvent letter;
    letter.keycode = 'N';
    letter.set_caps_lock();

    auto r = mgr.process_key(id, letter);
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r.result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(r.commit_text, "N");
    ASSERT_TRUE(!r.composing);
    ASSERT_EQ(r.ime_status.caps_lock(), true);
    ASSERT_EQ(r.ime_status.chinese_mode(), false);
    }

TEST(SessionStatus, caps_lock_key_off_restores_chinese_overlay) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict(), setup_capslock_config());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    auto [st1, s1] = mgr.sync_caps_lock(id, true);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(s1.caps_lock(), true);
    ASSERT_EQ(s1.chinese_mode(), false);

    cxxime::KeyEvent caps_off;
    caps_off.keycode = VK_CAPITAL;
    caps_off.is_key_up = false;
    auto r = mgr.process_key(id, caps_off);
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r.ime_status.caps_lock(), false);
    ASSERT_EQ(r.ime_status.chinese_mode(), true);
}

TEST(SessionStatus, caps_lock_key_up_does_not_override_key_down_state) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict(), setup_capslock_config());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    cxxime::KeyEvent caps_on_down;
    caps_on_down.keycode = VK_CAPITAL;
    caps_on_down.is_key_up = false;
    caps_on_down.set_caps_lock();
    auto r1 = mgr.process_key(id, caps_on_down);
    ASSERT_EQ(r1.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r1.ime_status.caps_lock(), true);
    ASSERT_EQ(r1.ime_status.chinese_mode(), false);

    cxxime::KeyEvent stale_caps_on_up;
    stale_caps_on_up.keycode = VK_CAPITAL;
    stale_caps_on_up.is_key_up = true;
    auto r2 = mgr.process_key(id, stale_caps_on_up);
    ASSERT_EQ(r2.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r2.ime_status.caps_lock(), true);
    ASSERT_EQ(r2.ime_status.chinese_mode(), false);

    cxxime::KeyEvent caps_off_down;
    caps_off_down.keycode = VK_CAPITAL;
    caps_off_down.is_key_up = false;
    auto r3 = mgr.process_key(id, caps_off_down);
    ASSERT_EQ(r3.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r3.ime_status.caps_lock(), false);
    ASSERT_EQ(r3.ime_status.chinese_mode(), true);

    cxxime::KeyEvent stale_caps_off_up;
    stale_caps_off_up.keycode = VK_CAPITAL;
    stale_caps_off_up.is_key_up = true;
    stale_caps_off_up.set_caps_lock();
    auto r4 = mgr.process_key(id, stale_caps_off_up);
    ASSERT_EQ(r4.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r4.ime_status.caps_lock(), false);
    ASSERT_EQ(r4.ime_status.chinese_mode(), true);
}

TEST(SessionStatus, get_ime_status) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    mgr.toggle_chinese(id);
    mgr.toggle_shape(id);

    auto [st, s] = mgr.get_ime_status(id);
    ASSERT_EQ(st, cxxime::IPCStatus::OK);
    ASSERT_EQ(s.chinese_mode(), false);
    ASSERT_EQ(s.full_shape(), true);
    ASSERT_EQ(s.chinese_punct(), true);
    ASSERT_EQ(s.input_mode, cxxime::InputMode::PINYIN);
    ASSERT_EQ(s.revision, (uint64_t)2);
}

// ============================================================
// Invalid Session Tests
// ============================================================

TEST(SessionStatus, status_commands_reject_an_unknown_session) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    ASSERT_EQ(mgr.toggle_chinese(999).first, cxxime::IPCStatus::ERR_INVALID_SESSION);
    ASSERT_EQ(mgr.toggle_shape(999).first, cxxime::IPCStatus::ERR_INVALID_SESSION);
    ASSERT_EQ(mgr.toggle_punct(999).first, cxxime::IPCStatus::ERR_INVALID_SESSION);
    ASSERT_EQ(mgr.switch_input_mode(999, cxxime::InputMode::PINYIN).first,
              cxxime::IPCStatus::ERR_INVALID_SESSION);
    ASSERT_EQ(mgr.get_ime_status(999).first, cxxime::IPCStatus::ERR_INVALID_SESSION);
}

// ============================================================
// Multiple Sessions
// ============================================================

TEST(SessionStatus, input_state_is_session_local_while_input_mode_remains_global) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id1 = mgr.create_session();
    uint32_t id2 = mgr.create_session();
    ASSERT_GT(id1, (uint32_t)0);
    ASSERT_GT(id2, (uint32_t)0);

    mgr.toggle_chinese(id1);
    auto [st1, s1] = mgr.get_ime_status(id1);
    auto [st2, s2] = mgr.get_ime_status(id2);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(st2, cxxime::IPCStatus::OK);
    ASSERT_EQ(s1.chinese_mode(), false);
    ASSERT_EQ(s2.chinese_mode(), true);

    mgr.toggle_shape(id2);
    auto [st3, s3] = mgr.get_ime_status(id1);
    auto [st4, s4] = mgr.get_ime_status(id2);
    ASSERT_EQ(st3, cxxime::IPCStatus::OK);
    ASSERT_EQ(st4, cxxime::IPCStatus::OK);
    ASSERT_EQ(s3.full_shape(), false);
    ASSERT_EQ(s4.full_shape(), true);

    mgr.toggle_punct(id1);
    auto [st5, s5] = mgr.get_ime_status(id1);
    auto [st6, s6] = mgr.get_ime_status(id2);
    ASSERT_EQ(st5, cxxime::IPCStatus::OK);
    ASSERT_EQ(st6, cxxime::IPCStatus::OK);
    ASSERT_EQ(s5.chinese_punct(), false);
    ASSERT_EQ(s6.chinese_punct(), true);

    mgr.switch_input_mode(id1, cxxime::InputMode::WUBI);
    auto [st7, s7] = mgr.get_ime_status(id1);
    auto [st8, s8] = mgr.get_ime_status(id2);
    ASSERT_EQ(st7, cxxime::IPCStatus::OK);
    ASSERT_EQ(st8, cxxime::IPCStatus::OK);
    ASSERT_EQ(s7.input_mode, cxxime::InputMode::WUBI);
    ASSERT_EQ(s8.input_mode, cxxime::InputMode::WUBI);
    }

TEST(SessionStatus, session_language_mode_survives_other_session_changes) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t notepad = mgr.create_session();
    uint32_t desktop = mgr.create_session();
    ASSERT_GT(notepad, (uint32_t)0);
    ASSERT_GT(desktop, (uint32_t)0);

    mgr.toggle_chinese(notepad);
    auto [st1, before] = mgr.get_ime_status(notepad);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(before.chinese_mode(), false);

    auto [st2, desktop_before] = mgr.get_ime_status(desktop);
        ASSERT_EQ(st2, cxxime::IPCStatus::OK);
    ASSERT_EQ(desktop_before.chinese_mode(), true);

    mgr.toggle_chinese(desktop);
    mgr.toggle_chinese(desktop);
    auto [st3, after] = mgr.get_ime_status(notepad);
    ASSERT_EQ(st3, cxxime::IPCStatus::OK);
    ASSERT_EQ(after.chinese_mode(), false);

    cxxime::KeyEvent letter;
    letter.keycode = 'N';
    auto r = mgr.process_key(notepad, letter);
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    // Laya: with the English word list installed, English mode opens a word for completion
    // instead of committing the letter at once; either way the session stays in English mode.
    if (r.result == cxxime::ProcessResult::COMMITTED) {
        ASSERT_EQ(r.commit_text, "n");
        ASSERT_TRUE(!r.composing);
    } else {
        ASSERT_EQ(r.result, cxxime::ProcessResult::ACCEPTED);
        ASSERT_TRUE(r.composing);
    }
    ASSERT_EQ(r.ime_status.chinese_mode(), false);
    }

TEST(SessionStatus, caps_lock_overlay_restores_each_session_base_mode) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict(), setup_capslock_config());
    uint32_t id1 = mgr.create_session();
    uint32_t id2 = mgr.create_session();
    ASSERT_GT(id1, (uint32_t)0);
    ASSERT_GT(id2, (uint32_t)0);

    mgr.toggle_chinese(id1);
    auto [st1, english] = mgr.get_ime_status(id1);
    auto [st2, chinese] = mgr.get_ime_status(id2);
    ASSERT_EQ(st1, cxxime::IPCStatus::OK);
    ASSERT_EQ(st2, cxxime::IPCStatus::OK);
    ASSERT_EQ(english.chinese_mode(), false);
    ASSERT_EQ(chinese.chinese_mode(), true);

    mgr.sync_caps_lock(id2, true);
    auto [st3, english_caps_on] = mgr.get_ime_status(id1);
    auto [st4, chinese_caps_on] = mgr.get_ime_status(id2);
    ASSERT_EQ(st3, cxxime::IPCStatus::OK);
    ASSERT_EQ(st4, cxxime::IPCStatus::OK);
    ASSERT_EQ(english_caps_on.caps_lock(), true);
    ASSERT_EQ(chinese_caps_on.caps_lock(), true);
    ASSERT_EQ(english_caps_on.chinese_mode(), false);
    ASSERT_EQ(chinese_caps_on.chinese_mode(), false);

    mgr.sync_caps_lock(id1, false);
    auto [st5, english_caps_off] = mgr.get_ime_status(id1);
    auto [st6, chinese_caps_off] = mgr.get_ime_status(id2);
    ASSERT_EQ(st5, cxxime::IPCStatus::OK);
    ASSERT_EQ(st6, cxxime::IPCStatus::OK);
    ASSERT_EQ(english_caps_off.caps_lock(), false);
    ASSERT_EQ(chinese_caps_off.caps_lock(), false);
    ASSERT_EQ(english_caps_off.chinese_mode(), false);
    ASSERT_EQ(chinese_caps_off.chinese_mode(), true);
    }

RUN_ALL_TESTS()
