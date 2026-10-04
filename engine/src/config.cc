// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/config.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <utility>

#include <json.hpp>

#include <cxxime/input_limits.h>
#include <cxxime/logging.h>
#include <cxxime/pinyin_scheme.h>

namespace cxxime {

template <typename Json>
static void load_int(Json& obj, const char* key, int& val) {
    if (obj.contains(key) && obj[key].is_number()) val = obj[key].get<int>();
}

template <typename Json>
static void load_string(Json& obj, const char* key, std::string& val) {
    if (obj.contains(key) && obj[key].is_string()) val = obj[key].get<std::string>();
}

template <typename Json>
static void load_bool(Json& obj, const char* key, bool& val) {
    if (obj.contains(key) && obj[key].is_boolean()) val = obj[key].get<bool>();
}

template <typename Json>
static void load_keyboard_shortcut(Json& obj, const char* key, KeyboardShortcut& value,
                                   bool (*validator)(const KeyboardShortcut&)) {
    if (!obj.contains(key) || !obj[key].is_string()) {
        return;
    }
    KeyboardShortcut parsed;
    if (parse_keyboard_shortcut(obj[key].template get<std::string>(), &parsed) &&
        validator(parsed)) {
        value = parsed;
    } else {
        value = {};
    }
}

// Fuzzy pinyin pairs in the config file ("fuzzy_groups": ["z_zh", ...]).
static const std::pair<uint8_t, const char*> kFuzzyGroupNames[] = {
    {0x01, "z_zh"}, {0x02, "c_ch"}, {0x04, "s_sh"}, {0x08, "n_l"},
    {0x10, "an_ang"}, {0x20, "en_eng"}, {0x40, "in_ing"},
};

static uint8_t fuzzy_group_bit(const std::string& name) {
    for (const auto& [bit, group_name] : kFuzzyGroupNames) {
        if (name == group_name) {
            return bit;
        }
    }
    return 0;
}

static int muted_text_color(int foreground, int background) {
    constexpr int foreground_weight = 3;
    constexpr int background_weight = 2;
    constexpr int weight_sum = foreground_weight + background_weight;
    auto blend_channel = [&](int shift) {
        int foreground_channel = (foreground >> shift) & 0xff;
        int background_channel = (background >> shift) & 0xff;
        return (foreground_channel * foreground_weight + background_channel * background_weight) /
                weight_sum;
    };
    return (foreground & 0xff000000) | blend_channel(0) | (blend_channel(8) << 8) |
            (blend_channel(16) << 16);
}

static double linearized_color_channel(int channel) {
    const double value = channel / 255.0;
    if (value <= 0.04045) {
        return value / 12.92;
    }
    return std::pow((value + 0.055) / 1.055, 2.4);
}

static double relative_luminance(int color) {
    const int red = color & 0xff;
    const int green = (color >> 8) & 0xff;
    const int blue = (color >> 16) & 0xff;
    return 0.2126 * linearized_color_channel(red) + 0.7152 * linearized_color_channel(green) +
           0.0722 * linearized_color_channel(blue);
}

static double contrast_ratio(int left, int right) {
    const double left_luminance = relative_luminance(left);
    const double right_luminance = relative_luminance(right);
    const double lighter = (std::max)(left_luminance, right_luminance);
    const double darker = (std::min)(left_luminance, right_luminance);
    return (lighter + 0.05) / (darker + 0.05);
}

static int color_distance(int left, int right) {
    const int red = std::abs((left & 0xff) - (right & 0xff));
    const int green = std::abs(((left >> 8) & 0xff) - ((right >> 8) & 0xff));
    const int blue = std::abs(((left >> 16) & 0xff) - ((right >> 16) & 0xff));
    return red + green + blue;
}

static int blend_color(int source, int target, double amount) {
    auto blend_channel = [&](int shift) {
        const int source_channel = (source >> shift) & 0xff;
        const int target_channel = (target >> shift) & 0xff;
        return static_cast<int>(
            std::round(source_channel * (1.0 - amount) + target_channel * amount));
    };
    return blend_channel(0) | (blend_channel(8) << 8) | (blend_channel(16) << 16);
}

static int default_preedit_cursor_color(const Config::SchemeColors& colors) {
    constexpr double minimum_background_contrast = 4.5;
    constexpr int minimum_text_distance = 96;
    constexpr double blend_amounts[] = {0.0, 0.12, 0.24, 0.36, 0.48, 0.60, 0.72, 0.84};
    const int samples[] = {
        colors.hilited_candidate_back_color,
        colors.hilited_back_color,
        colors.border_color,
        colors.label_text_color,
        colors.candidate_text_color,
        colors.comment_text_color,
        colors.text_color,
        colors.hilited_candidate_text_color,
    };
    const int adjustment_target =
        contrast_ratio(0x000000, colors.back_color) > contrast_ratio(0xffffff, colors.back_color)
        ? 0x000000
        : 0xffffff;
    for (int sample : samples) {
        for (double amount : blend_amounts) {
            const int candidate = blend_color(sample, adjustment_target, amount);
            if (contrast_ratio(candidate, colors.back_color) >= minimum_background_contrast &&
                color_distance(candidate, colors.hilited_text_color) >= minimum_text_distance) {
                return candidate;
            }
        }
    }

    constexpr int cursor_on_light = 0x00c06700;           // RGB #0067c0
    constexpr int cursor_on_light_alternate = 0x002c26a4; // RGB #a4262c
    constexpr int cursor_on_dark = 0x003fd2ff;            // RGB #ffd23f
    constexpr int cursor_on_dark_alternate = 0x00ffc24c;  // RGB #4cc2ff

    const bool light_background = relative_luminance(colors.back_color) >= 0.5;
    const int preferred = light_background ? cursor_on_light : cursor_on_dark;
    const int alternate = light_background ? cursor_on_light_alternate : cursor_on_dark_alternate;
    if (color_distance(preferred, colors.hilited_text_color) >= minimum_text_distance) {
        return preferred;
    }
    return alternate;
}

static int default_preedit_active_back_color(const Config::SchemeColors& colors, bool dark) {
    constexpr double minimum_contrast = 4.5;
    double amount = dark ? 0.20 : 0.12;
    while (amount > 0.0) {
        const int candidate =
            blend_color(colors.back_color, colors.hilited_candidate_back_color, amount);
        if (contrast_ratio(candidate, colors.hilited_text_color) >= minimum_contrast) {
            return candidate;
        }
        amount -= 0.02;
    }
    return colors.back_color;
}

static void apply_config_json(Config& config, nlohmann::json& j) {
    if (j.contains("engine") && j["engine"].is_object()) {
        auto& e = j["engine"];
        load_int(e, "page_size", config.page_size);
        if (config.page_size < 1) config.page_size = 1;
        if (config.page_size > 100) config.page_size = 100;
        load_int(e, "input_mode", config.input_mode);
        if (config.input_mode < 0) config.input_mode = 0;
        if (config.input_mode > 1) config.input_mode = 0;  // former mixed mode: pinyin
        load_string(e, "pinyin_scheme", config.pinyin_scheme);
        if (!find_pinyin_scheme(config.pinyin_scheme)) {
            CXXIME_LOG(L"Config: unknown pinyin scheme '%S', falling back to full Pinyin",
                       config.pinyin_scheme.c_str());
        }
        config.pinyin_scheme = normalize_pinyin_scheme_id(config.pinyin_scheme);
        load_bool(e, "wubi_auto_commit", config.wubi_auto_commit);
        load_bool(e, "wubi_commit_first_on_fifth_key",
                  config.wubi_commit_first_on_fifth_key);
        load_bool(e, "wubi_restart_on_fifth_after_miss",
                  config.wubi_restart_on_fifth_after_miss);
        load_bool(e, "wubi_code_hint", config.wubi_code_hint);
        load_bool(e, "candidate_learning", config.candidate_learning);
        load_bool(e, "pinyin_initials", config.pinyin_initials);
        load_bool(e, "fuzzy_pinyin", config.fuzzy_pinyin);
        if (e.contains("fuzzy_groups") && e["fuzzy_groups"].is_array()) {
            config.fuzzy_groups = 0;
            for (const auto& name : e["fuzzy_groups"]) {
                if (name.is_string()) {
                    config.fuzzy_groups |= fuzzy_group_bit(name.get<std::string>());
                }
            }
        }
    }

    if (j.contains("initial_state") && j["initial_state"].is_object()) {
        auto& initial = j["initial_state"];
        load_bool(initial, "full_shape", config.initial_full_shape);
        load_bool(initial, "chinese_punct", config.initial_chinese_punct);
    }

    if (j.contains("laya") && j["laya"].is_object()) {
        auto& l = j["laya"];
        load_bool(l, "enable", config.laya.enable);
        load_string(l, "model_dir", config.laya.model_dir);
        load_string(l, "onnx", config.laya.onnx);
        load_int(l, "min_candidates", config.laya.min_candidates);
        load_int(l, "context_chars", config.laya.context_chars);
        load_int(l, "threads", config.laya.threads);
        load_bool(l, "english", config.laya.english);
        load_int(l, "english_context_chars", config.laya.english_context_chars);
        if (l.contains("english_freq_weight") && l["english_freq_weight"].is_number())
            config.laya.english_freq_weight = l["english_freq_weight"].template get<double>();
        if (l.contains("english_correction_weight") && l["english_correction_weight"].is_number())
            config.laya.english_correction_weight =
                l["english_correction_weight"].template get<double>();
    }

    if (j.contains("english") && j["english"].is_object()) {
        auto& en = j["english"];
        load_bool(en, "mixed_in_chinese", config.english.mixed_in_chinese);
        load_int(en, "min_input", config.english.min_input);
        load_int(en, "position_in_pinyin", config.english.position_in_pinyin);
        load_bool(en, "completion_in_ascii", config.english.completion_in_ascii);
        load_bool(en, "word_mode", config.english.word_mode);
        load_int(en, "completion_pool", config.english.completion_pool);
        load_int(en, "completion_count", config.english.completion_count);
        load_int(en, "min_score", config.english.min_score);
        load_bool(en, "correction", config.english.correction);
        load_int(en, "correction_count", config.english.correction_count);
    }

    if (j.contains("style") && j["style"].is_object()) {
        auto& s = j["style"];
        load_string(s, "font_face", config.font_name);
        load_int(s, "font_point", config.font_size);
        if (config.font_size < 8) config.font_size = 8;
        if (config.font_size > 72) config.font_size = 72;
        load_string(s, "layout", config.layout);
        load_string(s, "render_backend", config.render_backend);
        load_string(s, "preedit_type", config.preedit_type);
        if (config.preedit_type != "composition" && config.preedit_type != "preview")
            config.preedit_type = "composition";
    }

    if (j.contains("layout") && j["layout"].is_object()) {
        auto& l = j["layout"];
        load_int(l, "min_width", config.layout_config.min_width);
        load_int(l, "max_width", config.layout_config.max_width);
        load_int(l, "max_height", config.layout_config.max_height);
        load_int(l, "margin_x", config.layout_config.margin_x);
        load_int(l, "margin_y", config.layout_config.margin_y);
        load_int(l, "spacing", config.layout_config.spacing);
        load_int(l, "candidate_spacing", config.layout_config.candidate_spacing);
        load_int(l, "hilite_spacing", config.layout_config.hilite_spacing);
        load_int(l, "hilite_padding_x", config.layout_config.hilite_padding_x);
        load_int(l, "hilite_padding_y", config.layout_config.hilite_padding_y);
        load_int(l, "round_corner", config.layout_config.round_corner);
        load_int(l, "round_corner_ex", config.layout_config.round_corner_ex);
        load_int(l, "label_font_point", config.layout_config.label_font_point);
        load_int(l, "border_width", config.layout_config.border_width);
        load_int(l, "preedit_highlight_padding_x",
                 config.layout_config.preedit_highlight_padding_x);
        load_int(l, "preedit_highlight_padding_y",
                 config.layout_config.preedit_highlight_padding_y);
        load_int(l, "preedit_confirmed_gap", config.layout_config.preedit_confirmed_gap);
        load_int(l, "preedit_boundary_gap", config.layout_config.preedit_boundary_gap);
        load_int(l, "preedit_highlight_corner", config.layout_config.preedit_highlight_corner);
        load_int(l, "preedit_highlight_border_width",
                    config.layout_config.preedit_highlight_border_width);
    }

    load_string(j, "theme", config.theme);
    if (j.contains("ui") && j["ui"].is_object()) {
        load_string(j["ui"], "language", config.ui_language);
    }

    if (j.contains("status_window") && j["status_window"].is_object()) {
        auto& sw = j["status_window"];
        load_bool(sw, "enable", config.status_window.enable);
        load_int(sw, "x", config.status_window.x);
        load_int(sw, "y", config.status_window.y);
        load_bool(sw, "show_on_startup", config.status_window.show_on_startup);
    }

    if (j.contains("ascii_composer") && j["ascii_composer"].is_object()) {
        auto& ac = j["ascii_composer"];
        if (ac.contains("switch_key") && ac["switch_key"].is_object()) {
            for (auto& [key, val] : ac["switch_key"].items()) {
                if (val.is_string()) config.ascii_switch_key[key] = val.get<std::string>();
            }
        }
    }

    if (j.contains("shortcuts") && j["shortcuts"].is_object()) {
        auto& shortcuts = j["shortcuts"];
        load_keyboard_shortcut(shortcuts, "activate_ime", config.activate_ime_shortcut,
                               is_valid_activate_ime_shortcut);
        load_keyboard_shortcut(shortcuts, "english_style", config.english_style_shortcut,
                               is_valid_input_mode_shortcut);
        load_keyboard_shortcut(shortcuts, "ascii_toggle", config.ascii_toggle_shortcut,
                               is_valid_input_mode_shortcut);
        // The IME activation shortcut wins over identical switch shortcuts, and the
        // Chinese/English switch over an identical style switch.
        if (config.english_style_shortcut.enabled() &&
            config.english_style_shortcut == config.activate_ime_shortcut) {
            config.english_style_shortcut = {};
        }
        if (config.ascii_toggle_shortcut.enabled() &&
            config.ascii_toggle_shortcut == config.activate_ime_shortcut) {
            config.ascii_toggle_shortcut = {};
        }
        if (config.english_style_shortcut.enabled() &&
            config.english_style_shortcut == config.ascii_toggle_shortcut) {
            config.english_style_shortcut = {};
        }
    }
}

template <typename Json>
static bool apply_color_schemes(Config& config, Json& schemes) {
    if (!schemes.is_object() || schemes.empty()) return false;

    std::unordered_map<std::string, Config::SchemeColors> parsed_schemes;
    std::vector<std::string> parsed_order;

    for (auto& [name, sc] : schemes.items()) {
        if (!sc.is_object()) return false;
        Config::SchemeColors c;
        load_string(sc, "name", c.name);
        load_int(sc, "back_color", c.back_color);
        load_int(sc, "border_color", c.border_color);
        load_int(sc, "text_color", c.text_color);
        load_int(sc, "candidate_text_color", c.candidate_text_color);
        load_int(sc, "label_text_color", c.label_text_color);
        load_int(sc, "hilited_text_color", c.hilited_text_color);
        load_int(sc, "hilited_back_color", c.hilited_back_color);
        load_int(sc, "hilited_candidate_text_color", c.hilited_candidate_text_color);
        load_int(sc, "hilited_candidate_back_color", c.hilited_candidate_back_color);
        load_int(sc, "preedit_cursor_color", c.preedit_cursor_color);
        load_int(sc, "comment_text_color", c.comment_text_color);
        load_int(sc, "prevpage_color", c.prevpage_color);
        load_int(sc, "nextpage_color", c.nextpage_color);
        load_int(sc, "preedit_active_back_color", c.preedit_active_back_color);
        load_int(sc, "preedit_active_border_color", c.preedit_active_border_color);
        // Weasel-style fallback chain (resolved at load time, not render time)
        if (c.text_color == -1) c.text_color = 0xff000000; // black
        if (c.back_color == -1) c.back_color = 0xffffffff; // white
        if (c.candidate_text_color == -1) c.candidate_text_color = c.text_color;
        if (c.border_color == -1) c.border_color = c.text_color;
        if (c.hilited_text_color == -1) c.hilited_text_color = c.text_color;
        if (c.hilited_back_color == -1) c.hilited_back_color = c.back_color;
        if (c.hilited_candidate_text_color == -1)
            c.hilited_candidate_text_color = c.hilited_text_color;
        if (c.hilited_candidate_back_color == -1)
            c.hilited_candidate_back_color = c.hilited_back_color;
        if (c.label_text_color == -1) c.label_text_color = c.text_color;
        if (c.comment_text_color == -1)
            c.comment_text_color = muted_text_color(c.candidate_text_color, c.back_color);
        if (c.preedit_cursor_color == -1)
            c.preedit_cursor_color = default_preedit_cursor_color(c);
        if (c.prevpage_color == -1) c.prevpage_color = c.text_color;
        if (c.nextpage_color == -1) c.nextpage_color = c.text_color;
        const bool dark = relative_luminance(c.back_color) < 0.5;
        if (c.preedit_active_back_color == -1) {
            c.preedit_active_back_color = default_preedit_active_back_color(c, dark);
        }
        if (c.preedit_active_border_color == -1) {
            c.preedit_active_border_color = blend_color(
                c.back_color, c.hilited_candidate_back_color, dark ? 0.40 : 0.30);
        }
        parsed_schemes[name] = c;
        parsed_order.push_back(name);
    }
    config.preset_color_schemes = std::move(parsed_schemes);
    config.preset_color_scheme_order = std::move(parsed_order);
    return true;
}

bool Config::load(const std::string& path) {
    if (path.empty()) return true;

    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        nlohmann::json j = nlohmann::json::parse(file);
        apply_config_json(*this, j);
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    cxxime::load_diagnostics_config(path, &diagnostics);
    return true;
}

bool Config::load_user(const std::string& path) {
    const DiagnosticsConfig package_diagnostics = diagnostics;
    if (!load(path)) {
        return false;
    }
    const DiagnosticTraceMode user_trace_mode = diagnostics.trace_mode;
    diagnostics = package_diagnostics;
    diagnostics.trace_mode = user_trace_mode;
    return true;
}

bool Config::load_json(const std::string& json_text) {
    if (json_text.empty()) {
        return false;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(json_text);
        if (!j.is_object()) {
            return false;
        }
        apply_config_json(*this, j);
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    cxxime::load_diagnostics_config_json(json_text, &diagnostics);
    return true;
}

bool Config::load_user_json(const std::string& json_text) {
    const DiagnosticsConfig package_diagnostics = diagnostics;
    if (!load_json(json_text)) {
        return false;
    }
    const DiagnosticTraceMode user_trace_mode = diagnostics.trace_mode;
    diagnostics = package_diagnostics;
    diagnostics.trace_mode = user_trace_mode;
    return true;
}

bool Config::load_runtime_json(const std::string& json_text) {
    if (json_text.empty()) {
        return false;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(json_text);
        if (!j.is_object()) {
            return false;
        }
        apply_config_json(*this, j);
        if (!j.contains("resolved_color_schemes") ||
            !apply_color_schemes(*this, j["resolved_color_schemes"]) ||
            preset_color_schemes.find(theme) == preset_color_schemes.end()) {
            return false;
        }
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    cxxime::load_diagnostics_config_json(json_text, &diagnostics);
    return true;
}

bool Config::load_themes(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    try {
        nlohmann::ordered_json j = nlohmann::ordered_json::parse(file);
        if (!j.contains("preset_color_schemes") ||
            !apply_color_schemes(*this, j["preset_color_schemes"])) {
            return false;
        }
        if (preset_color_schemes.find(theme) == preset_color_schemes.end()) {
            theme = preset_color_scheme_order.front();
        }
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    return true;
}

static nlohmann::json build_config_json(const Config& config, bool include_diagnostics) {
    nlohmann::json j;
    j["schema"]["name"] = "知意输入法";
    j["schema"]["version"] = "1.0";
    j["schema"]["description"] = "Zhiyi IME default configuration";

    j["engine"]["page_size"] = config.page_size;
    j["engine"]["input_mode"] = config.input_mode;
    j["engine"]["pinyin_scheme"] = normalize_pinyin_scheme_id(config.pinyin_scheme);
    j["engine"]["wubi_auto_commit"] = config.wubi_auto_commit;
    j["engine"]["wubi_commit_first_on_fifth_key"] =
        config.wubi_commit_first_on_fifth_key;
    j["engine"]["wubi_restart_on_fifth_after_miss"] =
        config.wubi_restart_on_fifth_after_miss;
    j["engine"]["wubi_code_hint"] = config.wubi_code_hint;
    j["engine"]["candidate_learning"] = config.candidate_learning;
    j["engine"]["pinyin_initials"] = config.pinyin_initials;
    j["engine"]["fuzzy_pinyin"] = config.fuzzy_pinyin;
    nlohmann::json groups = nlohmann::json::array();
    for (const auto& [bit, name] : kFuzzyGroupNames) {
        if (config.fuzzy_groups & bit) {
            groups.push_back(name);
        }
    }
    j["engine"]["fuzzy_groups"] = groups;
    j["engine"]["max_pinyin_length"] = kMaxInputCodeLength;

    j["initial_state"]["full_shape"] = config.initial_full_shape;
    j["initial_state"]["chinese_punct"] = config.initial_chinese_punct;

    j["laya"]["enable"] = config.laya.enable;
    j["laya"]["model_dir"] = config.laya.model_dir;
    j["laya"]["onnx"] = config.laya.onnx;
    j["laya"]["min_candidates"] = config.laya.min_candidates;
    j["laya"]["context_chars"] = config.laya.context_chars;
    j["laya"]["threads"] = config.laya.threads;
    j["laya"]["english"] = config.laya.english;
    j["laya"]["english_context_chars"] = config.laya.english_context_chars;
    j["laya"]["english_freq_weight"] = config.laya.english_freq_weight;
    j["laya"]["english_correction_weight"] = config.laya.english_correction_weight;

    j["english"]["mixed_in_chinese"] = config.english.mixed_in_chinese;
    j["english"]["min_input"] = config.english.min_input;
    j["english"]["position_in_pinyin"] = config.english.position_in_pinyin;
    j["english"]["completion_in_ascii"] = config.english.completion_in_ascii;
    j["english"]["word_mode"] = config.english.word_mode;
    j["english"]["completion_pool"] = config.english.completion_pool;
    j["english"]["completion_count"] = config.english.completion_count;
    j["english"]["min_score"] = config.english.min_score;
    j["english"]["correction"] = config.english.correction;
    j["english"]["correction_count"] = config.english.correction_count;

    j["style"]["font_face"] = config.font_name;
    j["style"]["font_point"] = config.font_size;
    j["style"]["layout"] = config.layout;
    j["style"]["render_backend"] = config.render_backend;
    j["style"]["inline_preedit"] = config.inline_preedit;
    j["style"]["preedit_type"] = config.preedit_type;

    j["layout"]["min_width"] = config.layout_config.min_width;
    j["layout"]["max_width"] = config.layout_config.max_width;
    j["layout"]["max_height"] = config.layout_config.max_height;
    j["layout"]["margin_x"] = config.layout_config.margin_x;
    j["layout"]["margin_y"] = config.layout_config.margin_y;
    j["layout"]["spacing"] = config.layout_config.spacing;
    j["layout"]["candidate_spacing"] = config.layout_config.candidate_spacing;
    j["layout"]["hilite_spacing"] = config.layout_config.hilite_spacing;
    j["layout"]["hilite_padding_x"] = config.layout_config.hilite_padding_x;
    j["layout"]["hilite_padding_y"] = config.layout_config.hilite_padding_y;
    j["layout"]["round_corner"] = config.layout_config.round_corner;
    j["layout"]["round_corner_ex"] = config.layout_config.round_corner_ex;
    j["layout"]["label_font_point"] = config.layout_config.label_font_point;
    j["layout"]["border_width"] = config.layout_config.border_width;
    j["layout"]["preedit_highlight_padding_x"] =
        config.layout_config.preedit_highlight_padding_x;
    j["layout"]["preedit_highlight_padding_y"] =
        config.layout_config.preedit_highlight_padding_y;
    j["layout"]["preedit_confirmed_gap"] = config.layout_config.preedit_confirmed_gap;
    j["layout"]["preedit_boundary_gap"] = config.layout_config.preedit_boundary_gap;
    j["layout"]["preedit_highlight_corner"] = config.layout_config.preedit_highlight_corner;
    j["layout"]["preedit_highlight_border_width"] =
        config.layout_config.preedit_highlight_border_width;

    j["theme"] = config.theme;
    j["ui"]["language"] = config.ui_language;

    j["status_window"]["enable"] = config.status_window.enable;
    j["status_window"]["x"] = config.status_window.x;
    j["status_window"]["y"] = config.status_window.y;
    j["status_window"]["show_on_startup"] = config.status_window.show_on_startup;

    if (include_diagnostics) {
        j["diagnostics"]["trace_mode"] = diagnostic_trace_mode_name(config.diagnostics.trace_mode);
        j["diagnostics"]["log_max_size"] = config.diagnostics.log_max_size;
        j["diagnostics"]["log_max_files"] = config.diagnostics.log_max_files;
        j["diagnostics"]["normal_sample_rate"] = config.diagnostics.normal_sample_rate;
        j["diagnostics"]["truncated_sample_rate"] = config.diagnostics.truncated_sample_rate;
        j["diagnostics"]["slow_query_us"] = config.diagnostics.slow_query_us;
        j["diagnostics"]["cache_miss_slow_us"] = config.diagnostics.cache_miss_slow_us;
        j["diagnostics"]["slow_ipc_us"] = config.diagnostics.slow_ipc_us;
        j["diagnostics"]["slow_window_us"] = config.diagnostics.slow_window_us;
        j["diagnostics"]["slow_total_us"] = config.diagnostics.slow_total_us;
    }

    nlohmann::json ac;
    nlohmann::json sk;
    for (const auto& [k, v] : config.ascii_switch_key)
        sk[k] = v;
    ac["switch_key"] = sk;
    j["ascii_composer"] = ac;

    j["shortcuts"]["activate_ime"] = keyboard_shortcut_string(config.activate_ime_shortcut);
    j["shortcuts"]["english_style"] = keyboard_shortcut_string(config.english_style_shortcut);
    j["shortcuts"]["ascii_toggle"] = keyboard_shortcut_string(config.ascii_toggle_shortcut);

    return j;
}

std::string Config::to_user_json() const {
    nlohmann::json j = build_config_json(*this, false);
    j["diagnostics"]["trace_mode"] = diagnostic_trace_mode_name(diagnostics.trace_mode);
    return j.dump(4);
}

std::string Config::to_runtime_json() const {
    nlohmann::json j = build_config_json(*this, true);
    j["resolved_color_schemes"] = nlohmann::json::object();
    for (const auto& [name, colors] : preset_color_schemes) {
        auto& scheme = j["resolved_color_schemes"][name];
        scheme["text_color"] = colors.text_color;
        scheme["back_color"] = colors.back_color;
        scheme["border_color"] = colors.border_color;
        scheme["candidate_text_color"] = colors.candidate_text_color;
        scheme["label_text_color"] = colors.label_text_color;
        scheme["hilited_text_color"] = colors.hilited_text_color;
        scheme["hilited_back_color"] = colors.hilited_back_color;
        scheme["hilited_candidate_text_color"] = colors.hilited_candidate_text_color;
        scheme["hilited_candidate_back_color"] = colors.hilited_candidate_back_color;
        scheme["preedit_cursor_color"] = colors.preedit_cursor_color;
        scheme["comment_text_color"] = colors.comment_text_color;
        scheme["prevpage_color"] = colors.prevpage_color;
        scheme["nextpage_color"] = colors.nextpage_color;
        scheme["preedit_active_back_color"] = colors.preedit_active_back_color;
        scheme["preedit_active_border_color"] = colors.preedit_active_border_color;
    }
    return j.dump(4);
}

} // namespace cxxime
