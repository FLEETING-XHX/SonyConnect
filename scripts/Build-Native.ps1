param([ValidateSet('Debug','Release')][string]$Configuration='Release',[switch]$SkipTests)
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot '../projects/cpp/scripts/Build-Native.ps1') -Configuration $Configuration -SkipTests:$SkipTests
