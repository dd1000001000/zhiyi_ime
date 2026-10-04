// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_CONFIG_H_
#define CXXIME_CONFIG_H_

#include <string>
#include <unordered_map>
#include <vector>

#include <cxxime/diagnostics_config.h>
#include <cxxime/keyboard_shortcut.h>

namespace cxxime {

struct LayoutConfig {
    int min_width = 160;
    int max_width = 0;
    int max_height = 0;
    int margin_x = 12;
    int margin_y = 12;
    int spacing = 10;          // preedit to candidates gap
    int candidate_spacing = 8; // between candidate cells
    int hilite_spacing = 4;    // inner gap: label↔text
    int hilite_padding_x = 4;  // highlight rect horizontal padding (InflateRect)
    int hilite_padding_y = 2;  // highlight rect vertical padding (InflateRect)
    int round_corner = 4;      // highlight rect corner radius
    int round_corner_ex = 4;   // window corner radius
    int border_width = 1;      // window border
    int label_font_point = 0;  // preedit font size, 0 = derive from font_point
    int preedit_highlight_padding_x = 4;    // focused input horizontal padding
    int preedit_highlight_padding_y = 2;    // focused input vertical padding
    int preedit_confirmed_gap = 6;          // confirmed text to active input
    int preedit_boundary_gap = 1;           // focused input to following syllable mark
    int preedit_highlight_corner = 3;       // focused input corner radius
    int preedit_highlight_border_width = 1; // focused input border
};

struct Config {
    bool load(const std::string& path);
    bool load_user(const std::string& path);
    bool load_json(const std::string& json_text);
    bool load_user_json(const std::string& json_text);
    bool load_runtime_json(const std::string& json_text);
    bool load_themes(const std::string& path);  // load themes.json separately
    std::string to_user_json() const;
    std::string to_runtime_json() const;

    // engine
    // Candidates per page (settings: 3-10). Laya compares the first 2 * page_size candidates.
    int page_size = 7;
    int input_mode = 0;  // Chinese input: 0=pinyin, 1=wubi (chosen in settings)
    std::string pinyin_scheme = "full_pinyin";  // full pinyin is the only scheme
    bool wubi_auto_commit = true;  // Auto-commit the only candidate at four codes.
    bool wubi_commit_first_on_fifth_key = true;  // Commit the first choice before code 5.
    bool wubi_restart_on_fifth_after_miss = true;
    bool wubi_code_hint = false;  // Show the shortest remaining Wubi code in candidates.
    bool candidate_learning = true;   // self-learning: picked candidates move up
    // User experience improvement program (privacy.experience_program, opt-in): the server
    // keeps a local log of how the IME runs (version, settings, speed, errors, which position
    // was picked), never anything typed. See docs/privacy.md.
    bool experience_program = false;
    bool pinyin_initials = false;     // pinyin style: full pinyin (false) or initials (true)
    // Fuzzy pinyin: master switch and the enabled pairs (FuzzyGroup bits, spellings_index.h).
    bool fuzzy_pinyin = false;
    uint8_t fuzzy_groups = 0x7F;

    // Initial state for each newly created input session.
    bool initial_full_shape = false;
    bool initial_chinese_punct = true;

    // style
    std::string font_name = "Microsoft YaHei UI";
    int font_size = 14;
    std::string layout = "horizontal";  // horizontal | vertical
    std::string render_backend = "d2d";  // gdi | d2d
    // What is typed is shown in the document, underlined (pinyin with syllable boundaries,
    // the English word being typed). Always on: style.inline_preedit is no longer read, because
    // the popup-only mode left a placeholder character in the document and older user configs
    // still hold false.
    bool inline_preedit = true;
    std::string preedit_type = "composition";

    // theme
    std::string theme = "moon_light";  // "moon_light" (light) or "moon_dark" (dark)
    // Settings UI language: "auto" (follow Windows) or a data/ui.<code>.json code.
    std::string ui_language = "auto";

    // layout (spacing and sizing)
    LayoutConfig layout_config;

    // ascii_composer
    std::unordered_map<std::string, std::string> ascii_switch_key;

    // shortcuts
    KeyboardShortcut activate_ime_shortcut;
    // Switches English mode between word completion and letter-by-letter input.
    KeyboardShortcut english_style_shortcut = {kKeyModifierControl, 0x20 /* VK_SPACE */};
    // Chinese/English switch by a key combination (shortcuts.ascii_toggle, e.g. Ctrl+Shift+E),
    // instead of or next to tapping a modifier alone (ascii_composer.switch_key).
    KeyboardShortcut ascii_toggle_shortcut;
    // Chinese/English punctuation (shortcuts.punct_toggle) and full/half width
    // (shortcuts.shape_toggle).
    KeyboardShortcut punct_toggle_shortcut = {kKeyModifierControl, 0xBE /* VK_OEM_PERIOD */};
    KeyboardShortcut shape_toggle_shortcut = {kKeyModifierShift, 0x20 /* VK_SPACE */};

    // status_window
    struct StatusWindowConfig {
        bool enable = true;
        int x = -1;
        int y = -1;
        bool show_on_startup = true;
    };
    StatusWindowConfig status_window;

    // Users control only trace_mode; rotation and thresholds remain package settings.
    DiagnosticsConfig diagnostics;

    // Laya context reranking of the first pinyin page (server side, CPU ONNX model).
    struct LayaConfig {
        bool enable = true;
        std::string model_dir = "laya";        // relative to the server executable's directory
        std::string onnx = "laya.int8g.onnx";
        int min_candidates = 2;   // fewer comparable candidates: keep the translator's order
        int context_chars = 48;   // trailing committed characters fed to the model
        int threads = 4;          // ONNX Runtime intra-op threads
        // English word mode (see EnglishConfig): completions are reranked too.
        bool english = true;
        int english_context_chars = 96;    // English needs more characters for the same context
        double english_freq_weight = 0.0;  // weight of log P_freq next to log P_laya
        // Spelling corrections: log10 P penalty per unit of typing cost (1 = a wrong letter
        // costs a factor of 10), so the model does not prefer a correction over a completion
        // of what was typed without a reason.
        double english_correction_weight = 1.0;
    };
    LayaConfig laya;

    // English words (data/english.words.tsv).
    struct EnglishConfig {
        // Chinese pinyin mode: when the typed letters are exactly a word of the list, that word
        // is added to the first page (no completions).
        bool mixed_in_chinese = true;
        int min_input = 3;              // letters typed before English words are looked up
        int position_in_pinyin = 1;     // 0-based slot when the input is valid pinyin (2nd);
                                        // input that is not complete pinyin puts the word first
        // English (ASCII) mode: word completion while typing letters ("word" style) or plain
        // letter-by-letter input. word_mode is the current style (switched by
        // shortcuts.english_style and persisted).
        bool completion_in_ascii = true;
        bool word_mode = true;
        int completion_count = 6;       // completions shown after the typed text
        int completion_pool = 8;        // dictionary words reranked before choosing them
        int min_score = 300;            // 100 * Zipf; hides rare words (Zipf < 3.0)
        // Spelling correction (teh -> the): candidates only, the typed text stays first.
        bool correction = true;
        int correction_count = 2;       // corrections among the shown words at most
    };
    EnglishConfig english;

    // Color scheme loaded from themes.json.
    // Fields default to -1 = "not set" (resolved to Weasel-style fallbacks in load_themes).
    struct SchemeColors {
        std::string name;
        int text_color = -1;
        int back_color = -1;
        int border_color = -1;
        int candidate_text_color = -1;
        int label_text_color = -1;
        int hilited_text_color = -1;
        int hilited_back_color = -1;
        int hilited_candidate_text_color = -1;
        int hilited_candidate_back_color = -1;
        int preedit_cursor_color = -1;
        int comment_text_color = -1;
        int prevpage_color = -1;
        int nextpage_color = -1;
        int preedit_active_back_color = -1;
        int preedit_active_border_color = -1;
    };
    std::unordered_map<std::string, SchemeColors> preset_color_schemes;
    std::vector<std::string> preset_color_scheme_order;
};

// The switch keys: Chinese/English (a combination, or Shift / Ctrl tapped alone), style,
// punctuation and full/half width. Valid when every combination is a valid switch key and none
// repeats another or the IME activation shortcut.
bool switch_keys_valid(const Config& config);
// Defaults: tap Shift, Ctrl+Space, Ctrl+., Shift+Space (one equal to the IME activation shortcut
// is left unset).
void reset_switch_keys(Config& config);

} // namespace cxxime

#endif // CXXIME_CONFIG_H_
