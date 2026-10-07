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
    def __init__(self, firmware):
        self.fw = firmware
        self.pos = 0                    # bytes of firmware received so far
        self.chunks = [24] + [2312, 1156, 2312, 1000, 2312, 256, 2310]   # sizes the ROM asks for, then repeats
        self.i = 0
        self.last = None
        self.stats = dict(writes=0, bytes=0, bad=0)
        self.cfg = {0xcd: 0, 0xb8: 0, 0xb9: 0, 0x01: 0, 0xcc: 0}
        self.acked = False              # MODEL: the ROM signals "download ready" only after the host
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

    def reg(self, r):
        if r == 0x03:
            self.acked = True                            # reading the status register acknowledges the boot interrupt
            return 0x01
        if r == 0x50:
            return 0x09 if self.ready() else 0x08        # card io ready (+ download ready)
        if r in (0x60, 0x61):
            n = self.want() + (self.want() & 1)          # ROM reports even lengths
            n = self.want()
            return (n >> (8 * (r - 0x60))) & 0xff
        if r in (0xc0, 0xc1):
            done = self.pos >= len(self.fw)
            return (0xfedc >> (8 * (r - 0xc0))) & 0xff if done else 0
        return self.cfg.get(r, 0)

    def write_reg(self, r, v):
        if r in self.cfg:
            self.cfg[r] = v

    def port_write(self, data):
        """A CMD53 block write to the memory port: must be exactly what the ROM asked for."""
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
    def __init__(self, machine, regs, firmware=None):
        self.m = machine
        self.r = regs              # shared register dict in the machine, keys ('msdc3', off)
        self.card = FakeCard()
        self.commands = []         # (opcode, answered)
        self.loader = FakeFirmwareLoader(firmware) if firmware else None
        self.card.loader = self.loader
        self.tx = []               # bytes written to the TX FIFO for the current data command
        self.want_bytes = 0
        self.port_cmd = None

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
            v = 0                                     # FIFO drains instantly: TXCNT/RXCNT 0
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
                    self.card.loader.port_write(bytes(self.tx[:self.want_bytes]))
                r[('msdc3', 0x0c)] = r.get(('msdc3', 0x0c), 0) | (1 << 12)     # XFER_COMPL
                self.port_cmd = None
            return True
        if off == 0x34:                               # SDC_CMD: run the command now
            op, arg = val & 0x3f, r.get(('msdc3', 0x38), 0)
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
