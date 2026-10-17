#Requires -Version 5.1
<#
.SYNOPSIS
    Run the deterministic fault-injection scenarios (seeded, reproducible).
    The fault tests are tagged with the "fault_" prefix in asterion-tests.
#>
[CmdletBinding()]
param([string]$Filter = "fault_")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Tests = Join-Path $Root "build\bin\Release\asterion-tests.exe"

if (-not (Test-Path $Tests)) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration Release
}

Write-Host "Running fault scenarios ($Filter)..."
& $Tests $Filter
if ($LASTEXITCODE -ne 0) { throw "fault scenario failed (seed/replay info above)" }
Write-Host "Fault scenarios passed."
