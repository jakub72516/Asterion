#Requires -Version 5.1
<#
.SYNOPSIS
    Start a local multi-node Asterion cluster.
.PARAMETER Nodes
    Number of nodes (default 3).
.PARAMETER BasePort
    First node's port (default 7100).
.PARAMETER RunDir
    Runtime directory for configs/data/logs (default <repo>\run).
#>
[CmdletBinding()]
param(
    [int]$Nodes = 3,
    [int]$BasePort = 7100,
    [string]$RunDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "build\bin\Release"
$NodeExe = Join-Path $Bin "asterion-node.exe"
$CliExe = Join-Path $Bin "asterion-cli.exe"

if (-not (Test-Path $NodeExe)) {
    throw "asterion-node.exe not found. Run scripts\build.ps1 first."
}
if (-not $RunDir) { $RunDir = Join-Path $Root "run" }

$ConfigDir = Join-Path $RunDir "configs"
$DataDir   = Join-Path $RunDir "data"
$LogDir    = Join-Path $RunDir "logs"
New-Item -ItemType Directory -Force -Path $ConfigDir, $DataDir, $LogDir | Out-Null

# Build peer list (host 127.0.0.1, ports BasePort..BasePort+Nodes-1).
$peersJson = @()
for ($i = 0; $i -lt $Nodes; $i++) {
    $peersJson += @{ node_id = ($i + 1); host = "127.0.0.1"; port = ($BasePort + $i) }
}

$pids = @()
for ($i = 0; $i -lt $Nodes; $i++) {
    $id = $i + 1
    $port = $BasePort + $i
    $cfg = [ordered]@{
        node_id = $id
        data_dir = Join-Path $DataDir "node$id"
        log_level = "info"
        log_file = Join-Path $LogDir "node$id.log"
        storage = [ordered]@{
            segment_size_bytes = 67108864
            flush_mode = "per_write"
            index_max_entries = 1000000
            compaction_enabled = $true
            compaction_interval_s = 60
        }
        network = [ordered]@{
            listen_host = "127.0.0.1"
            listen_port = $port
            max_connections = 128
            max_frame_size = 1048576
            io_threads = 2
            worker_threads = 2
        }
        replication = [ordered]@{
            replication_factor = 3
            partition_count = 4
            consistency_level = "quorum"
            peers = $peersJson
        }
        limits = [ordered]@{
            max_concurrent_scans = 32
            max_key_size = 1024
            max_payload_size = 1048576
        }
    }
    $cfgPath = Join-Path $ConfigDir "node$id.json"
    $json = $cfg | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($cfgPath, $json, (New-Object System.Text.UTF8Encoding($false)))

    Write-Host "Starting node$id on 127.0.0.1:$port ..."
    $p = Start-Process -FilePath $NodeExe -ArgumentList "`"$cfgPath`"" -WorkingDirectory $Root -PassThru
    $pids += $p.Id
}

$pids | Set-Content -Path (Join-Path $RunDir "pids.txt")
Write-Host "Started $Nodes node(s). PIDs: $($pids -join ', ')"
Write-Host "Use scripts\stop-cluster.ps1 to shut down gracefully."
