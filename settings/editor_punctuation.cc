// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Custom punctuation (docs/punctuation.md, issue #43), as Sogou Pinyin's: a card on the Advanced
// page opens a table of the keys with what each types at half and full width with Chinese
// punctuation on, and whether an English value joins the code in the middle of pinyin. A click
// on a cell edits it in place (two values with a space between: a pair), a right click puts the
// default back; what differs from data/punctuation.json goes to config.json ("punctuation"),
// saved with Apply.

#include "editor_app.h"

#include <algorithm>
#include <cwchar>
#include <string>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cxxime/data_path.h>
#include <cxxime/punctuation.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kPunctOpenId = 6700;
// The table window.
constexpr int kListId = 6800;
constexpr int kResetAllId = 6801;
constexpr int kOkId = 6802;
constexpr int kCancelId = 6803;
constexpr UINT kEndEditMessage = WM_APP + 1;  // wparam: keep the text; lparam: that edit box
constexpr wchar_t kTableClass[] = L"ZhiyiPunctuationTable";
constexpr int kMaxValueChars = 40;  // UTF-16 units; valid_punctuation_text allows 8 characters each

enum Column { kKeyColumn = 0, kHalfColumn = 1, kFullColumn = 2, kJoinColumn = 3 };

std::wstring replaced(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const std::size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, std::wcslen(key), value);
    return text;
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>((std::max)(0, length)) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>((std::max)(0, length)));
    return text;
}

// What a key types: the table's entry, else the key itself.
PunctEntry entry_of(const PunctMapping& table, char key, bool full) {
    const auto& column = full ? table.full_shape : table.half_shape;
    const auto it = column.find(std::string(1, key));
    if (it != column.end()) return it->second;
    return {PunctType::COMMIT, std::string(1, key), {}, {}};
}

// "【", or a pair as "“ ”" (as it is typed in the table).
std::wstring entry_display(const PunctEntry& entry) {
    if (entry.type == PunctType::PAIR && entry.pair.size() == 2) {
        return utf8_to_wstr(entry.pair[0]) + L" " + utf8_to_wstr(entry.pair[1]);
    }
    return utf8_to_wstr(punctuation_entry_text(entry));
}

// A cell as typed: one value, or two with spaces between (a pair); nullopt when not valid.
std::optional<PunctEntry> parse_cell(const std::wstring& text) {
    std::vector<std::string> parts;
    std::wstring part;
    for (const wchar_t ch : text + L" ") {
        if (ch == L' ' || ch == L'\t' || ch == 0x3000) {
            if (!part.empty()) parts.push_back(wstr_to_utf8(part));
            part.clear();
        } else {
            part += ch;
        }
    }
    PunctEntry entry{};
    if (parts.size() == 1) {
        entry = {PunctType::COMMIT, parts[0], {}, {}};
    } else if (parts.size() == 2) {
        entry = {PunctType::PAIR, "", parts, {}};
    } else {
        return std::nullopt;
    }
    if (!valid_punctuation_entry(entry)) return std::nullopt;
    return entry;
}

struct TableState {
    EditorApp* app = nullptr;
    PunctMapping defaults;
    std::vector<PunctuationKeyConfig> keys;  // being edited; config_ takes it on OK
    HWND list = nullptr;
    HWND reset_all = nullptr;
    HWND edit = nullptr;  // the cell being edited
    int edit_row = -1;
    int edit_column = -1;
    int footer_top = 0;
    bool accepted = false;
    bool done = false;
};

PunctuationKeyConfig* find_key(TableState& state, char key) {
    for (PunctuationKeyConfig& custom : state.keys) {
        if (custom.key == key) return &custom;
    }
    return nullptr;
}

const PunctuationKeyConfig* find_key(const std::vector<PunctuationKeyConfig>& keys, char key) {
    for (const PunctuationKeyConfig& custom : keys) {
        if (custom.key == key) return &custom;
    }
    return nullptr;
}

bool cell_changed(const TableState& state, int row, int column) {
    if (row < 0 || row >= static_cast<int>(kCustomPunctuationKeys.size())) return false;
    const PunctuationKeyConfig* custom = find_key(state.keys, kCustomPunctuationKeys[row]);
    if (!custom) return false;
    switch (column) {
    case kKeyColumn:
        return true;
    case kHalfColumn:
        return custom->half_shape.has_value();
    case kFullColumn:
        return custom->full_shape.has_value();
    case kJoinColumn:
        return custom->join_code.has_value();
    default:
        return false;
    }
}

void set_cell(HWND list, int row, int column, const std::wstring& text) {
    ListView_SetItemText(list, row, column, const_cast<wchar_t*>(text.c_str()));
}

void show_rows(const TableState& state) {
    const PunctMapping table = apply_punctuation_config(state.defaults, state.keys);
    for (int row = 0; row < static_cast<int>(kCustomPunctuationKeys.size()); ++row) {
        const char key = kCustomPunctuationKeys[row];
        set_cell(state.list, row, kHalfColumn, entry_display(entry_of(table, key, false)));
        set_cell(state.list, row, kFullColumn, entry_display(entry_of(table, key, true)));
        const bool joins = table.join_code.find(key) != std::string::npos;
        set_cell(state.list, row, kJoinColumn, tr(joins ? "punct.join_code" : "punct.join_commit"));
    }
    EnableWindow(state.reset_all, !state.keys.empty());
    InvalidateRect(state.list, nullptr, FALSE);
}

// A key with nothing set left any more is dropped.
void tidy_keys(TableState& state) {
    state.keys.erase(std::remove_if(state.keys.begin(), state.keys.end(),
                                    [](const PunctuationKeyConfig& custom) {
                                        return !custom.half_shape && !custom.full_shape &&
                                               !custom.join_code;
                                    }),
                     state.keys.end());
    std::sort(state.keys.begin(), state.keys.end(),
              [](const PunctuationKeyConfig& a, const PunctuationKeyConfig& b) {
                  return kCustomPunctuationKeys.find(a.key) < kCustomPunctuationKeys.find(b.key);
              });
}

PunctuationKeyConfig& key_config(TableState& state, char key) {
    if (PunctuationKeyConfig* custom = find_key(state, key)) return *custom;
    PunctuationKeyConfig custom;
    custom.key = key;
    state.keys.push_back(custom);
    return state.keys.back();
}

// A value set for a width; the table's own value is no setting.
void set_value(TableState& state, int row, int column, const std::optional<PunctEntry>& value) {
    const char key = kCustomPunctuationKeys[row];
    const bool full = column == kFullColumn;
    PunctuationKeyConfig& custom = key_config(state, key);
    std::optional<PunctEntry>& slot = full ? custom.full_shape : custom.half_shape;
    slot = value && !(*value == entry_of(state.defaults, key, full)) ? value : std::nullopt;
    tidy_keys(state);
}

void toggle_join(TableState& state, int row) {
    const char key = kCustomPunctuationKeys[row];
    const PunctMapping table = apply_punctuation_config(state.defaults, state.keys);
    const bool joins = table.join_code.find(key) == std::string::npos;  // the other way now
    const bool default_joins = state.defaults.join_code.find(key) != std::string::npos;
    key_config(state, key).join_code = joins == default_joins ? std::nullopt : std::optional<bool>(joins);
    tidy_keys(state);
}

// A right click: the cell's default back (on the key: the whole row).
void reset_cell(TableState& state, int row, int column) {
    PunctuationKeyConfig* custom = find_key(state, kCustomPunctuationKeys[row]);
    if (!custom) return;
    if (column == kKeyColumn) {
        *custom = PunctuationKeyConfig{custom->key, std::nullopt, std::nullopt, std::nullopt};
    } else if (column == kHalfColumn) {
        custom->half_shape.reset();
    } else if (column == kFullColumn) {
        custom->full_shape.reset();
    } else {
        custom->join_code.reset();
    }
    tidy_keys(state);
}

void end_edit(TableState& state, bool keep) {
    HWND edit = state.edit;
    if (!edit) return;
    state.edit = nullptr;
    if (keep) {
        const std::wstring text = window_text(edit);
        const bool blank = text.find_first_not_of(L" \t\x3000") == std::wstring::npos;
        const std::optional<PunctEntry> value = blank ? std::nullopt : parse_cell(text);
        if (blank || value) {
            set_value(state, state.edit_row, state.edit_column, value);
        } else {
            MessageBeep(MB_ICONWARNING);
        }
    }
    DestroyWindow(edit);
    show_rows(state);
    SetFocus(state.list);
}

LRESULT CALLBACK cell_edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                                DWORD_PTR data) {
    HWND dialog = reinterpret_cast<HWND>(data);
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DefSubclassProc(window, message, wparam, lparam);
    case WM_KEYDOWN:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) {
            PostMessageW(dialog, kEndEditMessage, wparam != VK_ESCAPE, reinterpret_cast<LPARAM>(window));
            return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == L'\r' || wparam == 0x1B || wparam == L'\t') return 0;  // no beep
        break;
    case WM_KILLFOCUS:
        PostMessageW(dialog, kEndEditMessage, TRUE, reinterpret_cast<LPARAM>(window));
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, cell_edit_proc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void begin_edit(TableState& state, HWND dialog, int row, int column) {
    end_edit(state, true);
    ListView_EnsureVisible(state.list, row, FALSE);
    RECT cell = {};
    if (!ListView_GetSubItemRect(state.list, row, column, LVIR_BOUNDS, &cell)) return;
    const PunctMapping table = apply_punctuation_config(state.defaults, state.keys);
    const std::wstring text = entry_display(entry_of(table, kCustomPunctuationKeys[row], column == kFullColumn));
    state.edit = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 cell.left, cell.top, cell.right - cell.left, cell.bottom - cell.top, state.list,
                                 nullptr, GetModuleHandle(nullptr), nullptr);
    if (!state.edit) return;
    state.edit_row = row;
    state.edit_column = column;
    SendMessageW(state.edit, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    SendMessageW(state.edit, EM_SETLIMITTEXT, kMaxValueChars, 0);
    SetWindowSubclass(state.edit, cell_edit_proc, 1, reinterpret_cast<DWORD_PTR>(dialog));
    SetWindowTheme(state.edit, ui_colors().dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    SetFocus(state.edit);
    SendMessageW(state.edit, EM_SETSEL, 0, -1);
}

// The row and column under a click in the list.
bool hit_cell(HWND list, POINT point, int* row, int* column) {
    LVHITTESTINFO hit = {};
    hit.pt = point;
    if (ListView_SubItemHitTest(list, &hit) < 0 || hit.iItem < 0) return false;
    *row = hit.iItem;
    *column = hit.iSubItem;
    return true;
}

void style_list(HWND list) {
    const UiColors& colors = ui_colors();
    ListView_SetBkColor(list, colors.control);
    ListView_SetTextBkColor(list, colors.control);
    ListView_SetTextColor(list, colors.text);
    SetWindowTheme(list, colors.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowTheme(ListView_GetHeader(list), colors.dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
}

}  // namespace

int EditorApp::create_punctuation_card(HWND panel, int top) {
    if (!punctuation_defaults_) {
        punctuation_defaults_ = load_punctuation_file(data_path("punctuation.json"));
    }
    RECT client = {};
    GetClientRect(panel, &client);
    const int x0 = kPanelPadLeft;
    const int width = client.right - x0 * 2;

    int y = card_begin(panel, top, tr("punct.card"));
    const int hint_rows = hint_lines(tr("punct.hint"), x0, width, panel);
    make_hint(tr("punct.hint"), x0, y, width, panel, hint_rows);
    y += S(hint_rows * (kFontPt + 6)) + S(10);
    const int button_width = S(150);
    make_page_button(kPunctOpenId, tr("punct.open"), x0, y, button_width, S(30), panel);
    hPunctChanged_ = make_hint(L"", x0 + button_width + S(14), y + S(6), width - button_width - S(14), panel, 1);
    y += S(30);
    const int bottom = card_end(panel, y);
    populate_punctuation();
    return bottom;
}

void EditorApp::populate_punctuation() {
    if (!hPunctChanged_) return;
    SetWindowTextW(hPunctChanged_, config_.punctuation.empty()
                                       ? tr("punct.all_default")
                                       : replaced(tr("punct.changed"), L"{count}",
                                                  std::to_wstring(config_.punctuation.size()))
                                             .c_str());
}

// The table: a modal window as the phrase dialog, the keys in a list view.
void EditorApp::run_punctuation_table() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = punctuation_table_proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kTableClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    const int pad = S(24);
    const int width = S(540);
    const int field_width = width - 2 * pad;
    const int list_height = S(340);
    const int hint_rows = 3;
    const int footer_top = pad + S(40) + list_height + S(10) + S(hint_rows * (kFontPt + 6)) + S(8);
    const int height = footer_top + S(72);
    RECT owner = {};
    GetWindowRect(hwnd_, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    TableState state;
    state.app = this;
    state.defaults = punctuation_defaults_ ? *punctuation_defaults_ : PunctMapping{};
    state.keys = config_.punctuation;
    state.footer_top = footer_top;
    HWND dialog = CreateWindowExW(WS_EX_CONTROLPARENT, kTableClass, tr("punct.card"), WS_POPUP | WS_CLIPCHILDREN, x,
                                  y, width, height, hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!dialog) return;
    SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    const DWORD round = 2;  // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(dialog, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));

    int top = pad;
    HWND heading = CreateWindowExW(0, L"STATIC", tr("punct.card"), WS_CHILD | WS_VISIBLE | SS_LEFT, pad, top,
                                   field_width, S(30), dialog, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(get_title_font()), TRUE);
    top += S(40);

    state.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                                     LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                 pad, top, field_width, list_height, dialog,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), GetModuleHandle(nullptr),
                                 nullptr);
    SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    ListView_SetExtendedListViewStyle(state.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const int scroll = GetSystemMetrics(SM_CXVSCROLL);
    const int key_width = S(70);
    const int value_width = S(130);
    const struct {
        const char* title;
        int width;
    } columns[] = {{"punct.col_key", key_width},
                   {"punct.col_half", value_width},
                   {"punct.col_full", value_width},
                   {"punct.col_join", field_width - key_width - 2 * value_width - scroll - S(4)}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(tr(columns[i].title));
        column.cx = columns[i].width;
        ListView_InsertColumn(state.list, i, &column);
    }
    for (int row = 0; row < static_cast<int>(kCustomPunctuationKeys.size()); ++row) {
        std::wstring key(1, static_cast<wchar_t>(kCustomPunctuationKeys[row]));
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = key.data();
        ListView_InsertItem(state.list, &item);
    }
    top += list_height + S(10);
    make_hint(tr("punct.table_hint"), pad, top, field_width, dialog, hint_rows);

    const int button_width = (field_width - 2 * S(10)) / 3;
    state.reset_all = make_page_button(kResetAllId, tr("punct.reset_all"), pad, footer_top + S(20), button_width,
                                       S(32), dialog);
    HWND ok = make_page_button(kOkId, tr("button.ok"), pad + button_width + S(10), footer_top + S(20),
                               button_width, S(32), dialog);
    set_button_primary(ok, true);
    make_page_button(kCancelId, tr("button.cancel"), pad + 2 * (button_width + S(10)), footer_top + S(20),
                     button_width, S(32), dialog);
    show_rows(state);
    style_popup(dialog);
    style_list(state.list);

    EnableWindow(hwnd_, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(state.list);
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
        config_.punctuation = state.keys;
        populate_punctuation();
    }
    EnableWindow(hwnd_, TRUE);
    SetActiveWindow(hwnd_);
    DestroyWindow(dialog);
    if (quit) PostQuitMessage(static_cast<int>(msg.wParam));
}

LRESULT CALLBACK EditorApp::punctuation_table_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<TableState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        if (state) {
            return state->app->control_colors(message, reinterpret_cast<HDC>(wparam),
                                              reinterpret_cast<HWND>(lparam));
        }
        break;
    case WM_NOTIFY: {
        if (!state) break;
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->hwndFrom != state->list) break;
        if (header->code == NM_CUSTOMDRAW) {
            // A changed cell in the accent color.
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
            switch (draw->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                const bool changed =
                    cell_changed(*state, static_cast<int>(draw->nmcd.dwItemSpec), draw->iSubItem);
                draw->clrText = changed ? (ui_colors().dark ? ui_colors().link : ui_colors().accent)
                                        : ui_colors().text;
                draw->clrTextBk = ui_colors().control;
                return CDRF_NEWFONT;
            }
            default:
                return CDRF_DODEFAULT;
            }
        }
        if (header->code == NM_CLICK || header->code == NM_RCLICK) {
            const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lparam);
            int row = -1;
            int column = -1;
            if (!hit_cell(state->list, activate->ptAction, &row, &column)) break;
            if (header->code == NM_RCLICK) {
                end_edit(*state, true);
                reset_cell(*state, row, column);
                show_rows(*state);
            } else if (column == kHalfColumn || column == kFullColumn) {
                begin_edit(*state, window, row, column);
            } else if (column == kJoinColumn) {
                end_edit(*state, true);
                toggle_join(*state, row);
                show_rows(*state);
            }
            return 0;
        }
        if (header->code == LVN_BEGINSCROLL) end_edit(*state, true);
        break;
    }
    case kEndEditMessage:
        // Only for the box still open (a click on another cell has ended it and opened the next).
        if (state && state->edit == reinterpret_cast<HWND>(lparam)) end_edit(*state, wparam != 0);
        return 0;
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
    case DM_GETDEFID:  // Enter in IsDialogMessage: OK
        return MAKELRESULT(kOkId, DC_HASDEFID);
    case WM_COMMAND: {
        if (!state) break;
        const int id = LOWORD(wparam);
        if (id == kOkId || id == IDOK) {
            end_edit(*state, true);
            state->accepted = true;
            state->done = true;
        } else if (id == kCancelId || id == IDCANCEL) {
            state->done = true;
        } else if (id == kResetAllId && !state->keys.empty()) {
            end_edit(*state, false);
            const std::wstring confirm = replaced(tr("punct.reset_all_confirm"), L"{count}",
                                                  std::to_wstring(state->keys.size()));
            if (MessageBoxW(window, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                state->keys.clear();
                show_rows(*state);
            }
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

bool EditorApp::handle_punctuation_command(int control_id, int notification) {
    if (control_id != kPunctOpenId) return false;
    if (notification == BN_CLICKED) run_punctuation_table();
    return true;
}

}  // namespace settings
}  // namespace cxxime
