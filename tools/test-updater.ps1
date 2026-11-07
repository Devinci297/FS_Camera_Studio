param([switch]$LiveDownload)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build\updater-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $compiler /nologo /target:exe /r:System.Net.Http.dll /r:System.Web.Extensions.dll "/out:$out\UpdaterTests.exe" "$root\src\Updater\UpdaterCore.cs" "$root\src\Updater\UpdaterTests.cs"
if ($LASTEXITCODE -ne 0) { throw 'Updater test compilation failed.' }
$fixture = Join-Path $out ([guid]::NewGuid().ToString())
$testArgs = @($fixture)
if ($LiveDownload) { $testArgs += '--live-download' }
& "$out\UpdaterTests.exe" @testArgs
if ($LASTEXITCODE -ne 0) { throw 'Updater tests failed.' }
