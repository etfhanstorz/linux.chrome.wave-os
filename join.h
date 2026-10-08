// Joining a Wi-Fi network (v1.52). Step 1 needs no password: associate (the router accepts anybody to the point where it starts the
// password handshake), then listen on the chip's data ports. If the router's first handshake packet (EAPOL, ethertype 0x888e)
// arrives, the whole receive path works and only the handshake itself is left.
//
// Formats come from Linux mwifiex (join.c, sdio.c, sta_tx.c, sta_rx.c, fw.h).

#define DATA_PORT 0x10000                 // data port p lives at 0x10000 + p (new mode), p = 0..31
static u8 dbuf[2312 + 256 + 256];         // one received data packet (SDIO header + rx descriptor + frame)
static u32 rd_cur_port, wr_cur_port;      // next data port to read / write (rolling, like Linux curr_rd_port / curr_wr_port)
static u32 rx_pkts, rx_eapol, rx_other, rx_errs, rx_events;
static u32 rx_last_off, rx_last_len;      // rx descriptor offset and SDIO length of the last data packet (rxtest prints them)
static u16 rx_first_type; static u8 rx_first_info[32]; static u32 rx_first_len;
static int joined; static u8 join_aid;
static u8 our_rsn[22];                  // the RSN element we sent when associating (the handshake repeats it in message 2)

// How data packets are read from the chip. Measured on the real hana (v1.53.6 rdsweep): mode 2 = the whole packet in ONE multi-block transfer
// is the only mode that returns big packets intact (mode 1 = pieces of 512 bytes: every extra read restarts the packet, bytes after ~512 are wrong;
// mode 0 = 256-byte blocks: wrong from byte ~120). Command-port reads keep using wifi_read_bytes (mode 1 works for those small packets).
static int data_read_mode = 2;

static int fn1_rd_u32(u32 reg, u32 *v) {   // four consecutive 8-bit registers, little endian
    u32 r = 0;
    for (u32 i = 0; i < 4; i++) { int b = fn1_rd(reg + i); if (b < 0) return -1; r |= (u32)b << (8 * i); }
    *v = r;
    return 0;
}

// Fetch one data packet if the chip has one. On success returns 1 and points *frame at an Ethernet II frame (dst, src, type, payload)
// of *flen bytes inside dbuf. Returns 0 if nothing waits, -1 on a read error.
static int wifi_data_poll(u8 **frame, u32 *flen) {
    u32 bm;
    if (fn1_rd_u32(0x04, &bm) < 0) return -1;                    // upload (receive) ready bitmap: one bit per port
    if (!bm) return 0;
    u32 p = rd_cur_port & 31;
    if (!((bm >> p) & 1)) { u32 q = 0; while (q < 32 && !((bm >> q) & 1)) q++; p = q & 31; }   // out of step with the chip: take the lowest port that has data
    int l0 = fn1_rd(0x0c + 2 * p), l1 = fn1_rd(0x0d + 2 * p);    // length of the packet on this port
    if (l0 < 0 || l1 < 0) return -1;
    u32 len = ((u32)l1 << 8) | (u32)l0;
    rd_cur_port = (p + 1) & 31;
    if (len < 24 || len > 2312) { rx_errs++; return -1; }
    u32 blocks = (len + 255) / 256;
    int saved_mode = wifi_read_bytes; wifi_read_bytes = data_read_mode;
    int rerr = sdio_read_port(DATA_PORT + p, dbuf, blocks);
    wifi_read_bytes = saved_mode;
    if (rerr) { rx_errs++; return -1; }
    if (get16(dbuf + 2) != 0) { rx_other++; return 0; }          // not a data packet
    u32 pkt_len = get16(dbuf + 4 + 2), off = get16(dbuf + 4 + 4);   // rx descriptor: frame length, offset of the frame from the descriptor
    if (off < 16 || off > 200 || 4 + off + 14 > len) { rx_errs++; return -1; }
    rx_last_off = off; rx_last_len = len;
    u8 *f = dbuf + 4 + off;
    u32 fl = pkt_len;
    if (4 + off + fl > len) fl = len - 4 - off;
    // The firmware hands over 802.3 + LLC/SNAP (dst, src, length, AA AA 03 00 00 00, ethertype); rebuild Ethernet II in place.
    if (fl >= 22 && f[14] == 0xaa && f[15] == 0xaa && f[16] == 0x03 && f[17] == 0 && f[18] == 0 && f[19] == 0) {
        for (int i = 11; i >= 0; i--) f[8 + i] = f[i];
        f += 8; fl -= 8;
    }
    *frame = f; *flen = fl;
    return 1;
}

// Read and discard one command-port packet if one waits (association and link events). Returns the event id, or 0.
static u32 wifi_event_drain(void) {
    int st = fn1_rd(0x03);
    if (!cmd_packet_waiting(st)) return 0;
    int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);
    u32 rx = ((u32)(l1 < 0 ? 0 : l1) << 8) | (u32)(l0 < 0 ? 0 : l0), blocks = (rx + 255) / 256;
    if (rx <= 4 || blocks * 256 > sizeof wbuf || sdio_read_port(WCMD_PORT, wbuf, blocks)) return 0;
    after_packet_read(rx);
    return get16(wbuf + 2) == 3 ? get16(wbuf + 4) : 0xffff;
}

// Does the router's RSN (WPA2) element offer what we can do: CCMP + PSK, no mandatory management-frame protection?
// Returns 0 = fine, 1 = no RSN element, 2 = no CCMP, 3 = no PSK, 4 = protected management frames required. group[4] gets the router's group cipher.
static int rsn_check(const u8 *ie, u32 len, u8 *group) {
    if (len < 22 || ie[0] != 48 || ie[1] + 2u > len) return 1;
    const u8 *p = ie + 2; u32 left = ie[1];
    if (left < 8) return 1;
    p += 2; left -= 2;                                            // version
    for (u32 i = 0; i < 4; i++) group[i] = p[i];
    p += 4; left -= 4;
    u32 pc = get16(p); p += 2; left -= 2;
    int ccmp = 0, psk = 0;
    for (u32 k = 0; k < pc; k++) { if (left < 4) return 1; if (p[0] == 0x00 && p[1] == 0x0f && p[2] == 0xac && p[3] == 4) ccmp = 1; p += 4; left -= 4; }
    if (left < 2) return 1;
    u32 ac = get16(p); p += 2; left -= 2;
    for (u32 k = 0; k < ac; k++) { if (left < 4) return 1; if (p[0] == 0x00 && p[1] == 0x0f && p[2] == 0xac && p[3] == 2) psk = 1; p += 4; left -= 4; }
    u32 caps = left >= 2 ? get16(p) : 0;
    if (!ccmp) return 2;
    if (!psk) return 3;
    if (caps & 0x40) return 4;                                    // MFPR: the router insists on protected management frames
    return 0;
}

static int wifi_assoc(void) {
    static u8 body[160];
    u8 group[4]; int why = rsn_check(target.rsn, target.rsn_len, group);
    if (why) {
        errs("WIFI", 15, why + 1, why == 1 ? "the network has no WPA2 (RSN) element" : why == 2 ? "the network does not offer CCMP encryption" : why == 3 ? "the network does not offer a WPA2 password (PSK) login" : "the network requires protected management frames (not supported yet)");
        sum_s("no wpa2-psk "); sum_u((u32)why); return 0;
    }
    u32 p = 0;
    for (u32 i = 0; i < 6; i++) body[p++] = target.bssid[i];
    put16(body + p, target.cap & ~((1u << 15) | (1u << 14) | (1u << 12) | (1u << 11) | (1u << 9))); p += 2;   // capabilities (Linux CAPINFO_MASK)
    put16(body + p, 10); p += 2;                                  // listen interval
    put16(body + p, target.bint); p += 2;                         // beacon interval
    body[p++] = 0;                                                // DTIM period
    u32 sl = 0; while (target.ssid[sl]) sl++;
    put16(body + p, 0); put16(body + p + 2, sl); p += 4;          // SSID
    for (u32 i = 0; i < sl; i++) body[p++] = (u8)target.ssid[i];
    put16(body + p, 3); put16(body + p + 2, 1); body[p + 4] = target.chan; p += 5;   // DS parameter set: the channel
    put16(body + p, 4); put16(body + p + 2, 6); for (u32 i = 0; i < 6; i++) body[p + 4 + i] = 0; p += 10;   // CF parameter set (empty)
    u32 nr = target.nrates > 12 ? 12 : target.nrates;
    put16(body + p, 1); put16(body + p + 2, nr); p += 4;          // supported rates, as the router lists them
    for (u32 i = 0; i < nr; i++) body[p++] = target.rates[i];
    put16(body + p, 0x011f); put16(body + p + 2, 2); put16(body + p + 4, 0); p += 6;   // authentication type: open system (WPA2 passwords come later, in the handshake)
    put16(body + p, 0x0101); put16(body + p + 2, 7); p += 4;      // channel list: just the router's channel
    body[p] = target.chan >= 36 ? 1 : 0; body[p + 1] = target.chan; body[p + 2] = 0; put16(body + p + 3, 0); put16(body + p + 5, 0); p += 7;
    u32 rs = p;
    put16(body + p, 48); put16(body + p + 2, 20); p += 4;         // our RSN element: WPA2, CCMP for both keys, PSK login, no extras
    body[p++] = 1; body[p++] = 0;
    for (u32 i = 0; i < 4; i++) body[p++] = group[i];             // group cipher: whatever the router uses
    body[p++] = 1; body[p++] = 0; body[p++] = 0x00; body[p++] = 0x0f; body[p++] = 0xac; body[p++] = 4;   // one pairwise cipher: CCMP
    body[p++] = 1; body[p++] = 0; body[p++] = 0x00; body[p++] = 0x0f; body[p++] = 0xac; body[p++] = 2;   // one login: PSK
    body[p++] = 0; body[p++] = 0;                                 // RSN capabilities
    our_rsn[0] = 48; our_rsn[1] = 20; for (u32 i = 0; i < 20; i++) our_rsn[2 + i] = body[rs + 4 + i];
    u8 r[32]; u32 n = 0;
    wifi_cmd_ms = 8000;
    int rc = wifi_cmd(0x0012, body, p, r, sizeof r, &n);
    wifi_cmd_ms = 1000;
    if (rc != 0) { wifi_cmd_err("ASSOCIATE", rc, 0); sum_s(rc > 0 ? "assoc rejected " : "assoc no answer "); if (rc > 0) sum_u((u32)rc); return 0; }
    u32 status = n >= 4 ? get16(r + 2) : 0xffff, aid = n >= 6 ? get16(r + 4) & 0x3fff : 0;
    if (status != 0) {
        errs("WIFI", 15, 1, "the router refused the connection (association status in the summary)");
        sum_s("assoc status "); sum_u(status); return 0;
    }
    joined = 1; join_aid = (u8)aid;
    sum_s("joined aid "); sum_u(aid);
    return 1;
}

// wifijoin NAME: find the network, associate, then listen for ~4 s and count what arrives on the data ports.
static int wifi_join(const char *name) {
    joined = 0; rx_pkts = rx_eapol = rx_other = rx_errs = rx_events = 0; rx_first_len = 0; rd_cur_port = wr_cur_port = 0;
    int found = wifi_find(name);
    sum_n = 0; sum_res[0] = 0;                                    // the join line replaces wififind's
    if (!found) { errs("WIFI", 15, 6, "the network was not found: cannot join"); sum_s("not found"); return 0; }
    if (!wifi_assoc()) return 0;
    u64 hz = tick_hz(), t0 = ticks();
    while (hz && ticks() - t0 < hz * 4) {
        u8 *f; u32 fl;
        int g = wifi_data_poll(&f, &fl);
        if (g == 1) {
            rx_pkts++;
            u32 type = (f[12] << 8) | f[13];
            if (type == 0x888e) rx_eapol++;
            if (rx_pkts == 1) { rx_first_type = (u16)type; rx_first_len = fl; for (u32 i = 0; i < 32; i++) rx_first_info[i] = i < fl ? f[i] : 0; }
        } else if (g == 0) {
            if (wifi_event_drain()) rx_events++;
            else delay_us(500);
        }
        wdt_kick();
    }
    sum_s(" rx"); sum_u(rx_pkts); sum_s(" eapol"); sum_u(rx_eapol);
    if (rx_events) { sum_s(" ev"); sum_u(rx_events); }
    if (rx_errs) { sum_s(" err"); sum_u(rx_errs); }
    if (wifi_verbose && rx_first_len) {
        puts("first packet: "); put_dec(rx_first_len); puts(" bytes, type "); put_hex(rx_first_type); puts(", begins:");
        for (u32 i = 0; i < 32; i++) { putc(' '); putc("0123456789abcdef"[rx_first_info[i] >> 4]); putc("0123456789abcdef"[rx_first_info[i] & 15]); }
        putc('\n');
    }
    return 1;
}
