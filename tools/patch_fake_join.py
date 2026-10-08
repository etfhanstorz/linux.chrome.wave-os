p = '/mnt/c/!ab1/os/fakehana/fakesdio.py'
s = open(p, newline='').read().replace('\r\n', '\n')

def sub(old, new):
    global s
    assert old in s, old[:60]
    s = s.replace(old, new, 1)

sub("""        self.cmd_resp = b''
        self.queue = []                  # packets waiting for the host on the command port (answers + events)""",
"""        self.cmd_resp = b''
        self.data_q = {}                 # data ports 0..31: port -> SDIO packet waiting for the host (rx descriptor + frame)
        self.data_ptr = 0                # next data port the chip fills (rolling, like Linux curr_rd_port)
        self.partial = {}                # data port -> bytes not yet read (byte-mode reads come in 512-byte pieces)
        self.assoc = None                # set when the host associated: (bssid, ssid)
        self.queue = []                  # packets waiting for the host on the command port (answers + events)""")

sub("""        self.mask = 0; self.int_status = 0; self.cmd_resp = b''; self.queue = []
        self.cold_boots""",
"""        self.mask = 0; self.int_status = 0; self.cmd_resp = b''; self.queue = []
        self.data_q = {}; self.data_ptr = 0; self.partial = {}; self.assoc = None
        self.cold_boots""")

sub("""    def host_command(self, d):""",
'''    def push_data(self, dst, src, ethertype, payload):
        """The chip received a frame for the host: SDIO header + rx descriptor + 802.3 header + LLC/SNAP (as the real firmware delivers it)."""
        frame = dst + src + struct.pack('>H', 8 + len(payload)) + bytes([0xaa, 0xaa, 0x03, 0, 0, 0]) + struct.pack('>H', ethertype) + payload
        rxpd = struct.pack('<BBHHH', 0, 0, len(frame), 20, 0).ljust(20, b'\\0')       # bss type/num, frame length, offset of the frame from the descriptor, type
        pkt = struct.pack('<HH', 4 + len(rxpd) + len(frame), 0) + rxpd + frame
        port = self.data_ptr
        self.data_ptr = (self.data_ptr + 1) % 32
        self.data_q[port] = pkt

    def host_command(self, d):''')

sub("""        elif cmd == 0x0006:                              # legacy SCAN: a few invented access points""",
'''        elif cmd == 0x0012:                              # ASSOCIATE: peer(6) cap(2) listen(2) beacon(2) dtim(1), then TLVs
            peer = bytes(d[12:18]); tlvs = {}; p = 12 + 13
            while p + 4 <= 12 + max(13, size - 8):
                tt, tl = struct.unpack_from('<HH', d, p); tlvs[tt] = bytes(d[p + 4:p + 4 + tl]); p += 4 + tl
            good = peer == bytes.fromhex('021122334455') and tlvs.get(0) == b'HomeWifi' and len(tlvs.get(48, b'')) == 20 and tlvs.get(0x0101, b'')[:2] == bytes([1, 157])
            status = 0 if good else 1
            body = struct.pack('<HHH', 0x0411, status, 0xc001 if good else 0)
            if good:
                self.assoc = (peer, tlvs[0])
                ap = peer; me = bytes.fromhex('0050431a2b3c')
                # the router starts the password handshake: EAPOL-Key message 1 (descriptor 2, key info 0x008a, replay counter 1, ANonce)
                key = bytes([2]) + struct.pack('>HHQ', 0x008a, 16, 1) + bytes(range(32)) + bytes(16) + bytes(8) + bytes(8) + bytes(16) + struct.pack('>H', 0)
                self.push_data(me, ap, 0x888e, bytes([2, 3]) + struct.pack('>H', len(key)) + key)
        elif cmd == 0x0006:                              # legacy SCAN: a few invented access points''')

sub("""        if r == 0x02:
            return self.mask
        if r in (0xb4, 0xb5):""",
"""        if r == 0x02:
            return self.mask
        if 0x04 <= r <= 0x07:                            # upload (receive) bitmap: bit p = data port p has a packet
            bm = sum(1 << pt for pt in self.data_q)
            return (bm >> (8 * (r - 0x04))) & 0xff
        if 0x08 <= r <= 0x0b:                            # download bitmap: every data port may be written
            return 0xff
        if 0x0c <= r < 0x0c + 64:                        # length of the packet on port (r-0x0c)//2
            pt = (r - 0x0c) // 2
            n = len(self.data_q.get(pt, b''))
            return (n >> (8 * ((r - 0x0c) & 1))) & 0xff
        if r in (0xb4, 0xb5):""")

sub("""    def port_read(self, addr, nbytes):
        if addr == 0x18000 and self.queue:""",
"""    def port_read(self, addr, nbytes):
        if 0x10000 <= addr < 0x10020:                    # a data port: the packet is read out in pieces
            pt = addr - 0x10000
            if pt not in self.partial and pt in self.data_q:
                self.partial[pt] = self.data_q.pop(pt)
            buf = self.partial.get(pt, b'')
            out, rest = buf[:nbytes], buf[nbytes:]
            if rest: self.partial[pt] = rest
            else: self.partial.pop(pt, None)
            return out.ljust(nbytes, b'\\0')
        if addr == 0x18000 and self.queue:""")

open(p, 'w', newline='').write(s)
print('patched fake')
