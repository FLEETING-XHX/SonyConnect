param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot '../projects/cpp/scripts/Publish-Native.ps1') -SkipBuild:$SkipBuild
