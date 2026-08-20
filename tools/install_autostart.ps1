# Install / remove the asio_bridge autostart scheduled task.
# Usage:  pwsh -File install_autostart.ps1           # install
#         pwsh -File install_autostart.ps1 -Remove   # uninstall
# Runs at user logon, hidden console, highest privileges, log to C:\Users\Administrator\asio_bridge.log
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
param([switch]$Remove, [int]$Buffer = 128, [switch]$Dither)

$taskName = 'ASIO Bridge'

if ($Remove) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Stop-Process -Name asio_bridge -Force -ErrorAction SilentlyContinue
    Write-Output "Autostart removed (task '$taskName')"
    exit 0
}

$exe = 'E:\Harness\asio-bridge\build\Release\asio_bridge.exe'
if (-not (Test-Path $exe)) {
    Write-Output "Bridge not found: $exe (build it first)"
    exit 1
}

# Generate a hidden VBS launcher: the task runs wscript.exe (GUI, no console),
# so no terminal window appears at logon.
$vbs = 'E:\Harness\asio-bridge\tools\start_hidden.vbs'
$vbsContent = 'Set ws = CreateObject("Wscript.Shell")' + "`r`n" +
    'ws.Run """' + $exe + '"" --log C:\Users\Administrator\asio_bridge.log'
if ($Buffer -gt 0) { $vbsContent += ' --buffer ' + $Buffer }
if ($Dither) { $vbsContent += ' --dither' }
$vbsContent += '", 0, False' + "`r`n"
Set-Content -Path $vbs -Value $vbsContent -Encoding ASCII

$action = New-ScheduledTaskAction -Execute 'wscript.exe' `
    -Argument ('"' + $vbs + '"') `
    -WorkingDirectory (Split-Path $exe)
$trigger = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" `
    -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings -Force | Out-Null
Write-Output "Autostart installed: task '$taskName' (logon, hidden, auto-restart x3, buffer=$Buffer)"
Write-Output "Log file: C:\Users\Administrator\asio_bridge.log"
