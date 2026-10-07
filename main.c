// Milestone 1: find the firmware-provided framebuffer in the DTB and fill it red.
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned short u16;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static int has_str(const u8 *d, u32 len, const char *s) {  // d is a list of NUL-terminated strings
    u32 i = 0;
    while (i < len) { if (streq((const char *)d + i, s)) return 1; while (i < len && d[i]) i++; i++; }
    return 0;
}

struct fb { u64 addr; u32 w, h, stride, bpp; };

static int find_fb(const u8 *dt, struct fb *f) {
    if (be32(dt) != 0xd00dfeed) return 0;
    const u8 *p = dt + be32(dt + 8);
    const char *strs = (const char *)dt + be32(dt + 12);
    int match = 0; u64 addr = 0; u32 w = 0, h = 0, stride = 0, bpp = 32;
    for (;;) {
        u32 tok = be32(p); p += 4;
        if (tok == 1) {                       // BEGIN_NODE
            while (*p) p++;
            p = (const u8 *)(((u64)p + 4) & ~3UL);
            match = 0; addr = 0; w = h = stride = 0; bpp = 32;
        } else if (tok == 2) {                // END_NODE
            if (match && addr && w && h) {
                if (!stride) stride = w * bpp / 8;
                f->addr = addr; f->w = w; f->h = h; f->stride = stride; f->bpp = bpp;
                return 1;
            }
            match = 0; addr = 0;
        } else if (tok == 3) {                // PROP
            u32 len = be32(p), off = be32(p + 4); p += 8;
            const char *name = strs + off;
            if (streq(name, "compatible") && has_str(p, len, "simple-framebuffer")) match = 1;
            else if (streq(name, "reg")) addr = len >= 16 ? be64(p) : be32(p);
            else if (streq(name, "width")) w = be32(p);
            else if (streq(name, "height")) h = be32(p);
            else if (streq(name, "stride")) stride = be32(p);
            else if (streq(name, "format")) bpp = ((const char *)p)[0] == 'r' && ((const char *)p)[1] == '5' ? 16 : 32;
            p = (const u8 *)(((u64)p + len + 3) & ~3UL);
        } else if (tok == 4) {                // NOP
        } else return 0;                      // END or garbage
    }
}

static void fill(const struct fb *f, u32 rgb) {
    for (u32 y = 0; y < f->h; y++) {
        u8 *row = (u8 *)f->addr + (u64)y * f->stride;
        if (f->bpp == 32) for (u32 x = 0; x < f->w; x++) ((volatile u32 *)row)[x] = rgb | 0xFF000000;  // opaque alpha
        else {
            u32 c = ((rgb >> 19 & 31) << 11) | ((rgb >> 10 & 63) << 5) | (rgb >> 3 & 31);
            for (u32 x = 0; x < f->w; x++) ((volatile unsigned short *)row)[x] = c;
        }
    }
}

#include "rlog.h"
#include "console.h"
#define VERSION "wave-os v1.20"
#include "err.h"
#include "sys.h"
#include "probe.h"
#include "display.h"
#include "wifi.h"
#include "ec.h"
#include "shell.h"
#ifdef QEMU
#include "qemu_ramfb.h"
#endif

// Print registers base+off for each offset, 3 per line. Reads are fault-safe (rd32).
static void dump(const char *name, u64 base, const u32 *offs, int n) {
    puts(name); puts(" @"); put_hex(base); putc('\n');
    for (int i = 0; i < n; i++) {
        puts("  +"); put_hex(offs[i]); puts("="); put_hex(rd32(base + offs[i]));
        if (i % 3 == 2 || i == n - 1) putc('\n');
    }
}

void main(const u8 *dtb) {
    icache_on();
#ifndef QEMU
    wdt_arm(30);                              // dead-man switch: any freeze -> reboot in 30 s
#else
    ramfb_init(0x50000000UL, 1366, 768);
#endif
    dt_scan(dtb);
    int have_log = rlog_init();
    status_reserve();                         // first log line: filled in as errors happen
    status_update();

    puts(VERSION " diagnostic log\n");
    puts("EL"); put_dec(current_el()); puts("  dtb "); put_hex((u64)dtb); puts(" size "); put_dec(be32(dtb + 4));
    puts(" nodes "); put_dec(dti.nodes); puts("  cntfrq "); put_dec(tick_hz()); putc('\n');
    puts("model: "); puts(dti.model ? dti.model : "(none)"); putc('\n');
    puts("bootargs: "); puts(dti.bootargs ? dti.bootargs : "(none)"); putc('\n');
    puts("ramoops ("); puts(rlog_src); puts("): "); puts(have_log ? "zone " : "not usable "); put_hex(rlog_zone); puts(" size "); put_hex(rlog_zone_size);
    puts(" rec "); put_hex(dti.rec_size); puts(" con "); put_hex(dti.con_size); puts(" ftrace "); put_hex(dti.ftrace_size);
    puts(" pmsg "); put_hex(dti.pmsg_size); puts(" ecc "); put_dec(dti.ecc_size); putc('\n');
    if (!dti.nodes) err("BOOT", 1, "device tree unreadable (nothing valid at x0)");
    if (!have_log) err("LOG", 1, "ramoops log area not usable: this boot leaves no log");

    // Firmware's coreboot table (may record the framebuffer the firmware drew on)
    struct cbfb cb; u64 cbt = 0;
    cb.addr = 0; cb.w = cb.h = cb.pitch = 0; cb.bpp = 0;
    int have_cb = cb_find_fb(&cb, &cbt);
    puts("coreboot node: "); puts(dti.cb_reg ? "yes" : "no"); puts("  table "); put_hex(cbt);
    puts("  framebuffer record: "); puts(have_cb ? "yes" : "no"); putc('\n');
    if (have_cb) {
        puts("  cb fb "); put_hex(cb.addr); putc(' '); put_dec(cb.w); putc('x'); put_dec(cb.h);
        puts(" pitch "); put_dec(cb.pitch); puts(" bpp "); put_dec(cb.bpp);
        puts(" r"); put_dec(cb.rpos); putc('/'); put_dec(cb.rsz); puts(" g"); put_dec(cb.gpos); putc('/'); put_dec(cb.gsz);
        puts(" b"); put_dec(cb.bpos); putc('/'); put_dec(cb.bsz); putc('\n');
    }

    // Display controller (these reads worked in v0.5)
    static const u32 ovl_regs[] = {0x0c, 0x20, 0x2c};
    dump("ovl0", 0x1400c000UL, ovl_regs, 3);
    dump("ovl1", 0x1400d000UL, ovl_regs, 3);
    struct fb pf;
    int have_probe = probe_fb(&pf);
    for (int i = 0; i < nlayers; i++) {
        struct layer *l = &layers[i];
        puts("  ovl"); put_dec(l->ovl); puts(" L"); put_dec(l->n); puts(l->en ? " on  " : " off ");
        puts("addr "); put_hex(l->addr); puts(" size "); put_hex(l->size); puts(" pitch "); put_dec(l->pitch);
        puts(" con "); put_hex(l->con); putc('\n');
    }

    // Pick a screen: device tree, then coreboot table, then display registers
    struct fb f; const char *src = 0;
    if (find_fb(dtb, &f)) src = "device tree";
    else if (have_cb && cb.w && cb.h && in_ram(cb.addr, (u64)cb.pitch * cb.h)) {
        f.addr = cb.addr; f.w = cb.w; f.h = cb.h; f.stride = cb.pitch; f.bpp = cb.bpp == 16 ? 16 : 32;
        src = "coreboot table";
    } else if (have_probe) { f = pf; src = "OVL registers"; }

    if (src) {
        puts("screen: "); puts(src); puts(" -> "); put_hex(f.addr); putc(' '); put_dec(f.w); putc('x'); put_dec(f.h);
        puts(" stride "); put_dec(f.stride); puts(" bpp "); put_dec(f.bpp); putc('\n');
        fill(&f, 0xFFFFFF);                   // white first: most visible
        con_init(&f);                         // from here, text also goes to the screen
        puts(VERSION "\nscreen from "); puts(src); putc('\n');
    } else {
        err("DISP", 1, "no screen found (device tree, coreboot table, display registers)");
    }

    // Register dumps go to the log only (scrolling the screen is slow with caches off)
    int screen = con_on;
    con_on = 0;
    static const u32 mmsys_regs[] = {0x100, 0x110};
    static const u32 rdma_regs[] = {0x10, 0x14, 0x18, 0x24, 0x2c, 0xf00};
    static const u32 dsi_regs[] = {0x00, 0x04, 0x14, 0x18};
    static const u32 pwm_regs[] = {0x00, 0x08, 0x10, 0x14};
    // dir/dout/din for pins 32-47 (32 backlight power, 41 panel power), 80-95 (87 brightness, 95 backlight
    // enable), 112-127 (115/127 eDP bridge reset/power-down); pinmux for pins 85-89
    static const u32 gpio_regs[] = {0x020, 0x420, 0x520, 0x050, 0x450, 0x550, 0x070, 0x470, 0x570, 0x710};
    static const u32 ovl_en[] = {0x0c};
    dump("mmsys", 0x14000000UL, mmsys_regs, 2);
    dump("rdma0", 0x1400e000UL, rdma_regs, 6);
    dump("rdma1", 0x1400f000UL, rdma_regs, 6);
    dump("gpio", 0x10005000UL, gpio_regs, 10);
    dump("disp_pwm0", 0x1401e000UL, pwm_regs, 4);
    dump("dsi0", 0x1401b000UL, dsi_regs, 4);
    wifi_probe();                             // read-only; also the 'wifi' shell command

#ifndef QEMU
    puts("turning on backlight (GPIO32, pin87, GPIO95) and overlay engine (OVL0_EN)\n");
    if (src && !display_on(&f)) err("DISP", 2, "display restarted but no frames reach the panel");
    puts("after:\n");
    dump("ovl0", 0x1400c000UL, ovl_en, 1);
    dump("gpio", 0x10005000UL, gpio_regs, 10);
#endif
    puts("log end ok\n");
    (void)have_log;
    con_on = screen;

#ifndef QEMU
    con_clear();
    con_fg = 0x40FF40;
    puts(VERSION "\n\n");
    con_fg = 0xFFE040;
    if (err_count) { con_on = 1; list_errors(); con_on = screen; }   // codes from before the screen was up
    con_fg = 0xFFFFFF;
    puts("keyboard...\n");
    wdt_kick();
    if (!kb_init()) {
        con_fg = 0xFFE040;
        puts("\nNo keyboard. Codes above are explained in ERRORS.md.\n");
        puts("Rebooting in 20 s so the log is kept; then Ctrl+D and run:\n");
        puts("  sudo head -3 /sys/fs/pstore/console-ramoops-0\n");
        wdt_kick();
        delay_s(20);
        reboot();
    }
    puts("\ntype help   (errors: list error codes; reboot: restart and keep the log)\n\n");
#else
    puts("type help (typing goes in the Ubuntu terminal)\n\n");
#endif
    shell();
}
