root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# ---- console: the shell can keep row 0 for the status bar ----
edit('console.h', [
("""static void con_clear(void) {""", """static u32 con_top;                                             // first row text may use (1 = row 0 is the status bar)
static int con_cleared;                                         // set by con_clear(): the status bar must be drawn again
static void con_clear(void) {"""),
("""    con_col = con_row = 0;""", """    con_col = 0; con_row = con_top; con_cleared = 1;"""),
("""    for (u32 y = 0; y + n * CH < con_rows * CH; y++) {""", """    for (u32 y = con_top * CH; y + n * CH < con_rows * CH; y++) {"""),
])

# ---- speed: the channel shortcut, non-DFS 5 GHz first, a faster bus option, stage times ----
edit('wifi.h', [
("""    static const u8 ch5[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};   // all US 5 GHz channels (52-144 are DFS: passive listening only)
    nap = 0; scan_events = 0; scan_bytes = 0; dbg_taken = 0; raw_taken = 0; ev_log = 0; recs_seen = 0; mb_fail = 0;
    wifi_event_hook = scan_event;
    have_target = 0;
    char grp[10]; u32 gn = 0;
    if (wifi_verbose) { puts("looking for "); puts(scan_ssid); puts(" on 2.4 GHz...\\n"); }
    grp[gn++] = scan_band(0, ch24, sizeof ch24) ? 'a' : '-';""",
"""    static const u8 ch5[] = {36, 40, 44, 48, 149, 153, 157, 161, 165, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144};   // US 5 GHz: the normal channels first, the DFS ones (52-144, listen-only, slow) last
    nap = 0; scan_events = 0; scan_bytes = 0; dbg_taken = 0; raw_taken = 0; ev_log = 0; recs_seen = 0; mb_fail = 0;
    wifi_event_hook = scan_event;
    have_target = 0;
    char grp[10]; u32 gn = 0;
    // v1.6-002 shortcut: look on the channel the network was last seen on (this boot), or the hint in wifi_local.h, before searching everything
    u32 hint = wifi_last_chan ? wifi_last_chan : WIFI_HINT_CHAN;
    if (hint) {
        u8 hc = (u8)hint;
        scan_cmd_ms = 3000;
        scan_band(hint >= 36 ? 1 : 0, &hc, 1);
        scan_cmd_ms = 10000;
        for (u32 i = 0; i < nap && !have_target; i++) if (streq(aps[i].ssid, scan_ssid)) { ap_copy(&target, &aps[i]); have_target = 1; }
        if (have_target) grp[gn++] = 'h';
    }
    if (!have_target) {
    if (wifi_verbose) { puts("looking for "); puts(scan_ssid); puts(" on 2.4 GHz...\\n"); }
    grp[gn++] = scan_band(0, ch24, sizeof ch24) ? 'a' : '-';"""),
("""        for (u32 k = 0; k < nap && !have_target; k++) if (streq(aps[k].ssid, scan_ssid)) { ap_copy(&target, &aps[k]); have_target = 1; }
    }
    scan_cmd_ms = 10000;    wifi_event_hook = 0;""",
"""        for (u32 k = 0; k < nap && !have_target; k++) if (streq(aps[k].ssid, scan_ssid)) { ap_copy(&target, &aps[k]); have_target = 1; }
    }
    }
    if (have_target) wifi_last_chan = target.chan;
    scan_cmd_ms = 10000;    wifi_event_hook = 0;"""),
("""static struct ap target; static int have_target;""",
"""static struct ap target; static int have_target;
static u32 wifi_last_chan;                       // the channel the network was found on last time (this boot)
#ifndef WIFI_HINT_CHAN
#define WIFI_HINT_CHAN 0                         // wifi_local.h may say which channel your router uses (0 = unknown: search everything)
#endif"""),
("""    msdc_set_clock(4);
    if (sdio_read_byte(0, 0x00) < 0) { errs("WIFI", 9, 1, "firmware upload failed: bus unreliable at the faster speed"); return 0; }""",
"""    msdc_set_clock(opt_on(OPT_FASTCLK) ? 2 : 4);                                   // opt 2 "fastclk": twice the bus clock (v1.6-002 experiment)
    if (opt_on(OPT_FASTCLK) && sdio_read_byte(0, 0x00) < 0) { puts("  fast bus clock does not work here: back to the normal one\\n"); msdc_set_clock(4); }
    if (sdio_read_byte(0, 0x00) < 0) { errs("WIFI", 9, 1, "firmware upload failed: bus unreliable at the faster speed"); return 0; }"""),
])
edit('prof.h', [
("""#define OPT_COUNT   2
static const char *opt_names[OPT_COUNT] = {"warm", "fastgap"};""",
"""#define OPT_FASTCLK 2          // twice the SDIO bus clock for the Wi-Fi chip (firmware upload and all traffic)
#define OPT_COUNT   3
static const char *opt_names[OPT_COUNT] = {"warm", "fastgap", "fastclk"};"""),
])

edit('wpa.h', [
("""static int wifi_connect(const char *name) {
    joined = 0; rd_cur_port = wr_cur_port = 0; wifi_up = 0;
    int found = wifi_find(name);""",
"""static u64 cstamp[7];                                                    // when each connect stage finished (0 = not reached)
static int wifi_connect_stages(const char *name) {
    joined = 0; rd_cur_port = wr_cur_port = 0; wifi_up = 0;
    if (!wifi_ready) wifi_init();
    cstamp[1] = ticks();
    int found = wifi_find(name);
    cstamp[2] = ticks();"""),
("""        for (u32 i = 0; i <= sl && i < sizeof pmk_ssid; i++) pmk_ssid[i] = target.ssid[i];
    }
    if (!wifi_assoc()) return 0;
    sum_n = 0; sum_res[0] = 0;
    if (!wifi_handshake()) return 0;
    sum_n = 0; sum_res[0] = 0;""",
"""        for (u32 i = 0; i <= sl && i < sizeof pmk_ssid; i++) pmk_ssid[i] = target.ssid[i];
    }
    cstamp[3] = ticks();
    if (!wifi_assoc()) return 0;
    cstamp[4] = ticks();
    sum_n = 0; sum_res[0] = 0;
    if (!wifi_handshake()) return 0;
    cstamp[5] = ticks();
    sum_n = 0; sum_res[0] = 0;"""),
("""    wifi_up = 1;
    sum_s("up "); sum_u(net_ip >> 24); sum_c('.'); sum_u(net_ip >> 16 & 255); sum_c('.'); sum_u(net_ip >> 8 & 255); sum_c('.'); sum_u(net_ip & 255);
    return 1;
}""",
"""    cstamp[6] = ticks();
    wifi_up = 1;
    sum_s("up "); sum_u(net_ip >> 24); sum_c('.'); sum_u(net_ip >> 16 & 255); sum_c('.'); sum_u(net_ip >> 8 & 255); sum_c('.'); sum_u(net_ip & 255);
    return 1;
}
// wificonnect: the stages above, then one line with how long each took (it goes to the log too, so slow stages can be found)
static int wifi_connect(const char *name) {
    for (u32 i = 0; i < 7; i++) cstamp[i] = 0;
    cstamp[0] = ticks();
    int ok = wifi_connect_stages(name);
    static const char *lab[6] = {"chip ", "find ", "key ", "join ", "handshake ", "address "};
    puts("  connect times:");
    for (u32 i = 1; i < 7; i++) { if (!cstamp[i]) break; puts(" "); puts(lab[i - 1]); put_secs(ms_of(cstamp[i] - cstamp[i - 1])); putc('s'); }
    puts(wifi_last_chan ? "  (channel " : ""); if (wifi_last_chan) { put_dec(wifi_last_chan); putc(')'); }
    putc('\\n');
    return ok;
}"""),
])

# ---- browser and shell: keep the bar current ----
edit('web.h', [
("""        if (c < 0) { wdt_kick(); wifi_service(); continue; }""", """        if (c < 0) { wdt_kick(); wifi_service(); bar_tick(1); continue; }"""),
("""    web_hints();
    for (u32 i = 0; pb[i] && i < 30; i++)""", """    web_hints();
    bar_draw(1);
    for (u32 i = 0; pb[i] && i < 30; i++)"""),
])
edit('shell.h', [
("""            while ((c = input_poll()) < 0) {
                if (wifi_service()) {""",
"""            while ((c = input_poll()) < 0) {
                if (con_cleared) { con_cleared = 0; bar_dirty = 1; }
                bar_tick(0);
                if (wifi_service()) {"""),
("""    static char buf[128];
    u32 n = 0;""",
"""    static char buf[128];
    u32 n = 0;
    con_top = 1; if (con_row < 1) con_row = 1; bar_dirty = 1;                // row 0 is the status bar from now on"""),
])
edit('main.c', [("""#include "web.h\"""", """#include "bar.h"
#include "web.h\"""")])

# ---- your network's channel ----
edit('wifi_local.example.h', [("""#define WIFI_DEFAULT_BSSID {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}""",
"""#define WIFI_DEFAULT_BSSID {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}
//   WIFI_HINT_CHAN     = the channel your router uses, if you know it (0 = unknown): the first connect after boot looks there first (much faster)
#define WIFI_HINT_CHAN     0""")])
s = open(root + 'wifi_local.h').read()
if 'WIFI_HINT_CHAN' not in s:
    open(root + 'wifi_local.h', 'a', newline='').write('#define WIFI_HINT_CHAN     157\n')
s = open(root + 'version.h').read().replace('#define WAVE_PATCH   "-001"', '#define WAVE_PATCH   "-002"')
open(root + 'version.h', 'w', newline='').write(s)

# ---- the fake: a battery in the EC, a clock in the PMIC, an internet time server ----
edit('fakehana/fakeec.py', [
("""        elif cmd == 0x0060:                                  # EC_CMD_MKBP_STATE""",
"""        elif cmd == 0x0007 and n >= 2:                       # EC_CMD_READ_MEMMAP: a battery at 75 %, on the charger
            mm = bytearray(256)
            struct.pack_into('<IIIB', mm, 0x40, 12000, 500, 3000, 0x0b)
            struct.pack_into('<III', mm, 0x50, 4200, 11400, 4000)
            res, data = 0, bytes(mm[params[0]:params[0] + params[1]])
        elif cmd == 0x0060:                                  # EC_CMD_MKBP_STATE"""),
])
edit('fakehana/fakehana.py', [
("""        self.pmic = {0x0100: 0x2091, 0x041e: 0x0000, 0x043a: 0x00a1}   # CID; VGP3 off; VGP3 at 2.8 V""",
"""        self.pmic = {0x0100: 0x2091, 0x041e: 0x0000, 0x043a: 0x00a1}   # CID; VGP3 off; VGP3 at 2.8 V
        import time as _t
        g = _t.gmtime(_t.time() - 3600)                                    # MODEL: the clock chip runs an hour behind (NTP corrects it)
        self.pmic.update({0xe00a: g.tm_sec, 0xe00c: g.tm_min, 0xe00e: g.tm_hour, 0xe010: g.tm_mday, 0xe012: g.tm_wday, 0xe014: g.tm_mon, 0xe016: g.tm_year - 1968})"""),
])
edit('fakehana/fakenet.py', [
("""        self.dns_names = {'example.test': PC_IP, 'site.test': PC_IP, 'example.com': PC_IP}""",
"""        self.dns_names = {'example.test': PC_IP, 'site.test': PC_IP, 'example.com': PC_IP, 'pool.ntp.org': PC_IP}"""),
("""            if dport == 53 and dstip in (GW_IP, PC_IP) and len(data) > 12: return self.dns(src, sport, srcip, dstip, data)""",
"""            if dport == 53 and dstip in (GW_IP, PC_IP) and len(data) > 12: return self.dns(src, sport, srcip, dstip, data)
            if dport == 123 and dstip == PC_IP and len(data) >= 48:                     # a time server: the real time now
                import time as _t
                r = bytearray(48); r[0] = 0x24; struct.pack_into('>I', r, 40, int(_t.time()) + 2208988800)
                self.events.append('ntp answered')
                u = struct.pack('>HHHH', 123, sport, 8 + 48, 0) + bytes(r)
                return self.send(self.ip_packet(dstip, srcip, 17, u, src, PC_MAC))"""),
])
print('ok')
