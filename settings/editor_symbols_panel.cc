// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <cxxime/data_path.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kCategoryId = 8000;
constexpr int kCandidatesId = 8001;
constexpr int kCopyId = 8002;
constexpr int kBrowsePageSize = 9;

} // namespace

void EditorApp::create_symbols_panel(HWND panel, int panel_width) {
    SetWindowSubclass(panel, PanelForwardProc, kCategoryId, reinterpret_cast<DWORD_PTR>(hwnd_));
    RECT rect = {};
    GetClientRect(panel, &rect);
    const int width = panel_width - kPanelPadLeft - S(10);
    const int top = kPanelPadTop;
    HWND help = CreateWindowExW(0, L"STATIC",
                                L"选择分类后浏览、复制符号。\r\n"
                                L"输入时：中文、中文标点、半角状态下，按 \\ 浏览分类；\r\n"
                                L"也可输入分类助记码，用 PgUp / PgDn 翻页，数字键选择。",
                                WS_CHILD | WS_VISIBLE, kPanelPadLeft, top, width, S(66), panel,
                                nullptr, nullptr, nullptr);
    SendMessageW(help, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hSymbolCategories_ = make_combo(kCategoryId, kPanelPadLeft, top + S(72), width, panel);
    set_combo_drop_count(hSymbolCategories_, 10);
    const int list_top = top + S(110);
    const int button_top = rect.bottom - S(38);
    hSymbolCandidates_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        kPanelPadLeft, list_top, width, button_top - list_top - S(8), panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCandidatesId)), nullptr, nullptr);
    SendMessageW(hSymbolCandidates_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hSymbolCopy_ = CreateWindowExW(
        0, L"BUTTON", L"复制符号", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        kPanelPadLeft, button_top, S(90), kCtrlH, panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyId)), nullptr, nullptr);
    SendMessageW(hSymbolCopy_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hSymbolStatus_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                     kPanelPadLeft + S(100), button_top, width - S(100), kCtrlH,
                                     panel, nullptr, nullptr, nullptr);
    SendMessageW(hSymbolStatus_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    EnableWindow(hSymbolCopy_, FALSE);
}

void EditorApp::load_symbol_categories() {
    const int previous = combo_index(hSymbolCategories_);
    SendMessageW(hSymbolCategories_, CB_RESETCONTENT, 0, 0);
    symbolCategoryCodes_.clear();
    if (symbolTable_.load(data_path("symbols.json"))) {
        for (int page = 0;; ++page) {
            const auto result = symbolTable_.translate_page("", page, kBrowsePageSize);
            for (const auto& category : result.candidates) {
                const std::wstring label = utf8_to_wstr(category.text + "  " + category.code);
                combo_add(hSymbolCategories_, label.c_str());
                symbolCategoryCodes_.push_back(category.code.substr(1));
            }
            if (result.extent.state != CandidateExtentState::kHasMore) {
                break;
            }
        }
    }
    if (!symbolCategoryCodes_.empty()) {
        const int index = previous >= 0 && previous < static_cast<int>(symbolCategoryCodes_.size())
                              ? previous
                              : 0;
        combo_set_index(hSymbolCategories_, index);
    }
    load_symbol_candidates();
}

void EditorApp::load_symbol_candidates() {
    SendMessageW(hSymbolCandidates_, LB_RESETCONTENT, 0, 0);
    symbolTexts_.clear();
    EnableWindow(hSymbolCopy_, FALSE);
    const int category = combo_index(hSymbolCategories_);
    if (category < 0 || category >= static_cast<int>(symbolCategoryCodes_.size())) {
        SetWindowTextW(hSymbolStatus_, L"符号表不可用，请检查词典文件。");
        return;
    }
    const auto& code = symbolCategoryCodes_[category];
    for (int page = 0;; ++page) {
        const auto result = symbolTable_.translate_page(code, page, kBrowsePageSize);
        for (const auto& candidate : result.candidates) {
            symbolTexts_.push_back(utf8_to_wstr(candidate.text));
            SendMessageW(hSymbolCandidates_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(symbolTexts_.back().c_str()));
        }
        if (result.extent.state != CandidateExtentState::kHasMore) {
            break;
        }
    }
    const std::wstring status = L"输入 " + utf8_to_wstr("\\" + code) + L" 可打开此分类";
    SetWindowTextW(hSymbolStatus_, status.c_str());
}

bool EditorApp::handle_symbols_command(int control_id, int notification) {
    if (control_id == kCategoryId && notification == CBN_SELCHANGE) {
        load_symbol_candidates();
        return true;
    }
    if (control_id == kCandidatesId && notification == LBN_SELCHANGE) {
        EnableWindow(hSymbolCopy_, SendMessageW(hSymbolCandidates_, LB_GETCURSEL, 0, 0) != LB_ERR);
        return true;
    }
    if (control_id == kCopyId && notification == BN_CLICKED) {
        const auto index = SendMessageW(hSymbolCandidates_, LB_GETCURSEL, 0, 0);
        if (index >= 0 && static_cast<size_t>(index) < symbolTexts_.size()) {
            const bool copied = copy_text_to_clipboard(hwnd_, symbolTexts_[index].c_str());
            SetWindowTextW(hSymbolStatus_,
                           copied ? L"已复制，可切换到应用粘贴。" : L"复制失败，请重试。");
        }
        return true;
    }
    return false;
}

} // namespace settings
} // namespace cxxime
