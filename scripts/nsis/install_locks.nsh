Function StopServer
    StrCpy $ServerStopResult 0
    StrCpy $ServerWasRunning $InitialServerWasRunning
    StrCmp $InitialServerWasRunning "1" stop_server_request stop_server_done
    stop_server_request:
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" stop-server "$ActiveServerDir\zhiyi-server.exe"'
    Pop $0
    StrCmp $0 "0" stop_server_wait_start
        StrCpy $ServerStopResult 2
        Return
    stop_server_wait_start:
    StrCpy $1 0
    stop_server_wait:
        Sleep 100
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$ActiveServerDir\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "1" stop_server_done
        StrCmp $0 "2" stop_server_failed
        IntOp $1 $1 + 1
        IntCmp $1 30 stop_server_force stop_server_wait stop_server_force
    stop_server_force:
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" force-stop-server "$ActiveServerDir\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "0" stop_server_force_wait
        Goto stop_server_failed
    stop_server_force_wait:
        StrCpy $1 0
    stop_server_force_poll:
        Sleep 100
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$ActiveServerDir\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "1" stop_server_done
        StrCmp $0 "2" stop_server_failed
        IntOp $1 $1 + 1
        IntCmp $1 30 stop_server_failed stop_server_force_poll stop_server_failed
    stop_server_failed:
        StrCpy $ServerStopResult 2
        Return
    stop_server_done:
    StrCpy $ServerWasRunning $InitialServerWasRunning
FunctionEnd

Function QueryTipRegistration
    StrCpy $1 0
    tip_registration_enum:
        ClearErrors
        EnumRegKey $0 HKLM "${TSF_TIP_KEY}" $1
        IfErrors tip_registration_missing
        StrCmp $0 "LanguageProfile" tip_registration_profile
        IntOp $1 $1 + 1
        Goto tip_registration_enum
    tip_registration_profile:
        ClearErrors
        EnumRegKey $0 HKLM "${TSF_TIP_KEY}\LanguageProfile" 0
        IfErrors tip_registration_missing
        Push 1
        Return
    tip_registration_missing:
        Push 0
FunctionEnd

Function CaptureServerState
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$ActiveServerDir\zhiyi-server.exe"'
    Pop $0
    StrCmp $0 "0" capture_server_running
    StrCmp $0 "1" capture_server_not_running
        StrCpy $FailureMessage "$(L_023)"
        Push 0
        Return
    capture_server_not_running:
        StrCpy $InitialServerWasRunning 0
        StrCpy $ServerWasRunning 0
        Push 1
        Return
    capture_server_running:
        StrCpy $InitialServerWasRunning 1
        StrCpy $ServerWasRunning 1
        nsExec::ExecToStack \
            '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-pid "$ActiveServerDir\zhiyi-server.exe"'
        Pop $1
        Pop $2
        StrCmp $1 "0" 0 capture_server_query_failed
        StrCpy $ServerProcessId $2
        Push 1
        Return
    capture_server_query_failed:
        StrCpy $FailureMessage "$(L_024)"
        Push 0
FunctionEnd

Function RestartInstalledServer
    StrCpy $ServerRestartResult 0
    StrCmp $InitialServerWasRunning "1" 0 restart_installed_server_done
    StrCmp $InstallStateVerified "1" 0 restart_installed_server_failed
    IfFileExists "$ActiveServerDir\zhiyi-server.exe" 0 restart_installed_server_failed
        ClearErrors
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" start-server "$ActiveServerDir\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "0" 0 restart_installed_server_failed
        StrCpy $1 0
    restart_installed_server_wait:
        Sleep 100
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$ActiveServerDir\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "0" restart_installed_server_ready
        StrCmp $0 "2" restart_installed_server_failed
        IntOp $1 $1 + 1
        IntCmp $1 30 restart_installed_server_failed restart_installed_server_wait \
            restart_installed_server_failed
    restart_installed_server_ready:
        StrCpy $ServerRestartResult 1
        DetailPrint "$(L_025)"
        Goto restart_installed_server_done
    restart_installed_server_failed:
        StrCpy $ServerRestartResult 2
        DetailPrint "$(L_026)"
    restart_installed_server_done:
FunctionEnd

Function StartNewServer
    IfFileExists "$INSTDIR\zhiyi-server.exe" 0 start_new_server_failed
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" start-server "$INSTDIR\zhiyi-server.exe"'
    Pop $0
    StrCmp $0 "0" start_new_server_poll start_new_server_failed
    StrCpy $1 0
    start_new_server_poll:
        Sleep 100
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-ready "$INSTDIR\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "0" start_new_server_ready
        StrCmp $0 "2" start_new_server_failed
        IntOp $1 $1 + 1
        IntCmp $1 30 start_new_server_failed start_new_server_poll start_new_server_failed
    start_new_server_ready:
        Push 1
        Return
    start_new_server_failed:
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" force-stop-server "$INSTDIR\zhiyi-server.exe"'
        Pop $0
        StrCpy $FailureMessage "$(L_027)"
        Push 0
FunctionEnd

Function CleanupRuntimeSnapshotAfterServerRestore
    StrCmp $ServerRestartResult "2" cleanup_runtime_snapshot_done
        Delete "$INSTDIR\..\${RUNTIME_MARKER}"
    cleanup_runtime_snapshot_done:
FunctionEnd

Function ReleaseInputProcessor
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" release'
    Pop $0
    Sleep 500
    StrCmp $0 "0" release_input_processor_done
        DetailPrint "$(L_028)"
    release_input_processor_done:
FunctionEnd

Function VerifyRestoredInstall
    ${If} $MultiVersionInstall == 1
        StrCpy $INSTDIR "$StateInstallDir"
    ${EndIf}
    StrCmp $OldInstallAvailable "1" restored_install_check_files
    IfFileExists "$INSTDIR\zhiyi-server.exe" 0 restored_install_no_old_resources
        Goto restored_install_invalid
    restored_install_no_old_resources:
    IfFileExists "$INSTDIR\zhiyi-resources.dll" 0 restored_install_no_old_tsf_x64
        Goto restored_install_invalid
    restored_install_no_old_tsf_x64:
    IfFileExists "$INSTDIR\zhiyi_tsf_x64.dll" 0 restored_install_no_old_tsf_x86
        Goto restored_install_invalid
    restored_install_no_old_tsf_x86:
    IfFileExists "$INSTDIR\zhiyi_tsf_x86.dll" 0 restored_install_no_old_ime_x64
        Goto restored_install_invalid
    restored_install_no_old_ime_x64:
    IfFileExists "$WINDIR\Sysnative\zhiyi.ime" 0 restored_install_no_old_ime_x86
        Goto restored_install_invalid
    restored_install_no_old_ime_x86:
    IfFileExists "$SYSDIR\zhiyi.ime" 0 restored_install_registry
        Goto restored_install_invalid

    restored_install_check_files:
    IfFileExists "$INSTDIR\zhiyi-server.exe" 0 restored_install_invalid
    IfFileExists "$INSTDIR\zhiyi-resources.dll" 0 restored_install_invalid
    StrCmp $OldTsfX64Present "1" restored_install_tsf_x64_present restored_install_tsf_x64_absent
    restored_install_tsf_x64_present:
    IfFileExists "$INSTDIR\zhiyi_tsf_x64.dll" 0 restored_install_invalid
    Goto restored_install_x86
    restored_install_tsf_x64_absent:
    IfFileExists "$INSTDIR\zhiyi_tsf_x64.dll" 0 restored_install_x86
        Goto restored_install_invalid
    restored_install_x86:
    StrCmp $OldTsfX86Present "1" restored_install_tsf_x86_present restored_install_tsf_x86_absent
    restored_install_tsf_x86_present:
    IfFileExists "$INSTDIR\zhiyi_tsf_x86.dll" 0 restored_install_invalid
    Goto restored_install_registry
    restored_install_tsf_x86_absent:
    IfFileExists "$INSTDIR\zhiyi_tsf_x86.dll" 0 restored_install_registry
        Goto restored_install_invalid
    restored_install_registry:
    ${If} $OldTsfX64Registered == 1
        SetRegView 64
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${If} ${Errors}
            Goto restored_install_invalid
        ${EndIf}
        ${If} $0 != "$INSTDIR\zhiyi_tsf_x64.dll"
            Goto restored_install_invalid
        ${EndIf}
    ${Else}
        SetRegView 64
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${IfNot} ${Errors}
            Goto restored_install_invalid
        ${EndIf}
    ${EndIf}
    ${If} $OldTsfX86Registered == 1
        SetRegView 32
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${If} ${Errors}
            SetRegView 64
            Goto restored_install_invalid
        ${EndIf}
        ${If} $0 != "$INSTDIR\zhiyi_tsf_x86.dll"
            SetRegView 64
            Goto restored_install_invalid
        ${EndIf}
        SetRegView 64
    ${Else}
        SetRegView 32
        ClearErrors
        ReadRegStr $0 HKLM "${TSF_INPROC_KEY}" ""
        ${IfNot} ${Errors}
            SetRegView 64
            Goto restored_install_invalid
        ${EndIf}
        SetRegView 64
    ${EndIf}
    SetRegView 64
    Call QueryTipRegistration
    Pop $0
    ${If} $0 != $OldTipX64Present
        Goto restored_install_invalid
    ${EndIf}
    SetRegView 32
    Call QueryTipRegistration
    Pop $0
    ${If} $0 != $OldTipX86Present
        SetRegView 64
        Goto restored_install_invalid
    ${EndIf}
    SetRegView 64
    ClearErrors
    ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "DisplayVersion"
    ${If} $OldUninstallPresent == 1
        ${If} ${Errors}
            Goto restored_install_invalid
        ${EndIf}
        ${If} $0 != "$OldDisplayVersion"
            Goto restored_install_invalid
        ${EndIf}
    ${Else}
        ${IfNot} ${Errors}
            Goto restored_install_invalid
        ${EndIf}
    ${EndIf}
    ClearErrors
    ReadRegStr $0 HKLM "${RUN_KEY}" "ZhiyiIMEServer"
    ${If} $OldRunPresent == 1
        ${If} ${Errors}
            Goto restored_install_invalid
        ${EndIf}
        ${If} $0 != "$OldRunValue"
            Goto restored_install_invalid
        ${EndIf}
    ${Else}
        ${IfNot} ${Errors}
            Goto restored_install_invalid
        ${EndIf}
    ${EndIf}
    StrCpy $InstallStateVerified 1
    Push 1
    Return
    restored_install_invalid:
        StrCpy $InstallStateVerified 0
        StrCpy $FailureMessage \
            "$(L_029)"
        Push 0
FunctionEnd

Function ReadLockReport
    StrCpy $LockReportText ""
    ClearErrors
    FileOpen $0 "$LockReportPath" r
    IfErrors lock_report_done
    lock_report_read:
        ClearErrors
        FileReadUTF16LE $0 $1
        IfErrors lock_report_close
        StrCpy $LockReportText "$LockReportText$1"
        Goto lock_report_read
    lock_report_close:
        FileClose $0
    lock_report_done:
    ${If} $LockReportText == ""
        StrCpy $LockReportText \
            "$(L_030)"
    ${EndIf}
FunctionEnd

Function CollectPreviousVersionLockNotice
    StrCpy $InstallLockNotice 0
    StrCpy $LockReportText ""
    StrCmp $MultiVersionInstall "1" 0 collect_previous_locks_done
    StrCmp $PreviousInstallDir "" collect_previous_locks_done

    Delete "$LockReportPath"
    nsExec::ExecToStack /TIMEOUT=3000 \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" query --report "$LockReportPath" \
        "$PreviousInstallDir\zhiyi_tsf_x64.dll" \
        "$PreviousInstallDir\zhiyi_tsf_x86.dll" \
        "$PreviousInstallDir\zhiyi_ime_x64.ime" \
        "$PreviousInstallDir\zhiyi_ime_x86.ime" \
        "$PreviousInstallDir\zhiyi-resources.dll" \
        "$PreviousInstallDir\zhiyi-server.exe" \
        "$PreviousInstallDir\zhiyi-settings.exe" \
        "$PreviousInstallDir\zhiyi-ime-host-probe-x64.exe" \
        "$PreviousInstallDir\zhiyi-ime-host-probe-x86.exe" \
        "$PreviousInstallDir\uninstall.exe" \
        "$WINDIR\System32\zhiyi.ime" "$SYSDIR\zhiyi.ime"'
    Pop $0
    Pop $1
    StrCmp $0 "2" collect_previous_locks_found
    StrCmp $0 "3" collect_previous_locks_reboot
    StrCmp $0 "5" collect_previous_locks_found_and_reboot
    StrCmp $0 "timeout" collect_previous_locks_timeout
    StrCmp $0 "0" collect_previous_locks_done
        DetailPrint "$(L_031)"
        Goto collect_previous_locks_done

    collect_previous_locks_timeout:
        DetailPrint "$(L_032)"
        Goto collect_previous_locks_done

    collect_previous_locks_found_and_reboot:
        SetRebootFlag true
        Goto collect_previous_locks_found

    collect_previous_locks_reboot:
        SetRebootFlag true
        Goto collect_previous_locks_done

    collect_previous_locks_found:
        StrCpy $InstallLockNotice 1
        Call ReadLockReport
        DetailPrint "$LockReportText"

    collect_previous_locks_done:
FunctionEnd
