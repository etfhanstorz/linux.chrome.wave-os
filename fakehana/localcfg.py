"""Your own network's name and router address for the fake router: read from ../wifi_local.h (git-ignored), else the published placeholder."""
import os, re

def load():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    for name in ('wifi_local.h', 'wifi_local.example.h'):
        p = os.path.join(root, name)
        if os.path.exists(p):
            t = open(p).read()
            ssid = re.search(r'WIFI_DEFAULT_SSID\s+"([^"]*)"', t).group(1)
            raw = re.search(r'WIFI_DEFAULT_BSSID\s+\{([^}]*)\}', t).group(1)
            return ssid.encode(), bytes(int(x, 0) for x in raw.split(','))
    return b'HomeWifi', bytes.fromhex('021122334455')

SSID, BSSID = load()
