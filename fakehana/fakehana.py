#!/usr/bin/env python3
"""fakehana: a fake Lenovo MT8173 "hana" Chromebook for testing wave-os without flashing.

What it imitates (best guesses where marked GUESS -- the real hardware is still the final test):
  * depthcharge's hand-off: unpack the signed kpart, read the FIT, pick the default config,
    add the DT nodes the firmware adds (bootargs, ramoops, optional coreboot table),
    place the arm64 Image at RAM + text_offset and jump to it with x0 = device tree.
  * 2 GB RAM at 0x40000000 (from the hana device tree).
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

RAM_BASE, RAM_SIZE = 0x40000000, 0x80000000          # 2 GB, as in the hana device tree
TIMER_HZ = 13_000_000                                # MT8173 system counter
DT_ADDR = 0x4a000000                                 # where our fake firmware puts the device tree
FB_ADDR = 0x7d000000                                 # GUESS: framebuffer the firmware drew on
FB_W, FB_H = 1366, 768                               # hana panel
CB_TABLE = 0x7cff0000                                # GUESS: coreboot table location
RAMOOPS = dict(base=0xb1f00000, size=0x100000, record=0x20000, console=0x20000, pmsg=0x20000)  # GUESS

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

def fixup_dt(dtb, cmdline, tmp, coreboot):
    """Add what depthcharge adds. Done by decompiling and re-compiling with extra node blocks."""
    src = sh('dtc', '-q', '-I', 'dtb', '-O', 'dts', '-', inp=dtb).decode()
    r = RAMOOPS
    extra = '\n/ {\n  chosen { bootargs = "%s"; };\n' % cmdline.replace('"', '\\"')
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
        self.uc.hook_add(UC_HOOK_INTR, self._intr)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        try:
            self.uc.hook_add(UC_HOOK_INSN, self._mrs, None, 1, 0, UC_ARM64_INS_MRS)
            self.has_mrs_hook = True
        except UcError:
            self.has_mrs_hook = False

    # --- what the firmware left the display looking like (GUESS: one ARGB8888 layer on OVL0)
    def _preset_display(self):
        r = self.regs
        r[('mmsys', 0x100)] = 0                      # MMSYS_CG_CON0: all display clocks on
        r[('mmsys', 0x110)] = 0
        ovl0 = 0xc000
        r[('mmsys', ovl0 + 0x0c)] = 1                # OVL_EN
        r[('mmsys', ovl0 + 0x20)] = (FB_H << 16) | FB_W   # ROI size
        r[('mmsys', ovl0 + 0x2c)] = 1                # SRC_CON: layer 0 on
        r[('mmsys', ovl0 + 0x30)] = 2 << 12          # L0_CON: ARGB8888
        r[('mmsys', ovl0 + 0x38)] = (FB_H << 16) | FB_W
        r[('mmsys', ovl0 + 0x44)] = FB_W * 4          # pitch
        r[('mmsys', ovl0 + 0xf40)] = FB_ADDR         # L0 address
        rdma0 = 0xe000
        r[('mmsys', rdma0 + 0x10)] = 1               # RDMA engine on (direct link from OVL)
        r[('mmsys', 0x1e000)] = 1                    # DISP_PWM0 enabled (backlight)
        r[('gpio', 0x450)] = 1 << 15                 # GPIO95 (backlight enable) high

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
            self.regs[(bank, off)] = val
            if bank == 'wdt':
                self._wdt_write(off, val)
        self.uc.mmio_map(base, size, rd, None, wr, None)

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
        if self.a.display == 'off' or not on or not (RAM_BASE <= addr < RAM_BASE + RAM_SIZE) or not w or not h:
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
    ap.add_argument('--wdt-keeps-ram', action='store_true', help='a watchdog reset keeps RAM (default: wipes it)')
    ap.add_argument('--max-insns', type=int, default=300_000_000)
    ap.add_argument('--png', default='fakehana-screen.png')
    ap.add_argument('--mmio', action='store_true', help='print every register access')
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        itb, cmdline, kb, pre = unpack_kpart(a.kpart, tmp)
        kernel, dtb, info = fit_pick(itb, tmp)
        dtb2 = fixup_dt(dtb, cmdline, tmp, a.coreboot)

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

    print('\n== /sys/fs/pstore/console-ramoops-0 on the next ChromeOS boot')
    text = m.ramoops_console() if (kind == 'REBOOT' and ram_kept) else None
    if kind == 'REBOOT' and not ram_kept:
        print('  (empty: the reboot wiped RAM)')
    elif kind != 'REBOOT':
        print('  (nothing yet: the machine did not reboot by itself)')
    elif text is None:
        print('  (no valid wave-os log found in the ramoops console zone)')
    else:
        for line in text.rstrip('\n').split('\n'):
            print('  | ' + line)

    print('\n== screen')
    if m.screen_png(a.png):
        print('  saved what the panel shows to %s' % a.png)
    else:
        print('  panel is dark (display off, or no active layer)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
