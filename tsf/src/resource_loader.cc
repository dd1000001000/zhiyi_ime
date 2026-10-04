// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "resource_loader.h"
#include "globals.h"
#include <cxxime/logging.h>
#include <cwchar>
#include <strsafe.h>

namespace cxxime_tsf {

namespace {

constexpr wchar_t kResourceDllName[] = L"zhiyi-resources.dll";

HMODULE load_resource_module() {
    wchar_t path[MAX_PATH] = {};
    if (!get_resource_dll_path(path, ARRAYSIZE(path))) {
        CXXIME_LOG(L"Resource DLL not found beside TSF DLL");
        return nullptr;
    }

    HMODULE module = LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        CXXIME_LOG(L"LoadLibraryEx failed for resource DLL: %ls, err=%lu", path, GetLastError());
    }
    return module;
}

HMODULE resource_module() {
    static HMODULE module = load_resource_module();
    return module;
}

}  // namespace

bool get_resource_dll_path(wchar_t* path, DWORD path_chars) {
    if (!path || path_chars == 0) {
        return false;
    }

    DWORD len = GetModuleFileNameW(g_hInst, path, path_chars);
    if (len == 0 || len >= path_chars) {
        path[0] = L'\0';
        return false;
    }

    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        path[0] = L'\0';
        return false;
    }
    *(slash + 1) = L'\0';

    HRESULT hr = StringCchCatW(path, path_chars, kResourceDllName);
    if (FAILED(hr)) {
        path[0] = L'\0';
        return false;
    }

    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        path[0] = L'\0';
        return false;
    }

    return true;
}

HICON load_resource_icon(UINT resource_id, int cx, int cy) {
    HMODULE module = resource_module();
    if (!module) {
        return nullptr;
    }
    return reinterpret_cast<HICON>(
        LoadImageW(module, MAKEINTRESOURCEW(resource_id), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
}

}  // namespace cxxime_tsf
