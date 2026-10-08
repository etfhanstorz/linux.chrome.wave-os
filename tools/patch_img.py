root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# ---- img.h: whole-number IDCT constants (x * 4096 + 0.5, truncated, as stb_image computes them) ----
s = open(root + 'img.h').read()
for f, v in [('0.5411961f', '2217'), ('-1.847759065f', '-7567'), ('0.765366865f', '3135'), ('1.175875602f', '4816'), ('0.298631336f', '1223'),
             ('2.053119869f', '8410'), ('3.072711026f', '12586'), ('1.501321110f', '6149'), ('-0.899976223f', '-3685'), ('-2.562915447f', '-10497'),
             ('-1.961570560f', '-8034'), ('-0.390180644f', '-1597')]:
    assert ('JF2F(%s)' % f) in s, f
    s = s.replace('JF2F(%s)' % f, '(%s)' % v)
s = s.replace('#define JF2F(x) ((int)((x) * 4096 + 0.5))\n', '')
s = s.replace('        static u32 tmp_row[4096 * 4];\n', '').replace('        (void)tmp_row;\n', '')
open(root + 'img.h', 'w', newline='').write(s)

# ---- link.ld: 32 MB for pictures after the BSS (not in the file, not cleared at boot) ----
edit('link.ld', [("""  __bss_end = .;
  . = ALIGN(0x10000);
  _kend = .;""", """  __bss_end = .;
  . = ALIGN(0x10000);
  __img_heap = .;            /* 32 MB of picture memory for the browser (img.h) */
  . += 0x2000000;
  _kend = .;""")])

edit('main.c', [("""#include "tls.h\"""", """#include "tls.h"
#include "img.h\"""")])

# ---- the browser: <img>, two passes, drawing ----
edit('web.h', [
("""static char web_links[WEB_MAXLINKS + 1][WEB_HREF];""",
"""static char web_links[WEB_MAXLINKS + 1][WEB_HREF];
// pictures (v1.8): the page is laid out once with "[img]" placeholders, the pictures are fetched and decoded, then it is laid out again
// with room for each picture (whole text lines), and web_draw() paints the pictures into that room.
#define WEB_MAXIMG 16
static char web_img_src[WEB_MAXIMG][WEB_HREF], web_img_alt[WEB_MAXIMG][48];
static u32 *web_img_pix[WEB_MAXIMG]; static u32 web_img_w[WEB_MAXIMG], web_img_h[WEB_MAXIMG], web_img_line[WEB_MAXIMG], web_img_rows[WEB_MAXIMG];
static u32 w_nimg, web_img_shown;"""),
("""    wn = 0; wnl = 0; wls[0] = 0; w_nlinks = 0;""", """    wn = 0; wnl = 0; wls[0] = 0; w_nlinks = 0; w_nimg = 0; for (u32 i = 0; i < WEB_MAXIMG; i++) web_img_rows[i] = 0;"""),
("""    char tn[12], href[WEB_HREF], alt[60];""", """    char tn[12], href[WEB_HREF], alt[60], isrc[WEB_HREF], dsrc[WEB_HREF];"""),
("""            href[0] = 0; alt[0] = 0;""", """            href[0] = 0; alt[0] = 0; isrc[0] = 0; dsrc[0] = 0;"""),
("""                char an[10]; u32 a = 0;
                while (j < n && h[j] != '=' && h[j] != ' ' && h[j] != '>' && h[j] != '/' && h[j] != '\\n' && h[j] != '\\t') { if (a < 9) an[a++] = (char)(h[j] | 0x20); j++; }""",
"""                char an[16]; u32 a = 0;
                while (j < n && h[j] != '=' && h[j] != ' ' && h[j] != '>' && h[j] != '/' && h[j] != '\\n' && h[j] != '\\t') { if (a < 15) an[a++] = (char)(h[j] | 0x20); j++; }"""),
("""                else if (an[0] == 'a' && an[1] == 'l' && an[2] == 't' && !an[3]) { dst = alt; dmax = sizeof alt; }""",
"""                else if (an[0] == 'a' && an[1] == 'l' && an[2] == 't' && !an[3]) { dst = alt; dmax = sizeof alt; }
                else if (tag_is(an, "src")) { dst = isrc; dmax = sizeof isrc; }
                else if (tag_is(an, "data-src") || tag_is(an, "data-lazy-src") || tag_is(an, "data-original")) { dst = dsrc; dmax = sizeof dsrc; }"""),
("""            else if (tag_is(tn, "img")) { u32 sv = wsty_cur; wsty_cur = S_DIM; web_puts("[img"); if (alt[0]) { web_puts(": "); web_puts(alt); } web_puts("]"); wsty_cur = sv; w_space = 1; }""",
"""            else if (tag_is(tn, "img")) {
                const char *use = dsrc[0] ? dsrc : (isrc[0] && !ci_prefix((const u8 *)isrc, "data:")) ? isrc : "";
                u32 id = use[0] && w_nimg < WEB_MAXIMG ? w_nimg++ : WEB_MAXIMG;
                if (id < WEB_MAXIMG) { u32 k2 = 0; while (use[k2] && k2 < WEB_HREF - 1) { web_img_src[id][k2] = use[k2]; k2++; } web_img_src[id][k2] = 0;
                                       k2 = 0; while (alt[k2] && k2 < 47) { web_img_alt[id][k2] = alt[k2]; k2++; } web_img_alt[id][k2] = 0; }
                if (id < WEB_MAXIMG && web_img_pix[id]) {                    // second pass: leave whole lines free for the picture
                    web_break();
                    web_img_line[id] = wnl;
                    u32 rows = (web_img_h[id] + CH - 1) / CH;
                    for (u32 r2 = 0; r2 < rows && wnl + 2 < WEB_MAXLINES; r2++) wls[++wnl] = wn;
                    web_img_rows[id] = rows;
                    w_space = 0;
                } else { u32 sv = wsty_cur; wsty_cur = S_DIM; web_puts("[img"); if (alt[0]) { web_puts(": "); web_puts(alt); } web_puts("]"); wsty_cur = sv; w_space = 1; }
            }"""),
# drawing
("""    // position, link count
    char pb[40];""",
"""    // pictures: paint the visible part of each one into the lines kept free for it
    for (u32 i = 0; i < WEB_MAXIMG; i++) {
        if (!web_img_pix[i] || !web_img_rows[i]) continue;
        u32 L = web_img_line[i];
        for (u32 r = 0; r < rows; r++) {
            u32 li = web_top + r;
            if (li < L || li >= L + web_img_rows[i]) continue;
            for (u32 sub = 0; sub < CH; sub++) {
                u32 iy = (li - L) * CH + sub, sy = (2 + r) * CH + sub;
                if (iy >= web_img_h[i] || sy >= (con_rows - 1) * CH) break;
                const u32 *src = web_img_pix[i] + iy * web_img_w[i];
                if (con_fb.bpp == 32) { volatile u32 *dst = (volatile u32 *)((u8 *)con_fb.addr + (u64)sy * con_fb.stride) + CW; for (u32 x = 0; x < web_img_w[i]; x++) dst[x] = src[x] | 0xFF000000; }
                else for (u32 x = 0; x < web_img_w[i]; x++) px(CW + x, sy, src[x]);
            }
        }
    }
    // position, link count
    char pb[40];"""),
])

# fetching the pictures after the page
edit('web.h', [
("""// ---- drawing ----
static void web_cell(""",
"""// ---- pictures ----
static u32 web_page_len, web_page_html;
// Download url into buf (following redirects); the length, or <0.
static int web_download(const char *url_in, u8 *buf, u32 max) {
    char url[300]; u32 k = 0; while (url_in[k] && k + 1 < sizeof url) { url[k] = url_in[k]; k++; } url[k] = 0;
    for (int hops = 0; hops < 4; hops++) {
        struct url u; if (!url_parse(url, &u)) return -1;
        u32 ip; if (dns_lookup(u.host, &ip)) return -1;
        http_any = 1;
        int n = u.https ? https_get(ip, u.port, u.host, u.path, buf, max, 15000) : http_get(ip, u.port, u.host, u.path, buf, max, 15000);
        http_any = 0;
        if (n < 0) return n;
        if ((http_status == 301 || http_status == 302 || http_status == 303 || http_status == 307 || http_status == 308) && http_location[0]) {
            char next[300]; if (!url_resolve(url, http_location, next, sizeof next)) return -1;
            k = 0; while (next[k] && k + 1 < sizeof url) { url[k] = next[k]; k++; } url[k] = 0; continue;
        }
        return http_status == 200 ? n : -6;
    }
    return -1;
}
static void web_draw(void);
static void web_load_images(const char *page_url) {
    if (!w_nimg) return;
    u32 found = w_nimg; web_img_shown = 0;
    u8 *buf = img_alloc(3u << 20);                                            // where each picture file is downloaded
    if (!buf) return;
    u32 maxw = (con_cols - 2) * CW, maxh = (con_rows * CH) * 3 / 5;
    for (u32 i = 0; i < found && i < WEB_MAXIMG; i++) {
        char m[40] = "loading picture "; u32 q = 16; u32 v = i + 1; char d2[4]; u32 dc = 0; do { d2[dc++] = (char)('0' + v % 10); v /= 10; } while (v); while (dc) m[q++] = d2[--dc];
        const char *of = " of "; for (u32 z = 0; of[z]; z++) m[q++] = of[z]; v = found; dc = 0; do { d2[dc++] = (char)('0' + v % 10); v /= 10; } while (v); while (dc) m[q++] = d2[--dc]; m[q] = 0;
        web_status(m);
        int key = kb_getc(); if (key == 'q' || key == 27) break;              // q / Esc: skip the rest of the pictures
        char url[300]; if (!url_resolve(page_url, web_img_src[i], url, sizeof url)) continue;
        int n = web_download(url, buf, 3u << 20);
        if (n <= 0) { u32 kk = con_on; con_on = 0; puts("picture failed to load: "); puts(url); putc('\\n'); con_on = kk; continue; }
        u32 w, h; u32 *pix = img_load(buf, (u32)n, maxw, maxh, &w, &h);
        if (!pix) { u32 kk = con_on; con_on = 0; puts("picture not shown ("); puts(img_why); puts("): "); puts(url); putc('\\n'); con_on = kk; continue; }
        web_img_pix[i] = pix; web_img_w[i] = w; web_img_h[i] = h; web_img_shown++;
    }
    if (web_img_shown) {                                                      // lay the page out again, now with room for the pictures
        web_begin();
        web_html(web_raw, web_page_len);
    }
}

// ---- drawing ----
static void web_cell("""),
("""        } else if (is_html) web_html(web_raw, (u32)n);""",
"""        } else if (is_html) { web_page_len = (u32)n; web_html(web_raw, (u32)n); }"""),
("""    web_top = 0; web_sel = 0;
    for (int hops = 0; hops < 6; hops++) {""",
"""    web_top = 0; web_sel = 0;
    img_bump = 0; web_img_shown = 0; for (u32 i = 0; i < WEB_MAXIMG; i++) web_img_pix[i] = 0;   // a new page: forget the old pictures
    for (int hops = 0; hops < 6; hops++) {"""),
("""static void web_goto(const char *url, int remember) {
    if (remember) web_push();
    int ok = web_fetch(url); (void)ok;
    web_msg[0] = 0; web_draw();""",
"""static void web_goto(const char *url, int remember) {
    if (remember) web_push();
    int ok = web_fetch(url);
    web_msg[0] = 0; web_draw();
    if (ok && w_nimg) { web_load_images(web_url); web_msg[0] = 0; web_draw(); }"""),
])
s = open(root + 'version.h').read().replace('#define WAVE_VERSION "1.7"', '#define WAVE_VERSION "1.8"')
open(root + 'version.h', 'w', newline='').write(s)
print('ok')
