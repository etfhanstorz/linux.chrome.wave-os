"""Fake MSDC3 controller (PIO, polled) + a fake Marvell 88W8897 SDIO card behind it.

Register behaviour follows Linux drivers/mmc/host/mtk-sd.c. The card only answers once it is powered
the way the real board needs: bus supply VGP3 on (MT6397 DIGLDO_CON7 bit 15) and chip power GPIO85
driven high in GPIO mode. Otherwise every command times out (MSDC_INT CMDTMO), like an empty slot.
"""
import struct

# A tiny CIS: CISTPL_MANFID (0x20) with Marvell's vendor id 0x02df and the 88W8897 device id 0x912d
CIS_ADDR = 0x1000
CIS = bytes([0x21, 0x02, 0x0c, 0x00,               # CISTPL_FUNCID: network
             0x20, 0x04, 0xdf, 0x02, 0x2d, 0x91,   # CISTPL_MANFID
             0xff])

class FakeCard:
    def __init__(self):
        self.state = 'idle'        # idle -> ready -> ident -> transfer
        self.ocr_polls = 0
        self.rca = 0x0001
        self.cccr = {0x00: 0x43, 0x01: 0x03, 0x02: 0x00, 0x03: 0x00, 0x08: 0x13,
                     0x09: CIS_ADDR & 0xff, 0x0a: (CIS_ADDR >> 8) & 0xff, 0x0b: 0}

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
            if fn == 0 and not write:
                if CIS_ADDR <= reg < CIS_ADDR + len(CIS):
                    data = CIS[reg - CIS_ADDR]
                else:
                    data = self.cccr.get(reg, 0)
                return 0x1000 | data
            return 0x1000
        return None

class FakeMSDC:
    def __init__(self, machine, regs):
        self.m = machine
        self.r = regs              # shared register dict in the machine, keys ('msdc3', off)
        self.card = FakeCard()
        self.commands = []         # (opcode, answered)

    def powered(self):
        m = self.m
        vgp3_on = (m.pmic.get(0x041e, 0) >> 15) & 1
        return vgp3_on and m.pinmode(85) == 0 and m.pin(85)

    def read(self, off):
        v = self.r.get(('msdc3', off), 0)
        if off == 0x00:
            v |= 1 << 7                               # CKSTB: clock stable
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
        if off == 0x34:                               # SDC_CMD: run the command now
            op, arg = val & 0x3f, r.get(('msdc3', 0x38), 0)
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
