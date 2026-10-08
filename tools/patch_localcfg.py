root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# the placeholder file that goes to GitHub, and the real one (never committed)
open(root + 'wifi_local.example.h', 'w', newline='').write('''// Your own network's details. build.sh copies this file to wifi_local.h (which git ignores) the first time; edit wifi_local.h, not this one.
//   WIFI_DEFAULT_SSID  = the network name the shortcuts k / f / j / c use
//   WIFI_DEFAULT_BSSID = that router's hardware address (as shown by ChromeOS at chrome://network); wifi5 uses it to spot the router even when its name is garbled
#define WIFI_DEFAULT_SSID  "HomeWifi"
#define WIFI_DEFAULT_BSSID {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}
''')
open(root + 'wifi_local.h', 'w', newline='').write('''// Your own network's details (this file is git-ignored; wifi_local.example.h is the placeholder that is published).
#define WIFI_DEFAULT_SSID  "HomeWifi"
#define WIFI_DEFAULT_BSSID {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}
''')

edit('shell.h', [
("""    static char al_arg[24];""", """    static char al_arg[48];"""),
("""const char *a = "157 HomeWifi";""", """const char *a = "157 " WIFI_DEFAULT_SSID;"""),
("""    else if (streq(line, "k")) { line = "wificonnect"; const char *a = "HomeWifi";""", """    else if (streq(line, "k")) { line = "wificonnect"; const char *a = WIFI_DEFAULT_SSID;"""),
("""    else if (streq(line, "j")) { line = "wifijoin"; const char *a = "HomeWifi";""", """    else if (streq(line, "j")) { line = "wifijoin"; const char *a = WIFI_DEFAULT_SSID;"""),
("""    else if (streq(line, "f")) { line = "wififind"; const char *a = "HomeWifi";""", """    else if (streq(line, "f")) { line = "wififind"; const char *a = WIFI_DEFAULT_SSID;"""),
("""wifi_join(*arg ? arg : "HomeWifi");""", """wifi_join(*arg ? arg : WIFI_DEFAULT_SSID);"""),
("""wifi_connect(*arg ? arg : "HomeWifi");""", """wifi_connect(*arg ? arg : WIFI_DEFAULT_SSID);"""),
])

edit('wifi.h', [
("""puts("usage: wifichan CHANNEL [NAME]   e.g. wifichan 157 HomeWifi\\n");""", """puts("usage: wifichan CHANNEL [NAME]   e.g. wifichan 157 " WIFI_DEFAULT_SSID "\\n");"""),
("""const char *d = "HomeWifi"; while (d[nl])""", """const char *d = WIFI_DEFAULT_SSID; while (d[nl])"""),
("""    static const u8 home_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};          // HomeWifi's address, from ChromeOS (chrome://network)""",
 """    static const u8 home_mac[6] = WIFI_DEFAULT_BSSID;                              // our router's address (wifi_local.h)"""),
("""        u32 pre = aps[k].bssid[0] == 0x02 && aps[k].bssid[1] == 0x11 && aps[k].bssid[2] == 0x22;
        u32 ex = pre; for (u32 m = 3; m < 6; m++) if (aps[k].bssid[m] != home_mac[m]) ex = 0;""",
 """        u32 pre = aps[k].bssid[0] == home_mac[0] && aps[k].bssid[1] == home_mac[1] && aps[k].bssid[2] == home_mac[2];
        u32 ex = pre; for (u32 m = 3; m < 6; m++) if (aps[k].bssid[m] != home_mac[m]) ex = 0;"""),
])

edit('main.c', [("""#include "wifi.h\"""", """#include "wifi_local.h"                              // WIFI_DEFAULT_SSID / WIFI_DEFAULT_BSSID: your own network (git-ignored)
#include "wifi.h\"""")])

edit('build.sh', [("""# This PC's address""", """# Your own network's name and router address live in wifi_local.h (git-ignored); the first build starts it from the published placeholder.
[ -f wifi_local.h ] || cp wifi_local.example.h wifi_local.h
# This PC's address""")])

# the fake reads the same file
open(root + 'fakehana/localcfg.py', 'w', newline='').write('''"""Your own network's name and router address for the fake router: read from ../wifi_local.h (git-ignored), else the published placeholder."""
import os, re

def load():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    for name in ('wifi_local.h', 'wifi_local.example.h'):
        p = os.path.join(root, name)
        if os.path.exists(p):
            t = open(p).read()
            ssid = re.search(r'WIFI_DEFAULT_SSID\\s+"([^"]*)"', t).group(1)
            raw = re.search(r'WIFI_DEFAULT_BSSID\\s+\\{([^}]*)\\}', t).group(1)
            return ssid.encode(), bytes(int(x, 0) for x in raw.split(','))
    return b'HomeWifi', bytes.fromhex('021122334455')

SSID, BSSID = load()
''')
edit('fakehana/fakeap.py', [
('''Network "HomeWifi", password''', '''Network = the name in wifi_local.h, password'''),
("""AP_MAC = bytes.fromhex('021122334455')
SSID = b'HomeWifi'""", """import localcfg
AP_MAC = localcfg.BSSID
SSID = localcfg.SSID"""),
])
edit('fakehana/fakesdio.py', [
("""(b'HomeWifi', '02:11:22:33:44:55', -62, 157, True)]""", """(localcfg.SSID, ':'.join('%02x' % b for b in localcfg.BSSID), -62, 157, True)]"""),
("""good = peer == bytes.fromhex('021122334455') and tlvs.get(0) == b'HomeWifi' and""", """good = peer == localcfg.BSSID and tlvs.get(0) == localcfg.SSID and"""),
("""import struct
""", """import struct
import localcfg
"""),
])
print('ok')
