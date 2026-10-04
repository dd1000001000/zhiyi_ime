Function un.StopServer
    StrCpy $UninstallServerStopResult 0
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$INSTDIR\zhiyi-server.exe"'
    Pop $0
    StrCmp $0 "1" un_stop_server_done
    StrCmp $0 "0" un_server_found
        StrCpy $UninstallServerStopResult 2
        Return
    un_server_found:
    StrCpy $UninstallServerWasRunning 1
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" stop-server "$INSTDIR\zhiyi-server.exe"'
    Pop $0
    StrCmp $0 "0" un_stop_server_wait_start
        StrCpy $UninstallServerStopResult 2
        Return
    un_stop_server_wait_start:
    StrCpy $1 0
    un_stop_server_wait:
        Sleep 100
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$INSTDIR\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "1" un_stop_server_done
        StrCmp $0 "2" un_stop_server_failed
        IntOp $1 $1 + 1
        IntCmp $1 30 un_stop_server_force un_stop_server_wait un_stop_server_force
    un_stop_server_force:
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" force-stop-server "$INSTDIR\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "0" un_stop_server_force_wait
        Goto un_stop_server_failed
    un_stop_server_force_wait:
        StrCpy $1 0
    un_stop_server_force_poll:
        Sleep 100
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$INSTDIR\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "1" un_stop_server_done
        StrCmp $0 "2" un_stop_server_failed
        IntOp $1 $1 + 1
        IntCmp $1 30 un_stop_server_failed un_stop_server_force_poll un_stop_server_failed
    un_stop_server_failed:
        StrCpy $UninstallServerStopResult 2
        Return
    un_stop_server_done:
FunctionEnd

Function un.RestartInstalledServer
    StrCmp $UninstallServerWasRunning "1" 0 un_restart_installed_server_done
    IfFileExists "$INSTDIR\zhiyi-server.exe" 0 un_restart_installed_server_done
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" start-server "$INSTDIR\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "0" un_restart_server_wait_start
        Goto un_restart_server_failed
    un_restart_server_wait_start:
        StrCpy $1 0
    un_restart_server_wait:
        Sleep 100
        nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" server-running "$INSTDIR\zhiyi-server.exe"'
        Pop $0
        StrCmp $0 "0" un_restart_installed_server_done
        IntOp $1 $1 + 1
        IntCmp $1 30 un_restart_server_failed un_restart_server_wait un_restart_server_failed
    un_restart_server_failed:
        StrCpy $FailureMessage "$(L_098)"
    un_restart_installed_server_done:
FunctionEnd

Function un.ReleaseInputProcessor
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" release'
    Pop $0
    Sleep 500
    StrCmp $0 "0" un_release_input_processor_done
        DetailPrint "$(L_028)"
    un_release_input_processor_done:
FunctionEnd

Function un.ReadLockReport
    StrCpy $LockReportText ""
    ClearErrors
    FileOpen $0 "$LockReportPath" r
    IfErrors un_lock_report_done
    un_lock_report_read:
        ClearErrors
        FileReadUTF16LE $0 $1
        IfErrors un_lock_report_close
        StrCpy $LockReportText "$LockReportText$1"
        Goto un_lock_report_read
    un_lock_report_close:
        FileClose $0
    un_lock_report_done:
    ${If} $LockReportText == ""
        StrCpy $LockReportText \
            "$(L_030)"
    ${EndIf}
FunctionEnd

Function un.CheckFileLocks
    Delete "$LockReportPath"
    nsExec::ExecToStack \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" query --report "$LockReportPath" \
        "$INSTDIR\zhiyi_tsf_x64.dll" "$INSTDIR\zhiyi_tsf_x86.dll" \
        "$INSTDIR\zhiyi_ime_x64.ime" "$INSTDIR\zhiyi_ime_x86.ime" \
        "$INSTDIR\zhiyi-resources.dll" "$INSTDIR\zhiyi-server.exe" \
        "$INSTDIR\zhiyi-settings.exe" "$INSTDIR\uninstall.exe" \
        "$WINDIR\System32\zhiyi.ime" "$SYSDIR\zhiyi.ime"'
    Pop $0
    Pop $1
    StrCmp $0 "0" un_lock_done
        Call un.ReadLockReport
        DetailPrint "$LockReportText"
    un_lock_done:
FunctionEnd
