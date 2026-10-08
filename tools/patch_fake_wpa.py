import re
base = '/mnt/c/!ab1/os/fakehana/'

def edit(name, pairs):
    s = open(base + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:70])
        s = s.replace(old, new, 1)
    open(base + name, 'w', newline='').write(s)

# --- fakeap: simpler padding ---
edit('fakeap.py', [(
"""            plain += bytes([0xdd]) + bytes((8 - (len(plain) + 1) % 8) % 8 + (0 if (len(plain) + 1) % 8 == 0 else 0))
            while len(plain) % 8:
                plain += b'\\0'
""",
"""            plain += b'\\xdd'                                   # padding: one 0xdd then zeros up to a multiple of 8
            while len(plain) % 8:
                plain += b'\\0'
""")])

# --- fakesdio: router, data TX, key install ---
edit('fakesdio.py', [
("""        self.assoc = None                # set when the host associated: (bssid, ssid)
        self.queue = []                  # packets waiting for the host on the command port (answers + events)""",
"""        self.assoc = None                # set when the host associated: (bssid, ssid)
        self.lan = None                  # the fake LAN behind the router (set by fakehana.py)
        self.ap = None                   # the fake WPA2 router (made on first use)
        self.keys_set = []               # key install commands seen: (pairwise?, key accepted?)
        self.queue = []                  # packets waiting for the host on the command port (answers + events)"""),
("""        self.data_q = {}; self.data_ptr = 0; self.partial = {}; self.assoc = None
        self.cold_boots""",
"""        self.data_q = {}; self.data_ptr = 0; self.partial = {}; self.assoc = None; self.keys_set = []
        self.ap = None
        self.cold_boots"""),
# association starts the router's handshake
("""                self.push_data(me, ap, 0x888e, bytes([2, 3]) + struct.pack('>H', len(key)) + key)""",
"""                import fakeap
                self.ap = fakeap.FakeAP(self)
                self.ap.start(me)"""),
# key install
("""        elif cmd == 0x0006:                              # legacy SCAN: a few invented access points""",
"""        elif cmd == 0x005e:                              # KEY_MATERIAL v2: action(2) TLV(type 0x019c, len 52: mac6 idx type info(2) pn8 keylen(2) key32)
            tt, tl = struct.unpack_from('<HH', d, 14)
            t = bytes(d[18:18 + tl])
            info, klen = struct.unpack_from('<H', t, 8)[0], struct.unpack_from('<H', t, 18)[0]
            key = t[20:20 + klen]
            want = None
            if self.ap and self.ap.done:
                want = self.ap.tk if info & 2 else self.ap.gtk
            ok = tt == 0x019c and tl == 52 and t[7] == 2 and klen == 16 and key == want
            self.keys_set.append(('ptk' if info & 2 else 'gtk', ok, hex(info)))
            if not ok:
                result = 1
        elif cmd == 0x0006:                              # legacy SCAN: a few invented access points"""),
# data TX
("""    def port_write(self, data, addr=0x10000):
        \"\"\"A CMD53 block write to the memory port: must be exactly what the ROM asked for.\"\"\"""",
"""    def guest_frame(self, frame):
        \"\"\"The host sent a data frame (Ethernet II): handshake frames go to the router, everything else to the LAN once the keys are in.\"\"\"
        et = struct.unpack('>H', frame[12:14])[0]
        if et == 0x888e:
            if self.ap: self.ap.on_eapol(frame[6:12], frame[14:])
        elif self.ap and self.ap.done and self.lan:
            self.lan.guest_mac = bytes(frame[6:12])
            self.lan.from_guest(bytes(frame))
            while self.lan.rx:
                f = self.lan.rx.pop(0)
                self.push_data(f[0:6], f[6:12], struct.unpack('>H', f[12:14])[0], f[14:])

    def port_write(self, data, addr=0x10000):
        \"\"\"A CMD53 block write to the memory port: must be exactly what the ROM asked for.\"\"\"
        if 0x10000 <= addr < 0x10020 and (self.running or self.pos >= len(self.fw)):          # firmware is up: this is a data port, not the download port
            total, typ = struct.unpack_from('<HH', data, 0)
            if typ == 0:
                flen, off = struct.unpack_from('<HH', data, 6)
                self.guest_frame(bytes(data[4 + off:4 + off + flen]))
            return"""),
])

# --- fakenet: remember the guest's own address ---
edit('fakenet.py', [(
"""        return self.ip_packet(PC_IP, GUEST_IP, 6, seg, GUEST_MAC, PC_MAC)""",
"""        return self.ip_packet(PC_IP, GUEST_IP, 6, seg, getattr(self, 'guest_mac', GUEST_MAC), PC_MAC)""")])

# --- fakehana: attach the LAN to the router; ec: pause token in --keys ---
edit('fakehana.py', [(
"""        self.net = FakeNet()                          # virtual network card + a tiny fake LAN (fake-only)""",
"""        self.net = FakeNet()                          # virtual network card + a tiny fake LAN (fake-only)
        if self.msdc.loader:
            self.msdc.loader.lan = self.net            # the fake Wi-Fi router forwards to the same fake LAN""")])
edit('fakeec.py', [(
"""        for ch in text:
            if ch not in pos:""",
"""        for ch in text:
            if ch == '\\x01':                              # pause token: 5 simulated seconds without typing (lets slow commands finish)
                t += 5.0
                continue
            if ch not in pos:""")])
print('patched')
