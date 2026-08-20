# Live stats window for asio_bridge: tails the bridge log in real time.
# Usage: pwsh -File watch_bridge.ps1   (Ctrl+C to close)
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
$Host.UI.RawUI.WindowTitle = 'ASIO Bridge - Live Stats (watermark/underrun/peak)'
Write-Host 'ASIO Bridge live stats - press Ctrl+C to close' -ForegroundColor Cyan
Write-Host ('Log: C:\Users\Administrator\asio_bridge.log  (bridge PID: ' + ((Get-Process -Name asio_bridge -ErrorAction SilentlyContinue | Select-Object -First 1).Id) + ')')
Write-Host '----------------------------------------------------------------------'
Get-Content 'C:\Users\Administrator\asio_bridge.log' -Tail 15 -Wait -Encoding UTF8
