"""A virtual network card for the fake Chromebook plus a tiny fake LAN behind it.

The card is a handful of MMIO registers (fake-only; it does not exist on the real machine):
  +0x00 id 'WNIC'   +0x04 length of the next received frame (0 = none)   +0x08 read next 4 bytes of it
  +0x0c write: drop the current frame   +0x10 write: append 4 bytes to the transmit buffer   +0x14 write: send N bytes

The LAN: gateway 192.168.4.1 (DHCP server, answers ARP and ping), a PC at 192.168.4.10 (answers ARP and ping,
collects UDP log lines on port 5140, echoes UDP port 7), and the guest gets 192.168.4.50.
"""
import hashlib, hmac, os, struct

GW_MAC, PC_MAC = bytes.fromhex('0250434a0001'), bytes.fromhex('0250434a0010')
GW_IP, PC_IP, GUEST_IP = '192.168.4.1', '192.168.4.10', '192.168.4.50'
GUEST_MAC = bytes.fromhex('020000000002')

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
        self.routes = {}            # HTTP server on the fake PC (port 8000): path -> bytes
        self.conns = {}             # guest port -> server state
        self.http_log = []
        self.dns_names = {'example.test': PC_IP, 'site.test': PC_IP, 'example.com': PC_IP, 'pool.ntp.org': PC_IP}   # the router's DNS: these names exist, everything else does not
        self.web = {}                                # port 80 on the fake PC: path -> (status, extra headers, body)

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
            if dport == 53 and dstip in (GW_IP, PC_IP) and len(data) > 12: return self.dns(src, sport, srcip, dstip, data)
            if dport == 123 and dstip == PC_IP and len(data) >= 48:                     # a time server: the real time now
                import time as _t
                r = bytearray(48); r[0] = 0x24; struct.pack_into('>I', r, 40, int(_t.time()) + 2208988800)
                self.events.append('ntp answered')
                u = struct.pack('>HHHH', 123, sport, 8 + 48, 0) + bytes(r)
                return self.send(self.ip_packet(dstip, srcip, 17, u, src, PC_MAC))
            if dport == 5140 and dstip == PC_IP:
                self.log_lines.append(data.decode(errors='replace')); self.events.append('PC log server got: %s' % self.log_lines[-1]); return
            if dport == 7:
                u = struct.pack('>HHHH', 7, sport, 8 + len(data), 0) + data
                self.send(self.ip_packet(dstip, srcip, 17, u, src, GW_MAC if dstip == GW_IP else PC_MAC)); return
        if proto == 6 and dstip == PC_IP and len(p) >= 20 and struct.unpack('>H', p[2:4])[0] == 80:
            return self.tcp_server(src, p, port=80)
        if proto == 6 and dstip == PC_IP and len(p) >= 20:
            return self.tcp_server(src, p)
        if proto == 1 and p and p[0] == 8 and dstip in (GW_IP, PC_IP):        # ping
            self.pings += 1
            r = bytearray(p); r[0] = 0; r[2:4] = b'\0\0'; struct.pack_into('>H', r, 2, csum(bytes(r)))
            self.events.append('ping to %s answered' % dstip)
            self.send(self.ip_packet(dstip, srcip, 1, bytes(r), src, GW_MAC if dstip == GW_IP else PC_MAC))


    # ---- a tiny TCP/HTTP server on the fake PC (port 8000) ----
    def tcp_packet(self, sport, dport, seq, ack, flags, data=b'', mss=False):
        hl = 24 if mss else 20
        h = bytearray(struct.pack('>HHIIBBHHH', sport, dport, seq & 0xffffffff, ack & 0xffffffff, (hl // 4) << 4, flags, 65535, 0, 0))
        if mss: h += struct.pack('>BBH', 2, 4, 1460)
        seg = bytes(h) + data
        ph = ip2b(PC_IP) + ip2b(GUEST_IP) + struct.pack('>BBH', 0, 6, len(seg))
        struct.pack_into('>H', h, 16, csum(ph + seg)); seg = bytes(h) + data
        return self.ip_packet(PC_IP, GUEST_IP, 6, seg, getattr(self, 'guest_mac', GUEST_MAC), PC_MAC)

    def dns(self, srcmac, sport, srcip, dstip, q):
        """Answer an A query (names in self.dns_names; anything else is NXDOMAIN)."""
        i = 12; labels = []
        while i < len(q) and q[i]:
            labels.append(q[i + 1:i + 1 + q[i]].decode(errors='replace')); i += 1 + q[i]
        name = '.'.join(labels); question = q[12:i + 5]
        ip = self.dns_names.get(name)
        hdr = q[0:2] + (b'\x81\x80' if ip else b'\x81\x83') + struct.pack('>HHHH', 1, 1 if ip else 0, 0, 0)
        ans = (b'\xc0\x0c' + struct.pack('>HHIH', 1, 1, 60, 4) + ip2b(ip)) if ip else b''
        self.events.append('dns %s -> %s' % (name, ip))
        u = struct.pack('>HHHH', 53, sport, 8 + len(hdr) + len(question) + len(ans), 0) + hdr + question + ans
        self.send(self.ip_packet(dstip, srcip, 17, u, srcmac, GW_MAC if dstip == GW_IP else PC_MAC))

    def tcp_server(self, srcmac, p, port=8000):
        sport, dport, seq, ack, off, fl = struct.unpack('>HHIIBB', p[:14]); hl = (off >> 4) * 4; data = p[hl:]
        if dport != port: return
        st = self.conns.get(sport)
        if fl & 2:                                                          # SYN
            mss = 1460                                                  # honour the MSS option in the client's SYN, like a real server
            o = hl + 0; i = 20
            while i + 1 < hl:
                if p[i] == 0: break
                if p[i] == 1: i += 1; continue
                if p[i] == 2 and p[i + 1] == 4: mss = struct.unpack('>H', p[i + 2:i + 4])[0]
                i += max(2, p[i + 1])
            st = self.conns[sport] = dict(snd=5000, rcv=seq + 1, sent=False, mss=mss)
            self.send(self.tcp_packet(port, sport, st['snd'], st['rcv'], 18, mss=True)); st['snd'] += 1
            return
        if not st: return
        if data and not st['sent']:
            st['rcv'] = seq + len(data); st['sent'] = True
            line = data.split(b'\r\n')[0].decode(errors='replace'); path = line.split(' ')[1] if ' ' in line else '/'
            self.http_log.append(path)
            if port == 80:                                      # the fake web server: (status, extra headers, body)
                status, extra, body = self.web.get(path, (404, '', b'<html><body><h1>Not Found</h1></body></html>'))
                resp = ('HTTP/1.0 %d X\r\nContent-Type: text/html\r\n%sContent-Length: %d\r\n\r\n' % (status, extra, len(body))).encode() + body
            else:
                body = self.routes.get(path)
                if body is None: resp = b'HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n'
                else: resp = b'HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n' % len(body) + body
            self.events.append('http GET %s -> %d bytes' % (path, len(resp)))
            m = st.get('mss', 1460)
            for i in range(0, len(resp), m):
                chunk = resp[i:i + m]; last = i + m >= len(resp)
                self.send(self.tcp_packet(port, sport, st['snd'], st['rcv'], 25 if last else 24, chunk))        # ACK|PSH (+FIN on the last)
                st['snd'] += len(chunk)
            st['snd'] += 1                                                  # the FIN
            return
        if fl & 1:                                                          # guest FIN: acknowledge it
            self.send(self.tcp_packet(port, sport, st['snd'], seq + 1, 16))
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
