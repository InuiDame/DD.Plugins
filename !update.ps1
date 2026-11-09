#Requires -Version 5

param (
    [switch]$Extern
)

$ErrorActionPreference = 'Stop'

$DKUtil = Join-Path $PSScriptRoot 'extern/DKUtil'

if (Test-Path (Join-Path $DKUtil '.git')) {
    Write-Host "Updating $DKUtil" -ForegroundColor Green
    & git -C $DKUtil pull --ff-only
} elseif ($Extern) {
    Write-Host "Cloning DKUtil into $DKUtil" -ForegroundColor Green
    & git clone https://github.com/gottyduke/DKUtil.git $DKUtil
} elseif ($env:DKUtilPath -and (Test-Path (Join-Path $env:DKUtilPath 'include/DKUtil/Hook.hpp'))) {
    Write-Host "Using DKUtil from DKUtilPath: $env:DKUtilPath" -ForegroundColor Green
} else {
    Write-Host 'DKUtil not found: set DKUtilPath to a local DKUtil repository, or run "!update.ps1 -Extern" to clone it into extern/DKUtil' -ForegroundColor Red
    exit 1
}

foreach ($Project in Get-ChildItem $PSScriptRoot -Directory) {
    if (Test-Path (Join-Path $Project.FullName '.gitmodules')) {
        Write-Host "Updating submodules of $($Project.Name)" -ForegroundColor Green
        & git -C $Project.FullName submodule update --init --recursive
    }
}
