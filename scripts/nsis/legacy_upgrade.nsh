Function UpgradeLegacyInstall
    StrCmp $LegacyUninstallPending "1" 0 upgrade_legacy_install_done
    IfFileExists "$RegisteredInstallDir\uninstall.exe" 0 setup_legacy_uninstaller_missing
    Call ReleaseInstallerMutex
    ExecWait '"$RegisteredInstallDir\uninstall.exe" /S' $2
    ClearErrors
    ReadRegStr $3 HKLM "${UNINSTALL_KEY}" "InstallLocation"
    ${If} ${Errors}
        Goto upgrade_legacy_install_complete
    ${EndIf}

    Call AcquireInstallerMutex
    Call PrepareLegacyDeferredUninstall
    Pop $4
    StrCmp $4 "1" upgrade_legacy_deferred_ready
        Goto setup_legacy_uninstall_failed_locked
    upgrade_legacy_deferred_ready:
    Call ReleaseInstallerMutex
    ExecWait '"$RegisteredInstallDir\uninstall.exe" /S' $2
    ClearErrors
    ReadRegStr $3 HKLM "${UNINSTALL_KEY}" "InstallLocation"
    ${IfNot} ${Errors}
        Call AcquireInstallerMutex
        Call CompleteLegacyUninstallHandoff
        Pop $4
        StrCmp $4 "1" upgrade_legacy_install_complete_locked
        StrCpy $FailureMessage \
            "$(L_058)"
        Goto setup_legacy_uninstall_failed_message
    ${EndIf}
    SetRebootFlag true

    upgrade_legacy_install_complete:
    Call AcquireInstallerMutex
    upgrade_legacy_install_complete_locked:
    StrCpy $LegacyUninstallPerformed 1
    StrCpy $LegacyUninstallPending 0
    StrCpy $RegisteredInstallDir ""
    StrCpy $PreviousInstallDir ""
    StrCpy $InstalledVersion ""
    StrCpy $MultiVersionInstall 0
    StrCpy $ActiveServerDir "$InstallBaseDir"
    StrCpy $StateInstallDir "$InstallBaseDir"
    StrCpy $INSTDIR "$InstallBaseDir"
    StrCpy $InstallTargetDir "$InstallBaseDir\${VERSION}"
    Goto upgrade_legacy_install_done

    setup_legacy_uninstaller_missing:
    StrCpy $2 "missing"
    Call ReleaseInstallerMutex
    Call AcquireInstallerMutex
    setup_legacy_uninstall_failed_locked:
    StrCpy $FailureMessage \
        "$(L_059)"
    setup_legacy_uninstall_failed_message:
    IfSilent setup_legacy_uninstall_failed_silent
        MessageBox MB_ICONSTOP "$FailureMessage"
    setup_legacy_uninstall_failed_silent:
    DetailPrint "$FailureMessage"
    SetErrorLevel 1
    Abort
    upgrade_legacy_install_done:
FunctionEnd

Function PrepareLegacyDeferredUninstall
    nsExec::Exec '"$PLUGINSDIR\zhiyi-installer-helper.exe" release'
    Pop $0
    nsExec::Exec \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" force-stop-server \
        "$RegisteredInstallDir\zhiyi-server.exe"'
    Pop $0
    IfFileExists "$RegisteredInstallDir\zhiyi_tsf_x86.dll" 0 legacy_unregister_x64
        nsExec::ExecToStack \
            '"$SYSDIR\regsvr32.exe" /u /s "$RegisteredInstallDir\zhiyi_tsf_x86.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" legacy_unregister_x64 legacy_deferred_prepare_failed
    legacy_unregister_x64:
    IfFileExists "$RegisteredInstallDir\zhiyi_tsf_x64.dll" 0 legacy_write_deferred_marker
        nsExec::ExecToStack \
            '"$WINDIR\Sysnative\regsvr32.exe" /u /s \
            "$RegisteredInstallDir\zhiyi_tsf_x64.dll"'
        Pop $0
        Pop $1
        StrCmp $0 "0" legacy_write_deferred_marker legacy_deferred_prepare_failed
    legacy_write_deferred_marker:
    ClearErrors
    FileOpen $0 "$RegisteredInstallDir\${LEGACY_UNINSTALL_DEFERRED_MARKER}" w
    IfErrors legacy_deferred_prepare_failed
    FileWriteUTF16LE /BOM $0 "[uninstall]$\r$\n"
    FileWriteUTF16LE $0 "format=1$\r$\n"
    FileWriteUTF16LE $0 "state=removing$\r$\n"
    FileClose $0
    IfErrors legacy_deferred_prepare_failed
    WriteINIStr "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}" "remove" "pending" "1"
    IfErrors legacy_deferred_prepare_failed
    Push 1
    Return

    legacy_deferred_prepare_failed:
    Call RestoreLegacyInstall
    Push 0
FunctionEnd

Function RestoreLegacyInstall
    Delete "$RegisteredInstallDir\${LEGACY_UNINSTALL_DEFERRED_MARKER}"
    Delete "$InstallBaseDir\${SYSTEM_IME_REMOVE_MARKER}"
    IfFileExists "$RegisteredInstallDir\zhiyi_tsf_x64.dll" 0 legacy_restore_x86
        nsExec::Exec \
            '"$WINDIR\Sysnative\regsvr32.exe" /s \
            "$RegisteredInstallDir\zhiyi_tsf_x64.dll"'
        Pop $0
    legacy_restore_x86:
    IfFileExists "$RegisteredInstallDir\zhiyi_tsf_x86.dll" 0 legacy_restore_server
        nsExec::Exec \
            '"$SYSDIR\regsvr32.exe" /s "$RegisteredInstallDir\zhiyi_tsf_x86.dll"'
        Pop $0
    legacy_restore_server:
    IfFileExists "$RegisteredInstallDir\zhiyi-server.exe" 0 legacy_restore_done
        nsExec::Exec \
            '"$PLUGINSDIR\zhiyi-installer-helper.exe" start-server \
            "$RegisteredInstallDir\zhiyi-server.exe"'
        Pop $0
    legacy_restore_done:
FunctionEnd

Function CompleteLegacyUninstallHandoff
    Call CleanupLegacyInstallFiles
    SetRegView 64
    DeleteRegValue HKLM "${RUN_KEY}" "ZhiyiIMEServer"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"
    DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\${CLSID}"
    DeleteRegKey HKLM "SOFTWARE\Microsoft\CTF\TIP\${CLSID}"
    SetRegView 32
    DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\${CLSID}"
    DeleteRegKey HKLM "SOFTWARE\Microsoft\CTF\TIP\${CLSID}"
    SetRegView 64
    ClearErrors
    ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "InstallLocation"
    IfErrors legacy_handoff_complete
        Push 0
        Return
    legacy_handoff_complete:
    SetRebootFlag true
    Push 1
FunctionEnd

Function CleanupLegacyInstallFiles
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi_tsf_x64.dll"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi_tsf_x86.dll"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi_ime_x64.ime"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi_ime_x86.ime"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi-resources.dll"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi-server.exe"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi-settings.exe"
    Delete /REBOOTOK "$RegisteredInstallDir\collect_diagnostics.ps1"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi-ime-host-probe-x64.exe"
    Delete /REBOOTOK "$RegisteredInstallDir\zhiyi-ime-host-probe-x86.exe"
    Delete /REBOOTOK "$RegisteredInstallDir\export_host_trace.ps1"
    Delete /REBOOTOK "$RegisteredInstallDir\license.txt"
    Delete /REBOOTOK "$RegisteredInstallDir\THIRD_PARTY_NOTICES.txt"
    Delete /REBOOTOK "$RegisteredInstallDir\uninstall.exe"
    Delete /REBOOTOK "$RegisteredInstallDir\.zhiyi-install-complete"
    Delete /REBOOTOK "$RegisteredInstallDir\.zhiyi-install-transaction"
    Delete /REBOOTOK "$RegisteredInstallDir\.zhiyi-install-transaction.tmp"
    Delete /REBOOTOK "$RegisteredInstallDir\.zhiyi-uninstall-transaction"
    Delete /REBOOTOK "$RegisteredInstallDir\.zhiyi-uninstall-transaction.tmp"
    Delete /REBOOTOK "$RegisteredInstallDir\${LEGACY_UNINSTALL_DEFERRED_MARKER}"
    Delete /REBOOTOK "$RegisteredInstallDir\data\default.json"
    Delete /REBOOTOK "$RegisteredInstallDir\data\settings_presets.json"
    Delete /REBOOTOK "$RegisteredInstallDir\data\themes.json"
    Delete /REBOOTOK "$RegisteredInstallDir\data\punctuation.json"
    Delete /REBOOTOK "$RegisteredInstallDir\data\symbols.json"
    Delete /REBOOTOK "$RegisteredInstallDir\data\dictionary_manifest.json"
    Delete /REBOOTOK "$RegisteredInstallDir\data\pinyin.dict.bin"
    Delete /REBOOTOK "$RegisteredInstallDir\data\pinyin.dict.idx"
    Delete /REBOOTOK "$RegisteredInstallDir\data\pinyin.spellings.bin"
    Delete /REBOOTOK "$RegisteredInstallDir\data\pinyin.topn.bin"
    Delete /REBOOTOK "$RegisteredInstallDir\data\pinyin.reverse.idx"
    Delete /REBOOTOK "$RegisteredInstallDir\data\wubi86.dict.bin"
    Delete /REBOOTOK "$RegisteredInstallDir\data\wubi86.dict.idx"
    Delete /REBOOTOK "$RegisteredInstallDir\data\wubi86.reverse.idx"
    Delete /REBOOTOK "$RegisteredInstallDir\licenses\rime-ice-GPL-3.0.txt"
    Delete /REBOOTOK "$RegisteredInstallDir\licenses\miniz-MIT.txt"
    Delete /REBOOTOK "$RegisteredInstallDir\licenses\Apache-2.0.txt"
    RMDir /r /REBOOTOK "$RegisteredInstallDir\.zhiyi-rollback"
    RMDir /r /REBOOTOK "$RegisteredInstallDir\.zhiyi-uninstall-rollback"
    RMDir /REBOOTOK "$RegisteredInstallDir\data"
    RMDir /REBOOTOK "$RegisteredInstallDir\licenses"
    RMDir /r /REBOOTOK "$InstallBaseDir\update"
    RMDir /r /REBOOTOK "$InstallBaseDir\.zhiyi-backup"
    StrCmp $RegisteredInstallDir $InstallBaseDir legacy_cleanup_done
        RMDir /REBOOTOK "$RegisteredInstallDir"
    legacy_cleanup_done:
FunctionEnd
