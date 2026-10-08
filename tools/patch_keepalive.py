root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('wpa.h', [
# state
("""static u8 ptk[48], anonce[32], snonce[32], gtk[16], gtk_id, ap_ver;""",
"""static u8 ptk[48], anonce[32], snonce[32], gtk[16], gtk_id, ap_ver;
static u32 wifi_up;                                     // 1 = connected (keys in, address from DHCP): the idle service keeps it that way
static u8 last_replay[8];                               // the router's last key-message counter (a refresh must count higher)
static u32 wifi_rekeys, wifi_drops, wifi_link_lost;
static void wifi_eapol_rx(const u8 *f, u32 fl);"""),
# EAPOL frames arriving while connected are key refreshes, not junk
("""        if (f[12] == 0x88 && f[13] == 0x8e) continue;               // stray handshake retransmission""",
"""        if (f[12] == 0x88 && f[13] == 0x8e) { wifi_eapol_rx(f, fl); continue; }   // a key refresh from the router (v1.6-001)"""),
# remember the counter of message 3
("""    mcopy(replay, k + 5, 8);
    static u8 kd[256];""",
"""    mcopy(replay, k + 5, 8); mcopy(last_replay, replay, 8);
    static u8 kd[256];"""),
# connect: mark up/down
("""static int wifi_connect(const char *name) {
    joined = 0; rd_cur_port = wr_cur_port = 0;""",
"""static int wifi_connect(const char *name) {
    joined = 0; rd_cur_port = wr_cur_port = 0; wifi_up = 0;"""),
("""    sum_s("up "); sum_u(net_ip >> 24);""",
"""    wifi_up = 1;
    sum_s("up "); sum_u(net_ip >> 24);"""),
])

open(root + 'wpa.h', 'a', newline='').write("""
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
        if (info & 0x80) { log_quiet("wifi: the router restarted the handshake: reconnecting\\n"); wifi_link_lost = 1; }
        return;
    }
    if ((info & 0x1380) != 0x1380 || (info & 7) != 2) return;                     // not a group key message 1
    if (!replay_newer(k + 5, last_replay)) return;                               // old or repeated: ignore
    if (fl < 14 + 99 + dlen || dlen < 24 || dlen > 256 || (dlen & 7)) return;
    static u8 copy[99 + 256]; mcopy(copy, e, 99 + dlen); mset(copy + 4 + 77, 0, 16);
    u8 h[20]; hmac_sha1(ptk, 16, copy, 99 + dlen, h);
    if (!meq(h, k + 77, 16)) { log_quiet("wifi: group key message with a bad signature: ignored\\n"); return; }
    static u8 kd[256];
    if (aes_unwrap(ptk + 16, k + 95, dlen / 8 - 1, kd)) { log_quiet("wifi: could not decrypt the new group key\\n"); return; }
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
    if (wifi_set_key(gtk_id, 0, gtk, 0)) { wifi_rekeys++; puts("wifi: the router refreshed the group key: done\\n"); }
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
    if (ev == 0x0003 || ev == 0x0008 || ev == 0x0009) { log_quiet(ev == 3 ? "wifi: the chip lost the link\\n" : "wifi: the router disconnected us\\n"); wifi_link_lost = 1; }
    if (!wifi_link_lost) return 0;
    wifi_link_lost = 0; wifi_up = 0; wifi_drops++;
    if (!pmk_valid) { puts("\\n(Wi-Fi dropped: type k to reconnect)\\n"); return 1; }
    puts("\\n(Wi-Fi dropped: reconnecting...)\\n");
    u32 keep = con_on; con_on = 0;
    int ok = wifi_connect(pmk_ssid[0] ? pmk_ssid : WIFI_DEFAULT_SSID);
    con_on = keep;
    if (ok) { puts("(Wi-Fi back: "); put_ip(net_ip); puts(")\\n"); log_ship(); }
    else puts("(Wi-Fi reconnect failed: type k)\\n");
    return 1;
}
""")

edit('shell.h', [
("""            while ((c = input_poll()) < 0) __asm__ volatile("nop");""",
"""            while ((c = input_poll()) < 0) {
                if (wifi_service()) {                                   // the network printed something: show the prompt and what was typed again
                    con_fg = 0x40E0FF; outs("wave"); con_fg = 0x60FF80; outs("> "); con_fg = C_TEXT;
                    for (u32 q = 0; q < n; q++) out(buf[q]);
                }
            }"""),
])
edit('web.h', [("""        if (c < 0) { wdt_kick(); continue; }""", """        if (c < 0) { wdt_kick(); wifi_service(); continue; }""")])

# ---- the fake router: refresh the group key, and later drop the link once ----
edit('fakehana/fakeap.py', [
("""        elif self.state == 'm3' and info == 0x030a and replay == self.replay:""",
"""        elif self.state == 'done' and info == 0x0302 and replay == self.replay:      # group key message 2
            if mic_ok(self.ptk[:16]): self.log.append('group msg2 ok'); self.rekeyed = True
            else: self.log.append('group msg2 bad mic')
        elif self.state == 'm3' and info == 0x030a and replay == self.replay:"""),
])
open(root + 'fakehana/fakeap.py', 'a', newline='').write('''
    def group_rekey(self):
        """Refresh the group key, the way routers do every so often: group message 1, the new key wrapped with the KEK."""
        self.replay += 1
        self.gtk = bytes(range(0x70, 0x80))
        kde = bytes([0xdd, 22, 0x00, 0x0f, 0xac, 1, 2, 0]) + self.gtk
        plain = kde + b'\\xdd'
        while len(plain) % 8:
            plain += b'\\0'
        self.log.append('group rekey sent')
        self.l.push_data(self.sta, AP_MAC, 0x888e, self._frame(0x1382, 0, self.replay, bytes(32), aes_wrap(self.ptk[16:32], plain), kck=self.ptk[:16]))
''')
edit('fakehana/fakesdio.py', [
("""        elif self.ap and self.ap.done and self.lan:
            self.lan.guest_mac = bytes(frame[6:12])""",
"""        elif self.ap and self.ap.done and self.lan:
            self.after_up = getattr(self, 'after_up', 0) + 1
            if self.after_up == 8 and not getattr(self.ap, 'rekeyed', False):
                self.ap.group_rekey()                      # MODEL: the router refreshes the group key a while after joining
            if self.after_up == 16 and self.drop_once:
                self.drop_once = False; self.after_up = 100
                self.ap.done = False
                self.queue.append(struct.pack('<HHI', 8, 3, 0x0003)); self.int_status = 0x40     # MODEL: the link is lost (event 3)
                return
            self.lan.guest_mac = bytes(frame[6:12])"""),
("""        self.restart_model = True        # MODEL (real hana): see port_read""",
"""        self.restart_model = True        # MODEL (real hana): see port_read
        self.drop_once = True            # MODEL: drop the link once after a while (tests the automatic reconnect)"""),
])
edit('fakehana/fakehana.py', [
("""    if m.net.events:
        print('  fake LAN: ' + '; '.join(m.net.events[:12]))""",
"""    if m.net.events:
        print('  fake LAN: ' + '; '.join(m.net.events[:12]))
    if m.msdc.loader and m.msdc.loader.ap:
        print('  fake router: ' + '; '.join(m.msdc.loader.ap.log[-10:]) + '; keys installed: ' + ', '.join('%s %s' % (k, 'ok' if ok else 'REJECTED') for k, ok, _ in m.msdc.loader.keys_set))"""),
])
edit('version.h', [('#define WAVE_PATCH   ""', '#define WAVE_PATCH   "-001"')])
print('ok')
