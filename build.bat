@echo off
setlocal

set BUILD_DIR=%~dp0build
set CONFIG=Release

if "%1"=="debug" set CONFIG=Debug
if "%1"=="Debug" set CONFIG=Debug
if "%1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Build directory cleaned.
    exit /b 0
)

echo === Zhiyi IME Build (%CONFIG%) ===

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

echo [1/2] Configuring CMake...
cmake .. -DCXXIME_PRODUCTION_BUILD=OFF -DCXXIME_BUILD_TOOLS=ON -DCXXIME_BUILD_TESTS=ON
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo [2/2] Building...
cmake --build . --config %CONFIG%
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo === Build succeeded (%CONFIG%) ===
echo Output: %BUILD_DIR%\server\%CONFIG%\zhiyi-server.exe
echo         %BUILD_DIR%\tsf\%CONFIG%\zhiyi_tsf_x64.dll
echo         %BUILD_DIR%\resource\%CONFIG%\zhiyi-resources.dll
echo         %BUILD_DIR%\test\%CONFIG%\cxxime-test.exe

endlocal
