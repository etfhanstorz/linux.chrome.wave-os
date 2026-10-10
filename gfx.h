// Graphical page rendering (v1.9): the page is laid out in pixels with real (anti-aliased, proportional) fonts, colours and pictures,
// on a white page like Chrome's default look. Supports a useful slice of CSS: <style>, style="", <link rel=stylesheet> (up to 4 files);
// selectors made of tags, .classes and #ids, also nested ("div.nav a"); properties color, background(-color), font-size, font-weight,
// font-family (monospace), text-align, display (none / block / inline), text-decoration. @media blocks and :pseudo rules are skipped.
// Not yet: box sizes, margins/padding from CSS, borders, flexbox/grid, floats, positioning, web fonts, JavaScript.
#include "font_aa.h"
static void web_status(const char *msg);
static u32 g_slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

// ---------------- fonts ----------------
static const struct aaface *g_face(u32 px, u32 bold, u32 mono) {
    const struct aaface *best = &aa_faces[0]; u32 bd = 999;
    for (u32 i = 0; i < AA_NFACES; i++) {
        const struct aaface *f = &aa_faces[i];
        if (f->mono != mono) continue;
        if (!mono && f->bold != bold) continue;
        u32 d = f->size > px ? f->size - px : px - f->size;
        if (d < bd) { bd = d; best = f; }
    }
    return best;
}
static u32 g_text_w(const struct aaface *f, const char *s, u32 n) { u32 w = 0; for (u32 i = 0; i < n; i++) { u8 c = (u8)s[i]; if (c < 32 || c > 126) c = '?'; w += f->g[c - 32].adv; } return w; }

// ---------------- the display list ----------------
#define GI_TEXT 1
#define GI_RECT 2
#define GI_IMG 3
#define GI_BOX 4                                    // a box with rounded corners: color = background (flags 16: none), bg = border colour, off = border width, len = radius
struct gitem { int x, y; u16 w, h; u8 type, face, link, flags; u32 color, bg, off; u16 len; };   // flags: 1 underline, 4 inline background, 8 placed with an inline-block; rects: len = radius (0x8000|N = N percent)
static struct gitem *gi; static u32 gin, gimax;
static char *gtx; static u32 gtn, gtmax;
static struct gitem g_items[32768]; static char g_text_pool[512u * 1024];   // the display list of the page on screen
static int gpage_h; static u32 gpage_bg = 0xffffff;

// ---------------- styles ----------------
struct gstyle { u8 size, bold, mono, pre, align, hidden, under, block, listnone, nowrap, disp, tborder, tt, svg; u32 color, bg; int left, right; u32 link; u32 list_n; u8 list_ol; int imgw, imgh; short cellpad, tspace, lh; };
// an open element: its style, and for boxes the pieces fixed at the end tag (background / side borders get their height then)
struct gnode { char tag[12]; u32 htag, hid, hcls[4]; u32 ncls; struct gstyle s;
               u32 bgitem, blitem, britem, bbitem, bcolor; int bx, bw, by, cy, pb, bb, mb, mt, minh;     // block box
               u32 ibitem, ibline; int ibx0, ibpr, ibmr;                                     // inline box with a background / padding
               u32 ei, istart, cstart; int mx, mn, ext, wfix, ml, mr;                        // pass-1 element number, its display items, measuring
               u8 cmode, atomic, aitems, va, isbox; int rowtop, rowbot, cursor, gapx, rgap, ccount; u32 mlo;   // cmode: 1 flex row, 2 column, 3 grid, 4 table row
               int sv_ls, sv_asc, sv_desc, sv_empty, sv_y, sv_base; u32 sv_item, sv_plo; };           // inline-block: the line it interrupted
#define GSTACK 256
static struct gnode gstk[GSTACK]; static int gsp;
static u32 g_hash(const char *s, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i++) { u8 c = (u8)s[i]; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 16777619u; } return h ? h : 1; }

// ---------------- CSS ----------------
struct ccomp { u32 tag, id, cls[3]; u8 ncls; };
#define CP_COLOR 1
#define CP_BG 2
#define CP_SIZE 4
#define CP_BOLD 8
#define CP_ALIGN 16
#define CP_DISPLAY 32
#define CP_DECO 64
#define CP_MONO 128
#define CP_BOX(k) (1ull << (8 + (k)))              // box sides: 0-3 margin top/right/bottom/left, 4-7 padding, 8-11 border width
#define CP_WIDTH (1ull << 20)
#define CP_MAXW (1ull << 21)
#define CP_MINH (1ull << 22)
#define CP_LIST (1ull << 23)
#define CP_BOXS (1ull << 24)
#define CP_BCOL (1ull << 25)
#define CP_WS (1ull << 26)
#define CP_FDIR (1ull << 27)
#define CP_WRAP (1ull << 28)
#define CP_JUST (1ull << 29)
#define CP_AITEMS (1ull << 30)
#define CP_VALIGN (1ull << 31)
#define CP_GAP (1ull << 32)
#define CP_RGAP (1ull << 33)
#define CP_GROW (1ull << 34)
#define CP_SHRINK (1ull << 35)
#define CP_BASIS (1ull << 36)
#define CP_GRIDT (1ull << 37)
#define CP_GSPAN (1ull << 38)
#define CP_BCOLL (1ull << 39)
#define CP_RADIUS (1ull << 40)
#define CP_LH (1ull << 41)
#define CP_TT (1ull << 42)
#define CP_MASK (1ull << 43)
#define CP_NBITS 44
#define BG_NONE 0x1000000u                          // colour values: transparent
#define BG_CUR 0x2000000u                           //                currentColor (the text colour)
#define LEN_AUTO (-32768)                           // lengths are pixels; 20000+N means N percent; LEN_AUTO = auto
// display: 0 none 1 block 2 inline 3 inline-block 4 flex 5 inline-flex 6 grid 7 inline-grid 8 table 9 table-row 10 table-cell 11 list-item 13 row group
#define GT_MAX 12                                   // grid-template-columns: up to 12 tracks (type 0 px, 1 percent, 2 fr x100, 3 auto); gt_n 255 = repeat(auto-fill, minmax(gt_min, 1fr))
struct crule { struct ccomp c[4]; u8 nc; u64 set, imp; u16 spec; u32 order; u32 color, bg, bcolor; short box[12], width, maxw, minh;
               u8 size, sizerel, bold, align, display, deco, mono, listnone, boxs, ws;
               u8 fdir, wrap, just, aitems, valign, bcoll, gspan, gt_n; short gap, rgap, grow, shrink, basis, gt_min; u8 gt_t[GT_MAX]; short gt_v[GT_MAX];
               short radius, lh; u8 tt, mask; };                          // radius: px or 20000+percent; lh: 0 normal, >0 px, <0 -(ratio x100); tt: 1 upper 2 lower 3 capitalize
static struct crule *crules; static u32 ncrules, crules_max;
static u32 css_order;
static int g_screen_w = 1366;                       // for @media (min-width / max-width)
static u32 *crule_next; static u32 crule_head[4096];   // rules bucketed by their last compound's id / class / tag (0 = any element)
// CSS custom properties (--name: value) from :root / html / body / * rules, used by var(--name, fallback)
#define CVAR_SLOTS 8192
static u32 *cvar_hash, *cvar_off; static char *cvar_pool; static u32 cvar_pn, cvar_pmax;

static int c_ws(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }
static int c_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int c_eq(const char *a, u32 n, const char *b) { u32 i = 0; for (; i < n && b[i]; i++) if (c_lower(a[i]) != b[i]) return 0; return i == n && !b[i]; }
static int c_starts(const char *a, u32 n, const char *b) { u32 i = 0; for (; b[i]; i++) if (i >= n || c_lower(a[i]) != b[i]) return 0; return 1; }
static u32 c_hex(char c) { return c >= '0' && c <= '9' ? (u32)(c - '0') : (u32)(c_lower(c) - 'a' + 10); }
// A colour value: returns 1 and *rgb, 0 if not understood (or transparent).
static int c_color(const char *v, u32 n, u32 *rgb) {
    while (n && c_ws(*v)) { v++; n--; }
    while (n && (c_ws(v[n - 1]) || v[n - 1] == ';')) n--;
    if (n > 10 && c_starts(v, n, "!important")) return 0;
    for (u32 i = 0; i + 9 < n; i++) if (v[i] == '!' ) { n = i; while (n && c_ws(v[n - 1])) n--; break; }
    if (n >= 4 && v[0] == '#') {
        for (u32 i = 1; i < n; i++) { char c = (char)c_lower(v[i]); if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; }
        if (n == 4 || n == 5) { *rgb = c_hex(v[1]) * 17 << 16 | c_hex(v[2]) * 17 << 8 | c_hex(v[3]) * 17; return n == 4 || c_hex(v[4]) > 7; }
        if (n == 7 || n == 9) { *rgb = (c_hex(v[1]) << 4 | c_hex(v[2])) << 16 | (c_hex(v[3]) << 4 | c_hex(v[4])) << 8 | (c_hex(v[5]) << 4 | c_hex(v[6])); return n == 7 || (c_hex(v[7]) << 4 | c_hex(v[8])) > 127; }
        return 0;
    }
    if (c_starts(v, n, "rgb")) {
        u32 vals[4] = {0, 0, 0, 255}, k = 0, i = 0; while (i < n && v[i] != '(') i++;
        for (i++; i < n && k < 4;) {
            while (i < n && (c_ws(v[i]) || v[i] == ',' || v[i] == '/')) i++;
            u32 x = 0, frac = 0, any = 0; int dot = 0;
            while (i < n && ((v[i] >= '0' && v[i] <= '9') || v[i] == '.')) { if (v[i] == '.') dot = 1; else if (!dot) x = x * 10 + (u32)(v[i] - '0'); else if (!frac) frac = (u32)(v[i] - '0') + 1; any = 1; i++; }
            if (!any) break;
            if (i < n && v[i] == '%') { x = x * 255 / 100; i++; }
            else if (k == 3) x = dot && x == 0 ? (frac ? (frac - 1) * 255 / 9 : 0) : 255;
            vals[k++] = x > 255 ? 255 : x;
        }
        if (k < 3) return 0;
        *rgb = vals[0] << 16 | vals[1] << 8 | vals[2];
        return vals[3] > 127;
    }
    static const struct { const char *n; u32 c; } names[] = {
        {"black", 0}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff}, {"gray", 0x808080}, {"grey", 0x808080},
        {"silver", 0xc0c0c0}, {"navy", 0x000080}, {"maroon", 0x800000}, {"purple", 0x800080}, {"teal", 0x008080}, {"olive", 0x808000},
        {"orange", 0xffa500}, {"yellow", 0xffff00}, {"lime", 0x00ff00}, {"aqua", 0x00ffff}, {"fuchsia", 0xff00ff}, {"darkgray", 0xa9a9a9},
        {"lightgray", 0xd3d3d3}, {"whitesmoke", 0xf5f5f5}, {"darkblue", 0x00008b}, {"darkred", 0x8b0000}, {"crimson", 0xdc143c}, {"gold", 0xffd700},
        {"dimgray", 0x696969}, {"gainsboro", 0xdcdcdc}, {"beige", 0xf5f5dc}, {"ivory", 0xfffff0}, {"linen", 0xfaf0e6}, {"steelblue", 0x4682b4},
    };
    for (u32 i = 0; i < sizeof names / sizeof names[0]; i++) if (c_eq(v, n, names[i].n)) { *rgb = names[i].c; return 1; }
    return 0;
}
// font-size -> pixels (parent size given). 0 = not understood.
static u32 c_size(const char *v, u32 n, u32 parent) {
    while (n && c_ws(*v)) { v++; n--; }
    static const struct { const char *n; u32 px; } kw[] = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"smaller", 0}, {"larger", 1}};
    for (u32 i = 0; i < sizeof kw / sizeof kw[0]; i++) if (c_starts(v, n, kw[i].n)) return kw[i].px == 0 ? parent * 5 / 6 : kw[i].px == 1 ? parent * 6 / 5 : kw[i].px;
    u32 x = 0, f = 0, fd = 1, i = 0; int dot = 0;
    while (i < n && ((v[i] >= '0' && v[i] <= '9') || v[i] == '.')) { if (v[i] == '.') dot = 1; else if (!dot) x = x * 10 + (u32)(v[i] - '0'); else if (fd < 1000) { f = f * 10 + (u32)(v[i] - '0'); fd *= 10; } i++; }
    if (i == 0) return 0;
    u32 milli = x * 1000 + f * 1000 / fd;                                     // value x1000
    if (c_starts(v + i, n - i, "px")) return milli / 1000;
    if (c_starts(v + i, n - i, "rem")) return milli * 16 / 1000;
    if (c_starts(v + i, n - i, "em")) return milli * parent / 1000;
    if (c_starts(v + i, n - i, "pt")) return milli * 4 / 3000;
    if (i < n && v[i] == '%') return milli * parent / 100000;
    return 0;
}
// A length: px, em/rem (taken as 16 px), %, pt, vw/vh, a bare 0, auto. Returns 1 and *out (see LEN_AUTO).
static int c_len(const char *v, u32 n, int *out) {
    while (n && c_ws(*v)) { v++; n--; }
    while (n && c_ws(v[n - 1])) n--;
    if (c_eq(v, n, "auto") || c_eq(v, n, "none")) { *out = LEN_AUTO; return 1; }
    u32 i = 0; int neg = 0; if (i < n && (v[i] == '-' || v[i] == '+')) { neg = v[i] == '-'; i++; }
    u32 x = 0, f = 0, fd = 1, any = 0; int dot = 0;
    while (i < n && ((v[i] >= '0' && v[i] <= '9') || v[i] == '.')) { if (v[i] == '.') dot = 1; else if (!dot) { if (x < 100000) x = x * 10 + (u32)(v[i] - '0'); } else if (fd < 1000) { f = f * 10 + (u32)(v[i] - '0'); fd *= 10; } any = 1; i++; }
    if (!any) return 0;
    u32 milli = x * 1000 + f * 1000 / fd; int r;
    const char *u = v + i; u32 un = n - i;
    if (!un) { if (milli) return 0; r = 0; }
    else if (c_eq(u, un, "px")) r = (int)(milli / 1000);
    else if (c_eq(u, un, "em") || c_eq(u, un, "rem")) r = (int)(milli * 16 / 1000);
    else if (c_eq(u, un, "pt")) r = (int)(milli * 4 / 3000);
    else if (c_eq(u, un, "vw")) r = (int)(milli / 1000 * (u32)g_screen_w / 100);
    else if (c_eq(u, un, "vh")) r = (int)(milli / 1000 * 768 / 100);
    else if (c_eq(u, un, "%")) { if (neg) return 0; *out = 20000 + (int)(milli / 1000 > 1000 ? 1000 : milli / 1000); return 1; }
    else return 0;
    if (r > 19999) r = 19999;
    *out = neg ? -r : r; return 1;
}
static int c_res(int l, int base) { return l == LEN_AUTO ? 0 : l >= 20000 ? (l - 20000) * base / 100 : l; }   // a length in pixels (percent of base)
// split a value into space-separated parts (not inside parentheses)
static u32 c_tokens(const char *v, u32 n, u32 *ts, u32 *tl, u32 max) {
    u32 k = 0, i = 0;
    while (i < n && k < max) {
        while (i < n && c_ws(v[i])) i++;
        if (i >= n) break;
        u32 s = i; int pa = 0; while (i < n && (!c_ws(v[i]) || pa)) { if (v[i] == '(') pa++; else if (v[i] == ')') pa--; i++; }
        ts[k] = s; tl[k] = i - s; k++;
    }
    return k;
}
// margin / padding shorthand: 1 to 4 lengths (top right bottom left)
static void c_sides(const char *v, u32 n, struct crule *r, u32 base) {
    u32 ts[4], tl[4], k = c_tokens(v, n, ts, tl, 4); int val[4];
    if (!k) return;
    for (u32 i = 0; i < k; i++) if (!c_len(v + ts[i], tl[i], &val[i])) return;
    if (k == 1) val[1] = val[2] = val[3] = val[0]; else if (k == 2) { val[2] = val[0]; val[3] = val[1]; } else if (k == 3) val[3] = val[1];
    for (u32 i = 0; i < 4; i++) { r->box[base + i] = (short)val[i]; r->set |= CP_BOX(base + i); }
}
// border shorthand "1px solid #ccc" for one side (0-3) or all (-1). No style word = no border (like Chrome).
static void c_border(const char *v, u32 n, struct crule *r, int side) {
    u32 ts[6], tl[6], k = c_tokens(v, n, ts, tl, 6);
    int w = -1, none = 0, style = 0, x; u32 col = 0, hc = 0;
    for (u32 i = 0; i < k; i++) {
        const char *t = v + ts[i]; u32 l = tl[i];
        if (c_eq(t, l, "none") || c_eq(t, l, "hidden")) none = 1;
        else if (c_eq(t, l, "thin")) w = 1; else if (c_eq(t, l, "medium")) w = 3; else if (c_eq(t, l, "thick")) w = 5;
        else if (c_len(t, l, &x) && x != LEN_AUTO && x < 20000) w = x < 0 ? 0 : x;
        else if (c_color(t, l, &col)) hc = 1;
        else if (c_eq(t, l, "transparent")) { col = BG_NONE; hc = 1; }
        else if (c_eq(t, l, "currentcolor")) { col = BG_CUR; hc = 1; }
        else if (c_eq(t, l, "solid") || c_eq(t, l, "dashed") || c_eq(t, l, "dotted") || c_eq(t, l, "double") || c_eq(t, l, "groove") || c_eq(t, l, "ridge") || c_eq(t, l, "inset") || c_eq(t, l, "outset")) style = 1;
    }
    if (hc) { r->bcolor = col; r->set |= CP_BCOL; }
    int ww = none || !style ? 0 : w >= 0 ? w : 3;
    if (ww > 40) ww = 40;
    for (int s = 0; s < 4; s++) if (side < 0 || side == s) { r->box[8 + s] = (short)ww; r->set |= CP_BOX(8 + s); }
}
// a plain number x100 ("1" = 100, "0.5" = 50)
static u32 c_num100(const char *v, u32 n) {
    u32 i = 0, x = 0, f = 0, fd = 1; int dot = 0;
    while (i < n && c_ws(v[i])) i++;
    while (i < n && ((v[i] >= '0' && v[i] <= '9') || v[i] == '.')) { if (v[i] == '.') dot = 1; else if (!dot) { if (x < 10000) x = x * 10 + (u32)(v[i] - '0'); } else if (fd < 100) { f = f * 10 + (u32)(v[i] - '0'); fd *= 10; } i++; }
    u32 r = x * 100 + f * 100 / fd; return r > 30000 ? 30000 : r;
}
// one grid track: 200px, 25%, 1fr, auto, minmax(a, b) (b wins unless it is auto), min-content / max-content (auto)
static int c_track(const char *t, u32 l, u8 *ty, short *val) {
    int x;
    if (c_starts(t, l, "minmax(")) { u32 a = 7, c = a; int pa = 0; while (c < l && (t[c] != ',' || pa)) { if (t[c] == '(') pa++; else if (t[c] == ')') pa--; c++; }
        u32 e = l; while (e > c && t[e - 1] != ')') e--; if (e > c + 1) { if (c_track(t + c + 1, e - 1 - c - 1, ty, val) && *ty != 3) return 1; } return c_track(t + a, c - a, ty, val); }
    if (l > 2 && t[l - 2] == 'f' && t[l - 1] == 'r') { *ty = 2; *val = (short)c_num100(t, l - 2); return 1; }
    if (c_eq(t, l, "auto") || c_eq(t, l, "min-content") || c_eq(t, l, "max-content") || c_starts(t, l, "fit-content")) { *ty = 3; *val = 0; return 1; }
    if (c_len(t, l, &x) && x != LEN_AUTO) { if (x >= 20000) { *ty = 1; *val = (short)(x - 20000); } else { *ty = 0; *val = (short)x; } return 1; }
    return 0;
}
static void c_grid_tracks(const char *v, u32 vn, struct crule *r) {
    u32 ts[GT_MAX], tl[GT_MAX], k = c_tokens(v, vn, ts, tl, GT_MAX); u32 n = 0;
    if (c_eq(v, vn, "none")) { r->gt_n = 0; r->set |= CP_GRIDT; return; }
    for (u32 q = 0; q < k && n < GT_MAX; q++) {
        const char *t = v + ts[q]; u32 l = tl[q];
        if (t[0] == '[') continue;                                            // [line-names]
        if (c_starts(t, l, "repeat(")) {
            u32 a = 7; while (a < l && c_ws(t[a])) a++;
            u32 c = a; while (c < l && t[c] != ',') c++;
            u32 e = l; while (e > c && t[e - 1] != ')') e--;
            const char *in = t + c + 1; u32 il = e > c + 1 ? e - 1 - c - 1 : 0; while (il && c_ws(*in)) { in++; il--; }
            if (c_starts(t + a, c - a, "auto-fill") || c_starts(t + a, c - a, "auto-fit")) {   // as many columns of at least the minimum as fit
                u8 ty; short vv; int mn = 200;
                if (c_starts(in, il, "minmax(")) { u32 z = 7; while (z < il && in[z] != ',') z++; if (c_track(in + 7, z - 7, &ty, &vv) && ty == 0) mn = vv; }
                else if (c_track(in, il, &ty, &vv) && ty == 0) mn = vv;
                r->gt_n = 255; r->gt_min = (short)(mn > 0 ? mn : 200); r->set |= CP_GRIDT; return;
            }
            u32 cnt = 0; for (u32 z = a; z < c && t[z] >= '0' && t[z] <= '9'; z++) cnt = cnt * 10 + (u32)(t[z] - '0');
            u32 its[GT_MAX], itl[GT_MAX], ik = c_tokens(in, il, its, itl, GT_MAX);
            for (u32 z = 0; z < cnt && n < GT_MAX; z++) for (u32 y = 0; y < ik && n < GT_MAX; y++) if (c_track(in + its[y], itl[y], &r->gt_t[n], &r->gt_v[n])) n++;
            continue;
        }
        if (c_track(t, l, &r->gt_t[n], &r->gt_v[n])) n++;
    }
    r->gt_n = (u8)n; r->set |= CP_GRIDT;
}
// custom properties
static int css_collect_vars;                                                 // pass 1 over the style sheets: only collect --name: value
static void cvar_set(const char *nm, u32 nn, const char *v, u32 vn) {
    if (!cvar_hash || cvar_pn + vn + 1 > cvar_pmax) return;
    u32 h = g_hash(nm, nn), s = h & (CVAR_SLOTS - 1);
    for (u32 t = 0; t < CVAR_SLOTS; t++, s = (s + 1) & (CVAR_SLOTS - 1))
        if (cvar_hash[s] == 0 || cvar_hash[s] == h) { cvar_hash[s] = h; cvar_off[s] = cvar_pn; for (u32 i = 0; i < vn; i++) cvar_pool[cvar_pn++] = v[i]; cvar_pool[cvar_pn++] = 0; return; }
}
static const char *cvar_get(const char *nm, u32 nn) {
    if (!cvar_hash) return 0;
    u32 h = g_hash(nm, nn), s = h & (CVAR_SLOTS - 1);
    for (u32 t = 0; t < CVAR_SLOTS; t++, s = (s + 1) & (CVAR_SLOTS - 1)) { if (!cvar_hash[s]) return 0; if (cvar_hash[s] == h) return cvar_pool + cvar_off[s]; }
    return 0;
}
// copy a value, replacing var(--name, fallback) with the property's value (nested up to 6 deep)
static u32 c_expand(const char *v, u32 n, char *out, u32 max, int depth) {
    u32 o = 0;
    for (u32 i = 0; i < n && o + 1 < max;) {
        if (i + 4 < n && c_starts(v + i, n - i, "var(")) {
            u32 j = i + 4, e = j; int pa = 1;
            while (e < n) { if (v[e] == '(') pa++; else if (v[e] == ')' && !--pa) break; e++; }
            u32 ns = j; while (ns < e && c_ws(v[ns])) ns++;
            u32 ne = ns; while (ne < e && v[ne] != ',' && !c_ws(v[ne])) ne++;
            u32 k = ne; while (k < e && v[k] != ',') k++;
            const char *val = cvar_get(v + ns, ne - ns); u32 vl = 0;
            if (val) { while (val[vl]) vl++; } else if (k < e) { val = v + k + 1; vl = e - k - 1; }
            if (val && depth < 6) o += c_expand(val, vl, out + o, max - o, depth + 1);
            i = e + 1; continue;
        }
        out[o++] = v[i++];
    }
    return o;
}
// Parse "prop: value; prop: value" into a rule's properties.
static void c_decls(const char *d, u32 n, struct crule *r, u32 parent_px) {
    u32 i = 0;
    int posabs = 0, tiny = 0, clipped = 0, offscreen = 0;                      // "screen reader only" text is hidden like Chrome hides it
    int ovhidden = 0, zeroh = 0;                                               // overflow: hidden with no height: a collapsed menu
    int invisible = 0;                                                         // opacity: 0 (hidden when it is also taken out of the flow)
    static char vbuf[1024];
    while (i < n) {
        while (i < n && (c_ws(d[i]) || d[i] == ';')) i++;
        u32 ps = i; while (i < n && d[i] != ':' && d[i] != ';') i++;
        u32 pe = i; while (pe > ps && c_ws(d[pe - 1])) pe--;
        if (i >= n || d[i] != ':') { while (i < n && d[i] != ';') i++; continue; }
        i++;
        u32 vs = i; int paren = 0; while (i < n && (d[i] != ';' || paren)) { if (d[i] == '(') paren++; else if (d[i] == ')') paren--; i++; }
        const char *p = d + ps, *v = d + vs; u32 pn = pe - ps, vn = i - vs;
        while (vn && c_ws(*v)) { v++; vn--; }
        while (vn && c_ws(v[vn - 1])) vn--;
        if (pn > 2 && p[0] == '-' && p[1] == '-') { if (css_collect_vars) cvar_set(p, pn, v, vn); if (i < n) i++; continue; }
        if (css_collect_vars) { if (i < n) i++; continue; }
        u32 imp = 0;
        for (u32 k = 0; k < vn; k++) if (v[k] == '!') { u32 q = k + 1; while (q < vn && c_ws(v[q])) q++; if (c_starts(v + q, vn - q, "important")) { imp = 1; vn = k; while (vn && c_ws(v[vn - 1])) vn--; } break; }
        for (u32 k = 0; k + 4 < vn; k++) if (v[k] == 'v' && v[k + 1] == 'a' && v[k + 2] == 'r' && v[k + 3] == '(') { vn = c_expand(v, vn, vbuf, sizeof vbuf, 0); v = vbuf; break; }
        u64 before = r->set; r->set = 0;
        u32 c; int L;
        if (c_eq(p, pn, "color")) { if (c_color(v, vn, &c)) { r->color = c; r->set |= CP_COLOR; } }
        else if (c_eq(p, pn, "background-color")) { if (c_color(v, vn, &c)) r->bg = c; else r->bg = c_eq(v, vn, "currentcolor") ? BG_CUR : BG_NONE; r->set |= CP_BG; }
        else if (c_eq(p, pn, "background")) {                               // the colour part of the shorthand (no colour given = transparent)
            r->bg = BG_NONE; r->set |= CP_BG;
            u32 k = 0; while (k < vn) { u32 s = k; int pa = 0; while (k < vn && (!c_ws(v[k]) || pa)) { if (v[k] == '(') pa++; else if (v[k] == ')') pa--; k++; } if (c_color(v + s, k - s, &c)) { r->bg = c; break; } if (c_eq(v + s, k - s, "currentcolor")) { r->bg = BG_CUR; break; } while (k < vn && c_ws(v[k])) k++; }
        }
        else if (c_eq(p, pn, "mask") || c_eq(p, pn, "mask-image") || c_eq(p, pn, "-webkit-mask") || c_eq(p, pn, "-webkit-mask-image")) { r->mask = c_starts(v, vn, "none") ? 0 : 1; r->set |= CP_MASK; }
        else if (c_eq(p, pn, "opacity")) { if (c_num100(v, vn) == 0 && v[0] >= '0' && v[0] <= '9') invisible = 1; }
        else if (c_eq(p, pn, "font-size")) { u32 px = c_size(v, vn, parent_px ? parent_px : 16); if (px) { r->size = (u8)(px > 60 ? 60 : px < 8 ? 8 : px); r->set |= CP_SIZE; } }
        else if (c_eq(p, pn, "font-weight")) { r->bold = (c_starts(v, vn, "bold") || c_starts(v, vn, "bolder") || (v[0] >= '6' && v[0] <= '9' && vn >= 3)) ? 1 : 0; r->set |= CP_BOLD; }
        else if (c_eq(p, pn, "font-family")) { u32 mono = 0; for (u32 k = 0; k + 4 <= vn; k++) if (c_starts(v + k, vn - k, "mono") || c_starts(v + k, vn - k, "courier")) mono = 1; r->mono = (u8)mono; r->set |= CP_MONO; }
        else if (c_eq(p, pn, "text-align")) { r->align = c_starts(v, vn, "center") ? 1 : c_starts(v, vn, "right") || c_starts(v, vn, "end") ? 2 : 0; r->set |= CP_ALIGN; }
        else if (c_eq(p, pn, "display")) {
            static const struct { const char *n; u8 d; } dv[] = {{"none", 0}, {"inline-block", 3}, {"inline-flex", 5}, {"inline-grid", 7}, {"inline-table", 3}, {"inline", 2}, {"contents", 2},
                {"flex", 4}, {"grid", 6}, {"table-row-group", 13}, {"table-header-group", 13}, {"table-footer-group", 13}, {"table-row", 9}, {"table-cell", 10}, {"table-caption", 1},
                {"table", 8}, {"list-item", 11}, {"block", 1}, {"flow-root", 1}};
            for (u32 k = 0; k < sizeof dv / sizeof dv[0]; k++) if (c_starts(v, vn, dv[k].n)) { r->display = dv[k].d; r->set |= CP_DISPLAY; break; }
        }
        else if (c_eq(p, pn, "flex-direction")) { r->fdir = c_starts(v, vn, "column") ? 1 : 0; r->set |= CP_FDIR; }
        else if (c_eq(p, pn, "flex-wrap")) { r->wrap = c_starts(v, vn, "wrap") ? 1 : 0; r->set |= CP_WRAP; }
        else if (c_eq(p, pn, "flex-flow")) { u32 ts[2], tl[2], k = c_tokens(v, vn, ts, tl, 2);
            for (u32 q = 0; q < k; q++) { if (c_starts(v + ts[q], tl[q], "column")) { r->fdir = 1; r->set |= CP_FDIR; } else if (c_starts(v + ts[q], tl[q], "row")) { r->fdir = 0; r->set |= CP_FDIR; }
                else if (c_eq(v + ts[q], tl[q], "wrap")) { r->wrap = 1; r->set |= CP_WRAP; } else if (c_eq(v + ts[q], tl[q], "nowrap")) { r->wrap = 0; r->set |= CP_WRAP; } } }
        else if (c_eq(p, pn, "justify-content")) {
            r->just = c_starts(v, vn, "center") ? 1 : c_starts(v, vn, "flex-end") || c_starts(v, vn, "end") || c_starts(v, vn, "right") ? 2 : c_starts(v, vn, "space-between") ? 3 : c_starts(v, vn, "space-around") ? 4 : c_starts(v, vn, "space-evenly") ? 5 : 0;
            r->set |= CP_JUST; }
        else if (c_eq(p, pn, "align-items")) {
            r->aitems = c_starts(v, vn, "center") ? 2 : c_starts(v, vn, "flex-start") || c_starts(v, vn, "start") || c_starts(v, vn, "baseline") || c_starts(v, vn, "self-start") ? 1 : c_starts(v, vn, "flex-end") || c_starts(v, vn, "end") ? 3 : 0;
            r->set |= CP_AITEMS; }
        else if (c_eq(p, pn, "vertical-align")) { r->valign = c_starts(v, vn, "top") || c_starts(v, vn, "baseline") ? 1 : c_starts(v, vn, "middle") ? 2 : c_starts(v, vn, "bottom") ? 3 : 0; r->set |= CP_VALIGN; }
        else if (c_eq(p, pn, "gap") || c_eq(p, pn, "grid-gap")) { u32 ts[2], tl[2], k = c_tokens(v, vn, ts, tl, 2); int a, b;
            if (k && c_len(v + ts[0], tl[0], &a) && a != LEN_AUTO) { b = a; if (k > 1 && !c_len(v + ts[1], tl[1], &b)) b = a; r->rgap = (short)a; r->gap = (short)b; r->set |= CP_GAP | CP_RGAP; } }
        else if (c_eq(p, pn, "column-gap") || c_eq(p, pn, "grid-column-gap")) { if (c_len(v, vn, &L) && L != LEN_AUTO) { r->gap = (short)L; r->set |= CP_GAP; } }
        else if (c_eq(p, pn, "row-gap") || c_eq(p, pn, "grid-row-gap")) { if (c_len(v, vn, &L) && L != LEN_AUTO) { r->rgap = (short)L; r->set |= CP_RGAP; } }
        else if (c_eq(p, pn, "flex-grow")) { r->grow = (short)c_num100(v, vn); r->set |= CP_GROW; }
        else if (c_eq(p, pn, "flex-shrink")) { r->shrink = (short)c_num100(v, vn); r->set |= CP_SHRINK; }
        else if (c_eq(p, pn, "flex-basis")) { if (c_len(v, vn, &L)) { r->basis = (short)L; r->set |= CP_BASIS; } }
        else if (c_eq(p, pn, "flex")) {                                       // flex: none | auto | <grow> [<shrink>] [<basis>]
            r->set |= CP_GROW | CP_SHRINK | CP_BASIS;
            if (c_eq(v, vn, "none")) { r->grow = 0; r->shrink = 0; r->basis = LEN_AUTO; }
            else if (c_eq(v, vn, "auto")) { r->grow = 100; r->shrink = 100; r->basis = LEN_AUTO; }
            else { u32 ts[3], tl[3], k = c_tokens(v, vn, ts, tl, 3); r->grow = 0; r->shrink = 100; r->basis = 0; u32 nums = 0;
                for (u32 q = 0; q < k; q++) { const char *t = v + ts[q]; u32 l = tl[q]; int x;
                    int unitless = 1; for (u32 z = 0; z < l; z++) if (!((t[z] >= '0' && t[z] <= '9') || t[z] == '.')) unitless = 0;
                    if (unitless && nums == 0) { r->grow = (short)c_num100(t, l); nums++; } else if (unitless && nums == 1) { r->shrink = (short)c_num100(t, l); nums++; }
                    else if (c_len(t, l, &x)) r->basis = (short)x; } }
        }
        else if (c_eq(p, pn, "grid-template-columns")) c_grid_tracks(v, vn, r);
        else if (c_eq(p, pn, "grid-column") || c_eq(p, pn, "grid-column-end")) {
            r->gspan = 0;
            for (u32 k = 0; k + 4 < vn; k++) if (c_starts(v + k, vn - k, "span")) { u32 q = k + 4; while (q < vn && c_ws(v[q])) q++; u32 x = 0; while (q < vn && v[q] >= '0' && v[q] <= '9') x = x * 10 + (u32)(v[q++] - '0'); r->gspan = (u8)(x > 12 ? 12 : x); }
            for (u32 k = 0; k + 1 < vn; k++) if (v[k] == '-' && v[k + 1] == '1') r->gspan = 255;            // 1 / -1: the whole row
            r->set |= CP_GSPAN; }
        else if (c_eq(p, pn, "border-radius")) { u32 ts[1], tl[1]; if (c_tokens(v, vn, ts, tl, 1) && c_len(v + ts[0], tl[0], &L) && L != LEN_AUTO && L >= 0) { r->radius = (short)L; r->set |= CP_RADIUS; } }
        else if (c_eq(p, pn, "line-height")) {
            int ok = 1; short val = 0;
            if (!c_eq(v, vn, "normal")) { int unitless = vn > 0; for (u32 z = 0; z < vn; z++) if (!((v[z] >= '0' && v[z] <= '9') || v[z] == '.')) unitless = 0;
                   if (unitless) val = (short)-(int)c_num100(v, vn); else if (c_len(v, vn, &L) && L != LEN_AUTO) val = (short)(L >= 20000 ? -(L - 20000) : L); else ok = 0; }
            if (ok) { r->lh = val; r->set |= CP_LH; } }
        else if (c_eq(p, pn, "text-transform")) { r->tt = c_starts(v, vn, "uppercase") ? 1 : c_starts(v, vn, "lowercase") ? 2 : c_starts(v, vn, "capitalize") ? 3 : 0; r->set |= CP_TT; }
        else if (c_eq(p, pn, "overflow") || c_eq(p, pn, "overflow-y")) { if (c_starts(v, vn, "hidden") || c_starts(v, vn, "clip")) ovhidden = 1; }
        else if (c_eq(p, pn, "max-height")) { if (c_len(v, vn, &L) && L == 0) zeroh = 1; }
        else if (c_eq(p, pn, "border-collapse")) { r->bcoll = c_starts(v, vn, "collapse") ? 1 : 0; r->set |= CP_BCOLL; }
        else if (c_eq(p, pn, "float")) {                                      // a float becomes an inline-block (columns side by side, roughly like Chrome)
            if (c_starts(v, vn, "left") || c_starts(v, vn, "right")) { r->display = 3; r->set |= CP_DISPLAY; } }
        else if (c_eq(p, pn, "text-decoration") || c_eq(p, pn, "text-decoration-line")) { r->deco = c_starts(v, vn, "underline") ? 1 : 0; r->set |= CP_DECO; }
        else if (c_eq(p, pn, "visibility")) { if (c_starts(v, vn, "hidden")) { r->display = 0; r->set |= CP_DISPLAY; } }
        else if (c_eq(p, pn, "margin")) c_sides(v, vn, r, 0);
        else if (c_eq(p, pn, "padding")) c_sides(v, vn, r, 4);
        else if (c_starts(p, pn, "margin-") || c_starts(p, pn, "padding-")) {
            u32 base = p[0] == 'm' ? 0 : 4; const char *sd = p + (base ? 8 : 7); u32 sn = pn - (base ? 8 : 7);
            int side = c_eq(sd, sn, "top") || c_eq(sd, sn, "block-start") ? 0 : c_eq(sd, sn, "right") || c_eq(sd, sn, "inline-end") ? 1 : c_eq(sd, sn, "bottom") || c_eq(sd, sn, "block-end") ? 2 : c_eq(sd, sn, "left") || c_eq(sd, sn, "inline-start") ? 3 : -1;
            if (side >= 0 && c_len(v, vn, &L)) { r->box[base + (u32)side] = (short)L; r->set |= CP_BOX(base + (u32)side); }
            else if (c_eq(sd, sn, "inline") || c_eq(sd, sn, "block")) {     // margin-inline: a [b]
                u32 ts[2], tl[2], k = c_tokens(v, vn, ts, tl, 2); int a, b;
                if (k && c_len(v + ts[0], tl[0], &a)) { b = a; if (k > 1 && !c_len(v + ts[1], tl[1], &b)) b = a;
                    u32 s0 = sd[1] == 'n' ? 3 : 0, s1 = sd[1] == 'n' ? 1 : 2; r->box[base + s0] = (short)a; r->box[base + s1] = (short)b; r->set |= CP_BOX(base + s0) | CP_BOX(base + s1); }
            }
        }
        else if (c_eq(p, pn, "border")) c_border(v, vn, r, -1);
        else if (c_eq(p, pn, "border-top")) c_border(v, vn, r, 0);
        else if (c_eq(p, pn, "border-right")) c_border(v, vn, r, 1);
        else if (c_eq(p, pn, "border-bottom")) c_border(v, vn, r, 2);
        else if (c_eq(p, pn, "border-left")) c_border(v, vn, r, 3);
        else if (c_eq(p, pn, "border-width")) c_sides(v, vn, r, 8);
        else if (c_eq(p, pn, "border-color") || c_eq(p, pn, "border-top-color") || c_eq(p, pn, "border-bottom-color")) { u32 ts[1], tl[1];
            if (c_tokens(v, vn, ts, tl, 1)) { if (c_color(v + ts[0], tl[0], &c)) r->bcolor = c; else r->bcolor = c_eq(v + ts[0], tl[0], "currentcolor") ? BG_CUR : BG_NONE; r->set |= CP_BCOL; } }
        else if (c_eq(p, pn, "border-style")) { if (c_starts(v, vn, "none") || c_starts(v, vn, "hidden")) for (u32 s = 0; s < 4; s++) { r->box[8 + s] = 0; r->set |= CP_BOX(8 + s); } }
        else if (c_eq(p, pn, "width")) { if (c_len(v, vn, &L)) { r->width = (short)L; r->set |= CP_WIDTH; if (L >= 0 && L <= 1) tiny = 1; } }
        else if (c_eq(p, pn, "max-width")) { if (c_len(v, vn, &L)) { r->maxw = (short)L; r->set |= CP_MAXW; } }
        else if (c_eq(p, pn, "height") || c_eq(p, pn, "min-height")) { if (c_len(v, vn, &L)) { if (L >= 0 && L <= 1) tiny = 1; if (L == 0 && p[0] == 'h') zeroh = 1; if (L >= 20000) L = LEN_AUTO; r->minh = (short)L; r->set |= CP_MINH; } }
        else if (c_eq(p, pn, "box-sizing")) { r->boxs = c_starts(v, vn, "border-box") ? 1 : 0; r->set |= CP_BOXS; }
        else if (c_eq(p, pn, "list-style") || c_eq(p, pn, "list-style-type")) { r->listnone = 0; for (u32 k = 0; k + 4 <= vn; k++) if (c_starts(v + k, vn - k, "none")) r->listnone = 1; r->set |= CP_LIST; }
        else if (c_eq(p, pn, "white-space")) { r->ws = c_starts(v, vn, "nowrap") ? 1 : c_starts(v, vn, "pre-line") ? 0 : c_starts(v, vn, "pre") || c_starts(v, vn, "break-spaces") ? 2 : 0; r->set |= CP_WS; }
        else if (c_eq(p, pn, "position")) { if (c_starts(v, vn, "absolute") || c_starts(v, vn, "fixed")) posabs = 1; }
        else if (c_eq(p, pn, "clip") || c_eq(p, pn, "clip-path")) { if (!c_starts(v, vn, "auto") && !c_starts(v, vn, "none")) clipped = 1; }
        else if (c_eq(p, pn, "left") || c_eq(p, pn, "top")) { if (c_len(v, vn, &L) && L != LEN_AUTO && L <= -999) offscreen = 1; }
        else if (c_eq(p, pn, "text-indent")) { if (c_len(v, vn, &L) && L != LEN_AUTO && L <= -999) { r->display = 0; r->set |= CP_DISPLAY; } }
        u64 got = r->set; r->set = before | got;
        if (imp) r->imp |= got;
        if (i < n) i++;
    }
    if ((posabs && (tiny || clipped || offscreen || invisible)) || (ovhidden && zeroh)) { r->display = 0; r->set |= CP_DISPLAY; }
}
// one cascaded property from rule s into d
static void c_take(struct crule *d, const struct crule *s, u32 b) {
    u64 bit = 1ull << b;
    if (b >= 27) {
        if (bit == CP_FDIR) d->fdir = s->fdir; else if (bit == CP_WRAP) d->wrap = s->wrap; else if (bit == CP_JUST) d->just = s->just;
        else if (bit == CP_AITEMS) d->aitems = s->aitems; else if (bit == CP_VALIGN) d->valign = s->valign; else if (bit == CP_GAP) d->gap = s->gap;
        else if (bit == CP_RGAP) d->rgap = s->rgap; else if (bit == CP_GROW) d->grow = s->grow; else if (bit == CP_SHRINK) d->shrink = s->shrink;
        else if (bit == CP_BASIS) d->basis = s->basis; else if (bit == CP_GSPAN) d->gspan = s->gspan; else if (bit == CP_BCOLL) d->bcoll = s->bcoll;
        else if (bit == CP_MASK) d->mask = s->mask;
        else if (bit == CP_RADIUS) d->radius = s->radius; else if (bit == CP_LH) d->lh = s->lh; else if (bit == CP_TT) d->tt = s->tt;
        else if (bit == CP_GRIDT) { d->gt_n = s->gt_n; d->gt_min = s->gt_min; for (u32 q = 0; q < GT_MAX; q++) { d->gt_t[q] = s->gt_t[q]; d->gt_v[q] = s->gt_v[q]; } }
        return;
    }
    if (bit == CP_COLOR) d->color = s->color; else if (bit == CP_BG) d->bg = s->bg; else if (bit == CP_SIZE) d->size = s->size;
    else if (bit == CP_BOLD) d->bold = s->bold; else if (bit == CP_ALIGN) d->align = s->align; else if (bit == CP_DISPLAY) d->display = s->display;
    else if (bit == CP_DECO) d->deco = s->deco; else if (bit == CP_MONO) d->mono = s->mono;
    else if (b >= 8 && b < 20) d->box[b - 8] = s->box[b - 8];
    else if (bit == CP_WIDTH) d->width = s->width; else if (bit == CP_MAXW) d->maxw = s->maxw; else if (bit == CP_MINH) d->minh = s->minh;
    else if (bit == CP_LIST) d->listnone = s->listnone; else if (bit == CP_BOXS) d->boxs = s->boxs; else if (bit == CP_BCOL) d->bcolor = s->bcolor;
    else if (bit == CP_WS) d->ws = s->ws;
}
// @media conditions, checked against our screen: (min-width / max-width), print, dark mode, portrait ... Any comma part true = true.
static int c_media_ok(const char *s, u32 n) {
    for (u32 a = 0; a < n;) {
        u32 b = a; int pa = 0; while (b < n && (s[b] != ',' || pa)) { if (s[b] == '(') pa++; else if (s[b] == ')') pa--; b++; }
        int ok = 1;
        for (u32 k = a; k < b && ok; k++) {
            const char *t = s + k; u32 l = b - k;
            if (c_starts(t, l, "print") || c_starts(t, l, "speech") || c_starts(t, l, "not ") || c_starts(t, l, "dark") || c_starts(t, l, "portrait") || c_starts(t, l, "forced-colors: active") || c_starts(t, l, "reduce")) ok = 0;
            int mn = c_starts(t, l, "min-width"), mx = c_starts(t, l, "max-width");
            if (mn || mx) { u32 q = k + 9; while (q < b && (s[q] == ':' || c_ws(s[q]))) q++; u32 e = q; while (e < b && s[e] != ')') e++; int px2; if (c_len(s + q, e - q, &px2) && px2 != LEN_AUTO && px2 < 20000) { if (mn && g_screen_w < px2) ok = 0; if (mx && g_screen_w > px2) ok = 0; } }
        }
        if (ok) return 1;
        a = b + 1;
    }
    return 0;
}
// One compound selector like div.nav#top. Returns 0 if it uses something we do not support (then the rule is skipped).
static int c_compound(const char *s, u32 n, struct ccomp *c) {
    c->tag = c->id = 0; c->ncls = 0;
    u32 i = 0;
    if (c_starts(s, n, ":root")) { c->tag = g_hash("html", 4); i = 5; }
    else if (i < n && s[i] == '*') i++;
    else if (i < n && s[i] != '.' && s[i] != '#') { u32 st = i; while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[' && s[i] != ':') i++; c->tag = g_hash(s + st, i - st); }
    while (i < n) {
        if (s[i] == '.' || s[i] == '#') {
            char k = s[i++]; u32 st = i;
            while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[' && s[i] != ':') i++;
            u32 h = g_hash(s + st, i - st);
            if (k == '#') c->id = h; else if (c->ncls < 3) c->cls[c->ncls++] = h; else return 0;
        } else if (c_starts(s + i, n - i, ":link") || c_starts(s + i, n - i, ":any-link")) {   // every link counts as not visited yet (like Chrome for pages)
            i += s[i + 1] == 'l' ? 5 : 9;
        } else return 0;                                                     // [attr], :hover, ::before ... not supported
    }
    return 1;
}
// Parse a style sheet (from <style> or a .css file) into rules.
static void css_parse(const char *s, u32 n) {
    u32 i = 0;
    while (i < n) {
        while (i < n && c_ws(s[i])) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') { i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++; i += 2; continue; }
        if (i < n && s[i] == '@') {                                          // @media (when it fits our screen) / @supports: the rules inside count; @font-face, @keyframes ...: skipped
            u32 at = i;
            while (i < n && s[i] != '{' && s[i] != ';') i++;
            if (i < n && s[i] == ';') { i++; continue; }
            u32 open = i;
            int depth = 0; while (i < n) { if (s[i] == '{') depth++; else if (s[i] == '}') { depth--; if (depth == 0) { i++; break; } } i++; }
            int use = 0;
            if (c_starts(s + at, open - at, "@media")) use = c_media_ok(s + at + 6, open - at - 6);
            else if (c_starts(s + at, open - at, "@supports")) { use = 1; for (u32 k = at; k + 4 < open; k++) if (c_starts(s + k, open - k, "not ")) use = 0; }
            else if (c_starts(s + at, open - at, "@layer") || c_starts(s + at, open - at, "@container")) use = 1;
            if (use && i > open + 1) css_parse(s + open + 1, i - open - 2);
            continue;
        }
        u32 sel = i; while (i < n && s[i] != '{') i++;
        if (i >= n) break;
        u32 sele = i; i++;
        u32 ds = i; while (i < n && s[i] != '}') i++;
        u32 de = i; if (i < n) i++;
        // one rule per comma-separated selector
        for (u32 a = sel; a < sele;) {
            u32 b = a; int pa = 0; while (b < sele && (s[b] != ',' || pa)) { if (s[b] == '(') pa++; else if (s[b] == ')') pa--; b++; }
            if (css_collect_vars) {                                           // pass 1: custom properties from :root / html / body / * only
                u32 x = a, y = b; while (x < y && c_ws(s[x])) x++; while (y > x && c_ws(s[y - 1])) y--;
                if (c_eq(s + x, y - x, ":root") || c_eq(s + x, y - x, "html") || c_eq(s + x, y - x, "body") || c_eq(s + x, y - x, "*") || c_eq(s + x, y - x, ":host")) {
                    struct crule tmp; tmp.set = 0; tmp.imp = 0; c_decls(s + ds, de - ds, &tmp, 0);
                    break;                                                    // once per rule is enough
                }
            } else if (ncrules < crules_max) {
                struct crule *r = &crules[ncrules];
                r->nc = 0; r->set = 0; r->imp = 0; r->spec = 0; int ok = 1;
                u32 k = a;
                while (k < b && ok) {                                             // descendant chain: compounds separated by spaces (or > + ~, treated as "inside")
                    while (k < b && (c_ws(s[k]) || s[k] == '>' || s[k] == '+' || s[k] == '~')) k++;
                    if (k >= b) break;
                    u32 st = k; while (k < b && !c_ws(s[k]) && s[k] != '>' && s[k] != '+' && s[k] != '~') k++;
                    if (r->nc >= 4 || !c_compound(s + st, k - st, &r->c[r->nc])) { ok = 0; break; }
                    struct ccomp *c = &r->c[r->nc++];
                    r->spec += (c->id ? 100 : 0) + c->ncls * 10 + (c->tag ? 1 : 0);
                }
                if (ok && r->nc) {
                    c_decls(s + ds, de - ds, r, 0);
                    r->order = css_order++;
                    if (r->set) ncrules++;
                }
            }
            a = b + 1;
        }
    }
}
static int c_match1(const struct ccomp *c, const struct gnode *e) {
    if (c->tag && c->tag != e->htag) return 0;
    if (c->id && c->id != e->hid) return 0;
    for (u32 i = 0; i < c->ncls; i++) { u32 f = 0; for (u32 k = 0; k < e->ncls; k++) if (e->hcls[k] == c->cls[i]) f = 1; if (!f) return 0; }
    return 1;
}
static int c_match(const struct crule *r, int at) {                          // does rule r match the element at gstk[at] (with its ancestors)?
    if (!c_match1(&r->c[r->nc - 1], &gstk[at])) return 0;
    int k = (int)r->nc - 2, e = at - 1;
    while (k >= 0) { while (e >= 0 && !c_match1(&r->c[k], &gstk[e])) e--; if (e < 0) return 0; k--; e--; }
    return 1;
}
// bucket the rules so each element only checks rules that can match it (big sites have 10000+ rules)
static u32 c_key(const struct crule *r) { const struct ccomp *c = &r->c[r->nc - 1]; return c->id ? c->id : c->ncls ? c->cls[0] : c->tag; }
static void c_index(void) {
    for (u32 b = 0; b < 4096; b++) crule_head[b] = 0xffffffff;
    if (!crule_next) return;
    for (u32 r = ncrules; r-- > 0;) { u32 b = c_key(&crules[r]) & 4095; crule_next[r] = crule_head[b]; crule_head[b] = r; }
}

// ---------------- layout ----------------
// Two passes over the page. Pass 1 (g_meas = 1) lays everything out with unlimited width and measures each element: how wide its content
// wants to be (max-content) and its widest unbreakable piece (min-content); it also records the element tree. Pass 2 lays out for real:
// flex rows, grids and tables use the pass-1 sizes to give their children widths and places them side by side.
static int g_left0, g_right0, g_x, g_y, g_line_start, g_line_asc, g_line_desc, g_pend_space, g_line_empty;
static u32 g_line_item, g_line_no;
static int g_last_margin;
static int g_meas;
static int g_last_base = -1;                                                    // baseline of the last finished line (for inline-blocks)
#define G_HUGE (1 << 26)
#define GNONE 0xffffffffu
#define RES(l, b) (g_meas && (l) >= 20000 ? 0 : c_res((l), (b)))                  // percentages mean nothing while measuring
struct gmeas {                                                                  // one element, from pass 1 (plus its planned place in pass 2)
    int maxc, minc, px, pw;
    u32 parent, first, last, next;
    short grow, shrink, basis, width, ml, mr, ext;
    u8 disp, hidden, gspan, hastext, colspan, autom, boxs, planned, rowstart;
};
static struct gmeas *gm; static u32 gm_n, gm_max;
struct gmem { u32 istart, cstart, iend, bg, bl, br, bb; int top, hb, mt, mb; u8 va; };   // a finished member of a row, aligned when the row is complete
#define GMEM_MAX 4096
static struct gmem gmem[GMEM_MAX]; static u32 gmn;
struct gpiece { u32 a, b; int top, asc; };                                      // an inline-block in the current line (items a..b)
#define GPIECE_MAX 512
static struct gpiece gpc[GPIECE_MAX]; static u32 gpn, g_piece_lo;
static u32 g_colspan; static int g_cellpad = -1, g_cellspace = -1, g_tborder;   // attributes of the tag being opened
static int g_itype;                                                             // form control being opened: 1 text box, 2 button, 3 check box, 4 radio, 5 drop-down, 6 text area

static void g_line_end(void) {
    if (g_line_empty) return;
    struct gstyle *st = &gstk[gsp].s;
    int lh = g_line_asc + g_line_desc;
    // vertical: text sits on the common baseline; pictures and inline-blocks sit on it too
    int shift = 0;
    if (st->align && gin > g_line_item) {
        int used = g_x - g_line_start, avail = st->right - g_line_start;
        shift = st->align == 1 ? (avail - used) / 2 : avail - used;
        if (shift < 0) shift = 0;
    }
    for (u32 i = g_line_item; i < gin; i++) {
        struct gitem *it = &gi[i];
        it->x += shift;
        if (it->flags & 8) continue;                                            // inside an inline-block: moved with it below
        if (it->type == GI_TEXT) it->y = g_y + g_line_asc - aa_faces[it->face].ascent;
        else if (it->type == GI_IMG) it->y = g_y + g_line_asc - it->h;
        else if (it->type == GI_RECT && (it->flags & 4)) it->y = g_y + g_line_asc - (int)it->off;   // an inline background
    }
    for (u32 p = g_piece_lo; p < gpn; p++) { int dy = g_y + g_line_asc - gpc[p].asc - gpc[p].top; for (u32 i = gpc[p].a; i < gpc[p].b && i < gin; i++) gi[i].y += dy; }
    gpn = g_piece_lo;
    g_last_base = g_y + g_line_asc;
    g_y += lh; g_line_no++;
    g_line_empty = 1; g_line_asc = 0; g_line_desc = 0; g_pend_space = 0;
    g_x = st->left; g_line_start = g_x; g_line_item = gin;
    g_last_margin = 0;
}
static void g_vspace(int px) { g_line_end(); if (px > g_last_margin) { g_y += px - g_last_margin; g_last_margin = px; } }
static u32 g_face_index(const struct gstyle *s) { const struct aaface *f = g_face(s->size, s->bold, s->mono); return (u32)(f - aa_faces); }
static void g_add_line_metrics(int asc, int desc) { if (asc > g_line_asc) g_line_asc = asc; if (desc > g_line_desc) g_line_desc = desc; }
static int g_lineh(const struct gstyle *st, const struct aaface *f) {          // CSS line-height (normal = the font's own)
    if (!st->lh) return f->line;
    int L = st->lh > 0 ? st->lh : -st->lh * st->size / 100;
    return L < 4 ? 4 : L > 300 ? 300 : L;
}
static void g_word(const char *w, u32 n) {
    struct gnode *top = &gstk[gsp]; struct gstyle *st = &top->s;
    if (st->hidden || !n) return;
    static char tw[256];
    if (st->tt && n < sizeof tw) {                                            // text-transform
        for (u32 i = 0; i < n; i++) { char c = w[i]; if (st->tt == 1 || (st->tt == 3 && i == 0)) { if (c >= 'a' && c <= 'z') c -= 32; } else if (st->tt == 2 && c >= 'A' && c <= 'Z') c += 32; tw[i] = c; }
        w = tw;
    }
    u32 fi = g_face_index(st); const struct aaface *f = &aa_faces[fi];
    u32 ww = g_text_w(f, w, n), sp = g_pend_space && !g_line_empty ? f->g[0].adv : 0;
    int lineh = g_lineh(st, f), asc = f->ascent + (lineh - f->line) / 2;
    if (g_meas) {
        g_x += (int)(sp + ww); if (g_x > top->mx) top->mx = g_x; if ((int)ww > top->mn) top->mn = (int)ww;
        g_add_line_metrics(asc, lineh - asc); g_pend_space = 0; g_line_empty = 0; g_last_margin = 0;
        if (top->cmode && top->ei != GNONE) gm[top->ei].hastext = 1;
        return;
    }
    if (!g_line_empty && !st->nowrap && g_x + (int)sp + (int)ww > st->right) { g_line_end(); sp = 0; }
    if (gtn + n + 2 > gtmax || gin + 2 > gimax) return;
    struct gitem *prev = gin > g_line_item ? &gi[gin - 1] : 0;
    g_add_line_metrics(asc, lineh - asc);
    if (prev && prev->type == GI_TEXT && prev->face == fi && prev->color == st->color && prev->link == (u8)st->link && prev->flags == (st->under ? 1 : 0) &&
        prev->off + prev->len == gtn && prev->len + n + 1 < 60000 && prev->x + prev->w == g_x) {            // continue the same run
        if (sp) { gtx[gtn++] = ' '; prev->len++; }
        for (u32 i = 0; i < n; i++) gtx[gtn++] = w[i];
        prev->len = (u16)(prev->len + n); prev->w = (u16)(prev->w + sp + ww); g_x += (int)sp;
    } else {
        struct gitem *it = &gi[gin++];
        it->type = GI_TEXT; it->face = (u8)fi; it->link = (u8)st->link; it->flags = st->under ? 1 : 0; it->color = st->color; it->bg = st->bg;
        it->x = g_x + (int)sp; it->y = g_y; it->w = (u16)ww; it->h = (u16)lineh; it->off = gtn; it->len = (u16)n;
        for (u32 i = 0; i < n; i++) gtx[gtn++] = w[i];
        g_x += (int)sp;
    }
    g_x += (int)ww; g_pend_space = 0; g_line_empty = 0; g_last_margin = 0;
}
static void g_rect(int x, int y, int w, int h, u32 color) {
    if (g_meas || gin + 1 > gimax || w <= 0 || h <= 0) return;
    struct gitem *it = &gi[gin++]; it->type = GI_RECT; it->x = x; it->y = y; it->w = (u16)(w > 65535 ? 65535 : w); it->h = (u16)(h > 65535 ? 65535 : h); it->color = color; it->link = 0; it->flags = 0; it->len = 0;
}
static void g_image(u32 id) {
    struct gnode *top = &gstk[gsp]; struct gstyle *st = &top->s;
    if (st->hidden || gin + 1 > gimax) return;
    u32 w = web_img_w[id], h = web_img_h[id];
    if (st->imgw > 0 && st->imgh > 0) { w = (u32)st->imgw; h = (u32)st->imgh; }                  // width / height from CSS or the tag
    else if (st->imgw > 0) { h = h * (u32)st->imgw / (w ? w : 1); w = (u32)st->imgw; }
    else if (st->imgh > 0) { w = w * (u32)st->imgh / (h ? h : 1); h = (u32)st->imgh; }
    if (!w || !h) return;
    if (g_meas) { g_x += (int)w + 4; if (g_x > top->mx) top->mx = g_x; if ((int)w > top->mn) top->mn = (int)w; g_add_line_metrics((int)h, 0); g_line_empty = 0; g_pend_space = 0; g_last_margin = 0; return; }
    if ((int)w > st->right - st->left) { h = h * (u32)(st->right - st->left) / w; w = (u32)(st->right - st->left); }
    if (!g_line_empty && g_x + (int)w > st->right) g_line_end();
    struct gitem *it = &gi[gin++];
    it->type = GI_IMG; it->x = g_x; it->y = g_y; it->w = (u16)w; it->h = (u16)h; it->off = id; it->link = (u8)st->link; it->flags = 0;
    g_add_line_metrics((int)h, 0);
    g_x += (int)w + 4; g_line_empty = 0; g_pend_space = 0; g_last_margin = 0;
}
// text inside an element: split into words, keep spaces/newlines inside <pre>
static void g_text(const char *t, u32 n) {
    struct gstyle *st = &gstk[gsp].s;
    if (st->hidden) return;
    if (st->pre) {
        u32 a = 0;
        for (u32 i = 0; i <= n; i++) {
            if (i == n || t[i] == '\n') { if (i > a) { g_word(t + a, i - a); } if (i < n) { if (g_line_empty) { const struct aaface *f = &aa_faces[g_face_index(st)]; g_add_line_metrics(f->ascent, f->line - f->ascent); g_line_empty = 0; } g_line_end(); } a = i + 1; }
        }
        return;
    }
    u32 i = 0;
    while (i < n) {
        if (c_ws(t[i])) { g_pend_space = 1; i++; continue; }
        u32 s = i; while (i < n && !c_ws(t[i])) i++;
        g_word(t + s, i - s);
        if (i < n) g_pend_space = 1;
    }
}

static int g_is_block(const char *t) {
    static const char *b[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "table", "tr", "blockquote", "pre", "section", "article", "header", "footer", "nav", "main", "aside", "form", "dl", "dt", "dd", "figure", "figcaption", "hr", "address", "fieldset", "details", "summary", "center", "body", "html", "caption", "legend", "menu", "hgroup", "search", 0};
    for (u32 i = 0; b[i]; i++) if (tag_is(t, b[i])) return 1;
    return 0;
}
static int g_is_void(const char *t) {
    static const char *v[] = {"br", "img", "hr", "meta", "link", "input", "wbr", "source", "area", "base", "col", "embed", "param", "track", "!doctype", 0};
    for (u32 i = 0; v[i]; i++) if (tag_is(t, v[i])) return 1;
    return 0;
}
static int g_is_tabletag(const char *t) { return tag_is(t, "table") || tag_is(t, "tr") || tag_is(t, "td") || tag_is(t, "th") || tag_is(t, "thead") || tag_is(t, "tbody") || tag_is(t, "tfoot"); }

// ---- rows: flex rows, grids and table rows place their children side by side; each row is aligned once its tallest member is known ----
static void g_shift(u32 a, u32 b, int dy) { for (u32 i = a; i < b && i < gin; i++) gi[i].y += dy; }
static void g_stretch(struct gmem *m, int extra) {
    if (m->bg < gin) gi[m->bg].h = (u16)(gi[m->bg].h + extra);
    if (m->bl < gin) gi[m->bl].h = (u16)(gi[m->bl].h + extra);
    if (m->br < gin) gi[m->br].h = (u16)(gi[m->br].h + extra);
}
static void g_row_finish(struct gnode *c) {
    int H = c->rowbot - c->rowtop;
    for (u32 k = c->mlo; k < gmn; k++) {
        struct gmem *m = &gmem[k];
        int extra = H - m->mt - m->mb - m->hb;
        if (extra <= 0) continue;
        if (m->va >= 10) {                                                      // flex / grid: align-items
            u32 a = m->va - 10;
            if (a == 0) { g_stretch(m, extra); if (m->bb < gin) gi[m->bb].y += extra; }
            else if (a == 2) g_shift(m->istart, m->iend, extra / 2);
            else if (a == 3) g_shift(m->istart, m->iend, extra);
        } else {                                                                // table cell: the box fills the row, the content is aligned (middle by default)
            g_stretch(m, extra);
            int dy = m->va == 1 ? 0 : m->va == 3 ? extra : extra / 2;
            g_shift(m->cstart, m->iend, dy);
            if (m->bb < gin) gi[m->bb].y = m->top + m->hb + extra - gi[m->bb].h;
        }
    }
    gmn = c->mlo;
}
// planning (pass 2, when the container opens): widths and places of its children from their pass-1 sizes
static u32 g_kids(u32 ei, u32 *out, u32 max) { u32 n = 0; for (u32 c = gm[ei].first; c != GNONE && n < max; c = gm[c].next) if (!gm[c].hidden) out[n++] = c; return n; }
static int g_base(const struct gmeas *g, int W) {                               // a child's preferred border-box width
    if (g->basis != LEN_AUTO) { int b = c_res(g->basis, W); return g->boxs ? b : b + g->ext; }
    if (g->width != LEN_AUTO) { int w = c_res(g->width, W); return g->boxs ? w : w + g->ext; }
    return g->maxc;
}
static u32 g_kid[512]; static int g_kbase[512], g_kmin[512], g_ksize[512];
static void g_plan_flex(struct gnode *e, int W, int just, int wrap, int gap) {
    u32 n = g_kids(e->ei, g_kid, 512); if (!n) return;
    for (u32 i = 0; i < n; i++) {
        struct gmeas *g = &gm[g_kid[i]];
        g_kbase[i] = g_base(g, W); if (g_kbase[i] < 0) g_kbase[i] = 0;
        g_kmin[i] = g->width != LEN_AUTO ? g_kbase[i] : (g->minc < g_kbase[i] ? g->minc : g_kbase[i]);
    }
    int first_line = 1;
    for (u32 a = 0; a < n;) {
        u32 b = a; int used = 0;
        while (b < n) { int need = g_kbase[b] + gm[g_kid[b]].ml + gm[g_kid[b]].mr + (b > a ? gap : 0); if (wrap && b > a && used + need > W) break; used += need; b++; }
        int fr = W - used, tg = 0, nauto = 0;
        for (u32 i = a; i < b; i++) { g_ksize[i] = g_kbase[i]; tg += gm[g_kid[i]].grow; nauto += (gm[g_kid[i]].autom & 1) + ((gm[g_kid[i]].autom >> 1) & 1); }
        if (fr > 0 && tg > 0) {
            int left = fr; u32 last = a;
            for (u32 i = a; i < b; i++) if (gm[g_kid[i]].grow) { int add = (int)((long)fr * gm[g_kid[i]].grow / tg); g_ksize[i] += add; left -= add; last = i; }
            g_ksize[last] += left; fr = 0;
        } else if (fr < 0) {
            long tot = 0; for (u32 i = a; i < b; i++) tot += (long)gm[g_kid[i]].shrink * g_kbase[i];
            if (tot > 0) { int need = -fr; for (u32 i = a; i < b; i++) { int cut = (int)((long)need * gm[g_kid[i]].shrink * g_kbase[i] / tot); g_ksize[i] = g_ksize[i] - cut < g_kmin[i] ? g_kmin[i] : g_ksize[i] - cut; } }
            fr = W; for (u32 i = a; i < b; i++) fr -= g_ksize[i] + gm[g_kid[i]].ml + gm[g_kid[i]].mr + (i > a ? gap : 0);
        }
        int am = fr > 0 && nauto ? fr / nauto : 0; if (am) fr = 0;               // margin: auto soaks up the free space
        int x = e->s.left, sp = 0, cnt = (int)(b - a);
        if (fr > 0) { if (just == 1) x += fr / 2; else if (just == 2) x += fr; else if (just == 3 && cnt > 1) sp = fr / (cnt - 1); else if (just == 4) { sp = fr / cnt; x += sp / 2; } else if (just == 5) { sp = fr / (cnt + 1); x += sp; } }
        for (u32 i = a; i < b; i++) {
            struct gmeas *g = &gm[g_kid[i]];
            x += ((g->autom & 1) ? am : 0) + g->ml;
            g->px = x; g->pw = g_ksize[i] > 1 ? g_ksize[i] : 1; g->planned = 1; g->rowstart = (u8)(i == a && !first_line);
            x += g->pw + g->mr + ((g->autom & 2) ? am : 0) + gap + sp;
        }
        first_line = 0; a = b;
    }
}
static void g_plan_col(struct gnode *e, int W, int align) {                     // flex column with align-items start / center / end: children shrink to fit
    u32 n = g_kids(e->ei, g_kid, 512);
    for (u32 i = 0; i < n; i++) {
        struct gmeas *g = &gm[g_kid[i]]; int room = W - g->ml - g->mr, w = g_base(g, W); if (w > room) w = room; if (w < 1) w = 1;
        g->pw = w; g->px = align == 2 ? e->s.left + g->ml + (room - w) / 2 : align == 3 ? e->s.left + W - g->mr - w : e->s.left + g->ml;
        g->planned = 1; g->rowstart = 0;
    }
}
static void g_plan_grid(struct gnode *e, const struct crule *st, int W, int gap) {
    u32 n = g_kids(e->ei, g_kid, 512); if (!n) return;
    int tw[GT_MAX], cols;
    if (st->gt_n == 255) { cols = (W + gap) / (st->gt_min + gap); if (cols < 1) cols = 1; if (cols > GT_MAX) cols = GT_MAX; for (int c = 0; c < cols; c++) tw[c] = (W - gap * (cols - 1)) / cols; }
    else {
        cols = st->gt_n; int fixed = 0, frt = 0, nauto = 0;
        for (int c = 0; c < cols; c++) { tw[c] = 0; if (st->gt_t[c] == 0) tw[c] = st->gt_v[c]; else if (st->gt_t[c] == 1) tw[c] = st->gt_v[c] * W / 100; else if (st->gt_t[c] == 2) frt += st->gt_v[c]; else nauto++; fixed += tw[c]; }
        if (nauto) {                                                            // auto columns: as wide as their widest item
            int col = 0;
            for (u32 i = 0; i < n; i++) { struct gmeas *g = &gm[g_kid[i]]; int sp = g->gspan == 255 ? cols : g->gspan ? g->gspan : 1; if (sp > cols) sp = cols; if (col + sp > cols) col = 0;
                if (sp == 1 && st->gt_t[col] == 3) { int w = g->maxc + g->ml + g->mr; if (w > tw[col]) tw[col] = w; } col += sp; if (col >= cols) col = 0; }
            fixed = 0; for (int c = 0; c < cols; c++) fixed += tw[c];
        }
        int rest = W - fixed - gap * (cols - 1); if (rest < 0) rest = 0;
        if (frt) { for (int c = 0; c < cols; c++) if (st->gt_t[c] == 2) tw[c] = (int)((long)rest * st->gt_v[c] / frt); }
        else if (nauto) { for (int c = 0; c < cols; c++) if (st->gt_t[c] == 3) tw[c] += rest / nauto; }
    }
    int col = 0, first = 1;
    for (u32 i = 0; i < n; i++) {
        struct gmeas *g = &gm[g_kid[i]];
        int sp = g->gspan == 255 ? cols : g->gspan ? g->gspan : 1; if (sp > cols) sp = cols;
        int newrow = 0; if (col + sp > cols) { col = 0; newrow = 1; }
        int x = e->s.left; for (int c = 0; c < col; c++) x += tw[c] + gap;
        int w = gap * (sp - 1); for (int c = col; c < col + sp; c++) w += tw[c];
        g->px = x + g->ml; g->pw = w - g->ml - g->mr; if (g->pw < 1) g->pw = 1; g->planned = 1; g->rowstart = (u8)(newrow && !first);
        first = 0; col += sp; if (col >= cols && i + 1 < n) { col = 0; gm[g_kid[i + 1]].rowstart = 0; }
        if (col == 0 && i + 1 < n) { /* the next item starts a new row */ }
    }
    // mark row starts (items placed at column 0, except the first)
    col = 0;
    for (u32 i = 0; i < n; i++) { struct gmeas *g = &gm[g_kid[i]]; int sp = g->gspan == 255 ? cols : g->gspan ? g->gspan : 1; if (sp > cols) sp = cols; if (col + sp > cols) col = 0; g->rowstart = (u8)(col == 0 && i > 0); col += sp; if (col >= cols) col = 0; }
}
#define GT_COLS 32
static u32 g_trow[1024];
static void g_plan_table(struct gnode *e, int W, int explicit_w) {
    u32 nr = 0;
    for (u32 c = gm[e->ei].first; c != GNONE && nr < 1024; c = gm[c].next) {
        if (gm[c].hidden) continue;
        if (gm[c].disp == 9) g_trow[nr++] = c;
        else if (gm[c].disp == 13) for (u32 r = gm[c].first; r != GNONE && nr < 1024; r = gm[r].next) if (!gm[r].hidden && gm[r].disp == 9) g_trow[nr++] = r;
    }
    if (!nr) return;
    int cmin[GT_COLS], cmax[GT_COLS], cw[GT_COLS], ncol = 0, sp = e->s.tspace;
    for (int c = 0; c < GT_COLS; c++) cmin[c] = cmax[c] = 0;
    for (int pass = 0; pass < 2; pass++)                                       // single-column cells first, then cells spanning columns
        for (u32 r = 0; r < nr; r++) {
            int col = 0;
            for (u32 c = gm[g_trow[r]].first; c != GNONE; c = gm[c].next) {
                struct gmeas *g = &gm[c]; if (g->hidden || g->disp != 10) continue;
                int span = g->colspan ? g->colspan : 1; if (col + span > GT_COLS) span = GT_COLS - col; if (span <= 0) break;
                int mx = g->maxc, mn = g->minc;
                if (g->width != LEN_AUTO && g->width < 20000) { int w = g->width + (g->boxs ? 0 : g->ext); mx = w > mn ? w : mn; mn = mx; }
                if (pass == 0 && span == 1) { if (mx > cmax[col]) cmax[col] = mx; if (mn > cmin[col]) cmin[col] = mn; }
                if (pass == 1 && span > 1) {
                    int smx = sp * (span - 1), smn = smx; for (int q = col; q < col + span; q++) { smx += cmax[q]; smn += cmin[q]; }
                    if (mx > smx) for (int q = col; q < col + span; q++) cmax[q] += (mx - smx) / span;
                    if (mn > smn) for (int q = col; q < col + span; q++) cmin[q] += (mn - smn) / span;
                }
                col += span; if (col > ncol) ncol = col;
            }
        }
    if (!ncol) return;
    int avail = W - sp * (ncol + 1), smax = 0, smin = 0;
    for (int c = 0; c < ncol; c++) { if (cmin[c] > cmax[c]) cmax[c] = cmin[c]; smax += cmax[c]; smin += cmin[c]; }
    if (smax <= avail) {
        int extra = explicit_w || ncol == 1 ? avail - smax : 0, given = 0;
        for (int c = 0; c < ncol; c++) { cw[c] = cmax[c] + (extra ? (smax ? (int)((long)extra * cmax[c] / smax) : extra / ncol) : 0); given += cw[c]; }
        if (explicit_w && given < avail) cw[ncol - 1] += avail - given;
    } else if (smin >= avail) for (int c = 0; c < ncol; c++) cw[c] = cmin[c];
    else for (int c = 0; c < ncol; c++) cw[c] = cmin[c] + (int)((long)(cmax[c] - cmin[c]) * (avail - smin) / (smax - smin));
    for (u32 r = 0; r < nr; r++) {
        int col = 0;
        for (u32 c = gm[g_trow[r]].first; c != GNONE; c = gm[c].next) {
            struct gmeas *g = &gm[c]; if (g->hidden || g->disp != 10) continue;
            int span = g->colspan ? g->colspan : 1; if (col + span > ncol) span = ncol - col; if (span <= 0) break;
            int x = e->s.left + sp; for (int q = 0; q < col; q++) x += cw[q] + sp;
            int w = sp * (span - 1); for (int q = col; q < col + span; q++) w += cw[q];
            g->px = x; g->pw = w > 1 ? w : 1; g->planned = 1; g->rowstart = 0;
            col += span;
        }
    }
}

static void g_open(const char *tn, const char *idv, const char *clsv, const char *stylev, const char *presv, const char *href) {
    if (gsp + 1 >= GSTACK) return;
    struct gnode *par = &gstk[gsp], *e = &gstk[gsp + 1];
    u32 k = 0; while (tn[k] && k < 11) { e->tag[k] = tn[k]; k++; } e->tag[k] = 0;
    e->htag = g_hash(tn, k); e->hid = idv[0] ? g_hash(idv, g_slen(idv)) : 0;
    e->ncls = 0;
    for (u32 i = 0; clsv[i] && e->ncls < 4;) { while (clsv[i] == ' ') i++; u32 s = i; while (clsv[i] && clsv[i] != ' ') i++; if (i > s) e->hcls[e->ncls++] = g_hash(clsv + s, i - s); }
    e->s = par->s; e->s.block = 0; e->s.imgw = e->s.imgh = 0;
    e->bgitem = e->blitem = e->britem = e->bbitem = e->ibitem = GNONE; e->pb = e->bb = e->mb = e->mt = e->minh = 0; e->ibpr = e->ibmr = 0;
    e->cmode = 0; e->atomic = 0; e->mx = e->mn = 0; e->ext = 0; e->wfix = -1; e->ml = e->mr = 0; e->ccount = 0; e->va = 0; e->isbox = 0; e->gapx = e->rgap = 0;
    e->istart = e->cstart = gin; e->mlo = gmn;
    u32 ei = gm && gm_n < gm_max ? gm_n++ : GNONE; e->ei = ei;
    struct gstyle *s = &e->s;
    int fs = par->s.size;
    // HTML defaults (Chrome's built-in style sheet); ua[] = margin top/right/bottom/left, padding x4, border x4 in pixels
    int ua[12]; for (u32 q = 0; q < 12; q++) ua[q] = 0;
    u32 ua_bcol = 0xffffffff;
    int disp = tag_is(tn, "table") ? 8 : tag_is(tn, "tr") ? 9 : tag_is(tn, "td") || tag_is(tn, "th") ? 10 : tag_is(tn, "thead") || tag_is(tn, "tbody") || tag_is(tn, "tfoot") ? 13 : tag_is(tn, "li") ? 11 : g_is_block(tn) ? 1 : 2;
    if (tn[0] == 'h' && tn[1] >= '1' && tn[1] <= '6' && !tn[2]) {
        static const u16 em[6] = {200, 150, 117, 100, 83, 67}, mg[6] = {67, 83, 100, 133, 167, 233};
        u32 sz = (u32)fs * em[tn[1] - '1'] / 100; s->size = (u8)(sz > 60 ? 60 : sz < 8 ? 8 : sz); s->bold = 1;
        ua[0] = ua[2] = s->size * mg[tn[1] - '1'] / 100;
    }
    else if (tag_is(tn, "b") || tag_is(tn, "strong")) s->bold = 1;
    else if (tag_is(tn, "th")) { s->bold = 1; s->align = 1; }
    else if (tag_is(tn, "pre")) { s->mono = 1; s->pre = 1; s->size = 13; ua[0] = ua[2] = 13; }
    else if (tag_is(tn, "code") || tag_is(tn, "kbd") || tag_is(tn, "samp") || tag_is(tn, "tt")) { s->mono = 1; s->size = 13; }
    else if (tag_is(tn, "small") || tag_is(tn, "sub") || tag_is(tn, "sup")) s->size = (u8)(fs * 5 / 6);
    else if (tag_is(tn, "a") && href[0]) { s->color = 0x0000ee; s->under = 1; }
    else if (tag_is(tn, "u") || tag_is(tn, "ins")) s->under = 1;
    else if (tag_is(tn, "center")) s->align = 1;
    else if (tag_is(tn, "ul") || tag_is(tn, "ol") || tag_is(tn, "menu")) {
        int nested = tag_is(par->tag, "li") || tag_is(par->tag, "ul") || tag_is(par->tag, "ol");
        if (!nested) ua[0] = ua[2] = fs;
        ua[7] = 40; s->list_n = 0; s->list_ol = tag_is(tn, "ol") ? 1 : 0;
    }
    else if (tag_is(tn, "p") || tag_is(tn, "dl")) ua[0] = ua[2] = fs;
    else if (tag_is(tn, "blockquote") || tag_is(tn, "figure")) { ua[0] = ua[2] = fs; ua[1] = ua[3] = 40; }
    else if (tag_is(tn, "dd")) ua[3] = 40;
    else if (tag_is(tn, "body")) ua[0] = ua[1] = ua[2] = ua[3] = 8;
    else if (tag_is(tn, "hr")) { ua[0] = ua[2] = 8; ua[8] = ua[9] = ua[10] = ua[11] = 1; ua_bcol = 0xc8c8c8; }
    else if (tag_is(tn, "table")) {
        s->cellpad = (short)g_cellpad; s->tborder = (u8)(g_tborder > 0); s->align = 0;
        if (g_tborder > 0) { ua[8] = ua[9] = ua[10] = ua[11] = 1; ua_bcol = 0x808080; }
    }
    else if (tag_is(tn, "fieldset")) { ua[1] = ua[3] = 2; ua[4] = 5; ua[5] = ua[7] = 12; ua[6] = 10; ua[8] = ua[9] = ua[10] = ua[11] = 2; ua_bcol = 0xc0c0c0; }
    else if (tag_is(tn, "option")) { if (tag_is(par->tag, "select") || tag_is(par->tag, "optgroup")) { if (par->s.list_n++) s->hidden = 1; } }   // a closed drop-down shows its first choice
    else if (tag_is(tn, "svg")) { disp = 3; s->svg = 1; }                    // icons: their space is kept (the drawing itself is not shown yet)
    else if (tag_is(tn, "script") || tag_is(tn, "style") || tag_is(tn, "head") || tag_is(tn, "noscript") || tag_is(tn, "template") || tag_is(tn, "iframe") || tag_is(tn, "dialog") || tag_is(tn, "datalist") || tag_is(tn, "map") || tag_is(tn, "object")) s->hidden = 1;
    if (par->s.svg) s->hidden = 1;
    int ua_radius = 0;
    if (g_itype) {                                                            // form controls (Chrome's look): text boxes, buttons, drop-downs, check boxes
        disp = 3; s->size = 13; s->color = 0; s->bold = 0; s->under = 0; s->list_n = 0;
        ua[8] = ua[9] = ua[10] = ua[11] = 1; ua_bcol = 0x767676; ua_radius = 2;
        if (g_itype == 1 || g_itype == 6) { ua[4] = ua[6] = 1; ua[5] = ua[7] = 2; s->nowrap = g_itype == 1; s->align = 0; }
        else if (g_itype == 2 || g_itype == 5) { ua[4] = ua[6] = 1; ua[5] = ua[7] = 6; if (g_itype == 5) ua[5] = 22; s->align = g_itype == 2; s->nowrap = 1; }
        else { ua[0] = ua[2] = ua[1] = 3; ua[3] = 4; ua_radius = g_itype == 4 ? 20050 : 2; }
    }
    if (disp == 10) { int cp = par->s.cellpad >= 0 ? par->s.cellpad : 1; ua[4] = ua[5] = ua[6] = ua[7] = cp; if (par->s.tborder) { ua[8] = ua[9] = ua[10] = ua[11] = 1; ua_bcol = 0x808080; } }
    // CSS: for each property the winning declaration (!important, then style="", then specificity and order; presentational attributes lowest)
    gsp++;
    u64 have = 0; u64 rank[CP_NBITS];
    struct crule best;
    if (presv[0]) {
        struct crule pr; pr.set = 0; pr.imp = 0; u32 sl = 0; while (presv[sl]) sl++;
        c_decls(presv, sl, &pr, par->s.size);
        for (u64 m = pr.set; m; m &= m - 1) { u32 b = (u32)__builtin_ctzll(m); have |= 1ull << b; rank[b] = 0; c_take(&best, &pr, b); }
    }
    if (crule_next && ncrules) {
        u32 bk[8], nb = 0;
        #define ADDB(x) do { u32 b_ = (x) & 4095, d_ = 0; for (u32 q_ = 0; q_ < nb; q_++) if (bk[q_] == b_) d_ = 1; if (!d_) bk[nb++] = b_; } while (0)
        if (e->hid) ADDB(e->hid);
        for (u32 q = 0; q < e->ncls; q++) ADDB(e->hcls[q]);
        ADDB(e->htag); ADDB(0);
        #undef ADDB
        for (u32 bi = 0; bi < nb; bi++)
            for (u32 r = crule_head[bk[bi]]; r != 0xffffffff; r = crule_next[r]) {
                struct crule *cr = &crules[r];
                if (!c_match(cr, gsp)) continue;
                u64 base = ((u64)cr->spec << 24 | cr->order) + 1;
                for (u64 m = cr->set; m; m &= m - 1) {
                    u32 b = (u32)__builtin_ctzll(m); u64 bit = 1ull << b;
                    u64 rk = base | ((cr->imp & bit) ? 1ull << 42 : 0);
                    if ((have & bit) && rk < rank[b]) continue;
                    have |= bit; rank[b] = rk; c_take(&best, cr, b);
                }
            }
    }
    if (stylev[0]) {
        struct crule in; in.set = 0; in.imp = 0; u32 sl = 0; while (stylev[sl]) sl++;
        c_decls(stylev, sl, &in, par->s.size);
        for (u64 m = in.set; m; m &= m - 1) {
            u32 b = (u32)__builtin_ctzll(m); u64 bit = 1ull << b;
            u64 rk = 1ull << 41 | ((in.imp & bit) ? 1ull << 42 : 0);
            if ((have & bit) && rk < rank[b]) continue;
            have |= bit; rank[b] = rk; c_take(&best, &in, b);
        }
    }
    if (have & CP_COLOR) s->color = best.color;
    if (have & CP_BG) { if (best.bg == BG_CUR) best.bg = s->color; if (best.bg == BG_NONE || ((have & CP_MASK) && best.mask)) have &= ~CP_BG; }   // transparent; mask icons are not drawn
    if ((have & CP_BCOL) && best.bcolor == BG_CUR) best.bcolor = s->color;
    if (have & CP_SIZE) s->size = best.size;
    if (have & CP_BOLD) s->bold = best.bold;
    if (have & CP_ALIGN) s->align = best.align;
    if (have & CP_DECO) s->under = best.deco;
    if (have & CP_MONO) s->mono = best.mono;
    if (have & CP_LIST) s->listnone = best.listnone;
    if (have & CP_WS) { s->nowrap = best.ws == 1; s->pre = best.ws == 2; }
    if (have & CP_VALIGN) e->va = best.valign;
    if (have & CP_LH) s->lh = best.lh;
    if (have & CP_TT) s->tt = best.tt;
    if (s->svg && !(have & CP_WIDTH)) s->hidden = 1;                         // an icon without a size: nothing to keep room for
    if (have & CP_DISPLAY) { if (best.display == 0) s->hidden = 1; else disp = best.display; }
    int css_cell = 0;
    if (!g_is_tabletag(tn)) { if (disp == 8) disp = 1; else if (disp == 13) disp = 1; else if (disp == 9) disp = 4; else if (disp == 10) { css_cell = 1; disp = par->cmode ? 1 : 3; } }   // CSS tables made of divs: rows of cells side by side
    else if (disp == 10 && par->cmode != 4) disp = 1;                         // a cell outside a row
    if (!s->hidden && par->cmode) { if (disp == 2 || disp == 3 || disp == 11) disp = 1; else if (disp == 5) disp = 4; else if (disp == 7) disp = 6; }   // flex / grid items are blocks
    if (disp == 8) { int bc = (have & CP_BCOLL) && best.bcoll; s->tspace = (short)(g_cellspace >= 0 ? g_cellspace : bc ? 0 : 2); }
    int atomic = disp == 3 || disp == 5 || disp == 7;
    s->disp = (u8)disp; s->block = (u8)(disp != 2);
    // the element tree (pass 1)
    if (g_meas && ei != GNONE) {
        struct gmeas *g = &gm[ei];
        g->parent = par->ei; g->first = g->last = g->next = GNONE; g->hidden = s->hidden; g->disp = (u8)disp; g->hastext = 0; g->planned = 0; g->rowstart = 0;
        g->colspan = (u8)(g_colspan > 32 ? 32 : g_colspan); g->maxc = g->minc = 0; g->ext = 0; g->ml = g->mr = 0; g->autom = 0;
        g->grow = (have & CP_GROW) ? best.grow : css_cell ? 100 : 0; g->shrink = (have & CP_SHRINK) ? best.shrink : 100; g->basis = (have & CP_BASIS) ? best.basis : LEN_AUTO;
        g->width = (have & CP_WIDTH) ? best.width : LEN_AUTO; g->gspan = (have & CP_GSPAN) ? best.gspan : 0; g->boxs = (u8)((have & CP_BOXS) && best.boxs);
        if (par->ei != GNONE) { struct gmeas *pg = &gm[par->ei]; if (pg->last == GNONE) pg->first = ei; else gm[pg->last].next = ei; pg->last = ei; }
    }
    if (s->hidden) return;
    // the box: margins, padding, borders (percentages are of the parent's width)
    int pw = par->s.right - par->s.left; if (pw < 1) pw = 1;
    int bxv[12]; for (u32 q = 0; q < 12; q++) bxv[q] = (have & CP_BOX(q)) ? best.box[q] : ua[q];
    int mlauto = bxv[3] == LEN_AUTO, mrauto = bxv[1] == LEN_AUTO;
    int m[4], pd[4], bd[4];
    for (u32 q = 0; q < 4; q++) {
        m[q] = RES(bxv[q], pw);
        pd[q] = RES(bxv[4 + q], pw); if (pd[q] < 0) pd[q] = 0; if (pd[q] > 400) pd[q] = 400;
        bd[q] = bxv[8 + q]; if (bd[q] < 0 || bd[q] >= 20000) bd[q] = 0;
    }
    u32 bcol = (have & CP_BCOL) ? best.bcolor : ua_bcol != 0xffffffff ? ua_bcol : s->color;
    int rad = (have & CP_RADIUS) ? best.radius : ua_radius;
    u16 renc = rad <= 0 ? 0 : rad >= 20000 ? (u16)(0x8000 | (rad - 20000 > 100 ? 100 : rad - 20000)) : (u16)(rad > 2000 ? 2000 : rad);
    int wset = (have & CP_WIDTH) && best.width != LEN_AUTO && !(g_meas && best.width >= 20000);
    int borderbox = (have & CP_BOXS) && best.boxs;
    int hp = pd[1] + pd[3] + bd[1] + bd[3];
    if (g_meas && ei != GNONE) { struct gmeas *g = &gm[ei]; g->ext = (short)hp; g->ml = (short)(mlauto ? 0 : m[3]); g->mr = (short)(mrauto ? 0 : m[1]); g->autom = (u8)(mlauto | mrauto << 1); }
    e->ext = hp; e->ml = mlauto ? 0 : m[3]; e->mr = mrauto ? 0 : m[1];
    if (disp == 2) {                                                          // inline: <img> sizes; a background / padding drawn around the text
        if (wset) s->imgw = RES(best.width, pw);
        if ((have & CP_MINH) && best.minh != LEN_AUTO) s->imgh = best.minh;
        if (have & CP_BG) s->bg = best.bg;
        int ml = m[3] > 0 ? m[3] : 0, mr = m[1] > 0 ? m[1] : 0, lp = pd[3] + bd[3], rp = pd[1] + bd[1];
        if ((have & CP_BG) || lp || rp || ml || mr) {
            if (g_pend_space && !g_line_empty) { g_x += aa_faces[g_face_index(&par->s)].g[0].adv; g_pend_space = 0; }
            g_x += ml;
            if (have & CP_BG) {
                const struct aaface *f = &aa_faces[g_face_index(s)];
                e->ibitem = gin; g_rect(g_x, g_y, 1, f->line + pd[0] + pd[2], best.bg);
                if (e->ibitem < gin) { gi[e->ibitem].flags = 4; gi[e->ibitem].off = (u32)(f->ascent + pd[0]); gi[e->ibitem].len = renc; }
            }
            e->ibx0 = g_x; e->ibline = g_line_no; e->ibpr = rp; e->ibmr = mr;
            g_x += lp;
            if (g_meas && g_x > e->mx) e->mx = g_x;
        }
        if (tag_is(tn, "br")) { if (g_line_empty) { const struct aaface *f = &aa_faces[g_face_index(s)]; g_add_line_metrics(f->ascent, f->line - f->ascent); g_line_empty = 0; } g_line_end(); }
        return;
    }
    // a box: where it goes and how wide it is
    if (wset) { int w = RES(best.width, pw); e->wfix = borderbox ? w : w + hp; }
    int mw = -1; if ((have & CP_MAXW) && best.maxw != LEN_AUTO && !(g_meas && best.maxw >= 20000)) { mw = RES(best.maxw, pw); if (!borderbox) mw += hp; }
    struct gmeas *pl = !g_meas && ei != GNONE && gm[ei].planned ? &gm[ei] : 0;
    int prow = par->cmode == 1 || par->cmode == 3 || par->cmode == 4;
    int x0, bbw;
    if (atomic) {                                                             // inline-block: a box that sits in the line like a big character
        int room = par->s.right - par->s.left - e->ml - e->mr;
        bbw = e->wfix >= 0 ? e->wfix : g_meas ? G_HUGE : ei != GNONE ? (gm[ei].maxc < room ? gm[ei].maxc : room) : room;
        if (mw >= 0 && bbw > mw) bbw = mw;
        if (bbw < hp + 1) bbw = hp + 1;
        int sp = g_pend_space && !g_line_empty ? aa_faces[g_face_index(&par->s)].g[0].adv : 0;
        if (!g_meas && !g_line_empty && g_x + sp + e->ml + bbw + e->mr > par->s.right) { g_line_end(); sp = 0; }
        g_x += sp; g_pend_space = 0;
        e->sv_ls = g_line_start; e->sv_asc = g_line_asc; e->sv_desc = g_line_desc; e->sv_empty = g_line_empty; e->sv_y = g_y; e->sv_item = g_line_item; e->sv_plo = g_piece_lo; e->sv_base = g_last_base; g_last_base = -1;
        x0 = g_x + e->ml;
        g_line_empty = 1; g_line_asc = g_line_desc = 0; g_piece_lo = gpn; g_last_margin = 0;
        e->atomic = 1;
    } else if (prow && (pl || g_meas)) {                                     // a flex / grid item or table cell: placed by the parent's plan
        g_line_end();
        if (pl && pl->rowstart) { g_row_finish(par); par->rowtop = par->rowbot + par->rgap; par->rowbot = par->rowtop; }
        g_y = par->rowtop + (m[0] > 0 ? m[0] : 0); g_last_margin = 0;
        if (g_meas) { x0 = par->cursor + e->ml; bbw = e->wfix >= 0 ? e->wfix : G_HUGE; }
        else { x0 = pl->px; bbw = pl->pw; }
    } else {
        if (par->cmode == 2) { g_line_end(); if (par->ccount) g_y += par->rgap; g_last_margin = 0; }
        par->ccount++;
        g_vspace(m[0] > 0 ? m[0] : 0);
        if (pl) { x0 = pl->px; bbw = pl->pw; }                                // flex column item that is not stretched
        else {
            bbw = pw - e->ml - e->mr;
            int sized = 0;
            if (e->wfix >= 0) { bbw = e->wfix; sized = 1; }
            if (mw >= 0 && bbw > mw) { bbw = mw; sized = 1; }
            if (disp == 8 && e->wfix < 0 && !g_meas && ei != GNONE && gm[ei].maxc > 0 && gm[ei].maxc < bbw) { bbw = gm[ei].maxc; sized = 1; }   // tables shrink to their content
            if (g_meas && e->wfix < 0) bbw = G_HUGE;
            x0 = par->s.left + e->ml;
            if (sized && (mlauto || mrauto)) {                                // margin: auto centres a box that has a width
                int fr = pw - bbw - e->ml - e->mr; if (fr < 0) fr = 0;
                x0 = par->s.left + (mlauto && mrauto ? fr / 2 : mlauto ? fr : e->ml);
            }
        }
    }
    if (bbw < hp + 8 && !atomic) bbw = hp + 8;
    s->left = x0 + bd[3] + pd[3]; s->right = x0 + bbw - bd[1] - pd[1];
    if (s->right < s->left + 1) s->right = s->left + 1;
    e->bx = x0; e->bw = bbw; e->by = g_y; e->bcolor = bcol; e->mt = m[0] > 0 ? m[0] : 0;
    int isroot = tag_is(tn, "body") || tag_is(tn, "html");
    if (have & CP_BG) { s->bg = best.bg; if (isroot) gpage_bg = best.bg; }
    int bdraw = bcol != BG_NONE;                                              // transparent borders take room but are not drawn
    if (renc && bd[0] == bd[1] && bd[1] == bd[2] && bd[2] == bd[3] && !isroot && ((have & CP_BG) || (bd[0] && bdraw))) {   // rounded corners: one box item
        e->isbox = 1; e->bgitem = gin; g_rect(x0, g_y, bbw, 1, (have & CP_BG) ? best.bg : 0);
        if (e->bgitem < gin) { struct gitem *bx = &gi[e->bgitem]; bx->type = GI_BOX; bx->bg = bcol; bx->off = bdraw ? (u32)bd[0] : 0; bx->len = renc; bx->flags = (have & CP_BG) ? 0 : 16; }
    } else {
        if ((have & CP_BG) && !isroot) { e->bgitem = gin; g_rect(x0, g_y, bbw, 1, best.bg); }
        if (bdraw) {
            if (bd[0]) g_rect(x0, g_y, bbw, bd[0], bcol);
            if (bd[3]) { e->blitem = gin; g_rect(x0, g_y, bd[3], 1, bcol); }
            if (bd[1]) { e->britem = gin; g_rect(x0 + bbw - bd[1], g_y, bd[1], 1, bcol); }
        }
    }
    g_y += bd[0] + pd[0]; if (bd[0] + pd[0]) g_last_margin = 0;
    if (disp == 8) g_y += s->tspace;
    e->cy = g_y; e->pb = pd[2]; e->bb = bd[2]; e->mb = m[2] > 0 ? m[2] : 0;
    if ((have & CP_MINH) && best.minh != LEN_AUTO && best.minh > 0 && best.minh < 20000) { e->minh = best.minh; if (borderbox) e->minh -= pd[0] + pd[2] + bd[0] + bd[2]; }
    e->cstart = gin;
    g_x = s->left; g_line_start = g_x; g_line_item = gin;
    // containers: flex, grid, table rows
    int W = s->right - s->left;
    e->gapx = (have & CP_GAP) ? RES(best.gap, W) : 0; e->rgap = (have & CP_RGAP) ? RES(best.rgap, W) : 0;
    e->aitems = (have & CP_AITEMS) ? best.aitems : 0;
    if (disp == 4 || disp == 5) e->cmode = (have & CP_FDIR) && best.fdir ? 2 : 1;
    else if (disp == 6 || disp == 7) e->cmode = (have & CP_GRIDT) && best.gt_n ? 3 : 2;
    else if (disp == 9) { e->cmode = 4; e->gapx = s->tspace; }
    if (!g_meas && e->cmode && e->cmode != 4 && ei != GNONE && gm[ei].hastext) e->cmode = 0;   // loose text in a flex box: lay it out as a normal block
    e->rowtop = e->rowbot = g_y; e->cursor = s->left + (disp == 9 ? s->tspace : 0); e->mlo = gmn; e->ccount = 0;
    if (!g_meas && ei != GNONE) {
        if (e->cmode == 1) g_plan_flex(e, W, (have & CP_JUST) ? best.just : 0, (have & CP_WRAP) && best.wrap, e->gapx);
        else if (e->cmode == 2 && (disp == 4 || disp == 5) && e->aitems) g_plan_col(e, W, e->aitems);
        else if (e->cmode == 3) g_plan_grid(e, &best, W, e->gapx);
        else if (disp == 8) g_plan_table(e, W, e->wfix >= 0);
    }
    if (disp == 11 && !s->listnone) {                                         // the bullet / number
        struct gstyle *ps = &par->s; ps->list_n++;
        const struct aaface *f = &aa_faces[g_face_index(s)];
        if (par->s.list_ol) {
            char nb[12]; u32 v = ps->list_n, q = 0; char t[10]; u32 tc = 0; do { t[tc++] = (char)('0' + v % 10); v /= 10; } while (v); while (tc) nb[q++] = t[--tc]; nb[q++] = '.';
            int mx0 = e->mx; g_x = s->left - (int)g_text_w(f, nb, q) - 6; g_line_start = g_x; g_word(nb, q); g_x = s->left; g_line_start = s->left; e->mx = mx0;
        } else {
            g_add_line_metrics(f->ascent, f->line - f->ascent); g_line_empty = 0;
            int d = f->size / 3 + 1;
            g_rect(s->left - 14, g_y + f->ascent - f->ascent / 2 - d / 2, d, d, s->color);
        }
    }
}
static void g_close_top(void) {
    struct gnode *e = &gstk[gsp], *par = &gstk[gsp > 0 ? gsp - 1 : 0];
    if (e->s.hidden) {}
    else if (!e->s.block) {                                                   // inline end
        g_x += e->ibpr;
        if (e->ibitem < gin) { int w = g_line_no == e->ibline ? g_x - e->ibx0 : 0; gi[e->ibitem].w = (u16)(w > 0 ? w : 0); }
        g_x += e->ibmr;
        if (g_meas) { if (g_x > e->mx) e->mx = g_x; if (e->mx > par->mx) par->mx = e->mx; if (e->mn > par->mn) par->mn = e->mn; }
    } else {
        if (!g_meas && (e->cmode == 1 || e->cmode == 3 || e->cmode == 4)) { g_line_end(); g_row_finish(e); if (e->rowbot > g_y) g_y = e->rowbot; g_last_margin = 0; }
        g_line_end();
        if (e->minh > 0 && g_y - e->cy < e->minh) g_y = e->cy + e->minh;
        if (e->pb) { g_y += e->pb; g_last_margin = 0; }
        if (e->bb) { if (!e->isbox && e->bcolor != BG_NONE) { e->bbitem = gin; g_rect(e->bx, g_y, e->bw, e->bb, e->bcolor); } g_y += e->bb; g_last_margin = 0; }
        if (!g_meas && tag_is(e->tag, "select")) {                            // the drop-down arrow
            int ax = e->bx + e->bw - 15, ay = e->cy + (g_y - e->cy) / 2 - 2;
            for (int q = 0; q < 4; q++) g_rect(ax + q, ay + q, 7 - 2 * q, 1, e->s.color);
        }
        int hgt = g_y - e->by; if (hgt < 0) hgt = 0; if (hgt > 65535) hgt = 65535;
        if (e->bgitem < gin) gi[e->bgitem].h = (u16)hgt;
        if (e->blitem < gin) gi[e->blitem].h = (u16)hgt;
        if (e->britem < gin) gi[e->britem].h = (u16)hgt;
        int prow = par->cmode == 1 || par->cmode == 3 || par->cmode == 4;
        if (g_meas) {                                                         // its sizes, for pass 2; and the parent's
            int maxc = e->wfix >= 0 ? e->wfix : (e->mx > e->s.left ? e->mx - e->s.left : 0) + e->ext;
            int minc = e->wfix >= 0 ? e->wfix : e->mn + e->ext;
            if (maxc > G_HUGE / 4) maxc = G_HUGE / 4;
            if (e->ei != GNONE) { gm[e->ei].maxc = maxc; gm[e->ei].minc = minc; }
            int right = e->bx + maxc + e->mr; if (right > par->mx) par->mx = right;
            if (minc + e->ml + e->mr > par->mn) par->mn = minc + e->ml + e->mr;
            if (prow && !e->atomic) par->cursor = e->bx + maxc + e->mr + par->gapx;
            e->bw = maxc;
        }
        if (e->atomic) {                                                      // back to the interrupted line: the box is one piece of it
            const struct aaface *f = &aa_faces[g_face_index(&par->s)];
            int asc = g_last_base > e->by ? g_last_base - e->by : hgt;     // its baseline is the baseline of its last line of text
            int desc = hgt - asc; if (desc < f->line - f->ascent) desc = f->line - f->ascent;
            g_line_start = e->sv_ls; g_line_asc = e->sv_asc; g_line_desc = e->sv_desc; g_y = e->sv_y; g_line_item = e->sv_item; g_piece_lo = e->sv_plo; g_last_base = e->sv_base;
            if (!g_meas) { for (u32 i = e->istart; i < gin; i++) gi[i].flags |= 8; if (gpn < GPIECE_MAX) { gpc[gpn].a = e->istart; gpc[gpn].b = gin; gpc[gpn].top = e->by; gpc[gpn].asc = asc; gpn++; } }
            g_add_line_metrics(asc, desc);
            g_x = e->bx + e->bw + e->mr; g_line_empty = 0; g_pend_space = 0; g_last_margin = 0;
            if (g_meas && g_x > par->mx) par->mx = g_x;
        } else if (prow && !g_meas && e->ei != GNONE && gm[e->ei].planned) {  // a member of its parent's row: aligned when the row is done
            if (gmn < GMEM_MAX) {
                struct gmem *mm = &gmem[gmn++];
                mm->istart = e->istart; mm->cstart = e->cstart; mm->iend = gin; mm->bg = e->bgitem; mm->bl = e->blitem; mm->br = e->britem; mm->bb = e->bbitem;
                mm->top = e->by; mm->hb = hgt; mm->mt = e->mt; mm->mb = e->mb; mm->va = (u8)(par->cmode == 4 ? e->va : 10 + par->aitems);
            }
            int bot = e->by + hgt + e->mb; if (bot > par->rowbot) par->rowbot = bot;
            g_y = par->rowtop; g_last_margin = 0;
        } else if (prow && g_meas) g_y = par->rowtop;
        else {
            g_vspace(e->mb);
            if (e->s.disp == 9) g_y += e->s.tspace;
        }
        if (!e->atomic) g_line_item = gin;
    }
    gsp--;
    if (g_line_empty) { g_x = gstk[gsp].s.left; g_line_start = g_x; }
}
static void g_close(const char *tn) {
    for (int k = gsp; k > 0; k--) if (tag_is(gstk[k].tag, tn)) { while (gsp >= k) g_close_top(); return; }
}
// HTML's optional end tags: a new <li> ends the last one, a cell ends a cell, a row a row, a block ends an open <p> ...
static void g_autoclose(const char *tn) {
    #define CLOSE_TO(k) do { int k_ = (k); while (gsp >= k_) g_close_top(); } while (0)
    if (tag_is(tn, "li")) { for (int k = gsp; k > 0; k--) { if (tag_is(gstk[k].tag, "li")) { CLOSE_TO(k); break; } if (tag_is(gstk[k].tag, "ul") || tag_is(gstk[k].tag, "ol") || tag_is(gstk[k].tag, "menu")) break; } }
    else if (tag_is(tn, "td") || tag_is(tn, "th")) { for (int k = gsp; k > 0; k--) { if (tag_is(gstk[k].tag, "td") || tag_is(gstk[k].tag, "th")) { CLOSE_TO(k); break; } if (tag_is(gstk[k].tag, "tr") || tag_is(gstk[k].tag, "table")) break; } }
    else if (tag_is(tn, "tr")) { for (int k = gsp; k > 0; k--) { if (tag_is(gstk[k].tag, "tr")) { CLOSE_TO(k); break; } if (tag_is(gstk[k].tag, "table")) break; } }
    else if (tag_is(tn, "thead") || tag_is(tn, "tbody") || tag_is(tn, "tfoot")) { for (int k = gsp; k > 0; k--) { if (tag_is(gstk[k].tag, "thead") || tag_is(gstk[k].tag, "tbody") || tag_is(gstk[k].tag, "tfoot")) { CLOSE_TO(k); break; } if (tag_is(gstk[k].tag, "table")) break; } }
    else if (tag_is(tn, "dd") || tag_is(tn, "dt")) { for (int k = gsp; k > 0; k--) { if (tag_is(gstk[k].tag, "dd") || tag_is(gstk[k].tag, "dt")) { CLOSE_TO(k); break; } if (tag_is(gstk[k].tag, "dl")) break; } }
    if (g_is_block(tn) || g_is_tabletag(tn)) {
        for (int k = gsp; k > 0; k--) { if (tag_is(gstk[k].tag, "p")) { CLOSE_TO(k); break; } if (g_is_block(gstk[k].tag) || tag_is(gstk[k].tag, "td") || tag_is(gstk[k].tag, "th")) break; }
    }
    #undef CLOSE_TO
}

// One pass over the page. CSS must already be in crules (g_collect_css). Images use web_img_* (ids in document order).
static void g_pass(const u8 *h, u32 n, int width) {
    gin = 0; gtn = 0; gsp = 0; gpage_bg = 0xffffff; gmn = 0; gpn = 0; g_piece_lo = 0; gm_n = 0;
    g_left0 = 0; g_right0 = width;
    struct gnode *root = &gstk[0];
    root->tag[0] = 0; root->htag = 0; root->hid = 0; root->ncls = 0; root->bgitem = root->blitem = root->britem = root->bbitem = root->ibitem = GNONE;
    root->pb = root->bb = root->mb = root->minh = 0; root->cmode = 0; root->atomic = 0; root->ei = GNONE; root->mx = root->mn = 0; root->ccount = 0;
    struct gstyle *s = &root->s;
    s->size = 16; s->bold = 0; s->mono = 0; s->pre = 0; s->align = 0; s->hidden = 0; s->under = 0; s->block = 1; s->color = 0x000000; s->bg = 0xffffff; s->listnone = 0; s->nowrap = 0; s->imgw = s->imgh = 0;
    s->left = g_left0; s->right = g_right0; s->link = 0; s->list_n = 0; s->list_ol = 0; s->disp = 1; s->cellpad = -1; s->tspace = 2; s->tborder = 0; s->lh = 0; s->tt = 0; s->svg = 0;
    g_x = g_left0; g_y = 0; g_line_start = g_x; g_line_asc = g_line_desc = 0; g_pend_space = 0; g_line_empty = 1; g_line_item = 0; g_last_margin = 0; g_line_no = 0;
    w_nlinks = 0; w_nimg = 0; web_title[0] = 0;
    u32 i = 0, in_title = 0;
    static char tn[12], idv[40], clsv[160], stylev[400], href[WEB_HREF], alt[60], isrc[WEB_HREF], dsrc[WEB_HREF], wv[12], hv[12];
    static char bgv[24], alv[12], valv[12], colv[24], spanv[6], cpv[6], csv[6], bdv[6], pres[240], typev[16], valuev[80], phv[80], szv[6];
    static char tbuf[512]; u32 tl = 0;
    #define FLUSH_TEXT() do { if (tl) { if (in_title) { u32 q = 0; while (web_title[q]) q++; for (u32 z = 0; z < tl && q + 1 < sizeof web_title; z++) { web_title[q++] = tbuf[z]; web_title[q] = 0; } } else g_text(tbuf, tl); tl = 0; } } while (0)
    while (i < n) {
        u8 c = h[i];
        if (c == '<') {
            if (i + 3 < n && h[i + 1] == '!' && h[i + 2] == '-' && h[i + 3] == '-') { FLUSH_TEXT(); i += 4; while (i + 2 < n && !(h[i] == '-' && h[i + 1] == '-' && h[i + 2] == '>')) i++; i += 3; continue; }
            u32 j = i + 1, closing = 0;
            if (j < n && h[j] == '/') { closing = 1; j++; }
            u32 k = 0; while (j < n && (((h[j] | 0x20) >= 'a' && (h[j] | 0x20) <= 'z') || (h[j] >= '0' && h[j] <= '9') || h[j] == '!' || h[j] == '-') && k < 11) tn[k++] = (char)(h[j++] | 0x20);
            tn[k] = 0;
            if (!k) { if (tl < sizeof tbuf - 1) tbuf[tl++] = '<'; i++; continue; }
            if (h[i + 1] == '!' || h[i + 1] == '?') { FLUSH_TEXT(); while (i < n && h[i] != '>') i++; i++; continue; }   // <!DOCTYPE>, <![CDATA[ ...
            FLUSH_TEXT();
            idv[0] = clsv[0] = stylev[0] = href[0] = alt[0] = isrc[0] = dsrc[0] = wv[0] = hv[0] = 0; u32 hidden_attr = 0, nowrap_attr = 0;
            bgv[0] = alv[0] = valv[0] = colv[0] = spanv[0] = cpv[0] = csv[0] = bdv[0] = typev[0] = valuev[0] = phv[0] = szv[0] = 0;
            while (j < n && h[j] != '>') {
                while (j < n && (c_ws((char)h[j]) || h[j] == '/')) j++;
                if (j >= n || h[j] == '>') break;
                char an[16]; u32 a = 0;
                while (j < n && h[j] != '=' && !c_ws((char)h[j]) && h[j] != '>' && h[j] != '/') { if (a < 15) an[a++] = (char)(h[j] | 0x20); j++; }
                an[a] = 0;
                char *dst = 0; u32 dmax = 0;
                if (tag_is(an, "id")) { dst = idv; dmax = sizeof idv; } else if (tag_is(an, "class")) { dst = clsv; dmax = sizeof clsv; } else if (tag_is(an, "style")) { dst = stylev; dmax = sizeof stylev; }
                else if (tag_is(an, "href")) { dst = href; dmax = sizeof href; } else if (tag_is(an, "alt")) { dst = alt; dmax = sizeof alt; } else if (tag_is(an, "src")) { dst = isrc; dmax = sizeof isrc; }
                else if (tag_is(an, "data-src") || tag_is(an, "data-lazy-src") || tag_is(an, "data-original")) { dst = dsrc; dmax = sizeof dsrc; }
                else if (tag_is(an, "width")) { dst = wv; dmax = sizeof wv; } else if (tag_is(an, "height")) { dst = hv; dmax = sizeof hv; }
                else if (tag_is(an, "bgcolor")) { dst = bgv; dmax = sizeof bgv; } else if (tag_is(an, "align")) { dst = alv; dmax = sizeof alv; } else if (tag_is(an, "valign")) { dst = valv; dmax = sizeof valv; }
                else if (tag_is(an, "color")) { dst = colv; dmax = sizeof colv; } else if (tag_is(an, "colspan")) { dst = spanv; dmax = sizeof spanv; }
                else if (tag_is(an, "cellpadding")) { dst = cpv; dmax = sizeof cpv; } else if (tag_is(an, "cellspacing")) { dst = csv; dmax = sizeof csv; } else if (tag_is(an, "border")) { dst = bdv; dmax = sizeof bdv; }
                else if (tag_is(an, "type")) { dst = typev; dmax = sizeof typev; } else if (tag_is(an, "value")) { dst = valuev; dmax = sizeof valuev; }
                else if (tag_is(an, "placeholder")) { dst = phv; dmax = sizeof phv; } else if (tag_is(an, "size")) { dst = szv; dmax = sizeof szv; }
                else if (tag_is(an, "hidden")) hidden_attr = 1;
                else if (tag_is(an, "nowrap")) nowrap_attr = 1;
                while (j < n && c_ws((char)h[j])) j++;
                if (j < n && h[j] == '=') {
                    j++; while (j < n && c_ws((char)h[j])) j++;
                    u8 q = 0; if (j < n && (h[j] == '"' || h[j] == '\'')) q = h[j++];
                    u32 v = 0;
                    while (j < n && (q ? h[j] != q : (!c_ws((char)h[j]) && h[j] != '>'))) {
                        if (dst && v + 1 < dmax) { char ent[8]; u32 used = h[j] == '&' ? web_entity(h + j + 1, n - j - 1, ent) : 0; if (used) { for (u32 e2 = 0; ent[e2] && v + 1 < dmax; e2++) dst[v++] = ent[e2]; j += used + 1; continue; } dst[v++] = (char)h[j]; }
                        j++;
                    }
                    if (dst) dst[v] = 0;
                    if (q && j < n) j++;
                }
            }
            i = j < n ? j + 1 : n;
            if (hidden_attr) { const char *dn = "display:none!important"; u32 q = 0; while (dn[q]) { stylev[q] = dn[q]; q++; } stylev[q] = 0; }
            if (tag_is(tn, "script") || tag_is(tn, "style") || tag_is(tn, "textarea")) {        // raw text up to the end tag: skipped here (style sheets were read before)
                if (!closing && tn[0] == 't') {                               // a text area: an empty box (with its hint text)
                    g_itype = 6; int sp0 = gsp;
                    g_open(tn, idv, clsv, stylev, "width:180px;height:34px;background-color:#fff;", href); g_itype = 0;
                    if (gsp > sp0) { if (phv[0]) { gstk[gsp].s.color = 0x757575; g_text(phv, g_slen(phv)); } g_close_top(); }
                }
                if (!closing) { while (i + 2 < n) { if (h[i] == '<' && h[i + 1] == '/') { u32 q = 0; while (tn[q] && i + 2 + q < n && (h[i + 2 + q] | 0x20) == (u8)tn[q]) q++; if (!tn[q]) break; } i++; } while (i < n && h[i] != '>') i++; i++; }
                continue;
            }
            if (tag_is(tn, "title")) { in_title = !closing; continue; }
            if (closing) { g_close(tn); continue; }
            // old-style presentational attributes, as CSS with the lowest priority
            u32 pn = 0;
            #define PADD(str) do { const char *p_ = (str); while (*p_ && pn + 1 < sizeof pres) pres[pn++] = *p_++; } while (0)
            #define PLEN(v) do { PADD(v); u32 d_ = 1; for (u32 z_ = 0; (v)[z_]; z_++) if (!((v)[z_] >= '0' && (v)[z_] <= '9')) d_ = 0; if (d_ && (v)[0]) PADD("px"); } while (0)
            int is_img = tag_is(tn, "img");
            if (bgv[0]) { PADD("background-color:"); PADD(bgv); PADD(";"); }
            if (wv[0] && !is_img) { PADD("width:"); PLEN(wv); PADD(";"); }
            if (hv[0] && !is_img) { PADD("height:"); PLEN(hv); PADD(";"); }
            if (alv[0]) { if (tag_is(tn, "table") && c_eq(alv, g_slen(alv), "center")) PADD("margin-left:auto;margin-right:auto;"); else if (!tag_is(tn, "table") && !is_img) { PADD("text-align:"); PADD(alv); PADD(";"); } }
            if (valv[0]) { PADD("vertical-align:"); PADD(valv); PADD(";"); }
            if (colv[0] && tag_is(tn, "font")) { PADD("color:"); PADD(colv); PADD(";"); }
            if (nowrap_attr) PADD("white-space:nowrap;");
            int itype = 0;                                                    // form controls: Chrome's default sizes and colours
            if (tag_is(tn, "input")) {
                u32 tlen = g_slen(typev);
                if (c_eq(typev, tlen, "hidden")) continue;
                itype = c_eq(typev, tlen, "submit") || c_eq(typev, tlen, "button") || c_eq(typev, tlen, "reset") || c_eq(typev, tlen, "image") || c_eq(typev, tlen, "file") ? 2 : c_eq(typev, tlen, "checkbox") ? 3 : c_eq(typev, tlen, "radio") ? 4 : 1;
            } else if (tag_is(tn, "button")) itype = 2; else if (tag_is(tn, "select")) itype = 5;
            if (itype == 1) { u32 sz = 0; for (u32 q = 0; szv[q] >= '0' && szv[q] <= '9'; q++) sz = sz * 10 + (u32)(szv[q] - '0'); char wb[12]; u32 w = sz ? (sz > 100 ? 100 : sz) * 7 : 146, wl = 0; char d[6]; u32 dc = 0; do { d[dc++] = (char)('0' + w % 10); w /= 10; } while (w); while (dc) wb[wl++] = d[--dc]; wb[wl] = 0;
                              PADD("width:"); PADD(wb); PADD("px;height:16px;background-color:#fff;"); }
            else if (itype == 2 || itype == 5) PADD("background-color:#efefef;");
            else if (itype == 3 || itype == 4) PADD("width:13px;height:13px;background-color:#fff;");
            pres[pn] = 0;
            #undef PADD
            #undef PLEN
            g_colspan = 0; for (u32 q = 0; spanv[q] >= '0' && spanv[q] <= '9'; q++) g_colspan = g_colspan * 10 + (u32)(spanv[q] - '0');
            g_cellpad = -1; if (cpv[0]) { g_cellpad = 0; for (u32 q = 0; cpv[q] >= '0' && cpv[q] <= '9'; q++) g_cellpad = g_cellpad * 10 + (cpv[q] - '0'); if (g_cellpad > 50) g_cellpad = 50; }
            g_cellspace = -1; if (csv[0]) { g_cellspace = 0; for (u32 q = 0; csv[q] >= '0' && csv[q] <= '9'; q++) g_cellspace = g_cellspace * 10 + (csv[q] - '0'); if (g_cellspace > 50) g_cellspace = 50; }
            g_tborder = 0; if (bdv[0]) { for (u32 q = 0; bdv[q] >= '0' && bdv[q] <= '9'; q++) g_tborder = g_tborder * 10 + (bdv[q] - '0'); if (!(bdv[0] >= '0' && bdv[0] <= '9')) g_tborder = 1; }
            g_autoclose(tn);
            if (tag_is(tn, "input")) {
                g_itype = itype; int sp0 = gsp;
                g_open(tn, idv, clsv, stylev, pres, href); g_itype = 0;
                if (gsp > sp0) {
                    u32 tlen = g_slen(typev);
                    if (itype == 1) { if (valuev[0]) g_text(valuev, g_slen(valuev)); else if (phv[0]) { gstk[gsp].s.color = 0x757575; g_text(phv, g_slen(phv)); } else g_word(" ", 1); }
                    else if (itype == 2) {
                        const char *lb = valuev[0] ? valuev : c_eq(typev, tlen, "file") ? "Choose File" : c_eq(typev, tlen, "reset") ? "Reset" : c_eq(typev, tlen, "button") ? " " : c_eq(typev, tlen, "image") && alt[0] ? alt : "Submit";
                        g_text(lb, g_slen(lb)); if (lb[0] == ' ') g_word(" ", 1);
                    }
                    g_close_top();
                }
                continue;
            }
            g_itype = itype;                                                  // <button> / <select>: their content is laid out inside the box
            if (tag_is(tn, "a")) {
                g_open(tn, idv, clsv, stylev, pres, href);
                if (href[0] && w_nlinks < WEB_MAXLINKS && !gstk[gsp].s.hidden) { web_href_store(href); w_nlinks++; gstk[gsp].s.link = w_nlinks; }
                continue;
            }
            if (is_img) {
                const char *use = dsrc[0] ? dsrc : (isrc[0] && !ci_prefix((const u8 *)isrc, "data:")) ? isrc : "";
                u32 id = use[0] && w_nimg < WEB_MAXIMG ? w_nimg++ : WEB_MAXIMG;
                if (id < WEB_MAXIMG) { u32 k2 = 0; while (use[k2] && k2 < WEB_HREF - 1) { web_img_src[id][k2] = use[k2]; k2++; } web_img_src[id][k2] = 0; }
                int sp0 = gsp;
                g_open(tn, idv, clsv, stylev, pres, href);
                if (gsp > sp0) {
                    struct gstyle *is = &gstk[gsp].s; u32 aw = 0, ah = 0;              // width= / height= on the tag (CSS wins)
                    for (u32 q = 0; wv[q] >= '0' && wv[q] <= '9'; q++) aw = aw * 10 + (u32)(wv[q] - '0');
                    for (u32 q = 0; hv[q] >= '0' && hv[q] <= '9'; q++) ah = ah * 10 + (u32)(hv[q] - '0');
                    if (!is->imgw && aw && aw < 4000) is->imgw = (int)aw; if (!is->imgh && ah && ah < 4000 && (!is->imgw || aw)) is->imgh = (int)ah;
                    if (!gstk[gsp].s.hidden) {
                        if (id < WEB_MAXIMG && web_img_pix[id]) g_image(id);
                        else if (alt[0]) { u32 sv = gstk[gsp].s.color; gstk[gsp].s.color = 0x888888; g_word("[", 1); g_text(alt, g_slen(alt)); g_word("]", 1); gstk[gsp].s.color = sv; }
                    }
                    g_close_top();
                }
                continue;
            }
            int sp0 = gsp;
            g_open(tn, idv, clsv, stylev, pres, href); g_itype = 0;
            if (g_is_void(tn) && gsp > sp0) g_close_top();
            continue;
        }
        if (gstk[gsp].s.hidden && !in_title) { i++; continue; }
        if (c == '&') { char ent[8]; u32 used = web_entity(h + i + 1, n - i - 1, ent); if (used) { for (u32 e2 = 0; ent[e2] && tl < sizeof tbuf - 1; e2++) tbuf[tl++] = ent[e2]; i += used + 1; continue; } }
        if (c >= 0x80) {
            u32 len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1, cp = len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : len == 4 ? c & 7 : c;
            for (u32 q = 1; q < len && i + q < n; q++) cp = cp << 6 | (h[i + q] & 0x3f);
            const char *r = web_cp(cp); while (*r && tl < sizeof tbuf - 1) tbuf[tl++] = *r++;
            i += len; continue;
        }
        if (tl >= sizeof tbuf - 2) FLUSH_TEXT();
        tbuf[tl++] = (char)c;
        i++;
    }
    FLUSH_TEXT();
    while (gsp > 0) g_close_top();
    g_line_end();
    gpage_h = g_y + 8;
    #undef FLUSH_TEXT
}
static void g_layout(const u8 *h, u32 n, int width) {
    if (!gm) { gm_max = 40000; gm = img_alloc(gm_max * sizeof(struct gmeas)); if (!gm) gm_max = 0; }
    g_meas = 1; g_pass(h, n, G_HUGE);                                          // pass 1: measure
    g_meas = 0; g_pass(h, n, width);                                           // pass 2: lay out
}

// Collect the page's style sheets: <style> blocks and up to 4 <link rel=stylesheet>. Linked files are downloaded (base = page address).
static int web_download(const char *url_in, u8 *buf, u32 max);
static void g_collect_css(const u8 *h, u32 n, const char *base) {
    ncrules = 0; css_order = 0; g_screen_w = (int)con_fb.w; cvar_pn = 0; gm = 0;
    crules_max = 12000; crules = img_alloc(crules_max * sizeof(struct crule)); crule_next = img_alloc(crules_max * 4);
    cvar_hash = img_alloc(CVAR_SLOTS * 4); cvar_off = img_alloc(CVAR_SLOTS * 4); cvar_pmax = 512u * 1024; cvar_pool = img_alloc(cvar_pmax);
    if (!cvar_hash || !cvar_off || !cvar_pool) cvar_hash = 0; else for (u32 q = 0; q < CVAR_SLOTS; q++) cvar_hash[q] = 0;
    if (!crules || !crule_next) { crules_max = 0; crule_next = 0; c_index(); return; }
    // the sheets in page order (style blocks point into the page, linked files are downloaded); read twice: custom properties first, then rules
    const u32 cmax = 3u << 20; u8 *cssbuf = img_alloc(cmax); u32 cused = 0, links = 0;
    static const char *sh_p[32]; static u32 sh_n[32]; u32 nsh = 0;
    for (u32 i = 0; i + 6 < n; i++) {
        if (h[i] != '<') continue;
        if ((h[i + 1] | 0x20) == 's' && (h[i + 2] | 0x20) == 't' && (h[i + 3] | 0x20) == 'y' && (h[i + 4] | 0x20) == 'l' && (h[i + 5] | 0x20) == 'e') {
            u32 a = i; while (a < n && h[a] != '>') a++; a++;
            u32 b = a; while (b + 7 < n && !(h[b] == '<' && h[b + 1] == '/' && (h[b + 2] | 0x20) == 's' && (h[b + 3] | 0x20) == 't')) b++;
            if (b > a && nsh < 32) { sh_p[nsh] = (const char *)h + a; sh_n[nsh++] = b - a; }
            i = b;
        } else if ((h[i + 1] | 0x20) == 'l' && (h[i + 2] | 0x20) == 'i' && (h[i + 3] | 0x20) == 'n' && (h[i + 4] | 0x20) == 'k' && links < 24 && cssbuf && nsh < 32 && cmax - cused > 4096) {
            u32 e = i; while (e < n && h[e] != '>') e++;
            int is_css = 0, preload = 0, as_style = 0; char href[1024]; href[0] = 0;
            for (u32 k = i; k + 10 < e; k++) if (ci_prefix(h + k, "stylesheet")) is_css = 1;
            for (u32 k = i; k + 7 < e; k++) { if (ci_prefix(h + k, "preload")) preload = 1; if (ci_prefix(h + k, "as=\"style") || ci_prefix(h + k, "as=style")) as_style = 1; }
            if (preload && as_style) is_css = 1;                              // <link rel=preload as=style onload=...>: a style sheet once its script runs
            for (u32 k = i; k + 7 < e; k++) if ((ci_prefix(h + k, " media=") || ci_prefix(h + k, "\tmedia=") || ci_prefix(h + k, "\nmedia=")) && is_css) {   // media="print" / dark mode only: not for us
                u32 q = k + 7; u8 qc = 0; if (h[q] == '"' || h[q] == '\'') qc = h[q++];
                u32 st = q; while (q < e && (qc ? h[q] != qc : !c_ws((char)h[q]))) q++;
                if (!c_media_ok((const char *)h + st, q - st)) is_css = 0;
                break;
            }
            for (u32 k = i; k + 5 < e; k++) if (ci_prefix(h + k, "href=") && (c_ws((char)h[k - 1]) || h[k - 1] == '"' || h[k - 1] == '\'')) {   // not data-href=
                u32 q = k + 5; u8 qc = 0; if (h[q] == '"' || h[q] == '\'') qc = h[q++];
                u32 v = 0; while (q < e && (qc ? h[q] != qc : !c_ws((char)h[q])) && v + 1 < sizeof href) { if (h[q] == '&' && ci_prefix(h + q, "&amp;")) { href[v++] = '&'; q += 5; continue; } href[v++] = (char)h[q++]; }
                href[v] = 0; break;
            }
            if (is_css && href[0]) {
                char url[1024];
                if (url_resolve(base, href, url, sizeof url)) {
                    static u32 seen[24]; u32 hsh = g_hash(url, g_slen(url)), dup = 0;   // the same file listed twice: read it once
                    for (u32 q = 0; q < links; q++) if (seen[q] == hsh) dup = 1;
                    if (dup) { i = e; continue; }
                    seen[links] = hsh;
                    web_status("loading style sheet...");
                    int got = web_download(url, cssbuf + cused, cmax - cused);
                    if (got > 0) { sh_p[nsh] = (const char *)cssbuf + cused; sh_n[nsh++] = (u32)got; cused += ((u32)got + 15) & ~15u; links++; }
                }
            }
            i = e;
        }
    }
    css_collect_vars = 1; for (u32 q = 0; q < nsh; q++) css_parse(sh_p[q], sh_n[q]);
    css_collect_vars = 0; for (u32 q = 0; q < nsh; q++) css_parse(sh_p[q], sh_n[q]);
    c_index();
}

// ---------------- drawing ----------------
static void g_blend_glyph(const struct aaface *f, u8 ch, int x, int y, u32 fg, u32 bg, int clip_y0, int clip_y1) {
    if (ch < 32 || ch > 126) ch = '?';
    const struct aaglyph *g = &f->g[ch - 32];
    const u8 *b = f->bits + g->off;
    u32 fr = fg >> 16 & 255, fgc = fg >> 8 & 255, fb = fg & 255, br = bg >> 16 & 255, bgc = bg >> 8 & 255, bb = bg & 255;
    for (u32 yy = 0; yy < g->h; yy++) {
        int sy = y + g->y + (int)yy;
        if (sy < clip_y0 || sy >= clip_y1) continue;
        volatile u32 *row = (volatile u32 *)((u8 *)con_fb.addr + (u64)sy * con_fb.stride);
        for (u32 xx = 0; xx < g->w; xx++) {
            u32 a = b[yy * g->w + xx];
            if (!a) continue;
            int sx = x + g->x + (int)xx;
            if (sx < 0 || sx >= (int)con_fb.w) continue;
            u32 c = a == 255 ? fg : ((fr * a + br * (255 - a)) / 255) << 16 | ((fgc * a + bgc * (255 - a)) / 255) << 8 | ((fb * a + bb * (255 - a)) / 255);
            if (con_fb.bpp == 32) row[sx] = c | 0xFF000000; else px((u32)sx, (u32)sy, c);
        }
    }
}
static void g_fill(int x, int y, int w, int h, u32 c, int clip_y0, int clip_y1) {
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (y + h > clip_y1) h = clip_y1 - y;
    if (x < 0) { w += x; x = 0; } if (x + w > (int)con_fb.w) w = (int)con_fb.w - x;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        volatile u32 *row = (volatile u32 *)((u8 *)con_fb.addr + (u64)(y + yy) * con_fb.stride);
        if (con_fb.bpp == 32) { u32 v = c | 0xFF000000; for (int xx = 0; xx < w; xx++) row[x + xx] = v; }
        else for (int xx = 0; xx < w; xx++) px((u32)(x + xx), (u32)(y + yy), c);
    }
}
// rounded corners: how far row yy of a box h rows tall is cut in at each side, for radius r
static u32 g_isqrt(u32 v) { u32 r = 0, b = 1u << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }
static int g_inset(int r, int yy, int h) {
    if (r <= 0) return 0;
    int t = yy < r ? yy : h - 1 - yy; if (t >= r || t < 0) return 0;
    int d2 = 2 * r - 1 - 2 * t; return r - (int)g_isqrt((u32)(4 * r * r - d2 * d2)) / 2;
}
static int g_radius(u16 enc, int w, int h) {
    int m = w < h ? w : h, r = (enc & 0x8000) ? (enc & 0x7fff) * m / 100 : enc;
    return r > m / 2 ? m / 2 : r;
}
static void g_fill_round(int x, int y, int w, int h, int r, u32 c, int y0, int y1) {
    if (r <= 0) { g_fill(x, y, w, h, c, y0, y1); return; }
    for (int yy = 0; yy < h; yy++) { if (y + yy < y0 || y + yy >= y1) continue; int in = g_inset(r, yy, h); g_fill(x + in, y + yy, w - 2 * in, 1, c, y0, y1); }
}
// a box with rounded corners: background inside, border ring around it
static void g_draw_box(const struct gitem *it, int sy, int y0, int y1) {
    int w = it->w, h = it->h, bw = (int)it->off, r = g_radius(it->len, w, h);
    if (!(it->flags & 16)) g_fill_round(it->x, sy, w, h, r, it->color, y0, y1);
    if (bw <= 0) return;
    int ri = r - bw; if (ri < 0) ri = 0;
    for (int yy = 0; yy < h; yy++) {
        int ry = sy + yy; if (ry < y0 || ry >= y1) continue;
        int io = g_inset(r, yy, h);
        if (yy < bw || yy >= h - bw) { g_fill(it->x + io, ry, w - 2 * io, 1, it->bg, y0, y1); continue; }
        int ii = bw + g_inset(ri, yy - bw, h - 2 * bw);
        g_fill(it->x + io, ry, ii - io, 1, it->bg, y0, y1);
        g_fill(it->x + w - ii, ry, ii - io, 1, it->bg, y0, y1);
    }
}
// Paint the page, scrolled to `top` pixels, into screen rows y0..y1. sel = the link picked with Left/Right (highlighted).
static void g_draw(int top, int y0, int y1, u32 sel) {
    g_fill(0, y0, (int)con_fb.w, y1 - y0, gpage_bg, y0, y1);
    for (u32 i = 0; i < gin; i++) {
        struct gitem *it = &gi[i];
        int sy = it->y - top + y0;
        if (sy >= y1 || sy + it->h <= y0) continue;
        if (it->type == GI_RECT) { if (it->len) g_fill_round(it->x, sy, it->w, it->h, g_radius(it->len, it->w, it->h), it->color, y0, y1); else g_fill(it->x, sy, it->w, it->h, it->color, y0, y1); continue; }
        if (it->type == GI_BOX) { g_draw_box(it, sy, y0, y1); continue; }
        if (it->type == GI_IMG) {
            u32 id = it->off; if (id >= WEB_MAXIMG || !web_img_pix[id]) continue;
            for (u32 yy = 0; yy < it->h; yy++) {
                int ry = sy + (int)yy; if (ry < y0 || ry >= y1) continue;
                u32 srcy = yy * web_img_h[id] / it->h; const u32 *src = web_img_pix[id] + srcy * web_img_w[id];
                volatile u32 *row = (volatile u32 *)((u8 *)con_fb.addr + (u64)ry * con_fb.stride);
                for (u32 xx = 0; xx < it->w; xx++) { int rx = it->x + (int)xx; if (rx < 0 || rx >= (int)con_fb.w) continue; u32 v = src[xx * web_img_w[id] / it->w]; if (con_fb.bpp == 32) row[rx] = v | 0xFF000000; else px((u32)rx, (u32)ry, v); }
            }
            if (sel && it->link == sel) { g_fill(it->x, sy, it->w, 2, 0x3a7bd5, y0, y1); g_fill(it->x, sy + it->h - 2, it->w, 2, 0x3a7bd5, y0, y1); }
            continue;
        }
        const struct aaface *f = &aa_faces[it->face];
        u32 fg = it->color, bg = it->bg;
        if (sel && it->link == sel) { bg = 0xcfe3ff; g_fill(it->x - 2, sy, it->w + 4, it->h, bg, y0, y1); }
        int x = it->x, base = sy + f->ascent;
        const char *t = gtx + it->off;
        for (u32 k = 0; k < it->len; k++) { u8 ch = (u8)t[k]; if (ch != ' ') g_blend_glyph(f, ch, x, base - f->ascent, fg, bg, y0, y1); x += f->g[(ch < 32 || ch > 126 ? '?' : ch) - 32].adv; }
        if (it->flags & 1) g_fill(it->x, base + 2, it->w, 1, fg, y0, y1);
    }
}
// the first item of link `id` (for scrolling to it)
static int g_link_y(u32 id) { for (u32 i = 0; i < gin; i++) if (gi[i].link == id && (gi[i].type == GI_TEXT || gi[i].type == GI_IMG)) return gi[i].y; return -1; }
