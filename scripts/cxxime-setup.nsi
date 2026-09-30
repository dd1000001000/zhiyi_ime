Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "WinMessages.nsh"
!include "Win\WinError.nsh"
!include "x64.nsh"

!define PRODUCT "CxxIME"
!define PUBLISHER "CxxIME Contributors"
!define CLSID "{B7E1E5A2-8F3D-4A9C-B6E7-2C4D8F1A3B5E}"
!define UNINSTALL_KEY "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CxxIME"
!define RUN_KEY "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"
!define TSF_INPROC_KEY "SOFTWARE\Classes\CLSID\${CLSID}\InprocServer32"
!define TSF_TIP_KEY "SOFTWARE\Microsoft\CTF\TIP\${CLSID}"
!define INSTALL_MARKER ".cxxime-install-complete"
!define TRANSACTION_MARKER ".cxxime-install-transaction"
!define TRANSACTION_TEMP ".cxxime-install-transaction.tmp"
!define RUNTIME_MARKER ".cxxime-install-runtime"
!define RUNTIME_TEMP ".cxxime-install-runtime.tmp"
!define SYSTEM_IME_UPDATE_MARKER ".cxxime-ime-update"
!define SYSTEM_IME_REMOVE_MARKER ".cxxime-ime-remove-pending"
!define LEGACY_SYSTEM_IME_X64_PENDING ".cxxime-ime-x64.pending"
!define LEGACY_SYSTEM_IME_X86_PENDING ".cxxime-ime-x86.pending"
!define LEGACY_INSTALL_STATE_MARKER ".cxxime-install-state"
!define LEGACY_INSTALL_STATE_TEMP ".cxxime-install-state.tmp"
!define LEGACY_UNINSTALL_DEFERRED_MARKER ".cxxime-uninstall-pending"
!define UNINSTALL_TRANSACTION_MARKER ".cxxime-uninstall-transaction"
!define UNINSTALL_TRANSACTION_TEMP ".cxxime-uninstall-transaction.tmp"
!define MOVEFILE_REPLACE_WRITE_THROUGH 0x9
!define MOVEFILE_DELAY_UNTIL_REBOOT 0x4
!define MOVEFILE_REPLACE_DELAY_UNTIL_REBOOT 0x5

!ifndef VERSION
    !error "VERSION must be provided by package.py"
!endif
!ifndef VERSION_NUMERIC
    !error "VERSION_NUMERIC must be provided by package.py"
!endif

Name "${PRODUCT} ${VERSION}"
!ifdef HOST_DIAGNOSTICS
    OutFile "cxxime-v${VERSION}-host-diag-setup.exe"
!else
    OutFile "cxxime-v${VERSION}-setup.exe"
!endif
InstallDir "$PROGRAMFILES\CxxIME"
RequestExecutionLevel admin
SetCompressor lzma
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION_NUMERIC}"
VIAddVersionKey /LANG=2052 "CompanyName" "${PUBLISHER}"
VIAddVersionKey /LANG=2052 "FileDescription" "CxxIME 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${VERSION_NUMERIC}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyright (c) 2026 CxxIME Contributors"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${VERSION}"

!define MUI_ICON "cxxime.ico"
!define MUI_UNICON "cxxime.ico"

Var ExistingInstall
Var RegisteredInstallDir
Var StageDir
Var TransactionDir
Var LockReportPath
Var LockReportText
Var InstallLockNotice
Var InstallLockDetailsButton
Var InstallLockDetailsText
Var InstallLockDetailsVisible
Var FailureMessage
Var OldInstallAvailable
Var OldTsfX64Present
Var OldTsfX86Present
Var OldTsfX64Registered
Var OldTsfX86Registered
Var OldTipX64Present
Var OldTipX86Present
Var OldUninstallPresent
Var OldDisplayVersion
Var OldRunPresent
Var OldRunValue
Var ServerWasRunning
Var InitialServerWasRunning
Var TransactionServerWasRunning
Var ServerRestartResult
Var ServerStopResult
Var ServerProcessId
Var InstallStateVerified
Var InstallBaseHandle
Var InstallMutexHandle
Var InstallBaseDir
Var PreviousInstallDir
Var MultiVersionInstall
Var ActiveServerDir
Var StateInstallDir
Var InstallTargetDir
Var InstallTargetPrepared
Var LifecycleResultPath
Var LifecycleActiveArg
Var LifecycleScheduled
Var LifecycleRemaining
Var LifecycleUnknown
Var LegacyUninstallPerformed
Var LegacyUninstallPending
Var InstalledVersion
Var AllowDowngrade
Var UninstallRemoveUserData
Var UninstallRemoveUserDataCheckbox
Var UninstallRemoveUserDataWarning
Var UninstallUserDataDir
Var UninstallUserDataDirSuffix
Var UninstallServerWasRunning
Var UninstallServerStopResult
Var UninstallTransactionPhase
Var UninstallTsfX64Registered
Var UninstallTsfX86Registered
Var UninstallCleanupWarning

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "license.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ValidateInstallDirectory
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_NOREBOOTSUPPORT
!define MUI_FINISHPAGE_RUN "$INSTDIR\cxxime-settings.exe"
!define MUI_FINISHPAGE_RUN_TEXT "启动 CxxIME 设置"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_PAGE_CUSTOMFUNCTION_SHOW FinishPageShow
!insertmacro MUI_PAGE_FINISH
UninstPage custom un.ConfirmPage un.ConfirmPageLeave
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_UNTEXT_FINISH_INFO_REBOOT \
    "CxxIME 已卸载。少量正在使用的程序文件将在下次重新启动 Windows 后自动删除。"
!define MUI_FINISHPAGE_NOREBOOTSUPPORT
!define MUI_PAGE_CUSTOMFUNCTION_SHOW un.FinishPageShow
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"

!include "nsis\legacy_upgrade.nsh"
!include "nsis\setup.nsh"
!include "nsis\install_recovery.nsh"
!include "nsis\install_locks.nsh"
!include "nsis\install_state.nsh"
!include "nsis\install_tsf.nsh"
!include "install_payload.nsh"

Section "Install"
    SetRegView 64
    SetShellVarContext all
    InitPluginsDir
    StrCpy $LifecycleResultPath "$PLUGINSDIR\cxxime-lifecycle.ini"
    SetOutPath "$PLUGINSDIR"
    File /oname=cxxime-installer-helper.exe "cxxime-installer-helper.exe"

    Call CheckInstallVersion
    Call UpgradeLegacyInstall
    Call PrepareInstallTarget
    Call SetTransactionPaths
    Call CheckFreshInstallBase
    Pop $0
    StrCmp $0 "1" install_base_contents_ready
        Goto install_failed_untrusted_base
    install_base_contents_ready:
    Call SecureInstallBase
    Pop $0
    StrCmp $0 "1" install_base_ready
        Goto install_failed_untrusted_base
    install_base_ready:
    Call LoadPreparedInstallTarget
    Pop $0
    StrCmp $0 "1" install_prepared_target_ready
        Goto install_failed_untrusted_base
    install_prepared_target_ready:
    Call CaptureServerState
    Pop $0
    StrCmp $0 "1" runtime_snapshot_ready
        Goto install_failed_before_swap
    runtime_snapshot_ready:
    Call ReleaseInputProcessor
    Call StopServer
    StrCmp $ServerStopResult "0" install_server_stopped
        StrCpy $FailureMessage "无法确认 CxxIME 后台已终止，未继续覆盖文件。"
        Goto install_failed_before_swap
    install_server_stopped:
    Call RecoverInterruptedInstall
    Pop $0
    StrCmp $0 "1" install_recovery_ready
        StrCmp $FailureMessage "" 0 install_failed_recovery
        StrCpy $FailureMessage "无法安全恢复上一次未完成的 CxxIME 安装。"
        Goto install_failed_recovery

    install_recovery_ready:
    ${If} $InitialServerWasRunning == 0
    ${AndIf} $TransactionServerWasRunning == 1
        StrCpy $InitialServerWasRunning 1
    ${EndIf}
    StrCpy $ServerWasRunning $InitialServerWasRunning
    Call RefreshInstallLayoutAfterRecovery
    Call SetTransactionPaths
    Call PrepareInstallLifecycle
    Pop $0
    StrCmp $0 "1" install_lifecycle_ready
        Goto install_failed_before_swap
    install_lifecycle_ready:
    Call CheckInstallDirectory
    Pop $0
    StrCmp $0 "1" install_directory_checked
        Goto install_failed_before_swap

    install_directory_checked:
    Call SetTransactionPaths
    Call SnapshotPreviousState

    ClearErrors
    RMDir "$StageDir"
    IfFileExists "$StageDir" 0 install_stage_path_ready
        StrCpy $FailureMessage "CxxIME 更新目录中仍有未完成的安装文件。"
        Goto install_failed_before_swap
    install_stage_path_ready:
    CreateDirectory "$StageDir"
    IfErrors 0 install_stage_directory_ready
        StrCpy $FailureMessage "无法创建 CxxIME 更新目录。"
        Goto install_failed_before_swap
    install_stage_directory_ready:

    !insertmacro InstallVersionPayload

    WriteUninstaller "$StageDir\uninstall.exe"
    IfErrors 0 install_stage_ready
        StrCpy $FailureMessage "无法解压 CxxIME 安装文件。"
        Goto install_failed_before_swap

    install_stage_ready:
    Call WriteTransactionState
    Pop $0
    StrCmp $0 "1" install_transaction_ready
        Goto install_failed_before_swap

    install_transaction_ready:
    SetOutPath "$PLUGINSDIR"
    RMDir "$INSTDIR"

    ClearErrors
    Rename "$StageDir" "$INSTDIR"
    IfErrors 0 install_stage_swapped
        StrCpy $FailureMessage "无法启用新的 CxxIME 文件。"
        Goto install_failed_after_transaction

    install_stage_swapped:
    Call RegisterNewTsf
    Pop $0
    StrCmp $0 "1" install_write_registry
        Goto install_failed_after_transaction

    install_write_registry:
    Call WriteInstallationRegistry
    Pop $0
    StrCmp $0 "1" install_start_new_server
        Goto install_failed_after_transaction

    install_start_new_server:
    Call StartNewServer
    Pop $0
    StrCmp $0 "1" install_prepare_system_ime
        Goto install_failed_after_transaction

    install_prepare_system_ime:
    Call PrepareSystemImeUpdate
    Pop $0
    StrCmp $0 "1" install_write_marker
        Goto install_failed_after_transaction

    install_write_marker:
    Call WriteInstallMarker
    Pop $0
    StrCmp $0 "1" install_commit
        Goto install_failed_after_transaction

    install_commit:
    Call CollectPreviousVersionLockNotice
    Call CommitInstallLifecycle
    Pop $0
    StrCmp $0 "1" install_lifecycle_committed
        Goto install_failed_after_transaction
    install_lifecycle_committed:
    ClearErrors
    Delete "$INSTDIR\${TRANSACTION_MARKER}"
    IfErrors 0 install_transaction_marker_removed
        DetailPrint "安装状态已提交，但未能删除安装事务标记。"
    install_transaction_marker_removed:
    Call CollectInstallGarbage
    Delete /REBOOTOK "$InstallBaseDir\${LEGACY_INSTALL_STATE_MARKER}"
    Delete /REBOOTOK "$InstallBaseDir\${LEGACY_INSTALL_STATE_TEMP}"
    Call CopyNewSystemIme
    Pop $0
    StrCmp $0 "1" install_system_ime_committed
        IfSilent install_system_ime_warning_silent
            MessageBox MB_ICONEXCLAMATION \
                "CxxIME ${VERSION} 已安装，但系统 IME 模块未能完成更新。$\r$\n$\r$\n$FailureMessage$\r$\n$\r$\n\
                安装状态已保留，后续运行安装程序时会再次尝试。"
        install_system_ime_warning_silent:
        DetailPrint "$FailureMessage"
    install_system_ime_committed:
    CreateDirectory "$PROFILE\cxxime"
    IfFileExists "$PROFILE\cxxime\default.json" install_user_config_ready
        CopyFiles /SILENT /FILESONLY "$INSTDIR\data\default.json" "$PROFILE\cxxime"
    install_user_config_ready:
    Call CreateInstallShortcuts
    DetailPrint "CxxIME ${VERSION} 安装已完成。"
    Goto install_done

    install_failed_untrusted_base:
    StrCmp $InstallBaseHandle "0" install_untrusted_base_handle_closed
    StrCmp $InstallBaseHandle "-1" install_untrusted_base_handle_closed
        System::Call 'kernel32::CloseHandle(p $InstallBaseHandle)'
        StrCpy $InstallBaseHandle 0
    install_untrusted_base_handle_closed:
    IfSilent install_untrusted_base_silent
        MessageBox MB_ICONSTOP "$FailureMessage"
    install_untrusted_base_silent:
    DetailPrint "$FailureMessage"
    SetErrorLevel 1
    Abort

    install_failed_before_swap:
    StrCpy $InstallStateVerified 1
    ClearErrors
    RMDir /r "$StageDir"
    IfErrors install_failed_before_swap_cleanup_failed
    IfFileExists "$StageDir" 0 install_failed_before_swap_cleanup_done
    install_failed_before_swap_cleanup_failed:
    StrCpy $InstallStateVerified 0
    StrCpy $FailureMessage "$FailureMessage$\r$\n$\r$\n无法清理安装暂存目录，未恢复启动后台。"
    Goto install_failed_before_swap_report_ready
    install_failed_before_swap_cleanup_done:
    Call RestartInstalledServer
    Call CleanupRuntimeSnapshotAfterServerRestore
    StrCmp $ServerRestartResult "2" 0 install_failed_before_swap_report_ready
        StrCpy $FailureMessage "$FailureMessage$\r$\n$\r$\nCxxIME 后台未能自动恢复，请检查占用进程或手动启动 CxxIME。"
    install_failed_before_swap_report_ready:
    IfSilent install_failed_silent
        MessageBox MB_ICONSTOP "$FailureMessage$\r$\n$\r$\n已安装的 CxxIME 文件未发生变化。"
        Goto install_failed_abort

    install_failed_after_transaction:
    StrCpy $InstallStateVerified 0
    nsExec::Exec '"$PLUGINSDIR\cxxime-installer-helper.exe" force-stop-server "$INSTDIR\cxxime-server.exe"'
    Pop $0
    Call RollbackInstall
    Pop $0
    StrCmp $0 "1" install_rollback_complete
        StrCpy $FailureMessage \
            "$FailureMessage$\r$\n$\r$\n自动回滚未能完成。请重新运行安装程序后再使用 CxxIME。"
        Goto install_failed_silent_or_message
    install_rollback_complete:
        Call VerifyRestoredInstall
        Pop $0
        StrCmp $0 "1" install_rollback_verified
            Goto install_failed_silent_or_message
        install_rollback_verified:
        StrCpy $FailureMessage "$FailureMessage$\r$\n$\r$\n已恢复 CxxIME 安装前的状态。"
        Goto install_failed_silent_or_message

    install_failed_recovery:
        StrCpy $InstallStateVerified 0
        Goto install_failed_silent_or_message

    install_failed_silent_or_message:
    Call RestartInstalledServer
    Call CleanupRuntimeSnapshotAfterServerRestore
    StrCmp $ServerRestartResult "2" 0 install_failed_restart_report_ready
        StrCpy $FailureMessage "$FailureMessage$\r$\n$\r$\nCxxIME 后台未能自动恢复，请检查占用进程或手动启动 CxxIME。"
    install_failed_restart_report_ready:
    IfSilent install_failed_silent
        MessageBox MB_ICONSTOP "$FailureMessage"
        Goto install_failed_abort

    install_failed_silent:
    DetailPrint "$FailureMessage"
    install_failed_abort:
    SetErrorLevel 1
    Abort

    install_done:
    StrCmp $InstallBaseHandle "0" install_base_handle_closed
        System::Call 'kernel32::CloseHandle(p $InstallBaseHandle)'
        StrCpy $InstallBaseHandle 0
    install_base_handle_closed:
SectionEnd

!include "nsis\uninstall_locks.nsh"
!include "nsis\uninstall_state.nsh"
!include "nsis\uninstall_files.nsh"

Section "Uninstall"
    SetRegView 64
    SetShellVarContext all
    InitPluginsDir
    StrCpy $LifecycleResultPath "$PLUGINSDIR\cxxime-lifecycle.ini"
    SetOutPath "$PLUGINSDIR"
    File /oname=cxxime-installer-helper.exe "cxxime-installer-helper.exe"
    StrCpy $LockReportPath "$PLUGINSDIR\cxxime-locks.txt"

    Call un.ReleaseInputProcessor
    Call un.StopServer
    StrCmp $UninstallServerStopResult "0" un_server_stopped
        DetailPrint "CxxIME 后台仍在退出；相关程序文件将在 Windows 重启后删除。"
    un_server_stopped:
    Call un.CheckFileLocks
    Call un.ValidateInstallLifecycle
    Pop $0
    StrCmp $0 "1" un_lifecycle_valid
        Call un.FailAndRestart
    un_lifecycle_valid:
    Call un.PrepareTransaction
    Pop $0
    StrCmp $0 "1" un_transaction_ready
        Call un.FailAndRestart

    un_transaction_ready:
    Call un.PrepareSystemImeRemoval
    Pop $0
    StrCmp $0 "1" un_system_ime_removal_ready
        Call un.FailAndRestart
    un_system_ime_removal_ready:
    Call un.UnregisterInstalledTsf
    Pop $0
    StrCmp $0 "1" un_remove_registry
        Goto un_rollback_failure

    un_rollback_failure:
    Call un.RollbackTransaction
    Pop $0
    StrCmp $0 "1" un_rollback_complete
        StrCpy $FailureMessage \
            "$FailureMessage$\r$\n$\r$\n自动回滚未能完成。请重新运行卸载程序后再使用 CxxIME。"
        Call un.FailAndRestart
    un_rollback_complete:
    StrCpy $FailureMessage "$FailureMessage$\r$\n$\r$\n已恢复卸载前的 CxxIME 状态。"
    Call un.FailAndRestart

    un_remove_registry:
    DeleteRegValue HKLM "${RUN_KEY}" "CxxIMEServer"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"
    DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\${CLSID}"
    DeleteRegKey HKLM "SOFTWARE\Microsoft\CTF\TIP\${CLSID}"
    SetRegView 32
    DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\${CLSID}"
    DeleteRegKey HKLM "SOFTWARE\Microsoft\CTF\TIP\${CLSID}"
    SetRegView 64
    ClearErrors
    ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "DisplayName"
    IfErrors un_uninstall_registry_removed
        StrCpy $FailureMessage \
            "无法删除 CxxIME 卸载注册表项。请重新运行卸载程序。"
        Goto un_rollback_failure
    un_uninstall_registry_removed:
    ClearErrors
    ReadRegStr $0 HKLM "${RUN_KEY}" "CxxIMEServer"
    IfErrors un_run_registry_removed
        StrCpy $FailureMessage \
            "无法删除 CxxIME 启动注册表项。请重新运行卸载程序。"
        Goto un_rollback_failure
    un_run_registry_removed:

    Call un.RemoveSystemIme
    Pop $0
    StrCmp $0 "1" +2
        StrCpy $UninstallCleanupWarning 1
    RMDir /r "$SMPROGRAMS\CxxIME"
    Call un.CommitInstallLifecycle
    Pop $0
    StrCmp $0 "1" +2
        StrCpy $UninstallCleanupWarning 1
    Delete /REBOOTOK "$InstallBaseDir\${RUNTIME_MARKER}"
    Delete /REBOOTOK "$InstallBaseDir\${RUNTIME_TEMP}"
    Delete /REBOOTOK "$InstallBaseDir\${LEGACY_INSTALL_STATE_MARKER}"
    Delete /REBOOTOK "$InstallBaseDir\${LEGACY_INSTALL_STATE_TEMP}"
    Delete /REBOOTOK "$InstallBaseDir\${SYSTEM_IME_UPDATE_MARKER}"
    StrCmp $LifecycleRemaining "-1" un_remove_user_data
    StrCmp $LifecycleRemaining "0" 0 un_remove_user_data
    StrCmp $LifecycleUnknown "0" 0 un_remove_user_data
        Delete /REBOOTOK "$InstallBaseDir\maintenance\install-state.json"
        RMDir /REBOOTOK "$InstallBaseDir\maintenance"
        RMDir /REBOOTOK "$InstallBaseDir"
    un_remove_user_data:
    ${If} $UninstallRemoveUserData == ${BST_CHECKED}
        StrCpy $UninstallUserDataDirSuffix $UninstallUserDataDir 7 -7
        ${If} $UninstallUserDataDir != ""
        ${AndIf} $UninstallUserDataDirSuffix == "\cxxime"
            RMDir /r "$UninstallUserDataDir"
        ${EndIf}
    ${EndIf}
    RMDir "$InstallBaseDir"
SectionEnd
