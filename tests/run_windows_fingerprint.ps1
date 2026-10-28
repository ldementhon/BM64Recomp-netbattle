# Exercise the full game's settings path and SHA-256 without a proprietary ROM.
param([string]$Game = "$PSScriptRoot/../build/BM64Recompiled.exe")
$ErrorActionPreference = 'Stop'
$Game = (Resolve-Path $Game).Path
function Invoke-Fingerprint {
    # Explicit redirection/wait also works for the Windows GUI-subsystem build.
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $Game
    $process.StartInfo.Arguments = '--online-fingerprint'
    $process.StartInfo.WorkingDirectory = $scratch
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    try {
        [void]$process.Start()
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(15000)) {
            $process.Kill(); $process.WaitForExit()
            throw 'Fingerprint process timed out'
        }
        return @{Code=$process.ExitCode; Output=$output.Result.Trim(); Error=$errors.Result.Trim()}
    } finally {
        $process.Dispose()
    }
}
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("bm64-fingerprint-" + [guid]::NewGuid())
$isolated = Join-Path $scratch ("isolated-" + [char]0x6e2c + [char]0x8a66)
$previousFolder = $env:APP_FOLDER_PATH
$previousDirectory = [Environment]::CurrentDirectory
$sha = [Security.Cryptography.SHA256]::Create()
New-Item $isolated -ItemType Directory -Force | Out-Null
Push-Location $scratch
try {
    New-Item portable.txt -ItemType File | Out-Null
    $env:APP_FOLDER_PATH = $isolated
    $rom = Join-Path $isolated 'bm64_us.z64'
    $fixture = [Text.Encoding]::UTF8.GetBytes('Synthetic fingerprint fixture; not game data.')
    [IO.File]::WriteAllBytes($rom, $fixture)
    $gameBytes = [IO.File]::ReadAllBytes($Game)
    $gameHash = $sha.ComputeHash($gameBytes)
    $profile = [Text.Encoding]::UTF8.GetBytes('BM64 online v2; Windows x86_64; fixed Battle profile 1; delay 2')
    $expected = ([BitConverter]::ToString($sha.ComputeHash([byte[]]($profile + $gameHash + $sha.ComputeHash($fixture))))).Replace('-', '').ToLowerInvariant()
    $actual = Invoke-Fingerprint
    if ($actual.Code -ne 0 -or $actual.Output -cne $expected) {
        throw "Fingerprint mismatch: actual=$($actual.Output) expected=$expected exit=$($actual.Code) error=$($actual.Error)"
    }
    Remove-Item $rom
    $missing = Invoke-Fingerprint
    if ($missing.Code -eq 0 -or $missing.Output) { throw 'Missing ROM was not refused' }
    $env:APP_FOLDER_PATH = $null
    [IO.File]::WriteAllBytes((Join-Path $scratch 'bm64_us.z64'), $fixture)
    $portable = Invoke-Fingerprint
    if ($portable.Code -ne 0 -or $portable.Output -cne $expected) { throw 'Portable fallback failed' }
    $after = $sha.ComputeHash([IO.File]::ReadAllBytes($Game))
    if ([BitConverter]::ToString($gameHash) -cne [BitConverter]::ToString($after)) {
        throw 'Executable changed during test'
    }
    Write-Output "PASS: full Windows fingerprint, Unicode isolation overrides portable mode, portable fallback, missing ROM refused; build=$(([BitConverter]::ToString($gameHash)).Replace('-', '').ToLowerInvariant())"
} finally {
    $sha.Dispose()
    $env:APP_FOLDER_PATH = $previousFolder
    Pop-Location
    [Environment]::CurrentDirectory = $previousDirectory
    Remove-Item $scratch -Recurse -Force
}
