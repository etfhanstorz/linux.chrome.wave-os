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
struct gitem { int x, y; u16 w, h; u8 type, face, link, flags; u32 color, bg, off; u16 len; };   // flags: 1 = underline
static struct gitem *gi; static u32 gin, gimax;
static char *gtx; static u32 gtn, gtmax;
static int gpage_h; static u32 gpage_bg = 0xffffff;

// ---------------- styles ----------------
struct gstyle { u8 size, bold, mono, pre, align, hidden, under, block; u32 color, bg; int left, right; u32 link; u32 list_n; u8 list_ol; };
struct gnode { char tag[12]; u32 htag, hid, hcls[4]; u32 ncls; struct gstyle s; u32 bgitem; int bgy; };
#define GSTACK 96
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
struct crule { struct ccomp c[4]; u8 nc; u16 set; u16 spec; u32 order; u32 color, bg; u8 size, sizerel, bold, align, display, deco, mono; };
static struct crule *crules; static u32 ncrules, crules_max;
static u32 css_order;

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
// Parse "prop: value; prop: value" into a rule's properties.
static void c_decls(const char *d, u32 n, struct crule *r, u32 parent_px) {
    u32 i = 0;
    while (i < n) {
        while (i < n && (c_ws(d[i]) || d[i] == ';')) i++;
        u32 ps = i; while (i < n && d[i] != ':' && d[i] != ';') i++;
        u32 pe = i; while (pe > ps && c_ws(d[pe - 1])) pe--;
        if (i >= n || d[i] != ':') { while (i < n && d[i] != ';') i++; continue; }
        i++;
        u32 vs = i; int paren = 0; while (i < n && (d[i] != ';' || paren)) { if (d[i] == '(') paren++; else if (d[i] == ')') paren--; i++; }
        const char *p = d + ps, *v = d + vs; u32 pn = pe - ps, vn = i - vs;
        while (vn && c_ws(*v)) { v++; vn--; }
        u32 c;
        if (c_eq(p, pn, "color")) { if (c_color(v, vn, &c)) { r->color = c; r->set |= CP_COLOR; } }
        else if (c_eq(p, pn, "background-color")) { if (c_color(v, vn, &c)) { r->bg = c; r->set |= CP_BG; } }
        else if (c_eq(p, pn, "background")) {                               // the colour part of the shorthand
            u32 k = 0; while (k < vn) { u32 s = k; int pa = 0; while (k < vn && (!c_ws(v[k]) || pa)) { if (v[k] == '(') pa++; else if (v[k] == ')') pa--; k++; } if (c_color(v + s, k - s, &c)) { r->bg = c; r->set |= CP_BG; break; } while (k < vn && c_ws(v[k])) k++; }
        }
        else if (c_eq(p, pn, "font-size")) { u32 px = c_size(v, vn, parent_px ? parent_px : 16); if (px) { r->size = (u8)(px > 60 ? 60 : px < 8 ? 8 : px); r->set |= CP_SIZE; } }
        else if (c_eq(p, pn, "font-weight")) { r->bold = (c_starts(v, vn, "bold") || c_starts(v, vn, "bolder") || (v[0] >= '6' && v[0] <= '9' && vn >= 3)) ? 1 : 0; r->set |= CP_BOLD; }
        else if (c_eq(p, pn, "font-family")) { u32 mono = 0; for (u32 k = 0; k + 4 <= vn; k++) if (c_starts(v + k, vn - k, "mono") || c_starts(v + k, vn - k, "courier")) mono = 1; r->mono = (u8)mono; r->set |= CP_MONO; }
        else if (c_eq(p, pn, "text-align")) { r->align = c_starts(v, vn, "center") ? 1 : c_starts(v, vn, "right") || c_starts(v, vn, "end") ? 2 : 0; r->set |= CP_ALIGN; }
        else if (c_eq(p, pn, "display")) { r->display = c_starts(v, vn, "none") ? 0 : c_starts(v, vn, "inline") ? 2 : 1; r->set |= CP_DISPLAY; }
        else if (c_eq(p, pn, "text-decoration") || c_eq(p, pn, "text-decoration-line")) { r->deco = c_starts(v, vn, "underline") ? 1 : 0; r->set |= CP_DECO; }
        else if (c_eq(p, pn, "visibility")) { if (c_starts(v, vn, "hidden")) { r->display = 0; r->set |= CP_DISPLAY; } }
        if (i < n) i++;
    }
}
// One compound selector like div.nav#top. Returns 0 if it uses something we do not support (then the rule is skipped).
static int c_compound(const char *s, u32 n, struct ccomp *c) {
    c->tag = c->id = 0; c->ncls = 0;
    u32 i = 0;
    if (i < n && s[i] == '*') i++;
    else if (i < n && s[i] != '.' && s[i] != '#') { u32 st = i; while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[' && s[i] != ':') i++; c->tag = g_hash(s + st, i - st); }
    while (i < n) {
        if (s[i] == '.' || s[i] == '#') {
            char k = s[i++]; u32 st = i;
            while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[' && s[i] != ':') i++;
            u32 h = g_hash(s + st, i - st);
            if (k == '#') c->id = h; else if (c->ncls < 3) c->cls[c->ncls++] = h; else return 0;
        } else return 0;                                                      // [attr], :hover, ::before ... not supported
    }
    return 1;
}
// Parse a style sheet (from <style> or a .css file) into rules.
static void css_parse(const char *s, u32 n) {
    u32 i = 0;
    while (i < n) {
        while (i < n && c_ws(s[i])) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') { i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++; i += 2; continue; }
        if (i < n && s[i] == '@') {                                          // @media, @font-face, @keyframes ...: skip the whole block (or statement)
            while (i < n && s[i] != '{' && s[i] != ';') i++;
            if (i < n && s[i] == ';') { i++; continue; }
            int depth = 0; while (i < n) { if (s[i] == '{') depth++; else if (s[i] == '}') { depth--; if (depth == 0) { i++; break; } } i++; }
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
            if (ncrules < crules_max) {
                struct crule *r = &crules[ncrules];
                r->nc = 0; r->set = 0; r->spec = 0; int ok = 1;
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

// ---------------- layout ----------------
static int g_left0, g_right0, g_x, g_y, g_line_start, g_line_asc, g_line_desc, g_pend_space, g_line_empty;
static u32 g_line_item;
static int g_last_margin;
static void g_line_end(void) {
    if (g_line_empty) return;
    struct gstyle *st = &gstk[gsp].s;
    int lh = g_line_asc + g_line_desc;
    // vertical: text sits on the common baseline; pictures sit on it too
    int shift = 0;
    if (st->align && gin > g_line_item) {
        int used = g_x - g_line_start, avail = st->right - g_line_start;
        shift = st->align == 1 ? (avail - used) / 2 : avail - used;
        if (shift < 0) shift = 0;
    }
    for (u32 i = g_line_item; i < gin; i++) {
        struct gitem *it = &gi[i];
        if (it->type == GI_TEXT) it->y = g_y + g_line_asc - aa_faces[it->face].ascent;
        else if (it->type == GI_IMG) it->y = g_y + g_line_asc - it->h;
        it->x += shift;
    }
    g_y += lh;
    g_line_empty = 1; g_line_asc = 0; g_line_desc = 0; g_pend_space = 0;
    g_x = st->left; g_line_start = g_x; g_line_item = gin;
    g_last_margin = 0;
}
static void g_vspace(int px) { g_line_end(); if (px > g_last_margin) { g_y += px - g_last_margin; g_last_margin = px; } }
static u32 g_face_index(const struct gstyle *s) { const struct aaface *f = g_face(s->size, s->bold, s->mono); return (u32)(f - aa_faces); }
static void g_add_line_metrics(int asc, int desc) { if (asc > g_line_asc) g_line_asc = asc; if (desc > g_line_desc) g_line_desc = desc; }
static void g_word(const char *w, u32 n) {
    struct gstyle *st = &gstk[gsp].s;
    if (st->hidden || !n) return;
    u32 fi = g_face_index(st); const struct aaface *f = &aa_faces[fi];
    u32 ww = g_text_w(f, w, n), sp = g_pend_space && !g_line_empty ? f->g[0].adv : 0;
    if (!g_line_empty && g_x + (int)sp + (int)ww > st->right) { g_line_end(); sp = 0; }
    if (gtn + n + 2 > gtmax || gin + 2 > gimax) return;
    struct gitem *prev = gin > g_line_item ? &gi[gin - 1] : 0;
    int lineh = f->line, asc = f->ascent; g_add_line_metrics(asc, lineh - asc);
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
    if (gin + 1 > gimax || w <= 0 || h <= 0) return;
    struct gitem *it = &gi[gin++]; it->type = GI_RECT; it->x = x; it->y = y; it->w = (u16)(w > 65535 ? 65535 : w); it->h = (u16)(h > 65535 ? 65535 : h); it->color = color; it->link = 0; it->flags = 0; it->len = 0;
}
static void g_image(u32 id) {
    struct gstyle *st = &gstk[gsp].s;
    if (st->hidden || gin + 1 > gimax) return;
    u32 w = web_img_w[id], h = web_img_h[id];
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

// tag defaults: block? margins (in "half lines"), size, bold, mono ...
static int g_is_block(const char *t) {
    static const char *b[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "table", "tr", "blockquote", "pre", "section", "article", "header", "footer", "nav", "main", "aside", "form", "dl", "dt", "dd", "figure", "figcaption", "hr", "address", "fieldset", "details", "summary", "center", "body", "html", 0};
    for (u32 i = 0; b[i]; i++) if (tag_is(t, b[i])) return 1;
    return 0;
}
static int g_is_void(const char *t) {
    static const char *v[] = {"br", "img", "hr", "meta", "link", "input", "wbr", "source", "area", "base", "col", "embed", "param", "track", "!doctype", 0};
    for (u32 i = 0; v[i]; i++) if (tag_is(t, v[i])) return 1;
    return 0;
}
static void g_open(const char *tn, const char *idv, const char *clsv, const char *stylev, const char *href, int closing_void) {
    (void)closing_void;
    if (gsp + 1 >= GSTACK) return;
    struct gnode *par = &gstk[gsp], *e = &gstk[gsp + 1];
    u32 k = 0; while (tn[k] && k < 11) { e->tag[k] = tn[k]; k++; } e->tag[k] = 0;
    e->htag = g_hash(tn, k); e->hid = idv[0] ? g_hash(idv, g_slen(idv)) : 0;
    e->ncls = 0;
    for (u32 i = 0; clsv[i] && e->ncls < 4;) { while (clsv[i] == ' ') i++; u32 s = i; while (clsv[i] && clsv[i] != ' ') i++; if (i > s) e->hcls[e->ncls++] = g_hash(clsv + s, i - s); }
    e->s = par->s; e->s.block = 0; e->bgitem = 0xffffffff;
    struct gstyle *s = &e->s;
    // HTML defaults (like Chrome's built-in style sheet)
    if (tn[0] == 'h' && tn[1] >= '1' && tn[1] <= '6' && !tn[2]) { static const u8 hs[6] = {32, 24, 19, 16, 13, 11}; s->size = (u8)(hs[tn[1] - '1'] * 15 / 16 + 1); s->bold = 1; }
    else if (tag_is(tn, "b") || tag_is(tn, "strong") || tag_is(tn, "th")) s->bold = 1;
    else if (tag_is(tn, "pre")) { s->mono = 1; s->pre = 1; s->size = 13; }
    else if (tag_is(tn, "code") || tag_is(tn, "kbd") || tag_is(tn, "samp") || tag_is(tn, "tt")) { s->mono = 1; s->size = 13; }
    else if (tag_is(tn, "small") || tag_is(tn, "sub") || tag_is(tn, "sup")) s->size = (u8)(s->size > 13 ? 13 : s->size);
    else if (tag_is(tn, "a") && href[0]) { s->color = 0x0000ee; s->under = 1; }
    else if (tag_is(tn, "u") || tag_is(tn, "ins")) s->under = 1;
    else if (tag_is(tn, "center")) s->align = 1;
    else if (tag_is(tn, "ul") || tag_is(tn, "ol")) { s->left += 32; s->list_n = 0; s->list_ol = tag_is(tn, "ol") ? 1 : 0; }
    else if (tag_is(tn, "blockquote") || tag_is(tn, "dd")) { s->left += 32; s->right -= 16; }
    else if (tag_is(tn, "script") || tag_is(tn, "style") || tag_is(tn, "head") || tag_is(tn, "noscript") || tag_is(tn, "template") || tag_is(tn, "svg") || tag_is(tn, "iframe") || tag_is(tn, "select") || tag_is(tn, "button") || tag_is(tn, "input") || tag_is(tn, "textarea")) s->hidden = 1;
    int block = g_is_block(tn);
    // CSS rules, lowest specificity first (later rules of equal specificity win)
    gsp++;
    u32 have = 0; u16 bspec[8] = {0}; u32 bord[8] = {0};
    struct crule best; best.set = 0;
    for (u32 r = 0; r < ncrules; r++) {
        if (!c_match(&crules[r], gsp)) continue;
        struct crule *cr = &crules[r];
        for (u32 b = 0; b < 8; b++) {
            u16 bit = (u16)(1u << b);
            if (!(cr->set & bit)) continue;
            if ((have & bit) && (cr->spec < bspec[b] || (cr->spec == bspec[b] && cr->order < bord[b]))) continue;
            have |= bit; bspec[b] = cr->spec; bord[b] = cr->order;
            if (bit == CP_COLOR) best.color = cr->color; else if (bit == CP_BG) best.bg = cr->bg; else if (bit == CP_SIZE) best.size = cr->size;
            else if (bit == CP_BOLD) best.bold = cr->bold; else if (bit == CP_ALIGN) best.align = cr->align; else if (bit == CP_DISPLAY) best.display = cr->display;
            else if (bit == CP_DECO) best.deco = cr->deco; else if (bit == CP_MONO) best.mono = cr->mono;
        }
    }
    if (stylev[0]) {                                                         // style="..." beats every rule
        struct crule in; in.set = 0; u32 sl = 0; while (stylev[sl]) sl++;
        c_decls(stylev, sl, &in, par->s.size);
        for (u32 b = 0; b < 8; b++) { u16 bit = (u16)(1u << b); if (!(in.set & bit)) continue; have |= bit;
            if (bit == CP_COLOR) best.color = in.color; else if (bit == CP_BG) best.bg = in.bg; else if (bit == CP_SIZE) best.size = in.size; else if (bit == CP_BOLD) best.bold = in.bold;
            else if (bit == CP_ALIGN) best.align = in.align; else if (bit == CP_DISPLAY) best.display = in.display; else if (bit == CP_DECO) best.deco = in.deco; else if (bit == CP_MONO) best.mono = in.mono; }
    }
    if (have & CP_COLOR) s->color = best.color;
    if (have & CP_SIZE) s->size = best.size;
    if (have & CP_BOLD) s->bold = best.bold;
    if (have & CP_ALIGN) s->align = best.align;
    if (have & CP_DECO) s->under = best.deco;
    if (have & CP_MONO) s->mono = best.mono;
    if (have & CP_DISPLAY) { if (best.display == 0) s->hidden = 1; else block = best.display == 1; }
    s->block = (u8)block;
    if (s->hidden) return;
    if (block) {
        int top = tn[0] == 'h' && tn[1] >= '1' && tn[1] <= '6' ? s->size * 2 / 3 : tag_is(tn, "p") || tag_is(tn, "ul") || tag_is(tn, "ol") || tag_is(tn, "blockquote") || tag_is(tn, "pre") || tag_is(tn, "table") || tag_is(tn, "dl") || tag_is(tn, "figure") ? 12 : 0;
        g_vspace(top);
        if (g_line_empty) { g_x = s->left; g_line_start = g_x; }              // a fresh line starts at this block's own indent
    }
    if ((have & CP_BG) && block) {                                           // a block with its own background: a rectangle under it (height fixed at the end tag)
        s->bg = best.bg;
        if (tag_is(tn, "body") || tag_is(tn, "html")) gpage_bg = best.bg;
        else { e->bgitem = gin; e->bgy = g_y; g_rect(g_left0, g_y, g_right0 - g_left0, 1, best.bg); }
    } else if (have & CP_BG) s->bg = best.bg;
    if (tag_is(tn, "pre") && !(have & CP_BG)) { e->bgitem = gin; e->bgy = g_y; s->bg = 0xf3f3f3; g_rect(s->left - 6, g_y, s->right - s->left + 12, 1, 0xf3f3f3); }
    if (tag_is(tn, "li")) {                                                  // the bullet / number
        struct gstyle *ps = &par->s; ps->list_n++;
        g_line_end();
        const struct aaface *f = &aa_faces[g_face_index(s)];
        if (par->s.list_ol) {
            char nb[12]; u32 v = ps->list_n, q = 0; char t[10]; u32 tc = 0; do { t[tc++] = (char)('0' + v % 10); v /= 10; } while (v); while (tc) nb[q++] = t[--tc]; nb[q++] = '.';
            g_x = s->left - (int)g_text_w(f, nb, q) - 6; g_line_start = g_x; g_word(nb, q); g_x = s->left; g_line_start = s->left;
        } else {
            g_add_line_metrics(f->ascent, f->line - f->ascent); g_line_empty = 0;
            int d = f->size / 3 + 1;
            g_rect(s->left - 14, g_y + f->ascent - f->ascent / 2 - d / 2, d, d, s->color);   // the bullet
        }
    }
    if (tag_is(tn, "br")) { if (g_line_empty) { const struct aaface *f = &aa_faces[g_face_index(s)]; g_add_line_metrics(f->ascent, f->line - f->ascent); g_line_empty = 0; } g_line_end(); }
    if (tag_is(tn, "hr")) { g_vspace(8); g_rect(s->left, g_y, s->right - s->left, 1, 0xc8c8c8); g_y += 1; g_vspace(8); }
}
static void g_close_top(void) {
    struct gnode *e = &gstk[gsp];
    if (!e->s.hidden && e->s.block) {
        g_line_end();
        int bottom = e->tag[0] == 'h' && e->tag[1] >= '1' && e->tag[1] <= '6' ? e->s.size / 2 : tag_is(e->tag, "p") || tag_is(e->tag, "ul") || tag_is(e->tag, "ol") || tag_is(e->tag, "blockquote") || tag_is(e->tag, "pre") || tag_is(e->tag, "table") || tag_is(e->tag, "dl") || tag_is(e->tag, "figure") ? 12 : 0;
        if (e->bgitem != 0xffffffff && e->bgitem < gin) { int pad = tag_is(e->tag, "pre") ? 6 : 0; gi[e->bgitem].y = e->bgy - pad; gi[e->bgitem].h = (u16)(g_y - e->bgy + 2 * pad > 65535 ? 65535 : g_y - e->bgy + 2 * pad); if (pad) g_y += pad; }
        g_vspace(bottom);
    }
    gsp--;
    g_x = g_line_empty ? gstk[gsp].s.left : g_x;
}
static void g_close(const char *tn) {
    for (int k = gsp; k > 0; k--) if (tag_is(gstk[k].tag, tn)) { while (gsp >= k) g_close_top(); return; }
}

// The whole page: CSS from <style> must already be in crules (g_collect_css). Images use web_img_* (ids in document order).
static void g_layout(const u8 *h, u32 n, int width) {
    gin = 0; gtn = 0; gsp = 0; gpage_bg = 0xffffff;
    g_left0 = 24; g_right0 = width - 24;
    struct gnode *root = &gstk[0];
    root->tag[0] = 0; root->htag = 0; root->hid = 0; root->ncls = 0; root->bgitem = 0xffffffff;
    struct gstyle *s = &root->s;
    s->size = 15; s->bold = 0; s->mono = 0; s->pre = 0; s->align = 0; s->hidden = 0; s->under = 0; s->block = 1; s->color = 0x111111; s->bg = 0xffffff;
    s->left = g_left0; s->right = g_right0; s->link = 0; s->list_n = 0; s->list_ol = 0;
    g_x = g_left0; g_y = 16; g_line_start = g_x; g_line_asc = g_line_desc = 0; g_pend_space = 0; g_line_empty = 1; g_line_item = 0; g_last_margin = 16;
    w_nlinks = 0; w_nimg = 0; web_title[0] = 0;
    u32 i = 0, in_title = 0;
    static char tn[12], idv[40], clsv[160], stylev[400], href[WEB_HREF], alt[60], isrc[WEB_HREF], dsrc[WEB_HREF];
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
            idv[0] = clsv[0] = stylev[0] = href[0] = alt[0] = isrc[0] = dsrc[0] = 0;
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
            if (tag_is(tn, "script") || tag_is(tn, "style") || tag_is(tn, "textarea")) {        // raw text up to the end tag: skipped here (style sheets were read before)
                if (!closing) { while (i + 2 < n) { if (h[i] == '<' && h[i + 1] == '/') { u32 q = 0; while (tn[q] && i + 2 + q < n && (h[i + 2 + q] | 0x20) == (u8)tn[q]) q++; if (!tn[q]) break; } i++; } while (i < n && h[i] != '>') i++; i++; }
                continue;
            }
            if (tag_is(tn, "title")) { in_title = !closing; continue; }
            if (closing) { g_close(tn); continue; }
            if (tag_is(tn, "a")) {
                g_open(tn, idv, clsv, stylev, href, 0);
                if (href[0] && w_nlinks < WEB_MAXLINKS && !gstk[gsp].s.hidden) { web_href_store(href); w_nlinks++; gstk[gsp].s.link = w_nlinks; }
                continue;
            }
            if (tag_is(tn, "img")) {
                const char *use = dsrc[0] ? dsrc : (isrc[0] && !ci_prefix((const u8 *)isrc, "data:")) ? isrc : "";
                u32 id = use[0] && w_nimg < WEB_MAXIMG ? w_nimg++ : WEB_MAXIMG;
                if (id < WEB_MAXIMG) { u32 k2 = 0; while (use[k2] && k2 < WEB_HREF - 1) { web_img_src[id][k2] = use[k2]; k2++; } web_img_src[id][k2] = 0; }
                g_open(tn, idv, clsv, stylev, href, 1);
                if (!gstk[gsp].s.hidden) {
                    if (id < WEB_MAXIMG && web_img_pix[id]) g_image(id);
                    else if (alt[0]) { u32 sv = gstk[gsp].s.color; gstk[gsp].s.color = 0x888888; g_word("[", 1); g_text(alt, g_slen(alt)); g_word("]", 1); gstk[gsp].s.color = sv; }
                }
                g_close_top();
                continue;
            }
            g_open(tn, idv, clsv, stylev, href, 0);
            if (g_is_void(tn)) g_close_top();
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
        if (tl >= sizeof tbuf - 2) { if (!c_ws((char)c)) { /* split long text at a safe point */ } FLUSH_TEXT(); }
        tbuf[tl++] = (char)c;
        i++;
    }
    FLUSH_TEXT();
    while (gsp > 0) g_close_top();
    g_line_end();
    gpage_h = g_y + 24;
    #undef FLUSH_TEXT
}

// Collect the page's style sheets: <style> blocks and up to 4 <link rel=stylesheet>. Linked files are downloaded (base = page address).
static int web_download(const char *url_in, u8 *buf, u32 max);
static void g_collect_css(const u8 *h, u32 n, const char *base) {
    ncrules = 0; css_order = 0;
    crules_max = 3000; crules = img_alloc(crules_max * sizeof(struct crule));
    if (!crules) { crules_max = 0; return; }
    u8 *cssbuf = img_alloc(512u * 1024); u32 links = 0;
    for (u32 i = 0; i + 6 < n; i++) {
        if (h[i] != '<') continue;
        if ((h[i + 1] | 0x20) == 's' && (h[i + 2] | 0x20) == 't' && (h[i + 3] | 0x20) == 'y' && (h[i + 4] | 0x20) == 'l' && (h[i + 5] | 0x20) == 'e') {
            u32 a = i; while (a < n && h[a] != '>') a++; a++;
            u32 b = a; while (b + 7 < n && !(h[b] == '<' && h[b + 1] == '/' && (h[b + 2] | 0x20) == 's' && (h[b + 3] | 0x20) == 't')) b++;
            if (b > a) css_parse((const char *)h + a, b - a);
            i = b;
        } else if ((h[i + 1] | 0x20) == 'l' && (h[i + 2] | 0x20) == 'i' && (h[i + 3] | 0x20) == 'n' && (h[i + 4] | 0x20) == 'k' && links < 4 && cssbuf) {
            u32 e = i; while (e < n && h[e] != '>') e++;
            int is_css = 0; char href[300]; href[0] = 0;
            for (u32 k = i; k + 10 < e; k++) if (ci_prefix(h + k, "stylesheet")) is_css = 1;
            for (u32 k = i; k + 5 < e; k++) if (ci_prefix(h + k, "href=")) {
                u32 q = k + 5; u8 qc = 0; if (h[q] == '"' || h[q] == '\'') qc = h[q++];
                u32 v = 0; while (q < e && (qc ? h[q] != qc : !c_ws((char)h[q])) && v + 1 < sizeof href) { if (h[q] == '&' && ci_prefix(h + q, "&amp;")) { href[v++] = '&'; q += 5; continue; } href[v++] = (char)h[q++]; }
                href[v] = 0; break;
            }
            if (is_css && href[0]) {
                char url[300];
                if (url_resolve(base, href, url, sizeof url)) {
                    web_status("loading style sheet...");
                    int got = web_download(url, cssbuf, 512u * 1024);
                    if (got > 0) { css_parse((const char *)cssbuf, (u32)got); links++; }
                }
            }
            i = e;
        }
    }
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
// Paint the page, scrolled to `top` pixels, into screen rows y0..y1. sel = the link picked with Left/Right (highlighted).
static void g_draw(int top, int y0, int y1, u32 sel) {
    g_fill(0, y0, (int)con_fb.w, y1 - y0, gpage_bg, y0, y1);
    for (u32 i = 0; i < gin; i++) {
        struct gitem *it = &gi[i];
        int sy = it->y - top + y0;
        if (sy >= y1 || sy + it->h <= y0) continue;
        if (it->type == GI_RECT) { g_fill(it->x, sy, it->w, it->h, it->color, y0, y1); continue; }
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
