// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SERVER_USER_BACKUP_SERVICE_H_
#define CXXIME_SERVER_USER_BACKUP_SERVICE_H_

#include <cstdint>
#include <string>

#include <cxxime/user_backup.h>

class ConfigWriteCoordinator;
class SessionManager;

class UserBackupService {
public:
    UserBackupService(SessionManager* session_manager, ConfigWriteCoordinator* config_writer);

    bool handle_request(const std::string& payload, std::string* response_payload);

private:
    bool export_backup(const std::wstring& path, std::uint32_t components,
                       cxxime::UserBackupSummary* summary, unsigned long* error);

    SessionManager* session_manager_;
    ConfigWriteCoordinator* config_writer_;
};

#endif // CXXIME_SERVER_USER_BACKUP_SERVICE_H_
