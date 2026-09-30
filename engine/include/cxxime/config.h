// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_CONFIG_H_
#define CXXIME_CONFIG_H_

#include <string>
#include <unordered_map>
#include <vector>

#include <cxxime/diagnostics_config.h>
#include <cxxime/keyboard_shortcut.h>

namespace cxxime {

enum class MixedCandidatePreference {
    kAuto,
    kWubi,
};

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
    int page_size = 9;
    int input_mode = 0;  // 0=pinyin, 1=wubi, 2=mixed
    std::string pinyin_scheme = "full_pinyin";
    bool fuzzy_pinyin = true;  // 模糊拼音开关
    bool wubi_auto_commit = true;  // Auto-commit the only candidate at four codes.
    bool wubi_commit_first_on_fifth_key = true;  // Commit the first choice before code 5.
    bool wubi_restart_on_fifth_after_miss = true;
    bool wubi_code_hint = false;  // Show the shortest remaining Wubi code in candidates.
    bool candidate_learning = false;
    MixedCandidatePreference mixed_candidate_preference = MixedCandidatePreference::kAuto;

    // Initial state for each newly created input session.
    bool initial_full_shape = false;
    bool initial_chinese_punct = true;

    // style
    std::string font_name = "Microsoft YaHei UI";
    int font_size = 14;
    std::string layout = "horizontal";  // horizontal | vertical
    std::string render_backend = "d2d";  // gdi | d2d
    bool inline_preedit = false;
    std::string preedit_type = "composition";

    // theme
    std::string theme = "azure";

    // layout (spacing and sizing)
    LayoutConfig layout_config;

    // ascii_composer
    std::unordered_map<std::string, std::string> ascii_switch_key;

    // shortcuts
    KeyboardShortcut input_mode_switch_shortcut;
    KeyboardShortcut activate_ime_shortcut;

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

} // namespace cxxime

#endif // CXXIME_CONFIG_H_
