# SSH runs in session 0, where DirectX cannot create this game's swap chain.
# Run one bounded test command as the same already signed-in desktop user.
param(
    [Parameter(Mandatory=$true)][string]$CommandFile,
    [Parameter(Mandatory=$true)][string]$LogFile,
    [ValidateRange(1, 1800)][int]$TimeoutSeconds = 360
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$command = (Resolve-Path -LiteralPath $CommandFile).Path
if ([IO.Path]::GetExtension($command) -ne '.cmd') { throw 'Supply a saved .cmd test command' }
$log = [IO.Path]::GetFullPath($LogFile)
if (Test-Path -LiteralPath $log) { throw "Refusing to replace an existing test log: $log" }
$user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
if ((Get-CimInstance Win32_ComputerSystem).UserName -ne $user) {
    throw 'The SSH user must already be signed in to the Windows desktop'
}
$taskName = 'BM64-private-test-' + [guid]::NewGuid()
$action = New-ScheduledTaskAction -Execute $env:ComSpec `
    -Argument ('/d /s /c ""{0}" > "{1}" 2>&1"' -f $command, $log) `
    -WorkingDirectory (Split-Path $command -Parent)
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds $TimeoutSeconds) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
try {
    $before = (Get-ScheduledTaskInfo -TaskName $taskName).LastRunTime
    Start-ScheduledTask -TaskName $taskName
    Write-Output "DESKTOP_TEST: task=$taskName user=$user log=$log"
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds + 5)
    do {
        Start-Sleep -Seconds 1
        $info = Get-ScheduledTaskInfo -TaskName $taskName
        $state = (Get-ScheduledTask -TaskName $taskName).State
        if ($info.LastRunTime -ne $before -and $state -ne 'Running' -and $state -ne 'Queued') {
            if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 15 }
            if ($info.LastTaskResult -ne 0) { throw "Desktop test exited with status $($info.LastTaskResult)" }
            Write-Output 'PASS: desktop test completed'
            return
        }
    } while ((Get-Date) -lt $deadline)
    throw 'Desktop test exceeded its deadline'
} finally {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
