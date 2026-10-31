// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The Custom phrases page (docs/custom-phrases.md, issue #40): letters that bring up a text at a
// chosen place ("dh" → a phone number), and the built-in date / time / weekday (rq / sj / xq).
// One card, as Microsoft Pinyin's page: the switches, Add / Import / Export / Clear, the list.
// A phrase is added or edited in a dialog, one at a time. The phrases live in phrases.ini (Sogou
// Pinyin's format, custom_phrases.h), read when the page is shown and written at once on each
// change: the server reads the file again whenever it changes (server/src/phrase_service.cc), so
// the user may edit it by hand too. The switches (enabled, rq / sj / xq) are settings, saved with
// Apply.

#include "editor_app.h"

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>

#include <dwmapi.h>
#include <shobjidl.h>

#include <cxxime/custom_phrases.h>
#include <cxxime/data_path.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kPhraseEnableId = 6200;
constexpr int kPhraseDateId = 6201;
constexpr int kPhraseTimeId = 6202;
constexpr int kPhraseWeekId = 6203;
constexpr int kPhraseImportId = 6210;
constexpr int kPhraseAddId = 6212;
constexpr int kPhraseExportId = 6213;
constexpr int kPhraseClearId = 6214;
// The Add / Edit dialog.
constexpr int kDialogCodeId = 6220;
constexpr int kDialogPositionId = 6221;
constexpr int kDialogTextId = 6222;
constexpr int kDialogOkId = 6223;
constexpr int kDialogCancelId = 6224;
constexpr wchar_t kPhraseDialogClass[] = L"ZhiyiPhraseDialog";
constexpr int kPhraseEditFirstId = 6300;
constexpr int kPhraseRemoveFirstId = 6500;
constexpr int kMaxPhraseRows = 150;  // more: edited in the file

std::wstring filled(std::wstring text, const wchar_t* key, const std::wstring& value) {
    for (std::size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + value.size())) {
        text.replace(at, std::wcslen(key), value);
    }
    return text;
}

std::string phrases_path() { return user_data_path("phrases\\phrases.ini"); }

bool read_bytes(const std::wstring& path, std::string* bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes->assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// Whole, through a temporary file: the server never reads half of it.
bool write_phrases_file(const std::vector<CustomPhrase>& phrases) {
    const std::wstring path = utf8_to_wstr(phrases_path());
    CreateDirectoryW(utf8_to_wstr(user_data_path("phrases")).c_str(), nullptr);
    const std::wstring temporary = path + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "\xEF\xBB\xBF" << serialize_custom_phrases(phrases);
        if (!out) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// The text as Windows edit controls hold it (CRLF) and as phrases.ini does (LF).
std::wstring to_edit_text(const std::string& text) {
    std::wstring out;
    for (wchar_t ch : utf8_to_wstr(text)) {
        if (ch == L'\n') out += L"\r\n";
        else out += ch;
    }
    return out;
}

std::string from_edit_text(const std::wstring& text) {
    std::wstring out;
    for (wchar_t ch : text) {
        if (ch != L'\r') out += ch;
    }
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    return wstr_to_utf8(out);
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>((std::max)(0, length)) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>((std::max)(0, length)));
    return text;
}

// One line for the list: "dh  ·  1  ·  13901234567" (the first line of a longer phrase, "…").
std::wstring row_text(const CustomPhrase& phrase) {
    std::string text = phrase.text;
    const std::size_t line_end = text.find('\n');
    if (line_end != std::string::npos) text = text.substr(0, line_end) + "…";
    std::wstring shown = utf8_to_wstr(text);
    if (shown.size() > 40) shown = shown.substr(0, 40) + L"…";
    return utf8_to_wstr(phrase.code) + L"    " +
           filled(tr("phrases.position_n"), L"{n}", std::to_wstring(phrase.position)) + L"    " + shown;
}

HWND make_text_edit(int id, int x, int y, int width, int height, bool multiline, HWND parent) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    style |= multiline ? (ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL) : ES_AUTOHSCROLL;
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style, x, y, width, height, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandle(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return edit;
}

std::wstring pick_phrase_file(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(tr("phrases.pick_file"));
    const std::wstring all = tr("phrases.filter_all");
    const std::wstring any = tr("wordlists.filter_any");
    const COMDLG_FILTERSPEC filters[] = {{all.c_str(), L"*.ini;*.txt"}, {any.c_str(), L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    std::wstring path;
    IShellItem* result = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&result))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            path = name;
            CoTaskMemFree(name);
        }
        result->Release();
    }
    dialog->Release();
    return path;
}

// Rime's custom_phrase.txt (tab separated, no "code,place=" line) or else Sogou's phrases.ini.
std::vector<CustomPhrase> parse_any_phrase_file(const std::string& bytes) {
    const std::vector<CustomPhrase> sogou = parse_custom_phrases(bytes);
    const std::vector<CustomPhrase> rime = parse_rime_custom_phrases(bytes);
    return rime.size() > sogou.size() ? rime : sogou;
}

// As Microsoft Pinyin: letters (up to 32), not starting with u or v, which start U mode and
// V mode (docs/uv-mode.md).
bool dialog_code_ok(const std::string& code) {
    return is_valid_custom_phrase_code(code) && code[0] != 'u' && code[0] != 'v';
}

struct PhraseDialogState {
    EditorApp* app = nullptr;
    HWND code = nullptr;
    HWND position = nullptr;
    HWND text = nullptr;
    HWND ok = nullptr;
    int footer_top = 0;
    bool accepted = false;
    bool done = false;
};

std::string code_of(HWND edit) {
    std::wstring code = window_text(edit);
    for (wchar_t& ch : code) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return wstr_to_utf8(code);
}

void update_dialog_ok(const PhraseDialogState& state) {
    EnableWindow(state.ok, dialog_code_ok(code_of(state.code)) && !from_edit_text(window_text(state.text)).empty());
}

HWND make_dialog_label(const wchar_t* text, int x, int y, int width, HWND parent) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, x, y,
                                 width, S(kFontPt + 8), parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return label;
}

// Where to export, and as which format (1 Sogou's phrases.ini, 2 Rime's custom_phrase.txt).
std::wstring pick_export_file(HWND owner, UINT* format) {
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    dialog->SetTitle(tr("phrases.export_title"));
    const std::wstring sogou = tr("phrases.filter_sogou");
    const std::wstring rime = tr("phrases.filter_rime");
    const COMDLG_FILTERSPEC filters[] = {{sogou.c_str(), L"*.ini"}, {rime.c_str(), L"*.txt"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"ini");  // the chosen type's extension is added
    dialog->SetFileName(L"phrases.ini");
    std::wstring path;
    IShellItem* result = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&result))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            path = name;
            CoTaskMemFree(name);
        }
        result->Release();
        dialog->GetFileTypeIndex(format);
    }
    dialog->Release();
    return path;
}

bool write_bytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << bytes;
    return static_cast<bool>(out);
}

}  // namespace

void EditorApp::create_phrase_panel(HWND panel) {
    RECT client = {};
    GetClientRect(panel, &client);
    const int x0 = kPanelPadLeft;
    const int width = client.right - x0 * 2;

    int y = card_begin(panel, kPanelPadTop, tr("phrases.card"));
    hPhraseEnable_ = make_check(kPhraseEnableId, tr("phrases.enable"), x0, y, width, panel);
    y += kRowH;
    const int third = (width - S(20)) / 3;
    hPhraseDate_ = make_check(kPhraseDateId, tr("phrases.builtin_date"), x0 + S(20), y, third, panel);
    hPhraseTime_ = make_check(kPhraseTimeId, tr("phrases.builtin_time"), x0 + S(20) + third, y, third, panel);
    hPhraseWeek_ = make_check(kPhraseWeekId, tr("phrases.builtin_week"), x0 + S(20) + 2 * third, y, third, panel);
    y += kRowH + S(8);

    // Add, Import, Export, Clear, as Microsoft Pinyin: sharing the card's width, none wider than
    // S(110).
    const struct {
        int id;
        const char* key;
    } buttons[] = {{kPhraseAddId, "phrases.add"},
                   {kPhraseImportId, "phrases.import"},
                   {kPhraseExportId, "phrases.export"},
                   {kPhraseClearId, "phrases.clear"}};
    const int gap = S(8);
    const int count = static_cast<int>(std::size(buttons));
    const int button_width = (std::min)(S(110), (width - gap * (count - 1)) / count);
    int x = x0;
    for (const auto& button : buttons) {
        HWND made = make_page_button(button.id, tr(button.key), x, y, button_width, S(28), panel);
        if (button.id == kPhraseClearId || button.id == kPhraseExportId) EnableWindow(made, !phrases_.empty());
        x += button_width + gap;
    }
    y += S(28) + S(14);

    if (phrases_.empty()) {
        make_hint(tr(phrases_loaded_ ? "phrases.empty" : "phrases.loading"), x0, y, width, panel, 1);
        y += S(kFontPt + 6) + S(6);
    }
    const int remove_width = S(68);
    const int edit_width = S(68);
    const int remove_x = client.right - kPanelPadLeft - remove_width;
    const int edit_x = remove_x - S(8) - edit_width;
    for (int i = 0; i < static_cast<int>(phrases_.size()) && i < kMaxPhraseRows; ++i) {
        make_hint(row_text(phrases_[static_cast<std::size_t>(i)]).c_str(), x0, y + S(4), edit_x - x0 - S(8),
                  panel, 1);
        make_page_button(kPhraseEditFirstId + i, tr("phrases.edit"), edit_x, y - S(2), edit_width, S(28), panel);
        make_page_button(kPhraseRemoveFirstId + i, tr("phrases.remove"), remove_x, y - S(2), remove_width,
                         S(28), panel);
        y += kRowH;
    }
    if (static_cast<int>(phrases_.size()) > kMaxPhraseRows) {
        make_hint(filled(tr("phrases.more"), L"{count}", std::to_wstring(phrases_.size())).c_str(), x0, y,
                  width, panel, 2);
        y += S(2 * (kFontPt + 6));
    }
    card_end(panel, y);
    populate_phrase_switches();
}

void EditorApp::populate_phrase_switches() {
    if (!hPhraseEnable_) return;
    set_check(hPhraseEnable_, config_.phrases.enable);
    set_check(hPhraseDate_, config_.phrases.builtin_date);
    set_check(hPhraseTime_, config_.phrases.builtin_time);
    set_check(hPhraseWeek_, config_.phrases.builtin_week);
    const bool on = config_.phrases.enable;
    for (HWND box : {hPhraseDate_, hPhraseTime_, hPhraseWeek_}) EnableWindow(box, on);
}

void EditorApp::read_phrase_switches() {
    if (!hPhraseEnable_) return;
    config_.phrases.enable = get_check(hPhraseEnable_);
    config_.phrases.builtin_date = get_check(hPhraseDate_);
    config_.phrases.builtin_time = get_check(hPhraseTime_);
    config_.phrases.builtin_week = get_check(hPhraseWeek_);
}

void EditorApp::rebuild_phrase_panel() {
    const HWND panel = hPanels_[kPhrasePanel];
    if (!panel) return;
    read_phrase_switches();  // the switches are made again: keep what the page shows
    SendMessageW(panel, WM_SETREDRAW, FALSE, 0);
    const int scrolled = clear_panel(panel);
    hints_.erase(std::remove_if(hints_.begin(), hints_.end(), [](HWND h) { return !IsWindow(h); }),
                 hints_.end());
    create_phrase_panel(panel);
    update_panel_scroll(panel);
    scroll_panel(panel, scrolled);
    SendMessageW(panel, WM_SETREDRAW, TRUE, 0);
    apply_ui_theme(ui_colors().dark, true);  // the new controls
}

void EditorApp::load_phrases() {
    std::string bytes;
    phrases_ = read_bytes(utf8_to_wstr(phrases_path()), &bytes) ? parse_custom_phrases(bytes)
                                                                 : std::vector<CustomPhrase>();
    phrases_loaded_ = true;
    rebuild_phrase_panel();
}

bool EditorApp::save_phrases() {
    if (write_phrases_file(phrases_)) return true;
    MessageBoxW(hwnd_, tr("phrases.error_save"), tr("window.title"), MB_OK | MB_ICONERROR);
    return false;
}

void EditorApp::edit_phrase(int index) {
    const bool adding = index < 0 || index >= static_cast<int>(phrases_.size());
    if (adding && phrases_.size() >= kMaxCustomPhrases) return;
    CustomPhrase phrase = adding ? CustomPhrase() : phrases_[static_cast<std::size_t>(index)];
    if (!run_phrase_dialog(&phrase, adding)) return;
    std::vector<CustomPhrase> before = phrases_;
    if (adding) phrases_.push_back(std::move(phrase));
    else phrases_[static_cast<std::size_t>(index)] = std::move(phrase);
    if (!save_phrases()) {
        phrases_ = std::move(before);
        return;
    }
    PostMessageW(hwnd_, kRebuildPhrasesMessage, 0, 0);
}

// Microsoft Pinyin's Add a phrase dialog: the letters, the place on the candidate window, the
// phrase (one line; a phrase written on several lines in the file keeps its lines here). Modal.
bool EditorApp::run_phrase_dialog(CustomPhrase* phrase, bool adding) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = phrase_dialog_proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPhraseDialogClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    const bool multiline = phrase->text.find('\n') != std::string::npos;
    const int pad = S(24);
    const int width = S(440);
    const int field_width = width - 2 * pad;
    const int text_height = multiline ? S(4 * (kFontPt + 8)) : kCtrlH;
    const int footer_height = S(72);
    const int footer_top = pad + S(40) + 3 * (S(26) + S(14)) + 2 * kCtrlH + text_height + pad;
    const int height = footer_top + footer_height;
    RECT owner = {};
    GetWindowRect(hwnd_, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    PhraseDialogState state;
    state.app = this;
    state.footer_top = footer_top;
    const wchar_t* title = tr(adding ? "phrases.dialog_add" : "phrases.dialog_edit");
    HWND dialog = CreateWindowExW(WS_EX_CONTROLPARENT, kPhraseDialogClass, title, WS_POPUP | WS_CLIPCHILDREN,
                                  x, y, width, height, hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!dialog) return false;
    SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    const DWORD round = 2;  // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(dialog, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));

    int top = pad;
    HWND heading = CreateWindowExW(0, L"STATIC", title, WS_CHILD | WS_VISIBLE | SS_LEFT, pad, top, field_width,
                                   S(30), dialog, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(get_title_font()), TRUE);
    top += S(40);
    make_dialog_label(tr("phrases.code_label"), pad, top, field_width, dialog);
    top += S(26);
    state.code = make_text_edit(kDialogCodeId, pad, top, field_width, kCtrlH, false, dialog);
    SetWindowLongPtrW(state.code, GWL_STYLE, GetWindowLongPtrW(state.code, GWL_STYLE) | ES_LOWERCASE);
    SendMessageW(state.code, EM_SETLIMITTEXT, kMaxCustomPhraseCodeBytes, 0);
    top += kCtrlH + S(14);
    make_dialog_label(tr("phrases.position_label"), pad, top, field_width, dialog);
    top += S(26);
    state.position = make_combo(kDialogPositionId, pad, top, S(120), dialog);
    for (int i = 1; i <= kMaxCustomPhrasePosition; ++i) combo_add(state.position, std::to_wstring(i).c_str());
    top += kCtrlH + S(14);
    make_dialog_label(tr("phrases.text_label"), pad, top, field_width, dialog);
    top += S(26);
    state.text = make_text_edit(kDialogTextId, pad, top, field_width, text_height, multiline, dialog);
    const int button_width = (field_width - S(10)) / 2;
    state.ok = make_page_button(kDialogOkId, tr(adding ? "phrases.add" : "phrases.save"), pad,
                                footer_top + S(20), button_width, S(32), dialog);
    set_button_primary(state.ok, true);
    make_page_button(kDialogCancelId, tr("phrases.cancel"), pad + button_width + S(10), footer_top + S(20),
                     button_width, S(32), dialog);

    SetWindowTextW(state.code, utf8_to_wstr(phrase->code).c_str());
    combo_set_index(state.position, std::clamp(phrase->position, 1, kMaxCustomPhrasePosition) - 1);
    SetWindowTextW(state.text, to_edit_text(phrase->text).c_str());
    update_dialog_ok(state);
    style_popup(dialog);

    // Modal: the window waits while the dialog is up.
    EnableWindow(hwnd_, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(state.code);
    SendMessageW(state.code, EM_SETSEL, 0, -1);
    MSG msg;
    bool quit = false;
    while (!state.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            quit = true;
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (state.accepted) {
        phrase->code = code_of(state.code);
        phrase->position = std::clamp(combo_index(state.position) + 1, 1, kMaxCustomPhrasePosition);
        phrase->text = from_edit_text(window_text(state.text));
    }
    EnableWindow(hwnd_, TRUE);
    SetActiveWindow(hwnd_);
    DestroyWindow(dialog);
    if (quit) PostQuitMessage(static_cast<int>(msg.wParam));
    return state.accepted;
}

LRESULT CALLBACK EditorApp::phrase_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PhraseDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (state) {
            return state->app->control_colors(message, reinterpret_cast<HDC>(wparam),
                                              reinterpret_cast<HWND>(lparam));
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item->CtlType == ODT_BUTTON) {
            draw_button(*item);
            return TRUE;
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, window_brush());
        return 1;
    }
    case WM_PAINT: {
        // A line above the buttons, and the outline.
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT rect;
        GetClientRect(window, &rect);
        HPEN pen = CreatePen(PS_SOLID, 1, ui_colors().border);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (state) {
            MoveToEx(dc, 0, state->footer_top, nullptr);
            LineTo(dc, rect.right, state->footer_top);
        }
        Rectangle(dc, 0, 0, rect.right, rect.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        EndPaint(window, &paint);
        return 0;
    }
    case DM_GETDEFID:  // Enter in IsDialogMessage: Add / Save
        return MAKELRESULT(kDialogOkId, DC_HASDEFID);
    case WM_COMMAND: {
        if (!state) break;
        const int id = LOWORD(wparam);
        if ((id == kDialogCodeId || id == kDialogTextId) && HIWORD(wparam) == EN_CHANGE) {
            update_dialog_ok(*state);
        } else if (id == kDialogOkId || id == IDOK) {
            if (IsWindowEnabled(state->ok)) {
                state->accepted = true;
                state->done = true;
            }
        } else if (id == kDialogCancelId || id == IDCANCEL) {
            state->done = true;
        }
        return 0;
    }
    case WM_CLOSE:
        if (state) state->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void EditorApp::remove_phrase(std::size_t index) {
    if (index >= phrases_.size()) return;
    const std::wstring confirm = filled(tr("phrases.remove_confirm"), L"{phrase}", row_text(phrases_[index]));
    if (MessageBoxW(hwnd_, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
        return;
    }
    std::vector<CustomPhrase> before = phrases_;
    phrases_.erase(phrases_.begin() + static_cast<std::ptrdiff_t>(index));
    if (!save_phrases()) {
        phrases_ = std::move(before);
        return;
    }
    PostMessageW(hwnd_, kRebuildPhrasesMessage, 0, 0);
}

void EditorApp::import_phrases() {
    const std::wstring path = pick_phrase_file(hwnd_);
    if (path.empty()) return;
    std::string bytes;
    if (!read_bytes(path, &bytes)) {
        MessageBoxW(hwnd_, tr("wordlists.error_open"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    const std::vector<CustomPhrase> read = parse_any_phrase_file(bytes);
    if (read.empty()) {
        MessageBoxW(hwnd_, tr("phrases.error_format"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    std::vector<CustomPhrase> before = phrases_;
    std::size_t added = 0;
    for (const CustomPhrase& phrase : read) {
        const bool known = std::any_of(phrases_.begin(), phrases_.end(), [&](const CustomPhrase& p) {
            return p.code == phrase.code && p.text == phrase.text;
        });
        if (known || phrases_.size() >= kMaxCustomPhrases) continue;
        phrases_.push_back(phrase);
        ++added;
    }
    if (added > 0 && !save_phrases()) {
        phrases_ = std::move(before);
        return;
    }
    std::wstring message = filled(tr("phrases.imported"), L"{added}", std::to_wstring(added));
    message = filled(message, L"{read}", std::to_wstring(read.size()));
    MessageBoxW(hwnd_, message.c_str(), tr("window.title"), MB_OK | MB_ICONINFORMATION);
    PostMessageW(hwnd_, kRebuildPhrasesMessage, 0, 0);
}

// Sogou's phrases.ini in UTF-16 (as Sogou writes it, so Sogou reads it back), or Rime's
// custom_phrase.txt (single-line phrases without date variables).
void EditorApp::export_phrases() {
    if (phrases_.empty()) return;
    UINT format = 1;
    const std::wstring path = pick_export_file(hwnd_, &format);
    if (path.empty()) return;
    std::string bytes;
    std::size_t skipped = 0;
    if (format == 2) {
        bytes = serialize_rime_custom_phrases(phrases_, &skipped);
    } else {
        const std::wstring text = utf8_to_wstr(serialize_custom_phrases(phrases_));
        bytes = "\xFF\xFE";
        bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    }
    if (!write_bytes(path, bytes)) {
        MessageBoxW(hwnd_, tr("phrases.error_export"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring message = filled(tr(skipped ? "phrases.exported_skipped" : "phrases.exported"), L"{count}",
                                  std::to_wstring(phrases_.size() - skipped));
    message = filled(message, L"{skipped}", std::to_wstring(skipped));
    MessageBoxW(hwnd_, message.c_str(), tr("window.title"), MB_OK | MB_ICONINFORMATION);
}

void EditorApp::clear_phrases() {
    if (phrases_.empty()) return;
    const std::wstring confirm = filled(tr("phrases.clear_confirm"), L"{count}", std::to_wstring(phrases_.size()));
    if (MessageBoxW(hwnd_, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
    std::vector<CustomPhrase> before = std::move(phrases_);
    phrases_.clear();
    if (!save_phrases()) {
        phrases_ = std::move(before);
        return;
    }
    PostMessageW(hwnd_, kRebuildPhrasesMessage, 0, 0);
}

bool EditorApp::handle_phrase_command(int control_id, int notification) {
    switch (control_id) {
    case kPhraseEnableId:
        if (notification == BN_CLICKED) {
            const bool on = get_check(hPhraseEnable_);
            for (HWND box : {hPhraseDate_, hPhraseTime_, hPhraseWeek_}) EnableWindow(box, on);
        }
        return true;
    case kPhraseDateId:
    case kPhraseTimeId:
    case kPhraseWeekId:
        return true;
    case kPhraseAddId:
        if (notification == BN_CLICKED) edit_phrase(-1);
        return true;
    case kPhraseImportId:
        if (notification == BN_CLICKED) import_phrases();
        return true;
    case kPhraseExportId:
        if (notification == BN_CLICKED) export_phrases();
        return true;
    case kPhraseClearId:
        if (notification == BN_CLICKED) clear_phrases();
        return true;
    default:
        break;
    }
    if (control_id >= kPhraseEditFirstId && control_id < kPhraseEditFirstId + kMaxPhraseRows) {
        if (notification == BN_CLICKED) edit_phrase(control_id - kPhraseEditFirstId);
        return true;
    }
    if (control_id >= kPhraseRemoveFirstId && control_id < kPhraseRemoveFirstId + kMaxPhraseRows) {
        if (notification == BN_CLICKED) remove_phrase(static_cast<std::size_t>(control_id - kPhraseRemoveFirstId));
        return true;
    }
    return false;
}

}  // namespace settings
}  // namespace cxxime
