param([switch]$SkipBuild, [string]$BuildRoot)
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$defaultBuildRoot = Join-Path $repositoryRoot 'build/native-ninja'
if (-not $BuildRoot) { $BuildRoot = $defaultBuildRoot }
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if (-not $SkipBuild -and $BuildRoot -ne $defaultBuildRoot) {
    throw 'For an existing custom build directory, build it first and pass -SkipBuild -BuildRoot <directory>.'
}
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
& $cmakePath --install $BuildRoot --config Release --prefix $portableRoot
if ($LASTEXITCODE -ne 0) { throw 'Native install failed.' }
if (-not (Test-Path -LiteralPath (Join-Path $portableRoot 'README-native.md'))) { throw 'Portable instructions are missing.' }
$executable = Join-Path $portableRoot 'SonyConnect.exe'
if ((Get-FileHash $executable).Hash -ne (Get-FileHash (Join-Path $BuildRoot 'Release/SonyConnect.exe')).Hash) {
    throw 'Installed executable does not match the selected Release build.'
}
$bytes = [System.IO.File]::ReadAllBytes($executable)
$releaseVersion = (Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
if ($releaseVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a three-part product version.' }
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
$uiReports = @()
foreach ($suite in @('slider','update','diagnostics')) {
    $testReport = Join-Path $testRoot ($suite + '.json')
    $nativeProcess = Start-Process -FilePath $executable -ArgumentList @(("--ui-$suite-self-test"),'--report',('"' + $testReport + '"')) -WindowStyle Hidden -PassThru
    try {
        if (-not $nativeProcess.WaitForExit(30000)) { $nativeProcess.Kill(); throw "Portable $suite validation timed out." }
        $result = Get-Content -LiteralPath $testReport -Raw | ConvertFrom-Json
        if ($nativeProcess.ExitCode -ne 0 -or -not $result.Passed -or $result.HardwareVerified -or
            $result.BluetoothApisCalled -or $result.ExternalNetworkCalled -or $result.NetworkCalled -or $result.LegacyWindowShown) {
            throw "Portable $suite validation failed."
        }
        $checks = if ($result.Checks) { @($result.Checks) } else { @($result.Scenarios) }
        if ($checks.Count -eq 0 -or @($checks | Where-Object { -not $_.Passed }).Count) { throw "Portable $suite checks are incomplete." }
        $uiReports += [ordered]@{ Suite=$suite; Report=$testReport; Checks=$checks.Count; Passed=$true }
    } finally { $nativeProcess.Dispose() }
}
$hashLines = @(Get-ChildItem $portableRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
    (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.FullName.Substring($portableRoot.Length + 1).Replace('\','/')
})
$hashLines | Set-Content (Join-Path $portableRoot 'SHA256SUMS.txt') -Encoding utf8
$archive = Join-Path $runRoot ("SonyConnect-v$releaseVersion-Windows-x64.zip")
Compress-Archive -Path (Join-Path $portableRoot '*') -DestinationPath $archive
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    foreach ($entry in $zip.Entries | Where-Object { $_.Name }) {
        $source = Join-Path $portableRoot $entry.FullName
        $stream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
        finally { $sha.Dispose(); $stream.Dispose() }
        if ($actual -ne (Get-FileHash $source -Algorithm SHA256).Hash) { throw "Archive content mismatch: $($entry.FullName)" }
    }
    if (@($zip.Entries | Where-Object { $_.Name }).Count -ne @(Get-ChildItem $portableRoot -Recurse -File).Count) { throw 'Archive file count mismatch.' }
} finally { $zip.Dispose() }
$archiveChecksum = $archive + '.sha256'
((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($archive)) |
    Set-Content -LiteralPath $archiveChecksum -Encoding ascii
$summary = [ordered]@{
    Passed = $true; RecordedAtUtc = [DateTimeOffset]::UtcNow.ToString('o');
    HardwareVerified = $false; BluetoothApisCalled = $false; ExternalNetworkCalled = $false;
    Executable = $executable; ExecutableBytes = $bytes.Length; ExecutableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant();
    Machine = 'AMD64'; ManagedClrRva = $clrRva; ManagedClrSize = $clrSize;
    Archive = $archive; ArchiveBytes = (Get-Item -LiteralPath $archive).Length; ArchiveSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant();
    SourceBuildRoot = $BuildRoot; PortableUiReports = $uiReports; UiScenarios = ($uiReports | ForEach-Object { $_.Checks } | Measure-Object -Sum).Sum;
    Version = $releaseVersion; ArchiveChecksum = $archiveChecksum;
    ZipIntegrityVerified = $true; LegacyWindowShown = $false; CleanWindowsVerified = $false; Published = $false
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runRoot 'summary.json') -Encoding UTF8
Write-Output ('Executable: ' + $executable)
Write-Output ('Archive: ' + $archive)
Write-Output ('Report: ' + (Join-Path $runRoot 'summary.json'))
Write-Output ('Checksum: ' + $archiveChecksum)
