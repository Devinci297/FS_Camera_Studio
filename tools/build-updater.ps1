param([string]$OutputDirectory = 'build/updater')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if (-not $out.StartsWith($root.TrimEnd('\') + '\build\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Updater output must be under this checkout build folder.' }
New-Item -ItemType Directory -Force $out | Out-Null
$sourceHashes = [ordered]@{}
foreach ($name in @('src/Updater/UpdaterCore.cs','src/Updater/Updater.cs','tools/build-updater.ps1')) {
    $sourceHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $root $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$receiptPath = Join-Path $out 'verified-build.json'
$executable = Join-Path $out 'FSCSUpdater.exe'
if ((Test-Path -LiteralPath $receiptPath) -and (Test-Path -LiteralPath $executable)) {
    $prior = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    $same = $true
    foreach ($name in $sourceHashes.Keys) { if ($prior.sources.$name -ne $sourceHashes[$name]) { $same = $false } }
    if ($same -and $prior.sha256 -eq (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()) {
        Write-Host "Reusing verified updater: $executable"
        return
    }
}

$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
& $compiler /nologo /target:winexe /optimize+ /r:System.Net.Http.dll /r:System.Web.Extensions.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll "/out:$out\FSCSUpdater.exe" "$root\src\Updater\UpdaterCore.cs" "$root\src\Updater\Updater.cs"
if ($LASTEXITCODE -ne 0) { throw 'Updater compilation failed.' }
$receipt = [ordered]@{ sources = $sourceHashes; sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() }
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
Write-Host "Updater: $executable"
