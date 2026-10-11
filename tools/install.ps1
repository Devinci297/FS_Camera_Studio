<#
.SYNOPSIS
  Installs the built FS Camera Studio package into the MSFS 2024 Community folder (or removes it).

.DESCRIPTION
  Finds the Community folder from UserCfg.opt (Steam or Microsoft Store edition) and creates a directory junction to
  build\package\Packages\fs-camera-studio, so rebuilding updates the installed copy. Use -Copy for a plain copy and
  -Uninstall to remove it. Close the simulator first. Saved profiles live in the package's \work storage, which the
  simulator keeps outside the Community folder.
#>
param(
    [switch] $Copy,
    [switch] $Uninstall,
    [string] $Community
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$package = Join-Path $root 'build\package\Packages\fs-camera-studio'

if (-not $Community) {
    $candidates = @(
        (Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024\UserCfg.opt'),
        (Join-Path $env:LOCALAPPDATA 'Packages\Microsoft.Limitless_8wekyb3d8bbwe\LocalCache\UserCfg.opt')
    )
    foreach ($cfg in $candidates) {
        if (-not (Test-Path $cfg)) { continue }
        $line = Select-String -Path $cfg -Pattern '^\s*InstalledPackagesPath\s+"(.+)"' | Select-Object -First 1
        if ($line) { $Community = Join-Path $line.Matches[0].Groups[1].Value 'Community'; break }
    }
}
if (-not $Community -or -not (Test-Path $Community)) {
    throw 'Could not find the MSFS 2024 Community folder. Pass -Community "<path>".'
}
if (Get-Process -Name 'FlightSimulator2024' -ErrorAction SilentlyContinue) {
    throw 'Close Microsoft Flight Simulator 2024 before installing or removing add-ons.'
}

# Never remove the installed copy unless a complete new package is ready to replace it.
if (-not $Uninstall -and -not (Test-Path (Join-Path $package 'layout.json'))) {
    throw "No built package at $package. Run tools\build.ps1, then tools\package.ps1 -Compile."
}

$target = Join-Path $Community 'fs-camera-studio'
if (Test-Path $target) {
    $item = Get-Item $target -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { $item.Delete() }  # removes the junction only
    else { Remove-Item -Recurse -Force $target }
    Write-Host "Removed $target"
}
if ($Uninstall) { return }

if ($Copy) {
    Copy-Item -Recurse $package $target
} else {
    New-Item -ItemType Junction -Path $target -Target $package | Out-Null
}
Write-Host "Installed FS Camera Studio into $Community"
