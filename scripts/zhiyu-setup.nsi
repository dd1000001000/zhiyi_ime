; 知语输入法 / Zhiyu IME installer (NSIS 3.x, Unicode).
;
;   makensis /DVERSION=1.6.0 /DSRCDIR=..\dist\zhiyu-v1.6.0 scripts\zhiyu-setup.nsi
;
; Mirrors the upstream model (its zhiyu-setup.nsi is not published):
;   * the installer is silent-capable: `/S` and `/D=<root>` (NSIS built-ins). scoop installs it
;     as  Start-Process setup.exe '/S', "/D=$persist_dir\zhiyuime"  — see scripts/zhiyu-scoop.json.
;   * <root>  holds  <root>\<version>\  (the program files) and  <root>\uninstall.exe.
;   * the 64-bit and 32-bit TSF DLLs are registered with regsvr32 (they export DllRegisterServer,
;     tsf/src/dllmain.cpp + register.cpp); the background server is started.
;   * the uninstall entry carries an InstallLocation containing "zhiyuime" (scoop looks it up),
;     and the uninstaller is named uninstall.exe (matches scoop's unins*.exe).
;
; The helper's transactional lifecycle (lifecycle-prepare / -commit / -gc, installer/src/main.cc)
; is the robust in-place upgrade path; this template does the plain version-directory layout.
;
; SRCDIR comes from:  python scripts/package_release.py --version 1.6.0 ...

Unicode true
!include "MUI2.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "1.6.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "..\dist\zhiyu-v${VERSION}"
!endif

!define APP_NAME "知语输入法"
!define APP_NAME_EN "Zhiyu IME"
!define COMPANY "Zhiyu IME"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyuIME"

Name "${APP_NAME} ${VERSION}"
OutFile "zhiyu-v${VERSION}-setup.exe"
; /D from the command line overrides this (scoop passes the persist directory).
InstallDir "$LOCALAPPDATA\ZhiyuIME"
InstallDirRegKey HKCU "Software\ZhiyuIME" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Var AppDir  ; <root>\<version>

!macro RegisterTsf
  ExecWait '"$SYSDIR\regsvr32.exe" /s "$AppDir\zhiyu_tsf_x64.dll"'
  ${If} ${RunningX64}
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s "$AppDir\zhiyu_tsf_x86.dll"'
  ${EndIf}
!macroend

!macro UnregisterTsf
  ExecWait '"$SYSDIR\regsvr32.exe" /s /u "$AppDir\zhiyu_tsf_x64.dll"'
  ${If} ${RunningX64}
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s /u "$AppDir\zhiyu_tsf_x86.dll"'
  ${EndIf}
!macroend

Section "Install"
  StrCpy $AppDir "$INSTDIR\${VERSION}"

  ; An older copy may be running: let its helper stop it (and release the TSF DLLs).
  IfFileExists "$INSTDIR\uninstall.exe" 0 +3
    ExecWait '"$INSTDIR\uninstall.exe" /S _?=$INSTDIR'

  CreateDirectory "$AppDir"
  SetOutPath "$AppDir"
  File /r "${SRCDIR}\*.*"

  ; A marker the settings program and the helper look for.
  FileOpen $0 "$AppDir\.zhiyu-install-complete" w
  FileClose $0

  !insertmacro RegisterTsf

  IfFileExists "$AppDir\zhiyu-server.exe" 0 +2
    Exec '"$AppDir\zhiyu-server.exe"'

  WriteRegStr HKCU "Software\ZhiyuIME" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME} (${APP_NAME_EN})"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${COMPANY}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  StrCpy $AppDir "$INSTDIR\${VERSION}"
  IfFileExists "$AppDir\zhiyu-installer-helper.exe" 0 +2
    ExecWait '"$AppDir\zhiyu-installer-helper.exe" force-stop-server "$AppDir"'

  !insertmacro UnregisterTsf

  RMDir /r "$INSTDIR"

  DeleteRegKey HKCU "${UNINST_KEY}"
  ; The user's data (%USERPROFILE%\zhiyu) is kept on purpose; remove it by hand if you want.
SectionEnd

Function .onInit
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Windows 10 or later is required."
    Abort
  ${EndIf}
FunctionEnd
