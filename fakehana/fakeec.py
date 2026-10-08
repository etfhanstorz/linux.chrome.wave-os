"""Fake MT8173 SPI controller + Chrome EC (host command protocol v3) with a scripted keyboard.

SPI register behaviour mirrors Linux drivers/spi/spi-mt65xx.c (FIFO mode); the EC's SPI byte stream
mirrors what drivers/platform/chrome/cros_ec_spi.c expects:
  while receiving a request the EC clocks out EC_SPI_RECEIVING (0xf9); then a few
  EC_SPI_PROCESSING (0xfa) bytes, EC_SPI_FRAME_START (0xec), the response, then EC_SPI_PAST_END (0xed).
"""
import struct
try:
    from . import keymap
except ImportError:
    import keymap

EC_RES_SUCCESS, EC_RES_INVALID_COMMAND, EC_RES_INVALID_PARAM, EC_RES_INVALID_CHECKSUM = 0, 1, 3, 8

class KeyScript:
    """Turn text into timed matrix presses. Each char: press at t, release at t+60 ms, next at t+120 ms."""
    def __init__(self, text, start_s=1.0):
        self.events = []          # (time_s, row, col, down)
        pos = {}
        for (r, c) in keymap.KEYS:
            n, s = keymap.NORM[r][c], keymap.SHIFTED[r][c]
            if n >= ' ' or n in ('\n', '\b', '\t'):
                pos.setdefault(n, (r, c, False))
            if s != n and s >= ' ':
                pos.setdefault(s, (r, c, True))
        shift = next(rc for rc, k in keymap.KEYS.items() if k == 'LEFTSHIFT')
        t = start_s
        for ch in text:
            if ch == '\x01':                              # pause token: 5 simulated seconds without typing (lets slow commands finish)
                t += 5.0
                continue
            if ch not in pos:
                raise ValueError('fake keyboard has no key for %r' % ch)
            r, c, shifted = pos[ch]
            if shifted:
                self.events.append((t - 0.02, shift[0], shift[1], True))
            self.events.append((t, r, c, True))
            self.events.append((t + 0.06, r, c, False))
            if shifted:
                self.events.append((t + 0.08, shift[0], shift[1], False))
            t += 0.12
        self.end_s = t

    def matrix(self, now_s):
        cols = [0] * keymap.COLS
        down = {}
        for t, r, c, d in self.events:
            if t <= now_s:
                down[(r, c)] = d
        for (r, c), d in down.items():
            if d:
                cols[c] |= 1 << r
        return bytes(cols)

class FakeEC:
    def __init__(self, machine, keys):
        self.m, self.keys = machine, keys
        self.rx = bytearray()     # bytes received from the host in this chip-select session
        self.out = []             # queued response bytes
        self.need = None
        self.commands = []        # (command, result) log

    def cs_begin(self):
        self.rx = bytearray(); self.out = []; self.need = None

    def byte(self, b):
        """Host clocks one byte in; return the byte the EC clocks out."""
        if getattr(self.m.a, 'ec', 'on') == 'off':
            return 0xff                                      # scenario: EC silent (line idles high)
        if self.out:
            return self.out.pop(0)
        if self.need is not None and len(self.rx) >= self.need:
            return 0xed                                      # EC_SPI_PAST_END
        self.rx.append(b)
        if len(self.rx) == 8:
            if self.rx[0] != 3:
                self.need = 8
                self._respond(EC_RES_INVALID_PARAM, b'')
                return 0xf9
            self.need = 8 + (self.rx[6] | self.rx[7] << 8)
        if self.need is not None and len(self.rx) == self.need:
            self._process()
        return 0xf9                                          # EC_SPI_RECEIVING

    def _respond(self, result, data):
        hdr = bytearray(struct.pack('<BBHHH', 3, 0, result, len(data), 0))
        hdr[1] = (-(sum(hdr) + sum(data))) & 0xff
        # The real EC takes a varying time to answer, so the frame start byte lands anywhere in a
        # 32-byte chunk. Vary it (deterministically) so drivers meet every alignment.
        busy = (len(self.commands) * 7) % 45 + 1
        self.out = [0xfa] * busy + [0xec] + list(hdr) + list(data)

    def _process(self):
        req = bytes(self.rx)
        cmd, ver, n = struct.unpack_from('<HBxH', req, 2)
        params = req[8:8 + n]
        if sum(req) & 0xff:
            self.commands.append((cmd, EC_RES_INVALID_CHECKSUM)); self._respond(EC_RES_INVALID_CHECKSUM, b''); return
        now_s = self.m.ticks / self.m.timer_hz
        if cmd == 0x0001 and n >= 4:                         # EC_CMD_HELLO
            res, data = 0, struct.pack('<I', (struct.unpack('<I', params[:4])[0] + 0x01020304) & 0xffffffff)
        elif cmd == 0x0002:                                  # EC_CMD_GET_VERSION
            res, data = 0, b'hana_fake_ro'.ljust(32, b'\0') + b'hana_fake_rw'.ljust(32, b'\0') + b'\0' * 32 + struct.pack('<I', 2)
        elif cmd == 0x0061:                                  # EC_CMD_MKBP_INFO
            res, data = 0, struct.pack('<IIB', keymap.ROWS, keymap.COLS, 0)
        elif cmd == 0x0007 and n >= 2:                       # EC_CMD_READ_MEMMAP: a battery at 75 %, on the charger
            mm = bytearray(256)
            struct.pack_into('<IIIB', mm, 0x40, 12000, 500, 3000, 0x0b)
            struct.pack_into('<III', mm, 0x50, 4200, 11400, 4000)
            res, data = 0, bytes(mm[params[0]:params[0] + params[1]])
        elif cmd == 0x0060:                                  # EC_CMD_MKBP_STATE
            res, data = 0, self.keys.matrix(now_s) if self.keys else bytes(keymap.COLS)
        else:
            res, data = EC_RES_INVALID_COMMAND, b''
        self.commands.append((cmd, res))
        self._respond(res, data)

class FakeSPI:
    """MT8173 SPI controller at 0x1100a000 (FIFO mode, polled)."""
    def __init__(self, ec):
        self.ec = ec
        self.regs = {0x00: 0x00010101, 0x04: 0, 0x18: 0x3000, 0x24: 1}
        self.tx, self.rxq = [], []
        self.status = 0
        self.active = False       # chip-select asserted
        self.transfers = 0
        # MODEL (from wave-os v0.8 on the real hana): after a chunk whose length is not a multiple
        # of 4, the next chunk in the same chip-select session never completes (STATUS0 stays 0).
        self.odd_pending = False
        self.jams = 0
        self.stall = 0                                       # set from --spi-stall N

    def read(self, off):
        if off == 0x14:                                      # SPI_RX_DATA
            w = 0
            for j in range(4):
                w |= (self.rxq.pop(0) if self.rxq else 0) << (8 * j)
            return w
        if off == 0x1c:                                      # SPI_STATUS0: read clears
            s, self.status = self.status, 0
            return s
        return self.regs.get(off, 0)

    def write(self, off, val):
        if off == 0x10:                                      # SPI_TX_DATA
            for j in range(4):
                self.tx.append((val >> (8 * j)) & 0xff)
            return
        if off != 0x18:
            self.regs[off] = val
            return
        old = self.regs.get(0x18, 0)
        self.regs[0x18] = val & ~0x3                         # ACT/RESUME are pulses
        if val & 0x4:                                        # SPI_CMD_RST
            self.tx, self.rxq, self.status = [], [], 0
            self.odd_pending = False
        if (old & 0x10) and not (val & 0x10) and self.active:
            self.active = False                              # PAUSE_EN cleared: chip-select released
        if val & 0x3:                                        # ACT or RESUME: run one packet
            if not self.active:
                self.active = True
                self.ec.cs_begin()
            n = ((self.regs.get(0x04, 0) >> 16) & 0x3ff) + 1
            if self.stall > 0:                               # scenario: first N packets never complete (v1.4 on hana)
                self.stall -= 1
                self.stall_hits = getattr(self, 'stall_hits', 0) + 1
                self.tx = []
                return
            if self.odd_pending:
                self.jams += 1                               # jammed: status never comes
                return
            if n % 4 and (val & 0x10):
                self.odd_pending = True
            data = (self.tx + [0] * n)[:n]
            self.tx = self.tx[n:] if len(self.tx) > n else []
            self.rxq = [self.ec.byte(b) for b in data]
            self.transfers += 1
            if val & 0x10:
                self.status = 2                              # paused: chip-select still held
            else:
                self.status = 1                              # finished: chip-select released
                self.active = False
