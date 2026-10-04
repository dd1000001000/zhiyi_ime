Function RegisterNewTsf
    nsExec::ExecToStack '"$WINDIR\Sysnative\regsvr32.exe" /s "$INSTDIR\zhiyi_tsf_x64.dll"'
    Pop $0
    Pop $1
    ${If} $0 != "0"
        StrCpy $FailureMessage "无法注册 64 位 TSF 模块。"
        Push 0
        Return
    ${EndIf}

    nsExec::ExecToStack '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\zhiyi_tsf_x86.dll"'
    Pop $0
    Pop $1
    ${If} $0 != "0"
        StrCpy $FailureMessage "无法注册 32 位 TSF 模块。"
        Push 0
        Return
    ${EndIf}
    Push 1
FunctionEnd

Function RegisterPreviousTsf
    StrCmp $OldTsfX64Registered "1" 0 restore_register_x86
    IfFileExists "$StateInstallDir\zhiyi_tsf_x64.dll" 0 restore_register_x64_missing
        nsExec::ExecToStack \
            '"$WINDIR\Sysnative\regsvr32.exe" /s "$StateInstallDir\zhiyi_tsf_x64.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" restore_register_x86
            StrCpy $FailureMessage "无法恢复先前的 64 位 TSF 注册。"
            Push 0
            Return
    restore_register_x86:
    StrCmp $OldTsfX86Registered "1" 0 restore_register_done
    IfFileExists "$StateInstallDir\zhiyi_tsf_x86.dll" 0 restore_register_x86_missing
        nsExec::ExecToStack '"$SYSDIR\regsvr32.exe" /s "$StateInstallDir\zhiyi_tsf_x86.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" restore_register_done
            StrCpy $FailureMessage "无法恢复先前的 32 位 TSF 注册。"
            Push 0
            Return
    restore_register_done:
    Push 1
    Return

    restore_register_x64_missing:
    StrCpy $FailureMessage "无法恢复先前的 64 位 TSF 模块。"
    Push 0
    Return

    restore_register_x86_missing:
    StrCpy $FailureMessage "无法恢复先前的 32 位 TSF 模块。"
    Push 0
FunctionEnd

Function WriteInstallationRegistry
    ClearErrors
    WriteRegStr HKLM "${RUN_KEY}" "ZhiyiIMEServer" '"$INSTDIR\zhiyi-server.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "知意输入法"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" '"$INSTDIR\zhiyi-resources.dll",-100'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallBaseLocation" "$InstallBaseDir"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
    IfErrors installation_registry_failed
    Push 1
    Return

    installation_registry_failed:
    StrCpy $FailureMessage "无法写入知意输入法安装注册表项。"
    Push 0
FunctionEnd

Function RestorePreviousRegistry
    ${If} $OldUninstallPresent == 1
        ClearErrors
        WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "知意输入法"
        WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "$OldDisplayVersion"
        WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
        WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" \
            '"$PreviousInstallDir\zhiyi-resources.dll",-100'
        WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$PreviousInstallDir"
        WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallBaseLocation" "$InstallBaseDir"
        WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" \
            '"$PreviousInstallDir\uninstall.exe"'
        WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" \
            '"$PreviousInstallDir\uninstall.exe" /S'
        WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
        WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
        IfErrors restore_registry_failed
    ${Else}
        DeleteRegKey HKLM "${UNINSTALL_KEY}"
    ${EndIf}
    ${If} $OldRunPresent == 1
        ClearErrors
        WriteRegStr HKLM "${RUN_KEY}" "ZhiyiIMEServer" "$OldRunValue"
        IfErrors restore_registry_failed
    ${Else}
        DeleteRegValue HKLM "${RUN_KEY}" "ZhiyiIMEServer"
    ${EndIf}
    Push 1
    Return

    restore_registry_failed:
    StrCpy $FailureMessage "无法恢复先前的知意输入法注册表状态。"
    Push 0
FunctionEnd

Function RollbackInstall
    IfFileExists "$INSTDIR\${TRANSACTION_MARKER}" 0 rollback_staged_transaction
        StrCpy $TransactionDir "$INSTDIR"
        Call RecoverTransaction
        Return
    rollback_staged_transaction:
    IfFileExists "$StageDir\${TRANSACTION_MARKER}" 0 rollback_transaction_missing
        StrCpy $TransactionDir "$StageDir"
        Call RecoverTransaction
        Return
    rollback_transaction_missing:
    StrCpy $FailureMessage "找不到知意输入法回滚事务。"
    Push 0
FunctionEnd

Function WriteInstallMarker
    ClearErrors
    FileOpen $0 "$INSTDIR\${INSTALL_MARKER}" w
    IfErrors install_marker_failed
    FileWrite $0 "version=${VERSION}$\r$\n"
    FileClose $0
    IfErrors install_marker_failed
    Push 1
    Return
    install_marker_failed:
        StrCpy $FailureMessage "无法完成知意输入法安装。"
        Push 0
FunctionEnd

Function CreateInstallShortcuts
    SetShellVarContext all
    CreateDirectory "$SMPROGRAMS\知意输入法"
    CreateShortCut "$SMPROGRAMS\知意输入法\知意输入法设置.lnk" "$INSTDIR\zhiyi-settings.exe"
    Delete "$SMPROGRAMS\知意输入法\Host Candidate Probe x64.lnk"
    Delete "$SMPROGRAMS\知意输入法\Host Candidate Probe x86.lnk"
    Delete "$SMPROGRAMS\知意输入法\Export Stage 1 Trace.lnk"
    Delete "$SMPROGRAMS\知意输入法\Export Host Trace.lnk"
    !ifdef HOST_DIAGNOSTICS
        CreateShortCut \
            "$SMPROGRAMS\知意输入法\Host Candidate Probe x64.lnk" \
            "$INSTDIR\zhiyi-ime-host-probe-x64.exe"
        CreateShortCut \
            "$SMPROGRAMS\知意输入法\Host Candidate Probe x86.lnk" \
            "$INSTDIR\zhiyi-ime-host-probe-x86.exe"
        CreateShortCut \
            "$SMPROGRAMS\知意输入法\Export Host Trace.lnk" \
            "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" \
            '-NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\export_host_trace.ps1"'
    !endif
    CreateShortCut \
        "$SMPROGRAMS\知意输入法\Collect Diagnostics.lnk" \
        "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" \
        '-NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\collect_diagnostics.ps1"'
    CreateShortCut "$SMPROGRAMS\知意输入法\Uninstall 知意输入法.lnk" "$INSTDIR\uninstall.exe"
FunctionEnd
