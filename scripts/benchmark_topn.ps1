param(
    [Parameter(Mandatory = $true)]
    [Alias("Input")]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$Builder,

    [Parameter(Mandatory = $true)]
    [string]$Benchmark,

    [Parameter(Mandatory = $true)]
    [string]$Dictionary,

    [string]$OutputDir = ".\topn-comparison",
    [string]$Report = ".\topn-comparison.txt",

    [ValidateRange(1, 20)]
    [int]$Runs = 3,

    [ValidateRange(1000, 10000000)]
    [int]$Queries = 200000,

    [ValidateRange(1, 64)]
    [int]$Threads = 4
)

$ErrorActionPreference = "Stop"

function Write-ReportOutput {
    process {
        $line = $_.ToString()
        Write-Host $line
        $line | Out-File -LiteralPath $script:reportPath -Append -Encoding utf8
    }
}

$inputPath = (Resolve-Path -LiteralPath $Source).Path
$builderPath = (Resolve-Path -LiteralPath $Builder).Path
$benchmarkPath = (Resolve-Path -LiteralPath $Benchmark).Path
$dictionaryPath = (Resolve-Path -LiteralPath $Dictionary).Path
$outputPath = [IO.Path]::GetFullPath($OutputDir)
$reportPath = [IO.Path]::GetFullPath($Report)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($reportPath)) |
    Out-Null

$index = Join-Path $outputPath "pinyin.topn.bin"

"Zhiyi IME Top-N index comparison" | Out-File -LiteralPath $reportPath -Encoding utf8
"timestamp=$([DateTime]::Now.ToString('o'))" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
"os=$([Environment]::OSVersion.VersionString)" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
"processor=$env:PROCESSOR_IDENTIFIER" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
"logical_processors=$env:NUMBER_OF_PROCESSORS" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
"benchmark_threads=$Threads" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
"input=$inputPath" | Out-File -LiteralPath $reportPath -Append -Encoding utf8
"input_sha256=$((Get-FileHash -Algorithm SHA256 -LiteralPath $inputPath).Hash.ToLowerInvariant())" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
"dictionary=$dictionaryPath" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
$dictionarySha256 =
    (Get-FileHash -Algorithm SHA256 -LiteralPath $dictionaryPath).Hash.ToLowerInvariant()
"dictionary_sha256=$dictionarySha256" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8

"`n[build]" | Out-File -LiteralPath $reportPath -Append -Encoding utf8
$elapsed = Measure-Command {
    & $builderPath --input $inputPath --dictionary $dictionaryPath --output $index 2>&1 |
        Write-ReportOutput
    if ($LASTEXITCODE -ne 0) {
        throw "topn_builder failed with exit code $LASTEXITCODE"
    }
}
"build_seconds=$($elapsed.TotalSeconds)" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8
"sha256=$((Get-FileHash -Algorithm SHA256 -LiteralPath $index).Hash.ToLowerInvariant())" |
    Out-File -LiteralPath $reportPath -Append -Encoding utf8

for ($run = 1; $run -le $Runs; $run++) {
    "`n[benchmark run $run/$Runs]" |
        Out-File -LiteralPath $reportPath -Append -Encoding utf8
    & $benchmarkPath --baseline $inputPath --dictionary $dictionaryPath --index $index `
        --queries $Queries --threads $Threads 2>&1 |
        Write-ReportOutput
    if ($LASTEXITCODE -ne 0) {
        throw "topn_benchmark failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Report: $reportPath"
