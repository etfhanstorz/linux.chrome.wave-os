# wave-os update server. Run this on the PC (double-click start-updateserver.bat), then on the Chromebook type:
#     update <this PC's address>
# It serves two things on port 8000: /Image (the newest build, straight from this folder) and /manifest
# (size, SHA-256 and a keyed signature of that image). wave-os only runs an update that matches both.
param([int]$Port = 8000)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$imagePath = Join-Path $root 'Image'
$keyPath = Join-Path $root 'update_key.txt'
if (-not (Test-Path $keyPath)) { Write-Host "No update_key.txt yet: run flash.bat or build.sh once so a key is created." -ForegroundColor Red; Read-Host "Press Enter"; exit 1 }
$key = [byte[]]@(for ($i = 0; $i -lt 32; $i += 2) { [Convert]::ToByte((Get-Content $keyPath -Raw).Trim().Substring($i, 2), 16) })

function Get-Manifest {
    $bytes = [IO.File]::ReadAllBytes($imagePath)
    $sha = [Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
    $mac = (New-Object Security.Cryptography.HMACSHA256 (,$key)).ComputeHash($bytes)
    $hex = { param($b) -join ($b | ForEach-Object { $_.ToString('x2') }) }
    return [Text.Encoding]::ASCII.GetBytes("$($bytes.Length) $(& $hex $sha) $(& $hex $mac)`n")
}

$ips = [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | Where-Object { $_.OperationalStatus -eq 'Up' } |
    ForEach-Object { $_.GetIPProperties().UnicastAddresses } | Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' -and -not $_.Address.IsIPv6LinkLocal -and $_.Address.ToString() -notlike '127.*' } |
    ForEach-Object { $_.Address.ToString() }
Write-Host "wave-os update server on port $Port, serving $imagePath" -ForegroundColor Green
Write-Host "This PC's address(es): $($ips -join ', ')   (on the Chromebook: update <address>)"
Write-Host "Windows may ask to allow this through the firewall: choose PRIVATE networks.`n"

$listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Any, $Port)
$listener.Start()
while ($true) {
    $client = $listener.AcceptTcpClient()
    try {
        $stream = $client.GetStream(); $stream.ReadTimeout = 5000
        $buf = New-Object byte[] 4096; $n = $stream.Read($buf, 0, $buf.Length)
        $req = [Text.Encoding]::ASCII.GetString($buf, 0, $n)
        $path = ($req -split "`r?`n")[0].Split(' ')[1]
        if ($path -eq '/Image' -and (Test-Path $imagePath)) { $body = [IO.File]::ReadAllBytes($imagePath); $status = '200 OK' }
        elseif ($path -eq '/manifest' -and (Test-Path $imagePath)) { $body = Get-Manifest; $status = '200 OK' }
        else { $body = [byte[]]@(); $status = '404 Not Found' }
        $head = [Text.Encoding]::ASCII.GetBytes("HTTP/1.0 $status`r`nContent-Type: application/octet-stream`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n")
        $stream.Write($head, 0, $head.Length); $stream.Write($body, 0, $body.Length); $stream.Flush()
        Write-Host ("{0}  {1}  {2}  {3} bytes" -f (Get-Date -Format 'HH:mm:ss'), $client.Client.RemoteEndPoint, $path, $body.Length)
    } catch { Write-Host "connection error: $_" -ForegroundColor Yellow }
    finally { $client.Close() }
}
