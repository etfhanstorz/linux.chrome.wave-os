// Network framework: Ethernet / ARP / IPv4 / ICMP / UDP / DHCP over a pluggable network card.
// The card is two functions (send a frame, receive a frame). The Wi-Fi chip's data path will plug in here;
// until then a virtual card in the fake Chromebook (fakehana/fakenet.py) stands in, so every layer above can be
// tested without radio hardware.

static u8 net_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
static u32 net_ip, net_mask, net_gw, net_dns, net_srv;       // IPv4 addresses (host order)
static u8 gw_mac[6];
static u32 gw_mac_known;
static int (*nic_send)(const u8 *frame, u32 len);
static int (*nic_recv)(u8 *frame, u32 max);                  // returns frame length, 0 = nothing waiting

static u8 frame_buf[1600];
static u8 rx_buf[1600];

static u32 be16r(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static void be16w(u8 *p, u32 v) { p[0] = v >> 8; p[1] = v; }
static u32 be32r(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void be32w(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 ip4(u32 a, u32 b, u32 c, u32 d) { return a << 24 | b << 16 | c << 8 | d; }
static void put_ip(u32 ip) { put_dec(ip >> 24); putc('.'); put_dec(ip >> 16 & 255); putc('.'); put_dec(ip >> 8 & 255); putc('.'); put_dec(ip & 255); }
static void mcopy(u8 *d, const u8 *s, u32 n) { for (u32 i = 0; i < n; i++) d[i] = s[i]; }
static void mset(u8 *d, u8 v, u32 n) { for (u32 i = 0; i < n; i++) d[i] = v; }
static int meq(const u8 *a, const u8 *b, u32 n) { for (u32 i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

static u32 csum(const u8 *p, u32 n, u32 sum) {
    for (u32 i = 0; i + 1 < n; i += 2) sum += (u32)p[i] << 8 | p[i + 1];
    if (n & 1) sum += (u32)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return sum;
}

// ---- last received packets of interest (filled by net_poll) ----
static u8 udp_in[1500]; static u32 udp_in_len, udp_in_port, udp_in_src, udp_in_got;      // last UDP payload for our listening port
static u32 udp_listen_port;
static u32 icmp_reply_seen, icmp_reply_id, icmp_reply_seq, icmp_reply_ttl;
static u8 dhcp_in[600]; static u32 dhcp_in_len, dhcp_in_got;

static int eth_send(const u8 *dst_mac, u32 ethertype, const u8 *payload, u32 plen) {
    if (!nic_send || plen + 14 > sizeof frame_buf) return -1;
    mcopy(frame_buf, dst_mac, 6); mcopy(frame_buf + 6, net_mac, 6); be16w(frame_buf + 12, ethertype);
    mcopy(frame_buf + 14, payload, plen);
    u32 len = 14 + plen; if (len < 60) { mset(frame_buf + len, 0, 60 - len); len = 60; }
    return nic_send(frame_buf, len);
}

static u32 ip_id;
static int ip_send(const u8 *dst_mac, u32 src, u32 dst, u32 proto, const u8 *payload, u32 plen) {
    static u8 pkt[1500];
    if (plen + 20 > sizeof pkt) return -1;
    pkt[0] = 0x45; pkt[1] = 0; be16w(pkt + 2, 20 + plen); be16w(pkt + 4, ++ip_id); be16w(pkt + 6, 0);
    pkt[8] = 64; pkt[9] = proto; be16w(pkt + 10, 0); be32w(pkt + 12, src); be32w(pkt + 16, dst);
    be16w(pkt + 10, ~csum(pkt, 20, 0) & 0xffff);
    mcopy(pkt + 20, payload, plen);
    return eth_send(dst_mac, 0x0800, pkt, 20 + plen);
}

static int udp_send_raw(const u8 *dst_mac, u32 src, u32 dst, u32 sport, u32 dport, const u8 *data, u32 len) {
    static u8 d[1480];
    if (len + 8 > sizeof d) return -1;
    be16w(d, sport); be16w(d + 2, dport); be16w(d + 4, 8 + len); be16w(d + 6, 0);            // UDP checksum 0 = none (legal over IPv4)
    mcopy(d + 8, data, len);
    return ip_send(dst_mac, src, dst, 17, d, 8 + len);
}

// ---- receive path: answers ARP requests and pings for us, remembers UDP/ICMP/DHCP replies ----
static void arp_reply(const u8 *rx) {
    u8 a[28];
    mcopy(a, rx + 14, 8); be16w(a + 6, 2);                                                  // reply
    mcopy(a + 8, net_mac, 6); be32w(a + 14, net_ip);
    mcopy(a + 18, rx + 14 + 8, 6); mcopy(a + 24, rx + 14 + 14, 4);
    eth_send(rx + 6, 0x0806, a, 28);
}

static void net_handle(const u8 *f, u32 len) {
    if (len < 14) return;
    u32 type = be16r(f + 12);
    if (type == 0x0806 && len >= 42) {                                                      // ARP
        u32 op = be16r(f + 20), tpa = be32r(f + 38), spa = be32r(f + 28);
        if (op == 1 && net_ip && tpa == net_ip) arp_reply(f);
        if (op == 2 && spa == net_gw) { mcopy(gw_mac, f + 22, 6); gw_mac_known = 1; }
        return;
    }
    if (type != 0x0800 || len < 34) return;
    u32 ihl = (f[14] & 15) * 4, proto = f[23], src = be32r(f + 26), dst = be32r(f + 30);
    const u8 *p = f + 14 + ihl; u32 plen = be16r(f + 16) - ihl;
    if (14 + ihl + plen > len) return;
    if (proto == 1 && plen >= 8) {                                                          // ICMP
        if (p[0] == 8 && dst == net_ip) {                                                   // echo request: answer it
            static u8 r[1480];
            mcopy(r, p, plen); r[0] = 0; be16w(r + 2, 0); be16w(r + 2, ~csum(r, plen, 0) & 0xffff);
            ip_send(f + 6, net_ip, src, 1, r, plen);
        } else if (p[0] == 0) { icmp_reply_seen = 1; icmp_reply_id = be16r(p + 4); icmp_reply_seq = be16r(p + 6); icmp_reply_ttl = f[22]; }
    } else if (proto == 17 && plen >= 8) {                                                  // UDP
        u32 sport = be16r(p), dport = be16r(p + 2), ulen = be16r(p + 4);
        if (ulen < 8 || ulen > plen) return;
        if (dport == 68 && ulen - 8 <= sizeof dhcp_in) { mcopy(dhcp_in, p + 8, ulen - 8); dhcp_in_len = ulen - 8; dhcp_in_got = 1; }
        else if (udp_listen_port && dport == udp_listen_port && ulen - 8 <= sizeof udp_in) {
            mcopy(udp_in, p + 8, ulen - 8); udp_in_len = ulen - 8; udp_in_port = sport; udp_in_src = src; udp_in_got = 1;
        }
    }
}

static void net_poll(void) {
    if (!nic_recv) return;
    for (int i = 0; i < 8; i++) { u32 n = nic_recv(rx_buf, sizeof rx_buf); if (!n) break; net_handle(rx_buf, n); }
}

static void net_wait_ms(u32 ms, int *flag) {
    u64 hz = tick_hz(), t0 = ticks();
    while (!*flag && hz && ticks() - t0 < hz / 1000 * ms) { net_poll(); wdt_kick(); }
}

// ---- ARP: find the MAC of the gateway ----
static int arp_gateway(void) {
    if (gw_mac_known) return 1;
    static const u8 bcast[6] = {255, 255, 255, 255, 255, 255};
    u8 a[28]; be16w(a, 1); be16w(a + 2, 0x0800); a[4] = 6; a[5] = 4; be16w(a + 6, 1);
    mcopy(a + 8, net_mac, 6); be32w(a + 14, net_ip); mset(a + 18, 0, 6); be32w(a + 24, net_gw);
    for (int tries = 0; tries < 4 && !gw_mac_known; tries++) {
        eth_send(bcast, 0x0806, a, 28);
        int flag = 0; u64 hz = tick_hz(), t0 = ticks();
        while (!gw_mac_known && hz && ticks() - t0 < hz / 4) { net_poll(); (void)flag; }
    }
    return gw_mac_known;
}

// ---- DHCP client ----
static int dhcp_wait(u32 want_type, u32 xid, u32 ms) {
    u64 hz = tick_hz(), t0 = ticks();
    while (hz && ticks() - t0 < hz / 1000 * ms) {
        dhcp_in_got = 0; net_poll();
        if (dhcp_in_got && dhcp_in_len > 240 && be32r(dhcp_in + 4) == xid) {
            u32 i = 240, mt = 0;
            while (i + 1 < dhcp_in_len && dhcp_in[i] != 255) {
                u32 op = dhcp_in[i];
                if (op == 0) { i++; continue; }
                u32 l = dhcp_in[i + 1];
                if (op == 53 && l == 1) mt = dhcp_in[i + 2];
                i += 2 + l;
            }
            if (mt == want_type) return 1;
        }
    }
    return 0;
}

static void dhcp_parse_options(void) {
    u32 i = 240;
    while (i + 1 < dhcp_in_len && dhcp_in[i] != 255) {
        u32 op = dhcp_in[i];
        if (op == 0) { i++; continue; }
        u32 l = dhcp_in[i + 1];
        if (op == 1 && l == 4) net_mask = be32r(dhcp_in + i + 2);
        if (op == 3 && l >= 4) net_gw = be32r(dhcp_in + i + 2);
        if (op == 6 && l >= 4) net_dns = be32r(dhcp_in + i + 2);
        if (op == 54 && l == 4) net_srv = be32r(dhcp_in + i + 2);
        i += 2 + l;
    }
}

static int dhcp_run(void) {
    static u8 m[320];
    static const u8 bcast[6] = {255, 255, 255, 255, 255, 255};
    u32 xid = (u32)ticks() ^ 0x5a17c0de;
    net_ip = 0; net_gw = 0; gw_mac_known = 0;
    for (int attempt = 1; attempt <= 3; attempt++) {
        mset(m, 0, sizeof m);
        m[0] = 1; m[1] = 1; m[2] = 6; be32w(m + 4, xid); be16w(m + 10, 0x8000);              // BOOTREQUEST, ethernet, broadcast flag
        mcopy(m + 28, net_mac, 6);
        be32w(m + 236, 0x63825363);                                                         // magic cookie
        u32 o = 240;
        m[o++] = 53; m[o++] = 1; m[o++] = 1;                                                // DISCOVER
        m[o++] = 55; m[o++] = 3; m[o++] = 1; m[o++] = 3; m[o++] = 6;                        // wanted: mask, router, dns
        m[o++] = 255;
        udp_send_raw(bcast, 0, 0xffffffff, 68, 67, m, o);
        if (!dhcp_wait(2, xid, 2000)) { puts("  dhcp: no offer (try "); put_dec(attempt); puts(")\n"); continue; }
        u32 yi = be32r(dhcp_in + 16);
        dhcp_parse_options();
        puts("  dhcp offer: "); put_ip(yi); putc('\n');
        mset(m, 0, sizeof m);
        m[0] = 1; m[1] = 1; m[2] = 6; be32w(m + 4, xid); be16w(m + 10, 0x8000);
        mcopy(m + 28, net_mac, 6); be32w(m + 236, 0x63825363);
        o = 240;
        m[o++] = 53; m[o++] = 1; m[o++] = 3;                                                // REQUEST
        m[o++] = 50; m[o++] = 4; be32w(m + o, yi); o += 4;
        m[o++] = 54; m[o++] = 4; be32w(m + o, net_srv); o += 4;
        m[o++] = 255;
        udp_send_raw(bcast, 0, 0xffffffff, 68, 67, m, o);
        if (!dhcp_wait(5, xid, 2000)) { puts("  dhcp: no ack\n"); continue; }
        net_ip = be32r(dhcp_in + 16);
        dhcp_parse_options();
        return 1;
    }
    return 0;
}

// ---- ping ----
static int net_ping(u32 dst, u32 *ms_out) {
    static u8 e[64];
    if (!arp_gateway()) return -1;
    e[0] = 8; e[1] = 0; be16w(e + 2, 0); be16w(e + 4, 0x7761); be16w(e + 6, 1);
    for (u32 i = 8; i < 40; i++) e[i] = (u8)i;
    be16w(e + 2, ~csum(e, 40, 0) & 0xffff);
    icmp_reply_seen = 0;
    u64 t0 = ticks();
    ip_send(gw_mac, net_ip, dst, 1, e, 40);
    net_wait_ms(2000, (int *)&icmp_reply_seen);
    if (!icmp_reply_seen) return -2;
    if (ms_out) *ms_out = (u32)((ticks() - t0) * 1000 / tick_hz());
    return 0;
}

// ---- UDP send to an address (via the gateway) and the log line sender used by the UDP logger ----
static int net_udp(u32 dst, u32 sport, u32 dport, const u8 *data, u32 len) {
    if (!net_ip || !arp_gateway()) return -1;
    return udp_send_raw(gw_mac, net_ip, dst, sport, dport, data, len);
}

// ---- the virtual network card of the fake Chromebook (never present on the real machine) ----
#define FAKENIC 0x1f000000UL
static int fakenic_present(void) { return rd32(FAKENIC) == 0x43494e57; }     // 'WNIC'; fault-safe read
static int fakenic_send(const u8 *f, u32 len) {
    volatile u32 *r = (volatile u32 *)FAKENIC;
    for (u32 i = 0; i < len; i += 4) r[4] = f[i] | (i + 1 < len ? f[i + 1] << 8 : 0) | (i + 2 < len ? (u32)f[i + 2] << 16 : 0) | (i + 3 < len ? (u32)f[i + 3] << 24 : 0);
    r[5] = len;
    return 0;
}
static int fakenic_recv(u8 *f, u32 max) {
    volatile u32 *r = (volatile u32 *)FAKENIC;
    u32 len = r[1];
    if (!len || len > max) { if (len) r[3] = 1; return 0; }
    for (u32 i = 0; i < len; i += 4) { u32 w = r[2]; f[i] = w; if (i + 1 < len) f[i + 1] = w >> 8; if (i + 2 < len) f[i + 2] = w >> 16; if (i + 3 < len) f[i + 3] = w >> 24; }
    r[3] = 1;                                                                               // pop
    return len;
}

// ---- the demo: bring the network up, ping the router, send a log line to the PC ----
static void net_demo(void) {
    // The probe reads an address that exists only in the fake: never touch it on the real machine (the fake adds 'wavefake' to the boot arguments).
    if (!dti.bootargs || !ci_eq(dti.bootargs, "wavefake") || !fakenic_present()) { puts("no network card here yet (this demo only runs on the fake Chromebook)\n"); return; }
    nic_send = fakenic_send; nic_recv = fakenic_recv;
    puts("network: virtual card, mac "); for (u32 i = 0; i < 6; i++) { putc("0123456789abcdef"[net_mac[i] >> 4]); putc("0123456789abcdef"[net_mac[i] & 15]); if (i < 5) putc(':'); } putc('\n');
    if (!dhcp_run()) { puts("dhcp failed\n"); return; }
    puts("  address "); put_ip(net_ip); puts("  mask "); put_ip(net_mask); puts("  gateway "); put_ip(net_gw); puts("  dns "); put_ip(net_dns); putc('\n');
    u32 ms = 0; int r = net_ping(net_gw, &ms);
    puts("  ping gateway: "); if (r) puts("no answer\n"); else { put_dec(ms); puts(" ms\n"); }
    static const u8 line[] = "wave-os: hello from the Chromebook (udp log test)";
    r = net_udp(ip4(192, 168, 4, 10), 5141, 5140, line, sizeof line - 1);
    puts("  log line to the PC: "); puts(r ? "failed\n" : "sent\n");
}
