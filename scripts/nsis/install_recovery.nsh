Function LoadTransactionState
    StrCpy $ServerWasRunning ""
    StrCpy $TransactionServerWasRunning ""
    StrCpy $OldInstallAvailable 0
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
    ClearErrors
    ReadINIStr $0 "$TransactionDir\${TRANSACTION_MARKER}" "transaction" "format"
    IfErrors transaction_state_invalid
    StrCmp $0 "4" 0 transaction_state_invalid
    ReadINIStr $OldInstallAvailable "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_install_available"
    ReadINIStr $OldTsfX64Present "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_tsf_x64_present"
    ReadINIStr $OldTsfX86Present "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_tsf_x86_present"
    ReadINIStr $OldTsfX64Registered "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_tsf_x64_registered"
    ReadINIStr $OldTsfX86Registered "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_tsf_x86_registered"
    ReadINIStr $OldTipX64Present "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_tip_x64_present"
    ReadINIStr $OldTipX86Present "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_tip_x86_present"
    ReadINIStr $OldUninstallPresent "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_uninstall_present"
    ReadINIStr $OldDisplayVersion "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_display_version"
    ReadINIStr $OldRunPresent "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_run_present"
    ReadINIStr $OldRunValue "$TransactionDir\${TRANSACTION_MARKER}" "transaction" "old_run_value"
    ReadINIStr $PreviousInstallDir "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "old_install_dir"
    StrCpy $StateInstallDir "$PreviousInstallDir"
    ${If} $PreviousInstallDir != ""
        StrCpy $MultiVersionInstall 1
        StrCpy $ActiveServerDir "$PreviousInstallDir"
    ${EndIf}
    ReadINIStr $ServerWasRunning "$TransactionDir\${TRANSACTION_MARKER}" "transaction" \
        "server_was_running"
    StrCpy $TransactionServerWasRunning $ServerWasRunning
    StrCmp $OldInstallAvailable "0" transaction_old_install_valid
    StrCmp $OldInstallAvailable "1" transaction_old_install_valid transaction_state_invalid
    transaction_old_install_valid:
    StrCmp $OldTsfX64Present "0" transaction_tsf_x64_valid
    StrCmp $OldTsfX64Present "1" transaction_tsf_x64_valid transaction_state_invalid
    transaction_tsf_x64_valid:
    StrCmp $OldTsfX86Present "0" transaction_tsf_x86_valid
    StrCmp $OldTsfX86Present "1" transaction_tsf_x86_valid transaction_state_invalid
    transaction_tsf_x86_valid:
    StrCmp $OldTsfX64Registered "0" transaction_tsf_x64_registration_valid
    StrCmp $OldTsfX64Registered "1" transaction_tsf_x64_registration_valid \
        transaction_state_invalid
    transaction_tsf_x64_registration_valid:
    StrCmp $OldTsfX86Registered "0" transaction_tsf_x86_registration_valid
    StrCmp $OldTsfX86Registered "1" transaction_tsf_x86_registration_valid \
        transaction_state_invalid
    transaction_tsf_x86_registration_valid:
    StrCmp $OldTipX64Present "0" transaction_tip_x86_valid
    StrCmp $OldTipX64Present "1" transaction_tip_x86_valid transaction_state_invalid
    transaction_tip_x86_valid:
    StrCmp $OldTipX86Present "0" transaction_old_install_state_valid
    StrCmp $OldTipX86Present "1" transaction_old_install_state_valid transaction_state_invalid
    transaction_old_install_state_valid:
    StrCmp $OldTsfX64Registered "0" transaction_tsf_x64_state_valid
    StrCmp $OldTsfX64Present "1" transaction_tsf_x64_state_valid transaction_state_invalid
    transaction_tsf_x64_state_valid:
    StrCmp $OldTsfX86Registered "0" transaction_tsf_x86_state_valid
    StrCmp $OldTsfX86Present "1" transaction_tsf_x86_state_valid transaction_state_invalid
    transaction_tsf_x86_state_valid:
    StrCmp $OldUninstallPresent "0" transaction_uninstall_valid
    StrCmp $OldUninstallPresent "1" transaction_uninstall_valid transaction_state_invalid
    transaction_uninstall_valid:
    StrCmp $OldRunPresent "0" transaction_run_valid
    StrCmp $OldRunPresent "1" transaction_run_valid transaction_state_invalid
    transaction_run_valid:
    StrCmp $TransactionServerWasRunning "0" transaction_server_state_valid
    StrCmp $TransactionServerWasRunning "1" transaction_server_state_valid transaction_state_invalid
    transaction_server_state_valid:
    Push 1
    Return

    transaction_state_invalid:
    StrCpy $FailureMessage "先前的知意输入法安装事务不完整或无效。"
    Push 0
FunctionEnd

Function UnregisterTransactionTsf
    IfFileExists "$TransactionDir\zhiyi_tsf_x86.dll" 0 unregister_transaction_x64
        nsExec::ExecToStack \
            '"$SYSDIR\regsvr32.exe" /u /s "$TransactionDir\zhiyi_tsf_x86.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" unregister_transaction_x64
            StrCpy $FailureMessage "无法注销中断安装留下的 32 位 TSF 模块。"
            Push 0
            Return
    unregister_transaction_x64:
    IfFileExists "$TransactionDir\zhiyi_tsf_x64.dll" 0 unregister_transaction_done
        nsExec::ExecToStack \
            '"$WINDIR\Sysnative\regsvr32.exe" /u /s "$TransactionDir\zhiyi_tsf_x64.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" unregister_transaction_done
            StrCpy $FailureMessage "无法注销中断安装留下的 64 位 TSF 模块。"
            Push 0
            Return
    unregister_transaction_done:
    Push 1
FunctionEnd

Function RecoverTransaction
    Call LoadTransactionState
    Pop $0
    StrCmp $0 "1" transaction_state_loaded
        Push 0
        Return

    transaction_state_loaded:
    Call CancelPendingSystemImeUpdate
    StrCmp $MultiVersionInstall "1" transaction_multiversion_restore
    Goto transaction_remove_partial_install

    transaction_multiversion_restore:
    Call UnregisterTransactionTsf
    Pop $0
    StrCmp $0 "1" transaction_multiversion_restore_register
        Push 0
        Return
    transaction_multiversion_restore_register:
    Call RegisterPreviousTsf
    Pop $0
    StrCmp $0 "1" transaction_multiversion_restore_registry
        Push 0
        Return
    transaction_multiversion_restore_registry:
    Call RestorePreviousRegistry
    Pop $0
    StrCmp $0 "1" transaction_multiversion_restore_cleanup
        Push 0
        Return
    transaction_multiversion_restore_cleanup:
    RMDir /r "$INSTDIR"
    IfFileExists "$INSTDIR\*" transaction_recovery_failed
    StrCmp $TransactionDir $INSTDIR transaction_multiversion_restore_done
        RMDir /r "$TransactionDir"
        IfFileExists "$TransactionDir" transaction_recovery_failed
    transaction_multiversion_restore_done:
    Push 1
    Return

    transaction_remove_partial_install:
    Call UnregisterTransactionTsf
    Pop $0
    StrCmp $0 "1" transaction_remove_partial_files
        Push 0
        Return
    transaction_remove_partial_files:
    RMDir /r "$TransactionDir"
    IfFileExists "$TransactionDir\*" transaction_recovery_failed
    Call RestorePreviousRegistry
    Pop $0
    StrCmp $0 "1" transaction_cleanup_done
        Push 0
        Return
    transaction_cleanup_done:
    DetailPrint "已恢复先前的知意输入法安装状态。"
    Push 1
    Return

    transaction_recovery_failed:
    StrCpy $FailureMessage "无法恢复先前的知意输入法程序文件。"
    Push 0
FunctionEnd

Function RecoverInterruptedInstall
    IfFileExists "$INSTDIR\${TRANSACTION_MARKER}" recover_target_transaction
    IfFileExists "$StageDir\${TRANSACTION_MARKER}" recover_stage_transaction
    Goto recover_check_registered_install

    recover_target_transaction:
        StrCpy $TransactionDir "$INSTDIR"
        Call RecoverTransaction
        Return

    recover_stage_transaction:
        StrCpy $TransactionDir "$StageDir"
        Call RecoverTransaction
        Return

    recover_check_registered_install:
    ${If} $MultiVersionInstall == 1
        IfFileExists "$PreviousInstallDir\${UNINSTALL_TRANSACTION_MARKER}" \
            recover_uninstall_transaction_pending
    ${EndIf}

    IfFileExists "$INSTDIR\${UNINSTALL_TRANSACTION_MARKER}" recover_uninstall_transaction_pending
    Goto recover_remove_uncommitted_stage
    recover_uninstall_transaction_pending:
        StrCpy $FailureMessage \
            "上一次知意输入法卸载尚未完成。请先重新运行已安装的卸载程序。"
        Push 0
        Return

    recover_remove_uncommitted_stage:
    ClearErrors
    RMDir /r "$StageDir"
    IfErrors recover_staged_cleanup_failed
    IfFileExists "$StageDir" 0 recover_remove_uncommitted_stage_done
        Goto recover_staged_cleanup_failed
    recover_remove_uncommitted_stage_done:
    Push 1
    Return

    recover_staged_cleanup_failed:
    StrCpy $FailureMessage "无法清理已完成安装留下的知意输入法文件。"
    Push 0
FunctionEnd
