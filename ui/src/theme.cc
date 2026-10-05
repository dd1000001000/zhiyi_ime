// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/render_context.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <cxxime/config.h>
#include <cxxime/data_path.h>

namespace cxxime {

static std::wstring to_wstr(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static Color to_color(int v) {
    return {(uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), 255};
}

static Color blend(const Color& source, const Color& target, double amount) {
    auto channel = [&](uint8_t left, uint8_t right) {
        return static_cast<uint8_t>(std::round(left * (1.0 - amount) + right * amount));
    };
    return {channel(source.r, target.r), channel(source.g, target.g),
            channel(source.b, target.b), 255};
}

static const Config::SchemeColors* find_scheme(const Config& cfg, const std::string& name) {
    auto it = cfg.preset_color_schemes.find(name);
    return (it != cfg.preset_color_schemes.end()) ? &it->second : nullptr;
}

static int preedit_font_size(const Config& cfg) {
    int size = cfg.font_size > 2 ? cfg.font_size - 2 : cfg.font_size;
    if (cfg.layout_config.label_font_point > 0) {
        size = cfg.layout_config.label_font_point;
    }
    return std::clamp(size, 1, 72);
}

Theme build_theme_from_config(const Config& cfg) {
    const Config::SchemeColors* s = find_scheme(cfg, cfg.theme);
    if (!s) s = find_scheme(cfg, "aqua");

    if (!s) {
        // Hardcoded azure fallback
        Theme t;
        t.background   = {1, 78, 139, 255};
        t.text         = {232, 232, 255, 255};
        t.comment_text = {139, 170, 208, 255};
        t.label_text   = {100, 150, 198, 255};
        t.hilited_text = {254, 255, 127, 255};
        t.hilited_back = {1, 94, 169, 255};
        t.border       = {1, 78, 139, 255};
        t.preedit_text = {248, 255, 255, 255};
        t.preedit_separator = t.comment_text;
        t.preedit_active_back = blend(t.background, t.hilited_back, 0.20);
        t.preedit_active_border = blend(t.background, t.hilited_back, 0.40);
        t.preedit_cursor = {254, 255, 127, 255};
        t.prev_page    = {100, 150, 198, 255};
        t.next_page    = {100, 150, 198, 255};
        t.font_size    = cfg.font_size;
        t.preedit_font_size = preedit_font_size(cfg);
        t.font_name    = to_wstr(cfg.font_name);
        return t;
    }

    Theme t;
    t.background   = to_color(s->back_color);
    t.border       = to_color(s->border_color);
    t.text         = to_color(s->candidate_text_color);
    t.comment_text = to_color(s->comment_text_color);
    t.label_text   = to_color(s->label_text_color);
    t.hilited_text = to_color(s->hilited_candidate_text_color);
    t.hilited_back = to_color(s->hilited_candidate_back_color);
    t.preedit_text = to_color(s->hilited_text_color);
    t.preedit_separator = t.comment_text;
    t.preedit_active_back = to_color(s->preedit_active_back_color);
    t.preedit_active_border = to_color(s->preedit_active_border_color);
    t.preedit_cursor = to_color(s->preedit_cursor_color);
    t.prev_page    = to_color(s->prevpage_color);
    t.next_page    = to_color(s->nextpage_color);
    t.font_size    = cfg.font_size;
    t.preedit_font_size = preedit_font_size(cfg);
    t.font_name    = to_wstr(cfg.font_name);
    return t;
}

Theme get_theme(const std::string& name) {
    Config cfg;
    cfg.theme = name;
    cfg.load_themes(data_path("themes.json"));
    return build_theme_from_config(cfg);
}

Theme make_light_theme() { return get_theme("aqua"); }
Theme make_dark_theme()  { return get_theme("dark_temple"); }

} // namespace cxxime
