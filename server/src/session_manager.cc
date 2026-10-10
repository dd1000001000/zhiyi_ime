// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "session_manager.h"

#include "machine_translator.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>

#include <windows.h>

#include <json.hpp>

#include <cxxime/data_path.h>
#include <cxxime/candidate_preference.h>
#include <cxxime/candidate_presentation.h>
#include <cxxime/diagnostics_config.h>
#include <cxxime/disabled_system_lexicon.h>
#include <cxxime/english_learning.h>
#include <cxxime/experience_log.h>
#include <cxxime/dictionary_manifest.h>
#include <cxxime/glossary.h>
#include <cxxime/input_limits.h>
#include <cxxime/logging.h>
#include <cxxime/manual_candidate_order.h>
#include <cxxime/pinyin_resource.h>
#include <cxxime/pinyin_scheme.h>
#include <cxxime/query_trace.h>
#include <cxxime/translator.h>
#include <cxxime/user_data_merge.h>
#include <cxxime/user_lexicon.h>
#include <cxxime/wubi_translator.h>

namespace {

struct DictionaryResources {
    std::shared_ptr<cxxime::Dict> dict;
    std::shared_ptr<cxxime::Dict> wubi_dict;
    std::shared_ptr<const cxxime::PinyinResourceSet> pinyin_resources;
    std::string dict_path;
    std::string wubi_dict_path;
    std::string manifest_path;
};

std::string user_dict_path_for(cxxime::UserDictKind kind) {
    return cxxime::user_data_path(kind == cxxime::UserDictKind::WUBI
                                  ? "user_wubi.tsv"
                                  : "user_pinyin.tsv");
}

std::string candidate_preference_path_for(cxxime::UserDictKind kind) {
    return cxxime::user_data_path(kind == cxxime::UserDictKind::WUBI
                                  ? "learning_wubi.tsv"
                                  : "learning_pinyin.tsv");
}

std::string manual_candidate_order_path_for(cxxime::UserDictKind kind) {
    return cxxime::user_data_path(kind == cxxime::UserDictKind::WUBI
                                      ? "candidate_order_wubi.tsv"
                                      : "candidate_order_pinyin.tsv");
}

std::string disabled_system_lexicon_path_for(cxxime::UserDictKind kind) {
    return cxxime::user_data_path(kind == cxxime::UserDictKind::WUBI
                                      ? "disabled_wubi.tsv"
                                      : "disabled_pinyin.tsv");
}

struct LexiconDataFile {
    const char* name;
    cxxime::LexiconResource resource;
    cxxime::UserDictKind kind;
};

constexpr std::array<LexiconDataFile, 8> kLexiconDataFiles = {{
    {"user_pinyin.tsv", cxxime::LexiconResource::kUserLexicon, cxxime::UserDictKind::PINYIN},
    {"user_wubi.tsv", cxxime::LexiconResource::kUserLexicon, cxxime::UserDictKind::WUBI},
    {"candidate_order_pinyin.tsv", cxxime::LexiconResource::kManualCandidateOrder,
     cxxime::UserDictKind::PINYIN},
    {"candidate_order_wubi.tsv", cxxime::LexiconResource::kManualCandidateOrder,
     cxxime::UserDictKind::WUBI},
    {"learning_pinyin.tsv", cxxime::LexiconResource::kCandidatePreference,
     cxxime::UserDictKind::PINYIN},
    {"learning_wubi.tsv", cxxime::LexiconResource::kCandidatePreference,
     cxxime::UserDictKind::WUBI},
    {"disabled_pinyin.tsv", cxxime::LexiconResource::kDisabledSystemLexicon,
     cxxime::UserDictKind::PINYIN},
    {"disabled_wubi.tsv", cxxime::LexiconResource::kDisabledSystemLexicon,
     cxxime::UserDictKind::WUBI},
}};

const LexiconDataFile* find_lexicon_data_file(const std::string& name) {
    const auto found = std::find_if(kLexiconDataFiles.begin(), kLexiconDataFiles.end(),
                                    [&](const auto& item) { return name == item.name; });
    return found == kLexiconDataFiles.end() ? nullptr : &*found;
}

constexpr char kEnglishLearningFile[] = "learning_english.json";

bool known_user_data_file(const std::string& name) {
    return name == "learning_composition.tsv" || name == kEnglishLearningFile ||
           find_lexicon_data_file(name) != nullptr;
}

bool valid_lexicon_data_file_contents(const LexiconDataFile& file,
                                      const std::string& contents) {
    switch (file.resource) {
    case cxxime::LexiconResource::kUserLexicon:
        return cxxime::UserLexicon::validate_contents(contents, file.kind);
    case cxxime::LexiconResource::kCandidatePreference:
        return cxxime::CandidatePreference::validate_contents(contents);
    case cxxime::LexiconResource::kManualCandidateOrder:
        return cxxime::ManualCandidateOrder::validate_contents(contents, file.kind);
    case cxxime::LexiconResource::kDisabledSystemLexicon:
        return cxxime::DisabledSystemLexicon::validate_contents(contents);
    }
    return false;
}

bool valid_user_data_file_contents(const std::string& name, const std::string& contents) {
    if (name == "learning_composition.tsv") {
        return cxxime::CompositionLearningService::validate_contents(contents);
    }
    if (name == kEnglishLearningFile) {
        return cxxime::EnglishLearning::validate_contents(contents);
    }
    const LexiconDataFile* file = find_lexicon_data_file(name);
    return file && valid_lexicon_data_file_contents(*file, contents);
}

std::wstring utf8_path_to_wide(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                          static_cast<int>(path.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                               static_cast<int>(path.size()), &result[0], count) == count
               ? result
               : std::wstring();
}

bool read_user_data_file(const std::string& name, std::string* contents) {
    const std::wstring path = utf8_path_to_wide(cxxime::user_data_path(name.c_str()));
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            contents->clear();
            return true;
        }
        return false;
    }
    LARGE_INTEGER size = {};
    bool succeeded = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
                     size.QuadPart <= 64LL * 1024LL * 1024LL;
    if (succeeded) {
        contents->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        succeeded =
            contents->empty() || (ReadFile(file, &(*contents)[0],
                                           static_cast<DWORD>(contents->size()), &read, nullptr) &&
                                  read == contents->size());
    }
    CloseHandle(file);
    return succeeded;
}

void parse_punct_section(const nlohmann::json& j, const char* section,
                         std::unordered_map<std::string, cxxime::PunctEntry>& target) {
    if (!j.contains(section) || !j[section].is_object())
        return;
    for (auto it = j[section].begin(); it != j[section].end(); ++it) {
        std::string key = it.key();
        const nlohmann::json& val = it.value();
        if (!val.is_object()) continue;
        cxxime::PunctEntry entry{};
        if (val.contains("commit") && val["commit"].is_string()) {
            entry.type = cxxime::PunctType::COMMIT;
            entry.commit = val["commit"].get<std::string>();
        } else if (val.contains("pair") && val["pair"].is_array()) {
            entry.type = cxxime::PunctType::PAIR;
            for (const auto& item : val["pair"]) {
                if (item.is_string())
                    entry.pair.push_back(item.get<std::string>());
            }
        } else if (val.contains("alternatives") && val["alternatives"].is_array()) {
            entry.type = cxxime::PunctType::ALTERNATIVES;
            for (const auto& item : val["alternatives"]) {
                if (item.is_string())
                    entry.alternatives.push_back(item.get<std::string>());
            }
        } else {
            CXXIME_LOG(L"Punct: unknown entry type for key '%S'", key.c_str());
            continue;
        }
        target[key] = std::move(entry);
    }
}

std::string manifest_role_path(const cxxime::DictionaryManifest& manifest,
                               const char* role) {
    const auto* file = manifest.find_role(role);
    return file ? file->absolute_path : std::string{};
}

bool same_visible_status(const cxxime::ImeStatus& a, const cxxime::ImeStatus& b) {
    return a.flags == b.flags && a.input_mode == b.input_mode;
}

struct CandidateStateToken {
    uint64_t composition_revision = 0;
    cxxime::CandidatePresentationPage presentation;
    bool composing = false;
};

CandidateStateToken candidate_state_token(const cxxime::Engine& engine) {
    const cxxime::Context& context = engine.context();
    CandidateStateToken token;
    token.composition_revision = context.preedit_revision();
    token.presentation = context.translation().presentation_page();
    token.composing = context.is_composing();
    return token;
}

bool same_candidate_item(const cxxime::CandidatePresentationItem& left,
                         const cxxime::CandidatePresentationItem& right) {
    return left.text == right.text && left.hint == right.hint &&
           left.recommended == right.recommended && left.learned == right.learned;
}

bool same_candidate_page(const cxxime::CandidatePresentationPage& left,
                         const cxxime::CandidatePresentationPage& right) {
    if (left.page_index != right.page_index || left.page_offset != right.page_offset ||
        left.page_size != right.page_size ||
        left.extent.known_count != right.extent.known_count ||
        left.extent.state != right.extent.state ||
        left.extent.complete != right.extent.complete ||
        left.highlighted != right.highlighted || left.items.size() != right.items.size()) {
        return false;
    }
    return std::equal(left.items.begin(), left.items.end(), right.items.begin(),
                      same_candidate_item);
}

bool same_candidate_state(const CandidateStateToken& left, const CandidateStateToken& right) {
    return left.composition_revision == right.composition_revision &&
           same_candidate_page(left.presentation, right.presentation) &&
           left.composing == right.composing;
}

void advance_candidate_revision(SessionEntry& entry, const CandidateStateToken& previous,
                                bool force = false) {
    if (entry.candidate_revision == 0 ||
        (!(force && previous.composing) &&
         same_candidate_state(previous, candidate_state_token(*entry.engine)))) {
        return;
    }
    if (entry.candidate_revision != (std::numeric_limits<uint64_t>::max)()) {
        ++entry.candidate_revision;
    }
}

bool learning_mode_shown(const cxxime::Config& config) {
    return (!config.chinese_gloss_target.empty() || !config.english_gloss_target.empty()) &&
           config.layout != "horizontal";
}

bool translator_enabled(const cxxime::Config& config) {
    return config.mt_enable && !config.mt_device.empty();
}

const std::string& gloss_target(const cxxime::Config& config, const cxxime::Candidate& candidate) {
    return candidate.source == cxxime::CandidateSource::kEnglish ? config.english_gloss_target
                                                                 : config.chinese_gloss_target;
}

// The translation the candidate window shows for `candidate`: from the language pack, or else
// one the offline translation model (machine_translator.h) has made, shown without a part of
// speech. `asked` tells whether the model was asked already (it may have found nothing).
std::string shown_gloss(const cxxime::Config& config, const cxxime::Candidate& candidate,
                        bool* asked = nullptr) {
    if (asked) *asked = false;
    if (candidate.source == cxxime::CandidateSource::kSymbol) return {};
    std::string gloss = cxxime::candidate_gloss(
        config.chinese_gloss_target, config.english_gloss_target, candidate.text,
        candidate.syllables, candidate.source == cxxime::CandidateSource::kEnglish);
    const std::string& target = gloss_target(config, candidate);
    if (gloss.empty() && translator_enabled(config) && !target.empty() &&
        MachineTranslator::instance().cached(target, candidate.text, &gloss) && asked) {
        *asked = true;
    }
    return gloss;
}

// Learning mode (docs/learning-mode.md): the translation of each candidate on the page, shown
// by the vertical candidate window.
void add_glosses(const SessionEntry& entry, cxxime::CandidatePresentationPage& page) {
    if (!entry.resources.runtime) return;
    const cxxime::Config& config = entry.resources.runtime->config();
    if (!learning_mode_shown(config)) return;
    const auto& entries = entry.engine->context().translation().entries;
    std::vector<cxxime::mt::Item> missing;
    for (std::size_t i = 0; i < page.items.size() && i < entries.size(); ++i) {
        const cxxime::Candidate& candidate = entries[i].candidate;
        bool asked = false;
        page.items[i].gloss = shown_gloss(config, candidate, &asked);
        // Not in the language pack nor asked yet: to the model, when on.
        const std::string& target = gloss_target(config, candidate);
        if (page.items[i].gloss.empty() && !asked &&
            candidate.source != cxxime::CandidateSource::kSymbol &&
            translator_enabled(config) && !target.empty() &&
            cxxime::mt::translatable(candidate.text)) {
            missing.push_back({target, candidate.text,
                               candidate.source == cxxime::CandidateSource::kEnglish ? "en" : "zh"});
        }
    }
    if (!missing.empty()) {
        MachineTranslator::instance().request(
            std::move(missing),
            entry.engine->laya_context(static_cast<std::size_t>(config.mt_context_chars)),
            config.mt_device, config.mt_idle_seconds);
    }
}

void fill_session_presentation(const SessionEntry& entry, ProcessKeyResult& result) {
    const cxxime::Context& context = entry.engine->context();
    result.composing = context.is_composing();
    result.candidate_revision = entry.candidate_revision;
    result.ime_status = entry.ime_status;
    if (!result.composing) {
        return;
    }
    std::size_t focused_input_bytes = context.active_input().size();
    std::string preferred_syllables;
    bool has_focused_selection = false;
    const bool supports_segmented_preedit =
        (entry.client_capabilities & cxxime::kClientCapabilitySegmentedPreeditPresentation) != 0;
    bool decorate_pinyin_preedit = supports_segmented_preedit &&
        context.composition_scheme() == cxxime::CompositionScheme::kPinyin;
    const cxxime::CandidateEntry* highlighted = context.candidate_entry(context.highlighted());
    if (highlighted) {
        if (const auto* action =
                std::get_if<cxxime::TextSelectionAction>(&highlighted->selection)) {
            focused_input_bytes = action->consumed_input_bytes;
            has_focused_selection = true;
            if (action->primary_variant < action->variants.size()) {
                preferred_syllables = action->variants[action->primary_variant].syllables;
            }
        }
        if (supports_segmented_preedit &&
            context.composition_scheme() == cxxime::CompositionScheme::kMixed) {
            decorate_pinyin_preedit =
                highlighted->candidate.source == cxxime::CandidateSource::kPinyin;
        }
    }
    const cxxime::CompositionPresentation composition = cxxime::derive_composition_presentation(
        context.composition(),
        entry.resources.runtime ? &entry.resources.runtime->pinyin_resources() : nullptr,
        focused_input_bytes, decorate_pinyin_preedit, preferred_syllables,
        entry.resources.runtime && entry.resources.runtime->pinyin_resources().kind() ==
                                       cxxime::PinyinSchemeKind::kShuangpin,
        !entry.resources.runtime || entry.resources.runtime->pinyin_query_policy().enable_fuzzy,
        entry.engine->pinyin_initials() && entry.engine->mode() == cxxime::InputMode::PINYIN,
        entry.resources.runtime ? entry.resources.runtime->pinyin_query_policy().fuzzy_groups
                                : cxxime::kAllFuzzyGroups);
    result.preedit = composition.display_preedit;
    result.preedit_cursor = composition.display_cursor_bytes;
    result.converted_prefix_bytes = composition.display_converted_prefix_bytes;
    result.focused_preedit_start_bytes = composition.focused_preedit_start_bytes;
    result.focused_preedit_end_bytes = composition.focused_preedit_end_bytes;
    result.preedit_presentation_flags = decorate_pinyin_preedit
            ? cxxime::preedit_presentation_flag(
                cxxime::PreeditPresentationFlag::SyllableBoundaries)
            : 0;
    if (!supports_segmented_preedit) {
        result.focused_preedit_start_bytes = result.preedit.size();
        result.focused_preedit_end_bytes = result.preedit.size();
    } else if (!has_focused_selection) {
        result.focused_preedit_start_bytes = context.translation().entries.empty()
            ? result.converted_prefix_bytes
            : result.preedit.size();
        result.focused_preedit_end_bytes = result.preedit.size();
    }
    result.presentation = context.translation().presentation_page();
    add_glosses(entry, result.presentation);
}

cxxime::InputMode next_input_mode(cxxime::InputMode mode) {
    return mode == cxxime::InputMode::PINYIN ? cxxime::InputMode::WUBI
                                             : cxxime::InputMode::PINYIN;
}

bool load_spelling_resources(const cxxime::DictionaryManifest& manifest,
                             const cxxime::PinyinSchemeDescriptor& scheme,
                             std::shared_ptr<const cxxime::PinyinResourceSet>& resources) {
    const std::string spellings_path = manifest_role_path(manifest, scheme.manifest_role);
    auto loaded = cxxime::PinyinResourceSet::create(
        scheme.id, scheme.kind, spellings_path, cxxime::PinyinSpellingRequirement::kRequired);
    if (!loaded) {
        CXXIME_LOG(L"SharedResources: %S spellings load FAILED", scheme.id);
        return false;
    }
    resources = std::move(loaded);
    return true;
}

bool load_dictionary_resources(const std::string& manifest_path, const std::string& pinyin_scheme,
                               DictionaryResources& out) {
    const auto& scheme = cxxime::resolve_pinyin_scheme(pinyin_scheme);
    const std::vector<std::string> runtime_dictionary_roles = {
        "pinyin_dict",
        "pinyin_idx",
        scheme.manifest_role,
        "pinyin_topn",
        "wubi_dict",
        "wubi_prefix_index",
    };
    cxxime::DictionaryManifest manifest;
    std::string manifest_error;
    if (!cxxime::load_dictionary_manifest(manifest_path, manifest, &manifest_error)) {
        CXXIME_LOG(L"SharedResources: manifest load FAILED: %S", manifest_error.c_str());
        return false;
    }
    if (!cxxime::validate_dictionary_manifest_files(
            manifest, runtime_dictionary_roles, &manifest_error)) {
        CXXIME_LOG(L"SharedResources: manifest validate FAILED: %S", manifest_error.c_str());
        return false;
    }

    std::string dict_path = manifest_role_path(manifest, "pinyin_dict");
    std::string dict_idx_path = manifest_role_path(manifest, "pinyin_idx");
    std::string topn_path = manifest_role_path(manifest, "pinyin_topn");
    std::string wubi_dict_path = manifest_role_path(manifest, "wubi_dict");
    std::string wubi_prefix_index_path =
        manifest_role_path(manifest, "wubi_prefix_index");

    auto loaded_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::PINYIN);
    std::string user_dict_path = user_dict_path_for(cxxime::UserDictKind::PINYIN);
    if (!loaded_dict->open_bundle(dict_path, user_dict_path, dict_idx_path, topn_path)) {
        CXXIME_LOG(L"SharedResources: dict.open FAILED");
        return false;
    }
    if (!loaded_dict->load_candidate_preferences(
            candidate_preference_path_for(cxxime::UserDictKind::PINYIN))) {
        CXXIME_LOG(L"SharedResources: pinyin candidate preferences load FAILED");
        return false;
    }
    if (!loaded_dict->load_manual_candidate_order(
            manual_candidate_order_path_for(cxxime::UserDictKind::PINYIN))) {
        CXXIME_LOG(L"SharedResources: pinyin manual candidate order load FAILED");
        return false;
    }
    if (!loaded_dict->load_disabled_system_entries(
            disabled_system_lexicon_path_for(cxxime::UserDictKind::PINYIN))) {
        CXXIME_LOG(L"SharedResources: disabled pinyin entries load FAILED");
        return false;
    }

    std::shared_ptr<const cxxime::PinyinResourceSet> loaded_pinyin_resources;
    if (!load_spelling_resources(manifest, scheme, loaded_pinyin_resources)) {
        return false;
    }

    auto loaded_wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    if (!loaded_wubi_dict->open_wubi_bundle(
            wubi_dict_path, user_dict_path_for(cxxime::UserDictKind::WUBI),
            wubi_prefix_index_path)) {
        CXXIME_LOG(L"SharedResources: required Wubi resources load FAILED");
        return false;
    }
    if (!loaded_wubi_dict->load_candidate_preferences(
            candidate_preference_path_for(cxxime::UserDictKind::WUBI))) {
        CXXIME_LOG(L"SharedResources: Wubi candidate preferences load FAILED");
        return false;
    }
    if (!loaded_wubi_dict->load_manual_candidate_order(
            manual_candidate_order_path_for(cxxime::UserDictKind::WUBI))) {
        CXXIME_LOG(L"SharedResources: Wubi manual candidate order load FAILED");
        return false;
    }
    if (!loaded_wubi_dict->load_disabled_system_entries(
            disabled_system_lexicon_path_for(cxxime::UserDictKind::WUBI))) {
        CXXIME_LOG(L"SharedResources: disabled Wubi entries load FAILED");
        return false;
    }
    CXXIME_LOG(L"SharedResources: wubi dict loaded");

    out.dict = std::move(loaded_dict);
    out.wubi_dict = std::move(loaded_wubi_dict);
    out.pinyin_resources = std::move(loaded_pinyin_resources);
    out.dict_path = dict_path;
    out.wubi_dict_path = std::move(wubi_dict_path);
    out.manifest_path = manifest_path;
    return true;
}

std::shared_ptr<const cxxime::PunctMapping> load_punctuation_mapping(const std::string& path) {
    if (path.empty()) return nullptr;

    std::ifstream file(path);
    if (!file.is_open()) {
        CXXIME_LOG(L"SharedResources: punctuation file not found: %S", path.c_str());
        return nullptr;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(file);
        auto mapping = std::make_shared<cxxime::PunctMapping>();

        parse_punct_section(j, "half_shape", mapping->half_shape);

        CXXIME_LOG(L"SharedResources: punctuation loaded (%zu entries)",
                   mapping->half_shape.size());
        return mapping;
    } catch (const nlohmann::json::exception& e) {
        CXXIME_LOG(L"SharedResources: punctuation parse error: %S", e.what());
        return nullptr;
    }
}

std::shared_ptr<const cxxime::SymbolTable> load_symbol_table(const std::string& path) {
    auto table = std::make_shared<cxxime::SymbolTable>();
    if (!table->load(path)) {
        CXXIME_LOG(L"SharedResources: symbol table not found or invalid: %S", path.c_str());
        return nullptr;
    }
    CXXIME_LOG(L"SharedResources: symbol table loaded");
    return table;
}

}  // anonymous namespace

bool SharedResources::load(const std::string& dict_path,
                           const std::shared_ptr<const cxxime::Config>& loaded_config) {
    if (!loaded_config) {
        return false;
    }
    std::string manifest_path = cxxime::dictionary_manifest_path_for_dict(dict_path);
    DictionaryResources dictionaries;
    if (!load_dictionary_resources(manifest_path, loaded_config->pinyin_scheme, dictionaries))
        return false;

    // Load punctuation mapping (non-fatal)
    std::string loaded_punct_path = cxxime::data_path("punctuation.json");
    auto loaded_punct_mapping = load_punctuation_mapping(loaded_punct_path);
    auto loaded_symbol_table = load_symbol_table(cxxime::data_path("symbols.json"));
    auto loaded_composition_learning =
        std::make_shared<cxxime::CompositionLearningService>();
    if (!loaded_composition_learning->load(
            cxxime::user_data_path("learning_composition.tsv"))) {
        CXXIME_LOG(L"%s", L"SharedResources: composition learning initialization disabled");
        loaded_composition_learning.reset();
    } else if (!loaded_composition_learning->start()) {
        CXXIME_LOG(L"%s", L"SharedResources: composition learning worker disabled");
        loaded_composition_learning.reset();
    }
    // English word mode learning (corrections picked, words kept as typed); process-wide.
    if (!cxxime::EnglishLearning::open_shared(cxxime::user_data_path("learning_english.json"))) {
        CXXIME_LOG(L"%s", L"SharedResources: English learning file unreadable, starting empty");
    }
    auto loaded_runtime = cxxime::EngineRuntimeState::create(
        *loaded_config, dictionaries.dict, dictionaries.wubi_dict,
        dictionaries.pinyin_resources, loaded_symbol_table, loaded_composition_learning);
    if (!loaded_runtime) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        this->dict_path = std::move(dictionaries.dict_path);
        wubi_dict_path = std::move(dictionaries.wubi_dict_path);
        this->manifest_path = std::move(dictionaries.manifest_path);
        runtime = std::move(loaded_runtime);
        punct_mapping = std::move(loaded_punct_mapping);
        punct_path = punct_mapping ? std::move(loaded_punct_path) : std::string{};
        pending_runtime.reset();
        ++generation;
    }

    return true;
}

SharedResourceSnapshot SharedResources::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    SharedResourceSnapshot s;
    s.runtime = runtime;
    s.punct_mapping = punct_mapping;
    return s;
}

std::shared_ptr<cxxime::Dict> SharedResources::dict_for_kind(cxxime::UserDictKind kind) const {
    std::lock_guard<std::mutex> lock(mutex);
    return dict_for_kind_locked(kind);
}

std::shared_ptr<cxxime::Dict>
SharedResources::dict_for_kind_locked(cxxime::UserDictKind kind) const {
    if (!runtime) {
        return nullptr;
    }
    return kind == cxxime::UserDictKind::WUBI ? runtime->wubi_dict_ptr()
                                              : runtime->pinyin_dict_ptr();
}

bool SharedResources::load_punctuation(const std::string& path) {
    if (path.empty()) return true;

    auto mapping = load_punctuation_mapping(path);
    if (!mapping)
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex);
        punct_mapping = std::move(mapping);
        punct_path = path;
    }
    return true;
}

bool SharedResources::reload_dictionaries() {
    std::string current_manifest_path;
    std::shared_ptr<const cxxime::EngineRuntimeState> current_runtime;
    std::uint64_t base_generation = 0;
    std::shared_ptr<cxxime::Dict> old_dict;
    std::shared_ptr<cxxime::Dict> old_wubi_dict;
    {
        std::lock_guard<std::mutex> lock(mutex);
        current_manifest_path = manifest_path;
        current_runtime = runtime;
        base_generation = generation;
        if (current_runtime) {
            old_dict = current_runtime->pinyin_dict_ptr();
            old_wubi_dict = current_runtime->wubi_dict_ptr();
        }
    }
    if (current_manifest_path.empty() || !current_runtime)
        return false;

    if ((old_dict && (!old_dict->save_user_dict() ||
                      !old_dict->save_candidate_preferences() ||
                      !old_dict->save_disabled_system_entries())) ||
        (old_wubi_dict && (!old_wubi_dict->save_user_dict() ||
                           !old_wubi_dict->save_candidate_preferences() ||
                           !old_wubi_dict->save_disabled_system_entries()))) {
        CXXIME_LOG(L"%s", L"SharedResources: user data flush before reload FAILED");
        return false;
    }

    DictionaryResources dictionaries;
    if (!load_dictionary_resources(current_manifest_path,
                                   current_runtime->config().pinyin_scheme, dictionaries))
        return false;
    auto next_runtime = cxxime::EngineRuntimeState::create(
        current_runtime->config(), dictionaries.dict, dictionaries.wubi_dict,
        dictionaries.pinyin_resources, current_runtime->symbol_table_ptr(),
        current_runtime->composition_learning_ptr());
    if (!next_runtime) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation != base_generation || runtime != current_runtime) {
            return false;
        }
        runtime = std::move(next_runtime);
        dict_path = std::move(dictionaries.dict_path);
        wubi_dict_path = std::move(dictionaries.wubi_dict_path);
        manifest_path = std::move(dictionaries.manifest_path);
        pending_runtime.reset();
        ++generation;
    }

    CXXIME_LOG(L"SharedResources: dictionaries reloaded");
    return true;
}

bool SessionManager::initialize(const std::string& dict_path,
                                const std::shared_ptr<const cxxime::Config>& config) {
    if (!shared_.load(dict_path, config)) {
        return false;
    }
    cxxime::set_diagnostics_config(config->diagnostics);
    cxxime::QueryTrace::set_enabled(config->diagnostics.trace_mode !=
                                    cxxime::DiagnosticTraceMode::kOff);
    cxxime::ExperienceLog::instance().configure(*config);
    reset_global_state(shared_.snapshot());
    return true;
}

void SessionManager::reset_global_state(const SharedResourceSnapshot& resources) {
    GlobalVisibleState state;
    if (resources.runtime) {
        state.input_mode =
            static_cast<cxxime::InputMode>(resources.runtime->config().input_mode);
        state.english_words = resources.runtime->config().english.word_mode;
        state.pinyin_initials = resources.runtime->config().pinyin_initials;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    global_state_ = state;
}

SessionManager::GlobalVisibleState SessionManager::snapshot_global_state() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return global_state_;
}

void SessionManager::commit_global_state(GlobalVisibleState next) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    global_state_ = next;
}

void SessionManager::align_session_to_global(SessionEntry& entry) {
    GlobalVisibleState state = snapshot_global_state();
    const cxxime::ImeStatus previous = entry.ime_status;

    if (entry.ime_status.input_mode != state.input_mode) {
        entry.engine->switch_mode(state.input_mode);
    }
    auto& ascii_composer = entry.engine->ascii_composer();
    if (!ascii_composer.is_temporary_ascii() || !entry.base_chinese_mode) {
        ascii_composer.set_ascii_mode(!entry.base_chinese_mode);
    }
    entry.engine->ascii_composer().sync_caps_lock(state.caps_lock,
                                                  entry.engine->context());
    entry.engine->set_english_word_mode(state.english_words);
    entry.ime_status.set_english_words(state.english_words);
    entry.engine->set_pinyin_initials(state.pinyin_initials);
    entry.ime_status.set_pinyin_initials(state.pinyin_initials);
    entry.ime_status.set_chinese_mode(state.caps_lock ? false : entry.base_chinese_mode);
    entry.ime_status.set_caps_lock(state.caps_lock);
    entry.ime_status.set_full_shape(entry.full_shape);
    entry.ime_status.set_chinese_punct(entry.chinese_punct);
    entry.ime_status.input_mode = entry.engine->mode();
    entry.ime_status.revision = same_visible_status(previous, entry.ime_status)
        ? previous.revision
        : previous.revision + 1;
    if (entry.ime_status.input_mode != state.input_mode) {
        state.input_mode = entry.ime_status.input_mode;
        commit_global_state(state);
    }
}

uint32_t SessionManager::create_session(uint64_t client_capabilities) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    auto engine = std::make_unique<cxxime::Engine>();
    auto resources = shared_.snapshot();
    if (!resources.runtime)
        return 0;

    if (!engine->initialize(resources.runtime))
        return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t id = next_id_++;
    auto entry = std::make_shared<SessionEntry>();
    entry->engine = std::move(engine);
    entry->last_activity = std::chrono::steady_clock::now();
    entry->resources = std::move(resources);
    entry->full_shape = entry->resources.runtime->config().initial_full_shape;
    entry->chinese_punct = entry->resources.runtime->config().initial_chinese_punct;
    entry->client_capabilities =
        client_capabilities & (cxxime::kClientCapabilitySegmentedSelection |
                               cxxime::kClientCapabilitySegmentedPreeditPresentation);
    const bool segmented_selection =
        (entry->client_capabilities & cxxime::kClientCapabilitySegmentedSelection) != 0;
    entry->candidate_revision = segmented_selection ? 1 : 0;
    entry->engine->set_partial_selection_enabled(segmented_selection);
    entry->ime_status.set_full_shape(entry->full_shape);
    entry->ime_status.set_chinese_punct(entry->chinese_punct);
    align_session_to_global(*entry);
    sessions_[id] = entry;
    return id;
}

void SessionManager::destroy_session(uint32_t id) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return;
        }
        entry = found->second;
    }
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->engine->clear_composition();
        entry->closing = true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
}

std::shared_ptr<SessionEntry> SessionManager::lookup_session(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        it->second->last_activity = std::chrono::steady_clock::now();
        return it->second;
    }
    return nullptr;
}

cxxime::Engine* SessionManager::get_engine(uint32_t id) {
    auto entry = lookup_session(id);
    return entry ? entry->engine.get() : nullptr;
}

size_t SessionManager::cleanup_idle_sessions(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    size_t count = 0;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second->last_activity).count();
        if (static_cast<uint32_t>(idle) > timeout_ms) {
            std::lock_guard<std::mutex> entry_lock(it->second->mutex);
            it->second->engine->clear_composition();
            it->second->closing = true;
            it = sessions_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    return count;
}

bool SharedResources::prepare_config(const std::shared_ptr<const cxxime::Config>& next_config) {
    if (!next_config) {
        return false;
    }

    std::string current_manifest_path;
    std::shared_ptr<const cxxime::EngineRuntimeState> current_runtime;
    std::uint64_t base_generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending_runtime.reset();
        current_runtime = runtime;
        base_generation = generation;
        current_manifest_path = manifest_path;
    }
    if (!current_runtime) {
        return false;
    }

    const auto& scheme = cxxime::resolve_pinyin_scheme(next_config->pinyin_scheme);
    std::shared_ptr<const cxxime::PinyinResourceSet> next_pinyin_resources =
        current_runtime->pinyin_resources_ptr();
    if (current_runtime->pinyin_resources().scheme_id() != scheme.id) {
        cxxime::DictionaryManifest manifest;
        std::string error;
        if (!cxxime::load_dictionary_manifest(current_manifest_path, manifest, &error) ||
            !cxxime::validate_dictionary_manifest_files(manifest, {scheme.manifest_role},
                                                        &error)) {
            CXXIME_LOG(L"SharedResources: scheme switch manifest validation FAILED: %S",
                       error.c_str());
            return false;
        }
        if (!load_spelling_resources(manifest, scheme, next_pinyin_resources)) {
            return false;
        }
    }

    auto next_runtime = cxxime::EngineRuntimeState::create(
        *next_config, current_runtime->pinyin_dict_ptr(), current_runtime->wubi_dict_ptr(),
        std::move(next_pinyin_resources), current_runtime->symbol_table_ptr(),
        current_runtime->composition_learning_ptr());
    if (!next_runtime) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (generation != base_generation || runtime != current_runtime) {
        return false;
    }
    pending_runtime = PendingRuntime{std::move(next_runtime), next_config, base_generation};
    return true;
}

bool SharedResources::commit_prepared_config(
    const std::shared_ptr<const cxxime::Config>& next_config) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!next_config || !pending_runtime ||
        pending_runtime->requested_config.get() != next_config.get() ||
        pending_runtime->base_generation != generation) {
        return false;
    }
    runtime = std::move(pending_runtime->runtime);
    pending_runtime.reset();
    ++generation;
    return true;
}

void SharedResources::cancel_prepared_config() {
    std::lock_guard<std::mutex> lock(mutex);
    pending_runtime.reset();
}

void apply_resource_snapshot(SessionEntry& entry, const SharedResourceSnapshot& resources) {
    if (resources.runtime && resources.runtime != entry.resources.runtime) {
        auto old_mode = entry.ime_status.input_mode;
        entry.engine->apply_runtime_state(resources.runtime);
        entry.resources.runtime = resources.runtime;
        entry.ime_status.input_mode = entry.engine->mode();
        if (old_mode != entry.ime_status.input_mode)
            entry.ime_status.revision++;
    }
    entry.resources.punct_mapping = resources.punct_mapping;
}

cxxime::IPCStatus SharedResources::add_user_entry(cxxime::UserDictKind kind,
                                                  const std::string& text,
                                                  const std::string& code,
                                                  const std::string& syllables) {
    if (text.empty() || code.empty())
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    if (!dict->add_user_entry_and_save(text, code, syllables))
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    return cxxime::IPCStatus::OK;
}

cxxime::IPCStatus SharedResources::import_user_dict(cxxime::UserDictKind kind,
                                                    const std::string& source_path) {
    if (source_path.empty()) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open()) {
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    }
    if (!dict->import_user_dict(source_path)) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    return cxxime::IPCStatus::OK;
}

cxxime::UserDictQueryResult SharedResources::query_user_entries(
    const std::string& query, cxxime::UserDictKind kind, size_t offset, size_t limit) {
    return query_lexicon_entries(cxxime::LexiconResource::kUserLexicon, query, kind, offset,
                                 limit);
}

cxxime::UserDictQueryResult SharedResources::query_lexicon_entries(
    cxxime::LexiconResource resource, const std::string& query, cxxime::UserDictKind kind,
    size_t offset, size_t limit, bool exact_text) {
    auto dict = dict_for_kind(kind);
    cxxime::UserDictQueryResult result;
    result.offset = offset;
    if (dict && dict->is_open()) {
        if (resource == cxxime::LexiconResource::kCandidatePreference) {
            result.resource_total = dict->candidate_preference_count();
            result.entries =
                dict->query_candidate_preferences(query, offset, limit, &result.match_total);
        } else if (resource == cxxime::LexiconResource::kDisabledSystemLexicon) {
            result.resource_total = dict->disabled_system_entry_count();
            result.entries =
                dict->query_disabled_system_entries(query, offset, limit, &result.match_total);
        } else {
            result.resource_total = dict->user_entry_count();
            result.entries =
                dict->query_user_entries(query, offset, limit, &result.match_total, exact_text);
        }
        result.has_more = offset < result.match_total &&
                          result.entries.size() < result.match_total - offset;
    }
    return result;
}

cxxime::UserDictQueryResult SharedResources::query_disabled_system_entry_status(
    cxxime::UserDictKind kind, const std::vector<std::string>& texts) {
    std::lock_guard<std::mutex> lock(mutex);
    cxxime::UserDictQueryResult result;
    const auto dictionary = dict_for_kind_locked(kind);
    if (!dictionary) {
        return result;
    }
    result.resource_total = dictionary->disabled_system_entry_count();
    result.offset = 0;
    result.entries.reserve(texts.size());
    for (const auto& text : texts) {
        if (!dictionary->is_system_entry_disabled(text)) {
            continue;
        }
        cxxime::UserDictEntryInfo entry;
        entry.text = text;
        result.entries.push_back(std::move(entry));
    }
    result.match_total = result.entries.size();
    return result;
}

cxxime::IPCStatus SharedResources::disable_system_entry(cxxime::UserDictKind kind,
                                                       const std::string& text) {
    if (text.empty()) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open()) {
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    }
    if (!dict->disable_system_entry_and_save(text)) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    return cxxime::IPCStatus::OK;
}

cxxime::IPCStatus SharedResources::restore_system_entry(cxxime::UserDictKind kind,
                                                       const std::string& text) {
    if (text.empty()) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open()) {
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    }
    if (!dict->restore_system_entry_and_save(text)) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    return cxxime::IPCStatus::OK;
}

cxxime::IPCStatus SharedResources::delete_candidate_preferences(
    cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries) {
    if (entries.empty())
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    return dict->delete_candidate_preferences_and_save(entries)
        ? cxxime::IPCStatus::OK
        : cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
}

cxxime::IPCStatus SharedResources::clear_candidate_preferences(cxxime::UserDictKind kind) {
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    if (!dict->clear_candidate_preferences_and_save()) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    // Pinyin learning also includes the words built from consecutive picks.
    if (kind == cxxime::UserDictKind::PINYIN) {
        std::shared_ptr<cxxime::CompositionLearningService> service;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (runtime) {
                service = runtime->composition_learning_ptr();
            }
        }
        if (service && !service->clear_and_save()) {
            return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
        }
        // English word mode learning is cleared with it (one switch for all self-learning).
        auto english = cxxime::EnglishLearning::shared();
        if (english && !english->clear_and_save()) {
            return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
        }
    }
    return cxxime::IPCStatus::OK;
}

cxxime::IPCStatus SharedResources::save_candidate_preferences(cxxime::UserDictKind kind) {
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    return dict->save_candidate_preferences()
        ? cxxime::IPCStatus::OK
        : cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
}

cxxime::CandidateOrderQueryResult SharedResources::query_candidate_order(
    cxxime::UserDictKind kind, const std::string& code, std::size_t limit) {
    cxxime::CandidateOrderQueryResult result;
    result.input_code = code;
    auto dictionary = dict_for_kind(kind);
    if (!dictionary || !dictionary->is_open() || limit == 0) {
        return result;
    }

    const auto resources = snapshot();
    const bool learning_enabled =
        resources.runtime && resources.runtime->config().candidate_learning;
    cxxime::TranslationRequest request;
    request.input = code;
    request.page_size = static_cast<int>(limit);
    request.policy.allow_partial_selection = false;
    cxxime::TranslationResult translation;
    if (kind == cxxime::UserDictKind::WUBI) {
        request.scheme = cxxime::CompositionScheme::kWubi;
        cxxime::WubiTranslator translator;
        translator.set_dict(dictionary.get());
        translator.set_candidate_learning_enabled(learning_enabled);
        translation = translator.translate(request);
    } else {
        request.scheme = cxxime::CompositionScheme::kPinyin;
        cxxime::PinyinTranslator translator;
        translator.set_dict(dictionary.get());
        translator.set_short_cache(&dictionary->short_cache());
        translator.set_candidate_learning_enabled(learning_enabled);
        translation = translator.translate(request);
    }

    result.version = dictionary->manual_candidate_order_version();
    result.manual_entries = dictionary->manual_candidate_order(code);
    result.has_more = cxxime::candidate_extent_may_continue(
        translation.extent,
        translation.page_offset + static_cast<int>(translation.entries.size()));
    result.entries.reserve(translation.entries.size());
    const auto source = kind == cxxime::UserDictKind::WUBI ? cxxime::CandidateSource::kWubi
                                                           : cxxime::CandidateSource::kPinyin;
    for (const auto& candidate_entry : translation.entries) {
        const cxxime::Candidate& candidate = candidate_entry.candidate;
        cxxime::CandidateOrderEntryInfo entry;
        entry.text = candidate.text;
        entry.code = candidate.code;
        entry.syllables = candidate.syllables;
        entry.available = dictionary->can_resolve_manual_candidate(
            {candidate.text, candidate.code, candidate.syllables}, source);
        if (dictionary->has_manual_candidate_order(code, candidate.text, candidate.code,
                                                   candidate.syllables)) {
            entry.reason = cxxime::CandidateOrderReason::kManual;
        } else if (learning_enabled &&
                   dictionary->has_candidate_preference(candidate.text, code)) {
            entry.reason = cxxime::CandidateOrderReason::kLearned;
        } else if (candidate.origin == cxxime::CandidateOrigin::kUser) {
            entry.reason = cxxime::CandidateOrderReason::kUserLexicon;
        }
        result.entries.push_back(std::move(entry));
    }
    for (const auto& manual : result.manual_entries) {
        const bool present =
            std::any_of(result.entries.begin(), result.entries.end(), [&](const auto& entry) {
                return entry.text == manual.text && entry.code == manual.code &&
                       entry.syllables == manual.syllables;
            });
        if (!present) {
            cxxime::CandidateOrderEntryInfo entry;
            entry.text = manual.text;
            entry.code = manual.code;
            entry.syllables = manual.syllables;
            entry.reason = cxxime::CandidateOrderReason::kManual;
            entry.available = dictionary->can_resolve_manual_candidate(manual, source);
            result.entries.push_back(std::move(entry));
        }
    }
    return result;
}

cxxime::IPCStatus SharedResources::replace_candidate_order(
    cxxime::UserDictKind kind, const std::string& code,
    const std::vector<cxxime::ManualCandidateOrderEntry>& entries, std::uint64_t expected_version,
    bool* version_conflict) {
    auto dictionary = dict_for_kind(kind);
    if (!dictionary || !dictionary->is_open()) {
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    }
    const auto source = kind == cxxime::UserDictKind::WUBI ? cxxime::CandidateSource::kWubi
                                                           : cxxime::CandidateSource::kPinyin;
    const auto previous_order = dictionary->manual_candidate_order(code);
    if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
            if (dictionary->can_resolve_manual_candidate(entry, source)) {
                return false;
            }
            return std::none_of(
                previous_order.begin(), previous_order.end(), [&](const auto& previous) {
                    return previous.text == entry.text && previous.code == entry.code &&
                           previous.syllables == entry.syllables;
                });
        })) {
        if (version_conflict) {
            *version_conflict = false;
        }
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    return dictionary->replace_manual_candidate_order_if_version(code, entries, expected_version,
                                                                 version_conflict)
               ? cxxime::IPCStatus::OK
               : cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
}

cxxime::IPCStatus SharedResources::clear_candidate_order(cxxime::UserDictKind kind,
                                                         const std::string& code,
                                                         std::uint64_t expected_version,
                                                         bool* version_conflict) {
    auto dictionary = dict_for_kind(kind);
    if (!dictionary || !dictionary->is_open()) {
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    }
    const auto previous_order = dictionary->manual_candidate_order(code);
    if (!dictionary->replace_manual_candidate_order_if_version(code, {}, expected_version,
                                                               version_conflict)) {
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    }
    if (dictionary->clear_candidate_preferences_for_code_and_save(code)) {
        return cxxime::IPCStatus::OK;
    }
    dictionary->replace_manual_candidate_order_and_save(code, previous_order);
    return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
}

cxxime::IPCStatus SharedResources::delete_user_entries(
    cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries) {
    if (entries.empty())
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    if (!dict->delete_user_entries_and_save(entries))
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    return cxxime::IPCStatus::OK;
}

cxxime::IPCStatus SharedResources::replace_user_entry(cxxime::UserDictKind kind,
                                                      const std::string& old_text,
                                                      const std::string& old_code,
                                                      const std::string& new_text,
                                                      const std::string& new_code,
                                                      const std::string& syllables) {
    if (old_text.empty() || new_text.empty() || new_code.empty())
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    if (!dict->replace_user_entry_and_save(old_text, old_code, new_text, new_code, syllables))
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    return cxxime::IPCStatus::OK;
}

cxxime::IPCStatus SharedResources::save_user_dict(cxxime::UserDictKind kind) {
    auto dict = dict_for_kind(kind);
    if (!dict || !dict->is_open())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
    if (!dict->save_user_dict())
        return cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
    return cxxime::IPCStatus::OK;
}

bool SharedResources::save_candidate_preferences(bool force) {
    std::shared_ptr<cxxime::Dict> pinyin;
    std::shared_ptr<cxxime::Dict> wubi;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (runtime) {
            pinyin = runtime->pinyin_dict_ptr();
            wubi = runtime->wubi_dict_ptr();
        }
    }
    constexpr auto kSaveDelay = std::chrono::milliseconds(1500);
    const bool pinyin_saved = !pinyin || (force ? pinyin->save_candidate_preferences()
                                                : pinyin->save_candidate_preferences_if_due(
                                                      kSaveDelay));
    const bool wubi_saved = !wubi || (force ? wubi->save_candidate_preferences()
                                            : wubi->save_candidate_preferences_if_due(
                                                  kSaveDelay));
    return pinyin_saved && wubi_saved;
}

bool SharedResources::freeze_and_stop_composition_learning() {
    std::shared_ptr<cxxime::CompositionLearningService> service;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (runtime) {
            service = runtime->composition_learning_ptr();
        }
    }
    const bool english_saved = cxxime::EnglishLearning::close_shared();
    return (!service || service->freeze_and_stop()) && english_saved;
}

bool SessionManager::prepare_config(const std::shared_ptr<const cxxime::Config>& config,
                                    unsigned long* error_code) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    if (shared_.prepare_config(config)) {
        if (error_code) {
            *error_code = ERROR_SUCCESS;
        }
        return true;
    }
    if (error_code) {
        *error_code = ERROR_INVALID_DATA;
    }
    return false;
}

void SessionManager::cancel_prepared_config() {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    shared_.cancel_prepared_config();
}

bool SessionManager::apply_config(const std::shared_ptr<const cxxime::Config>& config) {
    if (!config) {
        return false;
    }
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    auto previous_resources = shared_.snapshot();
    bool input_mode_changed = !previous_resources.runtime ||
                              previous_resources.runtime->config().input_mode !=
                                  config->input_mode;
    if (!shared_.commit_prepared_config(config) &&
        (!shared_.prepare_config(config) || !shared_.commit_prepared_config(config))) {
        CXXIME_LOG(L"%s", L"SessionManager: config apply rejected because scheme load failed");
        return false;
    }
    cxxime::set_diagnostics_config(config->diagnostics);
    cxxime::QueryTrace::set_enabled(config->diagnostics.trace_mode !=
                                    cxxime::DiagnosticTraceMode::kOff);
    cxxime::ExperienceLog::instance().configure(*config);
    auto resources = shared_.snapshot();

    std::vector<std::shared_ptr<SessionEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.reserve(sessions_.size());
        for (auto& item : sessions_) {
            entries.push_back(item.second);
        }
    }

    // Sync input_mode from config to all active sessions
    if (resources.runtime) {
        if (input_mode_changed) {
            GlobalVisibleState state = snapshot_global_state();
            state.input_mode =
                static_cast<cxxime::InputMode>(resources.runtime->config().input_mode);
            commit_global_state(state);
        }
        for (auto& entry : entries) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            const CandidateStateToken candidate_state_before =
                candidate_state_token(*entry->engine);
            apply_resource_snapshot(*entry, resources);
            align_session_to_global(*entry);
            advance_candidate_revision(*entry, candidate_state_before, true);
        }
    }
    CXXIME_LOG(L"SessionManager: config applied, %zu active sessions", entries.size());
    return true;
}

void SessionManager::set_config_patch_handler(ConfigPatchHandler handler) {
    config_patch_handler_ = std::move(handler);
}

cxxime::IPCStatus SessionManager::reload_dictionaries() {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);

    std::vector<std::shared_ptr<SessionEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.reserve(sessions_.size());
        for (auto& item : sessions_) {
            entries.push_back(item.second);
        }
    }

    std::vector<std::unique_lock<std::mutex>> entry_locks;
    entry_locks.reserve(entries.size());
    for (auto& entry : entries) {
        entry_locks.emplace_back(entry->mutex);
    }

    if (!shared_.reload_dictionaries())
        return cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;

    auto resources = shared_.snapshot();
    for (auto& entry : entries) {
        const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
        apply_resource_snapshot(*entry, resources);
        align_session_to_global(*entry);
        advance_candidate_revision(*entry, candidate_state_before, true);
    }

    CXXIME_LOG(L"SessionManager: dictionaries reloaded, %zu active sessions", entries.size());
    return cxxime::IPCStatus::OK;
}

bool SessionManager::reload_punctuation(const std::string& path) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.load_punctuation(path);
}

std::pair<cxxime::IPCStatus, cxxime::ImeStatus> SessionManager::get_ime_status(uint32_t id) {
    auto entry = lookup_session(id);
    if (!entry) return {cxxime::IPCStatus::ERR_INVALID_SESSION, {}};
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    return {cxxime::IPCStatus::OK, entry->ime_status};
}

std::pair<cxxime::IPCStatus, cxxime::ImeStatus> SessionManager::toggle_chinese(uint32_t id) {
    auto entry = lookup_session(id);
    if (!entry) return {cxxime::IPCStatus::ERR_INVALID_SESSION, {}};
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    entry->base_chinese_mode = !entry->base_chinese_mode;
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    return {cxxime::IPCStatus::OK, entry->ime_status};
}

ProcessKeyResult SessionManager::set_chinese_mode(uint32_t id, bool chinese_mode) {
    auto entry = lookup_session(id);
    if (!entry) {
        ProcessKeyResult error;
        error.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
        return error;
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.result = cxxime::ProcessResult::ACCEPTED;
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);

    const bool mode_changed = entry->base_chinese_mode != chinese_mode;
    if (mode_changed && entry->engine->context().is_composing()) {
        result.commit_text = entry->engine->commit_raw_composition();
        result.result = cxxime::ProcessResult::COMMITTED;
    }

    entry->base_chinese_mode = chinese_mode;
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    fill_session_presentation(*entry, result);
    return result;
}

std::pair<cxxime::IPCStatus, cxxime::ImeStatus> SessionManager::toggle_shape(uint32_t id) {
    auto entry = lookup_session(id);
    if (!entry) return {cxxime::IPCStatus::ERR_INVALID_SESSION, {}};
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    entry->full_shape = !entry->full_shape;
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    return {cxxime::IPCStatus::OK, entry->ime_status};
}

std::pair<cxxime::IPCStatus, cxxime::ImeStatus> SessionManager::toggle_punct(uint32_t id) {
    auto entry = lookup_session(id);
    if (!entry) return {cxxime::IPCStatus::ERR_INVALID_SESSION, {}};
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    entry->chinese_punct = !entry->chinese_punct;
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    return {cxxime::IPCStatus::OK, entry->ime_status};
}

std::pair<cxxime::IPCStatus, cxxime::ImeStatus>
SessionManager::switch_input_mode(uint32_t id, cxxime::InputMode mode) {
    auto entry = lookup_session(id);
    if (!entry) return {cxxime::IPCStatus::ERR_INVALID_SESSION, {}};
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    align_session_to_global(*entry);
    entry->engine->switch_mode(mode);
    GlobalVisibleState state = snapshot_global_state();
    state.input_mode = entry->engine->mode();
    commit_global_state(state);
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    persist_input_mode(entry->ime_status.input_mode);
    return {cxxime::IPCStatus::OK, entry->ime_status};
}

std::pair<cxxime::IPCStatus, cxxime::ImeStatus>
SessionManager::set_input_style(uint32_t id, bool english_style, bool value) {
    auto entry = lookup_session(id);
    if (!entry) return {cxxime::IPCStatus::ERR_INVALID_SESSION, {}};
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    GlobalVisibleState state = snapshot_global_state();
    const bool changed = english_style ? state.english_words != value
                                       : state.pinyin_initials != value;
    if (english_style) {
        state.english_words = value;
    } else {
        state.pinyin_initials = value;
    }
    commit_global_state(state);
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    if (changed) {
        if (english_style) {
            persist_english_word_mode(value);
        } else {
            persist_pinyin_initials(value);
        }
    }
    return {cxxime::IPCStatus::OK, entry->ime_status};
}

cxxime::IPCStatus SessionManager::sync_ascii_mode(uint32_t id, bool ascii_mode) {
    auto entry = lookup_session(id);
    if (!entry) return cxxime::IPCStatus::ERR_INVALID_SESSION;
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    entry->base_chinese_mode = !ascii_mode;
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    return cxxime::IPCStatus::OK;
}

std::pair<cxxime::IPCStatus, cxxime::ImeStatus> SessionManager::sync_caps_lock(uint32_t id,
                                                                               bool caps_lock) {
    auto entry = lookup_session(id);
    if (!entry) return {cxxime::IPCStatus::ERR_INVALID_SESSION, {}};
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    GlobalVisibleState state = snapshot_global_state();
    state.caps_lock = caps_lock;
    commit_global_state(state);
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    return {cxxime::IPCStatus::OK, entry->ime_status};
}

namespace {

// A key the IME handled, for the input log: "n", "3", "SPACE", "Ctrl+Space" ...
std::string input_key_name(const cxxime::KeyEvent& event) {
    std::string name;
    if (event.is_ctrl()) name += "Ctrl+";
    if (event.is_alt()) name += "Alt+";
    const uint32_t vk = event.keycode;
    if ((vk >= 'A' && vk <= 'Z')) {
        const bool upper = event.is_shift() != event.is_caps_lock();
        name += static_cast<char>(upper ? vk : vk - 'A' + 'a');
        return name;
    }
    if (event.is_shift()) name += "Shift+";
    if (vk >= '0' && vk <= '9') return name + static_cast<char>(vk);
    switch (vk) {
    case VK_SPACE: return name + "SPACE";
    case VK_BACK: return name + "BACK";
    case VK_RETURN: return name + "ENTER";
    case VK_ESCAPE: return name + "ESC";
    case VK_TAB: return name + "TAB";
    case VK_DELETE: return name + "DEL";
    case VK_LEFT: return name + "LEFT";
    case VK_RIGHT: return name + "RIGHT";
    case VK_UP: return name + "UP";
    case VK_DOWN: return name + "DOWN";
    case VK_HOME: return name + "HOME";
    case VK_END: return name + "END";
    case VK_PRIOR: return name + "PGUP";
    case VK_NEXT: return name + "PGDN";
    default: {
        char hex[8] = {};
        std::snprintf(hex, sizeof(hex), "VK%02X", vk);
        return name + hex;
    }
    }
}

std::string composition_code(const cxxime::Context& context) {
    std::string code;
    for (const auto& segment : context.composition().converted_segments()) {
        code += segment.raw_input;
    }
    return code + context.composition().active().input;
}

const char* composition_mode(cxxime::CompositionScheme scheme) {
    switch (scheme) {
    case cxxime::CompositionScheme::kPinyin: return "pinyin";
    case cxxime::CompositionScheme::kWubi: return "wubi";
    case cxxime::CompositionScheme::kMixed: return "mixed";
    case cxxime::CompositionScheme::kSymbol: return "symbol";
    case cxxime::CompositionScheme::kInlineAscii: return "english";
    }
    return "other";
}

// Second tier of the experience program: one record per input, from its first handled key to
// its commit or cancel. Keys the IME does not handle are not part of any input.
void track_input(SessionEntry& s, const cxxime::KeyEvent* event, bool was_composing,
                 bool handled, const ProcessKeyResult& ret,
                 const std::optional<cxxime::CandidatePick>& pick) {
    cxxime::ExperienceLog& log = cxxime::ExperienceLog::instance();
    if (!log.collecting_input()) {
        s.input_open = false;
        return;
    }
    const bool committed = !ret.commit_text.empty();
    if (!handled || (!was_composing && !ret.composing && !committed)) return;
    const cxxime::Engine& engine = *s.engine;
    if (!s.input_open) {
        s.input = {};
        s.input.app = s.input_app;
        s.input.window_title = s.input_window_title;
        s.input_start = std::chrono::steady_clock::now();
        s.input_open = true;
    }
    if (event) s.input.keys.push_back(input_key_name(*event));
    if (pick) s.input.picked = pick->index;
    if (ret.composing) {
        s.input.code = composition_code(engine.context());
        s.input.mode = composition_mode(engine.context().composition().active().scheme);
        s.input.candidates.clear();
        for (const auto& item : ret.presentation.items) {
            s.input.candidates.emplace_back(item.text, item.recommended);
        }
        // The context of this input, as the recommendation model gets it: the text before the
        // caret read from the input box (or the earlier commits where it cannot be read).
        const auto* rt = s.resources.runtime.get();
        const int context_chars = !rt ? 128
                                  : s.input.mode == "english" ? rt->config().laya.english_context_chars
                                                              : rt->config().laya.context_chars;
        s.input.laya_context = engine.laya_context(static_cast<std::size_t>(context_chars));
        return;
    }
    if (s.input.mode.empty()) {
        s.input.mode = composition_mode(engine.context().composition().active().scheme);
    }
    s.input.committed = ret.commit_text;
    s.input.duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              s.input_start)
            .count());
    log.record_input(s.input);
    s.input_open = false;
}

}  // namespace

bool SessionManager::set_input_target(uint32_t id, const std::string& app,
                                      const std::string& window_title) {
    auto entry = lookup_session(id);
    if (!entry) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    entry->input_app = app;
    entry->input_window_title = window_title;
    return true;
}

bool SessionManager::set_context(uint32_t id, const std::string& text, uint32_t flags) {
    auto entry = lookup_session(id);
    if (!entry) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->engine) return false;
    if (flags & cxxime::kContextTextRead) {
        entry->engine->set_text_before_caret(text);
    } else if (flags & cxxime::kContextNewInputBox) {
        entry->engine->clear_laya_context();
    }
    return true;
}

ProcessKeyResult SessionManager::process_key(uint32_t id, const cxxime::KeyEvent& event,
                                             uint32_t visible_candidate_count) {
    // Two-phase lock: lookup session and copy shared_ptr, then lock session
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            ProcessKeyResult err;
            err.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
            return err;
        }
        entry = it->second;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->closing) {
        ProcessKeyResult err;
        err.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
        return err;
    }
    auto& s = *entry;
    auto& engine = *s.engine;
    const CandidateStateToken candidate_state_before = candidate_state_token(engine);
    auto resources = shared_.snapshot();
    apply_resource_snapshot(s, resources);
    bool trace_enabled = false;
    if (resources.runtime) {
        trace_enabled =
            resources.runtime->config().diagnostics.trace_mode !=
            cxxime::DiagnosticTraceMode::kOff;
    }
    engine.set_trace_enabled(trace_enabled);

    align_session_to_global(s);

    // 1. Sync CapsLock before OutputOptions derivation. On first activation,
    // TSF may not deliver a VK_CAPITAL event because CapsLock was already on.
    // The physical modifier bit on the first real key is still authoritative.
    bool is_caps_lock_key = event.keycode == VK_CAPITAL;
    if (!is_caps_lock_key && s.ime_status.caps_lock() != event.is_caps_lock()) {
        GlobalVisibleState state = snapshot_global_state();
        state.caps_lock = event.is_caps_lock();
        commit_global_state(state);
        align_session_to_global(s);
    }

    // 2. derive OutputOptions
    auto opts = cxxime::OutputOptions::from(s.ime_status);
    opts.punct_mapping = resources.punct_mapping.get();

    // 3. set trace
    engine.set_trace_session_id(id);

    // 4. call Engine
    const bool was_composing = engine.context().is_composing();
    const auto key_start = std::chrono::steady_clock::now();
    auto result = engine.process_key(event, opts, static_cast<int>(visible_candidate_count));
    if (result == cxxime::ProcessResult::COMMIT_TRANSLATION) {
        // Ctrl+1..9 in learning mode: the first sense of the translation the window shows.
        std::string translation;
        const std::optional<int> index = engine.take_translation_request();
        const cxxime::Candidate* candidate = index ? engine.context().candidate(*index) : nullptr;
        if (candidate && resources.runtime) {
            const auto senses =
                cxxime::decode_candidate_gloss(shown_gloss(resources.runtime->config(), *candidate));
            if (!senses.empty()) translation = senses.front().text;
        }
        result = engine.commit_translation(index.value_or(-1), translation);
    }
    std::optional<cxxime::CandidatePick> pick = engine.take_candidate_pick();
    if (!event.is_key_up) {
        cxxime::ExperienceLog& experience = cxxime::ExperienceLog::instance();
        experience.record_key(static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - key_start)
                .count()));
        if (pick) experience.record_pick(*pick);
    }

    // 5. Publish shared mode changes and retain this session's base language mode.
    const bool temporary_ascii = engine.ascii_composer().is_temporary_ascii();
    bool new_ascii = engine.ascii_composer().is_ascii_mode();
    GlobalVisibleState state = snapshot_global_state();
    bool shared_state_changed = false;
    if (is_caps_lock_key && !event.is_key_up) {
        state.caps_lock = event.is_caps_lock();
        shared_state_changed = true;
    }
    if (!state.caps_lock && !temporary_ascii) {
        s.base_chinese_mode = !new_ascii;
    }

    // 6. Populate the return value. A Wubi fifth-key commit can start the next
    //    composition in the same engine step, so consume the commit before
    //    publishing the remaining composition state.
    ProcessKeyResult ret;
    ret.status = cxxime::IPCStatus::OK;
    ret.result = result;

    // Handle toggle results and return updated ImeStatus.
    if (result == cxxime::ProcessResult::TOGGLE_SHAPE) {
        s.full_shape = !s.full_shape;
    } else if (result == cxxime::ProcessResult::TOGGLE_PUNCT) {
        // In English mode, the punctuation switch also switches to Chinese mode
        if (!state.caps_lock && !s.base_chinese_mode) {
            s.base_chinese_mode = true;
            engine.ascii_composer().set_ascii_mode(false);
        }
        s.chinese_punct = !s.chinese_punct;
    } else if (result == cxxime::ProcessResult::SWITCH_INPUT_MODE) {
        engine.switch_mode(next_input_mode(engine.mode()));
        state.input_mode = engine.mode();
        shared_state_changed = true;
    } else if (result == cxxime::ProcessResult::TOGGLE_ENGLISH_STYLE) {
        state.english_words = !state.english_words;
        shared_state_changed = true;
    } else if (result == cxxime::ProcessResult::TOGGLE_PINYIN_STYLE) {
        state.pinyin_initials = !state.pinyin_initials;
        shared_state_changed = true;
    }

    if (shared_state_changed) {
        commit_global_state(state);
    }
    align_session_to_global(s);
    ret.ime_status = s.ime_status;
    if (result == cxxime::ProcessResult::SWITCH_INPUT_MODE) {
        persist_input_mode(ret.ime_status.input_mode);
    } else if (result == cxxime::ProcessResult::TOGGLE_ENGLISH_STYLE) {
        persist_english_word_mode(ret.ime_status.english_words());
    } else if (result == cxxime::ProcessResult::TOGGLE_PINYIN_STYLE) {
        persist_pinyin_initials(ret.ime_status.pinyin_initials());
    }

    if (result == cxxime::ProcessResult::COMMITTED) {
        auto [raw, source] = engine.take_commit_text_with_source();
        ret.commit_text = cxxime::OutputComposer::transform(raw, opts, source);
        ret.composing = engine.context().is_composing();
    } else if (result == cxxime::ProcessResult::TOGGLE_PUNCT
            || result == cxxime::ProcessResult::TOGGLE_SHAPE
            || result == cxxime::ProcessResult::TOGGLE_ENGLISH_STYLE
            || result == cxxime::ProcessResult::TOGGLE_PINYIN_STYLE) {
        ret.composing = engine.context().is_composing();
    } else if (result == cxxime::ProcessResult::SWITCH_INPUT_MODE) {
        ret.composing = false;
    } else {
        ret.composing = engine.context().is_composing();
    }
    advance_candidate_revision(s, candidate_state_before);
    fill_session_presentation(s, ret);
    if (!event.is_key_up) {
        track_input(s, &event, was_composing, result != cxxime::ProcessResult::REJECTED, ret,
                    pick);
    }

    // trace log
    if (trace_enabled && engine.last_trace().should_log()) {
        engine.last_trace().log_unchecked();
    }

    return ret;
}

cxxime::CandidatePage SessionManager::search_candidates(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const auto resources = shared_.snapshot();
    if (!resources.runtime) {
        return {};
    }

    const auto visible_state = snapshot_global_state();
    cxxime::Engine search_engine;
    if (!search_engine.initialize(resources.runtime)) {
        return {};
    }
    search_engine.switch_mode(visible_state.input_mode);
    cxxime::CandidatePage page = search_engine.translate_for_search(input, 10);
    std::vector<cxxime::Candidate> filtered;
    filtered.reserve(page.candidates.size());
    for (auto& candidate : page.candidates) {
        if (candidate.origin == cxxime::CandidateOrigin::kComposed || candidate.text.empty()) {
            continue;
        }

        bool redundant = false;
        for (auto it = filtered.begin(); it != filtered.end();) {
            const bool existing_prefix = candidate.text.size() >= it->text.size() &&
                candidate.text.compare(0, it->text.size(), it->text) == 0;
            const bool candidate_prefix = it->text.size() >= candidate.text.size() &&
                it->text.compare(0, candidate.text.size(), candidate.text) == 0;
            if (!existing_prefix && !candidate_prefix) {
                ++it;
                continue;
            }
            if (it->text.size() <= candidate.text.size()) {
                redundant = true;
                break;
            }
            it = filtered.erase(it);
        }
        if (!redundant) {
            filtered.push_back(std::move(candidate));
        }
    }
    page.candidates = std::move(filtered);
    page.extent.known_count = static_cast<int>(page.candidates.size());
    page.extent.state = cxxime::CandidateExtentState::kExhausted;
    page.page_index = 0;
    page.page_offset = 0;
    page.page_size = 10;
    page.highlighted = page.candidates.empty() ? -1 : 0;
    return page;
}

bool SessionManager::record_search_result(const std::string& input,
                                          const std::string& result) {
    if (input.empty() || result.empty()) {
        return false;
    }
    const auto resources = shared_.snapshot();
    if (!resources.runtime) {
        return false;
    }

    const auto visible_state = snapshot_global_state();
    cxxime::Engine search_engine;
    if (!search_engine.initialize(resources.runtime)) {
        return false;
    }
    search_engine.switch_mode(visible_state.input_mode);
    return search_engine.record_search_result(input, result);
}

ProcessKeyResult SessionManager::select_candidate(uint32_t id, int index,
                                                  uint64_t candidate_revision) {
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            ProcessKeyResult err;
            err.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
            return err;
        }
        entry = it->second;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->closing) {
        ProcessKeyResult err;
        err.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
        return err;
    }
    auto& s = *entry;
    const CandidateStateToken candidate_state_before_alignment = candidate_state_token(*s.engine);
    align_session_to_global(s);
    advance_candidate_revision(s, candidate_state_before_alignment);
    auto opts = cxxime::OutputOptions::from(s.ime_status);
    ProcessKeyResult ret;
    ret.status = cxxime::IPCStatus::OK;
    ret.ime_status = s.ime_status;

    const bool revision_required =
        (s.client_capabilities & cxxime::kClientCapabilitySegmentedSelection) != 0;
    if (revision_required && candidate_revision != s.candidate_revision) {
        ret.status = cxxime::IPCStatus::ERR_STALE_CANDIDATE;
        ret.result = cxxime::ProcessResult::REJECTED;
        fill_session_presentation(s, ret);
        return ret;
    }

    const CandidateStateToken candidate_state_before = candidate_state_token(*s.engine);
    const bool was_composing = s.engine->context().is_composing();
    std::optional<cxxime::CandidatePick> pick;
    if (s.engine->select_candidate(index)) {
        pick = s.engine->take_candidate_pick();
        if (pick) cxxime::ExperienceLog::instance().record_pick(*pick);
        if (s.engine->context().is_composing()) {
            ret.result = cxxime::ProcessResult::ACCEPTED;
        } else {
            auto [raw, source] = s.engine->take_commit_text_with_source();
            ret.commit_text = cxxime::OutputComposer::transform(raw, opts, source);
            ret.result = cxxime::ProcessResult::COMMITTED;
        }
    } else {
        ret.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
        ret.result = cxxime::ProcessResult::REJECTED;
    }
    advance_candidate_revision(s, candidate_state_before);
    fill_session_presentation(s, ret);
    // A mouse pick: part of the current input, without a key.
    track_input(s, nullptr, was_composing, pick.has_value(), ret, pick);
    return ret;
}

ProcessKeyResult SessionManager::commit_composition(uint32_t id) {
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            ProcessKeyResult err;
            err.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
            return err;
        }
        entry = it->second;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    auto& s = *entry;
    const CandidateStateToken candidate_state_before_alignment = candidate_state_token(*s.engine);
    align_session_to_global(s);
    advance_candidate_revision(s, candidate_state_before_alignment);
    auto opts = cxxime::OutputOptions::from(s.ime_status);
    ProcessKeyResult ret;
    ret.status = cxxime::IPCStatus::OK;
    ret.ime_status = s.ime_status;

    const CandidateStateToken candidate_state_before = candidate_state_token(*s.engine);
    auto [raw, source] = s.engine->commit_composition_with_source();
    if (!raw.empty()) {
        ret.commit_text = cxxime::OutputComposer::transform(raw, opts, source);
        ret.result = cxxime::ProcessResult::COMMITTED;
    } else {
        ret.result = cxxime::ProcessResult::ACCEPTED;
    }
    advance_candidate_revision(s, candidate_state_before);
    fill_session_presentation(s, ret);
    return ret;
}

ProcessKeyResult SessionManager::clear_composition(uint32_t id) {
    auto entry = lookup_session(id);
    if (!entry) {
        ProcessKeyResult result;
        result.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
        return result;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    entry->engine->clear_composition();
    advance_candidate_revision(*entry, candidate_state_before);
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.result = cxxime::ProcessResult::ACCEPTED;
    fill_session_presentation(*entry, result);
    return result;
}

ProcessKeyResult SessionManager::focus_in(uint32_t id) {
    auto entry = lookup_session(id);
    if (!entry) {
        ProcessKeyResult result;
        result.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
        return result;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    align_session_to_global(*entry);
    advance_candidate_revision(*entry, candidate_state_before);
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.result = cxxime::ProcessResult::ACCEPTED;
    fill_session_presentation(*entry, result);
    return result;
}

ProcessKeyResult SessionManager::focus_out(uint32_t id) {
    auto entry = lookup_session(id);
    if (!entry) {
        ProcessKeyResult result;
        result.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
        return result;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    const CandidateStateToken candidate_state_before = candidate_state_token(*entry->engine);
    entry->engine->clear_composition();
    advance_candidate_revision(*entry, candidate_state_before);
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.result = cxxime::ProcessResult::ACCEPTED;
    fill_session_presentation(*entry, result);
    return result;
}

cxxime::IPCStatus SessionManager::add_user_entry(cxxime::UserDictKind kind,
                                                 const std::string& text,
                                                 const std::string& code,
                                                 const std::string& syllables) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.add_user_entry(kind, text, code, syllables);
}

cxxime::UserDictQueryResult SessionManager::query_user_entries(
    const std::string& query, cxxime::UserDictKind kind, size_t offset, size_t limit) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.query_user_entries(query, kind, offset, limit);
}

cxxime::UserDictQueryResult SessionManager::query_lexicon_entries(
    cxxime::LexiconResource resource, const std::string& query, cxxime::UserDictKind kind,
    size_t offset, size_t limit, bool exact_text) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.query_lexicon_entries(resource, query, kind, offset, limit, exact_text);
}

cxxime::UserDictQueryResult SessionManager::query_disabled_system_entry_status(
    cxxime::UserDictKind kind, const std::vector<std::string>& texts) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.query_disabled_system_entry_status(kind, texts);
}

cxxime::IPCStatus SessionManager::delete_user_entries(
    cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.delete_user_entries(kind, entries);
}

cxxime::IPCStatus SessionManager::replace_user_entry(cxxime::UserDictKind kind,
                                                     const std::string& old_text,
                                                     const std::string& old_code,
                                                     const std::string& new_text,
                                                     const std::string& new_code,
                                                     const std::string& syllables) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.replace_user_entry(kind, old_text, old_code, new_text, new_code, syllables);
}

cxxime::IPCStatus SessionManager::import_user_dict(cxxime::UserDictKind kind,
                                                   const std::string& source_path) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.import_user_dict(kind, source_path);
}

cxxime::IPCStatus SessionManager::save_user_dict(cxxime::UserDictKind kind) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.save_user_dict(kind);
}

cxxime::IPCStatus SessionManager::disable_system_entry(cxxime::UserDictKind kind,
                                                       const std::string& text) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.disable_system_entry(kind, text);
}

cxxime::IPCStatus SessionManager::restore_system_entry(cxxime::UserDictKind kind,
                                                       const std::string& text) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.restore_system_entry(kind, text);
}

cxxime::IPCStatus SessionManager::delete_candidate_preferences(
    cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.delete_candidate_preferences(kind, entries);
}

cxxime::IPCStatus SessionManager::clear_candidate_preferences(cxxime::UserDictKind kind) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.clear_candidate_preferences(kind);
}

cxxime::IPCStatus SessionManager::save_candidate_preferences(cxxime::UserDictKind kind) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.save_candidate_preferences(kind);
}

cxxime::CandidateOrderQueryResult SessionManager::query_candidate_order(
    cxxime::UserDictKind kind, const std::string& code, std::size_t limit) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.query_candidate_order(kind, code, limit);
}

cxxime::IPCStatus SessionManager::replace_candidate_order(
    cxxime::UserDictKind kind, const std::string& code,
    const std::vector<cxxime::ManualCandidateOrderEntry>& entries,
    std::uint64_t expected_version, bool* version_conflict) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.replace_candidate_order(kind, code, entries, expected_version,
                                           version_conflict);
}

cxxime::IPCStatus SessionManager::clear_candidate_order(cxxime::UserDictKind kind,
                                                        const std::string& code,
                                                        std::uint64_t expected_version,
                                                        bool* version_conflict) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.clear_candidate_order(kind, code, expected_version, version_conflict);
}

bool SessionManager::save_candidate_preferences(bool force) {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.save_candidate_preferences(force);
}

bool SessionManager::snapshot_user_data(const std::vector<std::string>& file_names,
                                        std::map<std::string, std::string>* files) {
    if (!files || file_names.empty()) {
        return false;
    }
    const SharedResourceSnapshot resources = shared_.snapshot();
    std::map<std::string, std::string> snapshot;
    for (const std::string& name : file_names) {
        const LexiconDataFile* file = find_lexicon_data_file(name);
        const bool composition = name == "learning_composition.tsv";
        const bool english = name == kEnglishLearningFile;
        const auto dict = !resources.runtime
                              ? nullptr
                              : file && file->kind == cxxime::UserDictKind::WUBI
                                    ? resources.runtime->wubi_dict_ptr()
                                    : resources.runtime->pinyin_dict_ptr();
        bool ready = composition || english || file;
        if (ready && english) {
            const auto learning = cxxime::EnglishLearning::shared();
            ready = !learning || learning->flush();
        } else if (ready && composition) {
            ready = resources.runtime && resources.runtime->composition_learning() &&
                    resources.runtime->composition_learning()->flush();
        } else if (ready && (!file || !dict)) {
            ready = false;
        } else if (ready) {
            switch (file->resource) {
            case cxxime::LexiconResource::kUserLexicon:
                ready = dict->save_user_dict();
                break;
            case cxxime::LexiconResource::kCandidatePreference:
                ready = dict->save_candidate_preferences();
                break;
            case cxxime::LexiconResource::kDisabledSystemLexicon:
                ready = dict->save_disabled_system_entries();
                break;
            case cxxime::LexiconResource::kManualCandidateOrder:
                break;
            }
        }
        if (!ready || !read_user_data_file(name, &snapshot[name]) ||
            !valid_user_data_file_contents(name, snapshot[name])) {
            return false;
        }
    }
    *files = std::move(snapshot);
    return true;
}

bool SessionManager::validate_user_data(const std::map<std::string, std::string>& files) const {
    return std::all_of(files.begin(), files.end(), [](const auto& item) {
        return valid_user_data_file_contents(item.first, item.second);
    });
}

void SessionManager::merge_user_data(const std::map<std::string, std::string>& files,
                                     std::size_t* imported_count, std::size_t* skipped_count) {
    std::size_t imported = 0;
    std::size_t skipped = 0;
    bool changed = false;
    for (const auto& item : files) {
        std::lock_guard<std::mutex> reload_lock(reload_mutex_);
        const SharedResourceSnapshot resources = shared_.snapshot();
        const LexiconDataFile* file = find_lexicon_data_file(item.first);
        const bool composition = item.first == "learning_composition.tsv";
        const auto dict = !resources.runtime
                              ? nullptr
                              : file && file->kind == cxxime::UserDictKind::WUBI
                                    ? resources.runtime->wubi_dict_ptr()
                                    : resources.runtime->pinyin_dict_ptr();
        cxxime::UserDataMergeResult merged;
        if (item.first == kEnglishLearningFile) {
            // Not a dictionary resource: engines read the shared store, nothing to refresh.
            const auto learning = cxxime::EnglishLearning::shared();
            std::size_t count = 0;
            if (learning && learning->merge_contents_and_save(item.second, &count)) {
                imported += count;
            } else {
                ++skipped;
            }
            continue;
        }
        bool ready = composition || (file && dict);
        if (ready && composition) {
            ready = resources.runtime && resources.runtime->composition_learning() &&
                    resources.runtime->composition_learning()->merge_contents_and_save(
                        item.second, &merged);
        } else if (ready) {
            switch (file->resource) {
            case cxxime::LexiconResource::kUserLexicon:
                ready = dict->merge_user_dict_contents(item.second, &merged);
                break;
            case cxxime::LexiconResource::kCandidatePreference:
                ready = dict->merge_candidate_preference_contents(item.second, &merged);
                break;
            case cxxime::LexiconResource::kManualCandidateOrder:
                ready = dict->merge_manual_candidate_order_contents(item.second, &merged);
                break;
            case cxxime::LexiconResource::kDisabledSystemLexicon:
                ready = dict->merge_disabled_system_contents(item.second, &merged);
                break;
            }
        }
        if (ready) {
            imported += merged.imported_count;
            skipped += merged.skipped_count;
            changed = changed || merged.imported_count != 0;
        } else {
            skipped += merged.imported_count + merged.skipped_count + 1;
        }
    }

    if (changed) {
        std::vector<std::shared_ptr<SessionEntry>> entries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& item : sessions_) {
                entries.push_back(item.second);
            }
        }
        const SharedResourceSnapshot refreshed = shared_.snapshot();
        for (const auto& entry : entries) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            const CandidateStateToken before = candidate_state_token(*entry->engine);
            apply_resource_snapshot(*entry, refreshed);
            entry->engine->clear_query_cache();
            advance_candidate_revision(*entry, before, true);
        }
    }
    if (imported_count) {
        *imported_count = imported;
    }
    if (skipped_count) {
        *skipped_count = skipped;
    }
}

bool SessionManager::freeze_and_save_candidate_preferences() {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    auto resources = shared_.snapshot();
    if (resources.runtime) {
        resources.runtime->pinyin_dict().freeze_candidate_preferences();
    }
    if (resources.runtime && resources.runtime->wubi_dict()) {
        resources.runtime->wubi_dict()->freeze_candidate_preferences();
    }
    return shared_.save_candidate_preferences(true);
}

bool SessionManager::freeze_and_stop_composition_learning() {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    return shared_.freeze_and_stop_composition_learning();
}

void SessionManager::persist_english_word_mode(bool enabled) {
    if (config_patch_handler_) {
        nlohmann::json patch;
        patch["english"]["word_mode"] = enabled;
        config_patch_handler_(patch.dump());
    }
}

void SessionManager::persist_pinyin_initials(bool enabled) {
    if (config_patch_handler_) {
        nlohmann::json patch;
        patch["engine"]["pinyin_initials"] = enabled;
        config_patch_handler_(patch.dump());
    }
}

void SessionManager::persist_input_mode(cxxime::InputMode mode) {
    if (config_patch_handler_) {
        nlohmann::json patch;
        patch["engine"]["input_mode"] = static_cast<int>(mode);
        config_patch_handler_(patch.dump());
    }
}
