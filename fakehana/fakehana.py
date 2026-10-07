#!/usr/bin/env python3
"""fakehana: a fake Lenovo MT8173 "hana" Chromebook for testing wave-os without flashing.

What it imitates (best guesses where marked GUESS -- the real hardware is still the final test):
  * depthcharge's hand-off: unpack the signed kpart, read the FIT, pick the default config,
    add the DT nodes the firmware adds (bootargs, ramoops, optional coreboot table),
    place the arm64 Image at RAM + text_offset and jump to it with x0 = device tree.
  * RAM at 0x40000000 (real hana has 4 GB; the first 3 GB are modelled).
  * Display/backlight registers preset to the values wave-os v0.6.2 measured on the real machine.
  * ramoops exactly as ChromeOS uses it on hana (1 MB at 0xb1f00000), NOT in the device tree (as on the real one).
  * MediaTek peripherals at their real addresses: watchdog, display (MMSYS/OVL/RDMA/DSI),
    display PWM, GPIO. Every access is logged.
  * PSCI SYSTEM_RESET/OFF via smc, and the 13 MHz generic timer (fake, runs fast).
  * Scenarios: display left on/off by the firmware, whether a watchdog reset wipes RAM.

Output: what happened (reboot / freeze / crash), the register log, what ChromeOS would show
in /sys/fs/pstore/console-ramoops-0 afterwards, and a PNG of what the panel would show.

Usage: python3 fakehana.py ../out.kpart [--display off] [--coreboot] [--wdt-keeps-ram] [--png out.png]
"""
import argparse, os, struct, subprocess, sys, tempfile, zlib

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_INSN
from unicorn.arm64_const import *
from fakeec import FakeEC, FakeSPI, KeyScript

RAM_BASE, RAM_SIZE = 0x40000000, 0xc0000000          # first 3 GB of the real 4 GB
TIMER_HZ = 13_000_000                                # MT8173 system counter
DT_ADDR = 0x5ff00000                                 # real: x0 seen by wave-os v0.6.2 on hana
FB_ADDR = 0xfdaff000                                 # real: OVL0 layer 0 address on hana
FB_W, FB_H = 1366, 768                               # hana panel
CB_TABLE = 0x7cff0000                                # GUESS: coreboot table location
RAMOOPS = dict(base=0xb1f00000, size=0x100000, record=0x20000, console=0x20000, pmsg=0x20000)  # real: ChromeOS /sys/module/ramoops/parameters on hana

def sh(*cmd, inp=None):
    return subprocess.run(cmd, input=inp, capture_output=True, check=True).stdout

# ---------------------------------------------------------------- firmware side (depthcharge-ish)
def unpack_kpart(path, tmp):
    d = open(path, 'rb').read()
    kb_size = struct.unpack_from('<Q', d, 16)[0]
    pre = kb_size
    pre_size, = struct.unpack_from('<Q', d, pre)
    body_load, bl_addr = struct.unpack_from('<QQ', d, pre + 48)
    body = pre + pre_size
    fit_len = struct.unpack_from('>I', d, body + 4)[0]
    assert d[body:body + 4] == b'\xd0\x0d\xfe\xed', 'no FIT image at the start of the kernel body'
    fit = d[body:body + fit_len]
    # Body layout from futility: [vmlinuz][cmdline 4K][params 4K][bootloader]
    cmdline = ''
    for back in (8192, 4096):
        cfg_off = body + (bl_addr - body_load) - back
        s = d[cfg_off:cfg_off + 4096].split(b'\0')[0]
        if s and all(32 <= c < 127 for c in s):
            cmdline = s.decode().strip()
            break
    p = os.path.join(tmp, 'image.itb'); open(p, 'wb').write(fit)
    return p, cmdline, kb_size, pre_size

def fit_pick(itb, tmp):
    get = lambda node, prop: sh('fdtget', itb, node, prop).decode().strip()
    default = get('/configurations', 'default')
    conf = '/configurations/' + default
    kname, fname = get(conf, 'kernel'), get(conf, 'fdt').split()[0]
    images = sh('fdtget', '-l', itb, '/images').decode().split()
    def extract(name):
        out = os.path.join(tmp, name.replace('@', '_') + '.bin')
        sh('dumpimage', '-T', 'flat_dt', '-p', str(images.index(name)), '-o', out, itb)
        return open(out, 'rb').read()
    info = dict(default=default, kernel=kname, fdt=fname,
                ktype=get('/images/' + kname, 'type'), kcomp=get('/images/' + kname, 'compression'),
                fdt_desc=get('/images/' + fname, 'description'))
    return extract(kname), extract(fname), info

def fixup_dt(dtb, cmdline, tmp, coreboot, ramoops_in_dt):
    """Add what depthcharge adds. Done by decompiling and re-compiling with extra node blocks."""
    src = sh('dtc', '-q', '-I', 'dtb', '-O', 'dts', '-', inp=dtb).decode()
    r = RAMOOPS
    extra = '\n/ {\n  chosen { bootargs = "%s"; };\n' % cmdline.replace('"', '\\"')
    if ramoops_in_dt:
      extra += ('  reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges;\n'
              '    ramoops@%x { compatible = "ramoops"; reg = <0 0x%x 0 0x%x>; record-size = <0x%x>;'
              ' console-size = <0x%x>; pmsg-size = <0x%x>; };\n  };\n'
              % (r['base'], r['base'], r['size'], r['record'], r['console'], r['pmsg']))
    if coreboot:
        extra += ('  firmware { ranges; coreboot { compatible = "coreboot";'
                  ' reg = <0 0x%x 0 0x1000>, <0 0x%x 0 0x1000>; }; };\n' % (CB_TABLE, CB_TABLE))
    extra += '};\n'
    return sh('dtc', '-q', '-I', 'dts', '-O', 'dtb', '-', inp=(src + extra).encode())

def coreboot_table():
    rec = struct.pack('<IIQIII', 0x12, 40, FB_ADDR, FB_W, FB_H, FB_W * 4) + bytes([32, 16, 8, 8, 8, 0, 8, 24, 8]) + b'\0' * 3
    return b'LBIO' + struct.pack('<IIIII', 24, 0, len(rec), 0, 1) + rec

# ---------------------------------------------------------------- the fake machine
class Stop(Exception):
    pass

class Machine:
    def __init__(self, args):
        self.a = args
        self.log = []                 # (kind, text)
        self.end = None               # why emulation stopped
        self.ticks = 0
        self.wdt = dict(mode=0, length=0, armed_at=None, timeout_s=None)
        self.regs = {}                # (block, offset) -> value
        self.mmio_count = 0
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(RAM_BASE, RAM_SIZE)
        self._preset_display()
        self._map('gpio', 0x10005000, 0x1000)
        self._map('wdt', 0x10007000, 0x1000)
        self._map('mmsys', 0x14000000, 0x21000)       # MMSYS config + OVL/RDMA/DSI/PWM/MUTEX blocks
        self.timer_hz = TIMER_HZ
        self.keys = KeyScript(args.keys.encode().decode('unicode_escape')) if args.keys else None
        self.ec = FakeEC(self, self.keys)
        self.spi = FakeSPI(self.ec)
        self.stop_at = None
        if args.run_seconds:
            self.stop_at = int(args.run_seconds * TIMER_HZ)
        elif self.keys:
            self.stop_at = int((self.keys.end_s + 2.0) * TIMER_HZ)
        self._map_spi()
        self.uc.hook_add(UC_HOOK_INTR, self._intr)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        try:
            self.uc.hook_add(UC_HOOK_INSN, self._mrs, None, 1, 0, UC_ARM64_INS_MRS)
            self.has_mrs_hook = True
        except UcError:
            self.has_mrs_hook = False

    # --- what the firmware leaves, as measured by wave-os v0.6.2 on the real hana
    def _preset_display(self):
        r = self.regs
        r[('mmsys', 0x100)] = 0x7f7a7ffc             # MMSYS_CG_CON0 (1 = clock gated): OVL0, RDMA0, COLOR0, OD on
        r[('mmsys', 0x110)] = 0xffffffcf             # MMSYS_CG_CON1: only DSI0 clocks on (PWM clocks gated)
        for ovl in (0xc000, 0xd000):
            for n in range(1, 4):
                r[('mmsys', ovl + 0x30 + 0x20 * n)] = 0xff
        ovl0 = 0xc000
        r[('mmsys', ovl0 + 0x0c)] = 0                # OVL_EN = 0: the firmware stopped the overlay engine
        r[('mmsys', ovl0 + 0x20)] = (FB_H << 16) | FB_W
        r[('mmsys', ovl0 + 0x2c)] = 1                # SRC_CON: layer 0 on
        r[('mmsys', ovl0 + 0x30)] = 2 << 12          # L0_CON: ARGB8888
        r[('mmsys', ovl0 + 0x38)] = (FB_H << 16) | FB_W
        r[('mmsys', ovl0 + 0x44)] = FB_W * 4
        r[('mmsys', ovl0 + 0xf40)] = FB_ADDR
        r[('mmsys', 0xe000 + 0x10)] = 0x101; r[('mmsys', 0xe000 + 0x14)] = 0xb00556; r[('mmsys', 0xe000 + 0x18)] = 0x300
        r[('mmsys', 0xf000 + 0x10)] = 0x100; r[('mmsys', 0xf000 + 0x14)] = 0xb00280; r[('mmsys', 0xf000 + 0x18)] = 0x1e0
        r[('mmsys', 0x1b000 + 0x00)] = 1; r[('mmsys', 0x1b000 + 0x14)] = 1; r[('mmsys', 0x1b000 + 0x18)] = 0x3c   # DSI0 running
        r[('gpio', 0x020)] = 0x281; r[('gpio', 0x420)] = 0x280; r[('gpio', 0x520)] = 0xfbe4   # pins 32-47: 32 low, 41 high
        r[('gpio', 0x050)] = 0xb080; r[('gpio', 0x450)] = 0x3000; r[('gpio', 0x550)] = 0x7d04 # pins 80-95: 87, 95 low
        r[('gpio', 0x710)] = 1 << 6                  # pin 87 in DISP_PWM0 mode (mode 1)

    def pin(self, n):
        port = (n >> 4) << 4
        return (self.regs.get(('gpio', 0x400 + port), 0) >> (n & 15)) & 1
    def pinmode(self, n):
        return (self.regs.get(('gpio', 0x600 + (n // 5) * 0x10), 0) >> ((n % 5) * 3)) & 7

    def panel_state(self):
        """Is the panel lit and showing OVL0? Returns (lit, reasons-it-is-dark)."""
        r, why = self.regs, []
        if self.a.display == 'off': why.append('display powered off (scenario)')
        if not self.pin(41): why.append('panel power GPIO41 low')
        if not self.pin(32): why.append('backlight power GPIO32 low')
        if not self.pin(95): why.append('backlight enable GPIO95 low')
        pwm_on = self.pinmode(87) == 1 and r.get(('mmsys', 0x1e000), 0) & 1 and not (r.get(('mmsys', 0x110), 0) & 3)
        gpio_high = self.pinmode(87) == 0 and self.pin(87)
        if not (pwm_on or gpio_high): why.append('brightness pin 87 not driven (PWM off/clock-gated, or GPIO low)')
        if not r.get(('mmsys', 0xc000 + 0x0c), 0) & 1: why.append('overlay engine OVL0_EN = 0')
        return (not why), why
    BLOCKS = [(0x0000, 'mmsys_cfg'), (0xc000, 'ovl0'), (0xd000, 'ovl1'), (0xe000, 'rdma0'), (0xf000, 'rdma1'),
              (0x10000, 'rdma2'), (0x1b000, 'dsi0'), (0x1c000, 'dsi1'), (0x1d000, 'dpi0'), (0x1e000, 'disp_pwm0'),
              (0x1f000, 'disp_pwm1'), (0x20000, 'mutex')]

    def _blockname(self, bank, off):
        if bank != 'mmsys':
            return '%s+%#x' % (bank, off)
        name, base = 'mmsys?', 0
        for b, n in self.BLOCKS:
            if off >= b:
                name, base = n, b
        return '%s+%#x' % (name, off - base)

    def _display_hangs(self, bank, off):
        # Scenario "display off": unclocked display blocks freeze the bus on access (real MediaTek behaviour)
        return self.a.display == 'off' and bank == 'mmsys' and off >= 0xc000

    def _map(self, bank, base, size):
        def rd(uc, off, sz, _):
            if self._display_hangs(bank, off):
                self._stop('FREEZE', 'read of %s while the display is powered off (bus hang)' % self._blockname(bank, off))
                return 0
            v = self.regs.get((bank, off), 0)
            self._mmio('R', bank, off, v)
            return v
        def wr(uc, off, sz, val, _):
            if self._display_hangs(bank, off):
                self._stop('FREEZE', 'write to %s while the display is powered off (bus hang)' % self._blockname(bank, off))
                return
            self._mmio('W', bank, off, val)
            if bank == 'gpio' and off < 0x600 and (off & 0xf) in (4, 8):
                reg = off & ~0xf
                cur = self.regs.get(('gpio', reg), 0)
                self.regs[('gpio', reg)] = (cur | val) if (off & 0xf) == 4 else (cur & ~val)
                if 0x400 <= reg < 0x500:
                    self.regs[('gpio', reg + 0x100)] = self.regs[('gpio', reg)]
                return
            self.regs[(bank, off)] = val
            if bank == 'wdt':
                self._wdt_write(off, val)
        self.uc.mmio_map(base, size, rd, None, wr, None)

    def _map_spi(self):
        def rd(uc, off, sz, _):
            v = self.spi.read(off)
            self._mmio('R', 'spi', off, v)
            return v
        def wr(uc, off, sz, val, _):
            self._mmio('W', 'spi', off, val)
            self.spi.write(off, val)
        self.uc.mmio_map(0x1100a000, 0x1000, rd, None, wr, None)

    def _mmio(self, rw, bank, off, v):
        self.mmio_count += 1
        if self.mmio_count <= 400:
            self.log.append(('mmio', '%s %-18s %s %#x' % (rw, self._blockname(bank, off), '->' if rw == 'R' else '<-', v)))

    def _wdt_write(self, off, val):
        w = self.wdt
        if off == 0x00 and (val >> 24) == 0x22:
            w['mode'] = val & 0xffffff
            if val & 1:
                w['armed_at'] = self.ticks
                w['timeout_s'] = ((self.regs.get(('wdt', 0x04), 0) >> 5) >> 6) or 31
                self.log.append(('event', 'watchdog armed: reboot in %d s unless kicked' % w['timeout_s']))
            elif w['armed_at'] is not None:
                w['armed_at'] = None
                self.log.append(('event', 'watchdog disarmed'))
        elif off == 0x08 and val == 0x1971 and w['armed_at'] is not None:
            w['armed_at'] = self.ticks
        elif off == 0x14 and val == 0x1209:
            self._stop('REBOOT', 'watchdog software reset (%s)' % ('RAM kept' if self.a.wdt_keeps_ram else 'RAM wiped'),
                       ram_kept=self.a.wdt_keeps_ram)

    def _check_wdt(self):
        w = self.wdt
        if w['armed_at'] is not None and (self.ticks - w['armed_at']) >= w['timeout_s'] * TIMER_HZ:
            self._stop('REBOOT', 'watchdog dead-man timeout after %d s (RAM wiped)' % w['timeout_s'], ram_kept=False)

    def _mrs(self, uc, reg, cp, _):
        # Fake generic timer: CNTVCT/CNTPCT advance 1 ms per read so delays finish quickly.
        if cp.op0 == 3 and cp.op1 == 3 and cp.crn == 14 and cp.crm == 0 and cp.op2 in (1, 2):
            self.ticks += TIMER_HZ // 1000
            uc.reg_write(reg, self.ticks)
            self._check_wdt()
            if self.stop_at is not None and self.ticks >= self.stop_at:
                self._stop('DONE', 'stopped at fake time %.1f s' % (self.ticks / TIMER_HZ))
            return True
        if cp.op0 == 3 and cp.op1 == 3 and cp.crn == 14 and cp.crm == 0 and cp.op2 == 0:
            uc.reg_write(reg, TIMER_HZ)
            return True
        return False

    def _intr(self, uc, intno, _):
        pc = uc.reg_read(UC_ARM64_REG_PC)
        insn = [struct.unpack('<I', uc.mem_read(a, 4))[0] if RAM_BASE <= a < RAM_BASE + RAM_SIZE else 0 for a in (pc - 4, pc)]
        if 0xd4000003 in insn:                       # smc #0 -> PSCI
            fn = uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff
            if fn == 0x84000009:
                self._stop('REBOOT', 'PSCI SYSTEM_RESET (firmware reboot, RAM kept)', ram_kept=True)
            elif fn == 0x84000008:
                self._stop('POWER OFF', 'PSCI SYSTEM_OFF')
            else:
                self.log.append(('event', 'PSCI call %#x -> NOT_SUPPORTED' % fn))
                uc.reg_write(UC_ARM64_REG_X0, 0xffffffffffffffff)
                if insn[1] == 0xd4000003:
                    uc.reg_write(UC_ARM64_REG_PC, pc + 4)
            return
        if intno in (1, 3, 4):                       # undefined instruction / aborts
            self._stop('CRASH', 'CPU exception %d at pc %#x (insn %#010x)' % (intno, pc, insn[1]))
            return
        self.log.append(('event', 'cpu event %d at pc %#x (ignored)' % (intno, pc)))

    def _unmapped(self, uc, access, addr, size, value, _):
        self._stop('FREEZE', 'access to unmodelled address %#x (real hardware would likely hang or abort)' % addr)
        return False

    def _stop(self, kind, why, ram_kept=False):
        if self.end is None:
            self.end = (kind, why, ram_kept)
        self.uc.emu_stop()

    # --- run it
    def boot(self, kernel, dtb):
        hdr = kernel[:64]
        assert hdr[56:60] == b'ARMd', 'kernel is not an arm64 Image (no ARM\\x64 magic)'
        text_offset, image_size, flags = struct.unpack_from('<QQQ', hdr, 8)
        load = RAM_BASE + text_offset                # depthcharge: 2 MB aligned base + text_offset
        self.uc.mem_write(load, kernel)
        self.uc.mem_write(DT_ADDR, dtb)
        if self.a.coreboot:
            self.uc.mem_write(CB_TABLE, coreboot_table())
        self.uc.reg_write(UC_ARM64_REG_X0, DT_ADDR)
        for r in (UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3):
            self.uc.reg_write(r, 0)
        self.load = load
        self.kinfo = dict(text_offset=text_offset, image_size=image_size, flags=flags, size=len(kernel))
        try:
            self.uc.emu_start(load, 0, timeout=0, count=self.a.max_insns)
        except UcError as e:
            if self.end is None:
                pc = self.uc.reg_read(UC_ARM64_REG_PC)
                self.end = ('CRASH', 'emulator error %s at pc %#x' % (e, pc), False)
        if self.end is None:
            pc = self.uc.reg_read(UC_ARM64_REG_PC)
            w = self.wdt
            if w['armed_at'] is not None:
                self.end = ('REBOOT', 'still running at pc %#x after %d instructions; the armed watchdog '
                            'would reboot it (RAM wiped)' % (pc, self.a.max_insns), False)
            else:
                self.end = ('FREEZE', 'still running at pc %#x after %d instructions (no watchdog armed: '
                            'stuck until you force it off)' % (pc, self.a.max_insns), False)

    def ramoops_console(self):
        """What Linux would show as /sys/fs/pstore/console-ramoops-0 after this reboot."""
        r = RAMOOPS
        dump = r['size'] - r['console'] - r['pmsg']
        cnt = dump // r['record']
        zone = r['base'] + cnt * (dump // cnt)
        hdr = self.uc.mem_read(zone, 12)
        sig, start, size = struct.unpack('<III', hdr)
        if sig != 0x43474244 or size > r['console'] - 12 or start > size:
            return None
        data = bytes(self.uc.mem_read(zone + 12, size))
        return (data[start:size] + data[:start]).decode(errors='replace')

    def screen_png(self, path):
        ovl = 0xc000
        addr = self.regs.get(('mmsys', ovl + 0xf40), 0)
        size = self.regs.get(('mmsys', ovl + 0x38), 0)
        pitch = self.regs.get(('mmsys', ovl + 0x44), 0) & 0xffff
        on = self.regs.get(('mmsys', ovl + 0x2c), 0) & 1
        w, h = size & 0x1fff, (size >> 16) & 0x1fff
        if not on or not (RAM_BASE <= addr < RAM_BASE + RAM_SIZE) or not w or not h:
            return False
        raw = bytes(self.uc.mem_read(addr, pitch * h))
        rows = bytearray()
        for y in range(h):
            rows.append(0)
            line = raw[y * pitch:y * pitch + w * 4]
            px = bytearray(w * 3)
            px[0::3] = line[2::4]; px[1::3] = line[1::4]; px[2::3] = line[0::4]   # BGRA in memory -> RGB
            rows += px
        def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                               + chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))
        return True

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('kpart')
    ap.add_argument('--display', choices=['on', 'off'], default='on', help='did the firmware leave the display powered on?')
    ap.add_argument('--coreboot', action='store_true', help='firmware adds a /firmware/coreboot table with the framebuffer')
    ap.add_argument('--ramoops-in-dt', action='store_true', help='firmware adds a ramoops node to the DT (real hana: it does not)')
    ap.add_argument('--wdt-keeps-ram', action='store_true', help='a watchdog reset keeps RAM (default: wipes it)')
    ap.add_argument('--keys', help='text typed on the fake keyboard, e.g. "help\\n" (starts 1 s after boot)')
    ap.add_argument('--run-seconds', type=float, help='stop at this fake time')
    ap.add_argument('--max-insns', type=int, default=600_000_000)
    ap.add_argument('--png', default='fakehana-screen.png')
    ap.add_argument('--mmio', action='store_true', help='print every register access')
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        itb, cmdline, kb, pre = unpack_kpart(a.kpart, tmp)
        kernel, dtb, info = fit_pick(itb, tmp)
        dtb2 = fixup_dt(dtb, cmdline, tmp, a.coreboot, a.ramoops_in_dt)

    print('== fake hana: firmware hand-off')
    print('  kpart: keyblock %#x, preamble %#x, cmdline "%s"' % (kb, pre, cmdline))
    print('  FIT default config %s -> kernel %s (type %s, compression %s), fdt %s (%s)'
          % (info['default'], info['kernel'], info['ktype'], info['kcomp'], info['fdt'], info['fdt_desc']))
    if info['ktype'] != 'kernel_noload':
        print('  WARNING: kernel type "%s": the real firmware may copy it to its load address (0) and hang' % info['ktype'])
    print('  scenario: display %s, coreboot table %s, watchdog reset %s'
          % (a.display, 'yes' if a.coreboot else 'no', 'keeps RAM' if a.wdt_keeps_ram else 'wipes RAM'))

    m = Machine(a)
    m.boot(kernel, dtb2)
    k = m.kinfo
    print('  kernel placed at %#x (text_offset %#x, image_size %#x, flags %#x, %d bytes), x0 = dtb at %#x (%d bytes)'
          % (m.load, k['text_offset'], k['image_size'], k['flags'], k['size'], DT_ADDR, len(dtb2)))
    if not m.has_mrs_hook:
        print('  note: this Unicorn cannot hook timer reads; delays may hit the instruction limit')

    print('\n== what happened')
    for kind, text in m.log:
        if kind == 'event' or a.mmio:
            print('  ' + text)
    if not a.mmio:
        print('  (%d register accesses; --mmio to list them)' % m.mmio_count)
    kind, why, ram_kept = m.end
    print('  END: %s -- %s   [fake time %.1f s]' % (kind, why, m.ticks / TIMER_HZ))
    if kind == 'FREEZE' and m.wdt['armed_at'] is not None:
        print('       ...then the armed watchdog reboots it after %d s (RAM wiped)' % m.wdt['timeout_s'])
        kind = 'REBOOT'

    if m.ec.commands:
        from collections import Counter
        names = {1: 'HELLO', 2: 'GET_VERSION', 0x60: 'MKBP_STATE', 0x61: 'MKBP_INFO'}
        cnt = Counter((names.get(cmd, hex(cmd)), res) for cmd, res in m.ec.commands)
        print('  EC commands: ' + ', '.join('%s x%d%s' % (n, k, '' if r == 0 else ' (result %d)' % r)
                                            for (n, r), k in cnt.items()))
        print('  SPI packets: %d' % m.spi.transfers)
    if m.keys:
        print('  keys typed: %r (done at fake %.1f s)' % (a.keys, m.keys.end_s))

    print('\n== /sys/fs/pstore/console-ramoops-0 on the next ChromeOS boot')
    text = m.ramoops_console() if (kind == 'REBOOT' and ram_kept) else None
    if kind == 'REBOOT' and not ram_kept:
        print('  (empty: the reboot wiped RAM)')
    elif kind == 'DONE':
        print('  (still running -- type reboot in the shell to keep the log)')
    elif kind != 'REBOOT':
        print('  (nothing yet: the machine did not reboot by itself)')
    elif text is None:
        print('  (no valid wave-os log found in the ramoops console zone)')
    else:
        for line in text.rstrip('\n').split('\n'):
            print('  | ' + line)

    print('\n== screen')
    lit, why = m.panel_state()
    if lit and m.screen_png(a.png):
        print('  panel is LIT; saved what it shows to %s' % a.png)
    else:
        print('  panel is DARK: ' + '; '.join(why or ['no active layer']))
        if m.screen_png(a.png):
            print('  (what is in the screen buffer anyway: %s)' % a.png)
    return 0

if __name__ == '__main__':
    sys.exit(main())
