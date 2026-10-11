param([string]$Sdk = 'C:/MSFS 2024 SDK')
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$baseline = Join-Path $projectRoot 'build/wasm/mouse-smooth-review'
$trial = Join-Path $projectRoot 'build/wasm/mouse-dispatch-trial'
New-Item -ItemType Directory -Force $trial | Out-Null
$clang = Join-Path $Sdk 'WASM/llvm/bin/clang-cl.exe'
$linker = Join-Path $Sdk 'WASM/llvm/bin/wasm-ld.exe'
$sysroot = Join-Path $Sdk 'WASM/wasi-sysroot'
# Exact source order used by build.ps1 for the installed, proven-loading baseline.
$names = 'engine','math','bindings','json_util','profile','repository','protocol','adapter','coordinator','input_router','module_app','module_main','sim_session','wasm_camera','wasm_storage'
$objects = @($names | ForEach-Object { Join-Path $baseline "obj/$_.o" })
$linkFlags = @('--no-entry','--allow-undefined','--export-dynamic','--export=malloc','--export=free','--export=__wasm_call_ctors','--export-table','--gc-sections',"-L$sysroot/lib/wasm32-wasi",'-lc++','-lc++abi','-lc','-lclang_rt.builtins-wasm32',(Join-Path $Sdk 'WASM/WasmVersions/MSFS_WasmVersions.a'),'-O2','--strip-debug')
$reproduced = Join-Path $trial 'baseline-reproduced.wasm'
& $linker @linkFlags @objects -o $reproduced
if ($LASTEXITCODE) { throw 'Baseline link failed.' }
$expected = 'f1240a2ac04908eaed94d13463d19547179dbb190180602455bd0d3a97ebd89c'
if ((Get-FileHash $reproduced -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Baseline objects do not reproduce the installed module exactly; do not stage a trial.'
}
$wrapper = Join-Path $trial 'dispatch_once.o'
& $clang '--driver-mode=gcc' '--target=wasm32-unknown-wasi' "--sysroot=$sysroot" '-std=c++20' '-O2' '-fno-exceptions' '-fms-extensions' '-D_MSFS_WASM=1' '-D__wasi__' "-I$Sdk/WASM/include" "-I$Sdk/SimConnect SDK/include" -c (Join-Path $PSScriptRoot 'dispatch_once.cpp') -o $wrapper
if ($LASTEXITCODE) { throw 'Wrapper compilation failed.' }
$output = Join-Path $trial 'FSCameraStudio.wasm'
& $linker @linkFlags @objects $wrapper '--wrap=SimConnect_CallDispatch' '--wrap=SimConnect_Close' -o $output
if ($LASTEXITCODE) { throw 'Trial link failed.' }
# Small standalone wrapper for a real WASM test with mocked SDK imports.
& $linker '--no-entry' '--allow-undefined' '--export=__wrap_SimConnect_CallDispatch' '--export=__wrap_SimConnect_Close' $wrapper -o (Join-Path $trial 'dispatch-test.wasm')
if ($LASTEXITCODE) { throw 'Wrapper test link failed.' }
[ordered]@{
    purpose='Single callback registration trial; baseline UI and camera behavior retained'
    baselineSha256=$expected
    trialSha256=(Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
    objectHashes=@($objects | ForEach-Object { @{file=$_;sha256=(Get-FileHash $_ -Algorithm SHA256).Hash.ToLowerInvariant()} })
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $trial 'build-receipt.json')
Write-Output "Reproduced baseline exactly. Trial: $output"
