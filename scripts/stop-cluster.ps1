#Requires -Version 5.1
<#
.SYNOPSIS
    Gracefully stop the cluster (sends the shutdown command to every node).
#>
[CmdletBinding()]
param(
    [string]$RunDir = "",
    [int]$BasePort = 7100,
    [int]$Nodes = 3
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "build\bin\Release"
$CliExe = Join-Path $Bin "asterion-cli.exe"

if (-not $RunDir) { $RunDir = Join-Path $Root "run" }
$pidsFile = Join-Path $RunDir "pids.txt"

for ($i = 0; $i -lt $Nodes; $i++) {
    $port = $BasePort + $i
    Write-Host "Shutting down node on 127.0.0.1:$port ..."
    & $CliExe "127.0.0.1" $port "shutdown" 2>$null | Out-Null
}

Start-Sleep -Seconds 2

if (Test-Path $pidsFile) {
    $pids = Get-Content $pidsFile
    foreach ($pid in $pids) {
        $proc = Get-Process -Id $pid -ErrorAction SilentlyContinue
        if ($proc) { Write-Host "Waiting for PID $pid ..."; $proc.WaitForExit(10000) | Out-Null }
    }
    Remove-Item $pidsFile -ErrorAction SilentlyContinue
}
Write-Host "Cluster stopped."
