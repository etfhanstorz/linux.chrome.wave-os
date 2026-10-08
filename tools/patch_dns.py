root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# ---- net.h: header parsing in http_get, plus a DNS client ----
edit('net.h', [
("""// HTTP/1.0 GET into dst (max bytes). Returns the body length, or a negative error:""",
"""// What the last http_get saw in the reply headers (the browser follows redirects and picks the page type from these).
static int http_status;                                   // 200, 302, 404 ... (0 = none)
static char http_location[256], http_ctype[48];           // Location: and Content-Type: values
static int http_any;                                      // 1 = return the body for any status; 0 = only 200 (what `up` wants)
static int ci_prefix(const u8 *s, const char *p) { for (u32 i = 0; p[i]; i++) { u32 a = s[i] | 0x20, b = (u32)p[i] | 0x20; if (a != b) return 0; } return 1; }
static void http_parse_headers(const u8 *h, u32 n) {
    http_status = 0; http_location[0] = 0; http_ctype[0] = 0;
    if (n >= 12 && h[0] == 'H') { u32 i = 0; while (i < n && h[i] != ' ') i++; i++; while (i < n && h[i] >= '0' && h[i] <= '9') http_status = http_status * 10 + (h[i++] - '0'); }
    for (u32 i = 0; i < n; i++) {
        if (i && h[i - 1] != '\\n') continue;                                                   // only at the start of a line
        char *dst = 0; u32 max = 0, skip = 0;
        if (i + 9 < n && ci_prefix(h + i, "location:")) { dst = http_location; max = sizeof http_location; skip = 9; }
        else if (i + 13 < n && ci_prefix(h + i, "content-type:")) { dst = http_ctype; max = sizeof http_ctype; skip = 13; }
        if (!dst) continue;
        u32 j = i + skip, k = 0;
        while (j < n && (h[j] == ' ' || h[j] == '\\t')) j++;
        while (j < n && h[j] != '\\r' && h[j] != '\\n' && k + 1 < max) dst[k++] = (char)h[j++];
        dst[k] = 0;
    }
}

// HTTP/1.0 GET into dst (max bytes). Returns the body length, or a negative error:"""),
("""    const char *parts[] = {"GET ", path, " HTTP/1.0\\r\\nHost: ", host, "\\r\\nConnection: close\\r\\n\\r\\n"};""",
"""    const char *parts[] = {"GET ", path, " HTTP/1.0\\r\\nHost: ", host, "\\r\\nUser-Agent: wave-os\\r\\nAccept: text/html, text/plain, */*\\r\\nConnection: close\\r\\n\\r\\n"};"""),
("""    if (!hdr || total < 12 || dst[9] != '2' || dst[10] != '0' || dst[11] != '0') return hdr ? -6 : -4;""",
"""    if (hdr) http_parse_headers(dst, hdr);
    if (!hdr || total < 12) return -4;
    if (!http_any && (dst[9] != '2' || dst[10] != '0' || dst[11] != '0')) return -6;"""),
("""// ---- the virtual network card of the fake Chromebook""",
"""// ---- DNS: ask the router's DNS server (from DHCP) for an address (v1.6) ----
static char dns_cache_name[6][64]; static u32 dns_cache_ip[6], dns_cache_n;
static int dns_name_len(const u8 *p, u32 left) {            // length of a (possibly compressed) name at p, or -1
    u32 i = 0;
    while (i < left) {
        u32 l = p[i];
        if (l == 0) return (int)i + 1;
        if ((l & 0xc0) == 0xc0) return (int)i + 2;
        i += 1 + l;
    }
    return -1;
}
// Returns 0 and the address in *ip, or: -1 no network / no DNS server, -2 the name does not exist, -3 no answer, -4 bad name
static int dns_lookup(const char *name, u32 *ip) {
    u32 lit = 0;
    { const char *s = name; u32 dots = 0, ok = *s != 0; for (; *s; s++) { if (*s == '.') dots++; else if (*s < '0' || *s > '9') ok = 0; } if (ok && dots == 3) { u32 v[4] = {0, 0, 0, 0}, k = 0; for (s = name; *s; s++) { if (*s == '.') k++; else v[k] = v[k] * 10 + (*s - '0'); } lit = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]; } }
    if (lit) { *ip = lit; return 0; }                                                       // already a number
    for (u32 i = 0; i < dns_cache_n && i < 6; i++) { u32 k = 0; while (name[k] && name[k] == dns_cache_name[i][k]) k++; if (!name[k] && !dns_cache_name[i][k]) { *ip = dns_cache_ip[i]; return 0; } }
    if (!net_ip || !net_dns) return -1;
    static u8 q[300]; u32 n = 12;
    mset(q, 0, 12); u32 id = (u32)ticks() & 0xffff; be16w(q, id); be16w(q + 2, 0x0100); be16w(q + 4, 1);       // standard query, recursion desired, one question
    for (u32 i = 0; name[i];) {                                                              // name as labels
        u32 j = i; while (name[j] && name[j] != '.') j++;
        if (j == i || j - i > 63 || n + (j - i) + 6 > sizeof q) return -4;
        q[n++] = (u8)(j - i); for (u32 k = i; k < j; k++) q[n++] = (u8)name[k];
        i = name[j] ? j + 1 : j;
    }
    q[n++] = 0; be16w(q + n, 1); be16w(q + n + 2, 1); n += 4;                                // type A, class IN
    u32 sport = 49300 + (id & 0xff);
    udp_listen_port = sport;
    for (int attempt = 0; attempt < 3; attempt++) {
        udp_in_got = 0;
        if (net_udp(net_dns, sport, 53, q, n)) continue;
        net_wait_ms(1500, (int *)&udp_in_got);
        if (!udp_in_got || udp_in_len < 12 || be16r(udp_in) != id) continue;
        udp_listen_port = 0;
        u32 rcode = be16r(udp_in + 2) & 15, qd = be16r(udp_in + 4), an = be16r(udp_in + 6);
        if (rcode == 3) return -2;
        if (rcode) return -3;
        u32 p = 12;
        for (u32 i = 0; i < qd; i++) { int l = dns_name_len(udp_in + p, udp_in_len - p); if (l < 0) return -3; p += l + 4; }
        for (u32 i = 0; i < an && p + 10 <= udp_in_len; i++) {
            int l = dns_name_len(udp_in + p, udp_in_len - p); if (l < 0) return -3; p += l;
            u32 type = be16r(udp_in + p), rdl = be16r(udp_in + p + 8); p += 10;
            if (p + rdl > udp_in_len) return -3;
            if (type == 1 && rdl == 4) {
                *ip = be32r(udp_in + p);
                u32 s = dns_cache_n % 6; u32 k = 0; while (name[k] && k < 63) { dns_cache_name[s][k] = name[k]; k++; } dns_cache_name[s][k] = 0; dns_cache_ip[s] = *ip; dns_cache_n++;
                return 0;
            }
            p += rdl;
        }
        return -3;
    }
    udp_listen_port = 0;
    return -3;
}

// ---- the virtual network card of the fake Chromebook"""),
])

# ---- the fake LAN: a DNS server on the router and a small web server on the fake PC (port 80) ----
edit('fakehana/fakenet.py', [
("""        self.http_log = []""",
"""        self.http_log = []
        self.dns_names = {'example.test': PC_IP, 'site.test': PC_IP, 'example.com': PC_IP}   # the router's DNS: these names exist, everything else does not
        self.web = {}                                # port 80 on the fake PC: path -> (status, extra headers, body)"""),
("""            if dport == 67: return self.dhcp(src, data)""",
"""            if dport == 67: return self.dhcp(src, data)
            if dport == 53 and dstip in (GW_IP, PC_IP) and len(data) > 12: return self.dns(src, sport, srcip, dstip, data)"""),
("""        if proto == 6 and dstip == PC_IP and len(p) >= 20:""",
"""        if proto == 6 and dstip == PC_IP and len(p) >= 20 and struct.unpack('>H', p[2:4])[0] == 80:
            return self.tcp_server(src, p, port=80)
        if proto == 6 and dstip == PC_IP and len(p) >= 20:"""),
("""    def tcp_server(self, srcmac, p):
        sport, dport, seq, ack, off, fl = struct.unpack('>HHIIBB', p[:14]); hl = (off >> 4) * 4; data = p[hl:]
        if dport != 8000: return""",
"""    def dns(self, srcmac, sport, srcip, dstip, q):
        \"\"\"Answer an A query (names in self.dns_names; anything else is NXDOMAIN).\"\"\"
        i = 12; labels = []
        while i < len(q) and q[i]:
            labels.append(q[i + 1:i + 1 + q[i]].decode(errors='replace')); i += 1 + q[i]
        name = '.'.join(labels); question = q[12:i + 5]
        ip = self.dns_names.get(name)
        hdr = q[0:2] + (b'\\x81\\x80' if ip else b'\\x81\\x83') + struct.pack('>HHHH', 1, 1 if ip else 0, 0, 0)
        ans = (b'\\xc0\\x0c' + struct.pack('>HHIH', 1, 1, 60, 4) + ip2b(ip)) if ip else b''
        self.events.append('dns %s -> %s' % (name, ip))
        u = struct.pack('>HHHH', 53, sport, 8 + len(hdr) + len(question) + len(ans), 0) + hdr + question + ans
        self.send(self.ip_packet(dstip, srcip, 17, u, srcmac, GW_MAC if dstip == GW_IP else PC_MAC))

    def tcp_server(self, srcmac, p, port=8000):
        sport, dport, seq, ack, off, fl = struct.unpack('>HHIIBB', p[:14]); hl = (off >> 4) * 4; data = p[hl:]
        if dport != port: return"""),
("""            body = self.routes.get(path)
            if body is None: resp = b'HTTP/1.0 404 Not Found\\r\\nContent-Length: 0\\r\\n\\r\\n'
            else: resp = b'HTTP/1.0 200 OK\\r\\nContent-Type: application/octet-stream\\r\\nContent-Length: %d\\r\\n\\r\\n' % len(body) + body""",
"""            if port == 80:                                      # the fake web server: (status, extra headers, body)
                status, extra, body = self.web.get(path, (404, '', b'<html><body><h1>Not Found</h1></body></html>'))
                resp = ('HTTP/1.0 %d X\\r\\nContent-Type: text/html\\r\\n%sContent-Length: %d\\r\\n\\r\\n' % (status, extra, len(body))).encode() + body
            else:
                body = self.routes.get(path)
                if body is None: resp = b'HTTP/1.0 404 Not Found\\r\\nContent-Length: 0\\r\\n\\r\\n'
                else: resp = b'HTTP/1.0 200 OK\\r\\nContent-Type: application/octet-stream\\r\\nContent-Length: %d\\r\\n\\r\\n' % len(body) + body"""),
])
print('ok')
