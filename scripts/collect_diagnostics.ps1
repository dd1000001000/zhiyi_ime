# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
#
# Collect ZhiyiIME release diagnostics without modifying system state.
#
# Examples:
#   powershell -NoProfile -ExecutionPolicy Bypass -File collect_diagnostics.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File collect_diagnostics.ps1 -IncludeLogs
#   powershell -NoProfile -ExecutionPolicy Bypass -File collect_diagnostics.ps1 -OutputDir D:\Temp\zhiyi-diag -NoZip

param(
    [string]$OutputDir = "",
    [string]$InstallDir = "",
    [switch]$IncludeLogs,
    [switch]$IncludeUserConfig,
    [switch]$IncludeUserDict,
    [switch]$IncludeCandidatePreferences,
    [switch]$IncludeDisabledSystemLexicon,
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"

function Get-ZhiyiImeInstallDir {
    param([string]$ExplicitInstallDir)

    if ($ExplicitInstallDir -and (Test-Path -LiteralPath $ExplicitInstallDir)) {
        return (Resolve-Path -LiteralPath $ExplicitInstallDir).Path
    }

    $registryPaths = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME"
    )
    foreach ($path in $registryPaths) {
        if (Test-Path -LiteralPath $path) {
            $props = Get-ItemProperty -LiteralPath $path
            if ($props.InstallLocation -and (Test-Path -LiteralPath $props.InstallLocation)) {
                return (Resolve-Path -LiteralPath $props.InstallLocation).Path
            }
        }
    }

    $candidates = @(
        (Join-Path $env:ProgramFiles "ZhiyiIME"),
        (Join-Path ${env:ProgramFiles(x86)} "ZhiyiIME")
    )
    foreach ($path in $candidates) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }

    return ""
}

function New-DiagnosticRoot {
    param([string]$ExplicitOutputDir)

    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    if ($ExplicitOutputDir) {
        $root = $ExplicitOutputDir
    } else {
        $desktop = [Environment]::GetFolderPath("Desktop")
        if (-not $desktop) {
            $desktop = $env:USERPROFILE
        }
        $root = Join-Path $desktop "zhiyi-diagnostics-$stamp"
    }

    New-Item -ItemType Directory -Force -Path $root | Out-Null
    return (Resolve-Path -LiteralPath $root).Path
}

function Get-FileInfoSafe {
    param([string]$Path)

    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{
            path = $Path
            exists = $false
        }
    }

    $item = Get-Item -LiteralPath $Path
    $hash = $null
    try {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    } catch {
        $hash = "unavailable"
    }

    return [ordered]@{
        path = $item.FullName
        exists = $true
        size = $item.Length
        last_write = $item.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
        sha256 = $hash
    }
}

function Get-DirectoryInfoSafe {
    param([string]$Path)

    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Container)) {
        return [ordered]@{
            path = $Path
            exists = $false
        }
    }

    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $item.FullName
        exists = $true
        last_write = $item.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
    }
}

function Save-CommandOutput {
    param(
        [string]$Path,
        [scriptblock]$Command
    )

    try {
        & $Command 2>&1 | Out-File -Encoding UTF8 -FilePath $Path
    } catch {
        "FAILED: $($_.Exception.Message)" | Out-File -Encoding UTF8 -FilePath $Path
    }
}

function Copy-IfExists {
    param(
        [string]$Source,
        [string]$DestinationDir
    )

    if ($Source -and (Test-Path -LiteralPath $Source -PathType Leaf)) {
        New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
        Copy-Item -LiteralPath $Source -Destination $DestinationDir -Force
    }
}

function Get-LogInventory {
    param([string]$LogsDir)

    if (-not $LogsDir -or -not (Test-Path -LiteralPath $LogsDir -PathType Container)) {
        return @()
    }

    return @(Get-ChildItem -LiteralPath $LogsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*.jsonl*" -or $_.Name -like "*.log*" } |
        Sort-Object LastWriteTime -Descending |
        ForEach-Object {
            [ordered]@{
                name = $_.Name
                size = $_.Length
                last_write = $_.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
            }
        })
}

function Write-RecentTraceSummary {
    param(
        [string]$LogsDir,
        [string]$OutputPath
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("Zhiyi IME recent trace summary")
    $lines.Add("Generated: $((Get-Date).ToString("yyyy-MM-dd HH:mm:ss zzz"))")
    $lines.Add("")

    if (-not $LogsDir -or -not (Test-Path -LiteralPath $LogsDir -PathType Container)) {
        $lines.Add("No logs directory found.")
        $lines | Out-File -Encoding UTF8 -FilePath $OutputPath
        return
    }

    $files = @(Get-ChildItem -LiteralPath $LogsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*.jsonl*" -or $_.Name -like "*.log*" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 12)

    if (-not $files -or $files.Count -eq 0) {
        $lines.Add("No trace log files found.")
        $lines | Out-File -Encoding UTF8 -FilePath $OutputPath
        return
    }

    foreach ($file in $files) {
        $lines.Add("File: $($file.Name)")
        $lines.Add("  Size: $($file.Length)")
        $lines.Add("  LastWrite: $($file.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))")

        $deadline = 0
        $cancelled = 0
        $ipcFailed = 0
        $slow = 0
        $truncated = 0
        $recent = @()
        try {
            $recent = @(Get-Content -LiteralPath $file.FullName -Tail 200 -ErrorAction Stop)
            foreach ($line in $recent) {
                if ($line -match '"deadline":true') { $deadline++ }
                if ($line -match '"cancelled":true') { $cancelled++ }
                if ($line -match '"result":"ipc_failed"') { $ipcFailed++ }
                if ($line -match '"slow":true') { $slow++ }
                if ($line -match '"truncated":true') { $truncated++ }
            }
        } catch {
            $lines.Add("  ReadError: $($_.Exception.Message)")
        }

        $lines.Add("  RecentLines: $($recent.Count)")
        $lines.Add("  Deadline: $deadline")
        $lines.Add("  Cancelled: $cancelled")
        $lines.Add("  IpcFailed: $ipcFailed")
        $lines.Add("  Slow: $slow")
        $lines.Add("  Truncated: $truncated")
        $lines.Add("")
    }

    $lines | Out-File -Encoding UTF8 -FilePath $OutputPath
}

function Get-OsInfoSafe {
    try {
        $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop | Select-Object -First 1
        return [ordered]@{
            caption = $os.Caption
            version = $os.Version
            build_number = $os.BuildNumber
            architecture = $os.OSArchitecture
        }
    } catch {
        return [ordered]@{
            caption = [Environment]::OSVersion.VersionString
            version = [Environment]::OSVersion.Version.ToString()
            build_number = ""
            architecture = $env:PROCESSOR_ARCHITECTURE
            error = $_.Exception.Message
        }
    }
}

$installDir = Get-ZhiyiImeInstallDir -ExplicitInstallDir $InstallDir
$userDir = Join-Path $env:USERPROFILE "zhiyi"
$dataDir = if ($installDir) { Join-Path $installDir "data" } else { "" }
$logsDir = Join-Path $userDir "logs"
$root = New-DiagnosticRoot -ExplicitOutputDir $OutputDir
$cxxImeClsid = "{4EAC2DF0-F298-453E-BD4A-B4B3D3579718}"
$systemImeX64 = Join-Path $env:WINDIR "System32\zhiyi.ime"
$sysnativeImeX64 = Join-Path $env:WINDIR "Sysnative\zhiyi.ime"
if (Test-Path -LiteralPath (Split-Path -Parent $sysnativeImeX64)) {
    $systemImeX64 = $sysnativeImeX64
}
$systemImeX86 = Join-Path $env:WINDIR "SysWOW64\zhiyi.ime"

$report = [ordered]@{
    product = "ZhiyiIME"
    package_version = "development"
    collected_at = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss zzz")
    machine = [ordered]@{
        computer_name = $env:COMPUTERNAME
        user_name = $env:USERNAME
        os = Get-OsInfoSafe
        powershell = $PSVersionTable.PSVersion.ToString()
        culture = [System.Globalization.CultureInfo]::CurrentCulture.Name
    }
    directories = [ordered]@{
        install = Get-DirectoryInfoSafe $installDir
        data = Get-DirectoryInfoSafe $dataDir
        user = Get-DirectoryInfoSafe $userDir
        logs = Get-DirectoryInfoSafe $logsDir
    }
    expected_registration = [ordered]@{
        clsid = $cxxImeClsid
        tsf_x64 = if ($installDir) { Join-Path $installDir "zhiyi_tsf_x64.dll" } else { "" }
        tsf_x86 = if ($installDir) { Join-Path $installDir "zhiyi_tsf_x86.dll" } else { "" }
        ime_x64 = if ($installDir) { Join-Path $installDir "zhiyi_ime_x64.ime" } else { "" }
        ime_x86 = if ($installDir) { Join-Path $installDir "zhiyi_ime_x86.ime" } else { "" }
        system_ime_x64 = $systemImeX64
        system_ime_x86 = $systemImeX86
        resources = if ($installDir) { Join-Path $installDir "zhiyi-resources.dll" } else { "" }
    }
    log_inventory = Get-LogInventory $logsDir
}

$programFiles = @(
    "zhiyi_tsf_x64.dll",
    "zhiyi_tsf_x86.dll",
    "zhiyi_ime_x64.ime",
    "zhiyi_ime_x86.ime",
    "zhiyi-resources.dll",
    "zhiyi-server.exe",
    "zhiyi-settings.exe",
    "collect_diagnostics.ps1",
    "uninstall.exe"
)
$dataFiles = @(
    "default.json",
    "settings_presets.json",
    "themes.json",
    "punctuation.json",
    "symbols.json",
    "dictionary_manifest.json",
    "pinyin.dict.bin",
    "pinyin.dict.idx",
    "pinyin.spellings.bin",
    "pinyin.topn.bin",
    "pinyin.reverse.idx",
    "wubi86.dict.bin",
    "wubi86.dict.idx",
    "wubi86.reverse.idx"
)
$userFiles = @(
    "default.json",
    "user_pinyin.tsv",
    "user_wubi.tsv",
    "learning_pinyin.tsv",
    "learning_wubi.tsv",
    "candidate_order_pinyin.tsv",
    "candidate_order_wubi.tsv",
    "disabled_pinyin.tsv",
    "disabled_wubi.tsv"
)

$report.program_files = @()
foreach ($file in $programFiles) {
    $path = $file
    if ($installDir) {
        $path = Join-Path $installDir $file
    }
    $report.program_files += Get-FileInfoSafe $path
}

$report.data_files = @()
foreach ($file in $dataFiles) {
    $path = $file
    if ($dataDir) {
        $path = Join-Path $dataDir $file
    }
    $report.data_files += Get-FileInfoSafe $path
}

$report.user_files = @()
foreach ($file in $userFiles) {
    $report.user_files += Get-FileInfoSafe (Join-Path $userDir $file)
}

$report.system_ime_files = @()
$report.system_ime_files += Get-FileInfoSafe -Path $systemImeX64
$report.system_ime_files += Get-FileInfoSafe -Path $systemImeX86

$report.processes = Get-Process zhiyi-server, zhiyi-settings -ErrorAction SilentlyContinue |
    Select-Object ProcessName, Id, Path, StartTime

$report | ConvertTo-Json -Depth 8 | Out-File -Encoding UTF8 -FilePath (Join-Path $root "diagnostics.json")
Write-RecentTraceSummary -LogsDir $logsDir -OutputPath (Join-Path $root "trace-summary.txt")

Save-CommandOutput -Path (Join-Path $root "registry-uninstall.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME" /s
}
Save-CommandOutput -Path (Join-Path $root "registry-uninstall-64.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME" /s /reg:64
}
Save-CommandOutput -Path (Join-Path $root "registry-uninstall-32.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME" /s /reg:32
}
Save-CommandOutput -Path (Join-Path $root "registry-clsid-64.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Classes\CLSID\$cxxImeClsid" /s /reg:64
}
Save-CommandOutput -Path (Join-Path $root "registry-clsid-32.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Classes\CLSID\$cxxImeClsid" /s /reg:32
}
Save-CommandOutput -Path (Join-Path $root "registry-tip.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Microsoft\CTF\TIP" /s /f "知意输入法"
}
Save-CommandOutput -Path (Join-Path $root "registry-tip-64.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Microsoft\CTF\TIP" /s /f "知意输入法" /reg:64
}
Save-CommandOutput -Path (Join-Path $root "registry-tip-32.txt") -Command {
    reg.exe query "HKLM\SOFTWARE\Microsoft\CTF\TIP" /s /f "知意输入法" /reg:32
}
Save-CommandOutput -Path (Join-Path $root "keyboard-preload.txt") -Command {
    reg.exe query "HKCU\Keyboard Layout\Preload"
}
# Windows input method hotkeys (they can collide with the configured switch keys).
Save-CommandOutput -Path (Join-Path $root "ime-hotkeys.txt") -Command {
    reg.exe query "HKCU\Control Panel\Input Method\Hot Keys" /s
}
Save-CommandOutput -Path (Join-Path $root "registry-keyboard-layouts-zhiyi.txt") -Command {
    reg.exe query "HKLM\SYSTEM\CurrentControlSet\Control\Keyboard Layouts" /s /f "zhiyi.ime"
}
Save-CommandOutput -Path (Join-Path $root "tasklist-zhiyi.txt") -Command {
    tasklist.exe /v /fi "imagename eq zhiyi-server.exe"
    tasklist.exe /v /fi "imagename eq zhiyi-settings.exe"
}
Save-CommandOutput -Path (Join-Path $root "tasklist-tsf-module.txt") -Command {
    tasklist.exe /m zhiyi_tsf_x64.dll
    tasklist.exe /m zhiyi_tsf_x86.dll
    tasklist.exe /m zhiyi.ime
}

$readme = @"
ZhiyiIME diagnostics package

Generated: $((Get-Date).ToString("yyyy-MM-dd HH:mm:ss zzz"))

Included by default:
- diagnostics.json with version, OS, install path, data path, user path, file metadata and hashes.
- diagnostics.json also records System32/SysWOW64 zhiyi.ime file existence and hashes.
- trace-summary.txt with log file metadata and recent error/slow-event counters.
- registry query output for uninstall, CLSID, TIP, legacy HKL and keyboard preload state.
- tasklist output for ZhiyiIME processes, TSF DLLs and zhiyi.ime module ownership.

Optional:
- Use -IncludeLogs to copy %USERPROFILE%\zhiyi\logs. Trace logs may contain raw input codes.
- Use -IncludeUserConfig to copy user configuration files.
- Use -IncludeUserDict to copy user dictionary files. User dictionaries contain personal phrases.
- Use -IncludeCandidatePreferences to copy candidate preference and manual order files.
  These files reflect input habits and explicit candidate choices.
- Use -IncludeDisabledSystemLexicon to copy disabled system word files.
  These files reflect words hidden by the user.
"@
$readme | Out-File -Encoding UTF8 -FilePath (Join-Path $root "README.txt")

if ($IncludeLogs -and (Test-Path -LiteralPath $logsDir -PathType Container)) {
    $dst = Join-Path $root "logs"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Get-ChildItem -LiteralPath $logsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*.jsonl*" -or $_.Name -like "*.log*" } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dst -Force }
}

if ($IncludeUserConfig) {
    $dst = Join-Path $root "user-config"
    Copy-IfExists (Join-Path $userDir "default.json") $dst
}

if ($IncludeUserDict) {
    $dst = Join-Path $root "user-dict"
    Copy-IfExists (Join-Path $userDir "user_pinyin.tsv") $dst
    Copy-IfExists (Join-Path $userDir "user_wubi.tsv") $dst
}

if ($IncludeCandidatePreferences) {
    $dst = Join-Path $root "candidate-preferences"
    Copy-IfExists (Join-Path $userDir "learning_pinyin.tsv") $dst
    Copy-IfExists (Join-Path $userDir "learning_wubi.tsv") $dst
    Copy-IfExists (Join-Path $userDir "candidate_order_pinyin.tsv") $dst
    Copy-IfExists (Join-Path $userDir "candidate_order_wubi.tsv") $dst
}

if ($IncludeDisabledSystemLexicon) {
    $dst = Join-Path $root "disabled-system-lexicon"
    Copy-IfExists (Join-Path $userDir "disabled_pinyin.tsv") $dst
    Copy-IfExists (Join-Path $userDir "disabled_wubi.tsv") $dst
}

if (-not $NoZip) {
    $zip = "$root.zip"
    if (Test-Path -LiteralPath $zip) {
        Remove-Item -LiteralPath $zip -Force
    }
    Compress-Archive -Path (Join-Path $root "*") -DestinationPath $zip -Force
    Write-Host "Diagnostics written: $zip"
} else {
    Write-Host "Diagnostics written: $root"
}
