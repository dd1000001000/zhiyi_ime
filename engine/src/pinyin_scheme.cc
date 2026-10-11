// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/pinyin_scheme.h>

#include <algorithm>

namespace cxxime {
namespace {

// Full pinyin (initials-only abbreviations are part of it) and the shuangpin schemes, in the
// order of ImeStatus::shuangpin_scheme().
const std::vector<PinyinSchemeDescriptor> kBuiltInPinyinSchemes = {
    {"full_pinyin", L"全拼", PinyinSchemeKind::kFullPinyin, "pinyin_spellings",
     "pinyin.spellings.bin", "ni'hao"},
    {"microsoft_shuangpin", L"微软双拼", PinyinSchemeKind::kShuangpin,
     "pinyin_spellings_microsoft_shuangpin", "pinyin.microsoft-shuangpin.spellings.bin",
     "ni'hk"},
    {"xiaohe_shuangpin", L"小鹤双拼", PinyinSchemeKind::kShuangpin,
     "pinyin_spellings_xiaohe_shuangpin", "pinyin.xiaohe-shuangpin.spellings.bin", "ni'hc"},
    {"ziranma_shuangpin", L"自然码双拼", PinyinSchemeKind::kShuangpin,
     "pinyin_spellings_ziranma_shuangpin", "pinyin.ziranma-shuangpin.spellings.bin", "ni'hk"},
    {"sogou_shuangpin", L"搜狗双拼", PinyinSchemeKind::kShuangpin,
     "pinyin_spellings_sogou_shuangpin", "pinyin.sogou-shuangpin.spellings.bin", "ni'hk"},
};

} // namespace

const std::vector<PinyinSchemeDescriptor>& built_in_pinyin_schemes() {
    return kBuiltInPinyinSchemes;
}

const PinyinSchemeDescriptor* find_pinyin_scheme(std::string_view id) {
    const auto found = std::find_if(kBuiltInPinyinSchemes.begin(), kBuiltInPinyinSchemes.end(),
                                    [id](const auto& scheme) { return id == scheme.id; });
    return found == kBuiltInPinyinSchemes.end() ? nullptr : &*found;
}

const PinyinSchemeDescriptor& default_pinyin_scheme() { return kBuiltInPinyinSchemes.front(); }

const PinyinSchemeDescriptor& resolve_pinyin_scheme(std::string_view id) {
    const PinyinSchemeDescriptor* scheme = find_pinyin_scheme(id);
    return scheme ? *scheme : default_pinyin_scheme();
}

std::string normalize_pinyin_scheme_id(std::string_view id) { return resolve_pinyin_scheme(id).id; }

const PinyinSchemeDescriptor& resolve_shuangpin_scheme(std::string_view id) {
    const PinyinSchemeDescriptor* scheme = find_pinyin_scheme(id);
    return scheme && scheme->kind == PinyinSchemeKind::kShuangpin
               ? *scheme
               : *find_pinyin_scheme(kDefaultShuangpinScheme);
}

uint32_t shuangpin_scheme_index(std::string_view id) {
    const PinyinSchemeDescriptor& scheme = resolve_shuangpin_scheme(id);
    return static_cast<uint32_t>(&scheme - &kBuiltInPinyinSchemes[1]);
}

} // namespace cxxime
