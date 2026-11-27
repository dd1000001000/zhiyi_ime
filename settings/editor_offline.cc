// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// "Import from file" for the translation model, the voice models and the language packs (issue #52): the
// file dialog and the checks every part shares (update.h: open_offline_package). What is imported is
// installed exactly as a download (editor_translator.cc, editor_voice_panel.cc, editor_learning_panel.cc).

#include "editor_app_internal.h"
#include "i18n.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace cxxime {
namespace settings {

namespace {

std::wstring pick_file(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(tr("offline.pick_title"));
    const std::wstring packages = tr("offline.file_type");
    const COMDLG_FILTERSPEC filters[] = {{packages.c_str(), L"*.zip;*.json"}, {tr("backup.all_files"), L"*.*"}};
    dialog->SetFileTypes(2, filters);
    // Where a browser saves the package, unless the dialog remembers a folder from before.
    IShellItem* downloads = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&downloads)))) {
        dialog->SetDefaultFolder(downloads);
        downloads->Release();
    }
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

// What the package is, and where it is imported (offline.kind_*, offline.place_*).
const char* kind_key(update::PackageKind kind) {
    switch (kind) {
    case update::PackageKind::kTranslator: return "translator";
    case update::PackageKind::kVoice:
    case update::PackageKind::kQwenVoice: return "voice";
    case update::PackageKind::kGlossary: return "glossary";
    case update::PackageKind::kNone: break;
    }
    return "";
}

std::wstring fill(std::wstring text, std::initializer_list<std::wstring> values) {
    int i = 0;
    for (const std::wstring& value : values) {
        const std::wstring mark = L"{" + std::to_wstring(i++) + L"}";
        const size_t at = text.find(mark);
        if (at != std::wstring::npos) text.replace(at, mark.size(), value);
    }
    return text;
}

}  // namespace

bool pick_offline_package(HWND owner, update::PackageKind want, update::OfflinePackage* package) {
    const std::wstring path = pick_file(owner);
    if (path.empty()) return false;
    update::Status status;
    {
        WaitCursor wait;
        status = update::open_offline_package(path, package);
    }
    if (status != update::Status::kOk) {
        MessageBoxW(owner, tr(status == update::Status::kDisk ? "offline.unreadable" : "offline.invalid"),
                    tr("window.title"), MB_OK | MB_ICONWARNING);
        return false;
    }
    const bool voice = want == update::PackageKind::kVoice &&
                       (package->kind == update::PackageKind::kVoice || package->kind == update::PackageKind::kQwenVoice);
    if (package->kind != want && !voice) {
        const std::string kind = kind_key(package->kind);
        const std::wstring text = fill(tr("offline.wrong_kind"), {tr(("offline.kind_" + kind).c_str()),
                                                                  tr(("offline.place_" + kind).c_str())});
        MessageBoxW(owner, text.c_str(), tr("window.title"), MB_OK | MB_ICONINFORMATION);
        return false;
    }
    return true;
}

std::wstring offline_progress_text(bool importing, std::uint64_t done_mb, std::uint64_t total_mb) {
    return fill(tr(importing ? "learning.importing" : "learning.downloading"),
                {std::to_wstring(done_mb), std::to_wstring(total_mb)});
}

}  // namespace settings
}  // namespace cxxime
