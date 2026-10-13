; 知语输入法 / Zhiyu IME installer (NSIS 3.x, Unicode).
;
;   makensis /DVERSION=1.6.0 /DSRCDIR=..\dist\zhiyu-v1.6.0 scripts\zhiyu-setup.nsi
;
; Upstream's own zhiyi-setup.nsi is not published, so this mirrors its *model*: the files are
; laid out under <root>\<version>, the 64-bit and 32-bit TSF DLLs are registered with regsvr32
; (they export DllRegisterServer, see tsf/src/dllmain.cpp and register.cpp), the background
; server is started, and the uninstall entry lives under ...\Uninstall\ZhiyiIME.
;
; It does not drive the installer helper's transactional lifecycle (lifecycle-prepare /
; lifecycle-commit / lifecycle-gc, see installer/src/main.cc). Those commands exist and are the
; robust way to upgrade in place; wire them in here if you want the exact upstream behaviour.
;
; Build the input of SRCDIR with:  python scripts/package_release.py --version 1.6.0 ...

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
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME"

Name "${APP_NAME} ${VERSION}"
OutFile "zhiyu-v${VERSION}-setup.exe"
InstallDir "$LOCALAPPDATA\ZhiyuIME\${VERSION}"
InstallDirRegKey HKCU "Software\ZhiyiIME" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

!macro RegisterTsf
  ; 64-bit TSF DLL with the 64-bit regsvr32, the 32-bit one with the SysWOW64 regsvr32.
  ExecWait '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\zhiyi_tsf_x64.dll"'
  ${If} ${RunningX64}
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s "$INSTDIR\zhiyi_tsf_x86.dll"'
  ${EndIf}
!macroend

!macro UnregisterTsf
  ExecWait '"$SYSDIR\regsvr32.exe" /s /u "$INSTDIR\zhiyi_tsf_x64.dll"'
  ${If} ${RunningX64}
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s /u "$INSTDIR\zhiyi_tsf_x86.dll"'
  ${EndIf}
!macroend

Section "Install"
  SetOutPath "$INSTDIR"

  ; An older copy may be running: let the helper stop it (and release the TSF DLLs).
  IfFileExists "$INSTDIR\zhiyi-installer-helper.exe" 0 +2
    ExecWait '"$INSTDIR\zhiyi-installer-helper.exe" force-stop-server "$INSTDIR"'

  File /r "${SRCDIR}\*.*"

  !insertmacro RegisterTsf

  IfFileExists "$INSTDIR\zhiyi-server.exe" 0 +2
    Exec '"$INSTDIR\zhiyi-server.exe"'

  WriteRegStr HKCU "Software\ZhiyiIME" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME} (${APP_NAME_EN})"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${COMPANY}"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; A marker the settings program and the helper look for.
  FileOpen $0 "$INSTDIR\.zhiyi-install-complete" w
  FileClose $0
SectionEnd

Section "Uninstall"
  ExecWait '"$INSTDIR\zhiyi-installer-helper.exe" force-stop-server "$INSTDIR"'
  !insertmacro UnregisterTsf
  Delete "$INSTDIR\zhiyi-server.exe"
  RMDir /r "$INSTDIR"

  DeleteRegKey HKCU "${UNINST_KEY}"
  ; The user's data (%USERPROFILE%\zhiyi) is kept on purpose; remove it by hand if you want.
SectionEnd

Function .onInit
  ; Refuse to run on a machine without the TSF runtime bits (very old Windows).
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Windows 10 or later is required."
    Abort
  ${EndIf}
FunctionEnd
