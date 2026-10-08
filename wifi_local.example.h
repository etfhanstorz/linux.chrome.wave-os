// Your own network's details. build.sh copies this file to wifi_local.h (which git ignores) the first time; edit wifi_local.h, not this one.
//   WIFI_DEFAULT_SSID  = the network name the shortcuts k / f / j / c use
//   WIFI_DEFAULT_BSSID = that router's hardware address (as shown by ChromeOS at chrome://network); wifi5 uses it to spot the router even when its name is garbled
#define WIFI_DEFAULT_SSID  "HomeWifi"
#define WIFI_DEFAULT_BSSID {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}
