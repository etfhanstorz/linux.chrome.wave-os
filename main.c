// Milestone 1: find the firmware-provided framebuffer in the DTB and fill it red.
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

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

#include "console.h"
#define VERSION "wave-os v0.5"
#include "sys.h"
#include "probe.h"
#include "shell.h"
#ifdef QEMU
#include "qemu_ramfb.h"
#endif


void main(const u8 *dtb) {
    struct fb f;
    icache_on();
#ifdef QEMU
    ramfb_init(0x50000000UL, 1366, 768);
#endif
    struct fb pf;
    int have_probe = probe_fb(&pf);           // always probe so the screen can show what it saw
    const char *src = "device tree";
    if (!find_fb(dtb, &f)) {
        if (!have_probe) {
#ifndef QEMU
            reboot();                         // signal: no screen found anywhere -> immediate reboot
#endif
            for (;;) ;
        }
        f = pf; src = "display registers (OVL)";
    }
    fill(&f, 0xFFFFFF);                       // white = framebuffer found (most visible)
#ifndef QEMU
    delay_s(2);
#endif
    con_init(&f);
    puts(VERSION "\n\n");
    u64 el; __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    puts("exception level: EL"); put_dec(el >> 2); putc('\n');
    puts("dtb at: "); put_hex((u64)dtb); puts(", size "); put_dec(be32(dtb + 4)); putc('\n');
    puts("framebuffer: "); put_hex(f.addr); putc(' '); put_dec(f.w); putc('x'); put_dec(f.h);
    puts(" stride "); put_dec(f.stride); puts(" bpp "); put_dec(f.bpp); putc('\n');
    puts("console: "); put_dec(con_cols); putc('x'); put_dec(con_rows); puts(" chars\n\n");
    puts("screen found via: "); puts(src); putc('\n');
    for (int i = 0; i < nlayers; i++) {
        struct layer *l = &layers[i];
        puts("ovl"); put_dec(l->ovl); puts(" L"); put_dec(l->n); puts(l->en ? " on  " : " off ");
        puts("addr "); put_hex(l->addr); puts(" size "); put_hex(l->size); puts(" pitch "); put_dec(l->pitch);
        puts(" con "); put_hex(l->con); putc('\n');
    }
    puts("boot ok.\n");
#ifdef QEMU
    puts("type help (typing goes in the Ubuntu terminal)\n\n");
#else
    puts("keyboard: no driver for this hardware yet\n");
    puts("diagnostic build: rebooting in 20 seconds (take a photo!)\n");
    delay_s(20);
    reboot();
#endif
    shell();
}
