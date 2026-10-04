# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
#
# Sync regression runner — build, test, benchmark, threshold check.
#
# Exit codes:
#   0 = all passed
#   1 = build or unit test failed
#   2 = benchmark run failed
#   3 = threshold check failed
#   4 = release artifacts missing

param(
    [string]$BuildDir = "build",
    [string]$DataDir = "data",
    [int]$Repeat = 1000,
    [string]$Threshold = "tools\query_bench\thresholds.release.json"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ScriptDir
$ReportsDir = Join-Path $Root "reports"
$Config = "Release"

Write-Host "=== ZhiyiIME Sync Regression ==="
Write-Host "  Build:     $BuildDir"
Write-Host "  Data:      $DataDir"
Write-Host "  Repeat:    $Repeat"
Write-Host "  Threshold: $Threshold"
Write-Host ""

# ---- Step 1: Release build ----
Write-Host "[1/5] Building Release..."
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64 -DCXXIME_PRODUCTION_BUILD=ON 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: CMake configuration failed."
    exit 1
}

cmake --build $BuildDir --config $Config 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed."
    exit 1
}
Write-Host "  Build OK."
Write-Host ""

# ---- Step 2: Verify data files ----
Write-Host "[2/5] Verifying data files..."
$VerifyScript = Join-Path $ScriptDir "verify_dictionary_bundle.py"
$DataDirFull = Join-Path $Root $DataDir
if (-not (Test-Path $DataDirFull)) { $DataDirFull = $DataDir }

python $VerifyScript --data-dir $DataDirFull
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Data file integrity check failed."
    exit 4
}
Write-Host "  Data files OK."
Write-Host ""

# ---- Step 3: Unit tests ----
Write-Host "[3/5] Running unit tests..."
ctest --test-dir $BuildDir --config $Config --output-on-failure 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Unit tests failed."
    exit 1
}
Write-Host "  Tests OK."
Write-Host ""

# ---- Step 4: Run query_bench ----
Write-Host "[4/5] Running query_bench (repeat=$Repeat)..."
if (-not (Test-Path $ReportsDir)) {
    New-Item -ItemType Directory -Path $ReportsDir | Out-Null
}

$QueryBench = Join-Path $BuildDir "tools\query_bench\$Config\query_bench.exe"
$Jsonl = Join-Path $ReportsDir "query-bench-trace.jsonl"
$CasesFile = Join-Path $Root "tools\query_bench\cases.txt"

if (-not (Test-Path $QueryBench)) {
    Write-Host "ERROR: query_bench.exe not found at $QueryBench"
    exit 2
}

& $QueryBench --data $DataDir --file $CasesFile --repeat $Repeat --json $Jsonl --require-topn 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: query_bench failed with exit code $LASTEXITCODE."
    exit 2
}
Write-Host "  Benchmark OK."
Write-Host ""

# ---- Step 5: Threshold check ----
Write-Host "[5/5] Checking thresholds..."
$CheckScript = Join-Path $ScriptDir "check_query_bench.py"
$ThresholdPath = Join-Path $Root $Threshold

python $CheckScript --input $Jsonl --threshold $ThresholdPath --output-dir $ReportsDir
$checkExit = $LASTEXITCODE
if ($checkExit -ge 3) {
    Write-Host "ERROR: Threshold check failed."
    exit 3
}
if ($checkExit -ge 1) {
    Write-Host "ERROR: check_query_bench.py failed."
    exit 2
}
Write-Host "  Thresholds OK."
Write-Host ""

Write-Host "=== All regression checks PASSED ==="
exit 0
