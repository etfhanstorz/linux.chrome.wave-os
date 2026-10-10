// wave-os web browser (v1.6): fetch a page over HTTP, turn the HTML into text lines, show them with scrolling, numbered links and history.
// Not yet: HTTPS (needs TLS), images, styling, forms, JavaScript. It shows pages as readable text, like a very small Lynx.
//
// Keys: Up/Down scroll, Space/B page down/up, T top, E end, Left/Right (or Tab) pick a link, Enter open it, a number + Enter opens link N,
//       G type an address, F1 or Backspace back, F3 or R reload, Q or Esc quit.

#define WEB_MAXLINES 12000
#define WEB_MAXLINKS 250
#define WEB_HREF 140
#define WEB_RAW_MAX (512u * 1024)                              // the raw page, in update_buf (the browser and `up` never run together)
#define WEB_TEXT_MAX (384u * 1024)                             // the page as text: characters, a style per character, a link number per character
static u8 *web_raw;                                            // set up by web_init()
static char *wt; static u8 *wsty, *wlk;
static u32 wn, wnl, wls[WEB_MAXLINES + 2];                     // text length, number of lines, start of each line
static u32 web_width;                                          // characters per line
static u32 wsty_cur, wlink_cur, w_nlinks, w_inpre, w_listdepth, w_space, w_skipdepth;
static char w_skiptag[12];
static char web_title[100];
static char web_links[WEB_MAXLINKS + 1][WEB_HREF];
// pictures (v1.8): the page is laid out once with "[img]" placeholders, the pictures are fetched and decoded, then it is laid out again
// with room for each picture (whole text lines), and web_draw() paints the pictures into that room.
#define WEB_MAXIMG 16
static char web_img_src[WEB_MAXIMG][WEB_HREF], web_img_alt[WEB_MAXIMG][48];
static u32 *web_img_pix[WEB_MAXIMG]; static u32 web_img_w[WEB_MAXIMG], web_img_h[WEB_MAXIMG], web_img_line[WEB_MAXIMG], web_img_rows[WEB_MAXIMG];
static u32 w_nimg, web_img_shown, web_page_len;

// styles
#define S_NORM 0
#define S_HEAD 1
#define S_LINK 2
#define S_DIM 3
#define S_BOLD 4
#define S_ERR 5

// ---- building the text ----
static void web_newline(void) {
    if (wnl + 2 >= WEB_MAXLINES) return;
    if (wn == wls[wnl] && wnl > 0 && wls[wnl] == wls[wnl - 1]) return;          // never more than one blank line in a row
    wls[++wnl] = wn;
}
static void web_break(void) { if (wn != wls[wnl]) web_newline(); }
static void web_para(void) { web_break(); if (wnl > 0 && wls[wnl - 1] < wls[wnl]) web_newline(); }
static void web_emit(u8 c) {
    if (wn >= WEB_TEXT_MAX - 2) return;
    if (wn - wls[wnl] >= web_width) {                                           // line full: break at the last space if there is one, else here
        u32 k = wn; while (k > wls[wnl] + web_width / 3 && wt[k - 1] != ' ') k--;
        if (k > wls[wnl] + web_width / 3 && wt[k - 1] == ' ') { if (wnl + 2 < WEB_MAXLINES) wls[++wnl] = k; }
        else web_newline();
    }
    wt[wn] = (char)c; wsty[wn] = (u8)wsty_cur; wlk[wn] = (u8)wlink_cur; wn++;
}
static void web_puts(const char *s) { while (*s) web_emit((u8)*s++); }
static void web_text_char(u8 c) {                                               // one character of page text: collapse white space unless inside <pre>
    if (w_inpre) { if (c == '\n') web_newline(); else if (c == '\t') { for (u32 i = 0; i < 4; i++) web_emit(' '); } else if (c != '\r') web_emit(c >= 32 && c < 127 ? c : '?'); return; }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { w_space = 1; return; }
    if (c < 32 || c > 126) c = '?';
    if (w_space && wn != wls[wnl]) web_emit(' ');
    w_space = 0;
    web_emit(c);
}
static void web_text_str(const char *s) { while (*s) web_text_char((u8)*s++); }

static const char *web_cp(u32 cp) {                                            // a few common non-ASCII characters, as plain text
    switch (cp) {
    case 0xa0: return " "; case 0xa9: return "(c)"; case 0xae: return "(R)"; case 0xb7: case 0x2022: return "*";
    case 0x2018: case 0x2019: case 0x201a: return "'"; case 0x201c: case 0x201d: case 0x201e: return "\""; case 0x2013: return "-"; case 0x2014: return "--";
    case 0x2026: return "..."; case 0xab: return "<<"; case 0xbb: return ">>"; case 0x2192: return "->"; case 0x2190: return "<-"; case 0x20ac: return "EUR";
    case 0xe9: case 0xe8: case 0xea: return "e"; case 0xe1: case 0xe0: case 0xe2: case 0xe4: return "a"; case 0xf6: case 0xf3: return "o"; case 0xfc: case 0xfa: return "u"; case 0xed: return "i"; case 0xf1: return "n"; case 0xe7: return "c";
    }
    return "?";
}
// An entity at s (just after the '&'): returns the characters it stands for in out, and how many input characters were used (0 = not an entity).
static u32 web_entity(const u8 *s, u32 left, char *out) {
    static const struct { const char *n; const char *v; } tab[] = {
        {"amp;", "&"}, {"lt;", "<"}, {"gt;", ">"}, {"quot;", "\""}, {"apos;", "'"}, {"nbsp;", " "}, {"copy;", "(c)"}, {"reg;", "(R)"},
        {"mdash;", "--"}, {"ndash;", "-"}, {"hellip;", "..."}, {"lsquo;", "'"}, {"rsquo;", "'"}, {"ldquo;", "\""}, {"rdquo;", "\""},
        {"bull;", "*"}, {"middot;", "*"}, {"raquo;", ">>"}, {"laquo;", "<<"}, {"rarr;", "->"}, {"larr;", "<-"}, {"euro;", "EUR"}, {"trade;", "(TM)"},
    };
    if (left < 2) return 0;
    if (s[0] == '#') {
        u32 i = 1, v = 0, hex = 0;
        if (i < left && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        u32 d0 = i;
        while (i < left && ((s[i] >= '0' && s[i] <= '9') || (hex && ((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'f')))) { v = v * (hex ? 16 : 10) + (s[i] <= '9' ? s[i] - '0' : (s[i] | 0x20) - 'a' + 10); i++; }
        if (i == d0) return 0;
        if (i < left && s[i] == ';') i++;
        const char *r = v >= 32 && v < 127 ? 0 : web_cp(v);
        if (!r) { out[0] = (char)v; out[1] = 0; } else { u32 k = 0; while (r[k] && k < 6) { out[k] = r[k]; k++; } out[k] = 0; }
        return i;
    }
    for (u32 t = 0; t < sizeof tab / sizeof tab[0]; t++) {
        u32 k = 0; while (tab[t].n[k] && k < left && s[k] == (u8)tab[t].n[k]) k++;
        if (!tab[t].n[k]) { u32 j = 0; while (tab[t].v[j]) { out[j] = tab[t].v[j]; j++; } out[j] = 0; return k; }
    }
    return 0;
}

static void web_begin(void) {
    wn = 0; wnl = 0; wls[0] = 0; w_nlinks = 0; w_nimg = 0; for (u32 i = 0; i < WEB_MAXIMG; i++) web_img_rows[i] = 0; wsty_cur = S_NORM; wlink_cur = 0; w_inpre = 0; w_listdepth = 0; w_space = 0; w_skipdepth = 0; web_title[0] = 0;
    web_width = con_cols > 4 ? con_cols - 2 : 78;
}
static void web_finish(void) { if (wn != wls[wnl]) { wls[++wnl] = wn; } else if (wnl == 0 && wn == 0) { wls[++wnl] = 0; } }   // wnl = number of finished lines; line i = [wls[i], wls[i+1])
static void web_line(u32 sty, const char *s) { wsty_cur = sty; web_break(); web_puts(s); web_newline(); wsty_cur = S_NORM; }

// ---- HTML -> text ----
static int tag_is(const char *name, const char *want) { u32 i = 0; while (want[i] && name[i] == want[i]) i++; return !want[i] && !name[i]; }
static void web_href_store(const char *v) {
    if (w_nlinks >= WEB_MAXLINKS) return;
    u32 k = 0; while (v[k] && k < WEB_HREF - 1) { web_links[w_nlinks + 1][k] = v[k]; k++; } web_links[w_nlinks + 1][k] = 0;
}
static void web_html(const u8 *h, u32 n) {
    u32 i = 0, in_title = 0;
    char tn[12], href[WEB_HREF], alt[60], isrc[WEB_HREF], dsrc[WEB_HREF];
    while (i < n) {
        u8 c = h[i];
        if (w_skipdepth) {                                                       // inside <script>/<style>/...: look for the closing tag only
            if (c == '<' && i + 2 < n && h[i + 1] == '/') {
                u32 k = 0; while (w_skiptag[k] && i + 2 + k < n && (h[i + 2 + k] | 0x20) == (u8)w_skiptag[k]) k++;
                if (!w_skiptag[k]) { w_skipdepth = 0; while (i < n && h[i] != '>') i++; }
            }
            i++; continue;
        }
        if (c == '<') {
            if (i + 3 < n && h[i + 1] == '!' && h[i + 2] == '-' && h[i + 3] == '-') {                       // comment
                i += 4; while (i + 2 < n && !(h[i] == '-' && h[i + 1] == '-' && h[i + 2] == '>')) i++;
                i += 3; continue;
            }
            if (i + 1 < n && (h[i + 1] == '!' || h[i + 1] == '?')) { while (i < n && h[i] != '>') i++; i++; continue; }
            u32 j = i + 1, closing = 0;
            if (j < n && h[j] == '/') { closing = 1; j++; }
            u32 k = 0; while (j < n && ((h[j] | 0x20) >= 'a' && (h[j] | 0x20) <= 'z' || (h[j] >= '0' && h[j] <= '9')) && k < 11) tn[k++] = (char)(h[j++] | 0x20);
            tn[k] = 0;
            if (!k) { web_text_char('<'); i++; continue; }                                                  // a lone '<' in the text
            href[0] = 0; alt[0] = 0; isrc[0] = 0; dsrc[0] = 0;
            while (j < n && h[j] != '>') {                                                                   // attributes
                while (j < n && (h[j] == ' ' || h[j] == '\n' || h[j] == '\t' || h[j] == '\r' || h[j] == '/')) j++;
                if (j >= n || h[j] == '>') break;
                char an[16]; u32 a = 0;
                while (j < n && h[j] != '=' && h[j] != ' ' && h[j] != '>' && h[j] != '/' && h[j] != '\n' && h[j] != '\t') { if (a < 15) an[a++] = (char)(h[j] | 0x20); j++; }
                an[a] = 0;
                char *dst = 0; u32 dmax = 0;
                if (an[0] == 'h' && an[1] == 'r' && an[2] == 'e' && an[3] == 'f' && !an[4]) { dst = href; dmax = sizeof href; }
                else if (an[0] == 'a' && an[1] == 'l' && an[2] == 't' && !an[3]) { dst = alt; dmax = sizeof alt; }
                else if (tag_is(an, "src")) { dst = isrc; dmax = sizeof isrc; }
                else if (tag_is(an, "data-src") || tag_is(an, "data-lazy-src") || tag_is(an, "data-original")) { dst = dsrc; dmax = sizeof dsrc; }
                if (j < n && h[j] == '=') {
                    j++; u8 q = 0; if (j < n && (h[j] == '"' || h[j] == '\'')) q = h[j++];
                    u32 v = 0;
                    while (j < n && (q ? h[j] != q : (h[j] != ' ' && h[j] != '>' && h[j] != '\n' && h[j] != '\t'))) {
                        if (dst && v + 1 < dmax) {
                            char ent[8]; u32 used = h[j] == '&' ? web_entity(h + j + 1, n - j - 1, ent) : 0;
                            if (used) { for (u32 e = 0; ent[e] && v + 1 < dmax; e++) dst[v++] = ent[e]; j += used + 1; continue; }
                            dst[v++] = (char)h[j];
                        }
                        j++;
                    }
                    if (dst) dst[v] = 0;
                    if (q && j < n) j++;
                }
            }
            i = j < n ? j + 1 : n;
            // ---- what the tag does ----
            if (tag_is(tn, "script") || tag_is(tn, "style") || tag_is(tn, "noscript") || tag_is(tn, "svg") || tag_is(tn, "template") || tag_is(tn, "iframe")) {
                if (!closing) { w_skipdepth = 1; u32 q2 = 0; while (tn[q2] && q2 < 11) { w_skiptag[q2] = tn[q2]; q2++; } w_skiptag[q2] = 0; }
            } else if (tag_is(tn, "title")) { in_title = !closing; }
            else if (tag_is(tn, "br")) web_newline();
            else if (tag_is(tn, "hr")) { web_break(); wsty_cur = S_DIM; for (u32 q2 = 0; q2 < web_width; q2++) web_emit('-'); wsty_cur = S_NORM; web_newline(); }
            else if (tn[0] == 'h' && tn[1] >= '1' && tn[1] <= '6' && !tn[2]) { web_para(); wsty_cur = closing ? S_NORM : S_HEAD; w_space = 0; if (!closing && tn[1] <= '2') { /* big headings stand out through the colour only */ } }
            else if (tag_is(tn, "p") || tag_is(tn, "table") || tag_is(tn, "blockquote") || tag_is(tn, "form") || tag_is(tn, "dl") || tag_is(tn, "figure")) web_para();
            else if (tag_is(tn, "div") || tag_is(tn, "section") || tag_is(tn, "article") || tag_is(tn, "header") || tag_is(tn, "footer") || tag_is(tn, "nav") || tag_is(tn, "main") || tag_is(tn, "aside") || tag_is(tn, "tr") || tag_is(tn, "dt") || tag_is(tn, "dd") || tag_is(tn, "address") || tag_is(tn, "fieldset") || tag_is(tn, "caption")) web_break();
            else if (tag_is(tn, "ul") || tag_is(tn, "ol")) { if (closing) { if (w_listdepth) w_listdepth--; web_para(); } else { web_break(); w_listdepth++; } }
            else if (tag_is(tn, "li")) { if (!closing) { web_break(); for (u32 q2 = 1; q2 < w_listdepth; q2++) web_puts("  "); web_puts("* "); w_space = 0; } }
            else if (tag_is(tn, "td") || tag_is(tn, "th")) { if (!closing) { if (wn != wls[wnl]) web_puts("  "); w_space = 0; } }
            else if (tag_is(tn, "pre")) { web_para(); w_inpre = !closing; w_space = 0; }
            else if (tag_is(tn, "b") || tag_is(tn, "strong") || tag_is(tn, "em") || tag_is(tn, "i") || tag_is(tn, "mark")) { if (wsty_cur == S_NORM || wsty_cur == S_BOLD) wsty_cur = closing ? S_NORM : S_BOLD; }
            else if (tag_is(tn, "a")) {
                if (!closing) { if (href[0] && w_nlinks < WEB_MAXLINKS) { web_href_store(href); w_nlinks++; wlink_cur = w_nlinks; wsty_cur = S_LINK; } }
                else if (wlink_cur) { char nb[8]; u32 v = wlink_cur, q2 = 0, t2[4], tc = 0; do { t2[tc++] = v % 10; v /= 10; } while (v); nb[q2++] = '['; while (tc) nb[q2++] = (char)('0' + t2[--tc]); nb[q2++] = ']'; nb[q2] = 0; web_puts(nb); wlink_cur = 0; wsty_cur = S_NORM; }
            }
            else if (tag_is(tn, "img")) {
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
            }
            continue;
        }
        // ---- text ----
        if (c == '&') {
            char ent[8]; u32 used = web_entity(h + i + 1, n - i - 1, ent);
            if (used) { if (in_title) { u32 t = 0; while (web_title[t]) t++; for (u32 e = 0; ent[e] && t + 1 < sizeof web_title; e++) { web_title[t++] = ent[e]; web_title[t] = 0; } } else web_text_str(ent); i += used + 1; continue; }
        }
        if (c >= 0x80) {                                                                                     // UTF-8
            u32 len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1, cp = len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : len == 4 ? c & 7 : c;
            for (u32 q2 = 1; q2 < len && i + q2 < n; q2++) cp = cp << 6 | (h[i + q2] & 0x3f);
            if (!in_title) web_text_str(web_cp(cp));
            i += len; continue;
        }
        if (in_title) { u32 t = 0; while (web_title[t]) t++; if (t + 1 < sizeof web_title && c >= 32 && c < 127) { web_title[t] = (char)c; web_title[t + 1] = 0; } }
        else web_text_char(c);
        i++;
    }
    wsty_cur = S_NORM;
    web_finish();
}
static void web_plain(const u8 *h, u32 n) {                                                                  // text/plain: lines as they are
    w_inpre = 1;
    for (u32 i = 0; i < n; i++) web_text_char(h[i]);
    w_inpre = 0; web_finish();
}

// ---- addresses ----
static char web_url[300];                                    // the page being shown
static u32 web_secure;                                       // the page came over https (encrypted; the certificate is not checked yet)
struct url { char host[100]; u32 port, https; char path[200]; };
static int url_parse(const char *s, struct url *u) {
    u32 i = 0;
    u->https = 0; u->port = 80;
    if (ci_prefix((const u8 *)s, "http://")) i = 7;
    else if (ci_prefix((const u8 *)s, "https://")) { i = 8; u->https = 1; u->port = 443; }
    else return 0;
    u32 k = 0;
    while (s[i] && s[i] != '/' && s[i] != ':' && s[i] != '?' && s[i] != '#' && k < sizeof u->host - 1) u->host[k++] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]), i++;
    u->host[k] = 0;
    if (!k) return 0;
    if (s[i] == ':') { i++; u32 p = 0; while (s[i] >= '0' && s[i] <= '9') p = p * 10 + (s[i++] - '0'); if (p) u->port = p; }
    k = 0;
    if (!s[i] || s[i] == '#') u->path[k++] = '/';
    else if (s[i] == '?') { u->path[k++] = '/'; }
    while (s[i] && s[i] != '#' && k < sizeof u->path - 1) u->path[k++] = s[i++];
    u->path[k] = 0;
    return 1;
}
// Make `href` (as written in a page) into a full address, relative to base. Returns 0 if it is not something we can open (mailto:, javascript:, #fragment).
static int url_resolve(const char *base, const char *href, char *out, u32 max) {
    u32 k = 0;
    if (!href[0] || href[0] == '#') return 0;
    if (ci_prefix((const u8 *)href, "mailto:") || ci_prefix((const u8 *)href, "javascript:") || ci_prefix((const u8 *)href, "tel:") || ci_prefix((const u8 *)href, "data:")) return 0;
    if (ci_prefix((const u8 *)href, "http://") || ci_prefix((const u8 *)href, "https://")) { while (href[k] && k + 1 < max) { out[k] = href[k]; k++; } out[k] = 0; return 1; }
    struct url b; if (!url_parse(base, &b)) return 0;
    const char *scheme = b.https ? "https://" : "http://";
    for (u32 i = 0; scheme[i] && k + 1 < max; i++) out[k++] = scheme[i];
    if (href[0] == '/' && href[1] == '/') { href += 2; while (*href && k + 1 < max) out[k++] = *href++; out[k] = 0; return 1; }       // //host/path: same scheme
    for (u32 i = 0; b.host[i] && k + 1 < max; i++) out[k++] = b.host[i];
    if ((b.https ? b.port != 443 : b.port != 80)) { out[k++] = ':'; char t[6]; u32 v = b.port, tc = 0, d[5]; do { d[tc++] = v % 10; v /= 10; } while (v); while (tc && k + 1 < max) out[k++] = (char)('0' + d[--tc]); (void)t; }
    char path[300]; u32 pn = 0;
    if (href[0] == '/') { while (*href && pn + 1 < sizeof path) path[pn++] = *href++; }
    else {                                                                                       // relative to the directory of the base path
        u32 last = 0; for (u32 i = 0; b.path[i] && b.path[i] != '?'; i++) if (b.path[i] == '/') last = i;
        for (u32 i = 0; i <= last && pn + 1 < sizeof path; i++) path[pn++] = b.path[i];
        while (*href && pn + 1 < sizeof path) path[pn++] = *href++;
    }
    path[pn] = 0;
    // remove ./ and ../ segments
    char norm[300]; u32 nn = 0, i = 0;
    while (path[i] && path[i] != '?') {
        if (path[i] == '/' && path[i + 1] == '.' && path[i + 2] == '/') { i += 2; continue; }
        if (path[i] == '/' && path[i + 1] == '.' && path[i + 2] == '.' && (path[i + 3] == '/' || !path[i + 3])) { while (nn > 0 && norm[nn - 1] != '/') nn--; if (nn > 0) nn--; i += 3; if (!path[i]) norm[nn++] = '/'; continue; }
        norm[nn++] = path[i++];
    }
    while (path[i]) norm[nn++] = path[i++];
    norm[nn] = 0;
    for (u32 q = 0; norm[q] && k + 1 < max; q++) { if (norm[q] == '#') break; out[k++] = norm[q]; }
    out[k] = 0;
    return 1;
}

#include "gfx.h"
static u32 web_gfx = 1;                                      // 1 = graphical view (fonts, colours, pictures), 0 = text view (key V switches)
static u32 web_page_gfx;                                     // the page on screen was laid out graphically

// ---- loading ----
static void web_error_page(const char *title, const char *l1, const char *l2) {
    web_begin();
    u32 t = 0; while (title[t] && t + 1 < sizeof web_title) { web_title[t] = title[t]; t++; } web_title[t] = 0;
    web_line(S_ERR, title); web_newline(); web_line(S_NORM, l1); if (l2) web_line(S_DIM, l2);
    web_finish();
}
static void web_draw(void);
static u32 web_top, web_sel;
static void web_status(const char *msg);
// Fetch `url` (following up to 5 redirects) and build the text. Returns 1 if a page was shown, 0 if an error page was shown.
static int web_fetch(const char *url_in) {
    char url[300]; u32 k = 0; while (url_in[k] && k + 1 < sizeof url) { url[k] = url_in[k]; k++; } url[k] = 0;
    web_top = 0; web_sel = 0;
    img_bump = 0; web_img_shown = 0; for (u32 i = 0; i < WEB_MAXIMG; i++) web_img_pix[i] = 0;   // a new page: forget the old pictures
    web_page_gfx = 0; gm = 0; ncrules = 0; crule_next = 0;            // the old page's layout data lived in picture memory
    for (int hops = 0; hops < 6; hops++) {
        struct url u;
        if (!url_parse(url, &u)) { errs("NET", 31, 1, "the address is not valid (it must start with http://)"); web_error_page("Bad address", "The address must start with http:// (for example http://example.com).", url); for (u32 i = 0; i < sizeof web_url - 1 && url[i]; i++) web_url[i] = url[i]; return 0; }
        for (u32 i = 0; i < sizeof web_url; i++) { web_url[i] = url[i]; if (!url[i]) break; } web_url[sizeof web_url - 1] = 0;
        web_status("looking up the address...");
        u32 ip = 0; int r = dns_lookup(u.host, &ip);
        if (r) {
            errs("NET", 30, r == -2 ? 2 : r == -1 ? 1 : 3, r == -2 ? "that name does not exist" : r == -1 ? "no DNS server: connect first (k)" : "the DNS server did not answer");
            web_error_page(r == -2 ? "No such website" : r == -1 ? "Not connected" : "Cannot look up the name", r == -2 ? "That name does not exist." : r == -1 ? "Connect to Wi-Fi first (press q, then type k)." : "The DNS server did not answer.", u.host);
            return 0;
        }
        web_status(u.https ? "connecting securely..." : "loading...");
        http_any = 1; http_status = 0;
        int n = u.https ? https_get(ip, u.port, u.host, u.path, web_raw, WEB_RAW_MAX, 25000) : http_get(ip, u.port, u.host, u.path, web_raw, WEB_RAW_MAX, 25000);
        http_any = 0;
        web_secure = u.https;
        if (n <= -20) {
            static const char *why[] = {"", "The connection broke during setup.", "The website does not offer an encryption method wave-os knows yet (it needs X25519 + AES-GCM).", "The website did not answer the encryption setup in time.", "The website's answer failed its check: the connection may have been tampered with.", "Encrypted data arrived damaged."};
            u32 w = (u32)(-20 - n); if (w > 5) w = 1;
            errs("NET", 34, w, "the secure (https) connection could not be set up");
            web_error_page("Secure connection failed", why[w], u.host);
            if (tls.alert >= 0) { char a[40] = "the website said: alert "; u32 q = 24, v = (u32)tls.alert; char d[4]; u32 dc = 0; do { d[dc++] = (char)('0' + v % 10); v /= 10; } while (v); while (dc) a[q++] = d[--dc]; a[q] = 0; web_line(S_DIM, a); web_finish(); }
            return 0;
        }
        if (n < 0) {
            errs("NET", 32, (u32)(-n > 9 ? 9 : -n), "could not load the page");
            web_error_page("Could not load the page", n == -2 ? "Could not connect to the server." : n == -3 ? "The server took too long to answer." : "The server's answer was not understood.", u.host);
            return 0;
        }
        if ((http_status == 301 || http_status == 302 || http_status == 303 || http_status == 307 || http_status == 308) && http_location[0]) {
            char next[300];
            if (!url_resolve(url, http_location, next, sizeof next)) break;
            k = 0; while (next[k] && k + 1 < sizeof url) { url[k] = next[k]; k++; } url[k] = 0;
            continue;
        }
        web_begin();
        u32 is_html = 1;
        if (http_ctype[0] && !(ci_prefix((const u8 *)http_ctype, "text/html") || ci_prefix((const u8 *)http_ctype, "application/xhtml"))) is_html = 0;
        if (!is_html && !ci_prefix((const u8 *)http_ctype, "text/")) {
            web_line(S_HEAD, "Not a web page"); web_newline(); web_line(S_NORM, "wave-os can only show text and HTML pages."); web_line(S_DIM, http_ctype);
            web_finish();
        } else if (is_html) {
            web_page_len = (u32)n;
            if (web_gfx) { g_collect_css(web_raw, (u32)n, url); web_status("laying out the page..."); g_layout(web_raw, (u32)n, (int)con_fb.w); web_page_gfx = 1; }
            else web_html(web_raw, (u32)n);
        }
        else web_plain(web_raw, (u32)n);
        if (http_status >= 400) { u32 t = 0; while (web_title[t]) t++; if (!t) { const char *e = "Error"; for (u32 q = 0; e[q]; q++) web_title[q] = e[q]; web_title[5] = 0; } }
        return 1;
    }
    errs("NET", 33, 1, "too many redirects");
    web_error_page("Too many redirects", "The page kept redirecting.", url);
    return 0;
}

// ---- pictures ----
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
        if (n <= 0) { u32 kk = con_on; con_on = 0; puts("picture failed to load: "); puts(url); putc('\n'); con_on = kk; continue; }
        u32 w, h; u32 *pix = img_load(buf, (u32)n, maxw, maxh, &w, &h);
        if (!pix) { u32 kk = con_on; con_on = 0; puts("picture not shown ("); puts(img_why); puts("): "); puts(url); putc('\n'); con_on = kk; continue; }
        web_img_pix[i] = pix; web_img_w[i] = w; web_img_h[i] = h; web_img_shown++;
    }
    if (web_img_shown) {                                                      // lay the page out again, now with room for the pictures
        if (web_page_gfx) { web_status("laying out the page..."); g_layout(web_raw, web_page_len, (int)con_fb.w); }
        else { web_begin(); web_html(web_raw, web_page_len); }
    }
}

// ---- drawing ----
static void web_cell(u32 x, u32 y, char c, u32 fg, u32 bg) { u32 sf = con_fg, sb = con_bg; con_fg = fg; con_bg = bg; glyph(x, y, c); con_fg = sf; con_bg = sb; }
static void web_row(u32 y, const char *s, u32 fg, u32 bg, u32 start) {
    u32 x = start;
    for (u32 i = 0; s[i] && x < con_cols; i++, x++) web_cell(x, y, s[i], fg, bg);
    for (; x < con_cols; x++) web_cell(x, y, ' ', fg, bg);
}
static u32 web_style_color(u32 s) { return s == S_HEAD ? 0x40E0FF : s == S_LINK ? 0x7DB7FF : s == S_DIM ? 0x808090 : s == S_BOLD ? 0xFFFFFF : s == S_ERR ? 0xFF8080 : C_TEXT; }
static char web_msg[60];
static void web_hints(void) {
    char h[140]; u32 k = 0;
    const char *a = " Up/Down/Space scroll  Left/Right link  Enter open  G address  F1 back  R reload  V view  Q quit";
    while (a[k] && k + 1 < sizeof h) { h[k] = a[k]; k++; } h[k] = 0;
    web_row(con_rows - 1, h, 0xB0B8C8, 0x1A2433, 0);
}
static void web_status(const char *msg) {
    u32 k = 0; while (msg[k] && k + 1 < sizeof web_msg) { web_msg[k] = msg[k]; k++; } web_msg[k] = 0;
    char b[200]; u32 n = 0; b[n++] = ' ';
    for (u32 i = 0; web_url[i] && n < sizeof b - 60; i++) b[n++] = web_url[i];
    if (web_secure) { const char *s = "  (encrypted, not verified)"; for (u32 i = 0; s[i] && n < sizeof b - 30; i++) b[n++] = s[i]; }
    if (web_msg[0]) { b[n++] = ' '; b[n++] = ' '; b[n++] = '-'; b[n++] = ' '; for (u32 i = 0; web_msg[i] && n < sizeof b - 1; i++) b[n++] = web_msg[i]; }
    b[n] = 0;
    web_row(1, b, 0xE0E6F0, 0x24364F, 0);
}
static void web_draw(void) {
    char tb[160]; u32 n = 0; const char *t0 = " wave-web  ";
    while (t0[n]) { tb[n] = t0[n]; n++; }
    for (u32 i = 0; web_title[i] && n < sizeof tb - 1; i++) tb[n++] = web_title[i];
    tb[n] = 0;
    web_row(0, tb, 0xFFFFFF, 0x1F3A5F, 0);
    web_status(web_msg);
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
    }
    for (u32 r = 0; r < rows; r++) {
        u32 li = web_top + r, y = 2 + r, x = 1;
        web_cell(0, y, ' ', C_TEXT, con_bg);
        if (li < wnl) {
            u32 a = wls[li], b = wls[li + 1];
            while (b > a && wt[b - 1] == ' ') b--;
            for (u32 i = a; i < b && x < con_cols - 1; i++, x++) {
                u32 sel = wlk[i] && wlk[i] == web_sel;
                web_cell(x, y, wt[i], sel ? 0xFFFFFF : web_style_color(wsty[i]), sel ? 0x2F5A99 : con_bg);
            }
        }
        for (; x < con_cols; x++) web_cell(x, y, ' ', C_TEXT, con_bg);
    }
    // pictures: paint the visible part of each one into the lines kept free for it
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
    char pb[40]; u32 pn = 0; u32 pct = wnl > rows ? (web_top * 100) / (wnl - rows) : 100; if (pct > 100) pct = 100;
    { u32 v = pct, d[3], dc = 0; do { d[dc++] = v % 10; v /= 10; } while (v); while (dc) pb[pn++] = (char)('0' + d[--dc]); pb[pn++] = '%'; pb[pn++] = ' '; pb[pn++] = ' ';
      v = w_nlinks; dc = 0; do { d[dc++] = v % 10; v /= 10; } while (v && dc < 3); while (dc) pb[pn++] = (char)('0' + d[--dc]); const char *l = " links"; for (u32 q = 0; l[q]; q++) pb[pn++] = l[q]; }
    pb[pn] = 0;
    web_hints();
    bar_draw(1);
    for (u32 i = 0; pb[i] && i < 30; i++) web_cell(con_cols - 1 - pn - 1 + i, con_rows - 1, pb[i], 0xE0E6F0, 0x1A2433);
}

// ---- the program ----
static char web_hist[16][300]; static u32 web_hist_top[16], web_hist_n;
static void web_push(void) {
    if (!web_url[0]) return;
    if (web_hist_n == 16) { for (u32 i = 1; i < 16; i++) { for (u32 k = 0; k < 300; k++) web_hist[i - 1][k] = web_hist[i][k]; web_hist_top[i - 1] = web_hist_top[i]; } web_hist_n = 15; }
    u32 k = 0; while (web_url[k] && k < 299) { web_hist[web_hist_n][k] = web_url[k]; k++; } web_hist[web_hist_n][k] = 0;
    web_hist_top[web_hist_n++] = web_top;
}
// Line editor on the address row: returns the length typed, or -1 if cancelled.
static int web_edit(const char *prompt, char *buf, u32 max) {
    u32 n = 0; buf[0] = 0;
    for (;;) {
        char b[330]; u32 k = 0; b[k++] = ' '; for (u32 i = 0; prompt[i]; i++) b[k++] = prompt[i];
        for (u32 i = 0; i < n && k < sizeof b - 3; i++) b[k++] = buf[i];
        b[k++] = '_'; b[k] = 0;
        web_row(1, b, 0xFFFFFF, 0x3A5A2A, 0);
        int c; while ((c = kb_getc()) < 0) wdt_kick();
        if (c == '\n' || c == '\r') return (int)n;
        if (c == 27) return -1;
        if (c == '\b' || c == 0x7f) { if (n) buf[--n] = 0; continue; }
        if (c >= 32 && c < 127 && n + 1 < max) { buf[n++] = (char)c; buf[n] = 0; }
    }
}
static void web_goto(const char *url, int remember) {
    if (remember) web_push();
    int ok = web_fetch(url);
    web_msg[0] = 0; web_draw();
    if (ok && w_nimg) { web_load_images(web_url); web_msg[0] = 0; web_draw(); }
    log_ship();
}
static void web_scroll_to_link(void) {
    if (web_page_gfx) {
        int y = g_link_y(web_sel); u32 vh = (con_rows - 3) * CH;
        if (y >= 0) { if ((u32)y < web_top + CH) web_top = (u32)(y > (int)CH ? y - (int)CH : 0); else if ((u32)y > web_top + vh - 2 * CH) web_top = (u32)y - vh / 2; }
        return;
    }
    for (u32 li = 0; li < wnl; li++) for (u32 i = wls[li]; i < wls[li + 1]; i++) if (wlk[i] == web_sel) {
        u32 rows = con_rows - 3;
        if (li < web_top) web_top = li; else if (li >= web_top + rows) web_top = li + 1 - rows;
        return;
    }
}
static int web_run(const char *start) {
    web_raw = update_buf; wt = (char *)(update_buf + WEB_RAW_MAX); wsty = update_buf + WEB_RAW_MAX + WEB_TEXT_MAX; wlk = wsty + WEB_TEXT_MAX;
    gtx = wt; gtmax = WEB_TEXT_MAX;                                           // the graphical view shares the text view's memory (only one is in use)
    gi = (struct gitem *)(void *)wsty; gimax = (2 * WEB_TEXT_MAX) / sizeof(struct gitem);
    u32 keep_on = con_on; con_on = 0;                                         // while the browser runs, print() only logs (nothing is drawn over the page)
    con_clear();
    web_hist_n = 0; web_url[0] = 0; web_msg[0] = 0; web_title[0] = 0; wnl = 0;
    char url[300]; u32 k = 0; while (start[k] && k + 1 < sizeof url) { url[k] = start[k]; k++; } url[k] = 0;
    web_goto(url, 0);
    char numbuf[6]; u32 numn = 0;
    for (;;) {
        int c = kb_getc();
        if (c < 0) { wdt_kick(); wifi_service(); bar_tick(1); continue; }
        u32 rows = con_rows - 3, maxtop = wnl > rows ? wnl - rows : 0, step = 1, pagestep = rows - 1;
        if (web_page_gfx) { u32 vh = (con_rows - 3) * CH; maxtop = (u32)gpage_h > vh ? (u32)gpage_h - vh : 0; step = 3 * CH / 2; pagestep = vh - 2 * CH; }
        int redraw = 1;
        if (c >= '0' && c <= '9' && numn < 4) { numbuf[numn++] = (char)c; numbuf[numn] = 0; char m[20] = "link number: "; u32 q = 13; for (u32 i = 0; i < numn; i++) m[q++] = numbuf[i]; m[q] = 0; web_status(m); continue; }
        if (c == '\n' && numn) { u32 v = 0; for (u32 i = 0; i < numn; i++) v = v * 10 + (u32)(numbuf[i] - '0'); numn = 0; if (v >= 1 && v <= w_nlinks) web_sel = v; else { web_status("no such link"); continue; } c = '\n'; }
        else if (numn && c != '\n') { numn = 0; web_msg[0] = 0; }
        if (c == 'q' || c == 27) break;
        else if (c == K_DOWN) { web_top = web_top + step > maxtop ? maxtop : web_top + step; }
        else if (c == K_UP) { web_top = web_top > step ? web_top - step : 0; }
        else if (c == ' ') { web_top = web_top + pagestep > maxtop ? maxtop : web_top + pagestep; }
        else if (c == 'b') { web_top = web_top > pagestep ? web_top - pagestep : 0; }
        else if (c == 'v') {                                                  // switch between the graphical and the text view (same page, no download)
            web_gfx = !web_gfx; web_top = 0; web_sel = 0;
            if (web_page_len && (web_page_gfx || wnl)) {
                if (web_gfx) { web_status("laying out the page..."); g_layout(web_raw, web_page_len, (int)con_fb.w); web_page_gfx = 1; }
                else { web_page_gfx = 0; web_begin(); web_html(web_raw, web_page_len); }
            }
        }
        else if (c == 't') web_top = 0;
        else if (c == 'e') web_top = maxtop;
        else if (c == K_RIGHT || c == '\t') { if (w_nlinks) { web_sel = web_sel >= w_nlinks ? 1 : web_sel + 1; web_scroll_to_link(); } }
        else if (c == K_LEFT) { if (w_nlinks) { web_sel = web_sel <= 1 ? w_nlinks : web_sel - 1; web_scroll_to_link(); } }
        else if (c == '\n') {
            if (web_sel >= 1 && web_sel <= w_nlinks) {
                char next[300];
                if (url_resolve(web_url, web_links[web_sel], next, sizeof next)) { web_goto(next, 1); continue; }
                web_status("that link cannot be opened here"); continue;
            }
            redraw = 0;
        }
        else if (c == 'g') {
            char typed[300];
            int n = web_edit("address: ", typed, sizeof typed);
            if (n > 0) {
                char full[310]; u32 q = 0;
                if (!ci_prefix((const u8 *)typed, "http://") && !ci_prefix((const u8 *)typed, "https://")) { const char *p = "http://"; while (*p) full[q++] = *p++; }
                for (u32 i = 0; typed[i] && q + 1 < sizeof full; i++) full[q++] = typed[i];
                full[q] = 0;
                web_goto(full, 1); continue;
            }
        }
        else if (c == K_F1 || c == '\b') {
            if (web_hist_n) { web_hist_n--; char prev[300]; for (u32 i = 0; i < 300; i++) prev[i] = web_hist[web_hist_n][i]; u32 t = web_hist_top[web_hist_n]; web_goto(prev, 0); web_top = t > (wnl > rows ? wnl - rows : 0) ? (wnl > rows ? wnl - rows : 0) : t; web_draw(); continue; }
            else web_status("nothing to go back to");
        }
        else if (c == K_F1 + 2 || c == 'r') { char cur[300]; for (u32 i = 0; i < 300; i++) cur[i] = web_url[i]; web_goto(cur, 0); continue; }
        else redraw = 0;
        if (redraw) web_draw();
    }
    con_on = keep_on;
    con_clear();
    return 1;
}
