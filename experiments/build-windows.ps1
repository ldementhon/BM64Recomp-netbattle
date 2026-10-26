# Build a private source bundle exported by export-online-source.py.
param([int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    if (!$env:WindowsSdkDir -or !$env:WindowsSDKVersion) {
        throw 'Run from an x64 Visual Studio developer shell with CMake, Ninja, and clang-cl on PATH.'
    }
    $manifest = Get-Content source-bundle.json -Raw | ConvertFrom-Json
    foreach ($file in $manifest.files.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath $file.Name -Algorithm SHA256).Hash -ne $file.Value) {
            throw "Source bundle changed: $($file.Name)"
        }
    }
    Write-Output "Verified source bundle from $($manifest.root_commit)"
    $manifestTool = Join-Path $env:WindowsSdkDir "bin/$($env:WindowsSDKVersion)x64/mt.exe"
    & cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        -DBM64_PREGENERATED_PATCHES=ON -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        "-DCMAKE_MT=$manifestTool" '-DCMAKE_CXX_FLAGS=-Xclang -fexceptions -Xclang -fcxx-exceptions'
    if ($LASTEXITCODE) { throw 'Windows game configuration failed' }
    & cmake --build build --target BM64Recompiled -j $Jobs
    if ($LASTEXITCODE) { throw 'Windows game build failed' }
    Copy-Item assets build -Recurse -Force
    Copy-Item recompcontrollerdb.txt build -Force
    Get-FileHash build/BM64Recompiled.exe -Algorithm SHA256
} finally {
    Pop-Location
}
