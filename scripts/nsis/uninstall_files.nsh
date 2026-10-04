Function un.RegisterInstalledTsf
    StrCmp $UninstallTsfX64Registered "1" 0 un_restore_register_x86
    IfFileExists "$INSTDIR\zhiyi_tsf_x64.dll" 0 un_restore_register_x64_missing
        nsExec::ExecToStack \
            '"$WINDIR\Sysnative\regsvr32.exe" /s "$INSTDIR\zhiyi_tsf_x64.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" un_restore_register_x86
            StrCpy $FailureMessage "$(L_094)"
            Push 0
            Return
    un_restore_register_x86:
    StrCmp $UninstallTsfX86Registered "1" 0 un_restore_register_done
    IfFileExists "$INSTDIR\zhiyi_tsf_x86.dll" 0 un_restore_register_x86_missing
        nsExec::ExecToStack '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\zhiyi_tsf_x86.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" un_restore_register_done
            StrCpy $FailureMessage "$(L_095)"
            Push 0
            Return
    un_restore_register_done:
    Push 1
    Return

    un_restore_register_x64_missing:
    StrCpy $FailureMessage "$(L_096)"
    Push 0
    Return

    un_restore_register_x86_missing:
    StrCpy $FailureMessage "$(L_097)"
    Push 0
FunctionEnd

Function un.RestoreInstallationRegistry
    SetRegView 64
    ClearErrors
    WriteRegStr HKLM "${RUN_KEY}" "ZhiyiIMEServer" '"$INSTDIR\zhiyi-server.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "$(L_022)"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" '"$INSTDIR\zhiyi-resources.dll",-100'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallBaseLocation" "$InstallBaseDir"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
    IfErrors un_restore_registry_failed
    Push 1
    Return

    un_restore_registry_failed:
    Push 0
FunctionEnd

Function un.RollbackTransaction
    Call un.RegisterInstalledTsf
    Pop $0
    StrCmp $0 "1" 0 un_rollback_failed
    Call un.RestoreInstallationRegistry
    Pop $0
    StrCmp $0 "1" 0 un_rollback_failed
    Delete "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}"
    Delete "$INSTDIR\${UNINSTALL_TRANSACTION_TEMP}"
    Delete "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}"
    Push 1
    Return

    un_rollback_failed:
    Push 0
FunctionEnd

Function un.FailAndRestart
    Call un.RestartInstalledServer
    IfSilent un_failure_silent
        MessageBox MB_ICONSTOP "$FailureMessage"
    un_failure_silent:
    DetailPrint "$FailureMessage"
    SetErrorLevel 1
    Abort
FunctionEnd
