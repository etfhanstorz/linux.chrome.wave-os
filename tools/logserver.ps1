# wave-os log server. Run it on the PC (double-click start-logserver.bat). The Chromebook sends its log and error
# lines to this PC as small UDP packets; they appear here live and are appended to wave-log.txt in the project folder.
param([int]$Port = 5140, [string]$Only = '')          # -Only 192.168.1.23 = accept lines from the Chromebook's address only
$ErrorActionPreference = 'Stop'
$logFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'wave-log.txt'
$udp = New-Object Net.Sockets.UdpClient $Port
Write-Host "wave-os log server listening on UDP port $Port, writing $logFile" -ForegroundColor Green
Write-Host "Windows may ask to allow this through the firewall: choose PRIVATE networks.`n"
$ep = New-Object Net.IPEndPoint ([Net.IPAddress]::Any, 0)
while ($true) {
    $data = $udp.Receive([ref]$ep)
    if ($Only -and $ep.Address.ToString() -ne $Only) { continue }
    $text = ([Text.Encoding]::UTF8.GetString($data)).TrimEnd("`r", "`n")
    $line = "{0}  {1}  {2}" -f (Get-Date -Format 'HH:mm:ss'), $ep.Address, $text
    Write-Host $line
    Add-Content -Path $logFile -Value $line
}
