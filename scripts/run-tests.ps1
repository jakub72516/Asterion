#Requires -Version 5.1
<#
.SYNOPSIS
    Build the project (if needed) and run the test suite.
.PARAMETER Filter
    Test-name substring filter, or --fast / --all / --stress.
#>
[CmdletBinding()]
param([string]$Filter = "--fast")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Tests = Join-Path $Root "build\bin\Release\asterion-tests.exe"

& (Join-Path $PSScriptRoot "build.ps1") -Configuration Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "Running tests ($Filter)..."
& $Tests $Filter
if ($LASTEXITCODE -ne 0) { throw "tests failed" }
Write-Host "All tests passed."
