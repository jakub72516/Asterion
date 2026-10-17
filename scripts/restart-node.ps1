#Requires -Version 5.1
<#
.SYNOPSIS
    Restart a node (1-based index) using its generated config.
#>
[CmdletBinding()]
param(
    [int]$Index = 1,
    [string]$RunDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "build\bin\Release"
$NodeExe = Join-Path $Bin "asterion-node.exe"

if (-not $RunDir) { $RunDir = Join-Path $Root "run" }
$cfgPath = Join-Path $RunDir "configs\node$Index.json"

if (-not (Test-Path $cfgPath)) { Write-Error "Config not found: $cfgPath"; exit 1 }

# Kill any existing process with this config (best effort).
$pidsFile = Join-Path $RunDir "pids.txt"
if (Test-Path $pidsFile) {
    $pids = Get-Content $pidsFile
    if ($Index -le $pids.Count) {
        Stop-Process -Id ([int]$pids[$Index - 1]) -Force -ErrorAction SilentlyContinue
        $pids[$Index - 1] = 0
        $pids | Set-Content $pidsFile
    }
}

Write-Host "Restarting node$Index from $cfgPath ..."
$p = Start-Process -FilePath $NodeExe -ArgumentList "`"$cfgPath`"" -WorkingDirectory $Root -PassThru

if (Test-Path $pidsFile) {
    $pids = Get-Content $pidsFile
    $pids[$Index - 1] = $p.Id
    $pids | Set-Content $pidsFile
}
Write-Host "node$Index restarted (PID $($p.Id))."
