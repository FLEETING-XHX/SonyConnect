param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$vsLocator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vsLocator)) { throw 'Install Visual Studio Desktop development with C++ (including CMake and Windows SDK).' }
$vsRoot = & $vsLocator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'No installed MSVC C++ toolchain found.' }
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source }
else {
    $cmakePath = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmakePath)) { throw 'CMake is missing from the installed toolchain.' }
$buildRoot = Join-Path $repositoryRoot 'build/native-ninja'
$buildDriver = Join-Path $PSScriptRoot 'NativeBuild.cmd'
$testMode = if ($SkipTests) { 'skip' } else { 'test' }
# Launch the installed developer environment only for this build. Ninja avoids
# .NET MSBuild's duplicate Path/PATH failure in desktop-hosted environments.
$nativeBuildInfo = New-Object System.Diagnostics.ProcessStartInfo
$nativeBuildInfo.FileName = (Get-Command cmd.exe).Source
$nativeBuildInfo.UseShellExecute = $false
# A hidden console supplies a real UTF-8 console code page to localized MSVC.
# CREATE_NO_WINDOW would make chcp ineffective and lose Ninja header dependencies.
$nativeBuildInfo.CreateNoWindow = $false
$nativeBuildInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$nativeBuildInfo.RedirectStandardOutput = $true
$nativeBuildInfo.RedirectStandardError = $true
$nativeBuildInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$nativeBuildInfo.StandardErrorEncoding = [System.Text.Encoding]::UTF8
$nativeBuildInfo.Arguments = '/d /s /c ""' + $buildDriver + '" "' + $vsRoot + '" "' + $cmakePath + '" ' + $Configuration + ' ' + $testMode + '"'
$nativeBuildProcess = [System.Diagnostics.Process]::Start($nativeBuildInfo)
try {
    $nativeOutput = $nativeBuildProcess.StandardOutput.ReadToEndAsync()
    $nativeError = $nativeBuildProcess.StandardError.ReadToEndAsync()
    $nativeBuildProcess.WaitForExit()
    Write-Output $nativeOutput.Result
    if ($nativeError.Result) { Write-Output $nativeError.Result }
    if ($nativeBuildProcess.ExitCode -ne 0) { throw 'Native build or regression failed.' }
} finally { $nativeBuildProcess.Dispose() }
Write-Output ('Executable: ' + (Join-Path $buildRoot "$Configuration/SonyConnect.exe"))
