"""Fake MT8173 I2C4 controller (+ its AP_DMA channel) with a fake Elan touchpad at address 0x15.

Models what wave-os's tp.h uses: the controller's registers (DMA mode, write / read / write-then-read), the DMA channel copying to and
from guest RAM, and the Elan I2C protocol (16-bit command registers, reset with a 2-byte hello, absolute mode, 34-byte finger reports
announced by pulling GPIO117 low). After absolute mode is switched on it plays a short scripted gesture: one finger sliding, two
fingers, a click."""
import struct

I2C_BASE = 0x11011000
DMA_PAGE = 0x11000000          # the AP_DMA block; the I2C4 channel is at +0x300

class FakeElan:
    ADDR = 0x15
    def __init__(self):
        self.regs = {0x0101: 0x0045, 0x0102: 0x0011, 0x0105: 0x1f2a, 0x0106: 0x0c80, 0x0107: 0x0710, 0x0108: 0x0022, 0x0307: 0x0001}
        self.pending = []      # byte strings waiting to be read (IRQ low while any)
        self.abs = False
        self.log = []
        self.script = []

    def irq_low(self): return bool(self.pending)

    def write(self, data):
        if len(data) == 4:                                   # command register write: reg, value
            reg, val = struct.unpack('<HH', data)
            self.log.append('write %#06x = %#06x' % (reg, val))
            if reg == 0x0005 and val == 0x0100:              # reset: answer with a hello (two zero bytes)
                self.pending = [b'\x00\x00']; self.abs = False
            elif reg == 0x0300:
                self.abs = bool(val & 1)
                if self.abs: self.make_script()
            elif reg == 0x0307:
                self.regs[0x0307] = val
            return True
        return len(data) == 2                                # a register address (the read follows)

    def read_reg(self, reg, n):
        if reg == 0x0001: out = struct.pack('<HH', 30, 0x0100) + bytes(26)              # device descriptor
        elif reg == 0x0002: out = bytes(n)                                              # report descriptor
        else: out = struct.pack('<H', self.regs.get(reg, 0))
        return (out + bytes(n))[:n]

    def read_plain(self, n):
        if self.pending: out = self.pending.pop(0)
        else: out = bytes(n)
        if not self.pending and self.abs and self.script: self.pending.append(self.script.pop(0))
        return (out + bytes(n))[:n]

    @staticmethod
    def report(fingers, button=False):
        r = bytearray(34); r[0] = 34; r[2] = 0x5d
        info = 0x01 if button else 0
        p = 4
        for i, (x, y) in enumerate(fingers[:5]):
            info |= 0x08 << i
            yy = 0x0710 - y                                  # the pad reports y from the bottom
            r[p] = ((x >> 4) & 0xf0) | ((yy >> 8) & 0x0f); r[p + 1] = x & 0xff; r[p + 2] = yy & 0xff; r[p + 3] = 0x22; r[p + 4] = 40
            p += 5
        r[3] = info
        return bytes(r)

    def make_script(self):
        s = []
        for k in range(20): s.append(self.report([(400 + k * 100, 300 + k * 50)]))           # one finger slides
        s.append(self.report([]))                                                         # lifted
        for k in range(10): s.append(self.report([(1000, 800 + k * 20), (1600, 800 + k * 20)]))   # two fingers (scroll)
        s.append(self.report([]))
        s.append(self.report([(1500, 900)], True)); s.append(self.report([(1500, 900)], False)); s.append(self.report([]))   # click
        self.script = s
        if not self.pending: self.pending.append(self.script.pop(0))


class FakeI2C:
    def __init__(self, machine):
        self.m = machine
        self.r = {}            # controller registers
        self.d = {}            # DMA channel registers (offsets inside the channel)
        self.elan = FakeElan()
        self.xfers = 0
        self.log = []

    def update_irq(self):
        din = self.m.regs.get(('gpio', 0x570), 0xffff)
        din = (din & ~(1 << 5)) | (0 if self.elan.irq_low() else 1 << 5)                  # GPIO117 = port 0x70 bit 5, active low
        self.m.regs[('gpio', 0x570)] = din

    def read(self, off):
        return self.r.get(off, 0)

    def write(self, off, val):
        val &= 0xffff
        if off == 0x0c: self.r[0x0c] = self.r.get(0x0c, 0) & ~val; return                 # INTR_STAT: write 1 to clear
        if off == 0x50 and val & 1: self.r = {}; return                                     # soft reset
        self.r[off] = val
        if off == 0x24 and val & 1: self.transfer()

    def dma_read(self, off): return self.d.get(off, 0)
    def dma_write(self, off, val):
        if off == 0x08 and not (val & 1): return
        self.d[off] = val

    def transfer(self):
        self.xfers += 1
        ctl, slave = self.r.get(0x10, 0), self.r.get(0x04, 0)
        addr, rd = slave >> 1, slave & 1
        wrrd = bool(ctl & (1 << 4)) and self.r.get(0x18, 1) == 2
        uc = self.m.uc
        if addr != FakeElan.ADDR:
            self.r[0x0c] = self.r.get(0x0c, 0) | 0x2                                        # ACK error: nothing at that address
            self.log.append('i2c %#04x: no answer' % addr); self.r[0x24] = 0; return
        if wrrd:
            wlen, rlen = self.r.get(0x14, 0), self.r.get(0x6c, 0)
            w = bytes(uc.mem_read(self.d.get(0x1c, 0), wlen))
            self.elan.write(w)
            data = self.elan.read_reg(struct.unpack('<H', w[:2])[0] if len(w) >= 2 else 0, rlen)
            uc.mem_write(self.d.get(0x20, 0), data)
            self.log.append('i2c read reg %s -> %d bytes' % (w[:2].hex(), rlen))
        elif rd:
            n = self.r.get(0x14, 0)
            uc.mem_write(self.d.get(0x20, 0), self.elan.read_plain(n))
        else:
            n = self.r.get(0x14, 0)
            self.elan.write(bytes(uc.mem_read(self.d.get(0x1c, 0), n)))
        self.d[0x08] = 0                                                                   # DMA done
        self.r[0x0c] = self.r.get(0x0c, 0) | 0x1                                            # transfer complete
        self.r[0x24] = 0
        self.update_irq()
