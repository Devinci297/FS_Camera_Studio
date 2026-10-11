param([switch]$Install)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$baseline = Join-Path $projectRoot 'build/diagnostic/native-mouse-smooth/fs-camera-studio'
$candidate = Join-Path $projectRoot 'build/diagnostic/mouse-dispatch-once/fs-camera-studio'
$installed = Join-Path $projectRoot 'build/package/Packages/fs-camera-studio'
$review = Join-Path $projectRoot 'build/review/mouse-dispatch-once-20261011'
$prior = Get-Content (Join-Path $projectRoot 'build/review/mouse-smoothing-20261011/installed.json') -Raw | ConvertFrom-Json
function Get-Hash($path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
foreach ($entry in $prior.files) {
    if ((Get-Hash (Join-Path $baseline $entry.file)) -ne $entry.sha256) { throw "Baseline changed: $($entry.file)" }
}
New-Item -ItemType Directory -Force $candidate,$review | Out-Null
Copy-Item -Path (Join-Path $baseline '*') -Destination $candidate -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'build/wasm/mouse-dispatch-trial/FSCameraStudio.wasm') -Destination (Join-Path $candidate 'modules/FSCameraStudio.wasm') -Force
$layoutPath = Join-Path $candidate 'layout.json'
$layout = Get-Content $layoutPath -Raw | ConvertFrom-Json
$module = Get-Item (Join-Path $candidate 'modules/FSCameraStudio.wasm')
foreach ($entry in $layout.content) {
    if ($entry.path -eq 'modules/FSCameraStudio.wasm') { $entry.size=$module.Length; $entry.date=$module.LastWriteTimeUtc.ToFileTimeUtc() }
    if ((Get-Item (Join-Path $candidate $entry.path)).Length -ne $entry.size) { throw "Layout size mismatch: $($entry.path)" }
}
$layout | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $layoutPath
$files = @($prior.files | ForEach-Object { @{file=$_.file;sha256=(Get-Hash (Join-Path $candidate $_.file))} })
$changed = @($files | Where-Object { $new=$_; ($prior.files | Where-Object file -eq $new.file).sha256 -ne $new.sha256 } | ForEach-Object { $_.file.Replace('\','/') })
if ($changed.Count -ne 2 -or !($changed -contains 'layout.json') -or !($changed -contains 'modules/FSCameraStudio.wasm')) { throw 'Unexpected trial differences.' }
$liveLog = Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/WASM/MSFS2024/fs-camera-studio/work/fscs-diagnostics.log'
if (Test-Path $liveLog) { Copy-Item -LiteralPath $liveLog -Destination (Join-Path $review 'live-before.log') -Force }
$receipt = [ordered]@{status='staged';change='Register the existing WASM SimConnect callback once per connection';changed=$changed;files=$files;wrapperTest='216000 frames; reconnect; changed context; failure retry';nativeTests='95 passed, 0 failed, 4956 checks';liveAcceptance='pending'}
if ($Install) {
    $running = @(Get-Process | Where-Object { $_.ProcessName -like 'FlightSimulator*' })
    if ($running.Count) { throw 'MSFS is running; the installed package has not been changed.' }
    foreach ($entry in $prior.files) {
        if ((Get-Hash (Join-Path $installed $entry.file)) -ne $entry.sha256) { throw "Installed baseline differs: $($entry.file)" }
    }
    $backup = Join-Path $projectRoot ('build/rollback/before-dispatch-once-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '/fs-camera-studio')
    New-Item -ItemType Directory -Force $backup | Out-Null
    Copy-Item -Path (Join-Path $installed '*') -Destination $backup -Recurse -Force
    foreach ($entry in $prior.files) {
        if ((Get-Hash (Join-Path $backup $entry.file)) -ne $entry.sha256) { throw "Backup mismatch: $($entry.file)" }
    }
    if (@(Get-Process | Where-Object { $_.ProcessName -like 'FlightSimulator*' }).Count) { throw 'MSFS started during preparation; no installation made.' }
    foreach ($file in $changed) { Copy-Item -LiteralPath (Join-Path $candidate $file) -Destination (Join-Path $installed $file) -Force }
    $community = Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'
    foreach ($entry in $files) {
        if ((Get-Hash (Join-Path $installed $entry.file)) -ne $entry.sha256) { throw "Installed mismatch: $($entry.file)" }
        if ((Get-Hash (Join-Path $community $entry.file)) -ne $entry.sha256) { throw "Community mismatch: $($entry.file)" }
    }
    $receipt.status='installed-awaiting-live-test'; $receipt.backup=$backup
}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $review ($receipt.status + '.json'))
Write-Output "$($receipt.status): $($files.Count) hashes checked; only WASM and layout differ."
