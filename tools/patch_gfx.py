root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('gfx.h', [
('''#include "font_aa.h"''', '''#include "font_aa.h"
static void web_status(const char *msg);
static u32 g_slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }'''),
('''    e->htag = g_hash(tn, k); e->hid = idv[0] ? g_hash(idv, (u32)(sizeof(char) * 0 + ({ u32 q = 0; while (idv[q]) q++; q; }))) : 0;''',
 '''    e->htag = g_hash(tn, k); e->hid = idv[0] ? g_hash(idv, g_slen(idv)) : 0;'''),
('''            g_rect(s->left - 14, g_y + f->ascent - f->ascent / 2 - d / 2, d, d, s->color);
            g_rect(s->left - 14, 0, 0, 0, 0);
            gi[gin - 2].flags = 2;                                            // a bullet: its y is fixed now (already includes the line top)''',
 '''            g_rect(s->left - 14, g_y + f->ascent - f->ascent / 2 - d / 2, d, d, s->color);   // the bullet'''),
('''static u32 *g_counter_stack[1];
''', ''),
('''            if (!k || (h[i + 1] == '!' && !tag_is(tn, "!doctype"))) { if (!k) { if (tl < sizeof tbuf - 1) tbuf[tl++] = '<'; i++; continue; } }''',
 '''            if (!k) { if (tl < sizeof tbuf - 1) tbuf[tl++] = '<'; i++; continue; }
            if (h[i + 1] == '!' || h[i + 1] == '?') { FLUSH_TEXT(); while (i < n && h[i] != '>') i++; i++; continue; }   // <!DOCTYPE>, <![CDATA[ ...'''),
('''                    else if (alt[0]) { u32 sv = gstk[gsp].s.color; gstk[gsp].s.color = 0x888888; g_word("[", 1); g_text(alt, ({ u32 q = 0; while (alt[q]) q++; q; })); g_word("]", 1); gstk[gsp].s.color = sv; }''',
 '''                    else if (alt[0]) { u32 sv = gstk[gsp].s.color; gstk[gsp].s.color = 0x888888; g_word("[", 1); g_text(alt, g_slen(alt)); g_word("]", 1); gstk[gsp].s.color = sv; }'''),
])

edit('web.h', [
# include the renderer once url_resolve exists
('''// ---- loading ----''', '''#include "gfx.h"
static u32 web_gfx = 1;                                      // 1 = graphical view (fonts, colours, pictures), 0 = text view (key V switches)
static u32 web_page_gfx;                                     // the page on screen was laid out graphically

// ---- loading ----'''),
('''        } else if (is_html) { web_page_len = (u32)n; web_html(web_raw, (u32)n); }''',
 '''        } else if (is_html) {
            web_page_len = (u32)n;
            if (web_gfx) { g_collect_css(web_raw, (u32)n, url); web_status("laying out the page..."); g_layout(web_raw, (u32)n, (int)con_fb.w); web_page_gfx = 1; }
            else web_html(web_raw, (u32)n);
        }'''),
('''    img_bump = 0; web_img_shown = 0; for (u32 i = 0; i < WEB_MAXIMG; i++) web_img_pix[i] = 0;   // a new page: forget the old pictures''',
 '''    img_bump = 0; web_img_shown = 0; for (u32 i = 0; i < WEB_MAXIMG; i++) web_img_pix[i] = 0;   // a new page: forget the old pictures
    web_page_gfx = 0;'''),
('''    if (web_img_shown) {                                                      // lay the page out again, now with room for the pictures
        web_begin();
        web_html(web_raw, web_page_len);
    }''',
 '''    if (web_img_shown) {                                                      // lay the page out again, now with room for the pictures
        if (web_page_gfx) { web_status("laying out the page..."); g_layout(web_raw, web_page_len, (int)con_fb.w); }
        else { web_begin(); web_html(web_raw, web_page_len); }
    }'''),
# drawing: graphical pages
('''    web_status(web_msg);
    u32 rows = con_rows - 3;                                   // rows 2 .. con_rows-2''',
 '''    web_status(web_msg);
    u32 rows = con_rows - 3;                                   // rows 2 .. con_rows-2
    if (web_page_gfx) {
        int vy0 = 2 * CH, vy1 = (con_rows - 1) * CH, vh = vy1 - vy0;
        g_draw((int)web_top, vy0, vy1, web_sel);
        char pb[40]; u32 pn = 0; u32 pct = gpage_h > vh ? (web_top * 100) / (u32)(gpage_h - vh) : 100; if (pct > 100) pct = 100;
        { u32 v = pct, d[3], dc = 0; do { d[dc++] = v % 10; v /= 10; } while (v); while (dc) pb[pn++] = (char)('0' + d[--dc]); pb[pn++] = '%'; pb[pn++] = ' '; pb[pn++] = ' ';
          v = w_nlinks; dc = 0; do { d[dc++] = v % 10; v /= 10; } while (v && dc < 3); while (dc) pb[pn++] = (char)('0' + d[--dc]); const char *l = " links"; for (u32 q = 0; l[q]; q++) pb[pn++] = l[q]; }
        pb[pn] = 0;
        web_hints(); bar_draw(1);
        for (u32 i = 0; pb[i] && i < 30; i++) web_cell(con_cols - 1 - pn - 1 + i, con_rows - 1, pb[i], 0xE0E6F0, 0x1A2433);
        return;
    }'''),
# keys: pixel scrolling for graphical pages, V to switch views
('''        u32 rows = con_rows - 3, maxtop = wnl > rows ? wnl - rows : 0;''',
 '''        u32 rows = con_rows - 3, maxtop = wnl > rows ? wnl - rows : 0, step = 1, pagestep = rows - 1;
        if (web_page_gfx) { u32 vh = (con_rows - 3) * CH; maxtop = (u32)gpage_h > vh ? (u32)gpage_h - vh : 0; step = 3 * CH / 2; pagestep = vh - 2 * CH; }'''),
('''        else if (c == K_DOWN) { if (web_top < maxtop) web_top++; }
        else if (c == K_UP) { if (web_top) web_top--; }
        else if (c == ' ') { web_top = web_top + rows - 1 > maxtop ? maxtop : web_top + rows - 1; }
        else if (c == 'b') { web_top = web_top > rows - 1 ? web_top - (rows - 1) : 0; }''',
 '''        else if (c == K_DOWN) { web_top = web_top + step > maxtop ? maxtop : web_top + step; }
        else if (c == K_UP) { web_top = web_top > step ? web_top - step : 0; }
        else if (c == ' ') { web_top = web_top + pagestep > maxtop ? maxtop : web_top + pagestep; }
        else if (c == 'b') { web_top = web_top > pagestep ? web_top - pagestep : 0; }
        else if (c == 'v') {                                                  // switch between the graphical and the text view (same page, no download)
            web_gfx = !web_gfx; web_top = 0; web_sel = 0;
            if (web_page_len && (web_page_gfx || wnl)) {
                if (web_gfx) { web_status("laying out the page..."); g_layout(web_raw, web_page_len, (int)con_fb.w); web_page_gfx = 1; }
                else { web_page_gfx = 0; web_begin(); web_html(web_raw, web_page_len); }
            }
        }'''),
('''static void web_scroll_to_link(void) {''',
 '''static void web_scroll_to_link(void) {
    if (web_page_gfx) {
        int y = g_link_y(web_sel); u32 vh = (con_rows - 3) * CH;
        if (y >= 0) { if ((u32)y < web_top + CH) web_top = (u32)(y > (int)CH ? y - (int)CH : 0); else if ((u32)y > web_top + vh - 2 * CH) web_top = (u32)y - vh / 2; }
        return;
    }'''),
('''static int web_run(const char *start) {
    web_raw = update_buf; wt = (char *)(update_buf + WEB_RAW_MAX); wsty = update_buf + WEB_RAW_MAX + WEB_TEXT_MAX; wlk = wsty + WEB_TEXT_MAX;''',
 '''static int web_run(const char *start) {
    web_raw = update_buf; wt = (char *)(update_buf + WEB_RAW_MAX); wsty = update_buf + WEB_RAW_MAX + WEB_TEXT_MAX; wlk = wsty + WEB_TEXT_MAX;
    gtx = wt; gtmax = WEB_TEXT_MAX;                                           // the graphical view shares the text view's memory (only one is in use)
    gi = (struct gitem *)(void *)wsty; gimax = (2 * WEB_TEXT_MAX) / sizeof(struct gitem);'''),
(''' Up/Down/Space scroll  Left/Right link  Enter open  G address  F1 back  R reload  Q quit''',
 ''' Up/Down/Space scroll  Left/Right link  Enter open  G address  F1 back  R reload  V view  Q quit'''),
])
s = open(root + 'version.h').read().replace('#define WAVE_VERSION "1.8"', '#define WAVE_VERSION "1.9"')
open(root + 'version.h', 'w', newline='').write(s)
print('ok')
