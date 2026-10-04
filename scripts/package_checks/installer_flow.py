# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

from package_checks.common import forbid_text, require_order, require_text


def check_installer_flow(
    errors: list[str],
    text: str,
    install_text: str,
    uninstall_text: str,
) -> None:
    label = "zhiyi-setup.nsi"
    for item in (
        "Function AcquireInstallerMutex",
        "Function PrepareInstallLifecycle",
        "Function UpgradeLegacyInstall",
        "Function CheckInstallVersion",
        "Function PrepareLegacyDeferredUninstall",
        "Function RestoreLegacyInstall",
        "Function CompleteLegacyUninstallHandoff",
        "Function CleanupLegacyInstallFiles",
        "Function LoadPreparedInstallTarget",
        "Function CommitInstallLifecycle",
        "Function CollectInstallGarbage",
        "Function un.CommitInstallLifecycle",
        "Function un.ValidateInstallLifecycle",
        "Function CollectPreviousVersionLockNotice",
        "Function RecoverInterruptedInstall",
        "Function PrepareSystemImeUpdate",
        "Function WriteTransactionState",
        "Function RollbackInstall",
        "Function un.PrepareTransaction",
        "Function un.RollbackTransaction",
        "Function un.CheckFileLocks",
        "!insertmacro MUI_PAGE_FINISH",
        "!define MUI_FINISHPAGE_RUN_NOTCHECKED",
        "!define MUI_FINISHPAGE_NOREBOOTSUPPORT",
        'StrCpy $LockReportPath "$PLUGINSDIR\\zhiyi-locks.txt"',
        'StrCpy $InstallBaseDir "$PROGRAMFILES64\\ZhiyiIME"',
        "Call ReleaseInstallerMutex",
        'ExecWait \'"$RegisteredInstallDir\\uninstall.exe" /S\'',
        'FileWriteUTF16LE $0 "state=removing$\\r$\\n"',
        'WriteINIStr "$InstallBaseDir\\${SYSTEM_IME_REMOVE_MARKER}"',
        "Call CompleteLegacyUninstallHandoff",
        "Call CleanupLegacyInstallFiles",
        "compare-version",
        '"$InstalledVersion" "${VERSION}"',
        "/ALLOWDOWNGRADE",
        'StrCmp $LegacyUninstallPerformed "1" fresh_install_base_ready',
        'secure-install-root "$InstallBaseDir"',
        'validate-install-directory "$StageDir"',
        'IfFileExists "$InstallBaseDir\\maintenance\\install-state.json"',
        'IfFileExists "$RegisteredInstallDir\\${INSTALL_MARKER}" 0 setup_unknown_install',
        'IfFileExists "$RegisteredInstallDir\\uninstall.exe" 0 setup_unknown_install',
        'IfFileExists "$InstallBaseDir\\$1\\install-manifest.json"',
        'lifecycle-prepare "$InstallBaseDir"',
        "lifecycle-prepared-target",
        'lifecycle-commit "$InstallBaseDir"',
        "lifecycle-gc",
        'lifecycle-uninstall "$InstallBaseDir"',
        "lifecycle-validate-uninstall",
        'ReadINIStr $LifecycleRemaining "$LifecycleResultPath" "lifecycle" "remaining"',
        'ReadINIStr $LifecycleUnknown "$LifecycleResultPath" "lifecycle" "unknown"',
        'StrCmp $LifecycleRemaining "0" +2',
        'StrCpy $UninstallCleanupWarning 1',
        'RMDir /r /REBOOTOK "$RegisteredInstallDir\\.zhiyi-rollback"',
        'RMDir /r /REBOOTOK "$RegisteredInstallDir\\.zhiyi-uninstall-rollback"',
        'RMDir /r /REBOOTOK "$InstallBaseDir\\update"',
        'RMDir /r /REBOOTOK "$InstallBaseDir\\.zhiyi-backup"',
        'StrCpy $4 "$InstallBaseDir\\maintenance\\ime-$3-x64.pending"',
        'StrCpy $5 "$InstallBaseDir\\maintenance\\ime-$3-x86.pending"',
        'IfFileExists "$InstallBaseDir\\maintenance\\ime-*.pending"',
        "LEGACY_SYSTEM_IME_X64_PENDING",
        "LEGACY_SYSTEM_IME_X86_PENDING",
        "un_remove_system_ime_after_pending:",
    ):
        require_text(errors, text, item, label)

    require_order(
        errors,
        install_text,
        [
            "Call CheckInstallVersion",
            "Call UpgradeLegacyInstall",
            "Call PrepareInstallTarget",
            "Call CheckFreshInstallBase",
            "Call SecureInstallBase",
            "Call LoadPreparedInstallTarget",
            "Call CaptureServerState",
            "Call ReleaseInputProcessor",
            "Call StopServer",
            "Call RecoverInterruptedInstall",
            "Call PrepareInstallLifecycle",
            "Call CheckInstallDirectory",
            "Call SnapshotPreviousState",
            "!insertmacro InstallVersionPayload",
            "Call WriteTransactionState",
            'Rename "$StageDir" "$INSTDIR"',
            "Call RegisterNewTsf",
            "Call WriteInstallationRegistry",
            "Call StartNewServer",
            "Call PrepareSystemImeUpdate",
            "Call WriteInstallMarker",
            "Call CollectPreviousVersionLockNotice",
            "Call CommitInstallLifecycle",
            'Delete "$INSTDIR\\${TRANSACTION_MARKER}"',
            "Call CollectInstallGarbage",
            "Call CopyNewSystemIme",
        ],
        "Install lifecycle",
    )
    require_order(
        errors,
        uninstall_text,
        [
            "Call un.ReleaseInputProcessor",
            "Call un.StopServer",
            "Call un.CheckFileLocks",
            "Call un.ValidateInstallLifecycle",
            "Call un.PrepareTransaction",
            "Call un.PrepareSystemImeRemoval",
            "Call un.UnregisterInstalledTsf",
            'DeleteRegKey HKLM "${UNINSTALL_KEY}"',
            "Call un.RemoveSystemIme",
            "Call un.CommitInstallLifecycle",
        ],
        "Uninstall lifecycle",
    )
    require_order(
        errors,
        uninstall_text,
        [
            'Delete /REBOOTOK "$InstallBaseDir\\maintenance\\install-state.json"',
            'RMDir /REBOOTOK "$InstallBaseDir\\maintenance"',
            'RMDir /REBOOTOK "$InstallBaseDir"',
        ],
        "Uninstall root reboot cleanup",
    )

    for obsolete in (
        "CheckPreviousVersionLimit",
        "RecoverPendingSystemIme",
        "CleanupPreviousInstall",
        "WriteInstallLayoutState",
        "StageInstalledFiles",
        "DeleteStagedFiles",
        "BeginDeferredUninstall",
        "CommitDeferredUninstall",
        "MUI_UNPAGE_CONFIRM",
        "--prompt=",
        "INSTALL_PENDING_",
        'StrCmp $InstalledVersion "0.4.0" setup_mark_legacy_install',
    ):
        forbid_text(errors, text, obsolete, label)

    fresh_start = text.find("Function CheckFreshInstallBase")
    fresh_end = text.find("FunctionEnd", fresh_start)
    if fresh_start < 0 or fresh_end < 0:
        errors.append("Fresh install directory: missing function")
    else:
        fresh_block = text[fresh_start:fresh_end]
        require_order(
            errors,
            fresh_block,
            [
                'FindFirst $0 $1 "$InstallBaseDir\\*"',
                'StrCmp $1 "." fresh_install_base_next',
                'StrCmp $1 "maintenance" fresh_install_base_next',
                'IfFileExists "$InstallBaseDir\\$1\\install-manifest.json"',
                'FindClose $0',
                "$(L_083)",  # 所选产品目录包含不属于知意输入法的文件
                "fresh_install_base_next:",
                "FindNext $0 $1",
                "fresh_install_base_empty:",
                "fresh_install_base_ready:",
                "Push 1",
            ],
            "Fresh install directory",
        )

    # The install finish page never asks to restart; the uninstall one offers restart now /
    # later when files in use are left for the next restart (later preselected).
    if text.count("!define MUI_FINISHPAGE_NOREBOOTSUPPORT") != 1:
        errors.append("Finish pages: only the install finish page suppresses restart choices")
    if text.count("!define MUI_FINISHPAGE_REBOOTLATER_DEFAULT") != 1:
        errors.append("Finish pages: the uninstall restart choice must default to later")
    if text.count('StrCpy $LifecycleResultPath "$PLUGINSDIR\\zhiyi-lifecycle.ini"') != 2:
        errors.append("Lifecycle result path: must be initialized after each InitPluginsDir")

    lock_start = text.find("Function un.CheckFileLocks")
    lock_end = text.find("FunctionEnd", lock_start)
    if lock_start < 0 or lock_end < 0:
        errors.append("Uninstall lock check: missing function")
    else:
        lock_block = text[lock_start:lock_end]
        for obsolete in ("MessageBox", "Abort", "un_lock_retry"):
            forbid_text(errors, lock_block, obsolete, "Uninstall lock check")

    registry_start = text.find("Function WriteInstallationRegistry")
    registry_end = text.find("FunctionEnd", registry_start)
    if registry_start < 0 or registry_end < 0:
        errors.append("Installation registry: missing function")
    else:
        forbid_text(
            errors,
            text[registry_start:registry_end],
            'WriteRegStr HKLM "${UNINSTALL_KEY}" "PreviousInstallLocation"',
            "Installation registry",
        )

    require_order(
        errors,
        text,
        [
            "Function un.ConfirmPage",
            "$(L_077)",  # 用户配置和词库默认保留
            "$(L_078)",  # 删除用户配置和词库数据
            "$(L_079)",  # 个人数据将永久删除，无法撤销
            'SendMessage $0 ${WM_SETTEXT} 0 "$(L_080)"',  # STR:卸载
            "Function un.ToggleRemoveUserDataWarning",
            "ShowWindow $UninstallRemoveUserDataWarning ${SW_SHOW}",
        ],
        "Uninstall confirmation page",
    )
