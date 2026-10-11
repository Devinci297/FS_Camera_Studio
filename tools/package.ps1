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
(Get-Content $definition -Raw).Replace('@VERSION@', $Version) | Set-Content -Encoding utf8 $definition

# Web UI
$panelDir = Join-Path $project 'PackageSources\html_ui\InGamePanels\FSCameraStudio'
New-Item -ItemType Directory -Force $panelDir | Out-Null
foreach ($f in 'app.css', 'app.js', 'bridge.js', 'icons.js', 'math.js', 'msfs-panel.js') {
    Copy-Item (Join-Path $root "ui\$f") $panelDir
}

# Panel pages: the shared index.html wrapped in the simulator's <ingame-ui> host, with the sim's CommBus library.
$index = Get-Content (Join-Path $root 'ui\index.html') -Raw
$simHead = @'
<script type="text/javascript" src="/JS/coherent.js"></script>
<script type="text/javascript" src="/JS/common.js"></script>
<script type="text/javascript" src="/JS/Services/CommBus.js"></script>
<script type="text/javascript" src="/JS/Services/ToolBarPanels.js"></script>
<link rel="import" href="/templates/ingameUi/ingameUi.html" />
'@
$panels = @(
    @{ Surface = 'main'; File = 'main.html'; PanelId = 'PANEL_FSCS_CAMERA_STUDIO'; Title = 'Camera Studio' },
    @{ Surface = 'mini'; File = 'mini.html'; PanelId = 'PANEL_FSCS_CAMERA_MINI'; Title = 'Camera Studio Mini' }
)
foreach ($p in $panels) {
    $html = $index
    if ($html -notmatch '<div id="mc-sim"' -or $html -notmatch '<script src="math.js"></script>') {
        throw 'ui\index.html no longer has the expected #mc-sim root and script order; update tools\package.ps1.'
    }
    $html = $html.Replace('<link rel="stylesheet" href="app.css">', $simHead + '<link rel="stylesheet" href="app.css">')
    $open = '<ingame-ui id="FSCSPanel" panel-id="' + $p.PanelId + '" title="' + $p.Title + '" class="ingameUiFrame panelInvisible" content-fit="true">'
    $html = [regex]::Replace($html, '<div id="mc-sim"', ($open + "`n<div id=`"mc-sim`""), 1)
    $html = $html.Replace('<script src="math.js"></script>',
        "</ingame-ui>`n<script>window.FSCS_SURFACE = '" + $p.Surface + "';</script>`n<script src=`"math.js`"></script>")
    Set-Content -Encoding utf8 (Join-Path $panelDir $p.File) $html
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
New-Item -ItemType Directory -Force $output, $temp | Out-Null
& $tool (Join-Path $project 'fs-camera-studio.xml') -outputdir $output -tempdir $temp -rebuild -nopause
if ($LASTEXITCODE -ne 0) { throw "fspackagetool failed with exit code $LASTEXITCODE (see its log in $temp)." }
Write-Host "Package: $(Join-Path $output 'Packages\fs-camera-studio')"
