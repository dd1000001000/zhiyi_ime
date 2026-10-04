// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstdint>
#include <cstdio>
#include <fstream>

#include <windows.h>

#include <json.hpp>

#include <cxxime/config.h>
#include <cxxime/control_protocol.h>
#include <cxxime/render_context.h>

#include "support/testutil.h"

TEST(Config, defaults) {
    cxxime::Config cfg;
    ASSERT_EQ(cfg.page_size, 7);
    ASSERT_EQ(cfg.font_size, 14);
    ASSERT_TRUE(cfg.font_name == "Microsoft YaHei UI");
    ASSERT_TRUE(cfg.layout == "horizontal");
    ASSERT_TRUE(cfg.theme == "moon_light");
    ASSERT_TRUE(cfg.wubi_auto_commit);
    ASSERT_TRUE(cfg.wubi_commit_first_on_fifth_key);
    ASSERT_TRUE(cfg.wubi_restart_on_fifth_after_miss);
    ASSERT_TRUE(!cfg.wubi_code_hint);
    ASSERT_TRUE(cfg.candidate_learning);
    ASSERT_TRUE(!cfg.pinyin_initials);
    ASSERT_TRUE(!cfg.fuzzy_pinyin);
    ASSERT_EQ(cfg.fuzzy_groups, 0x7F);
    ASSERT_EQ(cfg.ui_language, "auto");
    ASSERT_EQ(cfg.pinyin_scheme, "full_pinyin");
    ASSERT_TRUE(cfg.english_style_shortcut.enabled());
    ASSERT_TRUE(!cfg.activate_ime_shortcut.enabled());
    ASSERT_TRUE(!cfg.initial_full_shape);
    ASSERT_TRUE(cfg.initial_chinese_punct);
    ASSERT_EQ(cfg.layout_config.preedit_highlight_padding_x, 4);
    ASSERT_EQ(cfg.layout_config.preedit_highlight_padding_y, 2);
    ASSERT_EQ(cfg.layout_config.preedit_confirmed_gap, 6);
    ASSERT_EQ(cfg.layout_config.preedit_boundary_gap, 1);
    ASSERT_EQ(cfg.layout_config.preedit_highlight_corner, 3);
    ASSERT_EQ(cfg.layout_config.preedit_highlight_border_width, 1);
    ASSERT_EQ(cfg.diagnostics.trace_mode, cxxime::DiagnosticTraceMode::kOff);
}

TEST(Config, diagnostic_trace_mode_is_fail_closed) {
    ASSERT_EQ(cxxime::parse_diagnostic_trace_mode("normal"), cxxime::DiagnosticTraceMode::kNormal);
    ASSERT_EQ(cxxime::parse_diagnostic_trace_mode("unknown"), cxxime::DiagnosticTraceMode::kOff);
}

TEST(Config, user_config_controls_only_diagnostic_trace_mode) {
    const char* path = "test_user_diagnostics_config.json";
    {
        std::ofstream file(path);
        file << R"({"diagnostics":{"trace_mode":"verbose","slow_ipc_us":9999}})";
    }

    cxxime::Config config;
    config.diagnostics.trace_mode = cxxime::DiagnosticTraceMode::kError;
    config.diagnostics.slow_ipc_us = 1234;
    ASSERT_TRUE(config.load_user(path));
    ASSERT_EQ(config.diagnostics.trace_mode, cxxime::DiagnosticTraceMode::kVerbose);
    ASSERT_EQ(config.diagnostics.slow_ipc_us, 1234);
    ASSERT_TRUE(
        config.load_user_json(R"({"diagnostics":{"trace_mode":"normal","slow_ipc_us":5678}})"));
    ASSERT_EQ(config.diagnostics.trace_mode, cxxime::DiagnosticTraceMode::kNormal);
    ASSERT_EQ(config.diagnostics.slow_ipc_us, 1234);

    const nlohmann::json saved = nlohmann::json::parse(config.to_user_json());
    ASSERT_TRUE(saved["diagnostics"].is_object());
    ASSERT_EQ(saved["diagnostics"].size(), 1u);
    ASSERT_TRUE(saved["diagnostics"]["trace_mode"] == "normal");
    std::remove(path);
}

TEST(Config, load_valid_json) {
    const char* path = "test_config.json";
    {
        std::ofstream f(path);
        f << R"({
        "engine": {
        "page_size": 5,
        "wubi_auto_commit": false,
        "wubi_commit_first_on_fifth_key": false,
        "wubi_restart_on_fifth_after_miss": false,
        "wubi_code_hint": true,
        "candidate_learning": false,
        "pinyin_initials": true
        },
        "shortcuts": {
            "english_style": "Ctrl+Shift+M",
            "activate_ime": "Ctrl+Alt+C"
        },
        "style": {"font_face": "Arial", "font_point": 18},
        "theme": "dark"
        })";
    }

    cxxime::Config cfg;
    ASSERT_TRUE(cfg.load(path));
    ASSERT_EQ(cfg.page_size, 5);
    ASSERT_EQ(cfg.font_size, 18);
    ASSERT_TRUE(cfg.font_name == "Arial");
    ASSERT_TRUE(cfg.theme == "dark");
    ASSERT_TRUE(!cfg.wubi_auto_commit);
    ASSERT_TRUE(!cfg.wubi_commit_first_on_fifth_key);
    ASSERT_TRUE(!cfg.wubi_restart_on_fifth_after_miss);
    ASSERT_TRUE(cfg.wubi_code_hint);
    ASSERT_TRUE(!cfg.candidate_learning);
    ASSERT_TRUE(cfg.pinyin_initials);
    ASSERT_EQ(cfg.english_style_shortcut.modifiers,
              cxxime::kKeyModifierControl | cxxime::kKeyModifierShift);
    ASSERT_EQ(cfg.english_style_shortcut.virtual_key, static_cast<uint32_t>('M'));
    ASSERT_EQ(cfg.activate_ime_shortcut.modifiers,
              cxxime::kKeyModifierControl | cxxime::kKeyModifierAlt);
    ASSERT_EQ(cfg.activate_ime_shortcut.virtual_key, static_cast<uint32_t>('C'));
    ASSERT_TRUE(!cfg.initial_full_shape);
    ASSERT_TRUE(cfg.initial_chinese_punct);

    std::remove(path);
}

TEST(Config, json_round_trip_preserves_wubi_options_learning_and_styles) {
    cxxime::Config saved;
    saved.wubi_auto_commit = false;
    saved.wubi_commit_first_on_fifth_key = false;
    saved.wubi_restart_on_fifth_after_miss = false;
    saved.wubi_code_hint = true;
    saved.candidate_learning = false;
    saved.pinyin_initials = true;
    saved.ui_language = "en-US";
    saved.fuzzy_pinyin = true;
    saved.fuzzy_groups = 0x01 | 0x10;  // z=zh, an=ang

    cxxime::Config loaded;
    ASSERT_TRUE(loaded.load_json(saved.to_user_json()));
    ASSERT_TRUE(!loaded.wubi_auto_commit);
    ASSERT_TRUE(!loaded.wubi_commit_first_on_fifth_key);
    ASSERT_TRUE(!loaded.wubi_restart_on_fifth_after_miss);
    ASSERT_TRUE(loaded.wubi_code_hint);
    ASSERT_TRUE(!loaded.candidate_learning);
    ASSERT_TRUE(loaded.pinyin_initials);
    ASSERT_EQ(loaded.ui_language, "en-US");
    ASSERT_TRUE(loaded.fuzzy_pinyin);
    ASSERT_EQ(loaded.fuzzy_groups, 0x11);

    cxxime::Config named;
    ASSERT_TRUE(named.load_json(R"({"engine":{"fuzzy_groups":["n_l","in_ing","unknown"]}})"));
    ASSERT_EQ(named.fuzzy_groups, 0x08 | 0x40);
}

TEST(Config, json_round_trip_preserves_preedit_highlight_layout) {
    cxxime::Config saved;
    saved.layout_config.preedit_highlight_padding_x = 7;
    saved.layout_config.preedit_highlight_padding_y = 4;
    saved.layout_config.preedit_confirmed_gap = 9;
    saved.layout_config.preedit_boundary_gap = 3;
    saved.layout_config.preedit_highlight_corner = 8;
    saved.layout_config.preedit_highlight_border_width = 2;

    cxxime::Config loaded;
    ASSERT_TRUE(loaded.load_json(saved.to_user_json()));
    ASSERT_EQ(loaded.layout_config.preedit_highlight_padding_x, 7);
    ASSERT_EQ(loaded.layout_config.preedit_highlight_padding_y, 4);
    ASSERT_EQ(loaded.layout_config.preedit_confirmed_gap, 9);
    ASSERT_EQ(loaded.layout_config.preedit_boundary_gap, 3);
    ASSERT_EQ(loaded.layout_config.preedit_highlight_corner, 8);
    ASSERT_EQ(loaded.layout_config.preedit_highlight_border_width, 2);
}

TEST(Config, former_mixed_input_mode_falls_back_to_pinyin) {
    cxxime::Config config;
    ASSERT_TRUE(config.load_json(R"({"engine":{"input_mode":2}})"));
    ASSERT_EQ(config.input_mode, 0);
    ASSERT_TRUE(config.load_json(R"({"engine":{"input_mode":1}})"));
    ASSERT_EQ(config.input_mode, 1);
}

TEST(Config, pinyin_scheme_is_always_full_pinyin) {
    // Shuangpin schemes were removed; old configs fall back to full pinyin.
    const char* scheme_ids[] = {
        "full_pinyin",
        "microsoft_shuangpin",
        "xiaohe_shuangpin",
        "ziranma_shuangpin",
        "sogou_shuangpin",
    };
    for (const char* scheme_id : scheme_ids) {
        cxxime::Config config;
        const std::string json =
            std::string(R"({"engine":{"pinyin_scheme":")") + scheme_id + R"("}})";
        ASSERT_TRUE(config.load_json(json));
        ASSERT_EQ(config.pinyin_scheme, "full_pinyin");
    }

    cxxime::Config loaded;
    ASSERT_TRUE(loaded.load_json(R"({"engine":{"pinyin_scheme":"unsupported"}})"));
    ASSERT_EQ(loaded.pinyin_scheme, "full_pinyin");
}

TEST(Config, shortcut_validation_accepts_current_bindings_and_disables_invalid_ones) {
    cxxime::Config config;
    ASSERT_TRUE(
        config.load_json(R"({"shortcuts":{"english_style":"M","activate_ime":"Shift+C"}})"));
    ASSERT_TRUE(!config.english_style_shortcut.enabled());
    ASSERT_TRUE(!config.activate_ime_shortcut.enabled());

    config = {};
    ASSERT_TRUE(config.load_json(R"({"shortcuts":{"activate_ime":"Ctrl+/"}})"));
    ASSERT_EQ(config.activate_ime_shortcut.modifiers, cxxime::kKeyModifierControl);
    ASSERT_EQ(config.activate_ime_shortcut.virtual_key, static_cast<uint32_t>(VK_OEM_2));
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(config.activate_ime_shortcut) == "Ctrl+/");

    config = {};
    ASSERT_TRUE(config.load_json(R"({"shortcuts":{"activate_ime":"F4"}})"));
    ASSERT_EQ(config.activate_ime_shortcut.modifiers, 0u);
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(config.activate_ime_shortcut) == "F4");
}

TEST(Config, chinese_english_switch_shortcut_loads_and_wins_over_style) {
    cxxime::Config config;
    ASSERT_TRUE(!config.ascii_toggle_shortcut.enabled());  // tapping Shift by default
    ASSERT_TRUE(config.load_json(R"({"shortcuts":{"ascii_toggle":"Ctrl+Shift+E"}})"));
    ASSERT_EQ(config.ascii_toggle_shortcut.modifiers,
              cxxime::kKeyModifierControl | cxxime::kKeyModifierShift);
    ASSERT_EQ(config.ascii_toggle_shortcut.virtual_key, static_cast<uint32_t>('E'));

    // A plain letter would block typing.
    config = {};
    ASSERT_TRUE(config.load_json(R"({"shortcuts":{"ascii_toggle":"E"}})"));
    ASSERT_TRUE(!config.ascii_toggle_shortcut.enabled());

    config = {};
    ASSERT_TRUE(config.load_json(R"({"shortcuts":{
        "ascii_toggle":"Ctrl+Space",
        "english_style":"Ctrl+Space"
    }})"));
    ASSERT_TRUE(config.ascii_toggle_shortcut.enabled());
    ASSERT_TRUE(!config.english_style_shortcut.enabled());

    cxxime::Config saved;
    saved.theme = "dark";
    saved.preset_color_schemes["dark"].text_color = 0x11223344;  // a runtime snapshot needs it
    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Alt+Q", &saved.ascii_toggle_shortcut));
    cxxime::Config loaded;
    ASSERT_TRUE(loaded.load_runtime_json(saved.to_runtime_json()));
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(loaded.ascii_toggle_shortcut) == "Alt+Q");
}

TEST(Config, conflicting_shortcuts_disable_style_switch) {
    cxxime::Config config;
    ASSERT_TRUE(config.load_json(R"({"shortcuts":{
        "english_style":"Ctrl+Alt+C",
        "activate_ime":"Ctrl+Alt+C"
    }})"));
    ASSERT_TRUE(!config.english_style_shortcut.enabled());
    ASSERT_TRUE(config.activate_ime_shortcut.enabled());
}

TEST(Config, initial_state_round_trip) {
    cxxime::Config saved;
    saved.initial_full_shape = true;
    saved.initial_chinese_punct = false;

    cxxime::Config loaded;
    ASSERT_TRUE(loaded.load_json(saved.to_user_json()));
    ASSERT_TRUE(loaded.initial_full_shape);
    ASSERT_TRUE(!loaded.initial_chinese_punct);
}

TEST(Config, obsolete_status_auto_dock_is_ignored) {
    const char* json = R"({"status_window":{
        "auto_dock":true,"enable":false,"x":120,"y":240,"show_on_startup":false
    }})";
    cxxime::Config loaded;
    ASSERT_TRUE(loaded.load_json(json));
    ASSERT_TRUE(!loaded.status_window.enable);
    ASSERT_EQ(loaded.status_window.x, 120);
    ASSERT_EQ(loaded.status_window.y, 240);
    ASSERT_TRUE(!loaded.status_window.show_on_startup);
    const auto saved = nlohmann::json::parse(loaded.to_user_json());
    ASSERT_TRUE(!saved["status_window"].contains("auto_dock"));
}

TEST(Config, runtime_snapshot_round_trip) {
    cxxime::Config saved;
    saved.page_size = 7;
    saved.font_name = "Arial";
    saved.font_size = 18;
    saved.theme = "dark";
    saved.inline_preedit = true;
    saved.status_window.enable = false;
    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Ctrl+Alt+M",
                                                &saved.english_style_shortcut));
    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Ctrl+Shift+Space",
                                                &saved.activate_ime_shortcut));
    saved.ascii_switch_key["Shift_L"] = "commit_code";
    saved.diagnostics.trace_mode = cxxime::DiagnosticTraceMode::kError;
    saved.diagnostics.slow_ipc_us = 2345;
    saved.preset_color_schemes["dark"].text_color = 0x11223344;

    const std::string snapshot = saved.to_runtime_json();
    ASSERT_TRUE(snapshot.size() <= cxxime::CONTROL_MAX_PAYLOAD);

    cxxime::Config loaded;
    ASSERT_TRUE(loaded.load_runtime_json(snapshot));
    ASSERT_EQ(loaded.page_size, 7);
    ASSERT_TRUE(loaded.font_name == "Arial");
    ASSERT_EQ(loaded.font_size, 18);
    ASSERT_TRUE(loaded.theme == "dark");
    ASSERT_TRUE(loaded.inline_preedit);
    ASSERT_TRUE(!loaded.status_window.enable);
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(loaded.english_style_shortcut) ==
                "Ctrl+Alt+M");
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(loaded.activate_ime_shortcut) ==
                "Ctrl+Shift+Space");
    ASSERT_TRUE(loaded.ascii_switch_key["Shift_L"] == "commit_code");
    ASSERT_EQ(loaded.diagnostics.trace_mode, cxxime::DiagnosticTraceMode::kError);
    ASSERT_EQ(loaded.diagnostics.slow_ipc_us, 2345);
    ASSERT_EQ(loaded.preset_color_schemes["dark"].text_color, 0x11223344);
}

TEST(Config, production_runtime_snapshot_fits_control_payload) {
    cxxime::Config config;
    ASSERT_TRUE(config.load(std::string(CXXIME_DATA_DIR) + "default.json"));
    ASSERT_TRUE(config.load_themes(std::string(CXXIME_DATA_DIR) + "themes.json"));

    const std::string snapshot = config.to_runtime_json();
    ASSERT_TRUE(!snapshot.empty());
    ASSERT_TRUE(snapshot.size() <= cxxime::CONTROL_MAX_PAYLOAD);
}

TEST(Config, built_in_themes_define_preedit_active_colors) {
    std::ifstream file(std::string(CXXIME_DATA_DIR) + "themes.json");
    ASSERT_TRUE(file.is_open());
    const nlohmann::json themes = nlohmann::json::parse(file);
    ASSERT_TRUE(themes.contains("preset_color_schemes"));
    ASSERT_EQ(themes["preset_color_schemes"].size(), 2u);  // light and dark
    for (const auto& item : themes["preset_color_schemes"].items()) {
        ASSERT_TRUE(item.value().contains("preedit_active_back_color"));
        ASSERT_TRUE(item.value().contains("preedit_active_border_color"));
        ASSERT_TRUE(item.value()["preedit_active_back_color"].is_number_integer());
        ASSERT_TRUE(item.value()["preedit_active_border_color"].is_number_integer());
    }
}

TEST(Config, themes_preserve_file_order_and_fallback_to_first_theme) {
    const char* path = "test_ordered_themes.json";
    {
        std::ofstream file(path);
        file << R"({"preset_color_schemes":{
            "second":{"back_color":16777215,"text_color":0},
            "first":{"back_color":0,"text_color":16777215}
        }})";
    }

    cxxime::Config config;
    config.theme = "removed";
    ASSERT_TRUE(config.load_themes(path));
    ASSERT_EQ(config.preset_color_scheme_order.size(), 2u);
    ASSERT_EQ(config.preset_color_scheme_order[0], "second");
    ASSERT_EQ(config.preset_color_scheme_order[1], "first");
    ASSERT_EQ(config.theme, "second");

    std::remove(path);
}

TEST(Config, invalid_theme_file_preserves_previous_themes) {
    const char* valid_path = "test_atomic_themes_valid.json";
    const char* invalid_path = "test_atomic_themes_invalid.json";
    {
        std::ofstream file(valid_path);
        file << R"({"preset_color_schemes":{
            "stable":{"back_color":16777215,"text_color":0}
        }})";
    }
    {
        std::ofstream file(invalid_path);
        file << R"({"preset_color_schemes":{
            "partial":{"back_color":0,"text_color":16777215},
            "invalid":false
        }})";
    }

    cxxime::Config config;
    config.theme = "stable";
    ASSERT_TRUE(config.load_themes(valid_path));
    ASSERT_TRUE(!config.load_themes(invalid_path));
    ASSERT_EQ(config.preset_color_scheme_order.size(), 1u);
    ASSERT_EQ(config.preset_color_scheme_order[0], "stable");
    ASSERT_TRUE(config.preset_color_schemes.find("stable") != config.preset_color_schemes.end());
    ASSERT_EQ(config.theme, "stable");

    std::remove(valid_path);
    std::remove(invalid_path);
}

TEST(Config, theme_derives_muted_comment_color_unless_explicitly_configured) {
    const char* path = "test_comment_color_themes.json";
    {
        std::ofstream file(path);
        file << R"({"preset_color_schemes":{
            "derived":{"back_color":16777215,"candidate_text_color":0},
            "explicit":{"back_color":16777215,"candidate_text_color":0,
            "comment_text_color":1193046}
            }})";
    }

    cxxime::Config derived;
    derived.theme = "derived";
    ASSERT_TRUE(derived.load_themes(path));
    ASSERT_EQ(derived.preset_color_schemes["derived"].comment_text_color, 0x666666);
    auto derived_theme = cxxime::build_theme_from_config(derived);
    ASSERT_EQ(derived_theme.comment_text.r, 0x66);
    ASSERT_EQ(derived_theme.comment_text.g, 0x66);
    ASSERT_EQ(derived_theme.comment_text.b, 0x66);

    cxxime::Config explicit_color;
    explicit_color.theme = "explicit";
    ASSERT_TRUE(explicit_color.load_themes(path));
    ASSERT_EQ(explicit_color.preset_color_schemes["explicit"].comment_text_color, 0x123456);

    std::remove(path);
}

TEST(Config, theme_derives_preedit_cursor_color_unless_explicitly_configured) {
    const char* path = "test_preedit_cursor_color_themes.json";
    {
        std::ofstream file(path);
        file << R"({"preset_color_schemes":{
            "derived_light":{"back_color":16777215,"hilited_text_color":0},
            "derived_dark":{"back_color":0,"hilited_text_color":16777215},
            "sampled":{"back_color":15528174,"hilited_text_color":0,
            "hilited_candidate_back_color":16398858},
            "adjusted":{"back_color":2236962,"hilited_text_color":10153609,
            "hilited_candidate_back_color":3355443},
            "explicit":{"back_color":16777215,"hilited_text_color":0,
            "preedit_cursor_color":1193046},
            "explicit_black":{"back_color":16777215,"hilited_text_color":16777215,
            "preedit_cursor_color":0}
            }})";
    }

    cxxime::Config config;
    config.theme = "derived_light";
    ASSERT_TRUE(config.load_themes(path));
    ASSERT_EQ(config.preset_color_schemes["derived_light"].preedit_cursor_color, 0x666666);
    ASSERT_EQ(config.preset_color_schemes["derived_dark"].preedit_cursor_color, 0x7a7a7a);
    ASSERT_EQ(config.preset_color_schemes["sampled"].preedit_cursor_color, 0xfa3a0a);
    ASSERT_EQ(config.preset_color_schemes["adjusted"].preedit_cursor_color, 0x959595);
    ASSERT_EQ(config.preset_color_schemes["explicit"].preedit_cursor_color, 0x123456);
    ASSERT_EQ(config.preset_color_schemes["explicit_black"].preedit_cursor_color, 0);

    std::remove(path);
}

TEST(Config, theme_derives_preedit_active_colors_unless_explicitly_configured) {
    const char* path = "test_preedit_active_color_themes.json";
    {
        std::ofstream file(path);
        file << R"({"preset_color_schemes":{
            "derived":{"back_color":16777215,"hilited_candidate_back_color":0},
            "contrast_protected":{"back_color":16777215,"hilited_text_color":7105644,
            "hilited_candidate_back_color":7105644},
            "explicit":{"back_color":16777215,"hilited_candidate_back_color":0,
            "preedit_active_back_color":1193046,
            "preedit_active_border_color":6636321}
        }})";
    }

    cxxime::Config config;
    config.theme = "derived";
    ASSERT_TRUE(config.load_themes(path));
    ASSERT_EQ(config.preset_color_schemes["derived"].preedit_active_back_color, 0xe0e0e0);
    ASSERT_EQ(config.preset_color_schemes["derived"].preedit_active_border_color, 0xb3b3b3);
    ASSERT_EQ(config.preset_color_schemes["contrast_protected"].preedit_active_back_color,
              0xf0f0f0);
    ASSERT_EQ(config.preset_color_schemes["explicit"].preedit_active_back_color, 0x123456);
    ASSERT_EQ(config.preset_color_schemes["explicit"].preedit_active_border_color, 0x654321);

    const auto derived_theme = cxxime::build_theme_from_config(config);
    ASSERT_EQ(derived_theme.preedit_active_back.r, 0xe0);
    ASSERT_EQ(derived_theme.preedit_active_border.r, 0xb3);
    std::remove(path);
}

TEST(Config, load_missing_file) {
    cxxime::Config cfg;
    ASSERT_TRUE(!cfg.load("nonexistent_file.json"));
}

TEST(Config, load_invalid_json) {
    const char* path = "test_bad_config.json";
    {
        std::ofstream f(path);
        f << "{bad json";
    }

    cxxime::Config cfg;
    ASSERT_TRUE(!cfg.load(path));

    std::remove(path);
}

TEST(Config, load_empty_path) {
    cxxime::Config cfg;
    ASSERT_TRUE(cfg.load(""));  // Empty path = use defaults
}

TEST(Config, load_partial_json) {
    const char* path = "test_partial_config.json";
    {
        std::ofstream f(path);
        f << R"({"engine":{"page_size":7}})";
    }

    cxxime::Config cfg;
    ASSERT_TRUE(cfg.load(path));
    ASSERT_EQ(cfg.page_size, 7);
    ASSERT_TRUE(cfg.font_name == "Microsoft YaHei UI");
    ASSERT_EQ(cfg.font_size, 14);

    std::remove(path);
}

TEST(Config, load_diagnostics_section) {
    const char* path = "test_diagnostics_config.json";
    {
        std::ofstream f(path);
        f << R"({
            "diagnostics": {
                "trace_mode": "error",
                "log_max_size": 1048576,
                "log_max_files": 2,
                "normal_sample_rate": 0,
                "truncated_sample_rate": 50,
                "slow_query_us": 20000,
                "cache_miss_slow_us": 5000,
                "slow_ipc_us": 1000,
                "slow_window_us": 3000,
                "slow_total_us": 7000
            }
        })";
    }

    cxxime::Config cfg;
    ASSERT_TRUE(cfg.load(path));
    ASSERT_EQ(cfg.diagnostics.trace_mode, cxxime::DiagnosticTraceMode::kError);
    ASSERT_EQ(cfg.diagnostics.log_max_size, (size_t)1048576);
    ASSERT_EQ(cfg.diagnostics.log_max_files, 2);
    ASSERT_EQ(cfg.diagnostics.normal_sample_rate, 0);
    ASSERT_EQ(cfg.diagnostics.truncated_sample_rate, 50);
    ASSERT_EQ(cfg.diagnostics.slow_query_us, 20000);
    ASSERT_EQ(cfg.diagnostics.cache_miss_slow_us, 5000);
    ASSERT_EQ(cfg.diagnostics.slow_ipc_us, 1000);
    ASSERT_EQ(cfg.diagnostics.slow_window_us, 3000);
    ASSERT_EQ(cfg.diagnostics.slow_total_us, 7000);

    std::remove(path);
}

TEST(Config, style_modes_parse_current_values_and_reject_unknown_values) {
    cxxime::Config configured;
    ASSERT_TRUE(
        configured.load_json(R"({"style":{"inline_preedit":false,"preedit_type":"preview"}})"));
    ASSERT_TRUE(configured.inline_preedit);  // always inline; old configs said false
    ASSERT_EQ(configured.preedit_type, "preview");

    cxxime::Config unknown;
    ASSERT_TRUE(unknown.load_json(R"({"style":{"preedit_type":"invalid_mode"}})"));
    ASSERT_EQ(unknown.preedit_type, "composition");
}

TEST(Config, settings_presets_layouts) {
    std::ifstream f(std::string(CXXIME_PROJECT_DIR) + "data/settings_presets.json");
    ASSERT_TRUE(f.is_open());

    nlohmann::json j = nlohmann::json::parse(f);
    ASSERT_TRUE(j.contains("candidate_window"));
    ASSERT_TRUE(j["candidate_window"].contains("layout_presets"));

    auto& presets = j["candidate_window"]["layout_presets"];
    ASSERT_TRUE(presets.contains("default"));
    ASSERT_TRUE(presets.contains("recommended"));
    ASSERT_TRUE(presets["default"].contains("horizontal"));
    ASSERT_TRUE(presets["default"].contains("vertical"));
    ASSERT_TRUE(presets["recommended"].contains("horizontal"));
    ASSERT_TRUE(presets["recommended"].contains("vertical"));

    ASSERT_EQ(presets["default"]["horizontal"]["candidate_spacing"].get<int>(), 11);
    ASSERT_EQ(presets["default"]["vertical"]["candidate_spacing"].get<int>(), 2);
    ASSERT_EQ(presets["recommended"]["horizontal"]["candidate_spacing"].get<int>(), 13);
    ASSERT_EQ(presets["recommended"]["vertical"]["candidate_spacing"].get<int>(), 4);
}

RUN_ALL_TESTS()
