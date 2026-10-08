// Tiny text console on a linear framebuffer (32bpp or 16bpp), 8x16 font drawn at 2x.
#include "font.h"
// Cells are 12x24: the 8x16 font drawn at 1.5x and smoothed (each source pixel is 3x3 sub-pixels, each screen pixel averages 2x2 of
// them), so letter edges are soft instead of blocky. Text colour is a soft off-white on a dark blue-grey background.
#define CW 12
#define CH 24
#define C_TEXT 0xD8DCE4

static struct fb con_fb;
static u32 con_col, con_row, con_cols, con_rows, con_fg = C_TEXT, con_bg = 0x0E1621;

static void px(u32 x, u32 y, u32 rgb) {
    u8 *p = (u8 *)con_fb.addr + (u64)y * con_fb.stride;
    if (con_fb.bpp == 32) ((volatile u32 *)p)[x] = rgb | 0xFF000000;   // opaque alpha
    else ((volatile unsigned short *)p)[x] = ((rgb >> 19 & 31) << 11) | ((rgb >> 10 & 63) << 5) | (rgb >> 3 & 31);
}
static u32 mix4(u32 bg, u32 fg, u32 cov) {                              // cov 0..4 quarters of foreground
    u32 out = 0;
    for (u32 s = 0; s < 24; s += 8) { u32 b = bg >> s & 255, f = fg >> s & 255; out |= ((b * (4 - cov) + f * cov) / 4) << s; }
    return out;
}
static void glyph(u32 cx, u32 cy, char c) {
    if (c < 32 || c > 126) c = '?';
    const u8 *g = font8x16[c - 32];
    for (u32 Y = 0; Y < CH; Y++)
        for (u32 X = 0; X < CW; X++) {
            u32 cov = 0;
            for (u32 sy = 0; sy < 2; sy++) {
                u32 r = (2 * Y + sy) / 3;                                // source row of this sub-pixel
                for (u32 sx = 0; sx < 2; sx++) { u32 b = (2 * X + sx) / 3; cov += (g[r] >> (7 - b)) & 1; }
            }
            px(cx * CW + X, cy * CH + Y, cov == 0 ? con_bg : cov == 4 ? con_fg : mix4(con_bg, con_fg, cov));
        }
}static u32 con_top;                                             // first row text may use (1 = row 0 is the status bar)
static int con_cleared;                                         // set by con_clear(): the status bar must be drawn again
static void con_clear(void) {
    for (u32 y = 0; y < con_fb.h; y++) for (u32 x = 0; x < con_fb.w; x++) px(x, y, con_bg);
    con_col = 0; con_row = con_top; con_cleared = 1;
}
// Scrolls SCROLL_LINES text lines at once (so the screen redraws 8x less often) and copies 8 bytes per step (8x faster per redraw).
#define SCROLL_LINES 8
static void con_scroll(void) {
    u32 n = con_rows > SCROLL_LINES * 2 ? SCROLL_LINES : 1;
    u32 words = con_cols * CW * (con_fb.bpp / 8) / 8;                      // 8-byte words per pixel row (a whole number: cols*12 pixels of 4 bytes)
    for (u32 y = con_top * CH; y + n * CH < con_rows * CH; y++) {
        volatile u64 *d = (volatile u64 *)((u8 *)con_fb.addr + (u64)y * con_fb.stride);
        volatile u64 *s = (volatile u64 *)((u8 *)con_fb.addr + (u64)(y + n * CH) * con_fb.stride);
        for (u32 i = 0; i < words; i++) d[i] = s[i];
    }
    u64 fill = con_fb.bpp == 32 ? ((u64)(con_bg | 0xFF000000) << 32 | (con_bg | 0xFF000000)) : 0;
    if (con_fb.bpp != 32) { u64 c16 = ((con_bg >> 19 & 31) << 11) | ((con_bg >> 10 & 63) << 5) | (con_bg >> 3 & 31); fill = c16 * 0x0001000100010001ULL; }
    for (u32 y = (con_rows - n) * CH; y < con_rows * CH; y++) {
        volatile u64 *d = (volatile u64 *)((u8 *)con_fb.addr + (u64)y * con_fb.stride);
        for (u32 i = 0; i < words; i++) d[i] = fill;
    }
    con_row = con_rows - n;
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
