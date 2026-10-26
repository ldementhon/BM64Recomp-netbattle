# Run in an x64 Visual Studio developer shell with clang-cl on PATH.
param([string]$Build = "$PSScriptRoot/../build")
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
$root = Split-Path $PSScriptRoot -Parent
$previousDirectory = [Environment]::CurrentDirectory
$entries = Get-Content "$Build/compile_commands.json" -Raw | ConvertFrom-Json
$entry = $entries | Where-Object { $_.file.Replace('\', '/').EndsWith('/src/game/input.cpp') } | Select-Object -First 1
if (!$entry) { throw 'Configure the Windows game build with CMAKE_EXPORT_COMPILE_COMMANDS=ON first' }
# Reuse project include paths and definitions; test optimization/assertions are explicit.
$flags = @([regex]::Matches($entry.command, '(?:^|\s)[-/][ID](?:"[^"]*"|[^\s"]+"[^"]*"|\S+)') |
    ForEach-Object { $_.Value.Trim().Replace('"', '') } | Where-Object { $_ -notmatch '^[-/]DNDEBUG$' })
$scratch = Join-Path $Build ('controller-tests-' + [guid]::NewGuid())
New-Item $scratch -ItemType Directory | Out-Null
Push-Location $scratch
try {
    Copy-Item "$Build/SDL2.dll" .
    foreach ($name in @('stick', 'ports')) {
        $sources = @("$root/tests/stick_calibration_test.cpp")
        if ($name -eq 'ports') {
            $sources = @("$root/tests/controller_ports_test.cpp", "$root/src/game/input.cpp",
                         "$root/src/game/controls.cpp", "$root/src/game/config.cpp",
                         "$root/lib/N64ModernRuntime/librecomp/src/files.cpp")
        }
        & clang-cl /nologo /std:c++20 /EHsc /O2 /UNDEBUG /Gy /Gw /DSDL_MAIN_HANDLED @flags `
            "-I$root/lib/N64ModernRuntime/librecomp/include/librecomp" @sources "/Fe:$name.exe" `
            /link /OPT:REF "$Build/_deps/sdl2-src/lib/x64/SDL2.lib" shell32.lib ole32.lib
        if ($LASTEXITCODE) { throw "Controller test build failed: $name" }
        & ".\$name.exe" $scratch
        if ($LASTEXITCODE) { throw "Controller test failed: $name" }
        if ($name -eq 'ports') {
            & .\ports.exe $scratch recreate
            if ($LASTEXITCODE) { throw 'Fresh-process controller handoff failed' }
        }
    }
} finally {
    Pop-Location
    [Environment]::CurrentDirectory = $previousDirectory
    Remove-Item $scratch -Recurse -Force
}
