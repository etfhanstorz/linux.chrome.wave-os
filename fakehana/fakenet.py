"""A virtual network card for the fake Chromebook plus a tiny fake LAN behind it.

The card is a handful of MMIO registers (fake-only; it does not exist on the real machine):
  +0x00 id 'WNIC'   +0x04 length of the next received frame (0 = none)   +0x08 read next 4 bytes of it
  +0x0c write: drop the current frame   +0x10 write: append 4 bytes to the transmit buffer   +0x14 write: send N bytes

The LAN: gateway 192.168.4.1 (DHCP server, answers ARP and ping), a PC at 192.168.4.10 (answers ARP and ping,
collects UDP log lines on port 5140, echoes UDP port 7), and the guest gets 192.168.4.50.
"""
import struct

GW_MAC, PC_MAC = bytes.fromhex('0250434a0001'), bytes.fromhex('0250434a0010')
GW_IP, PC_IP, GUEST_IP = '192.168.4.1', '192.168.4.10', '192.168.4.50'

def ip2b(s): return bytes(int(x) for x in s.split('.'))
def csum(b):
    if len(b) % 2: b += b'\0'
    s = sum(struct.unpack('>%dH' % (len(b) // 2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff

class FakeNet:
    def __init__(self):
        self.rx = []                # frames waiting for the guest
        self.tx = bytearray()
        self.cur = None             # frame being read, as bytes with a read offset
        self.off = 0
        self.log_lines = []         # UDP log lines the "PC" received
        self.events = []            # human readable trace
        self.pings = 0

    # ---- MMIO ----
    def read(self, off):
        if off == 0x00: return 0x43494e57
        if off == 0x04:
            return len(self.rx[0]) if self.rx else 0
        if off == 0x08 and self.rx:
            f = self.rx[0]; w = f[self.off:self.off + 4].ljust(4, b'\0'); self.off += 4
            return struct.unpack('<I', w)[0]
        return 0

    def write(self, off, val):
        if off == 0x0c and self.rx:
            self.rx.pop(0); self.off = 0
        elif off == 0x10:
            self.tx += struct.pack('<I', val)
        elif off == 0x14:
            frame = bytes(self.tx[:val]); self.tx = bytearray()
            self.from_guest(frame)

    # ---- the fake LAN ----
    def send(self, frame): self.rx.append(frame)

    def eth(self, dst, src, typ, payload): return dst + src + struct.pack('>H', typ) + payload

    def ip_packet(self, src_ip, dst_ip, proto, payload, dst_mac, src_mac):
        h = bytearray(struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 1, 0, 64, proto, 0, ip2b(src_ip), ip2b(dst_ip)))
        struct.pack_into('>H', h, 10, csum(bytes(h)))
        return self.eth(dst_mac, src_mac, 0x0800, bytes(h) + payload)

    def from_guest(self, f):
        if len(f) < 14: return
        dst, src, typ = f[0:6], f[6:12], struct.unpack('>H', f[12:14])[0]
        if typ == 0x0806 and len(f) >= 42:                                  # ARP
            op = struct.unpack('>H', f[20:22])[0]; tpa = '.'.join(map(str, f[38:42]))
            if op == 1 and tpa in (GW_IP, PC_IP):
                mac = GW_MAC if tpa == GW_IP else PC_MAC
                a = struct.pack('>HHBBH', 1, 0x0800, 6, 4, 2) + mac + ip2b(tpa) + src + f[28:32]
                self.events.append('arp: %s is at the fake %s' % (tpa, 'gateway' if tpa == GW_IP else 'PC'))
                self.send(self.eth(src, mac, 0x0806, a))
            return
        if typ != 0x0800 or len(f) < 34: return
        ihl = (f[14] & 15) * 4; proto = f[23]; srcip = '.'.join(map(str, f[26:30])); dstip = '.'.join(map(str, f[30:34]))
        p = f[14 + ihl:14 + struct.unpack('>H', f[16:18])[0]]
        if proto == 17 and len(p) >= 8:
            sport, dport, ulen = struct.unpack('>HHH', p[:6]); data = p[8:ulen]
            if dport == 67: return self.dhcp(src, data)
            if dport == 5140 and dstip == PC_IP:
                self.log_lines.append(data.decode(errors='replace')); self.events.append('PC log server got: %s' % self.log_lines[-1]); return
            if dport == 7:
                u = struct.pack('>HHHH', 7, sport, 8 + len(data), 0) + data
                self.send(self.ip_packet(dstip, srcip, 17, u, src, GW_MAC if dstip == GW_IP else PC_MAC)); return
        if proto == 1 and p and p[0] == 8 and dstip in (GW_IP, PC_IP):        # ping
            self.pings += 1
            r = bytearray(p); r[0] = 0; r[2:4] = b'\0\0'; struct.pack_into('>H', r, 2, csum(bytes(r)))
            self.events.append('ping to %s answered' % dstip)
            self.send(self.ip_packet(dstip, srcip, 1, bytes(r), src, GW_MAC if dstip == GW_IP else PC_MAC))

    def dhcp(self, guest_mac, d):
        if len(d) < 240: return
        xid = d[4:8]; mt = 0; i = 240
        while i + 1 < len(d) and d[i] != 255:
            if d[i] == 0: i += 1; continue
            if d[i] == 53: mt = d[i + 2]
            i += 2 + d[i + 1]
        if mt not in (1, 3): return
        rep = 2 if mt == 1 else 5
        m = bytearray(300)
        m[0], m[1], m[2] = 2, 1, 6; m[4:8] = xid; m[10:12] = b'\x80\x00'
        m[16:20] = ip2b(GUEST_IP); m[20:24] = ip2b(GW_IP); m[28:34] = d[28:34]; m[236:240] = bytes.fromhex('63825363')
        o = 240
        for opt in (bytes([53, 1, rep]), bytes([54, 4]) + ip2b(GW_IP), bytes([51, 4]) + struct.pack('>I', 86400),
                    bytes([1, 4]) + ip2b('255.255.255.0'), bytes([3, 4]) + ip2b(GW_IP), bytes([6, 4]) + ip2b(GW_IP), bytes([255])):
            m[o:o + len(opt)] = opt; o += len(opt)
        self.events.append('dhcp %s -> %s' % ('offer' if rep == 2 else 'ack', GUEST_IP))
        u = struct.pack('>HHHH', 67, 68, 8 + o, 0) + bytes(m[:o])
        self.send(self.ip_packet(GW_IP, '255.255.255.255', 17, u, b'\xff' * 6, GW_MAC))
