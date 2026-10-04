Function AcquireInstallerMutex
    System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Global\ZhiyiIME.Installation") p .r1 ?e'
    Pop $0
    ${If} $1 == 0
        StrCpy $FailureMessage "无法初始化知意输入法安装程序。"
        IfSilent installer_mutex_failed_silent
            MessageBox MB_ICONSTOP "$FailureMessage"
        installer_mutex_failed_silent:
        DetailPrint "$FailureMessage"
        SetErrorLevel 1
        Abort
    ${EndIf}
    ${If} $0 == ${ERROR_ALREADY_EXISTS}
        StrCpy $FailureMessage "另一个知意输入法安装程序或卸载程序正在运行。"
        IfSilent installer_already_running_silent
            MessageBox MB_ICONSTOP "$FailureMessage"
        installer_already_running_silent:
        DetailPrint "$FailureMessage"
        SetErrorLevel 1
        Abort
    ${EndIf}
    StrCpy $InstallMutexHandle $1
FunctionEnd

Function ReleaseInstallerMutex
    StrCmp $InstallMutexHandle "0" release_installer_mutex_done
        System::Call 'kernel32::CloseHandle(p $InstallMutexHandle)'
        StrCpy $InstallMutexHandle 0
    release_installer_mutex_done:
FunctionEnd

Function .onInit
    StrCpy $InstallLockNotice 0
    StrCpy $InstallLockDetailsVisible 0
    StrCpy $LockReportText ""
    StrCpy $ServerRestartResult 0
    StrCpy $InstallStateVerified 1
    StrCpy $InstallBaseHandle 0
    StrCpy $InstallMutexHandle 0
    StrCpy $LegacyUninstallPerformed 0
    StrCpy $LegacyUninstallPending 0
    StrCpy $InstalledVersion ""
    StrCpy $AllowDowngrade 0
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/ALLOWDOWNGRADE" $1
    ${IfNot} ${Errors}
        StrCpy $AllowDowngrade 1
    ${EndIf}
    StrCpy $InitialServerWasRunning 0
    StrCpy $TransactionServerWasRunning ""
    StrCpy $ServerStopResult 0
    StrCpy $ServerProcessId 0
    StrCpy $InstallBaseDir "$PROGRAMFILES64\ZhiyiIME"
    StrCpy $PreviousInstallDir ""
    StrCpy $MultiVersionInstall 0
    StrCpy $ActiveServerDir "$PROGRAMFILES64\ZhiyiIME"
    StrCpy $StateInstallDir "$PROGRAMFILES64\ZhiyiIME"
    StrCpy $InstallTargetDir "$PROGRAMFILES64\ZhiyiIME\${VERSION}"
    StrCpy $InstallTargetPrepared 0
    StrCpy $OldTipX64Present 0
    StrCpy $OldTipX86Present 0
    StrCpy $LifecycleScheduled 0
    ${IfNot} ${RunningX64}
        StrCpy $FailureMessage "知意输入法需要 64 位 Windows。"
        IfSilent installer_requires_x64_silent
            MessageBox MB_ICONSTOP "$FailureMessage"
        installer_requires_x64_silent:
        DetailPrint "$FailureMessage"
        SetErrorLevel 1
        Abort
    ${EndIf}

    Call AcquireInstallerMutex
    SetShellVarContext all
    SetRegView 64
    StrCpy $INSTDIR "$PROGRAMFILES64\ZhiyiIME"
    StrCpy $RegisteredInstallDir ""
    ClearErrors
    ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "InstallLocation"
    ${IfNot} ${Errors}
    ${AndIf} $0 != ""
        StrCpy $RegisteredInstallDir $0
        StrCpy $PreviousInstallDir $0
        StrCpy $INSTDIR $0
        ClearErrors
        ReadRegStr $InstalledVersion HKLM "${UNINSTALL_KEY}" "DisplayVersion"
        ${If} ${Errors}
            StrCpy $InstalledVersion ""
        ${EndIf}
        ClearErrors
        ReadRegStr $1 HKLM "${UNINSTALL_KEY}" "InstallBaseLocation"
        ${IfNot} ${Errors}
        ${AndIf} $1 != ""
            StrCpy $InstallBaseDir $1
        ${Else}
            StrCpy $InstallBaseDir $0
        ${EndIf}
        IfFileExists "$InstallBaseDir\maintenance\install-state.json" setup_lifecycle_install
        IfFileExists "$RegisteredInstallDir\${LEGACY_UNINSTALL_DEFERRED_MARKER}" \
            setup_mark_legacy_install
        IfFileExists "$RegisteredInstallDir\${INSTALL_MARKER}" 0 setup_unknown_install
        IfFileExists "$RegisteredInstallDir\uninstall.exe" 0 setup_unknown_install
        IfFileExists "$RegisteredInstallDir\zhiyi-server.exe" setup_mark_legacy_install
        IfFileExists "$RegisteredInstallDir\zhiyi_tsf_x64.dll" setup_mark_legacy_install
        setup_unknown_install:
        StrCpy $FailureMessage \
            "检测到无法自动升级的知意输入法安装。请先卸载当前版本，再运行此安装程序。"
        IfSilent setup_unknown_install_silent
            MessageBox MB_ICONSTOP "$FailureMessage"
        setup_unknown_install_silent:
        DetailPrint "$FailureMessage"
        SetErrorLevel 1
        Abort
        setup_lifecycle_install:
        StrCpy $INSTDIR $InstallBaseDir
        StrCpy $InstallTargetDir "$InstallBaseDir\${VERSION}"
        StrCpy $MultiVersionInstall 1
    ${EndIf}
    Return

    setup_mark_legacy_install:
    StrCpy $LegacyUninstallPending 1
FunctionEnd

Function CheckInstallVersion
    StrCmp $InstalledVersion "" check_install_version_done
    nsExec::Exec \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" compare-version \
        "$InstalledVersion" "${VERSION}"'
    Pop $0
    StrCmp $0 "0" check_install_version_done
    StrCmp $0 "1" check_install_version_done
    StrCmp $0 "2" check_install_version_downgrade
        StrCpy $FailureMessage \
            "无法比较已安装版本 $InstalledVersion 与安装包版本 ${VERSION}。"
        Goto check_install_version_failed

    check_install_version_downgrade:
    StrCmp $AllowDowngrade "1" check_install_version_done
    IfSilent check_install_version_silent_downgrade
        MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 \
            "当前已安装知意输入法 $InstalledVersion。继续将降级到 ${VERSION}。$\r$\n$\r$\n是否继续？" \
            IDYES check_install_version_done
        StrCpy $FailureMessage "用户取消了知意输入法降级安装。"
        Goto check_install_version_cancelled
    check_install_version_silent_downgrade:
    StrCpy $FailureMessage \
        "静默安装默认不允许从 $InstalledVersion 降级到 ${VERSION}。请显式使用 /ALLOWDOWNGRADE。"
    check_install_version_failed:
    IfSilent check_install_version_report
        MessageBox MB_ICONSTOP "$FailureMessage"
    check_install_version_report:
    DetailPrint "$FailureMessage"
    check_install_version_cancelled:
    SetErrorLevel 2
    Abort
    check_install_version_done:
FunctionEnd

Function RefreshInstallLayoutAfterRecovery
    StrCpy $RegisteredInstallDir ""
    StrCpy $PreviousInstallDir ""
    StrCpy $MultiVersionInstall 0
    ClearErrors
    ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "InstallLocation"
    IfErrors refresh_install_layout_fresh
    StrCmp $0 "" refresh_install_layout_fresh

    StrCpy $RegisteredInstallDir $0
    StrCpy $PreviousInstallDir $0
    StrCpy $ActiveServerDir $0
    StrCpy $StateInstallDir $0
    StrCpy $MultiVersionInstall 1
    ClearErrors
    ReadRegStr $1 HKLM "${UNINSTALL_KEY}" "InstallBaseLocation"
    ${IfNot} ${Errors}
    ${AndIf} $1 != ""
        StrCpy $InstallBaseDir $1
    ${Else}
        StrCpy $InstallBaseDir $0
    ${EndIf}
    StrCpy $InstallTargetDir "$InstallBaseDir\${VERSION}"
    StrCpy $INSTDIR $InstallTargetDir
    Return

    refresh_install_layout_fresh:
    StrCpy $ActiveServerDir $InstallBaseDir
    StrCpy $StateInstallDir $InstallBaseDir
    StrCpy $InstallTargetDir "$InstallBaseDir\${VERSION}"
    StrCpy $INSTDIR $InstallTargetDir
FunctionEnd

Function un.AcquireInstallerMutex
    System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Global\ZhiyiIME.Installation") p .r1 ?e'
    Pop $0
    ${If} $1 == 0
        StrCpy $FailureMessage "无法初始化知意输入法卸载程序。"
        IfSilent un_mutex_failed_silent
            MessageBox MB_ICONSTOP "$FailureMessage"
        un_mutex_failed_silent:
        DetailPrint "$FailureMessage"
        SetErrorLevel 1
        Abort
    ${EndIf}
    ${If} $0 == ${ERROR_ALREADY_EXISTS}
        StrCpy $FailureMessage "另一个知意输入法安装程序或卸载程序正在运行。"
        IfSilent un_already_running_silent
            MessageBox MB_ICONSTOP "$FailureMessage"
        un_already_running_silent:
        DetailPrint "$FailureMessage"
        SetErrorLevel 1
        Abort
    ${EndIf}
FunctionEnd

Function un.onInit
    Call un.AcquireInstallerMutex
    SetShellVarContext all
    SetRegView 64
    StrCpy $UninstallServerWasRunning 0
    StrCpy $UninstallServerStopResult 0
    StrCpy $UninstallRemoveUserData 0
    StrCpy $UninstallCleanupWarning 0
    StrCpy $UninstallUserDataDir "$PROFILE\zhiyi"
    StrCpy $InstallBaseDir "$INSTDIR"
    ClearErrors
    ReadRegStr $InstallBaseDir HKLM "${UNINSTALL_KEY}" "InstallBaseLocation"
    ${If} ${Errors}
    ${OrIf} $InstallBaseDir == ""
        StrCpy $InstallBaseDir "$INSTDIR"
    ${EndIf}
FunctionEnd

Function ToggleInstallLockDetails
    Pop $0
    StrCmp $InstallLockDetailsVisible "1" hide_install_lock_details

    ShowWindow $InstallLockDetailsText ${SW_SHOW}
    ${NSD_SetText} $InstallLockDetailsButton "收起占用详情"
    StrCpy $InstallLockDetailsVisible 1
    Return

    hide_install_lock_details:
        ShowWindow $InstallLockDetailsText ${SW_HIDE}
        ${NSD_SetText} $InstallLockDetailsButton "查看占用详情"
        StrCpy $InstallLockDetailsVisible 0
FunctionEnd

Function FinishPageShow
    StrCpy $InstallLockDetailsVisible 0
    StrCmp $InstallLockNotice "1" finish_page_occupied
    IfRebootFlag finish_page_reboot finish_page_done

    finish_page_occupied:
        IfRebootFlag finish_page_occupied_reboot finish_page_occupied_only
    finish_page_occupied_reboot:
        ${NSD_SetText} $mui.FinishPage.Text \
            "部分应用仍使用上一版本；部分更新将在下次 Windows 重启后生效。"
        Goto finish_page_create_details
    finish_page_occupied_only:
        ${NSD_SetText} $mui.FinishPage.Text \
            "部分正在运行的应用仍使用上一版本，重新打开后即可切换。"
    finish_page_create_details:
        ${NSD_CreateButton} 120u 108u 76u 16u "查看占用详情"
        Pop $InstallLockDetailsButton
        ${NSD_OnClick} $InstallLockDetailsButton ToggleInstallLockDetails
        ${NSD_CreateMLText} 120u 130u 195u 42u "$LockReportText"
        Pop $InstallLockDetailsText
        ${NSD_Edit_SetReadOnly} $InstallLockDetailsText 1
        ShowWindow $InstallLockDetailsText ${SW_HIDE}
        Goto finish_page_done

    finish_page_reboot:
        ${NSD_SetText} $mui.FinishPage.Text \
            "部分更新将在下次 Windows 重启后生效。"
    finish_page_done:
FunctionEnd

Function un.ConfirmPage
    !insertmacro MUI_HEADER_TEXT "卸载知意输入法" "移除程序，并选择是否同时删除个人数据。"
    nsDialogs::Create 1018
    Pop $0
    ${If} $0 == error
        Abort
    ${EndIf}

    ${NSD_CreateLabel} 20u 16u 100% 28u \
        "知意输入法将从系统中移除。正在使用的程序文件会自动在 Windows 重启后删除。"
    Pop $0
    ${NSD_CreateLabel} 20u 48u 100% 24u \
        "用户配置和词库默认保留，之后重新安装仍可继续使用。"
    Pop $0
    ${NSD_CreateCheckbox} 20u 82u 100% 20u "删除用户配置和词库数据"
    Pop $UninstallRemoveUserDataCheckbox
    ${NSD_SetState} $UninstallRemoveUserDataCheckbox $UninstallRemoveUserData
    ${NSD_CreateLabel} 38u 106u 100% 18u "勾选后个人数据将永久删除，无法撤销。"
    Pop $UninstallRemoveUserDataWarning
    ${NSD_OnClick} $UninstallRemoveUserDataCheckbox un.ToggleRemoveUserDataWarning
    StrCmp $UninstallRemoveUserData "${BST_CHECKED}" un_remove_user_data_warning_ready
        ShowWindow $UninstallRemoveUserDataWarning ${SW_HIDE}
    un_remove_user_data_warning_ready:
    GetDlgItem $0 $HWNDPARENT 1
    SendMessage $0 ${WM_SETTEXT} 0 "STR:卸载"
    nsDialogs::Show
FunctionEnd

Function un.ConfirmPageLeave
    ${NSD_GetState} $UninstallRemoveUserDataCheckbox $UninstallRemoveUserData
FunctionEnd

Function un.ToggleRemoveUserDataWarning
    Pop $0
    ${NSD_GetState} $UninstallRemoveUserDataCheckbox $1
    StrCmp $1 "${BST_CHECKED}" un_show_remove_user_data_warning
        ShowWindow $UninstallRemoveUserDataWarning ${SW_HIDE}
        Return
    un_show_remove_user_data_warning:
    ShowWindow $UninstallRemoveUserDataWarning ${SW_SHOW}
FunctionEnd

Function un.FinishPageShow
    StrCmp $UninstallCleanupWarning "1" un_finish_page_warning
    IfRebootFlag un_finish_page_deferred un_finish_page_done
    un_finish_page_deferred:
        ${NSD_SetText} $mui.FinishPage.Text \
            "知意输入法已卸载。少量正在使用的程序文件将在下次重新启动 Windows 后自动删除。"
        Goto un_finish_page_done
    un_finish_page_warning:
        ${NSD_SetText} $mui.FinishPage.Text \
            "知意输入法已从系统中移除，但部分程序文件未能自动清理。重新运行安装程序时会再次处理。"
    un_finish_page_done:
FunctionEnd

Function CheckInstallDirectory
    StrCpy $ExistingInstall 0
    IfFileExists "$INSTDIR\${INSTALL_MARKER}" install_directory_owned
    StrCmp $RegisteredInstallDir "$INSTDIR" 0 install_directory_scan_start
    IfFileExists "$INSTDIR\zhiyi-server.exe" 0 install_directory_scan_start
    IfFileExists "$INSTDIR\zhiyi_tsf_x64.dll" 0 install_directory_scan_start
    IfFileExists "$INSTDIR\uninstall.exe" install_directory_owned

    install_directory_scan_start:
    IfFileExists "$INSTDIR\zhiyi-resources.dll" 0 install_directory_scan_contents
    IfFileExists "$INSTDIR\zhiyi_tsf_x64.dll" install_directory_owned
    IfFileExists "$INSTDIR\zhiyi_tsf_x86.dll" install_directory_owned

    install_directory_scan_contents:
    FindFirst $0 $1 "$INSTDIR\*"
    IfErrors install_directory_ready
    install_directory_scan:
        StrCmp $1 "." install_directory_next
        StrCmp $1 ".." install_directory_next
        StrCmp $1 "${RUNTIME_MARKER}" install_directory_next
        FindClose $0
        StrCpy $FailureMessage \
            "所选目录不为空，并且不是知意输入法安装目录。请选择一个空目录。"
        Push 0
        Return
    install_directory_next:
        ClearErrors
        FindNext $0 $1
        IfErrors install_directory_empty
        Goto install_directory_scan
    install_directory_empty:
        FindClose $0
        Goto install_directory_ready

    install_directory_owned:
        StrCpy $ExistingInstall 1
    install_directory_ready:
    Push 1
FunctionEnd

Function ValidateInstallDirectory
    ${If} $RegisteredInstallDir != ""
        StrCpy $MultiVersionInstall 1
        StrCpy $PreviousInstallDir $RegisteredInstallDir
        StrCpy $InstallTargetPrepared 1
        Push 1
        Return
    ${EndIf}
    StrCpy $InstallBaseDir "$INSTDIR"
    Call CheckInstallDirectory
    Pop $0
    StrCmp $0 "1" install_directory_valid
        MessageBox MB_ICONSTOP "$FailureMessage"
        Abort
    install_directory_valid:
    StrCpy $InstallTargetDir "$InstallBaseDir\${VERSION}"
    StrCpy $INSTDIR $InstallTargetDir
    StrCpy $InstallTargetPrepared 1
    Return
FunctionEnd

Function SetTransactionPaths
    StrCpy $StageDir "$InstallBaseDir\update"
    StrCpy $LockReportPath "$PLUGINSDIR\zhiyi-locks.txt"
FunctionEnd

Function CheckFreshInstallBase
    StrCmp $LegacyUninstallPerformed "1" fresh_install_base_ready
    ${If} $MultiVersionInstall == 1
        Push 1
        Return
    ${EndIf}
    IfFileExists "$InstallBaseDir\maintenance\install-state.json" fresh_install_base_ready
    FindFirst $0 $1 "$InstallBaseDir\*"
    IfErrors fresh_install_base_ready
    fresh_install_base_scan:
        StrCmp $1 "." fresh_install_base_next
        StrCmp $1 ".." fresh_install_base_next
        StrCmp $1 "maintenance" fresh_install_base_next
        StrCmp $1 "${RUNTIME_MARKER}" fresh_install_base_next
        StrCmp $1 "${RUNTIME_TEMP}" fresh_install_base_next
        StrCmp $1 "${SYSTEM_IME_UPDATE_MARKER}" fresh_install_base_next
        StrCmp $1 "${SYSTEM_IME_REMOVE_MARKER}" fresh_install_base_next
        StrCmp $1 "${LEGACY_SYSTEM_IME_X64_PENDING}" fresh_install_base_next
        StrCmp $1 "${LEGACY_SYSTEM_IME_X86_PENDING}" fresh_install_base_next
        StrCmp $1 "${LEGACY_INSTALL_STATE_MARKER}" fresh_install_base_next
        StrCmp $1 "${LEGACY_INSTALL_STATE_TEMP}" fresh_install_base_next
        IfFileExists "$InstallBaseDir\$1\install-manifest.json" fresh_install_base_next
        FindClose $0
        StrCpy $FailureMessage \
            "所选产品目录包含不属于知意输入法的文件。请选择其他目录。"
        Push 0
        Return
    fresh_install_base_next:
        ClearErrors
        FindNext $0 $1
        IfErrors fresh_install_base_empty
        Goto fresh_install_base_scan
    fresh_install_base_empty:
        FindClose $0
    fresh_install_base_ready:
    Push 1
FunctionEnd

Function SecureInstallBase
    nsExec::Exec \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" secure-install-root "$InstallBaseDir"'
    Pop $0
    StrCmp $0 "0" 0 secure_install_base_failed
    System::Call 'kernel32::CreateFileW(\
        w "$InstallBaseDir", i 0x80, i 0x3, p 0, i 3, i 0x02200000, p 0) p .r0'
    StrCpy $InstallBaseHandle $0
    IntCmp $InstallBaseHandle -1 secure_install_base_failed
    nsExec::Exec \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" validate-install-directory "$StageDir"'
    Pop $0
    StrCmp $0 "0" 0 secure_install_base_failed
    nsExec::Exec \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" validate-install-directory "$INSTDIR"'
    Pop $0
    StrCmp $0 "0" secure_install_base_done
    secure_install_base_failed:
    StrCpy $FailureMessage "无法安全地准备知意输入法产品目录。"
    Push 0
    Return
    secure_install_base_done:
    Push 1
FunctionEnd

Function PrepareInstallTarget
    ${If} $MultiVersionInstall == 1
        StrCpy $ActiveServerDir "$PreviousInstallDir"
        StrCpy $StateInstallDir "$PreviousInstallDir"
        StrCpy $INSTDIR $InstallTargetDir
    ${Else}
        ${If} $InstallTargetPrepared == 0
            StrCpy $InstallBaseDir "$INSTDIR"
            StrCpy $InstallTargetDir "$InstallBaseDir\${VERSION}"
            StrCpy $InstallTargetPrepared 1
        ${EndIf}
        StrCpy $ActiveServerDir "$INSTDIR"
        StrCpy $StateInstallDir "$INSTDIR"
        StrCpy $INSTDIR "$InstallTargetDir"
    ${EndIf}
FunctionEnd

Function PrepareInstallLifecycle
    StrCpy $LifecycleActiveArg "-"
    StrCmp $RegisteredInstallDir "" +2
        StrCpy $LifecycleActiveArg "$RegisteredInstallDir"
    Delete "$LifecycleResultPath"
    nsExec::ExecToStack \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" lifecycle-prepare "$InstallBaseDir" \
        "$LifecycleActiveArg" "${VERSION}" "$LifecycleResultPath"'
    Pop $0
    Pop $1
    StrCmp $0 "0" lifecycle_prepare_read
        StrCpy $FailureMessage "无法准备知意输入法版本生命周期状态。"
        Push 0
        Return
    lifecycle_prepare_read:
    ClearErrors
    ReadINIStr $InstallTargetDir "$LifecycleResultPath" "lifecycle" "target"
    ReadINIStr $LifecycleScheduled "$LifecycleResultPath" "lifecycle" "scheduled"
    IfErrors lifecycle_prepare_failed
    StrCmp $InstallTargetDir "" lifecycle_prepare_failed
    StrCmp $LifecycleScheduled "0" +2
        SetRebootFlag true
    StrCpy $INSTDIR "$InstallTargetDir"
    StrCpy $InstallTargetPrepared 1
    Push 1
    Return
    lifecycle_prepare_failed:
    StrCpy $FailureMessage "知意输入法生命周期状态缺少有效的安装目标。"
    Push 0
FunctionEnd

Function LoadPreparedInstallTarget
    Delete "$LifecycleResultPath"
    nsExec::ExecToStack \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" lifecycle-prepared-target \
        "$InstallBaseDir" "$LifecycleResultPath"'
    Pop $0
    Pop $1
    StrCmp $0 "0" lifecycle_prepared_target_read
        StrCpy $FailureMessage "无法读取知意输入法安装事务目录。"
        Push 0
        Return
    lifecycle_prepared_target_read:
    ClearErrors
    ReadINIStr $2 "$LifecycleResultPath" "lifecycle" "target"
    IfErrors lifecycle_prepared_target_failed
    StrCmp $2 "" lifecycle_prepared_target_done
    IfFileExists "$2\${TRANSACTION_MARKER}" 0 lifecycle_prepared_target_done
        StrCpy $InstallTargetDir "$2"
        StrCpy $INSTDIR "$2"
        Call SetTransactionPaths
    lifecycle_prepared_target_done:
    Push 1
    Return
    lifecycle_prepared_target_failed:
    StrCpy $FailureMessage "知意输入法安装事务目录状态无效。"
    Push 0
FunctionEnd

Function CommitInstallLifecycle
    StrCpy $LifecycleActiveArg "-"
    StrCmp $PreviousInstallDir "" +2
        StrCpy $LifecycleActiveArg "$PreviousInstallDir"
    nsExec::Exec \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" lifecycle-commit "$InstallBaseDir" \
        "$INSTDIR" "$LifecycleActiveArg"'
    Pop $0
    StrCmp $0 "0" lifecycle_commit_done
        StrCpy $FailureMessage "无法提交知意输入法版本生命周期状态。"
        Push 0
        Return
    lifecycle_commit_done:
    Push 1
FunctionEnd

Function CollectInstallGarbage
    Delete "$LifecycleResultPath"
    nsExec::ExecToStack \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" lifecycle-gc \
        "$InstallBaseDir" "$LifecycleResultPath"'
    Pop $0
    Pop $1
    StrCmp $0 "0" lifecycle_gc_read
        DetailPrint "无法清理退役版本，后续安装将再次处理。"
        Return
    lifecycle_gc_read:
    ClearErrors
    ReadINIStr $LifecycleScheduled "$LifecycleResultPath" "lifecycle" "scheduled"
    IfErrors lifecycle_gc_done
    StrCmp $LifecycleScheduled "0" lifecycle_gc_done
        SetRebootFlag true
    lifecycle_gc_done:
FunctionEnd

Function un.CommitInstallLifecycle
    StrCpy $LifecycleRemaining -1
    StrCpy $LifecycleUnknown 0
    Delete "$LifecycleResultPath"
    nsExec::ExecToStack \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" lifecycle-uninstall "$InstallBaseDir" \
        "$INSTDIR" "$LifecycleResultPath"'
    Pop $0
    Pop $1
    StrCmp $0 "0" un_lifecycle_commit_read
        DetailPrint "无法完成程序文件清理；后续安装将再次处理残留文件。"
        Push 0
        Return
    un_lifecycle_commit_read:
    ClearErrors
    ReadINIStr $LifecycleScheduled "$LifecycleResultPath" "lifecycle" "scheduled"
    ReadINIStr $LifecycleRemaining "$LifecycleResultPath" "lifecycle" "remaining"
    ReadINIStr $LifecycleUnknown "$LifecycleResultPath" "lifecycle" "unknown"
    IfErrors un_lifecycle_commit_failed
    StrCmp $LifecycleRemaining "0" +2
        StrCpy $UninstallCleanupWarning 1
    StrCmp $LifecycleScheduled "0" un_lifecycle_commit_done
        SetRebootFlag true
    un_lifecycle_commit_done:
    Push 1
    Return
    un_lifecycle_commit_failed:
    DetailPrint "无法读取程序文件清理结果；后续安装将再次处理残留文件。"
    Push 0
FunctionEnd

Function un.ValidateInstallLifecycle
    nsExec::Exec \
        '"$PLUGINSDIR\zhiyi-installer-helper.exe" lifecycle-validate-uninstall \
        "$InstallBaseDir" "$INSTDIR"'
    Pop $0
    StrCmp $0 "0" un_lifecycle_validate_done
        StrCpy $FailureMessage \
            "无法验证知意输入法程序文件清单。卸载未进行，请重新安装后再卸载。"
        Push 0
        Return
    un_lifecycle_validate_done:
    Push 1
FunctionEnd
