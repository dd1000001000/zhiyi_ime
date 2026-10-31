// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "globals.h"

#include "class_factory.h"
#include "register.h"
#include "text_service.h"
#include "tsf_log_writer.h"

#include <cxxime/data_path.h>

// Forward declarations for DllRegisterServer/DllUnregisterServer
STDAPI DllUnregisterServer();

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID) {
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        g_hInst = hInst;
        cxxime::set_module_handle(hInst);
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_cs);
        break;
    case DLL_PROCESS_DETACH:
        // Normal teardown joins outside the loader lock when the last service unsubscribes.
        cxxime_tsf::request_tsf_log_writer_stop();
        cxxime::remove_all_thread_key_hooks();  // normally removed at deactivation
        DeleteCriticalSection(&g_cs);
        break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;

    if (!IsEqualCLSID(rclsid, c_clsidTextService)) return CLASS_E_CLASSNOTAVAILABLE;

    EnterCriticalSection(&g_cs);

    static ClassFactory s_classFactory;
    HRESULT hr = s_classFactory.QueryInterface(riid, ppv);

    LeaveCriticalSection(&g_cs);
    return hr;
}

STDAPI DllCanUnloadNow() {
    const bool writer_active = cxxime_tsf::tsf_log_writer_has_thread();
    const bool unloadable = g_cRefDll < 0 && !writer_active;
    return unloadable ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    HRESULT hr;

    hr = register_server();
    if (FAILED(hr)) goto cleanup;

    hr = register_profiles();
    if (FAILED(hr)) goto cleanup;

    hr = register_categories();
    if (FAILED(hr)) goto cleanup;

    return S_OK;

cleanup:
    DllUnregisterServer();
    return hr;
}

STDAPI DllUnregisterServer() {
    HRESULT result = S_OK;
    const HRESULT categories_hr = unregister_categories();
    if (FAILED(categories_hr)) {
        result = categories_hr;
    }
    const HRESULT profiles_hr = unregister_profiles();
    if (FAILED(profiles_hr) && SUCCEEDED(result)) {
        result = profiles_hr;
    }
    const HRESULT server_hr = unregister_server();
    if (FAILED(server_hr) && SUCCEEDED(result)) {
        result = server_hr;
    }
    return result;
}
