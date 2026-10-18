param([switch]$SkipBuild, [string]$BuildRoot)
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot '../projects/cpp/scripts/Publish-Native.ps1') -SkipBuild:$SkipBuild -BuildRoot $BuildRoot
