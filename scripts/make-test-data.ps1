#Requires -Version 5.1
<#
.SYNOPSIS
    Create deterministic test data in a running single-node instance.
    Appends N events to distinct keys for tenant 1.
#>
[CmdletBinding()]
param(
    [int]$Count = 1000,
    [int]$Port = 7100,
    [int]$Tenant = 1
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Cli = Join-Path $Root "build\bin\Release\asterion-cli.exe"
if (-not (Test-Path $Cli)) { throw "asterion-cli.exe not found. Run build.ps1 first." }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 1; $i -le $Count; $i++) {
    $payload = "value-$i"
    & $Cli "127.0.0.1" $Port "append" $Tenant "key-$i" 1 $payload | Out-Null
    if (($i % 1000) -eq 0) { Write-Host "appended $i / $Count" }
}
$sw.Stop()
Write-Host "Inserted $Count events in $($sw.Elapsed.TotalSeconds) s."
