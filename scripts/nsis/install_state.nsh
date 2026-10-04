Function SnapshotPreviousState
    StrCmp $MultiVersionInstall "1" 0 snapshot_state_dir_ready
        StrCpy $StateInstallDir "$PreviousInstallDir"
    snapshot_state_dir_ready:
    StrCpy $OldInstallAvailable $ExistingInstall
    ${If} $MultiVersionInstall == 1
        StrCpy $OldInstallAvailable 1
    ${EndIf}
    StrCpy $OldTsfX64Present 0
    StrCpy $OldTsfX86Present 0
    StrCpy $OldTsfX64Registered 0
    StrCpy $OldTsfX86Registered 0
    StrCpy $OldTipX64Present 0
    StrCpy $OldTipX86Present 0
    StrCpy $OldUninstallPresent 0
    StrCpy $OldDisplayVersion ""
    StrCpy $OldRunPresent 0
    StrCpy $OldRunValue ""

    IfFileExists "$StateInstallDir\zhiyi_tsf_x64.dll" 0 +2
        StrCpy $OldTsfX64Present 1
    IfFileExists "$StateInstallDir\zhiyi_tsf_x86.dll" 0 +2
        StrCpy $OldTsfX86Present 1

    ${If} $OldTsfX64Present == 1
        SetRegView 64
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${IfNot} ${Errors}
        ${AndIf} $0 == "$StateInstallDir\zhiyi_tsf_x64.dll"
            StrCpy $OldTsfX64Registered 1
        ${EndIf}
    ${EndIf}
    ${If} $OldTsfX86Present == 1
        SetRegView 32
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${IfNot} ${Errors}
        ${AndIf} $0 == "$StateInstallDir\zhiyi_tsf_x86.dll"
            StrCpy $OldTsfX86Registered 1
        ${EndIf}
    ${EndIf}
    SetRegView 64
    Call QueryTipRegistration
    Pop $0
    StrCpy $OldTipX64Present $0
    SetRegView 32
    Call QueryTipRegistration
    Pop $0
    StrCpy $OldTipX86Present $0
    SetRegView 64
    ${If} $OldInstallAvailable == 0
        StrCpy $OldTipX64Present 0
        StrCpy $OldTipX86Present 0
    ${EndIf}

    ClearErrors
    ReadRegStr $OldDisplayVersion HKLM "${UNINSTALL_KEY}" "DisplayVersion"
    ${IfNot} ${Errors}
        StrCpy $OldUninstallPresent 1
    ${EndIf}
    ClearErrors
    ReadRegStr $OldRunValue HKLM "${RUN_KEY}" "ZhiyiIMEServer"
    ${IfNot} ${Errors}
        StrCpy $OldRunPresent 1
    ${EndIf}
FunctionEnd

Function WriteTransactionState
    Delete "$StageDir\${TRANSACTION_TEMP}"
    ClearErrors
    FileOpen $0 "$StageDir\${TRANSACTION_TEMP}" w
    IfErrors transaction_state_write_failed
    FileWriteUTF16LE /BOM $0 "[transaction]$\r$\n"
    FileWriteUTF16LE $0 "format=4$\r$\n"
    FileWriteUTF16LE $0 "old_install_available=$OldInstallAvailable$\r$\n"
    FileWriteUTF16LE $0 "old_tsf_x64_present=$OldTsfX64Present$\r$\n"
    FileWriteUTF16LE $0 "old_tsf_x86_present=$OldTsfX86Present$\r$\n"
    FileWriteUTF16LE $0 "old_tsf_x64_registered=$OldTsfX64Registered$\r$\n"
    FileWriteUTF16LE $0 "old_tsf_x86_registered=$OldTsfX86Registered$\r$\n"
    FileWriteUTF16LE $0 "old_tip_x64_present=$OldTipX64Present$\r$\n"
    FileWriteUTF16LE $0 "old_tip_x86_present=$OldTipX86Present$\r$\n"
    FileWriteUTF16LE $0 "old_uninstall_present=$OldUninstallPresent$\r$\n"
    FileWriteUTF16LE $0 "old_display_version=$OldDisplayVersion$\r$\n"
    FileWriteUTF16LE $0 "old_run_present=$OldRunPresent$\r$\n"
    FileWriteUTF16LE $0 "old_run_value=$OldRunValue$\r$\n"
    FileWriteUTF16LE $0 "old_install_dir=$PreviousInstallDir$\r$\n"
    FileWriteUTF16LE $0 "server_was_running=$InitialServerWasRunning$\r$\n"
    IfErrors transaction_state_write_close_failed
    FileClose $0
    IfErrors transaction_state_write_failed
    IfFileExists "$StageDir\${TRANSACTION_TEMP}" transaction_state_commit \
        transaction_state_write_failed

    transaction_state_commit:
    System::Call 'kernel32::MoveFileExW(\
        w "$StageDir\${TRANSACTION_TEMP}", \
        w "$StageDir\${TRANSACTION_MARKER}", \
        i ${MOVEFILE_REPLACE_WRITE_THROUGH}) i .r0 ?e'
    Pop $1
    StrCmp $0 "0" transaction_state_commit_failed
    Push 1
    Return

    transaction_state_write_close_failed:
    FileClose $0
    transaction_state_write_failed:
    Delete "$StageDir\${TRANSACTION_TEMP}"
    StrCpy $FailureMessage "$(L_040)"
    Push 0
    Return

    transaction_state_commit_failed:
    Delete "$StageDir\${TRANSACTION_TEMP}"
    StrCpy $FailureMessage \
        "$(L_041)"
    Push 0
FunctionEnd

Function PrepareSystemImeUpdate
    WriteINIStr "$InstallBaseDir\${SYSTEM_IME_UPDATE_MARKER}" "update" "source" "$INSTDIR"
    IfErrors prepare_system_ime_failed
    Push 1
    Return

    prepare_system_ime_failed:
    StrCpy $FailureMessage "$(L_042)"
    Push 0
FunctionEnd

Function CancelPendingSystemImeUpdate
    Delete "$InstallBaseDir\${SYSTEM_IME_UPDATE_MARKER}"
FunctionEnd

Function CopyNewSystemIme
    StrCpy $2 0
    StrCpy $6 0
    IfFileExists "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}" 0 \
        install_system_ime_check_pending
        StrCpy $2 1
        StrCpy $6 1
    install_system_ime_check_pending:
    IfFileExists "$InstallBaseDir\maintenance\ime-*.pending" 0 +2
        StrCpy $2 1
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X64_PENDING}" 0 +2
        StrCpy $2 1
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X86_PENDING}" 0 +2
        StrCpy $2 1
    ${GetFileName} "$INSTDIR" $3
    StrCpy $4 "$InstallBaseDir\maintenance\ime-$3-x64.pending"
    StrCpy $5 "$InstallBaseDir\maintenance\ime-$3-x86.pending"
    Delete "$4"
    Delete "$5"
    System::Call 'kernel32::CopyFileW(\
        w "$INSTDIR\zhiyi_ime_x64.ime", \
        w "$WINDIR\Sysnative\zhiyi.ime", \
        i 0) i .r0'
    ${If} $0 == 0
    ${OrIf} $2 == 1
        System::Call 'kernel32::CopyFileW(\
            w "$INSTDIR\zhiyi_ime_x64.ime", \
            w "$4", \
            i 0) i .r0'
        StrCmp $0 "0" install_system_ime_x64_failed
        System::Call 'kernel32::MoveFileExW(\
            w "$4", \
            w "$WINDIR\Sysnative\zhiyi.ime", \
            i ${MOVEFILE_REPLACE_DELAY_UNTIL_REBOOT}) i .r0 ?e'
        StrCmp $0 "0" install_system_ime_x64_failed
        SetRebootFlag true
    ${EndIf}
    System::Call 'kernel32::CopyFileW(\
        w "$INSTDIR\zhiyi_ime_x86.ime", \
        w "$SYSDIR\zhiyi.ime", \
        i 0) i .r0'
    ${If} $0 == 0
    ${OrIf} $2 == 1
        System::Call 'kernel32::CopyFileW(\
            w "$INSTDIR\zhiyi_ime_x86.ime", \
            w "$5", \
            i 0) i .r0'
        StrCmp $0 "0" install_system_ime_x86_failed
        System::Call 'kernel32::MoveFileExW(\
            w "$5", \
            w "$SYSDIR\zhiyi.ime", \
            i ${MOVEFILE_REPLACE_DELAY_UNTIL_REBOOT}) i .r0 ?e'
        StrCmp $0 "0" install_system_ime_x86_failed
        SetRebootFlag true
    ${EndIf}
    ${If} $6 == 1
        System::Call 'kernel32::MoveFileExW(\
            w "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}", \
            p 0, i ${MOVEFILE_DELAY_UNTIL_REBOOT}) i .r0 ?e'
        StrCmp $0 "0" install_system_ime_marker_failed
    ${EndIf}
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X64_PENDING}" 0 +2
        System::Call 'kernel32::MoveFileExW(\
            w "$InstallBaseDir\${LEGACY_SYSTEM_IME_X64_PENDING}", \
            p 0, i ${MOVEFILE_DELAY_UNTIL_REBOOT})'
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X86_PENDING}" 0 +2
        System::Call 'kernel32::MoveFileExW(\
            w "$InstallBaseDir\${LEGACY_SYSTEM_IME_X86_PENDING}", \
            p 0, i ${MOVEFILE_DELAY_UNTIL_REBOOT})'
    Delete "$InstallBaseDir\${SYSTEM_IME_UPDATE_MARKER}"
    IfErrors install_system_ime_marker_failed
    Push 1
    Return

    install_system_ime_marker_failed:
    StrCpy $FailureMessage "$(L_043)"
    Push 0
    Return
    install_system_ime_x64_failed:
    Delete "$4"
    StrCpy $FailureMessage "$(L_044)"
    Push 0
    Return
    install_system_ime_x86_failed:
    Delete "$5"
    StrCpy $FailureMessage "$(L_045)"
    Push 0
FunctionEnd
