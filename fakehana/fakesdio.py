"""Fake MSDC3 controller (PIO, polled) + a fake Marvell 88W8897 SDIO card behind it.

Register behaviour follows Linux drivers/mmc/host/mtk-sd.c. The card only answers once it is powered
the way the real board needs: bus supply VGP3 on (MT6397 DIGLDO_CON7 bit 15) and chip power GPIO85
driven high in GPIO mode. Otherwise every command times out (MSDC_INT CMDTMO), like an empty slot.
"""
import struct

# Tiny CIS chains: CISTPL_MANFID (0x20) with Marvell's vendor id 0x02df. Like the real hana chip
# (measured by v1.2): the card (function 0) says 0x912c; the Wi-Fi function (1) says 0x912d.
CIS_ADDR, CIS1_ADDR = 0x1000, 0x2000
CIS = bytes([0x21, 0x02, 0x0c, 0x00,               # CISTPL_FUNCID: network
             0x20, 0x04, 0xdf, 0x02, 0x2c, 0x91,   # CISTPL_MANFID: card 0x912c
             0xff])
CIS1 = bytes([0x20, 0x04, 0xdf, 0x02, 0x2d, 0x91, 0xff])   # function 1: Wi-Fi 0x912d

class FakeCard:
    def __init__(self):
        self.state = 'idle'        # idle -> ready -> ident -> transfer
        self.ocr_polls = 0
        self.rca = 0x0001
        self.cccr = {0x00: 0x43, 0x01: 0x03, 0x02: 0x00, 0x03: 0x00, 0x08: 0x13,
                     0x09: CIS_ADDR & 0xff, 0x0a: (CIS_ADDR >> 8) & 0xff, 0x0b: 0,
                     0x109: CIS1_ADDR & 0xff, 0x10a: (CIS1_ADDR >> 8) & 0xff, 0x10b: 0}

    def command(self, op, arg):
        """Return the 32-bit response register value (bits 39:8 of the response), or None (timeout)."""
        if op == 0:
            self.state = 'idle'
            return 0
        if op == 5:                                   # IO_SEND_OP_COND -> R4
            ocr = 0x20ff8000                          # 2 functions, 2.7-3.6 V
            if arg:
                self.ocr_polls += 1
                if self.ocr_polls >= 2:               # ready after a couple of polls
                    self.state = 'ready'
                    return ocr | 0x80000000
            return ocr
        if op == 3 and self.state in ('ready', 'ident'):
            self.state = 'ident'
            return (self.rca << 16) | 0x0500          # R6
        if op == 7 and (arg >> 16) == self.rca:
            self.state = 'transfer'
            return 0x00000700
        if op == 52 and self.state == 'transfer':     # IO_RW_DIRECT -> R5: flags 0x10 (CMD state) + data
            fn, reg = (arg >> 28) & 7, (arg >> 9) & 0x1ffff
            write = arg >> 31
            if fn == 1 and getattr(self, 'loader', None):
                if write:
                    self.loader.write_reg(reg, arg & 0xff)
                    return 0x1000 | (arg & 0xff)
                return 0x1000 | self.loader.reg(reg)
            if fn == 0 and write:
                self.cccr[reg] = arg & 0xff
                if reg == 0x02:
                    self.cccr[0x03] = arg & 0xff      # functions report ready right after enabling
                return 0x1000 | (arg & 0xff)
            if fn == 0:
                if CIS_ADDR <= reg < CIS_ADDR + len(CIS):
                    data = CIS[reg - CIS_ADDR]
                elif CIS1_ADDR <= reg < CIS1_ADDR + len(CIS1):
                    data = CIS1[reg - CIS1_ADDR]
                else:
                    data = self.cccr.get(reg, 0)
                return 0x1000 | data
            return 0x1000
        return None

class FakeFirmwareLoader:
    """Function-1 registers of the 88W8897 boot ROM (numbers from Linux mwifiex_reg_sd8897)."""
    def __init__(self, firmware, running=False):
        self.fw = firmware
        self.running = running          # MODEL (real hana): the chip comes up with its firmware already running
        self.hung = running             # ...left over from ChromeOS, mid-conversation: it ignores new commands until power-cycled
        self.pos = 0                    # bytes of firmware received so far
        self.chunks = [24] + [2312, 1156, 2312, 1000, 2312, 256, 2310]   # sizes the ROM asks for, then repeats
        self.i = 0
        self.last = None
        self.stats = dict(writes=0, bytes=0, bad=0)
        self.cfg = {0xcd: 0, 0xb8: 0, 0xb9: 0, 0x01: 0, 0xcc: 0}
        self.mask = 0                    # reg 0x02: host interrupt mask (MODEL: status bits only show when unmasked, as in Linux mwifiex)
        self.int_status = 0              # reg 0x03: bit 6 = a packet waits on the command port
        self.cmd_resp = b''
        self.queue = []                  # packets waiting for the host on the command port (answers + events)
        self.cmds = []                   # host commands seen: (command, result)
        self.mask = 0                    # reg 0x02: host interrupt mask (MODEL: status bits only show when unmasked, as in Linux mwifiex)
        self.int_status = 0              # reg 0x03: bit 6 = a packet waits on the command port\n        self.cmd_resp = b''\n        self.cmds = []                   # host commands seen: (command, result)\n        self.acked = False              # MODEL: the ROM signals "download ready" only after the host
                                        # acknowledged its boot interrupt (read 0x03), set reset-on-read
                                        # (0x01) and auto re-enable (0xcc bit 4), as Linux mwifiex does.
                                        # (v1.5 skipped these on the real hana: WIFI-09, chip stopped asking.)

    def ready(self):
        return self.acked and (self.cfg[0x01] & 0xff) == 0xff and (self.cfg[0xcc] & 0x10)

    def want(self):
        if self.pos >= len(self.fw):
            return 0
        n = self.chunks[self.i] if self.i < len(self.chunks) else 2312
        return min(n, len(self.fw) - self.pos)

    def cold_boot(self):
        """Power-cycled: the boot ROM runs and waits for a firmware download."""
        self.running = self.hung = False
        self.pos = 0; self.i = 0; self.acked = False
        self.cfg = {0xcd: 0, 0xb8: 0, 0xb9: 0, 0x01: 0, 0xcc: 0}
        self.mask = 0; self.int_status = 0; self.cmd_resp = b''; self.queue = []
        self.cold_boots = getattr(self, 'cold_boots', 0) + 1

    def host_command(self, d):
        import struct
        if self.hung:
            return                                       # no answer
        total, typ = struct.unpack_from('<HH', d, 0)
        cmd, size, seq, _ = struct.unpack_from('<HHHH', d, 4)
        body = b''
        result = 0
        extra_events = []
        if cmd == 0x00a9:                                # FUNC_INIT
            body = b''
        elif cmd == 0x0003:                              # GET_HW_SPEC
            body = struct.pack('<HHHH6sHHI', 0x0001, 0x0067, 0, 32, bytes.fromhex('0050431a2b3c'), 0x10, 2, 0x0f444c11).ljust(63, b'\0')
        elif cmd == 0x0028:                              # MAC_CONTROL: echo
            body = d[12:12 + max(0, size - 8)]
        elif cmd == 0x0107:                              # EXT SCAN: ack now, results arrive as events (id 0x58)
            tl = d[12 + 4:12 + max(4, size - 8)]
            p, chans = 0, []
            while p + 4 <= len(tl):
                ttype, tlen = struct.unpack_from('<HH', tl, p)
                if ttype == 0x0101:
                    chans = [(tl[p + 4 + k * 6], tl[p + 4 + k * 6 + 1], tl[p + 4 + k * 6 + 2]) for k in range(tlen // 6)]
                p += 4 + tlen
            ok = bool(chans) and all(m & 2 for _, _, m in chans)       # the channel filter must be disabled
            allaps = [(b'HomeNet', '02:11:22:33:44:01', -52, 6, True), (b'CoffeeShop-Guest', '02:11:22:33:44:02', -71, 1, False),
                      (b'Neighbour5G', '02:11:22:33:44:03', -80, 149, True), (b'', '02:11:22:33:44:04', -85, 11, True)]

            def mk(ssid, mac, rssi, ch, sec):
                ies = bytes([0, len(ssid)]) + ssid + bytes([3, 1, ch]) + (bytes([48, 4, 1, 0, 0, 0]) if sec else b'')
                frame = struct.pack('<QHH', 123456789, 100, 0x0411 if sec else 0x0401) + ies
                bssid = bytes.fromhex(mac.replace(':', ''))
                t1 = struct.pack('<HH', 0x0156, 6 + len(frame)) + bssid + frame
                info = struct.pack('<hhBBB', rssi, 0, 0, 0 if ch < 36 else 1, ch).ljust(18, b'\0')
                t2 = struct.pack('<HH', 0x0157, len(info)) + info
                return t1 + t2
            mine = [a for a in allaps if ((a[3] >= 36) == (chans[0][0] == 1))] if ok else []
            for i in range(0, max(1, len(mine)), 2):
                part = mine[i:i + 2]
                tlvs = b''.join(mk(*a) for a in part)
                last = i + 2 >= len(mine)
                hdr = struct.pack('<HBBB3sHB', 0x58, 0, 0, 0 if last else 1, b'\0\0\0', len(tlvs), len(part))
                extra_events.append(struct.pack('<HH', 4 + len(hdr) + len(tlvs), 3) + hdr + tlvs)
        elif cmd == 0x0006:                              # legacy SCAN: a few invented access points
            aps = [(b'HomeNet', '02:11:22:33:44:01', 52, 6, True), (b'CoffeeShop-Guest', '02:11:22:33:44:02', 71, 1, False),
                   (b'Neighbour5G', '02:11:22:33:44:03', 80, 149, True), (b'', '02:11:22:33:44:04', 85, 11, True)]
            # MODEL (real hana, v1.11): without the 'disable channel filter' bit (0x02) on the channels, the firmware's
            # regulatory filter drops everything and the scan finds nothing (but returns no error).
            filt_off = False
            p = 12 + 7
            while p + 4 <= len(d):
                ttype, tlen = struct.unpack_from('<HH', d, p)
                if ttype == 0x0101:
                    chans = [d[p + 4 + k * 6 + 2] for k in range(tlen // 6)]
                    filt_off = bool(chans) and all(m & 2 for m in chans)
                    break
                p += 4 + tlen
            aps = []                                     # MODEL (real hana, v1.12): the legacy scan command finds nothing on this firmware
            recs = b''
            for ssid, mac, rssi, ch, sec in aps:
                ies = bytes([0, len(ssid)]) + ssid + bytes([3, 1, ch]) + (bytes([48, 4, 1, 0, 0, 0]) if sec else b'')
                rec = bytes.fromhex(mac.replace(':', '')) + bytes([rssi]) + struct.pack('<QHH', 123456789, 100, 0x0411 if sec else 0x0401) + ies
                recs += struct.pack('<H', len(rec)) + rec
            body = struct.pack('<HB', len(recs), len(aps)) + recs
        else:
            result = 1
        resp = struct.pack('<HHHHHH', 12 + len(body), 1, cmd | 0x8000, 8 + len(body), seq, result) + body
        self.cmds.append((cmd, result))
        self.queue.append(resp)
        for ev in extra_events:
            self.queue.append(ev)
        self.int_status = 0x40

    def port_read(self, addr, nbytes):
        if addr == 0x18000 and self.queue:
            r = self.queue.pop(0)
            if self.queue:
                self.int_status = 0x40                   # the next packet is waiting
            return r.ljust(nbytes, b'\0')[:nbytes]
        return bytes(nbytes)

    def reg(self, r):
        if r == 0x03:
            self.acked = True                            # reading the status register acknowledges the boot interrupt
            v, self.int_status = (self.int_status or 0x01), 0    # reset on read
            return v if self.mask & 0x40 or v == 0x01 else 0
        if r == 0x02:
            return self.mask
        if r in (0xb4, 0xb5):
            n = len(self.queue[0]) if self.queue else 0
            return (n >> (8 * (r - 0xb4))) & 0xff
        if r == 0x50:
            if self.running:
                return 0x08                              # running firmware never asks for a download
            return 0x09 if self.ready() else 0x08        # card io ready (+ download ready)
        if r in (0x60, 0x61):
            n = self.want() + (self.want() & 1)          # ROM reports even lengths
            n = self.want()
            return (n >> (8 * (r - 0x60))) & 0xff
        if r in (0xc0, 0xc1):
            done = self.running or self.pos >= len(self.fw)
            return (0xfedc >> (8 * (r - 0xc0))) & 0xff if done else 0
        return self.cfg.get(r, 0)

    def write_reg(self, r, v):
        if r == 0x02:
            self.mask = v
        if r in self.cfg:
            self.cfg[r] = v

    def port_write(self, data, addr=0x10000):
        """A CMD53 block write to the memory port: must be exactly what the ROM asked for."""
        if addr == 0x18000:                              # command port of the running firmware
            self.host_command(data)
            return
        n = self.want()
        self.stats['writes'] += 1
        if not (self.cfg[0xcd] & 1) or len(data) < n:
            self.stats['bad'] += 1
            return
        if data[:n] != self.fw[self.pos:self.pos + n]:
            self.stats['bad'] += 1
            return
        self.pos += n
        self.i += 1
        self.stats['bytes'] += n

class FakeMSDC:
    def __init__(self, machine, regs, firmware=None, fw_running=False):
        self.m = machine
        self.r = regs              # shared register dict in the machine, keys ('msdc3', off)
        self.card = FakeCard()
        self.commands = []         # (opcode, answered)
        self.loader = FakeFirmwareLoader(firmware, fw_running) if firmware else None
        self.card.loader = self.loader
        self.rx = []               # bytes waiting in the RX FIFO (CMD53 reads)
        self.tx = []               # bytes written to the TX FIFO for the current data command
        self.want_bytes = 0
        self.port_cmd = None

    def update_power(self):
        on = self.powered()
        if not on:
            self.was_off = True
        elif getattr(self, 'was_off', False):
            self.was_off = False
            if self.card.loader:
                self.card.loader.cold_boot()
            self.card.state = 'idle'; self.card.ocr_polls = 0
            self.power_cycles = getattr(self, 'power_cycles', 0) + 1

    def powered(self):
        # Chip power: GPIO85 is the ACTIVE-LOW enable of sdio_fixed_3v3 ("WIFI_PDN"): low = on.
        # (v0.11 drove it high and the real chip stayed silent: WIFI-03.)
        m = self.m
        vgp3_on = (m.pmic.get(0x041e, 0) >> 15) & 1
        return vgp3_on and m.pinmode(85) == 0 and not m.pin(85)

    def read(self, off):
        v = self.r.get(('msdc3', off), 0)
        if off == 0x00:
            v |= 1 << 7                               # CKSTB: clock stable
        if off == 0x14:
            v = min(len(self.rx), 128)                # RXCNT; TX FIFO drains instantly (TXCNT 0)
        if off == 0x1c:                               # MSDC_RXDATA: pop a word
            w = 0
            for j in range(4):
                w |= (self.rx.pop(0) if self.rx else 0) << (8 * j)
            return w
        return v

    def write(self, off, val):
        """Returns True if handled (don't store raw)."""
        r = self.r
        if off == 0x00:
            r[('msdc3', 0)] = val & ~(1 << 2)         # RST self-clears
            return True
        if off == 0x0c:                               # MSDC_INT: write 1 to clear
            r[('msdc3', 0x0c)] = r.get(('msdc3', 0x0c), 0) & ~val
            return True
        if off == 0x14:                               # FIFOCS: CLR self-clears
            r[('msdc3', 0x14)] = val & ~(1 << 31)
            return True
        if off == 0x18:                               # MSDC_TXDATA: a word into the TX FIFO
            self.tx += [(val >> (8 * j)) & 0xff for j in range(4)]
            if self.port_cmd and len(self.tx) >= self.want_bytes:
                if self.card.loader:
                    self.card.loader.port_write(bytes(self.tx[:self.want_bytes]), self.port_cmd)
                r[('msdc3', 0x0c)] = r.get(('msdc3', 0x0c), 0) | (1 << 12)     # XFER_COMPL
                self.port_cmd = None
            return True
        if off == 0x34:                               # SDC_CMD: run the command now
            op, arg = val & 0x3f, r.get(('msdc3', 0x38), 0)
            if op == 53 and not (val >> 13) & 1 and self.powered() and self.card.loader:   # CMD53 read
                n = ((val >> 16) & 0xfff) * r.get(('msdc3', 0x50), 1)
                self.rx = list(self.card.loader.port_read((arg >> 9) & 0x1ffff, n))
                self.commands.append((op, True))
                r[('msdc3', 0x40)] = 0x1000
                r[('msdc3', 0x0c)] = r.get(('msdc3', 0x0c), 0) | (1 << 8) | (1 << 12)   # CMDRDY + XFER_COMPL
                return True
            if op == 53 and (val >> 13) & 1 and self.powered():      # CMD53 write: data follows through the FIFO
                self.tx = []
                self.want_bytes = ((val >> 16) & 0xfff) * r.get(('msdc3', 0x50), 1)
                self.port_cmd = (arg >> 9) & 0x1ffff
                self.commands.append((op, True))
                r[('msdc3', 0x40)] = 0x1000
                r[('msdc3', 0x0c)] = r.get(('msdc3', 0x0c), 0) | (1 << 8)  # CMDRDY
                return True
            resp = self.card.command(op, arg) if self.powered() else None
            if (val >> 7) & 7 == 0:                   # no response expected: always "done"
                resp = 0
            self.commands.append((op, resp is not None))
            if resp is None:
                r[('msdc3', 0x0c)] = r.get(('msdc3', 0x0c), 0) | (1 << 9)    # CMDTMO
            else:
                r[('msdc3', 0x40)] = resp
                r[('msdc3', 0x0c)] = r.get(('msdc3', 0x0c), 0) | (1 << 8)    # CMDRDY
            r[('msdc3', 0x34)] = val
            return True
        return False
