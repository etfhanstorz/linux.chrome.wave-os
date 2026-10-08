// WPA2-PSK connection (v1.53): password -> key, the 4-way handshake with the router, key install, then the network stack
// (DHCP, ARP, ping, UDP, TCP, update) runs over the Wi-Fi data ports.
//
// The password is typed on the Chromebook's own keyboard, shown only as '*', kept in RAM only, and never written to the log.

static char wifi_pw[64]; static u32 wifi_pw_len;          // the typed password: only alive between typing it and deriving the key
static char pmk_ssid[33];          // the derived key (PMK) is kept for this network until a handshake fails
static u8 ptk[48], anonce[32], snonce[32], gtk[16], gtk_id, ap_ver;
static u32 wifi_up;                                     // 1 = connected (keys in, address from DHCP): the idle service keeps it that way
static u8 last_replay[8];                               // the router's last key-message counter (a refresh must count higher)
static u32 wifi_rekeys, wifi_drops, wifi_link_lost;
static void wifi_eapol_rx(const u8 *f, u32 fl);                // (pmk, pmk_valid: update.h, so they can ride over an update)

// ---- the password prompt ----
static int wifi_read_password(void) {
    puts("WPA2 password for "); puts(target.ssid); puts(" (shown as *, Enter to finish): ");
    wifi_pw_len = 0;
    for (;;) {
        int c; while ((c = kb_getc()) < 0) wdt_kick();
        if (c == '\r' || c == '\n') break;
        if (c == 0x7f || c == '\b') { if (wifi_pw_len) { wifi_pw_len--; putc('\b'); } continue; }
        if (c >= 32 && c < 127 && wifi_pw_len < 63) { wifi_pw[wifi_pw_len++] = (char)c; putc('*'); }
    }
    putc('\n');
    wifi_pw[wifi_pw_len] = 0;
    return wifi_pw_len >= 8;
}

// ---- sending a frame: SDIO header {length, type 0} + 20-byte tx descriptor + Ethernet II frame, written to a free data port ----
static u8 txbuf[2312 + 256 + 256];
static int wifi_data_tx(const u8 *frame, u32 len) {
    if (len + 24 > 2312) return -1;
    u64 hz = tick_hz(), t0 = ticks(); u32 p = 0, bm;
    for (;;) {
        if (fn1_rd_u32(0x08, &bm) < 0) return -1;                  // download bitmap: bit p = data port p can take a packet
        p = wr_cur_port & 31;
        if ((bm >> p) & 1) break;
        if (bm && hz && ticks() - t0 > hz / 10) { u32 q = 0; while (!((bm >> q) & 1)) q++; p = q; break; }   // out of step with the chip: take any free port
        if (hz && ticks() - t0 > hz / 2) return -2;
        delay_us(200);
    }
    u32 total = 4 + 20 + len, blocks = (total + 255) / 256;
    for (u32 i = 0; i < blocks * 256; i++) txbuf[i] = 0;
    put16(txbuf, total); put16(txbuf + 2, 0);                       // SDIO header: length, type = data
    put16(txbuf + 4 + 2, len); put16(txbuf + 4 + 4, 20);            // tx descriptor: frame length, offset of the frame from the descriptor (everything else 0)
    for (u32 i = 0; i < len; i++) txbuf[24 + i] = frame[i];
    wr_cur_port = (p + 1) & 31;
    return sdio_write_port(DATA_PORT + p, txbuf, blocks) ? -3 : 0;
}

// ---- the network card the net stack uses ----
static int wifi_nic_send(const u8 *f, u32 len) { return wifi_data_tx(f, len); }
static int wifi_nic_recv(u8 *out, u32 max) {
    for (int i = 0; i < 4; i++) {
        u8 *f; u32 fl;
        if (wifi_data_poll(&f, &fl) != 1) return 0;
        if (f[12] == 0x88 && f[13] == 0x8e) { wifi_eapol_rx(f, fl); continue; }   // a key refresh from the router (v1.6-001)
        if (fl > max) continue;
        mcopy(out, f, fl);
        return (int)fl;
    }
    return 0;
}

// ---- random nonce. Weak on purpose: only timer readings go in (fine for a hobby OS on a home network; replace with a real source later). ----
static void wifi_random(u8 *out) {
    static u32 ctr; u8 seed[40]; u64 t = ticks();
    for (u32 i = 0; i < 8; i++) seed[i] = (u8)(t >> (8 * i));
    ctr++; for (u32 i = 0; i < 4; i++) seed[8 + i] = (u8)(ctr >> (8 * i));
    for (u32 i = 0; i < 6; i++) seed[12 + i] = wifi_mac[i];
    t = ticks(); for (u32 i = 0; i < 8; i++) seed[18 + i] = (u8)(t >> (8 * i));
    for (u32 i = 26; i < 40; i++) seed[i] = (u8)(i * 37 + ctr);
    sha256(seed, 40, out);
}

// ---- EAPOL-Key frames: 4-byte EAPOL header + 95-byte key descriptor + key data ----
// key info bits: 0x008 pairwise, 0x040 install, 0x080 ack, 0x100 MIC, 0x200 secure, 0x1000 encrypted key data, low 3 bits = 2 (HMAC-SHA1 + AES)
static u32 eapol_key_build(u8 *out, u32 info, const u8 *replay8, const u8 *nonce, const u8 *data, u32 dlen) {
    u32 n = 4 + 95 + dlen;
    mset(out, 0, n);
    out[0] = ap_ver; out[1] = 3; be16w(out + 2, 95 + dlen);
    u8 *k = out + 4;
    k[0] = 2; be16w(k + 1, info);                                   // descriptor type 2 (RSN), key info; key length stays 0 in messages 2 and 4
    mcopy(k + 5, replay8, 8);
    if (nonce) mcopy(k + 13, nonce, 32);
    be16w(k + 93, dlen);
    if (dlen) mcopy(k + 95, data, dlen);
    if (info & 0x100) { u8 h[20]; hmac_sha1(ptk, 16, out, n, h); mcopy(k + 77, h, 16); }   // MIC with the KCK (first 16 bytes of the PTK)
    return n;
}
static int eapol_send(const u8 *eap, u32 n) {
    static u8 fr[14 + 99 + 64];
    mcopy(fr, target.bssid, 6); mcopy(fr + 6, wifi_mac, 6); be16w(fr + 12, 0x888e);
    mcopy(fr + 14, eap, n);
    return wifi_data_tx(fr, 14 + n);
}
// Wait for an EAPOL-Key frame from the router; returns a pointer to the EAPOL header (inside dbuf) or 0.
static const u8 *eapol_wait(u32 ms, u32 *elen) {
    u64 hz = tick_hz(), t0 = ticks();
    while (hz && ticks() - t0 < hz / 1000 * ms) {
        u8 *f; u32 fl;
        int g = wifi_data_poll(&f, &fl);
        if (g == 1) {
            if (f[12] == 0x88 && f[13] == 0x8e && fl >= 14 + 99 && f[15] == 3 && f[18] == 2) { *elen = fl - 14; return f + 14; }
        } else if (g == 0) { if (!wifi_event_drain()) delay_us(500); }
        wdt_kick();
    }
    return 0;
}

// ---- telling the chip the keys (command 0x005e, "key material v2"; layout and flags from Linux mwifiex sta_cmd.c) ----
static int wifi_set_key(u32 idx, int pairwise, const u8 *key16, int first_key) {
    static u8 b[2 + 4 + 52];
    mset(b, 0, sizeof b);
    put16(b, 1);                                                    // action: set
    put16(b + 2, 0x019c); put16(b + 4, 52);                         // key parameter TLV (fixed size: mac, index, type, info, pn[8], key length, key[32])
    u8 *t = b + 6;
    if (pairwise) mcopy(t, target.bssid, 6); else mset(t, 0xff, 6);
    t[6] = (u8)idx; t[7] = 2;                                       // key index, type AES (CCMP)
    u32 info = pairwise ? (0x04 | 0x02 | 0x10 | 0x20) : (0x04 | 0x01 | 0x20);   // enabled + unicast/multicast + tx/rx
    if (first_key) info |= 0x08;                                    // "default" while no group key is installed yet (Linux rule)
    put16(t + 8, info);
    put16(t + 10 + 8, 16);                                          // pn[8] stays 0, then the key length
    mcopy(t + 10 + 10, key16, 16);
    u8 r[16]; u32 n = 0;
    int rc = wifi_cmd(0x005e, b, sizeof b, r, sizeof r, &n);
    return wifi_cmd_err("KEY_MATERIAL", rc, 0);
}

// ---- the 4-way handshake ----
static int wifi_handshake(void) {
    u32 el; const u8 *e = eapol_wait(4000, &el);
    if (!e) { errs("WIFI", 16, 1, "the router never started the password handshake"); sum_s("no msg1"); return 0; }
    const u8 *k = e + 4; u32 info = be16r(k + 1);
    if ((info & 0x188) != 0x088 || (info & 7) != 2) { errs("WIFI", 16, 1, "the router's first handshake message was not what we expected"); sum_s("bad msg1 "); sum_u(info); return 0; }
    ap_ver = e[0];
    u8 replay[8]; mcopy(replay, k + 5, 8); mcopy(anonce, k + 13, 32);
    wifi_random(snonce);
    wpa_ptk(pmk, target.bssid, wifi_mac, anonce, snonce, ptk);
    static u8 out[99 + 32];
    u32 n = eapol_key_build(out, 0x010a, replay, snonce, our_rsn, 22);                     // message 2: our nonce + our RSN element, signed
    if (eapol_send(out, n)) { errs("WIFI", 16, 6, "could not send a handshake message"); sum_s("tx failed"); return 0; }
    // message 3 (the router may repeat message 1 first if it did not like message 2 or never heard it)
    u64 hz = tick_hz(), t0 = ticks(); int tries = 0;
    for (;;) {
        u64 spent = ticks() - t0;
        if (!hz || spent > hz * 6) { errs("WIFI", 16, 2, "the router sent no handshake message 3: the password is probably wrong"); sum_s("no msg3 (wrong password?)"); pmk_valid = 0; return 0; }
        e = eapol_wait(1000, &el);
        if (!e) continue;
        k = e + 4; info = be16r(k + 1);
        if ((info & 0x188) == 0x088) {                                                      // message 1 again: answer it again
            mcopy(replay, k + 5, 8);
            if (tries++ < 4) { n = eapol_key_build(out, 0x010a, replay, snonce, our_rsn, 22); eapol_send(out, n); }
            continue;
        }
        if ((info & 0x1fc8) != 0x13c8) continue;                                           // not message 3
        break;
    }
    u32 dlen = be16r(k + 93);
    if (el < 99 + dlen || dlen > 256 || (dlen & 7) || dlen < 24) { errs("WIFI", 16, 3, "handshake message 3 was malformed"); sum_s("bad msg3"); return 0; }
    static u8 copy[99 + 256]; mcopy(copy, e, 99 + dlen); mset(copy + 4 + 77, 0, 16);
    u8 h[20]; hmac_sha1(ptk, 16, copy, 99 + dlen, h);
    if (!meq(h, k + 77, 16)) { errs("WIFI", 16, 3, "handshake message 3 failed its signature check: the password is probably wrong"); sum_s("msg3 mic bad"); pmk_valid = 0; return 0; }
    if (!meq(anonce, k + 13, 32)) { errs("WIFI", 16, 3, "the router changed its nonce during the handshake"); sum_s("nonce changed"); return 0; }
    mcopy(replay, k + 5, 8); mcopy(last_replay, replay, 8);
    static u8 kd[256];
    if (aes_unwrap(ptk + 16, k + 95, dlen / 8 - 1, kd)) { errs("WIFI", 16, 4, "could not decrypt the group key in message 3"); sum_s("unwrap failed"); return 0; }
    u32 kdl = dlen - 8, got_gtk = 0;
    for (u32 i = 0; i + 2 <= kdl;) {                                                        // key data = elements; the group key is a vendor element 00-0f-ac:1
        u32 id = kd[i], l = kd[i + 1];
        if (id == 0) break;
        if (i + 2 + l > kdl) break;
        if (id == 0xdd && l >= 6 + 16 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0f && kd[i + 4] == 0xac && kd[i + 5] == 1) { gtk_id = kd[i + 6] & 3; mcopy(gtk, kd + i + 8, 16); got_gtk = 1; }
        i += 2 + l;
    }
    if (!got_gtk) { errs("WIFI", 16, 4, "message 3 carried no group key"); sum_s("no gtk"); return 0; }
    n = eapol_key_build(out, 0x030a, replay, 0, 0, 0);                                      // message 4: done, signed
    if (eapol_send(out, n)) { errs("WIFI", 16, 6, "could not send a handshake message"); sum_s("tx failed"); return 0; }
    delay_us(20000);                                                                        // let message 4 leave before the chip starts encrypting
    if (!wifi_set_key(0, 1, ptk + 32, 1)) { errs("WIFI", 16, 5, "the chip refused the encryption key"); sum_s("ptk rejected"); return 0; }
    if (!wifi_set_key(gtk_id, 0, gtk, 1)) { errs("WIFI", 16, 5, "the chip refused the group key"); sum_s("gtk rejected"); return 0; }
    return 1;
}

// wificonnect NAME: find, password, join, handshake, keys, DHCP.
static int wifi_connect(const char *name) {
    joined = 0; rd_cur_port = wr_cur_port = 0; wifi_up = 0;
    int found = wifi_find(name);
    sum_n = 0; sum_res[0] = 0;
    if (!found) { errs("WIFI", 15, 6, "the network was not found: cannot join"); sum_s("not found"); return 0; }
    u32 sl = 0; while (target.ssid[sl]) sl++;
    if (pmk_valid && pmk_any) { for (u32 i = 0; i <= sl && i < sizeof pmk_ssid; i++) pmk_ssid[i] = target.ssid[i]; pmk_any = 0; }   // a key that came over an update: it belongs to the network we just found
    if (!(pmk_valid && streq(pmk_ssid, target.ssid))) {              // no key for this network yet: ask for the password
        if (!wifi_read_password()) { for (u32 i = 0; i < sizeof wifi_pw; i++) wifi_pw[i] = 0; wifi_pw_len = 0; errs("WIFI", 16, 7, "the password must be 8 to 63 characters"); sum_s("password length"); return 0; }
        puts("computing the key from the password (takes a few seconds)...\n");
        u64 t0 = ticks();
        pbkdf2_wpa(wifi_pw, wifi_pw_len, (const u8 *)target.ssid, sl, pmk);
        prof_stop("pbkdf2", t0);
        for (u32 i = 0; i < sizeof wifi_pw; i++) wifi_pw[i] = 0;      // the password text is gone; only the derived key stays
        wifi_pw_len = 0; pmk_valid = 1;
        for (u32 i = 0; i <= sl && i < sizeof pmk_ssid; i++) pmk_ssid[i] = target.ssid[i];
    }
    if (!wifi_assoc()) return 0;
    sum_n = 0; sum_res[0] = 0;
    if (!wifi_handshake()) return 0;
    sum_n = 0; sum_res[0] = 0;
    mcopy(net_mac, wifi_mac, 6);
    nic_send = wifi_nic_send; nic_recv = wifi_nic_recv;
    if (!dhcp_run()) { errs("WIFI", 17, 1, "connected, but the router gave no network address (DHCP)"); sum_s("no dhcp"); return 0; }
    wifi_up = 1;
    sum_s("up "); sum_u(net_ip >> 24); sum_c('.'); sum_u(net_ip >> 16 & 255); sum_c('.'); sum_u(net_ip >> 8 & 255); sum_c('.'); sum_u(net_ip & 255);
    return 1;
}

// ping [ADDRESS]: default = the router
static int wifi_ping(const char *arg) {
    if (!net_ip) { errs("NET", 1, 1, "not connected: run k (wificonnect) first"); sum_s("not connected"); return 0; }
    u32 dst = net_gw;
    if (*arg && !parse_ip(arg, &dst)) { sum_s("bad address"); return 0; }
    u32 ms = 0; int r = net_ping(dst, &ms);
    if (r) { errs("NET", 1, 2, "no ping answer"); sum_s(r == -1 ? "no route" : "no answer"); return 0; }
    sum_s("reply "); sum_u(ms); sum_s(" ms");
    return 1;
}

// rxtest: for 40 s, measure pings sent to us. On the PC:  ping -n 10 -l 1400 <this address>   (try other -l sizes: 200, 800, 1200, 1472)
static int wifi_rxtest(void) {
    if (!net_ip) { errs("NET", 1, 1, "not connected: run k (wificonnect) first"); sum_s("not connected"); return 0; }
    rxt_n = 0; rxt_bad_ip = 0; net_bad_ip = net_bad_l4 = 0; rxtest_on = 1;
    puts("rxtest: on the PC run  ping -n 10 -l 1400 "); put_ip(net_ip); puts("   (listening 40 s)\n");
    u64 hz = tick_hz(), t0 = ticks(), last = 0; u32 seen = 0;
    while (hz && ticks() - t0 < hz * 40) {
        net_poll(); wdt_kick();
        u32 now = 0; for (u32 k = 0; k < rxt_n; k++) now += rxt[k].ok + rxt[k].bad;
        if (now != seen) { seen = now; last = ticks(); }
        if (seen && ticks() - last > hz * 4) break;                 // pings stopped: finish early
    }
    rxtest_on = 0;
    u32 tot_ok = 0, tot_bad = 0;
    for (u32 k = 0; k < rxt_n; k++) {
        puts("  size "); put_dec(rxt[k].len); puts(": ok "); put_dec(rxt[k].ok); puts(" bad "); put_dec(rxt[k].bad);
        if (rxt[k].bad) {
            puts("  first wrong byte at "); put_dec(rxt[k].first_min); puts(", up to "); put_dec(rxt[k].nbad_max); puts(" wrong, "); put_dec(rxt[k].zeros); puts(" of them 0; got:");
            for (u32 i = 0; i < 8; i++) { putc(' '); putc("0123456789abcdef"[rxt[k].got[i] >> 4]); putc("0123456789abcdef"[rxt[k].got[i] & 15]); }
        }
        putc('\n'); tot_ok += rxt[k].ok; tot_bad += rxt[k].bad;
    }
    puts("  last packet: chip length "); put_dec(rx_last_len); puts(", frame offset "); put_dec(rx_last_off); puts(";  read settings: chunk "); put_dec(rd_chunk); puts(" gap "); put_dec(rd_gap_us); puts(" div "); put_dec(rd_div); putc('\n');
    sum_s("ok "); sum_u(tot_ok); sum_s(" bad "); sum_u(tot_bad);
    if (rxt_bad_ip) { sum_s(" iphdr-bad "); sum_u(rxt_bad_ip); }
    return 1;
}
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
    puts("read settings: chunk "); put_dec(rd_chunk); puts(" gap "); put_dec(rd_gap_us); puts(" div "); put_dec(rd_div); putc('\n');
    sum_s("chunk "); sum_u(rd_chunk); sum_s(" gap "); sum_u(rd_gap_us); sum_s(" div "); sum_u(rd_div);
    return 1;
}

// rdmode [MODE [DTOC]]: how a packet is read from the chip. 1 = pieces of up to 512 bytes (only works for packets up to ~512 bytes: a second read
// restarts the packet), 2 = the whole packet in ONE multi-block transfer (what Linux does; DTOC = data timeout counter 1..255 for it), 0 = 256-byte blocks.
static int wifi_rdmode(const char *arg) {
    const char *s = arg;
    if (*s) { u32 m = parse_num(&s); data_read_mode = m > 2 ? 2 : (int)m; while (*s == ' ') s++; if (*s) rd_dtoc = parse_num(&s) & 255; }
    puts("data read mode "); put_dec((u32)data_read_mode); puts(", data timeout "); put_dec(rd_dtoc); putc('\n');
    sum_s("mode "); sum_u((u32)data_read_mode); sum_s(" dtoc "); sum_u(rd_dtoc);
    return 1;
}
// mss [N]: the largest TCP payload we ask senders for (applies to the next connection)
static int wifi_mss(const char *arg) {
    const char *s = arg;
    if (*s) { u32 m = parse_num(&s); if (m >= 100 && m <= 1460) tcp_mss = m; }
    puts("tcp mss "); put_dec(tcp_mss); putc('\n');
    sum_s("mss "); sum_u(tcp_mss);
    return 1;
}

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
    int old_mode = data_read_mode; u32 old_dtoc = rd_dtoc, old_div = rd_div;
    u32 good_cfgs = 0, first_good = 99;
    puts("rdsweep: ping "); put_ip(dst); puts(" with 300/800/1400 bytes under each read setting  (. ok  X damaged  - no reply)\n");
    for (u32 c = 0; c < sizeof cfg / sizeof cfg[0]; c++) {
        data_read_mode = cfg[c].mode; rd_dtoc = cfg[c].dtoc; rd_div = cfg[c].div;
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
        putc('\n');
        if (all_ok) { good_cfgs++; if (first_good == 99) first_good = c; }
    }
    data_read_mode = old_mode; rd_dtoc = old_dtoc; rd_div = old_div;
    sum_u(good_cfgs); sum_s(" settings work");
    if (first_good != 99) { sum_s(", first: #"); sum_u(first_good); }
    return 1;
}

// dns NAME: look a name up (v1.6)
static int wifi_dns(const char *arg) {
    if (!*arg) { sum_s("usage: dns NAME"); return 0; }
    u32 ip = 0; int r = dns_lookup(arg, &ip);
    if (r == -1) { errs("NET", 30, 1, "no DNS server: connect first (k)"); sum_s("no network"); return 0; }
    if (r == -2) { errs("NET", 30, 2, "that name does not exist"); sum_s("no such name"); return 0; }
    if (r == -4) { errs("NET", 30, 4, "not a valid name"); sum_s("bad name"); return 0; }
    if (r) { errs("NET", 30, 3, "the DNS server did not answer"); sum_s("no answer"); return 0; }
    sum_u(ip >> 24); sum_c('.'); sum_u(ip >> 16 & 255); sum_c('.'); sum_u(ip >> 8 & 255); sum_c('.'); sum_u(ip & 255);
    return 1;
}
// ---- staying connected (v1.6-001) ----
// Routers refresh the shared group key every so often (often hourly) with a two-message exchange; a station that does not answer is
// dropped. wave-os answers it here, and if the chip reports the link lost (or the router starts a whole new handshake) it reconnects
// with the saved key, without asking for the password. wifi_service() runs whenever the shell or the browser is waiting for a key.
static int replay_newer(const u8 *a, const u8 *b) { for (u32 i = 0; i < 8; i++) if (a[i] != b[i]) return a[i] > b[i]; return 0; }
static void log_quiet(const char *s) { u32 k = con_on; con_on = 0; puts(s); con_on = k; }
static void wifi_eapol_rx(const u8 *f, u32 fl) {
    if (!wifi_up || fl < 14 + 99 || f[15] != 3 || f[18] != 2) return;
    const u8 *e = f + 14, *k = e + 4;
    u32 info = be16r(k + 1), dlen = be16r(k + 93);
    if (info & 0x08) {                                                          // pairwise message 1: the router wants a whole new handshake
        if (info & 0x80) { log_quiet("wifi: the router restarted the handshake: reconnecting\n"); wifi_link_lost = 1; }
        return;
    }
    if ((info & 0x1380) != 0x1380 || (info & 7) != 2) return;                     // not a group key message 1
    if (!replay_newer(k + 5, last_replay)) return;                               // old or repeated: ignore
    if (fl < 14 + 99 + dlen || dlen < 24 || dlen > 256 || (dlen & 7)) return;
    static u8 copy[99 + 256]; mcopy(copy, e, 99 + dlen); mset(copy + 4 + 77, 0, 16);
    u8 h[20]; hmac_sha1(ptk, 16, copy, 99 + dlen, h);
    if (!meq(h, k + 77, 16)) { log_quiet("wifi: group key message with a bad signature: ignored\n"); return; }
    static u8 kd[256];
    if (aes_unwrap(ptk + 16, k + 95, dlen / 8 - 1, kd)) { log_quiet("wifi: could not decrypt the new group key\n"); return; }
    u32 got = 0, kdl = dlen - 8;
    for (u32 i = 0; i + 2 <= kdl;) {
        u32 id = kd[i], l = kd[i + 1];
        if (id == 0 || i + 2 + l > kdl) break;
        if (id == 0xdd && l >= 6 + 16 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0f && kd[i + 4] == 0xac && kd[i + 5] == 1) { gtk_id = kd[i + 6] & 3; mcopy(gtk, kd + i + 8, 16); got = 1; }
        i += 2 + l;
    }
    if (!got) return;
    u8 replay[8]; mcopy(replay, k + 5, 8); mcopy(last_replay, replay, 8);
    static u8 out[99]; u32 n = eapol_key_build(out, 0x0302, replay, 0, 0, 0);   // group message 2: done, signed
    eapol_send(out, n);
    u32 keep = con_on; con_on = 0;
    if (wifi_set_key(gtk_id, 0, gtk, 0)) { wifi_rekeys++; puts("wifi: the router refreshed the group key: done\n"); }
    con_on = keep;
}
// Returns 1 if it printed something on the screen (the shell then shows its prompt again).
static u64 wifi_svc_next;
static int wifi_service(void) {
    if (!wifi_up || !nic_recv) return 0;
    u64 hz = tick_hz(), now = ticks();
    if (hz && now < wifi_svc_next) return 0;
    wifi_svc_next = now + hz / 50;                                              // every 20 ms
    net_poll();                                                                 // answers ARP and pings, handles key refreshes
    u32 ev = wifi_event_drain();
    if (ev == 0x0003 || ev == 0x0008 || ev == 0x0009) { log_quiet(ev == 3 ? "wifi: the chip lost the link\n" : "wifi: the router disconnected us\n"); wifi_link_lost = 1; }
    if (!wifi_link_lost) return 0;
    wifi_link_lost = 0; wifi_up = 0; wifi_drops++;
    if (!pmk_valid) { puts("\n(Wi-Fi dropped: type k to reconnect)\n"); return 1; }
    puts("\n(Wi-Fi dropped: reconnecting...)\n");
    u32 keep = con_on; con_on = 0;
    int ok = wifi_connect(pmk_ssid[0] ? pmk_ssid : WIFI_DEFAULT_SSID);
    con_on = keep;
    if (ok) { puts("(Wi-Fi back: "); put_ip(net_ip); puts(")\n"); log_ship(); }
    else puts("(Wi-Fi reconnect failed: type k)\n");
    return 1;
}
