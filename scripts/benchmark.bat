@echo off
rem Benchmark script for ZhiyiIME query performance
rem Usage: benchmark.bat [repeat_count]

setlocal

set REPEAT=%1
if "%REPEAT%"=="" set REPEAT=100

set SCRIPT_DIR=%~dp0
set DATA_DIR=%SCRIPT_DIR%..\data
set CASES_FILE=%SCRIPT_DIR%..\tools\query_bench\cases.txt
set QUERY_BENCH=%SCRIPT_DIR%..\build\tools\query_bench\Release\query_bench.exe
set JSON_OUTPUT=%SCRIPT_DIR%..\benchmark-result.jsonl

if not exist "%QUERY_BENCH%" (
    echo Error: query_bench.exe not found at %QUERY_BENCH%
    echo Please build the project first: build.bat
    exit /b 1
)

echo Running benchmark with %REPEAT% iterations...
echo Data dir: %DATA_DIR%
echo Cases file: %CASES_FILE%
echo.

"%QUERY_BENCH%" --data "%DATA_DIR%" --file "%CASES_FILE%" --repeat %REPEAT% --json "%JSON_OUTPUT%"

echo.
echo Benchmark complete.
echo Results saved to: %JSON_OUTPUT%

endlocal
