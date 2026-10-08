root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# join.h: remember the geometry of the last data packet
edit('join.h', [
("""static u16 rx_first_type;""", """static u32 rx_last_off, rx_last_len;      // rx descriptor offset and SDIO length of the last data packet (rxtest prints them)
static u16 rx_first_type;"""),
("""    u8 *f = dbuf + 4 + off;
    u32 fl = pkt_len;""", """    rx_last_off = off; rx_last_len = len;
    u8 *f = dbuf + 4 + off;
    u32 fl = pkt_len;"""),
])

# net.h: capture what the wrong bytes look like
edit('net.h', [
("""static struct { u32 len, ok, bad, first_min, nbad_max; } rxt[RXT_MAX]; static u32 rxt_n, rxt_bad_ip;""",
 """static struct { u32 len, ok, bad, first_min, nbad_max, zeros; u8 got[8]; } rxt[RXT_MAX]; static u32 rxt_n, rxt_bad_ip, rxt_last_tick_ok;"""),
("""    u32 n = plen - 8, first = 0xffffffff, nbad = 0;
    for (u32 i = 0; i < n; i++) if (echo[8 + i] != 'a' + i % 23) { if (first == 0xffffffff) first = i; nbad++; }""",
 """    u32 n = plen - 8, first = 0xffffffff, nbad = 0, zeros = 0;
    for (u32 i = 0; i < n; i++) if (echo[8 + i] != 'a' + i % 23) { if (first == 0xffffffff) first = i; nbad++; if (!echo[8 + i]) zeros++; }"""),
("""rxt[k].first_min = 0xffffffff; rxt[k].nbad_max = 0; rxt_n++; }""",
 """rxt[k].first_min = 0xffffffff; rxt[k].nbad_max = 0; rxt[k].zeros = 0; rxt_n++; }"""),
("""    if (nbad || csum(echo, plen, 0) != 0xffff) { rxt[k].bad++; if (first < rxt[k].first_min) rxt[k].first_min = first; if (nbad > rxt[k].nbad_max) rxt[k].nbad_max = nbad; }""",
 """    if (nbad || csum(echo, plen, 0) != 0xffff) {
        rxt[k].bad++;
        if (first < rxt[k].first_min) { rxt[k].first_min = first; rxt[k].zeros = zeros; for (u32 i = 0; i < 8; i++) rxt[k].got[i] = first + i < n ? echo[8 + first + i] : 0; }
        if (nbad > rxt[k].nbad_max) rxt[k].nbad_max = nbad;
    }"""),
])

# wpa.h: rxtest prints more and stops early; rdcfg command
edit('wpa.h', [
("""    u64 hz = tick_hz(), t0 = ticks();
    while (hz && ticks() - t0 < hz * 40) { net_poll(); wdt_kick(); }
    rxtest_on = 0;""",
"""    u64 hz = tick_hz(), t0 = ticks(), last = 0; u32 seen = 0;
    while (hz && ticks() - t0 < hz * 40) {
        net_poll(); wdt_kick();
        u32 now = 0; for (u32 k = 0; k < rxt_n; k++) now += rxt[k].ok + rxt[k].bad;
        if (now != seen) { seen = now; last = ticks(); }
        if (seen && ticks() - last > hz * 4) break;                 // pings stopped: finish early
    }
    rxtest_on = 0;"""),
("""        if (rxt[k].bad) { puts("  first wrong byte at "); put_dec(rxt[k].first_min); puts(", up to "); put_dec(rxt[k].nbad_max); puts(" bytes wrong"); }
        putc('\\n'); tot_ok += rxt[k].ok; tot_bad += rxt[k].bad;
    }""",
"""        if (rxt[k].bad) {
            puts("  first wrong byte at "); put_dec(rxt[k].first_min); puts(", up to "); put_dec(rxt[k].nbad_max); puts(" wrong, "); put_dec(rxt[k].zeros); puts(" of them 0; got:");
            for (u32 i = 0; i < 8; i++) { putc(' '); putc("0123456789abcdef"[rxt[k].got[i] >> 4]); putc("0123456789abcdef"[rxt[k].got[i] & 15]); }
        }
        putc('\\n'); tot_ok += rxt[k].ok; tot_bad += rxt[k].bad;
    }
    puts("  last packet: chip length "); put_dec(rx_last_len); puts(", frame offset "); put_dec(rx_last_off); puts(";  read settings: chunk "); put_dec(rd_chunk); puts(" gap "); put_dec(rd_gap_us); puts(" div "); put_dec(rd_div); putc('\\n');"""),
])
open(root + 'wpa.h', 'a', newline='').write("""
// rdcfg [CHUNK [GAP [DIV]]]: how the chip's packets are read (bytes per bus transfer 4..512, pause between transfers in microseconds, bus clock divider 0 = unchanged).
// Change it, run rxtest again, compare: finds the setting that gets whole packets through.
static u32 parse_num(const char **s) { u32 v = 0; while (**s == ' ') (*s)++; while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; } return v; }
static int wifi_rdcfg(const char *arg) {
    const char *s = arg;
    if (*s) {
        u32 c = parse_num(&s); if (c >= 4 && c <= 512) rd_chunk = c & ~3u;
        while (*s == ' ') s++;
        if (*s) { rd_gap_us = parse_num(&s); while (*s == ' ') s++; if (*s) rd_div = parse_num(&s); }
    }
    puts("read settings: chunk "); put_dec(rd_chunk); puts(" gap "); put_dec(rd_gap_us); puts(" div "); put_dec(rd_div); putc('\\n');
    sum_s("chunk "); sum_u(rd_chunk); sum_s(" gap "); sum_u(rd_gap_us); sum_s(" div "); sum_u(rd_div);
    return 1;
}
""")
edit('shell.h', [
("""    else if (streq(line, "rxtest")) wifi_rxtest();""", """    else if (streq(line, "rxtest")) wifi_rxtest();
    else if (streq(line, "rdcfg")) wifi_rdcfg(arg);"""),
("""streq(name, "rxtest") || streq(name, "up")""", """streq(name, "rxtest") || streq(name, "rdcfg") || streq(name, "up")"""),
("""commands: c f j k r up upset ping rxtest""", """commands: c f j k r up upset ping rxtest rdcfg"""),
])
edit('main.c', [("wave-os v1.53.2", "wave-os v1.53.3")])
print('ok')
