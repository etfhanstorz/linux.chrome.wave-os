root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('wifi.h', [
# tighter polling while the chip starts
("""        if (hz && ticks() - t0 > hz) { err("WIFI", 4, "chip never became ready (CMD5 busy for 1 s)"); return 0; }
        delay_us(10000);""",
"""        if (hz && ticks() - t0 > hz) { err("WIFI", 4, "chip never became ready (CMD5 busy for 1 s)"); return 0; }
        delay_us(1000);                          // v1.6-004: poll every 1 ms (was 10 ms)"""),
("""        else if (hz && ticks() - t0 > hz) break;
        else delay_us(10000);""",
"""        else if (hz && ticks() - t0 > hz) break;
        else delay_us(1000);"""),
("""            if (cs >= 0 && (cs & 9) == 9) break;
            delay_us(500);""",
"""            if (cs >= 0 && (cs & 9) == 9) break;
            delay_us(50);                                                // v1.6-004: was 500 us per check, ~310 checks per upload"""),
("""            if (!len) delay_us(200);""", """            if (!len) delay_us(20);"""),
("""    puts("  upload done ("); put_dec(offset); puts(" bytes). waiting for the firmware to start...\\n");""",
"""    prof_stop("upload", t_up);
    puts("  upload done ("); put_dec(offset); puts(" bytes). waiting for the firmware to start...\\n");
    u64 t_boot = prof_start();"""),
("""        delay_us(10000);
    }
    puts("  firmware status 0xfedc: the Wi-Fi firmware is RUNNING.\\n");""",
"""        delay_us(1000);
    }
    prof_stop("boot", t_boot);
    puts("  firmware status 0xfedc: the Wi-Fi firmware is RUNNING.\\n");"""),
("""    static u8 buf[2312 + 256];
    u32 offset = 0, blocks = 0, retries = 0, last_kb = 0;""",
"""    static u8 buf[2312 + 256];
    u32 offset = 0, blocks = 0, retries = 0, last_kb = 0;
    u64 t_up = prof_start();"""),
# the channel shortcut does not wait for stragglers
("""        if (done_at && ticks() - done_at > hz * 3 / 2) break;""",
"""        if (done_at && (scan_quick || ticks() - done_at > hz * 3 / 2)) break;   // the channel shortcut stops as soon as the scan reports done"""),
("""// Wait up to `ms` for events on the command port, handing each to the hook, until a scan finishes.
static void wifi_poll_events(u32 ms) {""",
"""static u32 scan_quick;                      // 1 = do not keep listening after the scan's last report (one known channel)
// Wait up to `ms` for events on the command port, handing each to the hook, until a scan finishes.
static void wifi_poll_events(u32 ms) {"""),
("""        } else delay_us(500);
    }
}""", """        } else delay_us(100);
    }
}"""),
("""        scan_cmd_ms = 3000;
        scan_band(hint >= 36 ? 1 : 0, &hc, 1);
        scan_cmd_ms = 10000;""",
"""        scan_cmd_ms = 3000; scan_quick = 1;
        scan_band(hint >= 36 ? 1 : 0, &hc, 1);
        scan_cmd_ms = 10000; scan_quick = 0;"""),
])
edit('wifi.h', [
("""    if (!wifi_on()) return 0;
    u32 fwlen = (u32)(fw_end - fw_start);""",
"""    u64 t_on = prof_start();
    if (!wifi_on()) return 0;
    prof_stop("on", t_on);
    u32 fwlen = (u32)(fw_end - fw_start);"""),
])

# DHCP: retry a lost message after 0.6 s instead of 2 s
edit('net.h', [
("""    for (int attempt = 1; attempt <= 3; attempt++) {""", """    for (int attempt = 1; attempt <= 6; attempt++) {"""),
("""        if (!dhcp_wait(2, xid, 2000)) { puts("  dhcp: no offer (try "); put_dec(attempt); puts(")\\n"); continue; }""",
"""        if (!dhcp_wait(2, xid, 600)) { puts("  dhcp: no offer (try "); put_dec(attempt); puts(")\\n"); continue; }"""),
("""        if (!dhcp_wait(5, xid, 2000)) { puts("  dhcp: no ack\\n"); continue; }""",
"""        if (!dhcp_wait(5, xid, 600)) { puts("  dhcp: no ack\\n"); continue; }"""),
])

# the timing line also splits the chip stage
edit('prof.h', [("""static void prof_reset(void) { prof_idle = 0; prof_n = 0; }""",
"""static void prof_reset(void) { prof_idle = 0; prof_n = 0; }
static u64 prof_get(const char *name) { for (u32 i = 0; i < prof_n; i++) if (streq(prof_spans[i].name, name)) return prof_spans[i].t; return 0; }""")])
edit('wpa.h', [
("""    puts(wifi_last_chan ? "  (channel " : ""); if (wifi_last_chan) { put_dec(wifi_last_chan); putc(')'); }""",
"""    puts(wifi_last_chan ? "  (channel " : ""); if (wifi_last_chan) { put_dec(wifi_last_chan); putc(')'); }
    if (prof_get("upload")) {                                            // inside "chip": power cycle, chip power-up, firmware upload, firmware boot
        puts("  chip = power "); put_secs(ms_of(prof_get("pwr"))); puts(" on "); put_secs(ms_of(prof_get("on")));
        puts(" upload "); put_secs(ms_of(prof_get("upload"))); puts(" boot "); put_secs(ms_of(prof_get("boot")));
    }"""),
])
s = open(root + 'version.h').read().replace('#define WAVE_PATCH   "-003"', '#define WAVE_PATCH   "-004"')
open(root + 'version.h', 'w', newline='').write(s)
print('ok')
