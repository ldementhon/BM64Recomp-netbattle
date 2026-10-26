param([string]$Compiler = 'clang-cl')
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Force build/experiments | Out-Null
    foreach ($optimization in @('Od', 'O2')) {
        Write-Output "Optimization: /$optimization"
        $output = "build/experiments/coroutine_replay_$optimization"
        & $Compiler /nologo /std:c++20 /EHsc "/$optimization" /DNOMINMAX `
            /Ilib/N64ModernRuntime/N64Recomp/include `
            /Ilib/N64ModernRuntime/librecomp/include `
            /Ilib/N64ModernRuntime/ultramodern/include `
            tests/coroutine_replay.cpp "/Fe:$output.exe" "/Fo:$output.obj"
        if ($LASTEXITCODE) { throw "Coroutine test build failed ($optimization)" }
        & "$output.exe"
        if ($LASTEXITCODE) { throw "Coroutine restore test failed ($optimization)" }
    }
} finally {
    Pop-Location
}
