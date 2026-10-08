# wave-os remote shell: type on the PC, the Chromebook runs it, its answer appears here (read from wave-log.txt, so the log server must run).
#   wsh.bat                 interactive: type commands, empty line or Ctrl+C to stop
#   wsh.bat "help"          run one command and show the answer
#   wsh.bat "k" -Wait 40    give a slow command more time (seconds)
# Commands are signed with update_key.txt, which only this PC has; the Chromebook ignores anything else.
param([string]$Command = '', [string]$Ip = '', [int]$Wait = 15)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$keyHex = (Get-Content (Join-Path $root 'update_key.txt') -Raw).Trim()
$key = [byte[]]@(for ($i = 0; $i -lt $keyHex.Length; $i += 2) { [Convert]::ToByte($keyHex.Substring($i, 2), 16) })
$log = Join-Path $root 'wave-log.txt'
if (-not $Ip) {                                                      # the Chromebook's address: the last one the log server heard from
    $m = Select-String -Path $log -Pattern '^\d\d:\d\d:\d\d  (\d+\.\d+\.\d+\.\d+)  ' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if (-not $m) { Write-Host "No Chromebook address yet: connect it to Wi-Fi first (k), or pass -Ip." -ForegroundColor Red; exit 1 }
    $Ip = $m.Matches[0].Groups[1].Value
}
$udp = New-Object Net.Sockets.UdpClient
$script:lastSeq = 0
function Send-Line([string]$line) {
    $seq = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    if ($seq -le $script:lastSeq) { $seq = $script:lastSeq + 1 }
    $script:lastSeq = $seq
    $sb = [byte[]]::new(8); for ($i = 7; $i -ge 0; $i--) { $sb[$i] = [byte]($seq -band 0xff); $seq = $seq -shr 8 }
    $cb = [Text.Encoding]::ASCII.GetBytes($line)
    $h = New-Object Security.Cryptography.HMACSHA256 (,$key)
    $mac = $h.ComputeHash([byte[]]($sb + $cb))[0..15]
    $pkt = [byte[]]([Text.Encoding]::ASCII.GetBytes('WRSH') + $sb + $mac + $cb)
    [void]$udp.Send($pkt, $pkt.Length, $Ip, 5150)
}
function Show-New([long]$from, [int]$seconds) {                     # print what the Chromebook logs from now on; stop 3 s after it goes quiet
    $end = (Get-Date).AddSeconds($seconds); $quietSince = $null; $pos = $from
    while ((Get-Date) -lt $end) {
        $len = (Get-Item $log).Length
        if ($len -gt $pos) {
            $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($pos, 'Begin')
            $buf = [byte[]]::new($len - $pos); [void]$fs.Read($buf, 0, $buf.Length); $fs.Close()
            $txt = [Text.Encoding]::UTF8.GetString($buf) -replace '(?m)^\d\d:\d\d:\d\d  \d+\.\d+\.\d+\.\d+  ', ''
            Write-Host -NoNewline $txt; $pos = $len; $quietSince = Get-Date
        } elseif ($quietSince -and ((Get-Date) - $quietSince).TotalSeconds -gt 3) { break }
        Start-Sleep -Milliseconds 200
    }
    return $pos
}
if ($Command) { $p = (Get-Item $log).Length; Send-Line $Command; [void](Show-New $p $Wait); exit 0 }
Write-Host "wave-os remote shell -> $Ip   (empty line to quit)" -ForegroundColor Cyan
while ($true) {
    $line = Read-Host 'wave'
    if (-not $line) { break }
    $p = (Get-Item $log).Length; Send-Line $line; [void](Show-New $p $Wait)
}
