Function un.LoadTransactionState
    StrCpy $UninstallTransactionPhase ""
    StrCpy $UninstallTsfX64Registered 0
    StrCpy $UninstallTsfX86Registered 0
    ClearErrors
    ReadINIStr $0 "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}" "transaction" "format"
    IfErrors un_transaction_state_invalid
    StrCmp $0 "1" 0 un_transaction_state_invalid
    ReadINIStr $UninstallTransactionPhase "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}" \
        "transaction" "phase"
    ReadINIStr $UninstallTsfX64Registered "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}" \
        "transaction" "tsf_x64_registered"
    ReadINIStr $UninstallTsfX86Registered "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}" \
        "transaction" "tsf_x86_registered"
    StrCmp $UninstallTransactionPhase "prepared" un_transaction_phase_valid \
        un_transaction_state_invalid
    un_transaction_phase_valid:
    StrCmp $UninstallTsfX64Registered "0" un_transaction_tsf_x64_valid
    StrCmp $UninstallTsfX64Registered "1" un_transaction_tsf_x64_valid \
        un_transaction_state_invalid
    un_transaction_tsf_x64_valid:
    StrCmp $UninstallTsfX86Registered "0" un_transaction_tsf_x86_valid
    StrCmp $UninstallTsfX86Registered "1" un_transaction_tsf_x86_valid \
        un_transaction_state_invalid
    un_transaction_tsf_x86_valid:
    Push 1
    Return

    un_transaction_state_invalid:
    StrCpy $FailureMessage "$(L_099)"
    Push 0
FunctionEnd

Function un.WriteTransactionState
    Delete "$INSTDIR\${UNINSTALL_TRANSACTION_TEMP}"
    ClearErrors
    FileOpen $0 "$INSTDIR\${UNINSTALL_TRANSACTION_TEMP}" w
    IfErrors un_write_transaction_failed
    FileWriteUTF16LE /BOM $0 "[transaction]$\r$\n"
    FileWriteUTF16LE $0 "format=1$\r$\n"
    FileWriteUTF16LE $0 "phase=$UninstallTransactionPhase$\r$\n"
    FileWriteUTF16LE $0 "tsf_x64_registered=$UninstallTsfX64Registered$\r$\n"
    FileWriteUTF16LE $0 "tsf_x86_registered=$UninstallTsfX86Registered$\r$\n"
    IfErrors un_write_transaction_close_failed
    FileClose $0
    IfErrors un_write_transaction_failed
    System::Call 'kernel32::MoveFileExW(\
        w "$INSTDIR\${UNINSTALL_TRANSACTION_TEMP}", \
        w "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}", \
        i ${MOVEFILE_REPLACE_WRITE_THROUGH}) i .r0 ?e'
    Pop $1
    StrCmp $0 "0" un_commit_transaction_failed
    Push 1
    Return

    un_write_transaction_close_failed:
    FileClose $0
    un_write_transaction_failed:
    Delete "$INSTDIR\${UNINSTALL_TRANSACTION_TEMP}"
    StrCpy $FailureMessage "$(L_100)"
    Push 0
    Return

    un_commit_transaction_failed:
    Delete "$INSTDIR\${UNINSTALL_TRANSACTION_TEMP}"
    StrCpy $FailureMessage \
        "$(L_101)"
    Push 0
FunctionEnd

Function un.SnapshotTsfRegistration
    StrCpy $UninstallTsfX64Registered 0
    StrCpy $UninstallTsfX86Registered 0

    IfFileExists "$INSTDIR\zhiyi_tsf_x64.dll" 0 un_snapshot_tsf_x86
        SetRegView 64
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${IfNot} ${Errors}
        ${AndIf} $0 == "$INSTDIR\zhiyi_tsf_x64.dll"
            StrCpy $UninstallTsfX64Registered 1
        ${EndIf}
    un_snapshot_tsf_x86:
    IfFileExists "$INSTDIR\zhiyi_tsf_x86.dll" 0 un_snapshot_tsf_done
        SetRegView 32
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${IfNot} ${Errors}
        ${AndIf} $0 == "$INSTDIR\zhiyi_tsf_x86.dll"
            StrCpy $UninstallTsfX86Registered 1
        ${EndIf}
    un_snapshot_tsf_done:
    SetRegView 64
FunctionEnd

Function un.PrepareTransaction
    IfFileExists "$INSTDIR\${TRANSACTION_MARKER}" 0 un_prepare_check_existing_transaction
    IfFileExists "$INSTDIR\${INSTALL_MARKER}" 0 un_prepare_install_incomplete
        Delete "$INSTDIR\${TRANSACTION_MARKER}"

    un_prepare_check_existing_transaction:
    IfFileExists "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}" 0 un_prepare_new_transaction
        Call un.LoadTransactionState
        Return

    un_prepare_new_transaction:
    StrCpy $UninstallTsfX64Registered 0
    StrCpy $UninstallTsfX86Registered 0
    StrCpy $UninstallTransactionPhase "prepared"

    Call un.SnapshotTsfRegistration
    Call un.WriteTransactionState
    Return

    un_prepare_install_incomplete:
    StrCpy $FailureMessage \
        "$(L_102)"
    Push 0
FunctionEnd

Function un.PrepareSystemImeRemoval
    WriteINIStr "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}" "remove" "pending" "1"
    IfErrors un_prepare_system_ime_removal_failed
    Push 1
    Return

    un_prepare_system_ime_removal_failed:
    StrCpy $FailureMessage "$(L_103)"
    Push 0
FunctionEnd

Function un.UnregisterInstalledTsf
    StrCmp $UninstallTsfX86Registered "1" 0 un_unregister_installed_x64_path
    StrCpy $2 "$INSTDIR\zhiyi_tsf_x86.dll"
    IfFileExists "$2" 0 un_unregister_installed_x86_missing
        nsExec::ExecToStack '"$SYSDIR\regsvr32.exe" /u /s "$2"'
        Pop $0
        Pop $1
        StrCmp $0 "0" un_unregister_installed_x64_path
            StrCpy $FailureMessage "$(L_104)"
            Push 0
            Return

    un_unregister_installed_x64_path:
    StrCmp $UninstallTsfX64Registered "1" 0 un_unregister_installed_done
    StrCpy $2 "$INSTDIR\zhiyi_tsf_x64.dll"
    IfFileExists "$2" 0 un_unregister_installed_x64_missing
        nsExec::ExecToStack '"$WINDIR\Sysnative\regsvr32.exe" /u /s "$2"'
        Pop $0
        Pop $1
        StrCmp $0 "0" un_unregister_installed_done
            StrCpy $FailureMessage "$(L_105)"
            Push 0
            Return
    un_unregister_installed_done:
    Push 1
    Return

    un_unregister_installed_x86_missing:
    StrCpy $FailureMessage "$(L_106)"
    Push 0
    Return

    un_unregister_installed_x64_missing:
    StrCpy $FailureMessage "$(L_107)"
    Push 0
FunctionEnd

Function un.RemoveSystemIme
    IfFileExists "$InstallBaseDir\maintenance\ime-*.pending" \
        un_remove_system_ime_after_pending
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X64_PENDING}" \
        un_remove_system_ime_after_pending
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X86_PENDING}" \
        un_remove_system_ime_after_pending
    StrCpy $2 0
    ClearErrors
    Delete /REBOOTOK "$WINDIR\Sysnative\zhiyi.ime"
    IfErrors un_remove_system_ime_failed
    IfFileExists "$WINDIR\Sysnative\zhiyi.ime" 0 +2
        StrCpy $2 1
    ClearErrors
    Delete /REBOOTOK "$SYSDIR\zhiyi.ime"
    IfErrors un_remove_system_ime_failed
    IfFileExists "$SYSDIR\zhiyi.ime" 0 +2
        StrCpy $2 1
    StrCmp $2 "1" un_remove_system_ime_deferred
        Delete "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}"
        Push 1
        Return
    un_remove_system_ime_after_pending:
    IfFileExists "$WINDIR\Sysnative\zhiyi.ime" un_queue_system_ime_x64_delete
        System::Call 'kernel32::CopyFileW(\
            w "$INSTDIR\zhiyi_ime_x64.ime", \
            w "$WINDIR\Sysnative\zhiyi.ime", i 0) i .r0'
        StrCmp $0 "0" un_remove_system_ime_failed
    un_queue_system_ime_x64_delete:
    System::Call 'kernel32::MoveFileExW(\
        w "$WINDIR\Sysnative\zhiyi.ime", p 0, \
        i ${MOVEFILE_DELAY_UNTIL_REBOOT}) i .r0 ?e'
    StrCmp $0 "0" un_remove_system_ime_failed
    IfFileExists "$SYSDIR\zhiyi.ime" un_queue_system_ime_x86_delete
        System::Call 'kernel32::CopyFileW(\
            w "$INSTDIR\zhiyi_ime_x86.ime", \
            w "$SYSDIR\zhiyi.ime", i 0) i .r0'
        StrCmp $0 "0" un_remove_system_ime_failed
    un_queue_system_ime_x86_delete:
    System::Call 'kernel32::MoveFileExW(\
        w "$SYSDIR\zhiyi.ime", p 0, \
        i ${MOVEFILE_DELAY_UNTIL_REBOOT}) i .r0 ?e'
    StrCmp $0 "0" un_remove_system_ime_failed
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X64_PENDING}" 0 +2
        System::Call 'kernel32::MoveFileExW(\
            w "$InstallBaseDir\${LEGACY_SYSTEM_IME_X64_PENDING}", \
            p 0, i ${MOVEFILE_DELAY_UNTIL_REBOOT})'
    IfFileExists "$InstallBaseDir\${LEGACY_SYSTEM_IME_X86_PENDING}" 0 +2
        System::Call 'kernel32::MoveFileExW(\
            w "$InstallBaseDir\${LEGACY_SYSTEM_IME_X86_PENDING}", \
            p 0, i ${MOVEFILE_DELAY_UNTIL_REBOOT})'
    SetRebootFlag true
    un_remove_system_ime_deferred:
    System::Call 'kernel32::MoveFileExW(\
        w "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}", \
        p 0, i ${MOVEFILE_DELAY_UNTIL_REBOOT}) i .r0 ?e'
    StrCmp $0 "0" un_remove_system_ime_failed
    Push 1
    Return

    un_remove_system_ime_failed:
    DetailPrint "$(L_108)"
    Push 0
FunctionEnd
