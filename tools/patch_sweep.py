root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('net.h', [
("""static u32 icmp_reply_seen, icmp_reply_id, icmp_reply_seq, icmp_reply_ttl;""",
"""static u32 icmp_reply_seen, icmp_reply_id, icmp_reply_seq, icmp_reply_ttl;
static u8 icmp_reply_buf[1500]; static u32 icmp_reply_len, icmp_reply_csum_ok;       // the last ping reply as it arrived (kept even if damaged while rxtest_on)"""),
("""        if (rxtest_on && p[0] == 8) { rxtest_note(p, plen, ip_good); if (!ip_good) return; }
        else if (csum(p, plen, 0) != 0xffff) { net_bad_l4++; return; }""",
"""        if (rxtest_on && p[0] == 8) { rxtest_note(p, plen, ip_good); if (!ip_good) return; }
        else if (rxtest_on && p[0] == 0) {                                                  // a reply to our own big ping (pingbig / rdsweep): keep it even if damaged, the caller judges it
            icmp_reply_len = plen <= sizeof icmp_reply_buf ? plen : 0; if (icmp_reply_len) mcopy(icmp_reply_buf, p, plen);
            icmp_reply_csum_ok = csum(p, plen, 0) == 0xffff && ip_good; icmp_reply_seen = 1; return;
        }
        else if (csum(p, plen, 0) != 0xffff) { net_bad_l4++; return; }"""),
])

edit('shell.h', [
("""    else if (streq(line, "rdmode")) wifi_rdmode(arg);""",
"""    else if (streq(line, "rdmode")) wifi_rdmode(arg);
    else if (streq(line, "rdsweep")) wifi_rdsweep(arg);"""),
("""streq(name, "rdmode") ||""", """streq(name, "rdmode") || streq(name, "rdsweep") ||"""),
("""rdmode mss""", """rdmode rdsweep mss"""),
])

open(root + 'wpa.h', 'a', newline='').write("""
// ---- rdsweep: find a way to read whole packets. For each read setting, ping an address (default 8.8.8.8) with known data of three sizes,
// and check the reply byte by byte. One line per setting goes to the screen and the log: . = whole and right, X = damaged (with the first wrong byte), - = no reply. ----
static int ping_big_once(u32 dst, u32 size, u32 *first_bad) {
    static u8 e[1480];
    const u8 *dm = mac_for(dst);
    if (!dm || size + 8 > sizeof e) return -2;
    e[0] = 8; e[1] = 0; be16w(e + 2, 0); be16w(e + 4, 0x7762); be16w(e + 6, (u32)ticks() & 0xffff);
    for (u32 i = 0; i < size; i++) e[8 + i] = 'a' + i % 23;
    be16w(e + 2, ~csum(e, size + 8, 0) & 0xffff);
    icmp_reply_seen = 0; icmp_reply_len = 0; rxtest_on = 1;
    ip_send(dm, net_ip, dst, 1, e, size + 8);
    net_wait_ms(1500, (int *)&icmp_reply_seen);
    rxtest_on = 0;
    if (!icmp_reply_seen) return -1;
    if (icmp_reply_len != size + 8 || !icmp_reply_csum_ok) { *first_bad = 0xffff; }
    u32 bad = 0; *first_bad = 0xffffffff;
    for (u32 i = 0; i < size && i + 8 < icmp_reply_len; i++) if (icmp_reply_buf[8 + i] != 'a' + i % 23) { if (bad == 0) *first_bad = i; bad++; }
    return (bad || !icmp_reply_csum_ok || icmp_reply_len != size + 8) ? 1 : 0;
}
static int wifi_rdsweep(const char *arg) {
    if (!net_ip) { errs("NET", 1, 1, "not connected: run k (wificonnect) first"); sum_s("not connected"); return 0; }
    u32 dst = ip4(8, 8, 8, 8); if (*arg && !parse_ip(arg, &dst)) { sum_s("bad address"); return 0; }
    static const struct { u8 mode, dtoc, div; u8 chunk_pad; } cfg[] = { {1, 0, 0, 0}, {2, 0, 0, 0}, {2, 255, 0, 0}, {2, 64, 0, 0}, {2, 255, 8, 0}, {2, 255, 32, 0}, {0, 0, 0, 0} };
    static const u32 sizes[3] = {300, 800, 1400};
    int old_mode = wifi_read_bytes; u32 old_dtoc = rd_dtoc, old_div = rd_div;
    u32 good_cfgs = 0, first_good = 99;
    puts("rdsweep: ping "); put_ip(dst); puts(" with 300/800/1400 bytes under each read setting  (. ok  X damaged  - no reply)\\n");
    for (u32 c = 0; c < sizeof cfg / sizeof cfg[0]; c++) {
        wifi_read_bytes = cfg[c].mode; rd_dtoc = cfg[c].dtoc; rd_div = cfg[c].div;
        u32 e0 = rx_errs;
        puts("  mode "); put_dec(cfg[c].mode); puts(" dtoc "); put_dec(cfg[c].dtoc); puts(" div "); put_dec(cfg[c].div); puts(":");
        u32 all_ok = 1;
        for (u32 s = 0; s < 3; s++) {
            u32 fb = 0; int r = ping_big_once(dst, sizes[s], &fb);
            puts("  "); put_dec(sizes[s]); putc(' ');
            if (r == 0) putc('.'); else if (r == 1) { putc('X'); if (fb != 0xffffffff) { putc('@'); put_dec(fb); } all_ok = 0; } else { putc('-'); all_ok = 0; }
            wdt_kick();
        }
        if (rx_errs != e0) { puts("  (read errors: "); put_dec(rx_errs - e0); putc(')'); all_ok = 0; }
        putc('\\n');
        if (all_ok) { good_cfgs++; if (first_good == 99) first_good = c; }
    }
    wifi_read_bytes = old_mode; rd_dtoc = old_dtoc; rd_div = old_div;
    sum_u(good_cfgs); sum_s(" settings work");
    if (first_good != 99) { sum_s(", first: #"); sum_u(first_good); }
    return 1;
}
""")
edit('main.c', [("wave-os v1.53.5", "wave-os v1.53.6")])
print('ok')
