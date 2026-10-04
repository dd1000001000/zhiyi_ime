// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// candidate_window_tool — interactive visual test for CandidateWindow

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>
#include <cxxime/candidate_window.h>
#include <cxxime/candidate.h>
#include <cxxime/render_context.h>
#include <cxxime/config.h>
#include <cxxime/data_path.h>

static cxxime::CandidateWindow g_window;
static cxxime::CandidatePage g_page;
static bool g_d2d = false;  // will be set from config after load
static cxxime::Config g_config;
static int g_page_idx = 0;
static int g_total_pages = 3;
static HWND g_parent = nullptr;
static int g_font_size_idx = 1;  // index into {12, 14, 16, 18, 20}, start at 14
static const int g_font_sizes[] = {12, 14, 16, 18, 20};

static void update_display();

// Sample candidate data
static std::vector<std::vector<std::string>> g_pages = {
    {"你好", "您好", "昵称", "尼采", "拟态", "腻烦", "匿藏"},
    {"世界", "师姐", "时间", "实践", "事件", "世间", "时节"},
    {"中国", "种过", "忠果", "终过", "重过", "中锅", "种锅"},
};

// Test data for truncation: long text candidates
static std::vector<std::vector<std::string>> g_long_pages = {
    {"中华人民共和国中央人民政府公告", "中华人民共和国万岁", "世界人民大团结万岁",
     "中央", "中华", "人民", "共和国"},
    {"这是一个非常非常非常非常非常非常非常非常长的候选词文本", "短词", "中等长度的候选词",
     "另一个超长候选词用于测试文本截断功能是否正常工作", "你好"},
    {"SingleVeryLongCandidateTextThatExceedsMaxWidthForTestingMiddleTruncation", "short", "medium text"},
    // Single very long candidate — triggers middle truncation (前缀…后缀)
    {"中华人民共和国中央人民政府公告一九四九年十月一日在北京天安门广场举行开国大典毛泽东主席宣告中华人民共和国中央人民政府今天成立了"},
};
static bool g_long_mode = false;
static int g_long_page_idx = 0;

static void build_page() {
    g_page.candidates.clear();
    auto& words = g_long_mode ? g_long_pages[g_page_idx] : g_pages[g_page_idx];
    for (size_t i = 0; i < words.size(); ++i) {
        cxxime::Candidate c;
        c.text = words[i];
        c.recommended = i == 0;  // Laya recommendation mark (sparkle)
        g_page.candidates.push_back(c);
    }
    g_page.highlighted = std::getenv("ZHIYIIME_TOOL_HIGHLIGHT") ? std::atoi(std::getenv("ZHIYIIME_TOOL_HIGHLIGHT")) : 0;
    g_page.page_size = 7;
}

static void handle_candidate_selection(std::size_t index) {
    if (index < g_page.candidates.size()) {
        printf("Clicked: %s\n", g_page.candidates[index].text.c_str());
        g_window.hide();
    }
}

static void handle_page(cxxime::CandidatePageDirection direction) {
    if (direction == cxxime::CandidatePageDirection::Previous && g_page_idx > 0) {
        --g_page_idx;
    } else if (direction == cxxime::CandidatePageDirection::Next &&
               g_page_idx + 1 < g_total_pages) {
        ++g_page_idx;
    } else {
        return;
    }
    build_page();
    update_display();
}

static void configure_candidate_window_callbacks() {
    g_window.set_candidate_selection_callback(handle_candidate_selection);
    g_window.set_page_callback(handle_page);
}

static void update_display() {
    g_window.set_page_info(g_page_idx + 1, g_total_pages);
    g_window.update(g_page);
    RECT fakeCaret = {400, 300, 400, 320};
    g_window.move_to_caret(fakeCaret);
    g_window.show();
    wchar_t title[256];
    swprintf(title, 256, L"Candidate Window Tool [%s] 主题:%S 布局:%S 字体:%dpx",
             g_d2d ? L"D2D" : L"GDI",
             g_config.theme.c_str(),
             g_config.layout.c_str(),
             g_font_sizes[g_font_size_idx]);
    if (g_parent) SetWindowTextW(g_parent, title);
}

static LRESULT CALLBACK ParentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: {
        int vk = (int)wp;
        if (vk >= '1' && vk <= '9') {
            int idx = vk - '1';
            if (idx < (int)g_page.candidates.size()) {
                printf("Selected: %s\n", g_page.candidates[idx].text.c_str());
                g_window.hide();
            }
        } else if (vk == VK_ESCAPE) {
            printf("Escape — hiding window\n");
            g_window.hide();
        } else if (vk == VK_NEXT) {
            if (g_page_idx + 1 < g_total_pages) {
                g_page_idx++;
                build_page();
                update_display();
            }
        } else if (vk == VK_PRIOR) {
            if (g_page_idx > 0) {
                g_page_idx--;
                build_page();
                update_display();
            }
        } else if (vk == VK_SPACE) {
            printf("Committed: %s\n", g_page.candidates[g_page.highlighted].text.c_str());
            g_window.hide();
        } else if (vk == 'T') {
            // Cycle through the presets actually loaded from themes.json.
            static std::vector<std::string> schemes;
            if (schemes.empty()) {
                for (const auto& [name, colors] : g_config.preset_color_schemes) {
                    schemes.push_back(name);
                }
                std::sort(schemes.begin(), schemes.end());
            }
            static int ti = -1;  // first press skips the default theme
            if (schemes.empty()) {
                return 0;
            }
            ti = (ti + 1) % static_cast<int>(schemes.size());
            g_config.theme = schemes[ti];
            auto tm = cxxime::build_theme_from_config(g_config);
            printf("T=%s bg=(%d,%d,%d) hl=(%d,%d,%d)\n", schemes[ti].c_str(),
                   tm.background.r, tm.background.g, tm.background.b,
                   tm.hilited_back.r, tm.hilited_back.g, tm.hilited_back.b);
            g_window.hide();
            g_window.set_theme(tm);
            update_display();
        } else if (vk == 'L') {
            // Toggle layout
            static bool vert = false;
            vert = !vert;
            g_window.set_layout(vert ? "vertical" : "horizontal");
            printf("Layout: %s\n", vert ? "vertical" : "horizontal");
            update_display();
        } else if (vk == 'D') {
            g_d2d = !g_d2d;
            g_window.set_render_backend(g_d2d ? cxxime::RenderBackend::D2D : cxxime::RenderBackend::GDI);
            printf("渲染器: %s  主题: %s  布局: %s  字体: %dpx\n",
                   g_d2d ? "D2D" : "GDI",
                   g_config.theme.c_str(),
                   g_config.layout.c_str(),
                   g_font_sizes[g_font_size_idx]);
            update_display();
        } else if (vk == 'F') {
            g_font_size_idx = (g_font_size_idx + 1) % 5;
            int fs = g_font_sizes[g_font_size_idx];
            g_config.font_size = fs;
            printf("字体大小: %dpx\n", fs);
            g_window.destroy();
            g_window.create(nullptr, g_config);
            configure_candidate_window_callbacks();
            g_window.set_layout(g_config.layout);
            build_page();
            if (g_d2d) g_window.set_render_backend(cxxime::RenderBackend::D2D);
            update_display();
        } else if (vk == 'W') {
            // Toggle long text mode for truncation testing
            g_long_mode = !g_long_mode;
            g_long_page_idx = 0;
            g_page_idx = 0;
            if (g_long_mode) {
                g_total_pages = (int)g_long_pages.size();
                printf("Long text mode ON — testing truncation (W to toggle off)\n");
            } else {
                g_total_pages = (int)g_pages.size();
                printf("Long text mode OFF\n");
            }
            build_page();
            update_display();
        } else if (vk == 'P') {
            // Cycle preedit_type: off → composition → preview → off
            static int preedit_mode = 0;  // 0=off, 1=composition, 2=preview
            preedit_mode = (preedit_mode + 1) % 3;
            const char* mode_names[] = {"off", "composition", "preview"};
            const char* raw_pinyin = "ni'hao";
            std::string preedit_str;
            switch (preedit_mode) {
            case 0: preedit_str = ""; break;
            case 1: preedit_str = raw_pinyin; break;  // composition: raw pinyin
            case 2: {  // preview: first candidate
                if (!g_page.candidates.empty())
                    preedit_str = g_page.candidates[0].text;
                else
                    preedit_str = raw_pinyin;
                break;
            }
            }
            g_window.set_preedit(preedit_str);
            printf("Preedit type: %s → \"%s\"\n", mode_names[preedit_mode], preedit_str.c_str());
            update_display();
        } else {
            // Letters simulate pinyin input
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("=== Candidate Window Test Tool ===\n");
    printf("Keys: 1-9=select  Space=commit  Esc=hide\n");
    printf("      PageUp/PageDown=flip  T=theme  L=layout  D=D2D/GDI  F=font  P=preedit_type\n");
    printf("      W=long_text_mode (test truncation)\n");
    printf("      (click on window to select candidates)\n\n");

    // Create hidden parent window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ParentWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"ZhiyiIMEToolParent";
    RegisterClassExW(&wc);

    HWND parent = CreateWindowExW(0, L"ZhiyiIMEToolParent", L"Candidate Window Tool",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  400, 100, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    // Create candidate window, load themes
    g_config.load(cxxime::data_path("default.json"));
    g_config.load_themes(cxxime::data_path("themes.json"));
    g_config.page_size = 7;
    // Screenshots: ZHIYIIME_TOOL_THEME=moon_dark, ZHIYIIME_TOOL_BACKEND=gdi.
    if (const char* theme = std::getenv("ZHIYIIME_TOOL_THEME")) g_config.theme = theme;
    if (const char* backend = std::getenv("ZHIYIIME_TOOL_BACKEND")) g_config.render_backend = backend;
    g_d2d = (g_config.render_backend != "gdi");
    g_window.create(nullptr, g_config);

    configure_candidate_window_callbacks();

    build_page();
    g_window.set_preedit("ni'hao");
    update_display();

    g_parent = parent;
    ShowWindow(parent, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_window.destroy();
    return 0;
}
