param([Parameter(Mandatory=$true)][string]$BuildRoot)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$probePath = Join-Path $BuildRoot 'msvc-dependency-probe.cpp'
[System.IO.File]::WriteAllText($probePath, "#include <stddef.h>`r`n", [System.Text.Encoding]::ASCII)
$probeInfo = New-Object System.Diagnostics.ProcessStartInfo
$probeInfo.FileName = (Get-Command cl.exe).Source
$probeInfo.Arguments = '/nologo /TP /showIncludes /Zs "' + $probePath + '"'
$probeInfo.UseShellExecute = $false
$probeInfo.RedirectStandardOutput = $true
$probeInfo.RedirectStandardError = $true
$probeInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$probeInfo.StandardErrorEncoding = [System.Text.Encoding]::UTF8
$probeProcess = [System.Diagnostics.Process]::Start($probeInfo)
try {
    $probeOutput = $probeProcess.StandardOutput.ReadToEndAsync()
    $probeError = $probeProcess.StandardError.ReadToEndAsync()
    $probeProcess.WaitForExit()
    if ($probeProcess.ExitCode -ne 0) { throw ('MSVC dependency probe failed: ' + $probeError.Result) }
    $prefix = $null
    foreach ($line in ($probeOutput.Result -split "`r?`n")) {
        if ($line -match '^(.+?)(?=[A-Za-z]:[\\/]).*stddef\.h\s*$') { $prefix = $matches[1]; break }
    }
    if (-not $prefix) { throw 'Could not detect the localized MSVC include prefix.' }
    # Avoid a BOM: this is an exact byte prefix consumed by Ninja, not a UI label.
    [System.IO.File]::WriteAllText((Join-Path $BuildRoot 'msvc-dependency-prefix.txt'), $prefix, (New-Object System.Text.UTF8Encoding($false)))
} finally { $probeProcess.Dispose() }
