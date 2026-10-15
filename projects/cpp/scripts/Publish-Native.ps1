param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-Native.ps1') -Configuration Release
}
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source }
else {
    $vsLocator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsRoot = & $vsLocator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakePath = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$runRoot = Join-Path $repositoryRoot ('artifacts/native/packages/' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$portableRoot = Join-Path $runRoot 'portable'
New-Item -ItemType Directory -Path $portableRoot -Force | Out-Null
& $cmakePath --install (Join-Path $repositoryRoot 'build/native-ninja') --config Release --prefix $portableRoot
if ($LASTEXITCODE -ne 0) { throw 'Native install failed.' }
if (-not (Test-Path -LiteralPath (Join-Path $portableRoot 'README-native.md'))) { throw 'Portable instructions are missing.' }
$executable = Join-Path $portableRoot 'SonyConnect.exe'
$bytes = [System.IO.File]::ReadAllBytes($executable)
if ($bytes.Length -lt 1024 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { throw 'Invalid PE executable.' }
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 264 -ge $bytes.Length -or [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x4550) { throw 'Invalid PE header.' }
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
$optionalHeader = $peOffset + 24
if ($machine -ne 0x8664 -or [BitConverter]::ToUInt16($bytes, $optionalHeader) -ne 0x20b) { throw 'Expected native x64 PE32+ executable.' }
$clrDirectory = $optionalHeader + 112 + 14 * 8
$clrRva = [BitConverter]::ToUInt32($bytes, $clrDirectory)
$clrSize = [BitConverter]::ToUInt32($bytes, $clrDirectory + 4)
if ($clrRva -ne 0 -or $clrSize -ne 0) { throw 'Unexpected managed CLR entry in native executable.' }
$testRoot = Join-Path $runRoot 'portable-ui-test'
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$testReport = Join-Path $testRoot 'report.json'
$nativeProcess = Start-Process -FilePath $executable -ArgumentList @('--ui-self-test','--report',('"' + $testReport + '"')) -WindowStyle Hidden -PassThru
try {
    if (-not $nativeProcess.WaitForExit(30000)) { $nativeProcess.Kill(); throw 'Portable UI validation timed out.' }
    $result = Get-Content -LiteralPath $testReport -Raw | ConvertFrom-Json
    if ($nativeProcess.ExitCode -ne 0 -or -not $result.Passed -or $result.HardwareVerified -or $result.BluetoothApisCalled -or $result.ExternalNetworkCalled) { throw 'Portable UI validation failed.' }
} finally { $nativeProcess.Dispose() }
$archive = Join-Path $runRoot 'SonyConnect-Cpp-Windows-x64.zip'
Compress-Archive -Path (Join-Path $portableRoot '*') -DestinationPath $archive
$summary = [ordered]@{
    Passed = $true; RecordedAtUtc = [DateTimeOffset]::UtcNow.ToString('o');
    HardwareVerified = $false; BluetoothApisCalled = $false; ExternalNetworkCalled = $false;
    Executable = $executable; ExecutableBytes = $bytes.Length; ExecutableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant();
    Machine = 'AMD64'; ManagedClrRva = $clrRva; ManagedClrSize = $clrSize;
    Archive = $archive; ArchiveBytes = (Get-Item -LiteralPath $archive).Length; ArchiveSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant();
    PortableUiReport = $testReport; UiScenarios = $result.Scenarios.Count
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runRoot 'summary.json') -Encoding UTF8
Write-Output ('Executable: ' + $executable)
Write-Output ('Archive: ' + $archive)
Write-Output ('Report: ' + (Join-Path $runRoot 'summary.json'))
