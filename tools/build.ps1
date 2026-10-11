<#
.SYNOPSIS
  Builds the FS Camera Studio WASM module with the MSFS 2024 SDK toolchain and assembles the Community package.

.DESCRIPTION
  Produces build\package\fs-camera-studio\ (an MSFS 2024 Community package containing the toolbar panels and the
  standalone WASM module). Nothing is installed: copy or link that folder into your Community folder, or run
  tools\install.ps1.

.PARAMETER Configuration
  Release (default) or Debug. Debug keeps symbols for the SDK's WASM debugger.
#>
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [switch] $ModuleOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()

$sdk = $env:MSFS2024_SDK
if (-not $sdk -or -not (Test-Path (Join-Path $sdk 'WASM\llvm\bin\clang-cl.exe'))) {
    throw "MSFS2024_SDK does not point at the MSFS 2024 SDK (WASM toolchain not found)."
}
$sdk = $sdk.TrimEnd('\')
$clang = Join-Path $sdk 'WASM\llvm\bin\clang-cl.exe'
$wasmLd = Join-Path $sdk 'WASM\llvm\bin\wasm-ld.exe'
$sysroot = Join-Path $sdk 'WASM\wasi-sysroot'
$sdkVersion = (Get-Content (Join-Path $sdk 'version.txt') -TotalCount 1).Trim()

$out = Join-Path $root "build\wasm\$Configuration"
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
# Version header (passing a quoted -D through PowerShell to a native tool is unreliable).
New-Item -ItemType Directory -Force (Join-Path $out 'generated') | Out-Null
Set-Content -Encoding ascii (Join-Path $out 'generated\fscs_version.h') "#pragma once`n#define FSCS_VERSION `"$version`"`n"

$sources = @(
    Get-ChildItem (Join-Path $root 'src\CameraCore\src') -Filter *.cpp
    Get-ChildItem (Join-Path $root 'src\Profiles\src') -Filter *.cpp
    Get-ChildItem (Join-Path $root 'src\Protocol\src') -Filter *.cpp
    Get-ChildItem (Join-Path $root 'src\WasmModule') -Filter *.cpp
    Get-ChildItem (Join-Path $root 'src\WasmModule\wasm') -Filter *.cpp
)

$flags = @(
    '--driver-mode=gcc', '--target=wasm32-unknown-wasi', "--sysroot=$sysroot",
    '-std=c++20', '-fno-exceptions', '-fms-extensions', '-fvisibility=hidden', '-mthread-model', 'single',
    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-pragma-pack',
    '-D_MSFS_WASM=1', '-D__wasi__', '-D_LIBCPP_HAS_NO_THREADS',
    '-DJSON_NOEXCEPTION=1', '-DJSON_HAS_FILESYSTEM=0', '-DJSON_HAS_EXPERIMENTAL_FILESYSTEM=0',
    "-I$out\generated",
    "-I$root\src\CameraCore\include", "-I$root\src\Profiles\include", "-I$root\src\Protocol\include",
    "-I$root\src\WasmModule", "-I$root\src\WasmModule\wasm", "-I$root\third_party",
    "-I$sdk\WASM\include", "-I$sdk\SimConnect SDK\include"
)
if ($Configuration -eq 'Debug') { $flags += @('-O0', '-g') } else { $flags += @('-O2', '-DNDEBUG') }

Write-Host "FS Camera Studio $version - WASM module ($Configuration, SDK $sdkVersion)"
$objects = @()
foreach ($src in $sources) {
    $o = Join-Path $obj ($src.BaseName + '.o')
    $objects += $o
    & $clang @flags -c $src.FullName -o $o
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($src.Name)" }
}

$wasm = Join-Path $out 'FSCameraStudio.wasm'
$linkFlags = @(
    '--no-entry', '--allow-undefined', '--export-dynamic', '--export=malloc', '--export=free',
    '--export=__wasm_call_ctors', '--export-table', '--gc-sections',
    "-L$sysroot\lib\wasm32-wasi", '-lc++', '-lc++abi', '-lc', '-lclang_rt.builtins-wasm32',
    (Join-Path $sdk 'WASM\WasmVersions\MSFS_WasmVersions.a')
)
if ($Configuration -eq 'Release') { $linkFlags += @('-O2', '--strip-debug') }
& $wasmLd @linkFlags @objects -o $wasm
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
Write-Host ("Module: {0} ({1:N0} KB)" -f $wasm, ((Get-Item $wasm).Length / 1KB))
if ($ModuleOnly) { return }

& (Join-Path $PSScriptRoot 'package.ps1') -Wasm $wasm -Version $version
