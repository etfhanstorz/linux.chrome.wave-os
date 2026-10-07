// Tiny text console on a linear framebuffer (32bpp or 16bpp), 8x16 font drawn at 2x.
#include "font.h"
#define SCALE 2
#define CW (8 * SCALE)
#define CH (16 * SCALE)

static struct fb con_fb;
static u32 con_col, con_row, con_cols, con_rows, con_fg = 0xFFFFFF, con_bg = 0x001030;

static void px(u32 x, u32 y, u32 rgb) {
    u8 *p = (u8 *)con_fb.addr + (u64)y * con_fb.stride;
    if (con_fb.bpp == 32) ((volatile u32 *)p)[x] = rgb | 0xFF000000;   // opaque alpha
    else ((volatile unsigned short *)p)[x] = ((rgb >> 19 & 31) << 11) | ((rgb >> 10 & 63) << 5) | (rgb >> 3 & 31);
}
static void glyph(u32 cx, u32 cy, char c) {
    if (c < 32 || c > 126) c = '?';
    const u8 *g = font8x16[c - 32];
    for (u32 r = 0; r < 16; r++)
        for (u32 b = 0; b < 8; b++) {
            u32 rgb = (g[r] & (0x80 >> b)) ? con_fg : con_bg;
            for (u32 dy = 0; dy < SCALE; dy++)
                for (u32 dx = 0; dx < SCALE; dx++)
                    px(cx * CW + b * SCALE + dx, cy * CH + r * SCALE + dy, rgb);
        }
}
static void con_clear(void) {
    for (u32 y = 0; y < con_fb.h; y++) for (u32 x = 0; x < con_fb.w; x++) px(x, y, con_bg);
    con_col = con_row = 0;
}
static void con_scroll(void) {
    u32 bytes = con_fb.bpp / 8;
    for (u32 y = 0; y + CH < con_rows * CH; y++) {
        volatile u8 *d = (u8 *)con_fb.addr + (u64)y * con_fb.stride;
        volatile u8 *s = (u8 *)con_fb.addr + (u64)(y + CH) * con_fb.stride;
        for (u32 i = 0; i < con_cols * CW * bytes; i++) d[i] = s[i];
    }
    for (u32 y = (con_rows - 1) * CH; y < con_rows * CH; y++) for (u32 x = 0; x < con_cols * CW; x++) px(x, y, con_bg);
    con_row = con_rows - 1;
}
static int con_on;   // screen drawing enabled (text always goes to the log)
static void con_init(const struct fb *f) {
    con_fb = *f; con_cols = f->w / CW; con_rows = f->h / CH; con_clear(); con_on = 1;
}
static void putc(char c) {
    logc(c);
    if (!con_on) return;
    if (c == '\n') { con_col = 0; con_row++; }
    else if (c == '\b') { if (con_col) { con_col--; glyph(con_col, con_row, ' '); } }
    else { glyph(con_col, con_row, c); if (++con_col >= con_cols) { con_col = 0; con_row++; } }
    if (con_row >= con_rows) con_scroll();
}
static void puts(const char *s) { while (*s) putc(*s++); }
static void put_hex(u64 v) {
    puts("0x"); int started = 0;
    for (int i = 60; i >= 0; i -= 4) { u32 n = (v >> i) & 15; if (n || started || i == 0) { putc("0123456789abcdef"[n]); started = 1; } }
}
static void put_dec(u64 v) {
    char b[21]; int i = 20; b[i] = 0;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    puts(b + i);
}
