@echo off
rem Laya fork build: MSVC (latest toolset, x64) + Ninja, Release.
rem   build_laya.bat           configure + build into build\
rem   build_laya.bat test      also run the unit tests
rem Override the Visual Studio location with VS_PATH.
rem Keep this file ASCII-only with CRLF line endings.
setlocal
set "NoDefaultCurrentDirectoryInExePath="
if not defined VS_PATH set "VS_PATH=E:\Program Files\Microsoft Visual Studio\18\Community"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
set "PATH=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

set "ROOT=%~dp0"
cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCXXIME_PRODUCTION_BUILD=OFF -DCXXIME_BUILD_TOOLS=ON -DCXXIME_BUILD_TESTS=ON || exit /b 1
cmake --build "%ROOT%build" || exit /b 1
if /i "%~1"=="test" (
  ctest --test-dir "%ROOT%build" --output-on-failure || exit /b 1
)
echo build OK
