<#
.SYNOPSIS
  Stages the MSFS project for FS Camera Studio and, with -Compile, builds the Community package with fspackagetool.

.DESCRIPTION
  Staging is safe and offline: it copies the package sources, the web UI and the WASM module into build\project and
  generates the two toolbar panel pages (main.html, mini.html) from ui\index.html.

  -Compile runs the SDK's fspackagetool, which uses the simulator's build mode to compile the toolbar panel
  definitions (.spb). It writes the finished package to build\package\fs-camera-studio.
#>
param(
    [string] $Wasm,
    [string] $Version,
    [switch] $Compile
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Version) { $Version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim() }
if (-not $Wasm) { $Wasm = Join-Path $root 'build\wasm\Release\FSCameraStudio.wasm' }
if (-not (Test-Path $Wasm)) { throw "WASM module not found at $Wasm. Run tools\build.ps1 first." }

$project = Join-Path $root 'build\project'
if (Test-Path $project) { Remove-Item -Recurse -Force $project }
Copy-Item -Recurse (Join-Path $root 'package') $project

$definition = Join-Path $project 'PackageDefinitions\fs-camera-studio.xml'
[IO.File]::WriteAllText($definition, ([IO.File]::ReadAllText($definition)).Replace('@VERSION@', $Version), (New-Object Text.UTF8Encoding($false)))

# Web UI
$panelDir = Join-Path $project 'PackageSources\html_ui\InGamePanels\FSCameraStudio'
New-Item -ItemType Directory -Force $panelDir | Out-Null
foreach ($f in 'app.css', 'app.js', 'bridge.js', 'icons.js', 'math.js', 'msfs-panel.js') {
    Copy-Item (Join-Path $root "ui\$f") $panelDir
}

# Panel pages: the shared index.html wrapped in the simulator's <ingame-ui> host, with the sim's CommBus library.
# Files are read and written as UTF-8 without BOM (Windows PowerShell's defaults would mangle non-ASCII text).
$utf8 = New-Object Text.UTF8Encoding($false)
$index = [IO.File]::ReadAllText((Join-Path $root 'ui\index.html'), $utf8)
Copy-Item (Join-Path $root 'package\panel\panel-host.css') $panelDir
$simHead = @'
<script type="text/javascript" src="/JS/coherent.js"></script>
<script type="text/javascript" src="/JS/common.js"></script>
<script type="text/javascript" src="/JS/Services/CommBus.js"></script>
<script type="text/javascript" src="/JS/Services/ToolBarPanels.js"></script>
<link rel="import" href="/templates/ingameUi/ingameUi.html" />
'@
$panels = @(
    # Main: the simulator's own window frame gives a title bar to drag and native resizing; the editor fills it.
    @{ Surface = 'main'; File = 'main.html'; PanelId = 'PANEL_FSCS_CAMERA_STUDIO'; Title = 'Camera Studio'; Id = 'FSCSMain'; Attrs = 'class="ingameUiFrame"' },
    # Mini: borderless; the window height follows the content (content-fit + horizontal-only resize), so no
    # invisible slack blocks cockpit clicks.
    @{ Surface = 'mini'; File = 'mini.html'; PanelId = 'PANEL_FSCS_CAMERA_MINI'; Title = 'Camera Studio Mini'; Id = 'FSCSMini'; Attrs = 'class="ingameUiFrame panelInvisible" content-fit="true" min-height="0" resize="x"' }
)
foreach ($p in $panels) {
    $html = $index
    if ($html -notmatch '<div id="mc-sim"' -or $html -notmatch '<script src="math.js"></script>' -or
        $html -notmatch '<link rel="stylesheet" href="app.css">') {
        throw 'ui\index.html no longer has the expected #mc-sim root, stylesheet and script order; update tools\package.ps1.'
    }
    # The browser preview's Content-Security-Policy would block the simulator's own scripts; the sim sandbox applies.
    $html = [regex]::Replace($html, '<meta http-equiv="Content-Security-Policy"[^>]*>\s*', '')
    $html = $html.Replace('<link rel="stylesheet" href="app.css">',
        $simHead + '<link rel="stylesheet" href="app.css">' + "`n" + '<link rel="stylesheet" href="panel-host.css">')
    $open = '<ingame-ui id="' + $p.Id + '" panel-id="' + $p.PanelId + '" title="' + $p.Title + '" ' + $p.Attrs + '>'
    $html = [regex]::Replace($html, '<div id="mc-sim"', ($open + "`n<div id=`"mc-sim`""), 1)
    # The surface is set by a script file, not an inline script.
    $surfaceJs = 'surface-' + $p.Surface + '.js'
    [IO.File]::WriteAllText((Join-Path $panelDir $surfaceJs), "window.FSCS_SURFACE = '" + $p.Surface + "';`n", $utf8)
    $html = $html.Replace('<script src="math.js"></script>',
        "</ingame-ui>`n<script src=`"" + $surfaceJs + "`"></script>`n<script src=`"math.js`"></script>")
    [IO.File]::WriteAllText((Join-Path $panelDir $p.File), $html, $utf8)
}

# WASM module
$modules = Join-Path $project 'PackageSources\modules'
New-Item -ItemType Directory -Force $modules | Out-Null
Copy-Item $Wasm (Join-Path $modules 'FSCameraStudio.wasm')

Write-Host "Staged MSFS project: $project"
if (-not $Compile) {
    Write-Host 'Run tools\package.ps1 -Compile to build the Community package with fspackagetool (uses the simulator build mode).'
    return
}

$sdk = $env:MSFS2024_SDK.TrimEnd('\')
$tool = Join-Path $sdk 'Tools\bin\fspackagetool.exe'
$output = Join-Path $root 'build\package'
$temp = Join-Path $root 'build\package-temp'
foreach ($d in $output, $temp) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }  # no stale reports or files
New-Item -ItemType Directory -Force $output, $temp | Out-Null
& $tool (Join-Path $project 'fs-camera-studio.xml') -outputdir $output -tempdir $temp -rebuild -nopause
if ($LASTEXITCODE -ne 0) { throw "fspackagetool failed with exit code $LASTEXITCODE (see its log in $temp)." }
# fspackagetool can exit 0 after a failed build; its error report is authoritative.
$report = Join-Path $temp '_PackageInt\_RPTErrors.xml'
if ((Test-Path $report) -and (Select-String -Path $report -Pattern '<RPTError>' -Quiet)) {
    Get-Content $report | Select-String 'Message=' | ForEach-Object { Write-Host $_.Line.Trim() }
    throw "fspackagetool reported build errors ($report)."
}
$pkg = Join-Path $output 'Packages\fs-camera-studio'
# The simulator finishes copying after fspackagetool returns: wait for every expected output and for build mode to end.
$expected = @('InGamePanels\fscs-camera-studio.spb', 'InGamePanels\fscs-camera-mini.spb', 'modules\FSCameraStudio.wasm',
              'html_ui\InGamePanels\FSCameraStudio\main.html', 'html_ui\InGamePanels\FSCameraStudio\mini.html',
              'html_ui\InGamePanels\FSCameraStudio\app.js', 'manifest.json')
$deadline = (Get-Date).AddMinutes(3)
while ($true) {
    $missing = @($expected | Where-Object { -not (Test-Path (Join-Path $pkg $_)) })
    $busy = Get-Process -Name 'FlightSimulator2024' -ErrorAction SilentlyContinue
    if ($missing.Count -eq 0 -and -not $busy) { break }
    if ((Get-Date) -gt $deadline) { throw "The package was not completed. Missing: $($missing -join ', ')" }
    Start-Sleep -Seconds 2
}
# fspackagetool on this setup writes no layout.json (also seen in other projects on this PC); MSFS needs it to mount
# the package, so build it from the package contents: every file except manifest/layout, with size and FILETIME.
$entries = Get-ChildItem -Recurse -File $pkg | Where-Object { $_.Name -ne 'layout.json' -and $_.Name -ne 'manifest.json' } |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($pkg.Length + 1).Replace('\', '/')
            size = $_.Length
            date = $_.LastWriteTimeUtc.ToFileTimeUtc()
        }
    }
$layout = [ordered]@{ content = @($entries) } | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path $pkg 'layout.json'), $layout, (New-Object Text.UTF8Encoding($false)))
Write-Host "layout.json written ($(@($entries).Count) files)."
Write-Host "Package: $(Join-Path $output 'Packages\fs-camera-studio')"
