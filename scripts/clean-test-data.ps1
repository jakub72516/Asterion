#Requires -Version 5.1
<#
.SYNOPSIS
    Remove all runtime data/logs/configs (the run/ directory).
#>
[CmdletBinding()]
param([string]$RunDir = "")

$Root = Split-Path -Parent $PSScriptRoot
if (-not $RunDir) { $RunDir = Join-Path $Root "run" }
if (Test-Path $RunDir) {
    Remove-Item -LiteralPath $RunDir -Recurse -Force
    Write-Host "Removed $RunDir"
} else {
    Write-Host "Nothing to clean."
}
