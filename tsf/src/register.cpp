// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// The input profile icon is registered from cxxime-resources.dll. The TSF
// module does not embed icon resources.

#include "register.h"
#include "globals.h"
#include "resource_loader.h"
#include <imm.h>
#include <shlwapi.h>
#include <cwchar>

#pragma comment(lib, "shlwapi.lib")

namespace {

constexpr wchar_t kKeyboardLayoutsKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";
constexpr wchar_t kLegacyImeFile[] = L"zhiyi.ime";
constexpr wchar_t kLegacyLayoutFile[] = L"kbdus.dll";
constexpr DWORD kLegacyImeStart = 0xE0200000;
constexpr DWORD kLegacyImeEnd = 0xE0FF0000;

DWORD layout_id_for_langid(DWORD id_prefix, LANGID langid) {
    return id_prefix | static_cast<DWORD>(langid);
}

HKL hkl_from_layout_id(DWORD layout_id) {
    return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(layout_id));
}

bool format_layout_key(DWORD layout_id, WCHAR* buffer, size_t cch_buffer) {
    if (!buffer || cch_buffer < 9) {
        return false;
    }
    return swprintf_s(buffer, cch_buffer, L"%08X", layout_id) > 0;
}

bool query_layout_ime_file(HKEY layouts_key, DWORD layout_id, WCHAR* value, DWORD cch_value) {
    WCHAR key_name[9] = {};
    if (!format_layout_key(layout_id, key_name, ARRAYSIZE(key_name))) {
        return false;
    }

    HKEY layout_key = nullptr;
    LONG lr = RegOpenKeyExW(layouts_key, key_name, 0, KEY_READ, &layout_key);
    if (lr != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD cb_value = cch_value * sizeof(WCHAR);
    lr = RegQueryValueExW(layout_key, L"Ime File", nullptr, &type, reinterpret_cast<BYTE*>(value),
                          &cb_value);
    RegCloseKey(layout_key);
    return lr == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0';
}

bool ime_file_matches(const WCHAR* ime_file) {
    if (!ime_file || ime_file[0] == L'\0') {
        return false;
    }

    const WCHAR* base = wcsrchr(ime_file, L'\\');
    if (!base) {
        base = wcsrchr(ime_file, L'/');
    }
    base = base ? base + 1 : ime_file;
    return _wcsicmp(base, kLegacyImeFile) == 0;
}

bool is_cxxime_legacy_layout(HKEY layouts_key, DWORD layout_id) {
    WCHAR ime_file[MAX_PATH] = {};
    return query_layout_ime_file(layouts_key, layout_id, ime_file, ARRAYSIZE(ime_file)) &&
           ime_file_matches(ime_file);
}

bool parse_layout_id(const WCHAR* key_name, DWORD* layout_id) {
    if (!key_name || !layout_id) {
        return false;
    }

    WCHAR* end = nullptr;
    unsigned long value = wcstoul(key_name, &end, 16);
    if (!end || *end != L'\0') {
        return false;
    }
    *layout_id = static_cast<DWORD>(value);
    return true;
}

HKL find_legacy_ime_hkl(LANGID langid) {
    HKEY layouts_key = nullptr;
    LONG lr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKeyboardLayoutsKey, 0, KEY_READ, &layouts_key);
    if (lr != ERROR_SUCCESS) {
        return nullptr;
    }

    HKL hkl = nullptr;
    for (DWORD index = 0; !hkl; ++index) {
        WCHAR key_name[32] = {};
        DWORD cch_key_name = ARRAYSIZE(key_name);
        lr = RegEnumKeyExW(layouts_key, index, key_name, &cch_key_name, nullptr, nullptr, nullptr,
                           nullptr);
        if (lr != ERROR_SUCCESS) {
            break;
        }

        DWORD layout_id = 0;
        if (parse_layout_id(key_name, &layout_id) &&
            (layout_id & 0xFFFF) == static_cast<DWORD>(langid) &&
            is_cxxime_legacy_layout(layouts_key, layout_id)) {
            hkl = hkl_from_layout_id(layout_id);
        }
    }

    RegCloseKey(layouts_key);
    return hkl;
}

HRESULT install_legacy_ime_hkl(LANGID langid, HKL* hkl) {
    if (!hkl) {
        return E_POINTER;
    }
    *hkl = nullptr;

#ifdef _WIN64
    WCHAR system_dir[MAX_PATH] = {};
    if (GetSystemDirectoryW(system_dir, ARRAYSIZE(system_dir)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WCHAR ime_path[MAX_PATH] = {};
    if (swprintf_s(ime_path, ARRAYSIZE(ime_path), L"%s\\%s", system_dir, kLegacyImeFile) <= 0) {
        return E_FAIL;
    }
    if (GetFileAttributesW(ime_path) == INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HKL installed = ImmInstallIMEW(ime_path, TEXTSERVICE_DESC);
    if (installed) {
        *hkl = installed;
        return S_OK;
    }

    const DWORD last_error = GetLastError();
    HKL existing = find_legacy_ime_hkl(langid);
    if (existing) {
        *hkl = existing;
        return S_OK;
    }
    return HRESULT_FROM_WIN32(last_error ? last_error : ERROR_INSTALL_FAILURE);
#else
    UNREFERENCED_PARAMETER(langid);
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
#endif
}

HRESULT create_legacy_ime_hkl_manually(LANGID langid, HKL* hkl) {
    if (!hkl) {
        return E_POINTER;
    }
    *hkl = nullptr;

    HKEY layouts_key = nullptr;
    LONG lr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKeyboardLayoutsKey, 0, KEY_READ | KEY_WRITE,
                            &layouts_key);
    if (lr != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(lr);
    }

    for (DWORD id_prefix = kLegacyImeStart; id_prefix <= kLegacyImeEnd; id_prefix += 0x10000) {
        const DWORD layout_id = layout_id_for_langid(id_prefix, langid);
        WCHAR key_name[9] = {};
        format_layout_key(layout_id, key_name, ARRAYSIZE(key_name));

        HKEY existing_key = nullptr;
        lr = RegOpenKeyExW(layouts_key, key_name, 0, KEY_READ, &existing_key);
        if (lr == ERROR_SUCCESS) {
            RegCloseKey(existing_key);
            continue;
        }
        if (lr != ERROR_FILE_NOT_FOUND) {
            RegCloseKey(layouts_key);
            return HRESULT_FROM_WIN32(lr);
        }

        HKEY layout_key = nullptr;
        lr = RegCreateKeyExW(layouts_key, key_name, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
                             nullptr, &layout_key, nullptr);
        if (lr != ERROR_SUCCESS) {
            RegCloseKey(layouts_key);
            return HRESULT_FROM_WIN32(lr);
        }

        auto set_string = [&](const wchar_t* name, const wchar_t* value) -> LONG {
            return RegSetValueExW(layout_key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                                  static_cast<DWORD>((wcslen(value) + 1) * sizeof(WCHAR)));
        };

        LONG set_result = set_string(L"Ime File", kLegacyImeFile);
        if (set_result == ERROR_SUCCESS) {
            set_result = set_string(L"Layout File", kLegacyLayoutFile);
        }
        if (set_result == ERROR_SUCCESS) {
            set_result = set_string(L"Layout Text", TEXTSERVICE_DESC);
        }

        RegCloseKey(layout_key);
        if (set_result != ERROR_SUCCESS) {
            RegDeleteKeyW(layouts_key, key_name);
            RegCloseKey(layouts_key);
            return HRESULT_FROM_WIN32(set_result);
        }

        *hkl = hkl_from_layout_id(layout_id);
        RegCloseKey(layouts_key);
        return S_OK;
    }

    RegCloseKey(layouts_key);
    return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

HRESULT ensure_legacy_ime_hkl(LANGID langid, HKL* hkl) {
    if (!hkl) {
        return E_POINTER;
    }

    *hkl = find_legacy_ime_hkl(langid);
    if (*hkl) {
        return S_OK;
    }

    HRESULT hr = install_legacy_ime_hkl(langid, hkl);
    if (SUCCEEDED(hr) && *hkl) {
        return S_OK;
    }
    return create_legacy_ime_hkl_manually(langid, hkl);
}

void remove_preload_layout(const WCHAR* layout_name) {
    if (!layout_name || layout_name[0] == L'\0') {
        return;
    }

    HKEY preload_key = nullptr;
    LONG lr = RegOpenKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout\\Preload", 0, KEY_READ | KEY_WRITE,
                            &preload_key);
    if (lr != ERROR_SUCCESS) {
        return;
    }

    for (DWORD index = 0;;) {
        WCHAR value_name[32] = {};
        DWORD cch_value_name = ARRAYSIZE(value_name);
        WCHAR data[32] = {};
        DWORD cb_data = sizeof(data);
        DWORD type = 0;
        lr = RegEnumValueW(preload_key, index, value_name, &cch_value_name, nullptr, &type,
                           reinterpret_cast<BYTE*>(data), &cb_data);
        if (lr != ERROR_SUCCESS) {
            break;
        }

        if (type == REG_SZ && _wcsicmp(data, layout_name) == 0) {
            RegDeleteValueW(preload_key, value_name);
            continue;
        }
        ++index;
    }

    RegCloseKey(preload_key);
}

void unregister_legacy_ime_hkl(LANGID langid) {
    HKEY layouts_key = nullptr;
    LONG lr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKeyboardLayoutsKey, 0, KEY_READ | KEY_WRITE,
                            &layouts_key);
    if (lr != ERROR_SUCCESS) {
        return;
    }

    WCHAR keys_to_delete[32][32] = {};
    DWORD delete_count = 0;
    for (DWORD index = 0; index < 512 && delete_count < ARRAYSIZE(keys_to_delete); ++index) {
        WCHAR key_name[32] = {};
        DWORD cch_key_name = ARRAYSIZE(key_name);
        lr = RegEnumKeyExW(layouts_key, index, key_name, &cch_key_name, nullptr, nullptr, nullptr,
                           nullptr);
        if (lr != ERROR_SUCCESS) {
            break;
        }

        DWORD layout_id = 0;
        if (!parse_layout_id(key_name, &layout_id) ||
            (layout_id & 0xFFFF) != static_cast<DWORD>(langid) ||
            !is_cxxime_legacy_layout(layouts_key, layout_id)) {
            continue;
        }

        wcscpy_s(keys_to_delete[delete_count], ARRAYSIZE(keys_to_delete[delete_count]), key_name);
        ++delete_count;
    }

    for (DWORD i = 0; i < delete_count; ++i) {
        DWORD layout_id = 0;
        if (parse_layout_id(keys_to_delete[i], &layout_id)) {
            UnloadKeyboardLayout(hkl_from_layout_id(layout_id));
        }
        remove_preload_layout(keys_to_delete[i]);
        RegDeleteKeyW(layouts_key, keys_to_delete[i]);
    }

    RegCloseKey(layouts_key);
}

} // namespace

HRESULT register_server() {
    WCHAR dll_path[MAX_PATH] = {};
    const DWORD module_length = GetModuleFileNameW(g_hInst, dll_path, MAX_PATH);
    if (module_length == 0 || module_length >= MAX_PATH) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WCHAR key_path[256] = {};
    wsprintfW(key_path, L"CLSID\\{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              c_clsidTextService.Data1, c_clsidTextService.Data2, c_clsidTextService.Data3,
              c_clsidTextService.Data4[0], c_clsidTextService.Data4[1], c_clsidTextService.Data4[2],
              c_clsidTextService.Data4[3], c_clsidTextService.Data4[4], c_clsidTextService.Data4[5],
              c_clsidTextService.Data4[6], c_clsidTextService.Data4[7]);

    HKEY hKey = nullptr;
    LONG lr = RegCreateKeyExW(HKEY_CLASSES_ROOT, key_path, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (lr != ERROR_SUCCESS)
        return E_FAIL;

    lr = RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)TEXTSERVICE_DESC,
                        (DWORD)((wcslen(TEXTSERVICE_DESC) + 1) * sizeof(WCHAR)));
    if (lr != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return HRESULT_FROM_WIN32(lr);
    }

    HKEY hSubKey = nullptr;
    lr = RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &hSubKey, nullptr);
    if (lr != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return HRESULT_FROM_WIN32(lr);
    }
    lr = RegSetValueExW(hSubKey, nullptr, 0, REG_SZ, (const BYTE*)dll_path,
                        (DWORD)((wcslen(dll_path) + 1) * sizeof(WCHAR)));
    if (lr == ERROR_SUCCESS) {
        lr = RegSetValueExW(hSubKey, L"ThreadingModel", 0, REG_SZ,
                            (const BYTE*)TEXTSERVICE_MODEL,
                            (DWORD)((wcslen(TEXTSERVICE_MODEL) + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(hSubKey);
    RegCloseKey(hKey);
    return lr == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(lr);
}

HRESULT unregister_server() {
    WCHAR key_path[256] = {};
    wsprintfW(key_path, L"CLSID\\{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              c_clsidTextService.Data1, c_clsidTextService.Data2, c_clsidTextService.Data3,
              c_clsidTextService.Data4[0], c_clsidTextService.Data4[1], c_clsidTextService.Data4[2],
              c_clsidTextService.Data4[3], c_clsidTextService.Data4[4], c_clsidTextService.Data4[5],
              c_clsidTextService.Data4[6], c_clsidTextService.Data4[7]);

    const LONG lr = SHDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
    return lr == ERROR_SUCCESS || lr == ERROR_FILE_NOT_FOUND
               ? S_OK
               : HRESULT_FROM_WIN32(lr);
}

// Profile registration (Windows 8+ API).

HRESULT register_profiles() {
    WCHAR achIconFile[MAX_PATH] = {};
    if (!cxxime_tsf::get_resource_dll_path(achIconFile, ARRAYSIZE(achIconFile))) {
        return E_FAIL;
    }
    ULONG cchIconFile = (ULONG)wcslen(achIconFile);

    ITfInputProcessorProfileMgr* pProfileMgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_ALL,
                                  IID_ITfInputProcessorProfileMgr, (void**)&pProfileMgr);
    if (FAILED(hr))
        return hr;

    HKL legacy_hkl = nullptr;
    hr = ensure_legacy_ime_hkl(TEXTSERVICE_LANGID_HANS, &legacy_hkl);
    if (FAILED(hr)) {
        pProfileMgr->Release();
        return hr;
    }

    // Bind TSF and IMM as one input identity; remove with legacy installation for TSF-only.
    hr = pProfileMgr->RegisterProfile(
        c_clsidTextService,
        TEXTSERVICE_LANGID_HANS,
        c_guidProfile,
        TEXTSERVICE_DESC,
        (ULONG)wcslen(TEXTSERVICE_DESC),
        achIconFile,
        cchIconFile,
        TEXTSERVICE_ICON_INDEX,
        legacy_hkl,
        0,        // flags
        TRUE,     // enable
        0);

    pProfileMgr->Release();
    return hr;
}

HRESULT unregister_profiles() {
    ITfInputProcessorProfileMgr* pProfileMgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_ALL,
                                  IID_ITfInputProcessorProfileMgr, (void**)&pProfileMgr);
    if (FAILED(hr))
        return hr;

    // Ask TSF to deactivate loaded instances first so TextService::Deactivate can
    // remove its language bar item before the profile registration disappears.
    pProfileMgr->ReleaseInputProcessor(c_clsidTextService,
                                       TF_RIP_FLAG_FREEUNUSEDLIBRARIES);

    hr = pProfileMgr->UnregisterProfile(c_clsidTextService, TEXTSERVICE_LANGID_HANS,
                                        c_guidProfile, 0);
    pProfileMgr->Release();
    unregister_legacy_ime_hkl(TEXTSERVICE_LANGID_HANS);
    return hr;
}

// Category registration.

static const GUID kSupportCategories[] = {
    GUID_TFCAT_TIP_KEYBOARD,
    GUID_TFCAT_TIPCAP_SECUREMODE,
    GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
    GUID_TFCAT_TIPCAP_COMLESS,
    GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
};

HRESULT register_categories() {
    ITfCategoryMgr* pCategoryMgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfCategoryMgr, (void**)&pCategoryMgr);
    if (FAILED(hr)) {
        return hr;
    }

    // Older builds incorrectly advertised activation support for 16-bit tasks.
    pCategoryMgr->UnregisterCategory(
        c_clsidTextService, GUID_TFCAT_TIPCAP_WOW16, c_clsidTextService);

    for (const auto& guid : kSupportCategories) {
        hr = pCategoryMgr->RegisterCategory(c_clsidTextService, guid, c_clsidTextService);
        if (FAILED(hr)) {
            break;
        }
    }

    pCategoryMgr->Release();
    return hr;
}

HRESULT unregister_categories() {
    ITfCategoryMgr* pCategoryMgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfCategoryMgr, (void**)&pCategoryMgr);
    if (FAILED(hr)) {
        return hr;
    }

    for (const auto& guid : kSupportCategories) {
        pCategoryMgr->UnregisterCategory(c_clsidTextService, guid, c_clsidTextService);
    }
    pCategoryMgr->UnregisterCategory(
        c_clsidTextService, GUID_TFCAT_TIPCAP_WOW16, c_clsidTextService);

    pCategoryMgr->Release();
    return S_OK;
}
