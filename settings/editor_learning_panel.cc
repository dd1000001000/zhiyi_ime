// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Settings > Learning (docs/learning-mode.md): the languages of the translations shown after
// Chinese and English candidates (languages without an installed pack are grayed out), a preview,
// and the language packs: download, check for updates, update, remove. Downloads run on worker
// threads and report back with window messages; the IME picks up installed or removed packs by
// itself (the server checks the pack files).

#include "editor_app.h"

#include <algorithm>
#include <thread>

#include <cxxime/version.h>

#include "editor_app_internal.h"
#include "glossary_packs.h"

namespace cxxime {
namespace settings {
namespace {

enum ControlId {
    kChineseTargetId = 1501,
    kEnglishTargetId,
    kGlossPreviewId,
    kPackTabFirstId = 1510,  // .. 1511
    kCheckAllPacksId = 1515,
    kPackActionFirstId = 1520,  // .. 1526, one per row
    kPackRemoveFirstId = 1540,  // .. 1546
};

constexpr UINT kPacksCheckedMessage = WM_APP + 50;    // wParam pack index + 1 (0: all); lParam result
constexpr UINT kPackProgressMessage = WM_APP + 51;    // wParam pack index; lParam bytes done
constexpr UINT kPackDownloadedMessage = WM_APP + 52;  // wParam pack index; lParam result

constexpr int kRows = 7;  // targets per direction
constexpr wchar_t kPackListClass[] = L"ZhiyiPackList";

struct PackDownloadResult {
    update::Status status = update::Status::kNetwork;
    std::wstring path;
    update::GlossaryPack pack;
};

const char* const kSources[] = {"zh", "en"};

// Language names in their own writing, after the UI name.
const wchar_t* native_name(const std::string& code) {
    if (code == "zh") return L"简体中文";
    if (code == "en") return L"English";
    if (code == "ja") return L"日本語";
    if (code == "ko") return L"한국어";
    if (code == "fr") return L"Français";
    if (code == "de") return L"Deutsch";
    if (code == "es") return L"Español";
    if (code == "ru") return L"Русский";
    return L"";
}

const wchar_t* language_name(const std::string& code) { return tr(("lang." + code).c_str()); }

std::wstring format(const wchar_t* pattern, std::initializer_list<std::wstring> values) {
    std::wstring text = pattern;
    int index = 0;
    for (const std::wstring& value : values) {
        const std::wstring field = L"{" + std::to_wstring(index++) + L"}";
        const size_t at = text.find(field);
        if (at != std::wstring::npos) text.replace(at, field.size(), value);
    }
    return text;
}

std::wstring megabytes(std::uint64_t bytes) {
    wchar_t text[32];
    swprintf(text, 32, L"%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// The server's pack is one this program reads.
bool usable(const update::GlossaryPack& pack) {
    return pack.format <= kGlossaryFormatVersion &&
           (pack.min_app.empty() || !update::is_newer_release(pack.min_app, CXXIME_VERSION_STRING));
}

std::wstring sense_line(const std::vector<GlossSense>& senses) {
    std::wstring line;
    for (const GlossSense& sense : senses) {
        if (!line.empty()) line += L" · ";
        const std::string label = gloss_pos_label(sense.pos);
        if (!label.empty()) line += utf8_to_wstr(label) + L" ";
        line += utf8_to_wstr(sense.text);
    }
    return line;
}

}  // namespace

void EditorApp::init_learning() {
    seed_builtin_packs();
    packs_.clear();
    for (const char* source : kSources) {
        for (const std::string& target : glossary_targets(source)) {
            PackState pack;
            pack.source = source;
            pack.target = target;
            refresh_pack(pack);
            packs_.push_back(std::move(pack));
        }
    }
}

void EditorApp::refresh_pack(PackState& pack) {
    const InstalledPack installed = installed_pack(pack.source, pack.target);
    pack.installed = installed.installed;
    pack.version = installed.header.content_version;
    pack.entries = installed.header.entry_count;
    pack.size = installed.size;
    pack.builtin = is_builtin_pack(pack.source, pack.target);
}

void EditorApp::create_learning_panel(HWND panel) {
    RECT client = {};
    GetClientRect(panel, &client);
    const int x0 = kPanelPadLeft;
    const int labels = label_width({"learning.chinese", "learning.english"});
    int y = card_begin(panel, kPanelPadTop, tr("learning.card_translation"));
    const int top = y;
    const int combo_x = make_aligned_label(tr("learning.chinese"), x0, labels, y, panel);
    hChineseTarget_ = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        combo_x, y, S(180), S(300), panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChineseTargetId)), GetModuleHandle(nullptr),
        nullptr);
    y += kRowH;
    make_aligned_label(tr("learning.english"), x0, labels, y, panel);
    hEnglishTarget_ = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        combo_x, y, S(180), S(300), panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEnglishTargetId)), GetModuleHandle(nullptr),
        nullptr);
    for (HWND combo : {hChineseTarget_, hEnglishTarget_}) {
        SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
        scroll_page_on_wheel(combo);
    }
    y += kRowH;
    const int preview_width = S(250);
    const int preview_x = client.right - kPanelPadLeft - preview_width;
    make_hint(tr("learning.hint"), x0, y, preview_x - x0 - S(16), panel);
    y += S(40);
    make_hint(tr("learning.keys_hint"), x0, y, preview_x - x0 - S(16), panel);
    y += S(40);
    const int preview_height = (std::max)(y - top, S(118));
    hGlossPreview_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                     preview_x, top, preview_width, preview_height, panel,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGlossPreviewId)),
                                     GetModuleHandle(nullptr), nullptr);
    y = card_end(panel, (std::max)(y, top + preview_height) - S(4));

    // Language packs: two tabs (Chinese -> other, English -> other) over one list of 7 rows.
    const int card_top = y;
    y = card_begin(panel, y, tr("learning.card_packs"));
    hCheckAllPacks_ = make_page_button(kCheckAllPacksId, tr("learning.check_all"),
                                       client.right - kPanelPadLeft - S(130), card_top + S(10),
                                       S(130), S(28), panel);
    const char* const tab_keys[] = {"learning.tab_chinese", "learning.tab_english"};
    for (int tab = 0; tab < 2; ++tab) {
        hPackTabs_[tab] = make_page_button(kPackTabFirstId + tab, tr(tab_keys[tab]),
                                           x0 + tab * S(128), y, S(120), S(28), panel);
    }
    y += S(36);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = pack_list_proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPackListClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    const int row_height = S(40);
    hPackList_ = CreateWindowExW(WS_EX_CONTROLPARENT, kPackListClass, L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, x0, y,
                                 client.right - x0 * 2, row_height * kRows, panel, nullptr,
                                 GetModuleHandle(nullptr), nullptr);
    SetWindowLongPtrW(hPackList_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    hPackActions_.clear();
    hPackRemoves_.clear();
    for (int row = 0; row < kRows; ++row) {
        hPackActions_.push_back(make_page_button(kPackActionFirstId + row, L"", 0, 0, S(116),
                                                 S(28), hPackList_));
        hPackRemoves_.push_back(make_page_button(kPackRemoveFirstId + row, tr("learning.remove"),
                                                 0, 0, S(68), S(28), hPackList_));
    }
    y = card_end(panel, y + row_height * kRows - S(6));
    show_pack_tab(pack_tab_);
    create_translator_card(panel, y);
}

void EditorApp::show_pack_tab(int tab) {
    pack_tab_ = tab;
    for (int i = 0; i < 2; ++i) {
        if (hPackTabs_[i]) set_button_selected(hPackTabs_[i], i == tab);
    }
    layout_pack_rows();
}

void EditorApp::layout_pack_rows() {
    if (!hPackList_) return;
    RECT client = {};
    GetClientRect(hPackList_, &client);
    const int row_height = client.bottom / kRows;
    const int remove_x = client.right - S(68);
    const int action_x = remove_x - S(8) - S(116);
    for (int row = 0; row < kRows; ++row) {
        const PackState& pack = packs_[pack_tab_ * kRows + row];
        const int y = row * row_height + (row_height - S(28)) / 2;
        std::wstring action;
        bool primary = false;
        bool enabled = true;
        if (pack.busy == PackState::Busy::kDownloading) {
            action = tr("learning.cancel");
        } else if (pack.busy == PackState::Busy::kChecking) {
            action = tr("learning.checking");
            enabled = false;
        } else if (!pack.installed) {
            action = tr("learning.download");
        } else if (pack.remote_known && pack.remote.version > pack.version) {
            action = format(tr("learning.update_to"), {std::to_wstring(pack.remote.version)});
            primary = true;
        } else {
            action = tr("learning.check");
        }
        HWND button = hPackActions_[row];
        SetWindowTextW(button, action.c_str());
        set_button_primary(button, primary);
        EnableWindow(button, enabled);
        SetWindowPos(button, nullptr, action_x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        HWND remove = hPackRemoves_[row];
        SetWindowPos(remove, nullptr, remove_x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(remove, pack.installed && pack.busy == PackState::Busy::kNone ? SW_SHOW
                                                                                : SW_HIDE);
    }
    InvalidateRect(hPackList_, nullptr, TRUE);
}

void EditorApp::paint_pack_list(HWND list, HDC dc) {
    const UiColors& colors = ui_colors();
    RECT client = {};
    GetClientRect(list, &client);
    FillRect(dc, &client, card_brush());
    const int row_height = client.bottom / kRows;
    const int text_right = client.right - S(68) - S(8) - S(116) - S(12);
    SetBkMode(dc, TRANSPARENT);
    for (int row = 0; row < kRows; ++row) {
        const PackState& pack = packs_[pack_tab_ * kRows + row];
        const int top = row * row_height;
        if (row > 0) {
            HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, 0, top, nullptr);
            LineTo(dc, client.right, top);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        // Language, then its own name in gray.
        HGDIOBJ old_font = SelectObject(dc, get_font());
        RECT name_rect = {0, top + S(3), text_right, top + row_height / 2 + S(3)};
        const std::wstring name = language_name(pack.target);
        SetTextColor(dc, colors.text);
        DrawTextW(dc, name.c_str(), -1, &name_rect, DT_SINGLELINE | DT_BOTTOM | DT_LEFT);
        SIZE name_size = {};
        GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()), &name_size);
        SelectObject(dc, get_small_font());
        RECT native_rect = {name_size.cx + S(8), name_rect.top, text_right, name_rect.bottom};
        SetTextColor(dc, colors.hint);
        DrawTextW(dc, native_name(pack.target), -1, &native_rect,
                  DT_SINGLELINE | DT_BOTTOM | DT_LEFT);

        // Status: progress while downloading, else version and size, and the last result.
        RECT status_rect = {0, top + row_height / 2 + S(2), text_right, top + row_height - S(2)};
        std::wstring status;
        if (pack.busy == PackState::Busy::kDownloading) {
            const int bar_width = S(120);
            const int bar_top = status_rect.top + (status_rect.bottom - status_rect.top) / 2 - S(2);
            const RECT track = {0, bar_top, bar_width, bar_top + S(4)};
            fill_round_rect(dc, track, S(2), colors.border, colors.border);
            if (pack.total > 0 && pack.done > 0) {
                const int done = static_cast<int>(bar_width * (std::min)(pack.done, pack.total) /
                                                  pack.total);
                const RECT bar = {0, bar_top, (std::max)(done, S(4)), bar_top + S(4)};
                fill_round_rect(dc, bar, S(2), colors.accent, colors.accent);
            }
            status_rect.left = bar_width + S(10);
            status = format(tr("learning.downloading"), {megabytes(pack.done),
                                                         megabytes(pack.total)});
        } else if (pack.installed) {
            status = format(tr("learning.status_installed"),
                            {std::to_wstring(pack.version), megabytes(pack.size),
                             std::to_wstring(pack.entries)});
        } else if (pack.remote_known) {
            status = format(tr("learning.status_absent_size"), {megabytes(pack.remote.size)});
        } else {
            status = tr("learning.status_absent");
        }
        SetTextColor(dc, colors.hint);
        DrawTextW(dc, status.c_str(), -1, &status_rect,
                  DT_SINGLELINE | DT_TOP | DT_LEFT | DT_END_ELLIPSIS);
        if (!pack.note.empty() && pack.busy != PackState::Busy::kDownloading) {
            RECT measured = status_rect;
            DrawTextW(dc, status.c_str(), -1, &measured, DT_SINGLELINE | DT_LEFT | DT_CALCRECT);
            RECT note_rect = {measured.right, status_rect.top, text_right, status_rect.bottom};
            const std::wstring note = L" · " + pack.note;
            SetTextColor(dc, pack.note_good ? colors.link : colors.hint);
            DrawTextW(dc, note.c_str(), -1, &note_rect,
                      DT_SINGLELINE | DT_TOP | DT_LEFT | DT_END_ELLIPSIS);
        }
        SelectObject(dc, old_font);
    }
}

LRESULT CALLBACK EditorApp::pack_list_proc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam) {
    auto* app = reinterpret_cast<EditorApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        if (app) app->paint_pack_list(window, reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        if (app) app->paint_pack_list(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_COMMAND:
    case WM_DRAWITEM:
    case WM_NOTIFY:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetAncestor(window, GA_ROOT), message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void EditorApp::fill_target_combo(HWND combo, const std::string& source,
                                  const std::string& selected) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    combo_add(combo, tr("learning.off"));
    SendMessageW(combo, CB_SETITEMDATA, 0, 1);
    int selection = 0;
    const std::vector<std::string>& targets = glossary_targets(source);
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto found = std::find_if(packs_.begin(), packs_.end(), [&](const PackState& p) {
            return p.source == source && p.target == targets[i];
        });
        const bool installed = found != packs_.end() && found->installed;
        std::wstring text = language_name(targets[i]);
        if (!installed) text += tr("learning.not_downloaded");
        combo_add(combo, text.c_str());
        SendMessageW(combo, CB_SETITEMDATA, i + 1, installed ? 1 : 0);
        if (installed && targets[i] == selected) selection = static_cast<int>(i) + 1;
    }
    combo_set_index(combo, selection);
    (combo == hChineseTarget_ ? last_chinese_target_ : last_english_target_) = selection;
}

std::string EditorApp::combo_target(HWND combo, const std::string& source) const {
    const int index = combo_index(combo);
    const std::vector<std::string>& targets = glossary_targets(source);
    if (index <= 0 || index > static_cast<int>(targets.size())) return {};
    if (SendMessageW(combo, CB_GETITEMDATA, index, 0) == 0) return {};
    return targets[index - 1];
}

void EditorApp::populate_learning() {
    fill_target_combo(hChineseTarget_, "zh", config_.chinese_gloss_target);
    fill_target_combo(hEnglishTarget_, "en", config_.english_gloss_target);
    if (hGlossPreview_) InvalidateRect(hGlossPreview_, nullptr, TRUE);
}

void EditorApp::read_learning(Config& config) {
    if (!hChineseTarget_) return;
    config.chinese_gloss_target = combo_target(hChineseTarget_, "zh");
    config.english_gloss_target = combo_target(hEnglishTarget_, "en");
}

bool EditorApp::handle_learning_item(UINT message, LPARAM lparam) {
    if (message == WM_MEASUREITEM) {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure->CtlID != kChineseTargetId && measure->CtlID != kEnglishTargetId) {
            return false;
        }
        measure->itemHeight = measure->itemID == static_cast<UINT>(-1) ? kCtrlH - S(8) : S(26);
        return true;
    }
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (item->CtlID == kChineseTargetId || item->CtlID == kEnglishTargetId) {
        draw_target_item(*item);
        return true;
    }
    if (item->CtlID == kGlossPreviewId) {
        draw_gloss_preview(*item);
        return true;
    }
    return false;
}

void EditorApp::draw_target_item(const DRAWITEMSTRUCT& item) {
    const UiColors& colors = ui_colors();
    const HDC dc = item.hDC;
    RECT rect = item.rcItem;
    const bool field = (item.itemState & ODS_COMBOBOXEDIT) != 0;
    const bool available = item.itemID != static_cast<UINT>(-1) &&
                           SendMessageW(item.hwndItem, CB_GETITEMDATA, item.itemID, 0) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0 && !field && available;
    HBRUSH background = CreateSolidBrush(selected ? colors.nav_selected : colors.control);
    FillRect(dc, &rect, background);
    DeleteObject(background);
    if (item.itemID == static_cast<UINT>(-1)) return;
    const int length = static_cast<int>(SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0));
    std::wstring text(length + 1, L'\0');
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data()));
    text.resize(length);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, !available ? colors.hint : selected ? colors.nav_selected_text : colors.text);
    HGDIOBJ old_font = SelectObject(dc, get_font());
    rect.left += S(6);
    DrawTextW(dc, text.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

// A candidate window drawn with the selected language's pack: "shijian" for Chinese, "light"
// for English.
void EditorApp::draw_gloss_preview(const DRAWITEMSTRUCT& item) {
    const UiColors& colors = ui_colors();
    const HDC dc = item.hDC;
    const RECT rect = item.rcItem;
    FillRect(dc, &rect, card_brush());
    fill_round_rect(dc, rect, S(6), colors.window, colors.border);

    std::string source = "zh";
    std::string target = combo_target(hChineseTarget_, "zh");
    if (target.empty()) {
        const std::string english = combo_target(hEnglishTarget_, "en");
        if (!english.empty()) {
            source = "en";
            target = english;
        }
    }
    struct Sample {
        const wchar_t* text;
        const char* key;
        const char* syllables;
    };
    const Sample chinese[] = {{L"时间", "时间", "shi:jian"}, {L"事件", "事件", "shi:jian"},
                              {L"实践", "实践", "shi:jian"}};
    const Sample english[] = {{L"light", "light", ""}, {L"lights", "lights", ""},
                              {L"lighting", "lighting", ""}};
    const Sample* samples = source == "zh" ? chinese : english;
    Glossary glossary;
    if (!target.empty()) glossary.open(glossary_pack_path(source, target));

    SetBkMode(dc, TRANSPARENT);
    const int x = rect.left + S(10);
    int y = rect.top + S(8);
    HGDIOBJ old_font = SelectObject(dc, get_small_font());
    SetTextColor(dc, colors.hint);
    RECT label = {x, y, rect.right - S(10), y + S(18)};
    const std::wstring heading = std::wstring(tr("learning.preview")) + L"   " +
                                 (source == "zh" ? L"shijian" : L"light");
    DrawTextW(dc, heading.c_str(), -1, &label, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    y += S(22);
    for (int i = 0; i < 3; ++i) {
        const int row_height = S(26);
        RECT row = {rect.left + S(6), y, rect.right - S(6), y + row_height};
        if (i == 0) fill_round_rect(dc, row, S(4), colors.nav_selected, colors.nav_selected);
        SelectObject(dc, get_font());
        SetTextColor(dc, i == 0 ? colors.nav_selected_text : colors.text);
        const std::wstring candidate = std::to_wstring(i + 1) + L". " + samples[i].text;
        RECT text_rect = {x, y, rect.right - S(10), y + row_height};
        DrawTextW(dc, candidate.c_str(), -1, &text_rect, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SIZE size = {};
        GetTextExtentPoint32W(dc, candidate.c_str(), static_cast<int>(candidate.size()), &size);
        const std::wstring gloss =
            glossary.is_open() ? sense_line(glossary.lookup(samples[i].key, samples[i].syllables))
                               : std::wstring();
        if (!gloss.empty()) {
            SelectObject(dc, get_small_font());
            SetTextColor(dc, i == 0 ? colors.nav_selected_text : colors.hint);
            RECT gloss_rect = {x + size.cx + S(10), y, rect.right - S(10), y + row_height};
            DrawTextW(dc, gloss.c_str(), -1, &gloss_rect,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        y += row_height + S(2);
    }
    SelectObject(dc, old_font);
}

void EditorApp::start_pack_check(int pack_index) {
    bool any = false;
    for (int i = 0; i < static_cast<int>(packs_.size()); ++i) {
        PackState& pack = packs_[i];
        const bool in_scope = pack_index < 0 ? pack.installed : i == pack_index;
        if (!in_scope || pack.busy != PackState::Busy::kNone) continue;
        pack.busy = PackState::Busy::kChecking;
        pack.note.clear();
        any = true;
    }
    if (!any) return;
    layout_pack_rows();
    const HWND window = hwnd_;
    std::thread([window, pack_index] {
        auto* result = new update::GlossaryCheckResult(update::check_glossaries());
        if (!PostMessageW(window, kPacksCheckedMessage, static_cast<WPARAM>(pack_index + 1),
                          reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void EditorApp::on_packs_checked(const update::GlossaryCheckResult& result, int pack_index) {
    for (int i = 0; i < static_cast<int>(packs_.size()); ++i) {
        PackState& pack = packs_[i];
        if (pack.busy != PackState::Busy::kChecking) continue;
        if (pack_index >= 0 && i != pack_index) continue;
        pack.busy = PackState::Busy::kNone;
        pack.note_good = false;
        if (result.status != update::Status::kOk) {
            pack.note = result.status == update::Status::kInvalid ? tr("learning.failed_invalid")
                        : result.status == update::Status::kNotFound
                            ? tr("learning.not_found")
                            : tr("learning.failed_network");
            continue;
        }
        const std::string id = glossary_pack_id(pack.source, pack.target);
        const auto found = std::find_if(result.packs.begin(), result.packs.end(),
                                        [&](const update::GlossaryPack& p) {
                                            return p.id == id && usable(p);
                                        });
        pack.remote_known = found != result.packs.end();
        if (pack.remote_known) pack.remote = *found;
        if (pack.remote_known && pack.remote.version > pack.version) {
            pack.note = format(tr("learning.has_update"), {std::to_wstring(pack.remote.version)});
            pack.note_good = true;
        } else {
            pack.note = tr("learning.latest");
        }
    }
    layout_pack_rows();
}

void EditorApp::start_pack_download(int pack_index) {
    PackState& pack = packs_[pack_index];
    if (pack.busy != PackState::Busy::kNone) return;
    pack.busy = PackState::Busy::kDownloading;
    pack.done = 0;
    pack.total = pack.remote_known ? pack.remote.size : 0;
    pack.note.clear();
    pack.cancel = std::make_shared<std::atomic<bool>>(false);
    layout_pack_rows();

    const HWND window = hwnd_;
    const std::string id = glossary_pack_id(pack.source, pack.target);
    const bool known = pack.remote_known;
    const update::GlossaryPack remote = pack.remote;
    const std::shared_ptr<std::atomic<bool>> cancel = pack.cancel;
    const std::wstring path = utf8_to_wstr(glossary_directory() + "\\" + id + ".gloss.download");
    std::thread([window, pack_index, id, known, remote, cancel, path] {
        auto* result = new PackDownloadResult;
        result->path = path;
        result->pack = remote;
        if (!known) {
            const update::GlossaryCheckResult check = update::check_glossaries();
            result->status = check.status;
            const auto found = std::find_if(check.packs.begin(), check.packs.end(),
                                            [&](const update::GlossaryPack& p) {
                                                return p.id == id && usable(p);
                                            });
            if (check.status == update::Status::kOk) {
                if (found == check.packs.end()) {
                    result->status = update::Status::kNotFound;
                } else {
                    result->pack = *found;
                }
            }
        }
        if (known || result->status == update::Status::kOk) {
            result->status = update::download_glossary(
                result->pack, path,
                [window, pack_index](std::uint64_t done, std::uint64_t) {
                    PostMessageW(window, kPackProgressMessage, static_cast<WPARAM>(pack_index),
                                 static_cast<LPARAM>(done));
                },
                cancel.get());
        }
        if (!PostMessageW(window, kPackDownloadedMessage, static_cast<WPARAM>(pack_index),
                          reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void EditorApp::on_pack_downloaded(int pack_index, update::Status status,
                                   const std::wstring& path) {
    PackState& pack = packs_[pack_index];
    pack.busy = PackState::Busy::kNone;
    pack.cancel.reset();
    pack.note_good = false;
    switch (status) {
    case update::Status::kOk:
        if (install_pack(path, pack.source, pack.target)) {
            pack.note.clear();
        } else {
            pack.note = tr("learning.failed_disk");
        }
        break;
    case update::Status::kCancelled: pack.note = tr("learning.cancelled"); break;
    case update::Status::kInvalid: pack.note = tr("learning.failed_invalid"); break;
    case update::Status::kDisk: pack.note = tr("learning.failed_disk"); break;
    case update::Status::kNotFound: pack.note = tr("learning.not_found"); break;
    default: pack.note = tr("learning.failed_network"); break;
    }
    refresh_pack(pack);
    read_controls(false);  // keep the unsaved choices while the menus are refilled
    populate_learning();
    layout_pack_rows();
}

void EditorApp::remove_pack_at(int pack_index) {
    PackState& pack = packs_[pack_index];
    const std::wstring question =
        format(tr("learning.remove_confirm"), {language_name(pack.target)});
    if (MessageBoxW(hwnd_, question.c_str(), tr("window.title"),
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (!remove_pack(pack.source, pack.target)) {
        MessageBoxW(hwnd_, tr("learning.remove_failed"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    pack.note.clear();
    refresh_pack(pack);
    read_controls(false);
    populate_learning();  // a removed language falls back to Off
    layout_pack_rows();
}

void EditorApp::on_pack_action(int pack_index) {
    PackState& pack = packs_[pack_index];
    switch (pack.busy) {
    case PackState::Busy::kDownloading:
        if (pack.cancel) pack.cancel->store(true);
        return;
    case PackState::Busy::kChecking:
        return;
    case PackState::Busy::kNone:
        break;
    }
    if (pack.installed) {
        if (pack.remote_known && pack.remote.version > pack.version) {
            start_pack_download(pack_index);
        } else {
            start_pack_check(pack_index);
        }
        return;
    }
    // A removed built-in pack comes back from the installation, unless the server has a newer
    // one.
    if (pack.builtin &&
        !(pack.remote_known &&
          pack.remote.version > builtin_pack_version(pack.source, pack.target))) {
        if (restore_builtin_pack(pack.source, pack.target)) {
            refresh_pack(pack);
            read_controls(false);
            populate_learning();
            layout_pack_rows();
            return;
        }
    }
    start_pack_download(pack_index);
}

bool EditorApp::handle_learning_command(int control_id, int notification) {
    if (control_id == kChineseTargetId || control_id == kEnglishTargetId) {
        if (notification != CBN_SELCHANGE) return true;
        HWND combo = control_id == kChineseTargetId ? hChineseTarget_ : hEnglishTarget_;
        int& last = control_id == kChineseTargetId ? last_chinese_target_ : last_english_target_;
        const int index = combo_index(combo);
        if (index >= 0 && SendMessageW(combo, CB_GETITEMDATA, index, 0) == 0) {
            MessageBeep(MB_OK);  // not downloaded: stays as it was
            combo_set_index(combo, last);
            return true;
        }
        last = index;
        if (index > 0 && hVertical_) {  // translations show in the vertical layout
            set_check(hVertical_, true);
            set_check(hHorizontal_, false);
        }
        InvalidateRect(hGlossPreview_, nullptr, TRUE);
        return true;
    }
    if (notification != BN_CLICKED) return false;
    if (control_id == kPackTabFirstId || control_id == kPackTabFirstId + 1) {
        show_pack_tab(control_id - kPackTabFirstId);
        return true;
    }
    if (control_id == kCheckAllPacksId) {
        start_pack_check(-1);
        return true;
    }
    if (control_id >= kPackActionFirstId && control_id < kPackActionFirstId + kRows) {
        on_pack_action(pack_tab_ * kRows + control_id - kPackActionFirstId);
        return true;
    }
    if (control_id >= kPackRemoveFirstId && control_id < kPackRemoveFirstId + kRows) {
        remove_pack_at(pack_tab_ * kRows + control_id - kPackRemoveFirstId);
        return true;
    }
    return false;
}

bool EditorApp::handle_learning_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kPacksCheckedMessage: {
        std::unique_ptr<update::GlossaryCheckResult> result(
            reinterpret_cast<update::GlossaryCheckResult*>(lparam));
        on_packs_checked(*result, static_cast<int>(wparam) - 1);
        return true;
    }
    case kPackProgressMessage: {
        const int index = static_cast<int>(wparam);
        if (index >= 0 && index < static_cast<int>(packs_.size()) &&
            packs_[index].busy == PackState::Busy::kDownloading) {
            packs_[index].done = static_cast<std::uint64_t>(lparam);
            if (hPackList_) InvalidateRect(hPackList_, nullptr, FALSE);
        }
        return true;
    }
    case kPackDownloadedMessage: {
        std::unique_ptr<PackDownloadResult> result(reinterpret_cast<PackDownloadResult*>(lparam));
        const int index = static_cast<int>(wparam);
        if (index >= 0 && index < static_cast<int>(packs_.size())) {
            if (result->pack.size) packs_[index].total = result->pack.size;
            if (result->status == update::Status::kOk) {
                packs_[index].remote = result->pack;
                packs_[index].remote_known = true;
            }
            on_pack_downloaded(index, result->status, result->path);
        }
        return true;
    }
    default:
        return false;
    }
}

}  // namespace settings
}  // namespace cxxime
