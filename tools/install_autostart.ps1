# Install / remove the asio_bridge autostart scheduled task.
# Usage:  pwsh -File install_autostart.ps1           # install
#         pwsh -File install_autostart.ps1 -Remove   # uninstall
# Runs at user logon, hidden console, highest privileges.
# Paths are derived from this script's own location (tools\..\build\Release),
# so moving the repo does not break the task. Log goes to %USERPROFILE%\asio_bridge.log
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
param([switch]$Remove, [int]$Buffer = 128, [switch]$Dither)

$taskName = 'ASIO Bridge'

if ($Remove) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Stop-Process -Name asio_bridge -Force -ErrorAction SilentlyContinue
    Write-Output "Autostart removed (task '$taskName')"
    exit 0
}

# Derive paths from this script's location instead of hardcoding E:\Harness
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\Release\asio_bridge.exe'
if (-not (Test-Path $exe)) {
    Write-Output "Bridge not found: $exe (build it first)"
    exit 1
}
$logFile = Join-Path $env:USERPROFILE 'asio_bridge.log'

# Generate a hidden VBS launcher: the task runs wscript.exe (GUI, no console),
# so no terminal window appears at logon. The VBS itself derives paths dynamically,
# but we regenerate it so it always matches the current install options.
$vbs = Join-Path $PSScriptRoot 'start_hidden.vbs'
$vbsContent = 'Set fso = CreateObject("Scripting.FileSystemObject")' + "`r`n" +
    'Set ws = CreateObject("Wscript.Shell")' + "`r`n" +
    'here = fso.GetParentFolderName(fso.GetAbsolutePathName(WScript.ScriptFullName))' + "`r`n" +
    'root = fso.GetParentFolderName(here)' + "`r`n" +
    'exe = root & "\build\Release\asio_bridge.exe"' + "`r`n" +
    'logFile = ws.ExpandEnvironmentStrings("%USERPROFILE%") & "\asio_bridge.log"' + "`r`n" +
    'ws.Run """" & exe & """ --log """ & logFile & """'
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
Write-Output "Log file: $logFile"
