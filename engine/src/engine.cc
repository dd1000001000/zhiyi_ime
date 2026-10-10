// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/engine.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iterator>
#include <optional>

#include <windows.h>

#include <cxxime/composition_learning.h>
#include <cxxime/input_limits.h>
#include <cxxime/english_candidates.h>
#include <cxxime/english_learning.h>
#include <cxxime/english_lexicon.h>
#include <cxxime/laya_rerank.h>
#include <cxxime/logging.h>
#include <cxxime/output_composer.h>
#include <cxxime/pinyin_resource.h>
#include <cxxime/pinyin_scheme.h>
#include <cxxime/symbol_table.h>
#include <cxxime/wubi_input_policy.h>
#include <cxxime/wubi_processor.h>
#include <cxxime/wubi_translator.h>

namespace cxxime {

static inline void record_total_us(QueryTrace& trace,
    std::chrono::steady_clock::time_point start, bool enabled) {
    if (enabled) {
        trace.total_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
    }
}

static void add_wubi_code_hints(const std::string& input, TranslationResult& result) {
    for (auto& entry : result.entries) {
        Candidate& candidate = entry.candidate;
        if (candidate.source != CandidateSource::kWubi || candidate.code.size() <= input.size() ||
            candidate.code.compare(0, input.size(), input) != 0) {
            continue;
        }

        const size_t suffix_size = candidate.code.size() - input.size();
        if (suffix_size > 3) {
            continue;
        }
        const auto suffix_begin = candidate.code.begin() + input.size();
        if (!std::all_of(suffix_begin, candidate.code.end(),
            [](char ch) { return ch >= 'a' && ch <= 'z'; })) {
            continue;
        }
        entry.hint.assign(suffix_begin, candidate.code.end());
    }
}

static bool is_modifier_key(uint32_t keycode) {
    switch (keycode) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
        return true;
    default:
        return false;
    }
}

static bool record_candidate_preference(Dict* dict, const Candidate* candidate,
                                        const std::string& typed_code) {
    return dict && candidate && !typed_code.empty() &&
           dict->record_candidate_preference(*candidate, typed_code);
}

static bool is_main_candidate_command(const KeyEvent& event, const Context& context) {
    if (context.candidate_count() == 0 || event.is_shift() || event.is_ctrl() ||
        event.is_alt()) {
        return false;
    }
    return (event.keycode >= '1' && event.keycode <= '9') ||
           (event.keycode == '0' && context.selectable_candidate_count() >= 10) ||
           event.keycode == VK_OEM_MINUS || event.keycode == VK_OEM_PLUS;
}

static bool is_numpad_text_key(uint32_t keycode) {
    return (keycode >= VK_NUMPAD0 && keycode <= VK_NUMPAD9) || keycode == VK_ADD ||
           keycode == VK_SUBTRACT || keycode == VK_MULTIPLY || keycode == VK_DIVIDE ||
           keycode == VK_DECIMAL;
}

static bool is_inline_ascii_character(char ch) {
    static constexpr char kInlineCharacters[] = "`~@#$%^&*_+{}|";
    return std::strchr(kInlineCharacters, ch) != nullptr;
}

static bool is_ascii_digit_or_symbol(char ch) {
    return ch >= 0x21 && ch <= 0x7e && !std::isalpha(static_cast<unsigned char>(ch));
}

static bool is_ascii_symbol(char ch) {
    return ch >= 0x21 && ch <= 0x7e && !std::isalnum(static_cast<unsigned char>(ch));
}

// Static member for global query ID generation
std::atomic<uint64_t> Engine::next_query_id_{0};

bool Engine::symbol_input_enabled(const OutputOptions& opts) const {
    const SymbolTable* symbol_table = runtime_ ? runtime_->symbol_table() : nullptr;
    return symbol_table &&
           !symbol_table->empty() &&
           opts.chinese_mode &&
           opts.chinese_punct &&
           !opts.full_shape;
}

// Self-contained: owns all resources (tests/tools).
bool Engine::initialize(const std::string& dict_path, const std::string& config_path) {
    auto dict = std::make_shared<Dict>(UserDictKind::PINYIN);
    if (!dict->open(dict_path))
        return false;

    Config config;
    if (!config_path.empty()) {
        config.load(config_path);
    }

    std::string sp_path = derive_spellings_path(dict_path);
    const auto& scheme = resolve_pinyin_scheme(config.pinyin_scheme);
    if (scheme.kind == PinyinSchemeKind::kShuangpin) {
        const std::size_t separator = sp_path.find_last_of("\\/");
        sp_path = separator == std::string::npos
                      ? scheme.spelling_filename
                      : sp_path.substr(0, separator + 1) + scheme.spelling_filename;
    }
    const PinyinSpellingRequirement requirement =
        scheme.kind == PinyinSchemeKind::kFullPinyin
            ? PinyinSpellingRequirement::kOptionalForFullPinyin
            : PinyinSpellingRequirement::kRequired;
    auto pinyin_resources =
        PinyinResourceSet::create(scheme.id, scheme.kind, sp_path, requirement);
    return initialize(EngineRuntimeState::create(
        std::move(config), std::move(dict), nullptr, std::move(pinyin_resources)));
}

bool Engine::initialize(std::shared_ptr<const EngineRuntimeState> runtime) {
    if (!runtime) {
        return false;
    }
    auto previous_runtime = std::move(runtime_);
    runtime_ = std::move(runtime);
    init_per_session(runtime_->config());
    rebuild_pipeline(InputMode::PINYIN, true);
    return true;
}

bool Engine::apply_runtime_state(std::shared_ptr<const EngineRuntimeState> runtime) {
    if (!runtime) {
        return false;
    }
    auto previous_runtime = std::move(runtime_);
    runtime_ = std::move(runtime);
    init_per_session(runtime_->config());
    rebuild_pipeline(mode_, true);
    return true;
}

void Engine::init_per_session(const Config& config) {
    ascii_composer_.load_config(config);
    english_style_shortcut_ = config.english_style_shortcut;
    ascii_toggle_shortcut_ = config.ascii_toggle_shortcut;
    punct_toggle_shortcut_ = config.punct_toggle_shortcut;
    shape_toggle_shortcut_ = config.shape_toggle_shortcut;
    english_word_mode_ = config.english.word_mode;
    pinyin_initials_ = config.pinyin_initials;
    // Start loading the Laya model in the background so it is ready by the first keystroke.
    LayaRerank::instance().preload(config);
}

namespace {

// The last `max_chars` characters of UTF-8 `text`.
std::string tail_chars(const std::string& text, std::size_t max_chars) {
    std::size_t begin = text.size();
    std::size_t chars = 0;
    while (begin > 0 && chars < max_chars) {
        --begin;
        if ((static_cast<unsigned char>(text[begin]) & 0xC0) != 0x80) ++chars;
    }
    return text.substr(begin);
}

// The first / last code point of UTF-8 `text` (0 when empty).
char32_t first_code_point(const std::string& text) {
    if (text.empty()) return 0;
    const auto lead = static_cast<unsigned char>(text[0]);
    const int length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    char32_t code = length == 1 ? lead : lead & (0x7F >> length);
    for (int i = 1; i < length && i < static_cast<int>(text.size()); ++i) {
        code = (code << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
    }
    return code;
}

char32_t last_code_point(const std::string& text) {
    std::size_t begin = text.size();
    while (begin > 0 && (static_cast<unsigned char>(text[--begin]) & 0xC0) == 0x80) {
    }
    return first_code_point(text.substr(begin));
}

// A letter or digit that is not Chinese or Japanese (Latin, Cyrillic, Hangul), not punctuation
// or a space: a word a translation is spaced from.
bool spaced_word_char(char32_t c) {
    if (c < 0x80) return std::isalnum(static_cast<int>(c)) != 0;
    return c >= 0xC0 && c != 0xD7 && c != 0xF7 && !(c >= 0x2000 && c < 0x2C00) &&
           !(c >= 0x2E80 && c < 0xA000) && !(c >= 0xF900 && c < 0xFB00) &&
           !(c >= 0xFE30 && c < 0xFE50) && !(c >= 0xFF00 && c < 0xFFF0) && c < 0x20000;
}

// Appends, keeping a bounded tail (bytes); LayaRerank trims to config.laya.context_chars.
void append_bounded(std::string& history, const std::string& text) {
    history += text;
    constexpr size_t kMaxHistoryBytes = 1024;
    if (history.size() > kMaxHistoryBytes) {
        size_t cut = history.size() - kMaxHistoryBytes / 2;
        while (cut < history.size() && (static_cast<unsigned char>(history[cut]) & 0xC0) == 0x80) {
            ++cut;  // do not split a UTF-8 sequence
        }
        history.erase(0, cut);
    }
}

// For a long input the composer offers the context model up to 8 sentences to choose among
// (pinyin_translator.cc kWideSentenceSyllables); the page shows the first kShownSentences of
// them, after reranking, and the rest go to the end, behind the words for part of the input
// (这个输入法), so those stay on the first page.
void move_extra_sentences_back(const std::string& input, TranslationResult& result) {
    constexpr std::size_t kShownSentences = 5;
    std::vector<CandidateEntry> extra;
    std::size_t sentences = 0;
    for (std::size_t i = 0; i < result.entries.size();) {
        const auto* action = std::get_if<TextSelectionAction>(&result.entries[i].selection);
        if (result.entries[i].candidate.origin == CandidateOrigin::kComposed && action &&
            action->consumed_input_bytes == input.size() && ++sentences > kShownSentences) {
            extra.push_back(std::move(result.entries[i]));
            result.entries.erase(result.entries.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (auto& entry : extra) result.entries.push_back(std::move(entry));
}

}  // namespace

const std::string& Engine::live_laya_history() const {
    static const std::string kNone;
    const int seconds = runtime_ ? runtime_->config().laya.context_memory_seconds : 0;
    if (!laya_history_read_ && seconds > 0 &&
        std::chrono::steady_clock::now() - laya_history_time_ > std::chrono::seconds(seconds)) {
        return kNone;
    }
    return laya_history_;
}

std::string Engine::laya_context(std::size_t max_chars) const {
    return tail_chars(live_laya_history(), max_chars);
}

void Engine::set_text_before_caret(const std::string& text) {
    laya_history_.clear();
    append_bounded(laya_history_, text);
    laya_history_read_ = true;
    laya_history_time_ = std::chrono::steady_clock::now();
}

void Engine::clear_laya_context() {
    laya_history_.clear();
    laya_history_read_ = false;
}

void Engine::remember_commit(const std::string& text) {
    if (text.empty()) return;
    if (&live_laya_history() != &laya_history_) laya_history_.clear();  // forgotten: start afresh
    append_bounded(laya_history_, text);
    laya_history_time_ = std::chrono::steady_clock::now();
}

std::string Engine::laya_context(const CompositionState& state) const {
    std::string context = live_laya_history();
    for (const auto& segment : state.converted_segments()) context += segment.text;
    return context;
}

void Engine::finalize() {
    reset_composition_state();
    handled_shortcut_key_ = 0;
    translator_.reset();
    processor_.reset();
    runtime_.reset();
}

CandidatePage Engine::translate_for_search(const std::string& input, int limit) {
    if (!translator_ || input.empty() || limit <= 0) {
        return {};
    }

    QueryScratch scratch;
    QueryBudget budget = budget_;
    budget.deadline = QueryDeadline{};
    TranslationRequest request;
    request.scheme = scheme_for_mode(mode_);
    request.input = input;
    request.page_size = limit;
    request.budget = &budget;
    request.scratch = &scratch;
    return translator_->translate(request).candidate_page();
}

bool Engine::record_search_result(const std::string& input, const std::string& result) {
    if (!translator_ || input.empty() || result.empty()) {
        return false;
    }

    const CandidatePage page = translate_for_search(input, 64);
    const auto candidate = std::find_if(
        page.candidates.begin(), page.candidates.end(),
        [&](const Candidate& item) { return item.text == result; });
    if (candidate == page.candidates.end()) {
        return false;
    }
    Dict* dict = candidate->source == CandidateSource::kWubi
                     ? runtime_->wubi_dict()
                     : &runtime_->pinyin_dict();
    const std::string& learning_code =
        candidate->source == CandidateSource::kPinyin && !candidate->input_code.empty()
            ? candidate->input_code
            : input;
    return record_candidate_preference(dict, &*candidate, learning_code);
}

ProcessResult Engine::process_key(const KeyEvent& event) {
    return process_key(event, OutputOptions{});
}

ProcessResult Engine::process_key(const KeyEvent& event, const OutputOptions& opts,
                                  int visible_candidate_count) {
    CXXIME_LOG(L"Engine::process_key: vk=%u, is_key_up=%d, composing=%d",
               event.keycode, event.is_key_up, context_.is_composing());
    const uint64_t input_revision_before = context_.preedit_revision();
    context_.visible_candidate_count = (std::max)(0, visible_candidate_count);

    // Initialize the trace for this query when tracing is enabled.
    // Preserve session_id/revision set by caller (server) before this call.
    std::chrono::steady_clock::time_point total_start;
    if (trace_enabled_) {
        uint32_t saved_session_id = trace_.session_id;
        uint64_t saved_revision = trace_.revision;
        trace_ = QueryTrace{};
        trace_.query_id = next_query_id_.fetch_add(1, std::memory_order_relaxed);
        trace_.session_id = saved_session_id;
        trace_.revision = saved_revision;
        total_start = std::chrono::steady_clock::now();
    }

    // Create the per-query deadline.
    QueryDeadline per_query_deadline = QueryDeadline::from_now(query_deadline_ms_);

    // Let AsciiComposer track modifier key state and update ascii_mode when needed.
    const bool modifier_binding_applied =
        ascii_composer_.process_key(event.keycode, event.is_key_up, context_,
                                    event.is_caps_lock());

    // The key after a commit decides what the commit taught: Backspace takes it back (the
    // text was wrong), any other key keeps it; modifiers alone decide nothing.
    if (!event.is_key_up && last_learning_.armed && !is_modifier_key(event.keycode)) {
        if (event.keycode == VK_BACK && !context_.is_composing() && !event.is_ctrl() &&
            !event.is_alt()) {
            revoke_last_commit_learning();
        } else {
            last_learning_ = {};
        }
    }

    CXXIME_LOG(L"Engine::process_key: after ascii_composer, committed_text='%S'",
               context_.committed_text.c_str());

    if (event.is_key_up && handled_shortcut_key_ != 0 &&
        event.keycode == handled_shortcut_key_) {
        handled_shortcut_key_ = 0;
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::INPUT_MODE_SHORTCUT_HANDLED;
    }
    if (!event.is_key_up && handled_shortcut_key_ != 0 &&
        event.keycode != handled_shortcut_key_) {
        handled_shortcut_key_ = 0;
    }
    // Chinese/English switch shortcut (a key combination; tapping a modifier alone is handled
    // by the AsciiComposer above). Its key-up is swallowed like the style shortcut's.
    if (!event.is_key_up && ascii_toggle_shortcut_.matches(event)) {
        record_total_us(trace_, total_start, trace_enabled_);
        if (handled_shortcut_key_ != 0) {
            return ProcessResult::INPUT_MODE_SHORTCUT_HANDLED;  // auto-repeat
        }
        handled_shortcut_key_ = event.keycode;
        ascii_composer_.toggle_by_shortcut(context_);
        if (!context_.committed_text.empty()) {
            apply_commit_learning_plan();
            return ProcessResult::COMMITTED;
        }
        return ProcessResult::INPUT_MODE_SHORTCUT_HANDLED;
    }

    // Punctuation (Chinese/English) and full/half width shortcuts; auto-repeat and the key-up
    // are swallowed like the other switch keys'.
    for (const auto& [shortcut, toggle] :
         {std::pair{&punct_toggle_shortcut_, ProcessResult::TOGGLE_PUNCT},
          std::pair{&shape_toggle_shortcut_, ProcessResult::TOGGLE_SHAPE}}) {
        if (!event.is_key_up && shortcut->matches(event)) {
            record_total_us(trace_, total_start, trace_enabled_);
            if (handled_shortcut_key_ != 0) {
                return ProcessResult::INPUT_MODE_SHORTCUT_HANDLED;
            }
            handled_shortcut_key_ = event.keycode;
            return toggle;
        }
    }

    // Style shortcut: in English mode word completion <-> letter by letter (a word being typed
    // stays open; the new style applies from the next word); in Chinese pinyin mode full
    // pinyin <-> initials. Not used in Wubi mode.
    const bool pinyin_style_key = opts.chinese_mode && mode_ == InputMode::PINYIN;
    if (!event.is_key_up && english_style_shortcut_.matches(event) &&
        (!opts.chinese_mode || pinyin_style_key)) {
        record_total_us(trace_, total_start, trace_enabled_);
        if (handled_shortcut_key_ == 0) {
            handled_shortcut_key_ = event.keycode;
            return pinyin_style_key ? ProcessResult::TOGGLE_PINYIN_STYLE
                                    : ProcessResult::TOGGLE_ENGLISH_STYLE;
        }
        return ProcessResult::INPUT_MODE_SHORTCUT_HANDLED;
    }

    // Learning mode: Ctrl+1..9 types that candidate's translation (docs/learning-mode.md). While
    // typing the key is ours even without a translation, so it never reaches the application
    // (a browser would switch tabs); auto-repeat and the key-up are swallowed too.
    const Config* config = runtime_ ? &runtime_->config() : nullptr;
    if (!event.is_key_up && event.is_ctrl() && !event.is_alt() && !event.is_shift() &&
        event.keycode >= '1' && event.keycode <= '9' && config &&
        (!config->chinese_gloss_target.empty() || !config->english_gloss_target.empty()) &&
        config->layout != "horizontal" &&
        (context_.is_composing() || handled_shortcut_key_ == event.keycode)) {
        record_total_us(trace_, total_start, trace_enabled_);
        if (handled_shortcut_key_ == event.keycode) {
            return ProcessResult::INPUT_MODE_SHORTCUT_HANDLED;
        }
        handled_shortcut_key_ = event.keycode;
        const int index = static_cast<int>(event.keycode - '1');
        if (index >= context_.selectable_candidate_count()) {
            return ProcessResult::ACCEPTED;
        }
        translation_request_ = index;
        return ProcessResult::COMMIT_TRANSLATION;
    }

    // Application and system shortcuts own modified key combinations unless
    // CxxIME matched an explicit shortcut above.
    if (event.is_ctrl() || event.is_alt()) {
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::REJECTED;
    }

    const std::optional<char> normalized = normalize_ascii_key(event);

    // The numeric keypad is a literal number/formula input area. Its digits and operators stay
    // ASCII regardless of Chinese punctuation or full-shape mode.
    if (!event.is_key_up && !context_.is_composing() && normalized &&
        is_numpad_text_key(event.keycode)) {
        context_.committed_text.assign(1, *normalized);
        context_.set_commit_source(CommitSource::kRawCodePretransformed);
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::COMMITTED;
    }

    // Full-shape mode is an explicit literal-symbol input mode. Handle idle main-keyboard
    // symbols before Chinese punctuation and the backslash Symbol trigger.
    if (!event.is_key_up && !context_.is_composing() && opts.full_shape && normalized &&
        is_ascii_symbol(*normalized) && !is_numpad_text_key(event.keycode) &&
        handle_full_shape(event, context_, opts)) {
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::COMMITTED;
    }

    // Check if AsciiComposer committed text (e.g. Shift toggle with code style)
    if (!context_.committed_text.empty()) {
        apply_commit_learning_plan();
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::COMMITTED;
    }

    // Laya: English word completion in English (ASCII) mode.
    if (auto english = process_english_key(event, opts)) {
        record_total_us(trace_, total_start, trace_enabled_);
        return *english;
    }

    // Keep modifier key-up available to the host while allowing the server to return the
    // composition snapshot after a binding converts IME/Symbol input to inline ASCII.
    if (event.is_key_up && modifier_binding_applied && context_.is_composing()) {
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::ACCEPTED;
    }

    // Enter always submits the exact active preedit without changing the persistent mode.
    if (!event.is_key_up && event.keycode == VK_RETURN && context_.is_composing()) {
        context_.finalize_raw(CommitSource::kRawCodePreserveCase);
        apply_commit_learning_plan();
        ascii_composer_.finish_temporary_ascii();
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::COMMITTED;
    }

    const InlineAsciiResult inline_result =
        ascii_composer_.process_inline_ascii_composition(event, context_, opts.chinese_mode);
    if (inline_result != InlineAsciiResult::kNotHandled) {
        if (inline_result == InlineAsciiResult::kResumeOrigin && context_.is_composing()) {
            context_.update_translation(translate_current_composition(per_query_deadline));
        }
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::ACCEPTED;
    }

    // Propagate CapsLock style to Context for PinyinProcessor
    context_.caps_lock_style = ascii_composer_.get_binding(VK_CAPITAL);

    // CapsLock plus a letter commits directly with case inversion.
    // When CapsLock is not configured as an IME switch, keep the OS CapsLock
    // behavior: letters commit directly with case inversion.
    if (!event.is_key_up && event.is_caps_lock() && !ascii_composer_.is_ascii_mode() &&
        context_.caps_lock_style == AsciiModeSwitchStyle::NOOP) {
        uint32_t vk = event.keycode;
        if (vk >= 'A' && vk <= 'Z') {
            char ch = static_cast<char>(vk);
            if (event.is_shift())
                ch = static_cast<char>(tolower(ch));
            else
                ch = static_cast<char>(toupper(ch));
            context_.committed_text = std::string(1, ch);
            context_.set_commit_source(CommitSource::kRawCodePretransformed);
            record_total_us(trace_, total_start, trace_enabled_);
            return ProcessResult::COMMITTED;
        }
    }

    // Intercept digit keys in English full-width mode.
    if (!context_.is_composing() &&
        OutputComposer::intercept_key(event, opts, context_.committed_text)) {
        context_.set_commit_source(CommitSource::kRawCode);
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::COMMITTED;
    }

    // If in ASCII mode, handle letters/space directly
    if (ascii_composer_.is_ascii_mode() && !context_.is_composing() && !event.is_key_up) {
        uint32_t vk = event.keycode;

        // Letter keys (A-Z): commit as single ASCII char, respect Shift and CapsLock
        if (vk >= 'A' && vk <= 'Z') {
            char ch = static_cast<char>(vk);
            // Shift XOR CapsLock means uppercase; neither or both means lowercase.
            bool upper = event.is_shift() != event.is_caps_lock();
            if (!upper)
                ch = static_cast<char>(tolower(ch));
            context_.committed_text = opts.full_shape
                ? OutputComposer::to_full_width(ch)
                : std::string(1, ch);
            context_.set_commit_source(CommitSource::kRawCodePretransformed);
            if (ascii_composer_.is_temporary_ascii()) {
                ascii_composer_.set_ascii_mode(false);
            }
            if (trace_enabled_) {
                auto total_end = std::chrono::steady_clock::now();
                trace_.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();
            }
            return ProcessResult::COMMITTED;
        }

        // Idle half-width Space belongs to the host, including application shortcuts.
        // Only full-width Space needs an IME text commit.
        if (vk == 0x20) {  // VK_SPACE
            if (!opts.full_shape) {
                record_total_us(trace_, total_start, trace_enabled_);
                return ProcessResult::REJECTED;
            }
            context_.committed_text = OutputComposer::to_full_width(' ');
            context_.set_commit_source(CommitSource::kRawCode);
            if (trace_enabled_) {
                auto total_end = std::chrono::steady_clock::now();
                trace_.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();
            }
            return ProcessResult::COMMITTED;
        }

        // Enter is an application command in ASCII mode. Let the host handle it
        // so address bars and dialogs can navigate/confirm.
        if (vk == 0x0D) {  // VK_RETURN
            if (trace_enabled_) {
                auto total_end = std::chrono::steady_clock::now();
                trace_.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();
            }
            return ProcessResult::REJECTED;
        }

        // Punctuation / full-shape handling
        if (handle_punctuation(event, context_, opts)) {
            if (trace_enabled_) {
                auto total_end = std::chrono::steady_clock::now();
                trace_.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();
            }
            return ProcessResult::COMMITTED;
        }
        if (handle_full_shape(event, context_, opts)) {
            if (trace_enabled_) {
                auto total_end = std::chrono::steady_clock::now();
                trace_.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();
            }
            return ProcessResult::COMMITTED;
        }

        // Other keys: reject (pass through to app)
        record_total_us(trace_, total_start, trace_enabled_);
        return ProcessResult::REJECTED;
    }

    bool wubi_continuation_commit = false;
    const bool symbol_trigger_enabled = symbol_input_enabled(opts);
    const bool symbol_trigger = symbol_trigger_enabled && !context_.is_composing() &&
                                SymbolProcessor::is_trigger(event);
    const bool plain_main_zero = event.keycode == '0' && !event.is_shift();
    const bool processor_code_key = normalized && is_ascii_digit_or_symbol(*normalized) &&
                                    processor_ && processor_->accepts_code_key(event, context_);
    std::optional<ProcessResult> routed_result;

    if (context_.composition_scheme() != CompositionScheme::kSymbol &&
        context_.composition_scheme() != CompositionScheme::kInlineAscii &&
        context_.is_composing() &&
        !processor_code_key &&
        normalized && is_ascii_digit_or_symbol(*normalized) &&
        !is_main_candidate_command(event, context_)) {
        const bool has_candidates = context_.candidate_count() > 0;
        const bool start_inline = is_inline_ascii_character(*normalized) ||
                                  plain_main_zero || is_numpad_text_key(event.keycode) ||
                                  !has_candidates;
        if (opts.full_shape && !is_numpad_text_key(event.keycode)) {
            if (handle_full_shape(event, context_, opts)) {
                routed_result = ProcessResult::COMMITTED;
            }
        } else if (start_inline) {
            if (context_.composition().raw_input_size() < kMaxInputCodeLength) {
                context_.enter_inline_ascii(true);
                context_.insert_preedit(*normalized);
                context_.clear_translation();
                context_.reset_pagination();
            }
            routed_result = ProcessResult::ACCEPTED;
        } else {
            if (!handle_punctuation(event, context_, opts)) {
                commit_with_punctuation(context_, std::string(1, *normalized));
            }
            routed_result = ProcessResult::COMMITTED;
        }
    }

    const bool plain_letter_key = !event.is_key_up && !event.is_shift() && !event.is_ctrl() &&
                                  !event.is_alt() && !event.is_caps_lock() &&
                                  event.keycode >= 'A' && event.keycode <= 'Z';
    const WubiFifthKeyAction fifth_key_action = plain_letter_key
        ? WubiInputPolicy::fifth_key_action(context_.composition_scheme(),
                                            context_.active_input(), context_.preedit_cursor(),
                                            context_.translation(), runtime_->config(),
                                            event.keycode)
        : WubiFifthKeyAction::kNone;
    if (!routed_result && fifth_key_action == WubiFifthKeyAction::kCommitFirstAndRestart) {
        const CandidateEntry* first = context_.candidate_entry(0);
        if (first && finalize_selection(*first)) {
            const char restarted_code = static_cast<char>(event.keycode - 'A' + 'a');
            context_.start_composition(scheme_for_mode(mode_), std::string(1, restarted_code), 1);
            wubi_continuation_commit = true;
            routed_result = ProcessResult::ACCEPTED;
        }
    } else if (!routed_result && fifth_key_action == WubiFifthKeyAction::kRestartAfterMiss) {
        const char restarted_code = static_cast<char>(event.keycode - 'A' + 'a');
        context_.start_composition(scheme_for_mode(mode_), std::string(1, restarted_code), 1);
        context_.clear_translation();
        context_.reset_pagination();
        routed_result = ProcessResult::ACCEPTED;
    }

    // Process pinyin and Wubi input.
    std::chrono::steady_clock::time_point t0, t1, t2;
    if (trace_enabled_) {
        t0 = std::chrono::steady_clock::now();
    }
    ProcessResult result;
    bool candidate_action_applied = false;
    if (routed_result) {
        result = *routed_result;
    } else if (SymbolProcessor::is_active(context_) &&
               context_.active_input() == std::string(1, kSymbolPrefix) &&
               SymbolProcessor::is_trigger(event)) {
        // A second \ right after the one that opened symbol input: the \ punctuation (、).
        context_.reset();
        if (!handle_punctuation(event, context_, opts)) {
            commit_with_punctuation(context_, std::string(1, kSymbolPrefix));
        }
        result = ProcessResult::COMMITTED;
    } else if (SymbolProcessor::is_active(context_) || symbol_trigger) {
        result = symbol_processor_.process_key(event, context_, symbol_trigger_enabled);
    } else {
        result = processor_->process_key(event, context_);
    }
    if (result == ProcessResult::CANDIDATE_SELECTED) {
        const std::optional<int> requested = context_.take_requested_candidate_selection();
        if (!requested || !dispatch_candidate_selection(*requested, per_query_deadline)) {
            result = ProcessResult::ACCEPTED;
        } else {
            candidate_action_applied = context_.is_composing();
            result = context_.is_composing() ? ProcessResult::ACCEPTED : ProcessResult::COMMITTED;
        }
    }

    if (result == ProcessResult::REJECTED &&
        context_.composition_scheme() == CompositionScheme::kSymbol && normalized &&
        is_ascii_digit_or_symbol(*normalized)) {
        const bool has_candidates = context_.candidate_count() > 0;
        const bool start_inline = is_inline_ascii_character(*normalized) ||
                                  plain_main_zero || is_numpad_text_key(event.keycode);
        if (opts.full_shape && !is_numpad_text_key(event.keycode)) {
            if (handle_full_shape(event, context_, opts)) {
                result = ProcessResult::COMMITTED;
            }
        } else if (has_candidates && !start_inline) {
            if (!handle_punctuation(event, context_, opts)) {
                commit_with_punctuation(context_, std::string(1, *normalized));
            }
            result = ProcessResult::COMMITTED;
        } else {
            if (context_.composition().raw_input_size() < kMaxInputCodeLength) {
                context_.enter_inline_ascii(true);
                context_.insert_preedit(*normalized);
                context_.clear_translation();
                context_.reset_pagination();
            }
            result = ProcessResult::ACCEPTED;
        }
    }
    const std::optional<CandidateNavigationRequest> navigation =
        context_.take_candidate_navigation();
    const bool input_changed = context_.preedit_revision() != input_revision_before;
    if (input_changed) {
        context_.reset_pagination();
    }
    const bool previous_page_requested =
        navigation && navigation->direction == CandidateNavigation::kPreviousPage;
    const bool next_page_requested =
        navigation && navigation->direction == CandidateNavigation::kNextPage;
    const bool previous_page_applied =
        !input_changed && previous_page_requested &&
        context_.apply_previous_page(navigation->highlight_last);
    CandidateNavigationOutcome navigation_outcome = previous_page_applied
                                                        ? CandidateNavigationOutcome::kMovedPrevious
                                                        : CandidateNavigationOutcome::kNone;
    uint32_t continuation_effort = 0;
    if (trace_enabled_) {
        t1 = std::chrono::steady_clock::now();
        trace_.processor_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        // Record raw_input AFTER processor updates the buffer (captures current state, not stale)
        if (result == ProcessResult::ACCEPTED) {
            size_t input_len = context_.active_input().size();
            if (input_len >= sizeof(trace_.raw_input))
                input_len = sizeof(trace_.raw_input) - 1;
            std::memcpy(trace_.raw_input, context_.active_input().data(), input_len);
            trace_.raw_input[input_len] = '\0';
        }
    }

    // Preserve explicit sources set by candidate and raw-text finalization.
    if (result == ProcessResult::COMMITTED &&
        context_.commit_source() == CommitSource::kRawCode) {
        if (context_.active_input().empty()) {
            context_.set_commit_source(CommitSource::kRawCode);
        } else {
            context_.set_commit_source(CommitSource::kCandidate);
        }
    }

    // Auto-restore from temporary inline_ascii when composition ends
    if (result == ProcessResult::COMMITTED && ascii_composer_.is_temporary_ascii()) {
        ascii_composer_.finish_temporary_ascii();
    }

    // Handle punctuation and full-width input only when the input processor rejects the key.
    if (result == ProcessResult::REJECTED && !SymbolProcessor::is_active(context_)) {
        if (handle_punctuation(event, context_, opts)) {
            result = ProcessResult::COMMITTED;
        } else if (handle_full_shape(event, context_, opts)) {
            result = ProcessResult::COMMITTED;
        }
    }

    // Update candidates after processing while the composition remains active.
    if (result == ProcessResult::ACCEPTED && context_.is_composing()) {
        if (trace_enabled_) {
            trace_.page_index = context_.page_index();
            trace_.page_size = runtime_->config().page_size;
        }
        bool append_raw = context_.composition_scheme() == CompositionScheme::kInlineAscii ||
                                context_.commit_source() == CommitSource::kRawCodePreserveCase;
        const bool refresh_candidates =
            !candidate_action_applied && (input_changed || next_page_requested);
        if (refresh_candidates) {
            // Skip translate if deadline already expired (e.g. slow ascii_composer/processor)
            if (per_query_deadline.enabled && per_query_deadline.expired()) {
                if (trace_enabled_) {
                    trace_.deadline_exceeded = true;
                    trace_.truncated = true;
                }
                if (input_changed) {
                    context_.clear_translation();
                } else if (next_page_requested) {
                    navigation_outcome = CandidateNavigationOutcome::kRetryable;
                }
            } else if (append_raw) {
                context_.clear_translation();
                context_.reset_pagination();
            } else if (next_page_requested && !input_changed) {
                const int visible_count = context_.selectable_candidate_count();
                const int target_offset = context_.page_offset() + visible_count;
                const CandidateExtent current_extent = context_.translation().extent;
                const bool anchored_continuation =
                    context_.composition_scheme() != CompositionScheme::kSymbol;
                if (visible_count > 0 &&
                    candidate_extent_may_continue(current_extent, target_offset)) {
                    continuation_effort = !current_extent.complete
                                              ? context_.begin_candidate_continuation()
                                              : 0;
                    bool sequence_invalidated = false;
                    TranslationResult translated =
                        anchored_continuation
                            ? translate_after_visible_anchor(per_query_deadline, visible_count,
                                                             target_offset, continuation_effort,
                                                             &sequence_invalidated)
                            : translate_composition(context_.composition(),
                                                    context_.page_index() + 1, target_offset,
                                                    per_query_deadline);
                    if (sequence_invalidated && translated.usable()) {
                        context_.replace_candidate_sequence(std::move(translated));
                        navigation_outcome = CandidateNavigationOutcome::kSequenceReset;
                    } else if (translated.usable() && !translated.entries.empty()) {
                        if (context_.apply_next_page(std::move(translated), visible_count)) {
                            navigation_outcome = CandidateNavigationOutcome::kMovedNext;
                        }
                    } else if (translated.usable()) {
                            context_.update_current_extent(translated.extent);
                            navigation_outcome =
                                translated.extent.state == CandidateExtentState::kExhausted
                                    ? CandidateNavigationOutcome::kConfirmedEnd
                                    : CandidateNavigationOutcome::kRetryable;
                    } else {
                        context_.update_current_extent(translated.extent);
                        navigation_outcome = CandidateNavigationOutcome::kRetryable;
                    }
                } else {
                    navigation_outcome = CandidateNavigationOutcome::kConfirmedEnd;
                }
            } else {
                TranslationResult translated = translate_current_composition(per_query_deadline);
                if (translated.usable()) {
                    context_.update_translation(std::move(translated));
                } else {
                    context_.clear_translation();
                }
            }
        }
        if (trace_enabled_) {
            trace_.candidate_count = context_.candidate_count();
            trace_.candidate_known_count = context_.translation().extent.known_count;
            trace_.candidate_extent_state =
                static_cast<uint32_t>(context_.translation().extent.state);
            trace_.candidate_extent_complete = context_.translation().extent.complete ? 1u : 0u;
            trace_.navigation_outcome = navigation_outcome;
            trace_.continuation_effort = continuation_effort;
            trace_.page_index = context_.page_index();
        }

        // Auto-commit a unique 4-code Wubi candidate in Wubi or mixed mode.
        if ((refresh_candidates || previous_page_applied) && !append_raw &&
            result == ProcessResult::ACCEPTED &&
            WubiInputPolicy::should_auto_commit(context_.composition_scheme(),
                                                context_.active_input(),
                                                context_.translation(),
                                                runtime_->config())) {
            const CandidateEntry* first = context_.candidate_entry(0);
            if (first && finalize_selection(*first)) {
                result = ProcessResult::COMMITTED;
            }
        }
    }
    if (wubi_continuation_commit && result == ProcessResult::ACCEPTED) {
        context_.set_commit_source(CommitSource::kCandidate);
        result = ProcessResult::COMMITTED;
    }
    if (trace_enabled_) {
        t2 = std::chrono::steady_clock::now();
        trace_.translate_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
    }

    CXXIME_LOG(L"Engine::process_key: result=%d, input='%S'",
               (int)result, context_.active_input().c_str());

    if (result == ProcessResult::COMMITTED) {
        apply_commit_learning_plan();
    }

    // Finalize the query trace.
    if (trace_enabled_) {
        auto total_end = std::chrono::steady_clock::now();
        trace_.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();
    }

    return result;
}

void Engine::commit_with_punctuation(Context& context, const std::string& output) {
    const int selectable_count = context.selectable_candidate_count();
    if (context.is_composing() && selectable_count > 0) {
        int index = context.highlighted();
        if (index < 0 || index >= selectable_count) {
            index = 0;
        }
        if (context.commit_candidate(index)) {
            context.committed_text += output;
            context.set_commit_source(CommitSource::kCandidate);
        } else {
            context.set_commit_source(CommitSource::kRawCodePreserveCase);
            auto committed = context.commit_with_source();
            context.committed_text = std::move(committed.first) + output;
            context.set_commit_source(CommitSource::kRawCodePreserveCase);
        }
    } else if (context.is_composing()) {
        context.set_commit_source(CommitSource::kRawCodePreserveCase);
        auto committed = context.commit_with_source();
        context.committed_text = std::move(committed.first) + output;
        context.set_commit_source(CommitSource::kRawCodePreserveCase);
    } else {
        context.committed_text = output;
        context.set_commit_source(CommitSource::kRawCodePretransformed);
    }
    context.last_committed_char = output.back();
}

bool Engine::handle_punctuation(const KeyEvent& event, Context& context,
                                const OutputOptions& opts) {
    // Guard: key-up
    if (event.is_key_up)
        return false;

    // Guard: modifier keys
    if (event.is_ctrl() || event.is_alt())
        return false;

    // Semantic punctuation mappings apply only in half-shape Chinese punctuation mode.
    if (!opts.chinese_punct || opts.full_shape)
        return false;

    // Step 1: convert virtual key to character.
    const std::optional<char> normalized = normalize_ascii_key(event);
    if (!normalized)
        return false;
    const char ch = *normalized;

    // Step 2: check punct_mapping
    if (!opts.punct_mapping)
        return false;

    // Step 3: select mapping table
    std::string key(1, ch);
    const PunctEntry* entry = nullptr;
    auto it = opts.punct_mapping->half_shape.find(key);
    if (it != opts.punct_mapping->half_shape.end())
        entry = &it->second;

    // Step 4: not found
    if (!entry)
        return false;

    // Step 5: digit separator guard
    if (context.active_input().empty() &&
        std::isdigit(static_cast<unsigned char>(context.last_committed_char))) {
        if (key == "." || key == ",")
            return false;
    }

    // Step 6: process punct type
    std::string output;
    switch (entry->type) {
    case PunctType::COMMIT:
        output = entry->commit;
        break;
    case PunctType::PAIR: {
        bool opened = context.pair_open[key];
        int idx = opened ? 1 : 0;
        output = entry->pair[idx];
        context.pair_open[key] = !opened;
        break;
    }
    case PunctType::ALTERNATIVES: {
        int idx = context.alt_index[key] % static_cast<int>(entry->alternatives.size());
        output = entry->alternatives[idx];
        context.alt_index[key] = idx + 1;
        break;
    }
    }

    // Step 7: commit
    commit_with_punctuation(context, output);
    CXXIME_LOG(L"handle_punctuation: key='%c' -> '%S'", ch, output.c_str());
    return true;
}

bool Engine::handle_full_shape(const KeyEvent& event, Context& context, const OutputOptions& opts) {
    // Guard: key-up
    if (event.is_key_up)
        return false;

    // Guard: modifier keys
    if (event.is_ctrl() || event.is_alt())
        return false;

    // Guard: full_shape mode
    if (!opts.full_shape)
        return false;

    // Step 1: convert virtual key to character.
    const std::optional<char> normalized = normalize_ascii_key(event);
    if (!normalized)
        return false;
    const char ch = *normalized;

    // Step 2: range check
    if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e)
        return false;

    // Step 3: full-width conversion
    std::string output = OutputComposer::to_full_width(ch);

    // Step 4: commit
    commit_with_punctuation(context, output);
    CXXIME_LOG(L"handle_full_shape: '%c' -> full-width", ch);
    return true;
}

const Context& Engine::context() const {
    return context_;
}

Context& Engine::context() {
    return context_;
}

bool Engine::select_candidate(int index) {
    if (index < 0 || index >= context_.candidate_count()) {
        return false;
    }
    if (!dispatch_candidate_selection(index, QueryDeadline::from_now(query_deadline_ms_))) {
        return false;
    }
    if (!context_.is_composing()) {
        apply_commit_learning_plan();
    }
    return true;
}

ProcessResult Engine::commit_translation(int index, const std::string& translation,
                                         bool spaced) {
    const CandidateEntry* entry = context_.candidate_entry(index);
    const auto* action = entry ? std::get_if<TextSelectionAction>(&entry->selection) : nullptr;
    if (!action || translation.empty() ||
        action->consumed_input_bytes > context_.active_input().size()) {
        return ProcessResult::ACCEPTED;
    }
    std::string text;
    for (const auto& segment : context_.composition().converted_segments()) text += segment.text;
    // "现在" then "now": no space; "I" then "now": a space, as between typed words.
    if (spaced && spaced_word_char(last_code_point(live_laya_history() + text))) {
        text += ' ';
    }
    text += translation;
    const std::string rest = context_.active_input().substr(action->consumed_input_bytes);
    const bool english = english_composing_;
    reset_composition_state();
    english_composing_ = false;
    if (!rest.empty() && !english &&
        context_.start_composition(scheme_for_mode(mode_), rest, rest.size())) {
        TranslationResult translated =
            translate_current_composition(QueryDeadline::from_now(query_deadline_ms_));
        if (translated.usable()) context_.update_translation(std::move(translated));
    }
    context_.committed_text = std::move(text);
    context_.set_commit_source(CommitSource::kCandidate);
    return ProcessResult::COMMITTED;
}

std::string Engine::get_commit_text() {
    std::string text = context_.committed_text;
    reset_composition_state();
    remember_commit(text);
    return text;
}

std::pair<std::string, CommitSource> Engine::take_commit_text_with_source() {
    auto result = std::make_pair(std::move(context_.committed_text), context_.commit_source());
    remember_commit(result.first);
    if (context_.is_composing()) {
        context_.committed_text.clear();
        context_.set_commit_source(CommitSource::kRawCode);
    } else {
        reset_composition_state();
    }
    return result;
}

std::pair<std::string, CommitSource> Engine::commit_composition_with_source() {
    auto result = context_.commit_with_source();
    remember_commit(result.first);
    apply_commit_learning_plan();
    ascii_composer_.finish_temporary_ascii();
    return result;
}

std::string Engine::commit_raw_composition() {
    if (!context_.finalize_raw(CommitSource::kRawCodePreserveCase)) {
        return {};
    }
    apply_commit_learning_plan();
    std::string raw = std::move(context_.committed_text);
    reset_composition_state();
    remember_commit(raw);
    return raw;
}

void Engine::clear() {
    reset_composition_state();
    handled_shortcut_key_ = 0;
}

void Engine::clear_composition() {
    reset_composition_state();
    handled_shortcut_key_ = 0;
}

void Engine::reset_composition_state() {
    context_.reset();
    ascii_composer_.finish_temporary_ascii();
}

TranslationResult Engine::translate_current_composition(const QueryDeadline& deadline) {
    return translate_composition(context_.composition(), context_.page_index(),
                                 context_.page_offset(), deadline);
}

TranslationResult Engine::translate_composition(const CompositionState& state,
                                                int page_index,
                                                int page_offset,
                                                const QueryDeadline& deadline,
                                                int candidate_limit,
                                                uint32_t continuation_effort) {
    if (state.active().scheme == CompositionScheme::kSymbol) {
        TranslationRequest request;
        request.scheme = CompositionScheme::kSymbol;
        request.input = state.active().input.substr(1);
        request.page_index = page_index;
        request.page_offset = page_offset;
        request.page_size = candidate_limit > 0 ? candidate_limit : runtime_->config().page_size;
        const SymbolTable* symbol_table = runtime_->symbol_table();
        return symbol_table ? symbol_table->translate(request) : TranslationResult{};
    }

    scratch_.reset_for_query();
    QueryBudget effective_budget =
        make_budget(static_cast<int>(state.active().input.size()),
                    candidate_limit > 0 ? candidate_limit : runtime_->config().page_size);
    effective_budget = scale_query_budget(effective_budget, continuation_effort);
    effective_budget.deadline = deadline;
    TranslationRequest request;
    request.scheme = state.active().scheme;
    request.input = state.active().input;
    request.page_index = page_index;
    request.page_offset = page_offset;
    request.page_size = candidate_limit > 0 ? candidate_limit : runtime_->config().page_size;
    // Laya looks at laya_candidate_count() candidates (2 * page_size by default): the first
    // pinyin page fetches that many and keeps page_size after reranking. (Later pages re-query from the start with a longer limit,
    // rerank the same way and skip the candidates already shown.)
    const int page_size = runtime_->config().page_size;
    const bool laya_first_page = request.scheme == CompositionScheme::kPinyin &&
                                 page_index == 0 && page_offset == 0 && candidate_limit <= 0 &&
                                 runtime_->config().laya.enable;
    if (laya_first_page) {
        request.page_size = (std::max)(page_size, runtime_->config().laya_candidate_count());
    }
    request.policy = translation_policy_;
    request.trace = trace_enabled_ ? &trace_ : nullptr;
    request.budget = &effective_budget;
    request.scratch = &scratch_;
    TranslationResult result = translator_->translate(request);
    if (runtime_->config().wubi_code_hint) {
        add_wubi_code_hints(state.active().input, result);
    }
    // Laya: reorder the first pinyin page by context. Later pages keep the translator's order,
    // so page 1 stays a permutation of the original first page.
    if (request.scheme == CompositionScheme::kPinyin && page_index == 0 && page_offset == 0) {
        LayaRerank::instance().apply(runtime_->config(), laya_context(state), request.input, result);
        move_extra_sentences_back(request.input, result);
        if (laya_first_page && static_cast<int>(result.entries.size()) > page_size) {
            result.entries.resize(page_size);
            result.page_size = page_size;
            result.extent = make_candidate_extent(result.extent.known_count, page_size,
                                                  !result.extent.complete);
        }
        add_english_candidates(request.input, page_size, result);
        // Nothing matched (neither pinyin nor an English word): offer the typed text itself.
        const std::string& input = request.input;
        // Only for a completed query: a failed or degraded one keeps the engine's retry and
        // retention behaviour.
        if (result.entries.empty() && input.size() >= 2 && state.converted_segments().empty() &&
            result.status == TranslationStatus::kSuccess && result.extent.complete &&
            std::all_of(input.begin(), input.end(), [](char c) { return c >= 'a' && c <= 'z'; })) {
            Candidate raw;
            raw.text = input;
            raw.source = CandidateSource::kEnglish;
            raw.code = input;
            raw.input_code = input;
            result.entries.push_back(make_text_candidate_entry(std::move(raw), input.size(), input));
            result.highlighted = 0;
            result.extent = make_candidate_extent(1, 1, false);
        }
    }
    return result;
}

void Engine::add_english_candidates(const std::string& input, int page_size,
                                    TranslationResult& result) const {
    const auto& ec = runtime_->config().english;
    if (!ec.mixed_in_chinese || static_cast<int>(input.size()) < ec.min_input) return;
    if (!std::all_of(input.begin(), input.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return;
    auto lexicon = EnglishLexicon::shared();
    if (!lexicon) return;

    // Only a word typed in full (exact match, rare words included); the best-scored spelling
    // when several differ in case ("us" / "US").
    const std::vector<EnglishWord> words = lexicon->lookup(input, 1, 0, true);
    if (words.empty() || !words[0].exact) return;

    // Valid pinyin (the first candidate consumes the whole input): the word goes after the
    // leading (Laya-ranked) Chinese candidate. Otherwise (e.g. "hello", where the pinyin
    // candidates only cover a prefix) the word leads the page.
    bool valid_pinyin = false;
    if (!result.entries.empty()) {
        if (const auto* head = std::get_if<TextSelectionAction>(&result.entries[0].selection))
            valid_pinyin = head->consumed_input_bytes == input.size();
    }
    Candidate c;
    c.text = words[0].text;
    c.source = CandidateSource::kEnglish;
    c.code = input;
    c.input_code = input;
    c.frequency = words[0].score;
    c.source_frequency = words[0].score;
    const size_t pos = valid_pinyin
                           ? (std::min)(result.entries.size(), static_cast<size_t>((std::max)(0, ec.position_in_pinyin)))
                           : 0;
    result.entries.insert(result.entries.begin() + static_cast<std::ptrdiff_t>(pos),
                          make_text_candidate_entry(std::move(c), input.size(), input));
    // Keep the requested page size; Chinese candidates pushed out reappear on the next page
    // (paging re-queries page 0 and continues after the last visible candidate).
    if (page_size > 0 && static_cast<int>(result.entries.size()) > page_size)
        result.entries.resize(static_cast<size_t>(page_size));
    if (result.highlighted < 0 && !result.entries.empty()) result.highlighted = 0;
}

namespace {

CandidateEntry make_english_entry(const std::string& text, const std::string& code, int score) {
    Candidate c;
    c.text = text;
    c.source = CandidateSource::kEnglish;
    c.code = code;
    c.input_code = code;
    c.frequency = score;
    c.source_frequency = score;
    return make_text_candidate_entry(std::move(c), code.size(), code);
}

char english_letter(const KeyEvent& event) {
    const char ch = static_cast<char>(event.keycode);  // 'A'..'Z'
    const bool upper = event.is_shift() != event.is_caps_lock();
    return upper ? ch : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

}  // namespace

void Engine::refresh_english_candidates() {
    const std::string typed = context_.composition().active().input;
    const auto& ec = runtime_->config().english;
    const int page_size = (std::max)(1, runtime_->config().page_size);
    TranslationResult result;
    result.page_size = page_size;
    // The typed text always comes first, so Space/1 commit exactly what was typed when no
    // dictionary word matches.
    result.entries.push_back(make_english_entry(typed, typed, 0));
    english_corrections_shown_.clear();
    if (auto lexicon = EnglishLexicon::shared()) {
        // Dictionary order (exact match first, then by frequency; corrections and learned words
        // merged in) in the typed case, as the model was trained; Laya then picks the word for
        // the recommended slot by context.
        const std::shared_ptr<EnglishLearning> learning =
            runtime_->config().candidate_learning ? EnglishLearning::shared() : nullptr;
        EnglishPool pool = build_english_pool(*lexicon, learning.get(), typed, ec);
        std::vector<EnglishWord>& words = pool.words;
        // The model was trained on completions of the typed letters and favours words sharing
        // its beginning (thier -> this), so it does not choose among corrections: they keep
        // their own order (frequency and typing cost) and only the best one competes with the
        // completions. The others follow it.
        std::vector<EnglishWord> later_corrections;
        bool best_correction = false;
        for (auto it = words.begin(); it != words.end();) {
            if (it->corrected && best_correction) {
                later_corrections.push_back(std::move(*it));
                it = words.erase(it);
            } else {
                best_correction = best_correction || it->corrected;
                ++it;
            }
        }
        bool recommended = LayaRerank::instance().apply_english(
            runtime_->config(), laya_context(context_.composition()), typed, words);
        if (!recommended && !words.empty() && words.front().corrected)
            recommended = true;  // only corrections: the best one is the recommendation
        if (!later_corrections.empty()) {
            auto best = std::find_if(words.begin(), words.end(),
                                     [](const EnglishWord& w) { return w.corrected; });
            if (best != words.end()) ++best;
            words.insert(best, std::make_move_iterator(later_corrections.begin()),
                         std::make_move_iterator(later_corrections.end()));
        }
        if (!pool.learned_pick.empty()) {
            // A correction picked again and again takes the recommended slot.
            auto pick = std::find_if(words.begin(), words.end(),
                                     [&](const EnglishWord& w) { return w.text == pool.learned_pick; });
            if (pick != words.end()) {
                std::rotate(words.begin(), pick, pick + 1);
                recommended = true;
            }
        }
        if (recommended && !words.empty() && words.front().text == typed)
            result.entries.front().candidate.recommended = true;  // the pick is what was typed
        int shown = 0;
        int corrections = 0;
        for (size_t i = 0; i < words.size(); ++i) {
            const auto& w = words[i];
            if (static_cast<int>(result.entries.size()) >= page_size || shown >= ec.completion_count) break;
            if (w.text == typed) continue;  // already first
            if (w.corrected && corrections >= ec.correction_count) continue;
            result.entries.push_back(make_english_entry(w.text, typed, w.score));
            result.entries.back().candidate.recommended = recommended && i == 0;
            if (w.corrected) {
                ++corrections;
                english_corrections_shown_.push_back(w.text);
            }
            ++shown;
        }
    }
    const int n = static_cast<int>(result.entries.size());
    result.extent = make_candidate_extent(n, n, false);
    result.highlighted = 0;
    context_.update_translation(std::move(result));
}

void Engine::learn_english_commit(const std::string& typed, const std::string& committed) {
    if (!runtime_ || !runtime_->config().candidate_learning) return;
    const std::shared_ptr<EnglishLearning> learning = EnglishLearning::shared();
    const std::shared_ptr<const EnglishLexicon> lexicon = EnglishLexicon::shared();
    if (!learning || !lexicon) return;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    if (lower(committed) == lower(typed)) {
        // The typed text as it is (or a learned word in its own case, e.g. useState).
        learning->record_kept(committed,
                              english_common_word(*lexicon, committed, runtime_->config().english));
        return;
    }
    if (std::find(english_corrections_shown_.begin(), english_corrections_shown_.end(), committed) ==
        english_corrections_shown_.end()) {
        return;  // an ordinary completion
    }
    // Learn the dictionary form of the correction (the shown text is in the typed case).
    for (const auto& w : lexicon->lookup(committed, 4, 0, true)) {
        if (w.exact && lower(w.text) == lower(committed)) {
            learning->record_correction(typed, w.text);
            return;
        }
    }
}

ProcessResult Engine::commit_english(std::string text) {
    reset_composition_state();
    english_composing_ = false;
    context_.committed_text = std::move(text);
    context_.set_commit_source(CommitSource::kRawCodePretransformed);
    return ProcessResult::COMMITTED;
}

std::optional<ProcessResult> Engine::process_english_key(const KeyEvent& event, const OutputOptions& opts) {
    if (event.is_key_up || !runtime_) return std::nullopt;  // uninitialized engines (tests) skip this
    if (!context_.is_composing()) english_composing_ = false;
    const uint32_t vk = event.keycode;
    const bool letter = vk >= 'A' && vk <= 'Z';

    if (!english_composing_) {
        // Start a word only from idle, persistent (not temporary) English mode.
        // CapsLock keeps its literal meaning (uppercase letters committed directly).
        if (!letter || !english_word_mode_ || !runtime_->config().english.completion_in_ascii || event.is_caps_lock() ||
            !ascii_composer_.is_ascii_mode() || ascii_composer_.is_temporary_ascii() ||
            context_.is_composing() || opts.full_shape || !EnglishLexicon::shared()) {
            return std::nullopt;
        }
        if (!context_.start_composition(CompositionScheme::kInlineAscii,
                                        std::string(1, english_letter(event)), 1)) {
            return std::nullopt;
        }
        english_composing_ = true;
        refresh_english_candidates();
        return ProcessResult::ACCEPTED;
    }

    const std::string typed = context_.composition().active().input;
    if (letter) {
        context_.insert_preedit(english_letter(event));
        refresh_english_candidates();
        return ProcessResult::ACCEPTED;
    }
    switch (vk) {
    case VK_BACK:
    case VK_DELETE:
        if (vk == VK_BACK) {
            context_.erase_preedit_before_cursor();
        } else {
            context_.erase_preedit_at_cursor();
        }
        if (context_.composition().active().input.empty()) {
            reset_composition_state();
            english_composing_ = false;
        } else {
            refresh_english_candidates();
        }
        return ProcessResult::ACCEPTED;
    case VK_ESCAPE:
        reset_composition_state();
        english_composing_ = false;
        return ProcessResult::ACCEPTED;
    case VK_RETURN:
        learn_english_commit(typed, typed);
        return commit_english(typed);
    case VK_SPACE: {
        const int h = (std::max)(0, context_.translation().highlighted);
        const CandidateEntry* entry = context_.candidate_entry(h);
        const std::string word = entry ? entry->candidate.text : typed;
        learn_english_commit(typed, word);
        return commit_english(word + " ");
    }
    case VK_TAB:
        if (event.is_shift()) {
            context_.move_to_previous_candidate();
        } else {
            context_.move_to_next_candidate();
        }
        return ProcessResult::ACCEPTED;
    case VK_DOWN:
        context_.move_to_next_candidate();
        return ProcessResult::ACCEPTED;
    case VK_UP:
        context_.move_to_previous_candidate();
        return ProcessResult::ACCEPTED;
    case VK_LEFT:
        context_.move_preedit_cursor_left();
        return ProcessResult::ACCEPTED;
    case VK_RIGHT:
        context_.move_preedit_cursor_right();
        return ProcessResult::ACCEPTED;
    case VK_HOME:
        context_.move_preedit_cursor_home();
        return ProcessResult::ACCEPTED;
    case VK_END:
        context_.move_preedit_cursor_end();
        return ProcessResult::ACCEPTED;
    default:
        break;
    }
    // 1-9 on the main keyboard select a candidate (as in Chinese mode). To type a digit after a
    // word, commit the word first with Space.
    if (vk >= '1' && vk <= '9' && !event.is_shift()) {
        const CandidateEntry* entry = context_.candidate_entry(static_cast<int>(vk - '1'));
        if (entry) {
            learn_english_commit(typed, entry->candidate.text);
            return commit_english(entry->candidate.text);
        }
        return ProcessResult::ACCEPTED;
    }
    // Any other printable key (punctuation, 0, numpad, shifted digits): commit the typed text
    // and the character, so ordinary English typing is never blocked.
    if (auto ch = normalize_ascii_key(event)) {
        learn_english_commit(typed, typed);
        return commit_english(typed + *ch);
    }
    return ProcessResult::ACCEPTED;  // other non-text keys are ignored while a word is open
}

TranslationResult Engine::translate_after_visible_anchor(const QueryDeadline& deadline,
                                                         int visible_count, int target_offset,
                                                         uint32_t continuation_effort,
                                                         bool* sequence_invalidated) {
    if (sequence_invalidated) {
        *sequence_invalidated = false;
    }
    TranslationResult unavailable;
    unavailable.status = TranslationStatus::kFailed;
    unavailable.extent.state = CandidateExtentState::kIndeterminate;
    unavailable.extent.complete = false;
    const CandidateEntry* anchor = context_.candidate_entry(visible_count - 1);
    if (!anchor) {
        return unavailable;
    }

    const int prefix_limit = target_offset + runtime_->config().page_size + 1;
    TranslationResult prefix = translate_composition(context_.composition(), 0, 0, deadline,
                                                     prefix_limit, continuation_effort);
    if (!prefix.usable()) {
        return unavailable;
    }
    const auto anchor_position = std::find_if(
        prefix.entries.begin(), prefix.entries.end(),
        [&](const CandidateEntry& entry) { return same_candidate_entry_identity(entry, *anchor); });
    if (anchor_position == prefix.entries.end()) {
        if (!prefix.extent.complete) {
            return unavailable;
        }
        if (sequence_invalidated) {
            *sequence_invalidated = true;
        }
        prefix.page_index = 0;
        prefix.page_offset = 0;
        prefix.page_size = runtime_->config().page_size;
        if (static_cast<int>(prefix.entries.size()) > runtime_->config().page_size) {
            prefix.entries.resize(runtime_->config().page_size);
        }
        const bool prefix_incomplete = !prefix.extent.complete;
        prefix.extent = make_candidate_extent(
            prefix.extent.known_count, static_cast<int>(prefix.entries.size()), prefix_incomplete);
        prefix.highlighted = prefix.entries.empty() ? -1 : 0;
        return prefix;
    }

    const int anchor_end = static_cast<int>(anchor_position - prefix.entries.begin()) + 1;
    std::vector<CandidateEntry> remaining;
    remaining.reserve(prefix.entries.size() - static_cast<std::size_t>(anchor_end));
    for (auto entry = prefix.entries.begin() + anchor_end; entry != prefix.entries.end(); ++entry) {
        if (!context_.candidate_was_published(*entry, visible_count)) {
            remaining.push_back(std::move(*entry));
        }
    }
    const int available_after_anchor = static_cast<int>(remaining.size());
    const int next_count = (std::min)(runtime_->config().page_size, available_after_anchor);
    TranslationResult next;
    next.status = prefix.status;
    next.page_index = context_.page_index() + 1;
    next.page_offset = target_offset;
    next.page_size = runtime_->config().page_size;
    next.entries.assign(std::make_move_iterator(remaining.begin()),
                        std::make_move_iterator(remaining.begin() + next_count));
    if (!next.entries.empty()) {
        next.highlighted = 0;
    }
    const int unmaterialized_known =
        (std::max)(0, prefix.extent.known_count - static_cast<int>(prefix.entries.size()));
    const int known_count = target_offset + available_after_anchor + unmaterialized_known;
    const bool prefix_incomplete = !prefix.extent.complete;
    next.extent = make_candidate_extent(known_count, target_offset + next_count, prefix_incomplete);
    if (next.extent.state == CandidateExtentState::kIndeterminate &&
        next.status == TranslationStatus::kSuccess) {
        next.status = TranslationStatus::kStableDegraded;
    }
    return next;
}

bool Engine::finalize_selection(const CandidateEntry& entry) {
    const auto* action = std::get_if<TextSelectionAction>(&entry.selection);
    if (!action || action->consumed_input_bytes != context_.active_input().size()) {
        return false;
    }
    if (!context_.commit_entry(entry)) {
        return false;
    }
    return true;
}

void Engine::apply_commit_learning_plan() {
    CommitLearningPlan plan = context_.take_commit_learning_plan();
    if (!runtime_ || !runtime_->config().candidate_learning || plan.empty()) {
        return;
    }
    last_learning_ = {};
    for (const CandidatePreferenceLearningEvent& event : plan.candidate_preferences) {
        Dict* dictionary = nullptr;
        if (event.target == LearningTarget::kPinyin) {
            dictionary = &runtime_->pinyin_dict();
        } else if (event.target == LearningTarget::kWubi) {
            dictionary = runtime_->wubi_dict();
        }
        CandidatePreferenceReceipt receipt;
        if (dictionary && event.candidate.source != CandidateSource::kSymbol &&
            dictionary->record_candidate_preference(event.candidate, event.typed_code,
                                                    &receipt)) {
            last_learning_.receipts.emplace_back(dictionary, std::move(receipt));
        }
    }
    if (plan.composition && runtime_->composition_learning() &&
        runtime_->composition_learning()->enqueue(*plan.composition)) {
        last_learning_.composition = plan.composition;
    }
    last_learning_.armed =
        !last_learning_.receipts.empty() || last_learning_.composition.has_value();
}

void Engine::revoke_last_commit_learning() {
    CommitLearningSession session = std::move(last_learning_);
    last_learning_ = {};
    for (const auto& [dictionary, receipt] : session.receipts) {
        dictionary->revoke_candidate_preference(receipt);
    }
    if (session.composition && runtime_ && runtime_->composition_learning()) {
        runtime_->composition_learning()->revoke(*session.composition);
    }
}

bool Engine::replace_active_input(const ReplaceActiveInputAction& action,
                                  const QueryDeadline& deadline) {
    CompositionState next = context_.composition();
    if (!next.replace_active_input(action)) {
        return false;
    }
    TranslationResult translated = translate_composition(next, 0, 0, deadline);
    if (!translated.usable() || !derive_composition_presentation(next).fits_transport()) {
        return false;
    }
    context_.replace_composition(std::move(next), std::move(translated));
    return true;
}

bool Engine::dispatch_candidate_selection(int index, const QueryDeadline& deadline) {
    const CandidateEntry* current = context_.candidate_entry(index);
    if (!current) {
        return false;
    }
    CandidatePick pick;
    pick.index = index;
    pick.recommended = current->candidate.recommended;
    for (int i = 0; i < context_.candidate_count() && !pick.had_recommendation; ++i) {
        const CandidateEntry* entry = context_.candidate_entry(i);
        pick.had_recommendation = entry && entry->candidate.recommended;
    }
    last_pick_ = pick;
    CandidateEntry entry = *current;
    if (const auto* replacement = std::get_if<ReplaceActiveInputAction>(&entry.selection)) {
        return replace_active_input(*replacement, deadline);
    }
    const auto& action = std::get<TextSelectionAction>(entry.selection);
    if (action.consumed_input_bytes == context_.active_input().size()) {
        return finalize_selection(entry);
    }

    CompositionState next = context_.composition();
    if (!next.confirm_prefix(action)) {
        return false;
    }
    TranslationResult translated = translate_composition(next, 0, 0, deadline);
    const CompositionPresentation presentation = derive_composition_presentation(next);
    if (!translated.usable() || !presentation.fits_transport()) {
        return false;
    }
    context_.replace_composition(std::move(next), std::move(translated));
    return true;
}

CompositionScheme Engine::scheme_for_mode(InputMode mode) {
    return mode == InputMode::WUBI ? CompositionScheme::kWubi : CompositionScheme::kPinyin;
}

void Engine::set_sentence_composition_enabled(bool enabled) {
    sentence_composition_enabled_ = enabled;
    if (translator_) {
        translator_->set_sentence_composition_enabled(enabled);
    }
}

void Engine::clear_query_cache() {
    if (translator_) {
        translator_->clear_query_cache();
    }
}

void Engine::switch_mode(InputMode mode) {
    rebuild_pipeline(mode);
}

void Engine::set_pinyin_initials(bool enabled) {
    if (pinyin_initials_ == enabled) {
        return;
    }
    pinyin_initials_ = enabled;
    if (mode_ == InputMode::PINYIN) {
        rebuild_pipeline(mode_, true);
    }
}

void Engine::rebuild_pipeline(InputMode mode, bool force) {
    if (!runtime_) {
        return;
    }
    Dict& pinyin_dict = runtime_->pinyin_dict();
    Dict* wubi_dict = runtime_->wubi_dict();
    const Config& config = runtime_->config();

    // Chinese input is pinyin or Wubi (mixed input was removed). Fall back to pinyin when the
    // optional Wubi dictionary is unavailable.
    if (mode != InputMode::WUBI || !wubi_dict)
        mode = InputMode::PINYIN;

    if (!force && mode == mode_) return;

    reset_composition_state();

    mode_ = mode;
    context_.set_ime_scheme(scheme_for_mode(mode_));
    if (mode == InputMode::WUBI) {
        processor_ = std::make_unique<WubiProcessor>();
        auto wubi_trans = std::make_unique<WubiTranslator>();
        wubi_trans->set_dict(wubi_dict);
        translator_ = std::move(wubi_trans);
    } else {
        processor_ = std::make_unique<PinyinProcessor>();
        auto pinyin_trans = std::make_unique<PinyinTranslator>();
        pinyin_trans->set_dict(&pinyin_dict);
        PinyinQueryPolicy policy = runtime_->pinyin_query_policy();
        policy.initials_only = pinyin_initials_;
        pinyin_trans->bind_pinyin(runtime_->pinyin_resources_ptr(), policy);
        if (pinyin_dict.has_short_cache()) {
            pinyin_trans->set_short_cache(&pinyin_dict.short_cache());
        }
        translator_ = std::move(pinyin_trans);
    }
    translator_->set_sentence_composition_enabled(sentence_composition_enabled_);
    translator_->set_candidate_learning_enabled(config.candidate_learning);
    translator_->set_composition_learning_service(runtime_->composition_learning());
}

std::string Engine::derive_spellings_path(const std::string& dict_path) {
    // pinyin.dict.bin -> pinyin.spellings.bin
    // pinyin.dict.db  -> pinyin.spellings.bin
    static const char kDictBinExt[] = ".dict.bin";
    static const char kDictDbExt[] = ".dict.db";
    static const char kSpellingsExt[] = ".spellings.bin";

    std::string path = dict_path;
    auto replace_ext = [&](const char* from, const char* to) {
        size_t pos = path.rfind(from);
        if (pos != std::string::npos && pos + strlen(from) == path.size()) {
            path.replace(pos, strlen(from), to);
            return true;
        }
        return false;
    };

    if (replace_ext(kDictBinExt, kSpellingsExt))
        return path;
    if (replace_ext(kDictDbExt, kSpellingsExt))
        return path;

    // Unknown extension: append .spellings.bin.
    return path + kSpellingsExt;
}

} // namespace cxxime
