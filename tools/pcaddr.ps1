# Prints this PC's IPv4 address on the network it reaches the internet through (the Wi-Fi / Ethernet that has the default route).
# build.sh runs this so the Chromebook's `up` command knows where to fetch updates from without typing an address.
try {
    $route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop | Where-Object { $_.NextHop -ne '0.0.0.0' } | Sort-Object { $_.RouteMetric + $_.InterfaceMetric } | Select-Object -First 1
    $ip = Get-NetIPAddress -InterfaceIndex $route.InterfaceIndex -AddressFamily IPv4 -ErrorAction Stop | Where-Object { $_.IPAddress -notlike '169.254.*' } | Select-Object -First 1
    if ($ip) { $ip.IPAddress }
} catch { }
