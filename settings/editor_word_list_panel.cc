// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The Word lists page (docs/settings-guide.md): the dictionaries the user imported (Sogou cell
// dictionaries, Rime dictionaries, pinyin text exports, English word lists), each enabled or
// disabled with its box, removed with its button. The server reads, keeps and applies them
// (server/src/word_list_service.cc); a change takes effect at once, without Apply.

#include "editor_app.h"

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <string>

#include <shobjidl.h>

#include <cxxime/word_list.h>
#include <cxxime/word_list_control.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kWordListImportId = 5030;
constexpr int kWordListCheckFirstId = 5100;   // one per list
constexpr int kWordListViewFirstId = 5300;    // "readings": the pinyin worked out
constexpr int kWordListRemoveFirstId = 5500;
// Two languages of kMaxWordListsPerLanguage lists each.
constexpr int kMaxWordListRows = static_cast<int>(2 * kMaxWordListsPerLanguage);
constexpr int kWordListTimeoutMs = 5000;
constexpr std::size_t kShownGuessedWords = 40;

std::wstring fill(std::wstring text, std::initializer_list<std::pair<const wchar_t*, std::wstring>> values) {
    for (const auto& [key, value] : values) {
        const std::size_t length = std::wcslen(key);
        for (std::size_t at = text.find(key); at != std::wstring::npos;
             at = text.find(key, at + value.size())) {
            text.replace(at, length, value);
        }
    }
    return text;
}

std::wstring number(std::uint64_t value) { return std::to_wstring(value); }

const wchar_t* format_text(const std::string& format) {
    if (format == "scel") return tr("wordlists.format_scel");
    if (format == "rime") return tr("wordlists.format_rime");
    if (format == "english_text") return tr("wordlists.format_english_text");
    return tr("wordlists.format_pinyin_text");
}

std::wstring detail_text(const WordListInfo& list) {
    const bool english = list.language == "en";
    std::wstring text = fill(tr(english ? "wordlists.detail_en" : "wordlists.detail_zh"),
                             {{L"{added}", number(list.added)},
                              {L"{existing}", number(list.existing)},
                              {L"{wubi}", number(list.wubi)},
                              {L"{time}", utf8_to_wstr(list.imported_at)}});
    if (!english && list.guessed > 0) {
        text += fill(tr("wordlists.detail_guessed"), {{L"{guessed}", number(list.guessed)}});
    }
    return text;
}

// Why a file was not imported (or the lists not changed).
std::wstring import_error_text(const WordListControlResult& result) {
    switch (result.error_code) {
    case kWordListErrorDuplicate:
        return fill(tr("wordlists.error_duplicate"), {{L"{name}", utf8_to_wstr(result.existing_name)}});
    case kWordListErrorTooMany:
        return fill(tr(result.list.language == "en" ? "wordlists.error_too_many_en"
                                                    : "wordlists.error_too_many_zh"),
                    {{L"{count}", number(kMaxWordListsPerLanguage)}});
    case kWordListErrorStorage:
        return tr("wordlists.error_storage");
    case kWordListErrorExtension:
        return tr("wordlists.error_extension");
    case kWordListErrorFormat:
        return tr("wordlists.error_format");
    case kWordListErrorNoWords:
        return tr("wordlists.error_no_words");
    case kWordListErrorNoPinyin:
        return tr("wordlists.error_no_pinyin");
    case kWordListErrorTooLarge:
        return tr("wordlists.error_too_large");
    case kWordListErrorNothingNew:
        return tr("wordlists.error_nothing_new");
    case ERROR_OPEN_FAILED:
        return tr("wordlists.error_open");
    default:
        return tr("wordlists.error_failed");
    }
}

std::wstring pick_word_list_file(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(tr("wordlists.pick_file"));
    // One choice per format, all of them first (shown at the start); "all files" too, whose
    // other files are refused when picked (import_word_list).
    const std::wstring all = tr("wordlists.filter_all");
    const std::wstring scel = tr("wordlists.filter_scel");
    const std::wstring rime = tr("wordlists.filter_rime");
    const std::wstring text = tr("wordlists.filter_text");
    const std::wstring any = tr("wordlists.filter_any");
    const COMDLG_FILTERSPEC filters[] = {
        {all.c_str(), L"*.scel;*.dict.yaml;*.yaml;*.yml;*.txt;*.csv;*.tsv"},
        {scel.c_str(), L"*.scel"},
        {rime.c_str(), L"*.dict.yaml;*.yaml;*.yml"},
        {text.c_str(), L"*.txt;*.csv;*.tsv"},
        {any.c_str(), L"*.*"},
    };
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

}  // namespace

void EditorApp::create_word_list_panel(HWND panel) {
    RECT client = {};
    GetClientRect(panel, &client);
    const int x0 = kPanelPadLeft;
    const int width = client.right - x0 * 2;
    const int card_top = kPanelPadTop;
    int y = card_begin(panel, card_top, tr("wordlists.card"));
    make_page_button(kWordListImportId, tr("wordlists.import"),
                     client.right - kPanelPadLeft - S(130), card_top + S(10), S(130), S(28), panel);
    make_hint(tr("wordlists.hint"), x0, y, width, panel, 4);
    y += S(4 * (kFontPt + 6)) + S(10);

    if (word_lists_.empty()) {
        make_hint(tr(word_lists_loaded_ ? "wordlists.empty" : "wordlists.loading"), x0, y, width,
                  panel, 1);
        y += S(kFontPt + 6) + S(6);
    }
    const int remove_width = S(68);
    const int view_width = S(96);
    const int remove_x = client.right - kPanelPadLeft - remove_width;
    const int view_x = remove_x - S(8) - view_width;
    for (int i = 0; i < static_cast<int>(word_lists_.size()) && i < kMaxWordListRows; ++i) {
        const WordListInfo& list = word_lists_[static_cast<std::size_t>(i)];
        const HWND check = make_check(kWordListCheckFirstId + i, utf8_to_wstr(list.name).c_str(), x0,
                                      y, view_x - x0 - S(8), panel);
        set_check(check, list.enabled);
        if (list.guessed > 0) {
            make_page_button(kWordListViewFirstId + i, tr("wordlists.view"), view_x, y - S(2),
                             view_width, S(28), panel);
        }
        make_page_button(kWordListRemoveFirstId + i, tr("wordlists.remove"), remove_x, y - S(2),
                         remove_width, S(28), panel);
        y += kRowH;
        make_hint(detail_text(list).c_str(), x0 + S(20), y - S(6), view_x - x0 - S(28), panel, 1);
        y += S(kFontPt + 6) + S(8);
    }
    card_end(panel, y);
}

void EditorApp::rebuild_word_list_panel() {
    const HWND panel = hPanels_[kWordListPanel];
    if (!panel) return;
    SendMessageW(panel, WM_SETREDRAW, FALSE, 0);
    const int scrolled = clear_panel(panel);
    hints_.erase(std::remove_if(hints_.begin(), hints_.end(), [](HWND h) { return !IsWindow(h); }),
                 hints_.end());
    create_word_list_panel(panel);
    update_panel_scroll(panel);
    scroll_panel(panel, scrolled);
    SendMessageW(panel, WM_SETREDRAW, TRUE, 0);
    apply_ui_theme(ui_colors().dark, true);  // the new controls
}

void EditorApp::load_word_lists() {
    if (!ensure_server_running()) return;
    WordListControlResult result;
    bool listed = false;
    {
        WaitCursor wait;
        listed = WordListControlClient(kWordListTimeoutMs).list(&result);
    }
    if (!listed) {
        MessageBoxW(hwnd_,
                    result.error_code == kWordListErrorStorage ? tr("wordlists.error_storage")
                                                               : tr("wordlists.error_load"),
                    tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    word_lists_ = std::move(result.lists);
    word_lists_loaded_ = true;
    rebuild_word_list_panel();
}

void EditorApp::import_word_list() {
    const std::wstring path = pick_word_list_file(hwnd_);
    if (path.empty()) return;
    const std::string utf8_path = wstr_to_utf8(path);
    // Another extension is no supported format: refused here, before the server reads it.
    if (!word_list_extension_supported(utf8_path)) {
        MessageBoxW(hwnd_, tr("wordlists.error_extension"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    if (!ensure_server_running()) return;
    WordListControlClient client(kWordListTimeoutMs);

    // A look at the file first: what it holds and what of it is new.
    WordListControlResult preview;
    bool read = false;
    {
        WaitCursor wait;
        read = client.preview(utf8_path, &preview);
    }
    if (!read) {
        MessageBoxW(hwnd_, import_error_text(preview).c_str(), tr("window.title"),
                    MB_OK | MB_ICONERROR);
        return;
    }
    const WordListInfo& list = preview.list;
    if (list.added == 0) {
        MessageBoxW(hwnd_, tr("wordlists.error_nothing_new"), tr("window.title"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring typing;
    if (list.language == "en") {
        typing = tr("wordlists.preview_en");
    } else {
        typing = fill(tr("wordlists.preview_zh"),
                      {{L"{pinyin}", number(list.pinyin)}, {L"{wubi}", number(list.wubi)}});
        if (list.guessed > 0) {
            typing += L"\n" + fill(tr("wordlists.preview_guessed"), {{L"{guessed}", number(list.guessed)}});
        }
    }
    const std::wstring confirm = fill(tr("wordlists.preview"),
                                      {{L"{name}", utf8_to_wstr(list.name)},
                                       {L"{format}", format_text(list.format)},
                                       {L"{read}", number(list.read)},
                                       {L"{added}", number(list.added)},
                                       {L"{existing}", number(list.existing)},
                                       {L"{invalid}", number(list.invalid)},
                                       {L"{typing}", typing}});
    if (MessageBoxW(hwnd_, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }

    WordListControlResult result;
    bool imported = false;
    {
        WaitCursor wait;
        imported = client.import_list(utf8_path, &result);
    }
    if (!imported) {
        MessageBoxW(hwnd_, import_error_text(result).c_str(), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    word_lists_ = std::move(result.lists);
    word_lists_loaded_ = true;
    PostMessageW(hwnd_, kRebuildWordListsMessage, 0, 0);
}

void EditorApp::set_word_list_enabled(std::size_t index, bool enabled) {
    if (index >= word_lists_.size()) return;
    const HWND check = GetDlgItem(hPanels_[kWordListPanel], kWordListCheckFirstId + static_cast<int>(index));
    WordListControlResult result;
    bool changed = false;
    if (ensure_server_running()) {
        WaitCursor wait;  // the dictionaries are rebuilt: a moment for a large list
        changed = WordListControlClient(kWordListTimeoutMs)
                      .set_enabled(word_lists_[index].id, enabled, &result);
    }
    if (!changed) {
        if (check) set_check(check, !enabled);
        MessageBoxW(hwnd_,
                    result.error_code == kWordListErrorStorage ? tr("wordlists.error_storage")
                                                               : tr("wordlists.error_change"),
                    tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    word_lists_ = std::move(result.lists);
}

void EditorApp::remove_word_list(std::size_t index) {
    if (index >= word_lists_.size()) return;
    const std::wstring confirm =
        fill(tr("wordlists.remove_confirm"), {{L"{name}", utf8_to_wstr(word_lists_[index].name)}});
    if (MessageBoxW(hwnd_, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
        return;
    }
    WordListControlResult result;
    bool removed = false;
    if (ensure_server_running()) {
        WaitCursor wait;
        removed = WordListControlClient(kWordListTimeoutMs).remove(word_lists_[index].id, &result);
    }
    if (!removed) {
        MessageBoxW(hwnd_,
                    result.error_code == kWordListErrorStorage ? tr("wordlists.error_storage")
                                                               : tr("wordlists.error_change"),
                    tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    word_lists_ = std::move(result.lists);
    // Not from inside the click: the button pressed is one of the controls made again.
    PostMessageW(hwnd_, kRebuildWordListsMessage, 0, 0);
}

void EditorApp::show_guessed_readings(std::size_t index) {
    if (index >= word_lists_.size() || !ensure_server_running()) return;
    WordListControlResult result;
    bool got = false;
    {
        WaitCursor wait;
        got = WordListControlClient(kWordListTimeoutMs).guessed(word_lists_[index].id, &result);
    }
    if (!got) {
        MessageBoxW(hwnd_, tr("wordlists.error_load"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring text = fill(tr("wordlists.guessed_intro"), {{L"{name}", utf8_to_wstr(word_lists_[index].name)}});
    text += L"\n";
    for (std::size_t i = 0; i < result.words.size() && i < kShownGuessedWords; ++i) {
        text += L"\n" + utf8_to_wstr(result.words[i].first) + L"    " + utf8_to_wstr(result.words[i].second);
    }
    if (word_lists_[index].guessed > kShownGuessedWords) {
        text += L"\n\n" + fill(tr("wordlists.guessed_more"),
                               {{L"{count}", number(word_lists_[index].guessed)}});
    }
    MessageBoxW(hwnd_, text.c_str(), tr("window.title"), MB_OK | MB_ICONINFORMATION);
}

bool EditorApp::handle_word_list_command(int control_id, int notification) {
    if (control_id == kWordListImportId) {
        if (notification == BN_CLICKED) import_word_list();
        return true;
    }
    const auto row = [&](int first) {
        return control_id >= first && control_id < first + kMaxWordListRows
                   ? static_cast<std::size_t>(control_id - first)
                   : SIZE_MAX;
    };
    if (const std::size_t index = row(kWordListCheckFirstId); index != SIZE_MAX) {
        if (notification == BN_CLICKED) {
            const HWND check = GetDlgItem(hPanels_[kWordListPanel], control_id);
            if (check) set_word_list_enabled(index, get_check(check));
        }
        return true;
    }
    if (const std::size_t index = row(kWordListViewFirstId); index != SIZE_MAX) {
        if (notification == BN_CLICKED) show_guessed_readings(index);
        return true;
    }
    if (const std::size_t index = row(kWordListRemoveFirstId); index != SIZE_MAX) {
        if (notification == BN_CLICKED) remove_word_list(index);
        return true;
    }
    return false;
}

}  // namespace settings
}  // namespace cxxime
