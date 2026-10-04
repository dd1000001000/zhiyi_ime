@echo off
setlocal enabledelayedexpansion
rem Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
rem
rem Sync regression runner — build, test, benchmark, threshold check.
rem
rem Exit codes:
rem   0 = all passed
rem   1 = build or unit test failed
rem   2 = benchmark run failed
rem   3 = threshold check failed
rem   4 = release artifacts missing

set SCRIPT_DIR=%~dp0
set ROOT=%SCRIPT_DIR%..
set BUILD_DIR=%ROOT%\build
set DATA_DIR=%ROOT%\data
set TOOLS_DIR=%ROOT%\tools\query_bench
set REPORTS_DIR=%ROOT%\reports
set CONFIG=Release
set REPEAT=1000
set THRESHOLD=%TOOLS_DIR%\thresholds.release.json

rem Parse optional arguments
if not "%1"=="" set BUILD_DIR=%1
if not "%2"=="" set DATA_DIR=%2
if not "%3"=="" set REPEAT=%3
if not "%4"=="" set THRESHOLD=%4

echo === ZhiyiIME Sync Regression ===
echo   Build:  %BUILD_DIR%
echo   Data:   %DATA_DIR%
echo   Repeat: %REPEAT%
echo   Threshold: %THRESHOLD%
echo.

:: ---- Step 1: Release build ----
echo [1/5] Building Release...
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DCXXIME_PRODUCTION_BUILD=ON >nul 2>&1
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)
cmake --build "%BUILD_DIR%" --config %CONFIG% 2>&1
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)
echo   Build OK.
echo.

:: ---- Step 2: Verify data files ----
echo [2/5] Verifying data files...
python "%SCRIPT_DIR%verify_dictionary_bundle.py" --data-dir "%DATA_DIR%"
if errorlevel 1 (
    echo ERROR: Data file integrity check failed.
    exit /b 4
)
echo   Data files OK.
echo.

:: ---- Step 3: Unit tests ----
echo [3/5] Running unit tests...
ctest --test-dir "%BUILD_DIR%" --config %CONFIG% --output-on-failure 2>&1
if errorlevel 1 (
    echo ERROR: Unit tests failed.
    exit /b 1
)
echo   Tests OK.
echo.

:: ---- Step 4: Run query_bench ----
echo [4/5] Running query_bench (repeat=%REPEAT%)...
if not exist "%REPORTS_DIR%" mkdir "%REPORTS_DIR%"
set QUERY_BENCH=%BUILD_DIR%\tools\query_bench\%CONFIG%\query_bench.exe
set JSONL=%REPORTS_DIR%\query-bench-trace.jsonl

if not exist "%QUERY_BENCH%" (
    echo ERROR: query_bench.exe not found at %QUERY_BENCH%
    exit /b 2
)

"%QUERY_BENCH%" --data "%DATA_DIR%" --file "%TOOLS_DIR%\cases.txt" --repeat %REPEAT% --json "%JSONL%" --require-topn 2>&1
if errorlevel 1 (
    echo ERROR: query_bench failed with exit code %errorlevel%.
    exit /b 2
)
echo   Benchmark OK.
echo.

:: ---- Step 5: Threshold check ----
echo [5/5] Checking thresholds...
python "%SCRIPT_DIR%check_query_bench.py" --input "%JSONL%" --threshold "%THRESHOLD%" --output-dir "%REPORTS_DIR%"
if errorlevel 3 (
    echo ERROR: Threshold check failed.
    exit /b 3
)
if errorlevel 1 (
    echo ERROR: check_query_bench.py failed.
    exit /b 2
)
echo   Thresholds OK.
echo.

echo === All regression checks PASSED ===
exit /b 0
