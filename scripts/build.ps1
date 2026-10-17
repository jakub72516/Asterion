#Requires -Version 5.1
<#
.SYNOPSIS
    Configure and build Asterion.
.PARAMETER Configuration
    Debug or Release (default Release).
.PARAMETER Clean
    Remove the build directory first.
.PARAMETER Generator
    CMake generator (default "Visual Studio 17 2022").
.PARAMETER Arch
    Target architecture (default x64).
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")] [string]$Configuration = "Release",
    [switch]$Clean,
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root "build"

# Locate vcvars64.bat via vswhere (Build Tools or full VS).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
    $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($inst) {
        $cand = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $cand) { $vcvars = $cand }
    }
}
if (-not $vcvars) {
    throw "Could not locate vcvars64.bat. Install Visual Studio 2022 Build Tools with the C++ workload."
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Removing $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    $cmd = "`"$vcvars`" >nul 2>&1 && cmake -S `"$Root`" -B `"$BuildDir`" -G `"$Generator`" -A `"$Arch`" " +
           "-DASTERION_BUILD_TESTS=ON -DASTERION_BUILD_BENCH=ON"
    Write-Host "Configuring..."
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

Write-Host "Building ($Configuration)..."
$bcmd = "`"$vcvars`" >nul 2>&1 && cmake --build `"$BuildDir`" --config $Configuration -j 8"
cmd /c $bcmd
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
Write-Host "Build complete: $BuildDir\bin\$Configuration"
