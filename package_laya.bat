@echo off
rem Zhiyi IME installer: builds x64 + x86 with Visual Studio and packages with NSIS.
rem   package_laya.bat                 full installer into ..\output\
rem   package_laya.bat --skip-nsis     only prepare dist\ (NSIS not needed)
rem Extra arguments are passed to scripts\package.py. Override the Visual Studio location
rem with VS_PATH and the CMake generator with VS_GENERATOR.
rem Keep this file ASCII-only with CRLF line endings.
setlocal
set "NoDefaultCurrentDirectoryInExePath="
if not defined VS_PATH set "VS_PATH=E:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VS_GENERATOR set "VS_GENERATOR=Visual Studio 18 2026"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
set "PATH=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
if exist "C:\Program Files (x86)\NSIS\makensis.exe" set "PATH=C:\Program Files (x86)\NSIS;%PATH%"
if exist "C:\Program Files\NSIS\makensis.exe" set "PATH=C:\Program Files\NSIS;%PATH%"

python "%~dp0scripts\fetch_model.py" || exit /b 1
python "%~dp0scripts\package.py" --generator "%VS_GENERATOR%" --platform x64 %* || exit /b 1
