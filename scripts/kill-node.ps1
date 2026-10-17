#Requires -Version 5.1
<#
.SYNOPSIS
    Force-kill a selected node (by 1-based node index).
.PARAMETER Index
    1-based node index to kill (default 1).
#>
[CmdletBinding()]
param(
    [int]$Index = 1,
    [string]$RunDir = ""
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $RunDir) { $RunDir = Join-Path $Root "run" }
$pidsFile = Join-Path $RunDir "pids.txt"

if (-not (Test-Path $pidsFile)) { Write-Error "No pids.txt; is the cluster running?"; exit 1 }

$pids = Get-Content $pidsFile
if ($Index -lt 1 -or $Index -gt $pids.Count) {
    Write-Error "Node index $Index out of range (1..$($pids.Count))"
    exit 1
}

$pid = [int]$pids[$Index - 1]
Write-Host "Force-killing node $Index (PID $pid) ..."
Stop-Process -Id $pid -Force -ErrorAction SilentlyContinue
Write-Host "Done."
