// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/engine_runtime.h>

#include <utility>

#include <cxxime/pinyin_scheme.h>

namespace cxxime {

EngineRuntimeState::EngineRuntimeState(
    Config config, std::shared_ptr<Dict> pinyin_dict, std::shared_ptr<Dict> wubi_dict,
    std::shared_ptr<const PinyinResourceSet> pinyin_resources,
    std::shared_ptr<const SymbolTable> symbol_table,
    std::shared_ptr<CompositionLearningService> composition_learning)
    : config_(std::move(config))
    , pinyin_dict_(std::move(pinyin_dict))
    , wubi_dict_(std::move(wubi_dict))
    , pinyin_resources_(std::move(pinyin_resources))
    , symbol_table_(std::move(symbol_table))
    , composition_learning_(std::move(composition_learning))
    , pinyin_query_policy_{false} {}  // exact pinyin only (fuzzy pinyin was removed)

std::shared_ptr<const EngineRuntimeState>
EngineRuntimeState::create(Config config, std::shared_ptr<Dict> pinyin_dict,
                           std::shared_ptr<Dict> wubi_dict,
                           std::shared_ptr<const PinyinResourceSet> pinyin_resources,
                           std::shared_ptr<const SymbolTable> symbol_table,
                           std::shared_ptr<CompositionLearningService> composition_learning) {
    const PinyinSchemeDescriptor& scheme = resolve_pinyin_scheme(config.pinyin_scheme);
    config.pinyin_scheme = scheme.id;
    if (!pinyin_dict || !pinyin_dict->is_open() || pinyin_dict->kind() != UserDictKind::PINYIN ||
        !pinyin_resources || pinyin_resources->scheme_id() != scheme.id ||
        pinyin_resources->kind() != scheme.kind ||
        (scheme.kind == PinyinSchemeKind::kShuangpin && !pinyin_resources->has_spellings())) {
        return nullptr;
    }
    if (wubi_dict && (!wubi_dict->is_open() || wubi_dict->kind() != UserDictKind::WUBI)) {
        return nullptr;
    }
    return std::shared_ptr<const EngineRuntimeState>(new EngineRuntimeState(
        std::move(config), std::move(pinyin_dict), std::move(wubi_dict),
        std::move(pinyin_resources), std::move(symbol_table), std::move(composition_learning)));
}

} // namespace cxxime
