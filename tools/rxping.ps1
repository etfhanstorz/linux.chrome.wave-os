# Sends test pings of several sizes to the Chromebook (run `rxtest` on the Chromebook first). Usage: rxping.bat 192.168.1.60
param([string]$Ip = '192.168.1.60')
foreach ($l in 200, 800, 1200, 1400) { ping -n 5 -l $l $Ip | Select-String 'Reply|timed out|unreachable' | Select-Object -First 1 }
