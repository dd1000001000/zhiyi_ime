// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// My words (我的词表, issue #47; docs/settings-guide.md), on the Word lists page: words typed into
// a box instead of imported from a file. The Add words dialog takes up to 100 lines (a Chinese
// word, its pinyin optional; an English word or phrase) and shows, as one types, what the server
// makes of each line: its reading and Wubi code, new, known or why it cannot be used. The Edit
// dialog lists My words a page at a time, to search, change a Chinese word's reading or remove
// words. The server keeps them as the lists my-zh and my-en (server/src/word_list_service.cc);
// the page shows them as one row. Changes take effect at once, without Apply.

#include "editor_app.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include <commctrl.h>
#include <dwmapi.h>

#include <cxxime/word_list.h>
#include <cxxime/word_list_control.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {
namespace {

constexpr wchar_t kMyWordsDialogClass[] = L"ZhiyiMyWordsDialog";
constexpr int kWordsTimeoutMs = 8000;
// The Add words dialog.
constexpr int kAddInputId = 6700;
constexpr int kAddPreviewId = 6701;
constexpr int kAddSummaryId = 6702;
constexpr int kAddOkId = 6703;
constexpr int kAddCancelId = 6704;
// The Edit My words dialog.
constexpr int kEditSearchId = 6710;
constexpr int kEditListId = 6711;
constexpr int kEditPreviousId = 6712;
constexpr int kEditNextId = 6713;
constexpr int kEditPageId = 6714;
constexpr int kEditReadingId = 6715;
constexpr int kEditSetReadingId = 6716;
constexpr int kEditRemoveId = 6717;
// Both dialogs: the × at the top right.
constexpr int kCloseId = 6720;
constexpr wchar_t kCloseGlyph[] = L"\xE8BB";  // ChromeClose (Segoe Fluent Icons / MDL2 Assets)
constexpr UINT_PTR kPreviewTimer = 1;  // the box changed: a preview after a pause in typing
constexpr UINT_PTR kSearchTimer = 2;   // the search changed
constexpr UINT kPreviewDelayMs = 400;
constexpr UINT kSearchDelayMs = 300;
constexpr std::size_t kEditPageSize = 100;

std::wstring fill(std::wstring text, std::initializer_list<std::pair<const wchar_t*, std::wstring>> values) {
    for (const auto& [key, value] : values) {
        const std::size_t length = std::wcslen(key);
        for (std::size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + value.size())) {
            text.replace(at, length, value);
        }
    }
    return text;
}

std::wstring number(std::uint64_t value) { return std::to_wstring(value); }

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>((std::max)(0, length)) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>((std::max)(0, length)));
    return text;
}

// The box's text as the server takes it: LF line ends.
std::string box_text(HWND edit) {
    std::wstring out;
    for (wchar_t ch : window_text(edit)) {
        if (ch != L'\r') out += ch;
    }
    return wstr_to_utf8(out);
}

// Lines that are words (not empty, not a # comment): what the limit of 100 counts.
std::size_t word_line_count(const std::string& text) {
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::size_t a = start;
        while (a < end && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) ++a;
        if (a < end && text[a] != '#') ++count;
        start = end + 1;
    }
    return count;
}

HWND make_text_edit(int id, int x, int y, int width, int height, bool multiline, bool read_only, HWND parent) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    style |= multiline ? (ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL) : ES_AUTOHSCROLL;
    if (read_only) style |= ES_READONLY;
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style, x, y, width, height, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr),
                                nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return edit;
}

HWND make_text(int id, const wchar_t* text, int x, int y, int width, int height, HWND parent) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, x, y, width,
                                 height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandle(nullptr), nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return label;
}

std::wstring problem_text(const std::string& problem) {
    for (const char* known : {"not_pinyin", "syllable_count", "no_reading", "extra_text", "mixed", "not_text",
                              "too_long"}) {
        if (problem == known) return tr(("words.problem_" + problem).c_str());
    }
    return tr("words.problem_not_text");
}

std::wstring status_text(const MyWordLineInfo& line) {
    if (line.status == "added") {
        // Half known: only the Wubi code (or only the reading) goes in.
        if (line.pinyin_known) return tr("words.status_added_wubi");
        if (line.wubi_known) return tr("words.status_added_pinyin");
        return tr("words.status_added");
    }
    if (line.status == "existing") return tr("words.status_existing");
    if (line.status == "in_list") return tr("words.status_in_list");
    if (line.status == "repeated") return tr("words.status_repeated");
    return fill(tr("words.status_invalid"), {{L"{problem}", problem_text(line.problem)}});
}

// "zhang san feng（推算）· 五笔 xddd"
std::wstring reading_text(const std::string& syllables, bool guessed, const std::string& wubi) {
    std::wstring text;
    if (!syllables.empty()) {
        text = utf8_to_wstr(syllables);
        if (guessed) text += tr("words.guessed");
        text += L" · ";
    }
    text += wubi.empty() ? std::wstring(tr("words.wubi_none"))
                         : fill(tr("words.wubi"), {{L"{code}", utf8_to_wstr(wubi)}});
    return text;
}

// A preview row: "3. 行长    hang zhang · 五笔 tvta    新增".
std::wstring preview_row(const MyWordLineInfo& line) {
    std::wstring row = number(line.line) + L". " + utf8_to_wstr(line.text) + L"    ";
    if (line.status == "invalid") return row + status_text(line);
    if (line.language == "en") {
        row += fill(tr("words.typed_as"), {{L"{code}", utf8_to_wstr(line.code)}});
    } else {
        row += reading_text(line.syllables, line.guessed, line.wubi);
    }
    return row + L"    " + status_text(line);
}

// A row of the Edit dialog's list: "行长    hang zhang · 五笔 tvta" / "machine learning    英文".
std::wstring word_row(const MyWordInfo& word) {
    std::wstring row = utf8_to_wstr(word.text) + L"    ";
    if (word.language == "en") return row + tr("words.english");
    row += reading_text(word.syllables, word.guessed, word.wubi);
    // No reading of its own: the system dictionary types it with pinyin already.
    if (word.syllables.empty()) row += std::wstring(L" · ") + tr("words.pinyin_in_dictionary");
    return row;
}

struct MyWordsDialog {
    EditorApp* app = nullptr;
    bool editing = false;
    HWND window = nullptr;
    int footer_top = 0;
    bool done = false;
    // Add words.
    HWND input = nullptr;
    HWND preview = nullptr;
    HWND summary = nullptr;
    HWND ok = nullptr;
    bool preview_current = false;  // the preview is of the box as it is
    std::size_t added = 0;         // lines the preview found new
    // Edit My words.
    HWND search = nullptr;
    HWND list = nullptr;
    HWND previous = nullptr;
    HWND next = nullptr;
    HWND page = nullptr;
    HWND reading = nullptr;
    HWND set_reading = nullptr;
    HWND remove = nullptr;
    std::vector<MyWordInfo> words;  // the page shown
    std::size_t offset = 0;
    std::uint64_t total = 0;
    std::vector<WordListInfo> lists;  // after the last change
    bool changed = false;
};

void show_summary(MyWordsDialog& d, const std::wstring& text, const std::wstring& preview) {
    SetWindowTextW(d.summary, text.c_str());
    SetWindowTextW(d.preview, preview.c_str());
}

// Asks the server what it makes of the box (nothing is kept).
void run_preview(MyWordsDialog& d) {
    KillTimer(d.window, kPreviewTimer);
    d.preview_current = true;
    d.added = 0;
    EnableWindow(d.ok, FALSE);
    const std::string text = box_text(d.input);
    const std::size_t lines = word_line_count(text);
    if (lines == 0) {
        show_summary(d, tr("words.preview_empty"), L"");
        return;
    }
    if (lines > kMaxMyWordLines || text.size() > kMaxMyWordTextBytes) {
        show_summary(d,
                     fill(tr("words.too_many_lines"),
                          {{L"{max}", number(kMaxMyWordLines)}, {L"{count}", number(lines)}}),
                     L"");
        return;
    }
    WordListControlResult result;
    bool read = false;
    {
        WaitCursor wait;
        read = WordListControlClient(kWordsTimeoutMs).preview_words(text, &result);
    }
    if (!read) {
        show_summary(d,
                     result.error_code == kWordListErrorTooManyLines
                         ? fill(tr("words.too_many_lines"),
                                {{L"{max}", number(kMaxMyWordLines)}, {L"{count}", number(lines)}})
                         : std::wstring(tr("words.error_load")),
                     L"");
        return;
    }
    std::size_t zh = 0, en = 0, known = 0, invalid = 0;
    std::wstring rows;
    for (const MyWordLineInfo& line : result.lines) {
        if (line.status == "added") {
            ++(line.language == "en" ? en : zh);
        } else if (line.status == "invalid") {
            ++invalid;
        } else {
            ++known;
        }
        if (!rows.empty()) rows += L"\r\n";
        rows += preview_row(line);
    }
    d.added = zh + en;
    show_summary(d,
                 fill(tr("words.summary"), {{L"{added}", number(d.added)},
                                            {L"{zh}", number(zh)},
                                            {L"{en}", number(en)},
                                            {L"{known}", number(known)},
                                            {L"{invalid}", number(invalid)}}),
                 rows);
    EnableWindow(d.ok, d.added > 0);
}

// Add: the new words go into My words; the dialog closes when they did.
void add_from_box(MyWordsDialog& d) {
    if (!d.preview_current) run_preview(d);
    if (d.added == 0) return;
    const std::string text = box_text(d.input);
    WordListControlResult result;
    bool added = false;
    {
        WaitCursor wait;
        added = WordListControlClient(kWordsTimeoutMs).add_words(text, &result);
    }
    if (!added) {
        const wchar_t* message = result.error_code == kWordListErrorNothingNew ? tr("words.nothing_new")
                                 : result.error_code == kWordListErrorStorage  ? tr("wordlists.error_storage")
                                                                               : tr("words.error_add");
        MessageBoxW(d.window, message, tr("window.title"), MB_OK | MB_ICONERROR);
        d.preview_current = false;
        run_preview(d);
        return;
    }
    d.lists = std::move(result.lists);
    d.changed = true;
    std::size_t new_words = 0;
    for (const MyWordLineInfo& line : result.lines) new_words += line.status == "added" ? 1 : 0;
    const std::size_t skipped = result.lines.size() - new_words;
    if (skipped > 0) {
        MessageBoxW(d.window,
                    fill(tr("words.added_partial"), {{L"{added}", number(new_words)}, {L"{skipped}", number(skipped)}})
                        .c_str(),
                    tr("window.title"), MB_OK | MB_ICONINFORMATION);
    }
    d.done = true;
}

std::vector<int> selected_rows(const MyWordsDialog& d) {
    const int count = static_cast<int>(SendMessageW(d.list, LB_GETSELCOUNT, 0, 0));
    std::vector<int> rows(static_cast<std::size_t>((std::max)(0, count)));
    if (count > 0) SendMessageW(d.list, LB_GETSELITEMS, count, reinterpret_cast<LPARAM>(rows.data()));
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](int row) { return row < 0 || row >= static_cast<int>(d.words.size()); }),
               rows.end());
    return rows;
}

// The reading box and buttons follow the selection: a reading for one Chinese word.
void update_selection(MyWordsDialog& d) {
    const std::vector<int> rows = selected_rows(d);
    const bool one_chinese = rows.size() == 1 && d.words[static_cast<std::size_t>(rows[0])].language == "zh";
    SetWindowTextW(d.reading, one_chinese ? utf8_to_wstr(d.words[static_cast<std::size_t>(rows[0])].syllables).c_str()
                                          : L"");
    EnableWindow(d.reading, one_chinese);
    EnableWindow(d.set_reading, one_chinese);
    EnableWindow(d.remove, !rows.empty());
}

// The page at `offset` of the words the search finds.
void load_page(MyWordsDialog& d) {
    KillTimer(d.window, kSearchTimer);
    const std::string query = edit_text_utf8(d.search);
    WordListControlResult result;
    bool got = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            WaitCursor wait;
            got = WordListControlClient(kWordsTimeoutMs).query_words(query, d.offset, kEditPageSize, &result);
        }
        // Past the end (the last words of the last page removed): the last page.
        if (got && result.total > 0 && d.offset >= result.total) {
            d.offset = static_cast<std::size_t>((result.total - 1) / kEditPageSize * kEditPageSize);
            continue;
        }
        break;
    }
    if (!got) {
        MessageBoxW(d.window, tr("words.error_load"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    d.words = std::move(result.my_words);
    d.total = result.total;
    SendMessageW(d.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(d.list, LB_RESETCONTENT, 0, 0);
    for (const MyWordInfo& word : d.words) {
        SendMessageW(d.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(word_row(word).c_str()));
    }
    SendMessageW(d.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d.list, nullptr, TRUE);
    const std::wstring page =
        d.total == 0 ? std::wstring(tr("words.page_empty"))
                     : fill(tr("words.page"), {{L"{from}", number(d.offset + 1)},
                                               {L"{to}", number(d.offset + d.words.size())},
                                               {L"{total}", number(d.total)}});
    SetWindowTextW(d.page, page.c_str());
    EnableWindow(d.previous, d.offset > 0);
    EnableWindow(d.next, d.offset + d.words.size() < d.total);
    update_selection(d);
}

void change_reading(MyWordsDialog& d) {
    const std::vector<int> rows = selected_rows(d);
    if (rows.size() != 1) return;
    const MyWordInfo word = d.words[static_cast<std::size_t>(rows[0])];
    const std::string pinyin = edit_text_utf8(d.reading);
    if (pinyin.empty() || pinyin == word.syllables) return;
    WordListControlResult result;
    bool changed = false;
    {
        WaitCursor wait;
        changed = WordListControlClient(kWordsTimeoutMs).set_reading(word, pinyin, &result);
    }
    if (!changed) {
        const std::wstring message =
            result.error_code == kWordListErrorReading
                ? fill(tr("words.reading_bad"), {{L"{problem}", problem_text(result.problem)}})
            : result.error_code == kWordListErrorReadingKnown ? std::wstring(tr("words.reading_known"))
                                                              : std::wstring(tr("words.error_change"));
        MessageBoxW(d.window, message.c_str(), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    d.lists = std::move(result.lists);
    d.changed = true;
    load_page(d);
}

void remove_selected(MyWordsDialog& d) {
    const std::vector<int> rows = selected_rows(d);
    if (rows.empty()) return;
    const std::wstring confirm = fill(tr("words.remove_confirm"), {{L"{count}", number(rows.size())}});
    if (MessageBoxW(d.window, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
    std::vector<MyWordInfo> words;
    for (int row : rows) words.push_back(d.words[static_cast<std::size_t>(row)]);
    WordListControlResult result;
    bool removed = false;
    {
        WaitCursor wait;
        removed = WordListControlClient(kWordsTimeoutMs).remove_words(words, &result);
    }
    if (!removed) {
        MessageBoxW(d.window, tr("words.error_change"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    d.lists = std::move(result.lists);
    d.changed = true;
    load_page(d);
}

// A modal window of this file's class: shown over the settings window until `done`.
void run_modal(MyWordsDialog& d, HWND owner, HWND first_focus) {
    style_popup(d.window);
    EnableWindow(owner, FALSE);
    ShowWindow(d.window, SW_SHOW);
    SetFocus(first_focus);
    MSG msg;
    bool quit = false;
    while (!d.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            quit = true;
            break;
        }
        if (!IsDialogMessageW(d.window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(d.window);
    if (quit) PostQuitMessage(static_cast<int>(msg.wParam));
}

HWND make_dialog_window(MyWordsDialog& d, HWND owner, const wchar_t* title, int width, int height,
                        WNDPROC proc) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kMyWordsDialogClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    RECT rect = {};
    GetWindowRect(owner, &rect);
    const int x = rect.left + ((rect.right - rect.left) - width) / 2;
    const int y = rect.top + ((rect.bottom - rect.top) - height) / 2;
    d.window = CreateWindowExW(WS_EX_CONTROLPARENT, kMyWordsDialogClass, title, WS_POPUP | WS_CLIPCHILDREN, x,
                               (std::max)(static_cast<int>(rect.top), y), width, height, owner, nullptr,
                               GetModuleHandle(nullptr), nullptr);
    if (!d.window) return nullptr;
    SetWindowLongPtrW(d.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    const DWORD round = 2;  // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(d.window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    return d.window;
}

// The title, and the × that closes the dialog at the top right.
int make_heading(const wchar_t* title, int pad, int dialog_width, HWND window) {
    const int close = S(36);
    HWND heading = CreateWindowExW(0, L"STATIC", title, WS_CHILD | WS_VISIBLE | SS_LEFT, pad, pad,
                                   dialog_width - 2 * pad - close, S(30), window, nullptr, GetModuleHandle(nullptr),
                                   nullptr);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(get_title_font()), TRUE);
    HWND x = make_page_button(kCloseId, kCloseGlyph, dialog_width - S(12) - close, S(12), close, S(32), window);
    SendMessageW(x, WM_SETFONT, reinterpret_cast<WPARAM>(get_icon_font(kFontPt - 4)), TRUE);
    return pad + S(40);
}

// The × drawn without a frame: the glyph, on a tint while pressed.
void draw_close(const DRAWITEMSTRUCT& item) {
    const UiColors& colors = ui_colors();
    RECT rect = item.rcItem;
    FillRect(item.hDC, &rect, window_brush());
    if (item.itemState & ODS_SELECTED) fill_round_rect(item.hDC, rect, S(4), colors.button_hover, colors.button_hover);
    HGDIOBJ old_font = SelectObject(item.hDC, get_icon_font(kFontPt - 4));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, colors.text);
    DrawTextW(item.hDC, kCloseGlyph, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    SelectObject(item.hDC, old_font);
}

// Pasted text with LF line ends (from a web page, a Unix file): an edit box shows CRLF lines only.
LRESULT CALLBACK paste_lines_proc(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_PASTE && IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(edit)) {
        std::wstring text;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
                text = chars;
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        std::wstring lines;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\r') {
                lines += L"\r\n";
                if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            } else if (text[i] == L'\n') {
                lines += L"\r\n";
            } else {
                lines += text[i];
            }
        }
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(lines.c_str()));
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, paste_lines_proc, 1);
    return DefSubclassProc(edit, message, wparam, lparam);
}

}  // namespace

void EditorApp::add_my_words() {
    if (!ensure_server_running()) return;
    MyWordsDialog d;
    d.app = this;
    const int pad = S(24);
    const int width = S(620);
    const int field = width - 2 * pad;
    const int line_h = S(kFontPt + 8);
    const int hint_h = 3 * line_h;
    const int input_h = S(150);
    const int preview_h = S(170);
    const int summary_h = 2 * line_h;
    d.footer_top = pad + S(40) + hint_h + S(8) + input_h + S(12) + S(26) + preview_h + S(8) + summary_h + pad;
    const int height = d.footer_top + S(72);
    const wchar_t* title = tr("words.dialog_add");
    if (!make_dialog_window(d, hwnd_, title, width, height, my_words_dialog_proc)) return;

    int top = make_heading(title, pad, width, d.window);
    make_text(0, fill(tr("words.add_hint"), {{L"{max}", number(kMaxMyWordLines)}}).c_str(), pad, top, field,
              hint_h, d.window);
    top += hint_h + S(8);
    d.input = make_text_edit(kAddInputId, pad, top, field, input_h, true, false, d.window);
    SetWindowSubclass(d.input, paste_lines_proc, 1, 0);
    top += input_h + S(12);
    make_text(0, tr("words.preview_label"), pad, top, field, S(24), d.window);
    top += S(26);
    d.preview = make_text_edit(kAddPreviewId, pad, top, field, preview_h, true, true, d.window);
    top += preview_h + S(8);
    d.summary = make_text(kAddSummaryId, tr("words.preview_empty"), pad, top, field, summary_h, d.window);
    const int button_width = (field - S(10)) / 2;
    d.ok = make_page_button(kAddOkId, tr("words.add"), pad, d.footer_top + S(20), button_width, S(32), d.window);
    set_button_primary(d.ok, true);
    EnableWindow(d.ok, FALSE);
    make_page_button(kAddCancelId, tr("words.cancel"), pad + button_width + S(10), d.footer_top + S(20),
                     button_width, S(32), d.window);
    d.preview_current = true;  // empty: nothing to look at

    run_modal(d, hwnd_, d.input);
    if (d.changed) {
        word_lists_ = std::move(d.lists);
        word_lists_loaded_ = true;
        PostMessageW(hwnd_, kRebuildWordListsMessage, 0, 0);
    }
}

void EditorApp::edit_my_words() {
    if (!ensure_server_running()) return;
    MyWordsDialog d;
    d.app = this;
    d.editing = true;
    const int pad = S(24);
    const int width = S(640);
    const int field = width - 2 * pad;
    const int list_h = S(300);
    // No footer: the × at the top right closes it.
    d.footer_top = -1;
    const int height = pad + S(40) + S(26) + kCtrlH + S(10) + list_h + S(8) + S(32) + S(12) + S(32) + pad;
    const wchar_t* title = tr("words.dialog_edit");
    if (!make_dialog_window(d, hwnd_, title, width, height, my_words_dialog_proc)) return;

    int top = make_heading(title, pad, width, d.window);
    make_text(0, tr("words.search"), pad, top, field, S(24), d.window);
    top += S(26);
    d.search = make_text_edit(kEditSearchId, pad, top, field, kCtrlH, false, false, d.window);
    top += kCtrlH + S(10);
    d.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL |
                                 LBS_NOINTEGRALHEIGHT,
                             pad, top, field, list_h, d.window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditListId)), GetModuleHandle(nullptr),
                             nullptr);
    SendMessageW(d.list, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    top += list_h + S(8);
    d.previous = make_page_button(kEditPreviousId, tr("words.previous"), pad, top, S(96), S(32), d.window);
    d.next = make_page_button(kEditNextId, tr("words.next"), pad + S(104), top, S(96), S(32), d.window);
    d.page = make_text(kEditPageId, L"", pad + S(212), top + S(6), field - S(212), S(24), d.window);
    top += S(32) + S(12);
    const int reading_label = S(64);
    const int remove_width = S(130);
    const int set_width = S(110);
    make_text(0, tr("words.reading"), pad, top + S(6), reading_label, S(24), d.window);
    const int reading_width = field - reading_label - set_width - remove_width - S(24);
    d.reading = make_text_edit(kEditReadingId, pad + reading_label, top + (S(32) - kCtrlH) / 2, reading_width,
                               kCtrlH, false, false, d.window);
    d.set_reading = make_page_button(kEditSetReadingId, tr("words.set_reading"),
                                     pad + reading_label + reading_width + S(8), top, set_width, S(32), d.window);
    d.remove = make_page_button(kEditRemoveId, tr("words.remove_selected"), pad + field - remove_width, top,
                                remove_width, S(32), d.window);

    load_page(d);
    run_modal(d, hwnd_, d.search);
    if (d.changed) {
        word_lists_ = std::move(d.lists);
        word_lists_loaded_ = true;
        PostMessageW(hwnd_, kRebuildWordListsMessage, 0, 0);
    }
}

LRESULT CALLBACK EditorApp::my_words_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* d = reinterpret_cast<MyWordsDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (d) return d->app->control_colors(message, reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item->CtlType == ODT_BUTTON) {
            if (item->CtlID == kCloseId) {
                draw_close(*item);
            } else {
                draw_button(*item);
            }
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
        if (d && d->footer_top >= 0) {
            MoveToEx(dc, 0, d->footer_top, nullptr);
            LineTo(dc, rect.right, d->footer_top);
        }
        Rectangle(dc, 0, 0, rect.right, rect.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (!d) break;
        if (wparam == kPreviewTimer) run_preview(*d);
        if (wparam == kSearchTimer) {
            d->offset = 0;
            load_page(*d);
        }
        return 0;
    case WM_COMMAND: {
        if (!d) break;
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        switch (id) {
        case kAddInputId:
            if (code == EN_CHANGE) {
                d->preview_current = false;
                SetTimer(window, kPreviewTimer, kPreviewDelayMs, nullptr);
            }
            break;
        case kAddOkId:
            if (code == BN_CLICKED) add_from_box(*d);
            break;
        case kEditSearchId:
            if (code == EN_CHANGE) SetTimer(window, kSearchTimer, kSearchDelayMs, nullptr);
            break;
        case kEditListId:
            if (code == LBN_SELCHANGE) update_selection(*d);
            break;
        case kEditPreviousId:
            if (code == BN_CLICKED && d->offset > 0) {
                d->offset = d->offset >= kEditPageSize ? d->offset - kEditPageSize : 0;
                load_page(*d);
            }
            break;
        case kEditNextId:
            if (code == BN_CLICKED && d->offset + d->words.size() < d->total) {
                d->offset += kEditPageSize;
                load_page(*d);
            }
            break;
        case kEditSetReadingId:
            if (code == BN_CLICKED) change_reading(*d);
            break;
        case kEditRemoveId:
            if (code == BN_CLICKED) remove_selected(*d);
            break;
        case kAddCancelId:
        case kCloseId:
        case IDCANCEL:
            d->done = true;
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_CLOSE:
        if (d) d->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void EditorApp::set_my_words_enabled(bool enabled) {
    WordListControlResult result;
    bool changed = ensure_server_running();
    if (changed) {
        WaitCursor wait;  // the dictionaries are rebuilt
        const WordListControlClient client(kWordsTimeoutMs);
        for (const WordListInfo& list : std::vector<WordListInfo>(word_lists_)) {
            if (!is_my_word_list_id(list.id)) continue;
            changed = changed && client.set_enabled(list.id, enabled, &result);
        }
    }
    if (!changed) {
        MessageBoxW(hwnd_,
                    result.error_code == kWordListErrorStorage ? tr("wordlists.error_storage")
                                                               : tr("wordlists.error_change"),
                    tr("window.title"), MB_OK | MB_ICONERROR);
        load_word_lists();  // the row shows what the server keeps
        return;
    }
    word_lists_ = std::move(result.lists);
}

void EditorApp::remove_my_words() {
    std::uint64_t count = 0;
    for (const WordListInfo& list : word_lists_) {
        if (is_my_word_list_id(list.id)) count += list.added;
    }
    const std::wstring confirm = fill(tr("wordlists.mine_remove_confirm"), {{L"{count}", number(count)}});
    if (MessageBoxW(hwnd_, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
    if (!ensure_server_running()) return;
    WordListControlResult result;
    bool removed = true;
    {
        WaitCursor wait;
        const WordListControlClient client(kWordsTimeoutMs);
        for (const WordListInfo& list : std::vector<WordListInfo>(word_lists_)) {
            if (!is_my_word_list_id(list.id)) continue;
            removed = client.remove(list.id, &result) && removed;
        }
    }
    if (!removed) {
        MessageBoxW(hwnd_,
                    result.error_code == kWordListErrorStorage ? tr("wordlists.error_storage")
                                                               : tr("wordlists.error_change"),
                    tr("window.title"), MB_OK | MB_ICONERROR);
        load_word_lists();
        return;
    }
    word_lists_ = std::move(result.lists);
    // Not from inside the click: the button pressed is one of the controls made again.
    PostMessageW(hwnd_, kRebuildWordListsMessage, 0, 0);
}

}  // namespace settings
}  // namespace cxxime
