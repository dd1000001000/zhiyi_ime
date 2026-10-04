Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "WinMessages.nsh"
!include "Win\WinError.nsh"
!include "x64.nsh"

!define PRODUCT "知意输入法"
!define PUBLISHER "Zhiyi IME Contributors"
!define CLSID "{4EAC2DF0-F298-453E-BD4A-B4B3D3579718}"
!define UNINSTALL_KEY "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME"
!define RUN_KEY "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"
!define TSF_INPROC_KEY "SOFTWARE\Classes\CLSID\${CLSID}\InprocServer32"
!define TSF_TIP_KEY "SOFTWARE\Microsoft\CTF\TIP\${CLSID}"
!define INSTALL_MARKER ".zhiyi-install-complete"
!define TRANSACTION_MARKER ".zhiyi-install-transaction"
!define TRANSACTION_TEMP ".zhiyi-install-transaction.tmp"
!define RUNTIME_MARKER ".zhiyi-install-runtime"
!define RUNTIME_TEMP ".zhiyi-install-runtime.tmp"
!define SYSTEM_IME_UPDATE_MARKER ".zhiyi-ime-update"
!define SYSTEM_IME_REMOVE_MARKER ".zhiyi-ime-remove-pending"
!define LEGACY_SYSTEM_IME_X64_PENDING ".zhiyi-ime-x64.pending"
!define LEGACY_SYSTEM_IME_X86_PENDING ".zhiyi-ime-x86.pending"
!define LEGACY_INSTALL_STATE_MARKER ".zhiyi-install-state"
!define LEGACY_INSTALL_STATE_TEMP ".zhiyi-install-state.tmp"
!define LEGACY_UNINSTALL_DEFERRED_MARKER ".zhiyi-uninstall-pending"
!define UNINSTALL_TRANSACTION_MARKER ".zhiyi-uninstall-transaction"
!define UNINSTALL_TRANSACTION_TEMP ".zhiyi-uninstall-transaction.tmp"
!define MOVEFILE_REPLACE_WRITE_THROUGH 0x9
!define MOVEFILE_DELAY_UNTIL_REBOOT 0x4
!define MOVEFILE_REPLACE_DELAY_UNTIL_REBOOT 0x5

!ifndef VERSION
    !error "VERSION must be provided by package.py"
!endif
!ifndef VERSION_NUMERIC
    !error "VERSION_NUMERIC must be provided by package.py"
!endif

Name "$(L_022) ${VERSION}"
!ifdef HOST_DIAGNOSTICS
    OutFile "zhiyi-v${VERSION}-host-diag-setup.exe"
!else
    OutFile "zhiyi-v${VERSION}-setup.exe"
!endif
InstallDir "$PROGRAMFILES\ZhiyiIME"
RequestExecutionLevel admin
SetCompressor lzma
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION_NUMERIC}"
VIAddVersionKey /LANG=2052 "CompanyName" "${PUBLISHER}"
VIAddVersionKey /LANG=2052 "FileDescription" "知意输入法安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${VERSION_NUMERIC}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyright (c) 2026 知意输入法 Contributors, GPL-3.0; based on CxxIME (Apache-2.0)"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "CompanyName" "${PUBLISHER}"
VIAddVersionKey /LANG=1033 "FileDescription" "Zhiyi IME Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION_NUMERIC}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) 2026 Zhiyi IME Contributors, GPL-3.0; based on CxxIME (Apache-2.0)"
VIAddVersionKey /LANG=1033 "ProductName" "Zhiyi IME"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"

!define MUI_ICON "zhiyi.ico"
!define MUI_UNICON "zhiyi.ico"

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
Var ExperienceProgram
Var ExperienceCheckbox
Var CollectInput
Var CollectInputCheckbox

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "license.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ValidateInstallDirectory
!insertmacro MUI_PAGE_DIRECTORY
; User experience improvement program (opt-in; nsis\setup.nsh).
Page custom ExperiencePage ExperiencePageLeave
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_NOREBOOTSUPPORT
!define MUI_FINISHPAGE_RUN "$INSTDIR\zhiyi-settings.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(L_001)"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_PAGE_CUSTOMFUNCTION_SHOW FinishPageShow
!insertmacro MUI_PAGE_FINISH
UninstPage custom un.ConfirmPage un.ConfirmPageLeave
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_UNTEXT_FINISH_INFO_REBOOT \
    "$(L_002)"
; Files still in use are deleted at the next restart (SetRebootFlag in
; un.CommitInstallLifecycle): the finish page then offers to restart now or later (later is
; preselected so nobody restarts by accident).
!define MUI_FINISHPAGE_REBOOTLATER_DEFAULT
!define MUI_PAGE_CUSTOMFUNCTION_SHOW un.FinishPageShow
!insertmacro MUI_UNPAGE_FINISH
; The installer asks for its language (ChooseInstallerLanguage in nsis\setup.nsh): the
; settings language, else the Windows UI language, is preselected; any language other than
; Chinese preselects English, the first language inserted. Silent installs do not ask. The
; uninstaller uses the language chosen at installation.
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"
; Both languages are listed whatever the system code page (A, not AC); the dialog is shown
; before a language is chosen, so its text is in both.
!define MUI_LANGDLL_ALLLANGUAGES
!define MUI_LANGDLL_WINDOWTITLE "Zhiyi IME / 知意输入法"
!define MUI_LANGDLL_INFO "Please select a language.$\r$\n请选择安装语言。"
!insertmacro MUI_RESERVEFILE_LANGDLL
!include "nsis\lang.nsh"

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
    StrCpy $LifecycleResultPath "$PLUGINSDIR\zhiyi-lifecycle.ini"
    SetOutPath "$PLUGINSDIR"
    File /oname=zhiyi-installer-helper.exe "zhiyi-installer-helper.exe"

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
        StrCpy $FailureMessage "$(L_003)"
        Goto install_failed_before_swap
    install_server_stopped:
    Call RecoverInterruptedInstall
    Pop $0
    StrCmp $0 "1" install_recovery_ready
        StrCmp $FailureMessage "" 0 install_failed_recovery
        StrCpy $FailureMessage "$(L_004)"
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
        StrCpy $FailureMessage "$(L_005)"
        Goto install_failed_before_swap
    install_stage_path_ready:
    CreateDirectory "$StageDir"
    IfErrors 0 install_stage_directory_ready
        StrCpy $FailureMessage "$(L_006)"
        Goto install_failed_before_swap
    install_stage_directory_ready:

    !insertmacro InstallVersionPayload

    WriteUninstaller "$StageDir\uninstall.exe"
    IfErrors 0 install_stage_ready
        StrCpy $FailureMessage "$(L_007)"
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
        StrCpy $FailureMessage "$(L_008)"
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
    ; Before the server starts and reads the user config.
    Call ApplyInstallerLanguage
    Call ApplyExperienceProgram
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
        DetailPrint "$(L_009)"
    install_transaction_marker_removed:
    Call CollectInstallGarbage
    Delete /REBOOTOK "$InstallBaseDir\${LEGACY_INSTALL_STATE_MARKER}"
    Delete /REBOOTOK "$InstallBaseDir\${LEGACY_INSTALL_STATE_TEMP}"
    Call CopyNewSystemIme
    Pop $0
    StrCmp $0 "1" install_system_ime_committed
        IfSilent install_system_ime_warning_silent
            MessageBox MB_ICONEXCLAMATION \
                "$(L_010)"
        install_system_ime_warning_silent:
        DetailPrint "$FailureMessage"
    install_system_ime_committed:
    CreateDirectory "$PROFILE\zhiyi"
    IfFileExists "$PROFILE\zhiyi\default.json" install_user_config_ready
        CopyFiles /SILENT /FILESONLY "$INSTDIR\data\default.json" "$PROFILE\zhiyi"
    install_user_config_ready:
    Call CreateInstallShortcuts
    DetailPrint "$(L_011)"
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
    StrCpy $FailureMessage "$(L_012)"
    Goto install_failed_before_swap_report_ready
    install_failed_before_swap_cleanup_done:
    Call RestartInstalledServer
    Call CleanupRuntimeSnapshotAfterServerRestore
    StrCmp $ServerRestartResult "2" 0 install_failed_before_swap_report_ready
        StrCpy $FailureMessage "$(L_013)"
    install_failed_before_swap_report_ready:
    IfSilent install_failed_silent
        MessageBox MB_ICONSTOP "$(L_014)"
        Goto install_failed_abort

    install_failed_after_transaction:
    StrCpy $InstallStateVerified 0
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" force-stop-server "$INSTDIR\zhiyi-server.exe"'
    Pop $0
    Call RollbackInstall
    Pop $0
    StrCmp $0 "1" install_rollback_complete
        StrCpy $FailureMessage \
            "$(L_015)"
        Goto install_failed_silent_or_message
    install_rollback_complete:
        Call VerifyRestoredInstall
        Pop $0
        StrCmp $0 "1" install_rollback_verified
            Goto install_failed_silent_or_message
        install_rollback_verified:
        StrCpy $FailureMessage "$(L_016)"
        Goto install_failed_silent_or_message

    install_failed_recovery:
        StrCpy $InstallStateVerified 0
        Goto install_failed_silent_or_message

    install_failed_silent_or_message:
    Call RestartInstalledServer
    Call CleanupRuntimeSnapshotAfterServerRestore
    StrCmp $ServerRestartResult "2" 0 install_failed_restart_report_ready
        StrCpy $FailureMessage "$(L_013)"
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
    StrCpy $LifecycleResultPath "$PLUGINSDIR\zhiyi-lifecycle.ini"
    SetOutPath "$PLUGINSDIR"
    File /oname=zhiyi-installer-helper.exe "zhiyi-installer-helper.exe"
    StrCpy $LockReportPath "$PLUGINSDIR\zhiyi-locks.txt"

    Call un.ReleaseInputProcessor
    Call un.StopServer
    StrCmp $UninstallServerStopResult "0" un_server_stopped
        DetailPrint "$(L_017)"
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
            "$(L_018)"
        Call un.FailAndRestart
    un_rollback_complete:
    StrCpy $FailureMessage "$(L_019)"
    Call un.FailAndRestart

    un_remove_registry:
    DeleteRegValue HKLM "${RUN_KEY}" "ZhiyiIMEServer"
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
            "$(L_020)"
        Goto un_rollback_failure
    un_uninstall_registry_removed:
    ClearErrors
    ReadRegStr $0 HKLM "${RUN_KEY}" "ZhiyiIMEServer"
    IfErrors un_run_registry_removed
        StrCpy $FailureMessage \
            "$(L_021)"
        Goto un_rollback_failure
    un_run_registry_removed:

    Call un.RemoveSystemIme
    Pop $0
    StrCmp $0 "1" +2
        StrCpy $UninstallCleanupWarning 1
    ; Start menu folder in either language (the installer language may have changed).
    RMDir /r "$SMPROGRAMS\知意输入法"
    RMDir /r "$SMPROGRAMS\Zhiyi IME"
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
        ${AndIf} $UninstallUserDataDirSuffix == "\zhiyi"
            RMDir /r "$UninstallUserDataDir"
        ${EndIf}
    ${EndIf}
    RMDir "$InstallBaseDir"
SectionEnd
