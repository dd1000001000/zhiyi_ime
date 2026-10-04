// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/installer_path_security.h>

#include <cstring>
#include <utility>
#include <vector>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

namespace cxxime {
namespace installer {
namespace {

bool normalize_local_path(const std::wstring& path, std::wstring* normalized) {
    if (!normalized || path.empty()) {
        return false;
    }
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                              buffer.data(), nullptr);
        if (length == 0 || length >= 32768) {
            return false;
        }
        if (length < buffer.size()) {
            normalized->assign(buffer.data(), length);
            return normalized->size() >= 3 && (*normalized)[1] == L':' &&
                   (*normalized)[2] == L'\\';
        }
        buffer.resize(length + 1);
    }
}

bool path_ancestors_are_directories(const std::wstring& path, std::wstring* normalized) {
    if (!normalize_local_path(path, normalized)) {
        return false;
    }
    for (size_t end = 3; end <= normalized->size(); ++end) {
        if (end != normalized->size() && (*normalized)[end] != L'\\') {
            continue;
        }
        const std::wstring component = normalized->substr(0, end);
        const DWORD attributes = GetFileAttributesW(component.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return false;
        }
    }
    return true;
}

bool trusted_machine_principal(PSID sid) {
    if (!sid || !IsValidSid(sid)) {
        return false;
    }
    BYTE system_buffer[SECURITY_MAX_SID_SIZE] = {};
    BYTE administrators_buffer[SECURITY_MAX_SID_SIZE] = {};
    DWORD system_size = sizeof(system_buffer);
    DWORD administrators_size = sizeof(administrators_buffer);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer, &system_size) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_buffer,
                            &administrators_size)) {
        return false;
    }
    if (EqualSid(sid, system_buffer) || EqualSid(sid, administrators_buffer)) {
        return true;
    }
    const UCHAR count = *GetSidSubAuthorityCount(sid);
    const SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    return count > 0 &&
           std::memcmp(GetSidIdentifierAuthority(sid), &nt_authority, sizeof(nt_authority)) == 0 &&
           *GetSidSubAuthority(sid, 0) == SECURITY_SERVICE_ID_BASE_RID;
}

PSID validate_ace_sid(ACE_HEADER* header, BYTE* sid) {
    BYTE* ace_end = reinterpret_cast<BYTE*>(header) + header->AceSize;
    if (sid > ace_end || static_cast<size_t>(ace_end - sid) < sizeof(SID) || !IsValidSid(sid) ||
        GetLengthSid(sid) > static_cast<DWORD>(ace_end - sid)) {
        return nullptr;
    }
    return sid;
}

PSID allowed_ace_sid(void* raw_ace) {
    auto* header = static_cast<ACE_HEADER*>(raw_ace);
    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE ||
        header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE) {
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
        return validate_ace_sid(header, reinterpret_cast<BYTE*>(&ace->SidStart));
    }
    if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
        header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
        auto* ace = static_cast<ACCESS_ALLOWED_OBJECT_ACE*>(raw_ace);
        BYTE* sid = reinterpret_cast<BYTE*>(&ace->Flags) + sizeof(ace->Flags);
        if (ace->Flags & ACE_OBJECT_TYPE_PRESENT) {
            sid += sizeof(GUID);
        }
        if (ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) {
            sid += sizeof(GUID);
        }
        return validate_ace_sid(header, sid);
    }
    return nullptr;
}

bool object_prevents_untrusted_access(const std::wstring& path, ACCESS_MASK dangerous_rights) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result =
        GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                              nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS || !owner || !trusted_machine_principal(owner) || !dacl ||
        !IsValidAcl(dacl)) {
        if (descriptor) {
            LocalFree(descriptor);
        }
        return false;
    }

    bool protected_parent = true;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace)) {
            protected_parent = false;
            break;
        }
        auto* header = static_cast<ACE_HEADER*>(raw_ace);
        if (header->AceFlags & INHERIT_ONLY_ACE) {
            continue;
        }
        PSID sid = allowed_ace_sid(raw_ace);
        if (!sid) {
            continue;
        }
        const ACCESS_MASK mask = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace)->Mask;
        if ((mask & dangerous_rights) != 0 && !trusted_machine_principal(sid)) {
            protected_parent = false;
            break;
        }
    }
    LocalFree(descriptor);
    return protected_parent;
}

bool directory_is_empty(const std::wstring& path) {
    WIN32_FIND_DATAW entry = {};
    const std::wstring pattern = path + L"\\*";
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool empty = true;
    do {
        if (wcscmp(entry.cFileName, L".") != 0 && wcscmp(entry.cFileName, L"..") != 0) {
            empty = false;
            break;
        }
    } while (FindNextFileW(search, &entry) != FALSE);
    FindClose(search);
    return empty;
}

bool apply_protected_security(const std::wstring& path, PSID protected_owner,
                              PACL protected_dacl) {
    return SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                 OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                     PROTECTED_DACL_SECURITY_INFORMATION,
                                 protected_owner, nullptr, protected_dacl,
                                 nullptr) == ERROR_SUCCESS;
}

bool inspect_install_tree(const std::wstring& root, PSID protected_owner, PACL protected_dacl,
                          bool apply_security) {
    constexpr ACCESS_MASK kModificationRights =
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
        FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    std::vector<std::wstring> pending = {root};
    while (!pending.empty()) {
        const std::wstring directory = std::move(pending.back());
        pending.pop_back();
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return false;
        }
        if (protected_dacl) {
            if (!object_prevents_untrusted_access(directory, kModificationRights) ||
                (apply_security &&
                 !apply_protected_security(directory, protected_owner, protected_dacl))) {
                return false;
            }
        }

        WIN32_FIND_DATAW entry = {};
        const std::wstring pattern = directory + L"\\*";
        HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) {
                continue;
            }
            return false;
        }
        bool valid = true;
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                valid = false;
                break;
            }
            const std::wstring child = directory + L"\\" + entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                pending.push_back(child);
            } else if (protected_dacl) {
                if (!object_prevents_untrusted_access(child, kModificationRights) ||
                    (apply_security &&
                     !apply_protected_security(child, protected_owner, protected_dacl))) {
                    valid = false;
                    break;
                }
            }
        } while (FindNextFileW(search, &entry) != FALSE);
        const DWORD find_error = GetLastError();
        FindClose(search);
        if (!valid || (find_error != ERROR_NO_MORE_FILES && find_error != ERROR_SUCCESS)) {
            return false;
        }
    }
    return true;
}

} // namespace

int secure_install_root(const std::wstring& path) {
    std::wstring normalized;
    if (!normalize_local_path(path, &normalized)) {
        return 1;
    }
    const size_t separator = normalized.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator < 2) {
        return 1;
    }
    const std::wstring parent =
        separator == 2 ? normalized.substr(0, 3) : normalized.substr(0, separator);
    // The install root may live on any local drive (data drives usually let Authenticated Users
    // modify their root), so only the root and its contents are locked down; the ancestors just
    // have to be real directories, not links.
    std::wstring normalized_parent;
    if (!path_ancestors_are_directories(parent, &normalized_parent)) {
        return 1;
    }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // AppContainer processes use a restricted-token access check. BUILTIN\Users alone is not
    // sufficient after inheritance is disabled, so preserve read/execute access for packaged
    // input hosts through ALL APPLICATION PACKAGES.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)"
            L"(A;OICI;GRGX;;;AC)",
            SDDL_REVISION_1, &descriptor, nullptr)) {
        return 1;
    }
    BOOL owner_defaulted = FALSE;
    PSID owner = nullptr;
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted) || !owner ||
        !GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted) ||
        !dacl_present || !dacl) {
        LocalFree(descriptor);
        return 1;
    }
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            LocalFree(descriptor);
            return 1;
        }
        SECURITY_ATTRIBUTES security = {};
        security.nLength = sizeof(security);
        security.lpSecurityDescriptor = descriptor;
        if (!CreateDirectoryW(normalized.c_str(), &security)) {
            LocalFree(descriptor);
            return 1;
        }
    } else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        LocalFree(descriptor);
        return 1;
    }
    // An existing empty folder (for example one the user created in the folder picker) holds
    // nothing that could have been tampered with, so it is adopted and locked down. A non-empty
    // tree must already be protected before it is trusted.
    const bool adopt_empty_root = directory_is_empty(normalized);

    HANDLE root_handle = CreateFileW(
        normalized.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (root_handle == INVALID_HANDLE_VALUE) {
        LocalFree(descriptor);
        return 1;
    }
    const bool secured =
        (adopt_empty_root ? apply_protected_security(normalized, owner, dacl)
                          : inspect_install_tree(normalized, owner, dacl, false)) &&
        inspect_install_tree(normalized, owner, dacl, true);
    CloseHandle(root_handle);
    LocalFree(descriptor);
    return secured ? 0 : 1;
}

int validate_install_directory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : 1;
    }
    std::wstring normalized;
    return path_ancestors_are_directories(path, &normalized) &&
                   inspect_install_tree(normalized, nullptr, nullptr, false)
               ? 0
               : 1;
}

} // namespace installer
} // namespace cxxime
