// Pictures (v1.8): decoders for PNG (with a DEFLATE decompressor), GIF (first frame) and baseline JPEG, plus smooth down-scaling.
// Everything decodes to 0x00RRGGBB pixels. Transparent parts are laid on white, like on a normal web page.
// Not yet: progressive JPEG, interlaced PNG, WebP, SVG (they show as "[img: ...]" text instead).

extern u8 __img_heap[] __attribute__((visibility("hidden")));               // 32 MB from link.ld (not part of the file, not cleared at boot)
#define IMG_HEAP (32u << 20)
static u32 img_bump;                                                         // simple allocator: reset for each page
static void *img_alloc(u32 n) { n = (n + 15) & ~15u; if (img_bump + n > IMG_HEAP) return 0; void *p = __img_heap + img_bump; img_bump += n; return p; }
static const char *img_why;                                                  // why the last decode failed

// ---------------- DEFLATE (RFC 1951), after zlib's "puff" ----------------
struct inf { const u8 *in; u32 inlen, inpos, bitbuf, bitcnt; u8 *out; u32 outlen, outpos; };
struct huff { short *count, *symbol; };
static int inf_bits(struct inf *s, u32 need) {
    u32 val = s->bitbuf;
    while (s->bitcnt < need) { if (s->inpos >= s->inlen) return -1; val |= (u32)s->in[s->inpos++] << s->bitcnt; s->bitcnt += 8; }
    s->bitbuf = val >> need; s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}
static int inf_stored(struct inf *s) {
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return -2;
    u32 len = s->in[s->inpos] | s->in[s->inpos + 1] << 8; s->inpos += 4;
    if (s->inpos + len > s->inlen) return -2;
    if (s->outpos + len > s->outlen) return -3;
    while (len--) s->out[s->outpos++] = s->in[s->inpos++];
    return 0;
}
static int inf_decode(struct inf *s, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = inf_bits(s, 1); if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -10;
}
static int inf_construct(struct huff *h, const short *length, int n) {
    for (int len = 0; len <= 15; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    short offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}
static const short inf_lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short inf_lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short inf_dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short inf_dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static int inf_codes(struct inf *s, const struct huff *lc, const struct huff *dc) {
    for (;;) {
        int sym = inf_decode(s, lc);
        if (sym < 0) return sym;
        if (sym < 256) { if (s->outpos >= s->outlen) return -3; s->out[s->outpos++] = (u8)sym; }
        else if (sym == 256) return 0;
        else {
            sym -= 257; if (sym >= 29) return -10;
            int e = inf_bits(s, (u32)inf_lext[sym]); if (e < 0) return -1;
            u32 len = (u32)(inf_lbase[sym] + e);
            int ds = inf_decode(s, dc); if (ds < 0 || ds >= 30) return -10;
            e = inf_bits(s, (u32)inf_dext[ds]); if (e < 0) return -1;
            u32 dist = (u32)(inf_dbase[ds] + e);
            if (dist > s->outpos) return -11;
            if (s->outpos + len > s->outlen) return -3;
            while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
        }
    }
}
static int inf_fixed(struct inf *s) {
    static short lcnt[16], lsym[288], dcnt[16], dsym[30]; static int built;
    static struct huff lc = {lcnt, lsym}, dc = {dcnt, dsym};
    if (!built) {
        short len[288];
        for (int i = 0; i < 144; i++) len[i] = 8; for (int i = 144; i < 256; i++) len[i] = 9; for (int i = 256; i < 280; i++) len[i] = 7; for (int i = 280; i < 288; i++) len[i] = 8;
        inf_construct(&lc, len, 288);
        for (int i = 0; i < 30; i++) len[i] = 5;
        inf_construct(&dc, len, 30);
        built = 1;
    }
    return inf_codes(s, &lc, &dc);
}
static int inf_dynamic(struct inf *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320], lcnt[16], lsym[288], dcnt[16], dsym[30];
    struct huff lc = {lcnt, lsym}, dc = {dcnt, dsym};
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30 || ncode < 4) return -3;
    int i;
    for (i = 0; i < ncode; i++) { int b = inf_bits(s, 3); if (b < 0) return -1; lengths[order[i]] = (short)b; }
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (inf_construct(&lc, lengths, 19) != 0) return -4;
    int index = 0;
    while (index < nlen + ndist) {
        int sym = inf_decode(s, &lc); if (sym < 0) return sym;
        if (sym < 16) lengths[index++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (index == 0) return -5; len = lengths[index - 1]; rep = 3 + inf_bits(s, 2); }
            else if (sym == 17) rep = 3 + inf_bits(s, 3);
            else rep = 11 + inf_bits(s, 7);
            if (index + rep > nlen + ndist) return -6;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -9;
    int err = inf_construct(&lc, lengths, nlen); if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -7;
    err = inf_construct(&dc, lengths + nlen, ndist); if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -8;
    return inf_codes(s, &lc, &dc);
}
// Decompress a raw DEFLATE stream. Returns the output length, or <0.
static int inflate_raw(const u8 *in, u32 inlen, u8 *out, u32 outlen) {
    struct inf s; s.in = in; s.inlen = inlen; s.inpos = 0; s.bitbuf = 0; s.bitcnt = 0; s.out = out; s.outlen = outlen; s.outpos = 0;
    int last, err;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        err = type == 0 ? inf_stored(&s) : type == 1 ? inf_fixed(&s) : type == 2 ? inf_dynamic(&s) : -2;
        if (err) return err;
        wdt_kick();
    } while (!last);
    return (int)s.outpos;
}

// ---------------- PNG ----------------
static u32 img_be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 blend_white(u32 r, u32 g, u32 b, u32 a) { return ((r * a + 255 * (255 - a)) / 255) << 16 | ((g * a + 255 * (255 - a)) / 255) << 8 | ((b * a + 255 * (255 - a)) / 255); }
static int png_decode(const u8 *d, u32 n, u32 **pix, u32 *w, u32 *h) {
    if (n < 33 || d[0] != 0x89 || d[1] != 'P' || d[2] != 'N' || d[3] != 'G') { img_why = "not a PNG"; return -1; }
    u32 W = 0, H = 0, depth = 0, ctype = 0, inter = 0, npal = 0, trns_n = 0; u8 pal[768], trns[256]; u32 tgray = 0x10000, tr = 0x10000, tg = 0, tb = 0;
    u32 idat_len = 0;
    for (u32 p = 8; p + 12 <= n;) {                                        // first pass: header, palette, transparency, total compressed size
        u32 len = img_be32(d + p); const u8 *t = d + p + 4, *c = d + p + 8;
        if (p + 12 + len > n) break;
        if (t[0] == 'I' && t[1] == 'H' && t[2] == 'D' && t[3] == 'R' && len >= 13) { W = img_be32(c); H = img_be32(c + 4); depth = c[8]; ctype = c[9]; inter = c[12]; }
        else if (t[0] == 'P' && t[1] == 'L' && t[2] == 'T' && t[3] == 'E') { npal = len / 3 > 256 ? 256 : len / 3; for (u32 i = 0; i < npal * 3; i++) pal[i] = c[i]; }
        else if (t[0] == 't' && t[1] == 'R' && t[2] == 'N' && t[3] == 'S') {
            if (ctype == 3) { trns_n = len > 256 ? 256 : len; for (u32 i = 0; i < trns_n; i++) trns[i] = c[i]; }
            else if (ctype == 0 && len >= 2) tgray = c[0] << 8 | c[1];
            else if (ctype == 2 && len >= 6) { tr = c[0] << 8 | c[1]; tg = c[2] << 8 | c[3]; tb = c[4] << 8 | c[5]; }
        }
        else if (t[0] == 'I' && t[1] == 'D' && t[2] == 'A' && t[3] == 'T') idat_len += len;
        p += 12 + len;
    }
    if (!W || !H || W > 8000 || H > 8000) { img_why = "PNG size"; return -1; }
    if (inter) { img_why = "interlaced PNG"; return -1; }
    u32 ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!ch || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) { img_why = "PNG format"; return -1; }
    u32 bpp = (ch * depth + 7) / 8, stride = (W * ch * depth + 7) / 8;
    if ((u64)W * H * 4 > (12u << 20)) { img_why = "picture too large"; return -1; }
    u8 *z = img_alloc(idat_len + 16), *raw = img_alloc((stride + 1) * H + 16);
    u32 *out = img_alloc(W * H * 4);
    if (!z || !raw || !out) { img_why = "out of picture memory"; return -1; }
    u32 zl = 0;
    for (u32 p = 8; p + 12 <= n;) {
        u32 len = img_be32(d + p); const u8 *t = d + p + 4;
        if (p + 12 + len > n) break;
        if (t[0] == 'I' && t[1] == 'D' && t[2] == 'A' && t[3] == 'T') { mcopy(z + zl, d + p + 8, len); zl += len; }
        p += 12 + len;
    }
    if (zl < 2) { img_why = "PNG data"; return -1; }
    int got = inflate_raw(z + 2, zl - 2, raw, (stride + 1) * H);           // skip the 2-byte zlib header
    if (got < (int)((stride + 1) * H)) { img_why = "PNG data damaged"; return -1; }
    // undo the per-row filters
    u8 *prev = 0;
    for (u32 y = 0; y < H; y++) {
        u8 *row = raw + y * (stride + 1), f = row[0], *cur = row + 1;
        for (u32 x = 0; x < stride; x++) {
            u32 a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            if (f == 1) cur[x] = (u8)(cur[x] + a);
            else if (f == 2) cur[x] = (u8)(cur[x] + b);
            else if (f == 3) cur[x] = (u8)(cur[x] + ((a + b) >> 1));
            else if (f == 4) { int pa = (int)b - (int)c, pb = (int)a - (int)c, pc = pa + pb; if (pa < 0) pa = -pa; if (pb < 0) pb = -pb; if (pc < 0) pc = -pc; cur[x] = (u8)(cur[x] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c)); }
        }
        prev = cur;
        // to pixels
        for (u32 x = 0; x < W; x++) {
            u32 r, g, bl, al = 255;
            if (depth == 16) {
                const u8 *s = cur + x * ch * 2;
                if (ch == 1) { r = g = bl = s[0]; if ((u32)(s[0] << 8 | s[1]) == tgray) al = 0; }
                else if (ch == 2) { r = g = bl = s[0]; al = s[2]; }
                else if (ch == 3) { r = s[0]; g = s[2]; bl = s[4]; if ((u32)(s[0] << 8 | s[1]) == tr && (u32)(s[2] << 8 | s[3]) == tg && (u32)(s[4] << 8 | s[5]) == tb) al = 0; }
                else { r = s[0]; g = s[2]; bl = s[4]; al = s[6]; }
            } else if (depth == 8) {
                const u8 *s = cur + x * ch;
                if (ctype == 3) { u32 i = s[0]; r = i < npal ? pal[i * 3] : 0; g = i < npal ? pal[i * 3 + 1] : 0; bl = i < npal ? pal[i * 3 + 2] : 0; if (i < trns_n) al = trns[i]; }
                else if (ch == 1) { r = g = bl = s[0]; if (s[0] == tgray) al = 0; }
                else if (ch == 2) { r = g = bl = s[0]; al = s[1]; }
                else if (ch == 3) { r = s[0]; g = s[1]; bl = s[2]; if (s[0] == tr && s[1] == tg && s[2] == tb) al = 0; }
                else { r = s[0]; g = s[1]; bl = s[2]; al = s[3]; }
            } else {                                                       // 1, 2 or 4 bits per pixel: gray or palette
                u32 per = 8 / depth, v = (cur[x / per] >> ((per - 1 - x % per) * depth)) & ((1u << depth) - 1);
                if (ctype == 3) { r = v < npal ? pal[v * 3] : 0; g = v < npal ? pal[v * 3 + 1] : 0; bl = v < npal ? pal[v * 3 + 2] : 0; if (v < trns_n) al = trns[v]; }
                else { r = g = bl = v * 255 / ((1u << depth) - 1); if (v == tgray) al = 0; }
            }
            out[y * W + x] = al == 255 ? (r << 16 | g << 8 | bl) : blend_white(r, g, bl, al);
        }
        if ((y & 63) == 0) wdt_kick();
    }
    *pix = out; *w = W; *h = H;
    return 0;
}

// ---------------- GIF (first frame) ----------------
static int gif_decode(const u8 *d, u32 n, u32 **pix, u32 *w, u32 *h) {
    if (n < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F') { img_why = "not a GIF"; return -1; }
    u32 W = d[6] | d[7] << 8, H = d[8] | d[9] << 8, flags = d[10], p = 13;
    if (!W || !H || W > 4000 || H > 4000) { img_why = "GIF size"; return -1; }
    const u8 *gct = 0; u32 gct_n = 0;
    if (flags & 0x80) { gct_n = 2u << (flags & 7); gct = d + p; p += gct_n * 3; }
    u32 *out = img_alloc(W * H * 4); if (!out) { img_why = "out of picture memory"; return -1; }
    for (u32 i = 0; i < W * H; i++) out[i] = 0xffffff;
    int transp = -1;
    while (p < n) {
        u8 b = d[p++];
        if (b == 0x21) {                                                     // extension: remember the transparent colour, skip the rest
            if (p + 1 > n) break;
            u8 label = d[p++];
            if (label == 0xf9 && p + 5 < n && d[p] >= 4) { if (d[p + 1] & 1) transp = d[p + 4]; }
            while (p < n && d[p]) p += 1 + d[p];
            p++;
        } else if (b == 0x2c) {                                              // the picture
            if (p + 9 > n) break;
            u32 ix = d[p] | d[p + 1] << 8, iy = d[p + 2] | d[p + 3] << 8, iw = d[p + 4] | d[p + 5] << 8, ih = d[p + 6] | d[p + 7] << 8, f = d[p + 8]; p += 9;
            const u8 *ct = gct; u32 ct_n = gct_n;
            if (f & 0x80) { ct_n = 2u << (f & 7); ct = d + p; p += ct_n * 3; }
            int interlaced = (f & 0x40) != 0;
            if (p >= n || !ct) { img_why = "GIF data"; return -1; }
            u32 minc = d[p++];
            if (minc < 2 || minc > 8) { img_why = "GIF data"; return -1; }
            // collect the data sub-blocks
            u32 total = 0, q = p; while (q < n && d[q]) { total += d[q]; q += 1 + d[q]; }
            u8 *data = img_alloc(total + 4); if (!data) { img_why = "out of picture memory"; return -1; }
            u32 k = 0; q = p; while (q < n && d[q]) { mcopy(data + k, d + q + 1, d[q]); k += d[q]; q += 1 + d[q]; }
            // LZW
            static u16 prefix[4096]; static u8 suffix[4096], stack[4097];
            u32 clear = 1u << minc, end = clear + 1, avail = clear + 2, size = minc + 1, mask = (1u << size) - 1, old = 0xffff, first = 0;
            for (u32 c = 0; c < clear; c++) { prefix[c] = 0xffff; suffix[c] = (u8)c; }
            u32 bits = 0, nb = 0, bp = 0, pos = 0, total_px = iw * ih;
            u32 pass = 0, row = 0, col = 0;
            while (pos < total_px) {
                while (nb < size && bp < k) { bits |= (u32)data[bp++] << nb; nb += 8; }
                if (nb < size) break;
                u32 code = bits & mask; bits >>= size; nb -= size;
                if (code == clear) { size = minc + 1; mask = (1u << size) - 1; avail = clear + 2; old = 0xffff; continue; }
                if (code == end) break;
                u32 sp = 0, in = code;
                if (old == 0xffff) { stack[sp++] = suffix[code]; first = code; old = code; }
                else {
                    if (code >= avail) { stack[sp++] = (u8)first; code = old; }
                    while (code >= clear) { if (sp >= 4096) break; stack[sp++] = suffix[code]; code = prefix[code]; }
                    first = suffix[code]; stack[sp++] = (u8)first;
                    if (avail < 4096) { prefix[avail] = (u16)old; suffix[avail] = (u8)first; avail++; if ((avail & mask) == 0 && avail < 4096) { size++; mask = (1u << size) - 1; } }
                    old = in;
                }
                while (sp && pos < total_px) {
                    u32 ci = stack[--sp];
                    u32 x = ix + col, y = iy + row;
                    if (x < W && y < H && (int)ci != transp && ci < ct_n) out[y * W + x] = (u32)ct[ci * 3] << 16 | (u32)ct[ci * 3 + 1] << 8 | ct[ci * 3 + 2];
                    pos++;
                    if (++col >= iw) {
                        col = 0;
                        if (!interlaced) row++;
                        else {                                               // rows come in 4 passes: 0,8,16.. then 4,12.. then 2,6.. then 1,3..
                            static const u8 start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
                            row += step[pass];
                            while (row >= ih && pass < 3) { pass++; row = start[pass]; }
                        }
                    }
                }
            }
            *pix = out; *w = W; *h = H;
            return 0;
        } else break;
    }
    img_why = "GIF has no picture"; return -1;
}

// ---------------- JPEG (baseline, Huffman) ----------------
static const u8 jpg_zz[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
struct jhuff { u16 code[256]; u8 size[256], val[256]; u32 n; int maxcode[18], valptr[17], mincode[17]; };
struct jcomp { u32 id, h, v, tq, td, ta; int dc; u8 *plane; u32 pw, ph; };
static struct { const u8 *d; u32 n, p, bits, nb; int eof; u16 q[4][64]; struct jhuff hd[4], ha[4]; struct jcomp c[4]; u32 nc, W, H, hmax, vmax, restart; } jp;
static void jhuff_build(struct jhuff *t, const u8 *counts, const u8 *vals) {
    u32 k = 0, code = 0;
    for (u32 len = 1; len <= 16; len++) {
        t->valptr[len] = (int)k; t->mincode[len] = (int)code;
        for (u32 i = 0; i < counts[len - 1]; i++) { t->code[k] = (u16)code; t->size[k] = (u8)len; t->val[k] = vals[k]; k++; code++; }
        t->maxcode[len] = counts[len - 1] ? (int)code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff; t->n = k;
}
static u32 jbit(void) {
    if (!jp.nb) {
        if (jp.p >= jp.n) { jp.eof = 1; return 0; }
        u8 b = jp.d[jp.p];
        if (b == 0xff) { u8 nx = jp.p + 1 < jp.n ? jp.d[jp.p + 1] : 0; if (nx == 0) jp.p += 2; else { jp.eof = 1; return 0; } }   // a marker: no more data in this segment
        else jp.p++;
        jp.bits = b; jp.nb = 8;
    }
    jp.nb--;
    return (jp.bits >> jp.nb) & 1;
}
static u32 jbits(u32 n) { u32 v = 0; while (n--) v = v << 1 | jbit(); return v; }
static int jdecode(const struct jhuff *t) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = code << 1 | (int)jbit();
        if (t->maxcode[len] >= 0 && code <= t->maxcode[len]) return t->val[t->valptr[len] + code - t->mincode[len]];
    }
    return -1;
}
static int jextend(u32 v, u32 s) { return s && v < (1u << (s - 1)) ? (int)v - (int)(1u << s) + 1 : (int)v; }
// integer IDCT (the classic "islow" version, as in stb_image)
#define JIDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    int p2, p3, p1, t0, t1, t2, t3, p4, p5, x0, x1, x2, x3; \
    p2 = s2; p3 = s6; p1 = (p2 + p3) * (2217); t2 = p1 + p3 * (-7567); t3 = p1 + p2 * (3135); \
    p2 = s0; p3 = s4; t0 = (p2 + p3) * 4096; t1 = (p2 - p3) * 4096; \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; p5 = (p3 + p4) * (4816); \
    t0 = t0 * (1223); t1 = t1 * (8410); t2 = t2 * (12586); t3 = t3 * (6149); \
    p1 = p5 + p1 * (-3685); p2 = p5 + p2 * (-10497); p3 = p3 * (-8034); p4 = p4 * (-1597); \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;
static u8 jclamp(int x) { return x < 0 ? 0 : x > 255 ? 255 : (u8)x; }
static void jidct(const int *in, u8 *out, u32 stride) {
    int v[64];
    for (int i = 0; i < 8; i++) {
        const int *d = in + i; int *o = v + i;
        if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) { int dc = d[0] * 4; for (int k = 0; k < 8; k++) o[k * 8] = dc; continue; }
        JIDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        o[0] = (x0 + t3) >> 10; o[56] = (x0 - t3) >> 10; o[8] = (x1 + t2) >> 10; o[48] = (x1 - t2) >> 10;
        o[16] = (x2 + t1) >> 10; o[40] = (x2 - t1) >> 10; o[24] = (x3 + t0) >> 10; o[32] = (x3 - t0) >> 10;
    }
    for (int i = 0; i < 8; i++) {
        int *vv = v + i * 8; u8 *o = out + i * stride;
        JIDCT_1D(vv[0], vv[1], vv[2], vv[3], vv[4], vv[5], vv[6], vv[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        o[0] = jclamp((x0 + t3) >> 17); o[7] = jclamp((x0 - t3) >> 17); o[1] = jclamp((x1 + t2) >> 17); o[6] = jclamp((x1 - t2) >> 17);
        o[2] = jclamp((x2 + t1) >> 17); o[5] = jclamp((x2 - t1) >> 17); o[3] = jclamp((x3 + t0) >> 17); o[4] = jclamp((x3 - t0) >> 17);
    }
}
static int jblock(struct jcomp *c, u8 *dst, u32 stride) {
    int blk[64]; for (int i = 0; i < 64; i++) blk[i] = 0;
    int t = jdecode(&jp.hd[c->td]); if (t < 0) return -1;
    int diff = t ? jextend(jbits((u32)t), (u32)t) : 0;
    c->dc += diff;
    blk[0] = c->dc * jp.q[c->tq][0];
    for (int k = 1; k < 64;) {
        int rs = jdecode(&jp.ha[c->ta]); if (rs < 0) return -1;
        int r = rs >> 4, s = rs & 15;
        if (!s) { if (r == 15) { k += 16; continue; } break; }
        k += r; if (k > 63) return -1;
        blk[jpg_zz[k]] = jextend(jbits((u32)s), (u32)s) * jp.q[c->tq][k];
        k++;
    }
    jidct(blk, dst, stride);
    return 0;
}
static int jpeg_decode(const u8 *d, u32 n, u32 **pix, u32 *w, u32 *h) {
    if (n < 4 || d[0] != 0xff || d[1] != 0xd8) { img_why = "not a JPEG"; return -1; }
    jp.d = d; jp.n = n; jp.nc = 0; jp.restart = 0;
    u32 p = 2;
    while (p + 4 <= n) {
        if (d[p] != 0xff) { p++; continue; }
        u8 m = d[p + 1];
        if (m == 0xff) { p++; continue; }
        if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7)) { p += 2; continue; }
        if (m == 0xd9) break;
        u32 len = d[p + 2] << 8 | d[p + 3]; const u8 *s = d + p + 4;
        if (p + 2 + len > n) { img_why = "JPEG cut short"; return -1; }
        if (m == 0xdb) {                                                     // quantisation tables
            for (u32 q = 0; q + 1 < len - 2;) { u32 pq = s[q] >> 4, tq = s[q] & 3; q++; for (u32 i = 0; i < 64; i++) { jp.q[tq][i] = pq ? (u16)(s[q] << 8 | s[q + 1]) : s[q]; q += pq ? 2 : 1; } }
        } else if (m == 0xc4) {                                              // Huffman tables
            for (u32 q = 0; q + 17 <= len - 2;) {
                u32 tc = s[q] >> 4, th = s[q] & 3; const u8 *counts = s + q + 1; u32 tot = 0;
                for (u32 i = 0; i < 16; i++) tot += counts[i];
                if (tot > 256) { img_why = "JPEG tables"; return -1; }
                jhuff_build(tc ? &jp.ha[th] : &jp.hd[th], counts, s + q + 17);
                q += 17 + tot;
            }
        } else if (m == 0xc0 || m == 0xc1) {                                 // baseline frame
            if (s[0] != 8) { img_why = "12-bit JPEG"; return -1; }
            jp.H = s[1] << 8 | s[2]; jp.W = s[3] << 8 | s[4]; jp.nc = s[5];
            if (!jp.W || !jp.H || jp.nc == 0 || jp.nc > 3 || jp.nc == 2) { img_why = "JPEG format"; return -1; }
            if ((u64)jp.W * jp.H * 4 > (12u << 20)) { img_why = "picture too large"; return -1; }
            jp.hmax = jp.vmax = 1;
            for (u32 i = 0; i < jp.nc; i++) { jp.c[i].id = s[6 + i * 3]; jp.c[i].h = s[7 + i * 3] >> 4; jp.c[i].v = s[7 + i * 3] & 15; jp.c[i].tq = s[8 + i * 3] & 3; if (jp.c[i].h > jp.hmax) jp.hmax = jp.c[i].h; if (jp.c[i].v > jp.vmax) jp.vmax = jp.c[i].v; }
        } else if (m == 0xc2 || m == 0xc3 || (m >= 0xc5 && m <= 0xcf && m != 0xc8 && m != 0xcc)) { img_why = m == 0xc2 ? "progressive JPEG" : "JPEG type"; return -1; }
        else if (m == 0xdd) jp.restart = s[0] << 8 | s[1];
        else if (m == 0xda) {                                                // the picture data
            if (!jp.nc) { img_why = "JPEG data"; return -1; }
            u32 ns = s[0];
            for (u32 i = 0; i < ns; i++) for (u32 k = 0; k < jp.nc; k++) if (jp.c[k].id == s[1 + i * 2]) { jp.c[k].td = s[2 + i * 2] >> 4; jp.c[k].ta = s[2 + i * 2] & 3; }
            u32 mcux = (jp.W + 8 * jp.hmax - 1) / (8 * jp.hmax), mcuy = (jp.H + 8 * jp.vmax - 1) / (8 * jp.vmax);
            for (u32 k = 0; k < jp.nc; k++) { jp.c[k].pw = mcux * jp.c[k].h * 8; jp.c[k].ph = mcuy * jp.c[k].v * 8; jp.c[k].plane = img_alloc(jp.c[k].pw * jp.c[k].ph); jp.c[k].dc = 0; if (!jp.c[k].plane) { img_why = "out of picture memory"; return -1; } }
            jp.p = p + 2 + len; jp.nb = 0; jp.eof = 0;
            u32 todo = jp.restart;
            for (u32 my = 0; my < mcuy; my++) {
                for (u32 mx = 0; mx < mcux; mx++) {
                    if (jp.restart && !todo) {                               // restart marker: realign and reset the DC predictors
                        jp.nb = 0; while (jp.p + 1 < jp.n && !(jp.d[jp.p] == 0xff && jp.d[jp.p + 1] >= 0xd0 && jp.d[jp.p + 1] <= 0xd7)) jp.p++;
                        jp.p += 2; jp.eof = 0; for (u32 k = 0; k < jp.nc; k++) jp.c[k].dc = 0; todo = jp.restart;
                    }
                    for (u32 k = 0; k < jp.nc; k++) {
                        struct jcomp *c = &jp.c[k];
                        for (u32 by = 0; by < c->v; by++) for (u32 bx = 0; bx < c->h; bx++)
                            if (jblock(c, c->plane + ((my * c->v + by) * 8) * c->pw + (mx * c->h + bx) * 8, c->pw)) { img_why = "JPEG data damaged"; goto done; }
                    }
                    if (jp.restart) todo--;
                }
                if ((my & 7) == 0) wdt_kick();
            }
        done:;
            u32 *out = img_alloc(jp.W * jp.H * 4); if (!out) { img_why = "out of picture memory"; return -1; }
            for (u32 y = 0; y < jp.H; y++) for (u32 x = 0; x < jp.W; x++) {
                struct jcomp *c0 = &jp.c[0];
                int Y = c0->plane[(y * c0->v / jp.vmax) * c0->pw + x * c0->h / jp.hmax];
                if (jp.nc == 1) { out[y * jp.W + x] = (u32)Y << 16 | (u32)Y << 8 | (u32)Y; continue; }
                struct jcomp *c1 = &jp.c[1], *c2 = &jp.c[2];
                int cb = c1->plane[(y * c1->v / jp.vmax) * c1->pw + x * c1->h / jp.hmax] - 128, cr = c2->plane[(y * c2->v / jp.vmax) * c2->pw + x * c2->h / jp.hmax] - 128;
                int r = Y + ((91881 * cr) >> 16), g = Y - ((22554 * cb + 46802 * cr) >> 16), b = Y + ((116130 * cb) >> 16);
                out[y * jp.W + x] = (u32)jclamp(r) << 16 | (u32)jclamp(g) << 8 | jclamp(b);
            }
            *pix = out; *w = jp.W; *h = jp.H;
            return 0;
        }
        p += 2 + len;
    }
    img_why = "JPEG has no picture"; return -1;
}

// ---------------- any picture -> scaled to fit ----------------
// Decode d[0..n) and shrink it (averaging) to at most maxw x maxh. Returns the pixels (in the picture memory) or 0 with img_why set.
static u32 *img_load(const u8 *d, u32 n, u32 maxw, u32 maxh, u32 *ow, u32 *oh) {
    u32 *pix = 0, w = 0, h = 0; int r;
    u32 mark = img_bump;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P') r = png_decode(d, n, &pix, &w, &h);
    else if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') r = gif_decode(d, n, &pix, &w, &h);
    else if (n >= 3 && d[0] == 0xff && d[1] == 0xd8) r = jpeg_decode(d, n, &pix, &w, &h);
    else { img_why = (n >= 12 && d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P') ? "WebP" : (n > 4 && (d[0] == '<' || d[1] == '<' || d[0] == 0xef)) ? "SVG or a page" : "unknown format"; img_bump = mark; return 0; }
    if (r) { img_bump = mark; return 0; }
    u32 tw = w, th = h;
    if (tw > maxw) { th = (u32)((u64)th * maxw / tw); tw = maxw; }
    if (th > maxh) { tw = (u32)((u64)tw * maxh / th); th = maxh; }
    if (!tw) tw = 1; if (!th) th = 1;
    u32 *small = 0;
    if (tw == w && th == h) {
        img_bump = mark;                                                         // keep only the picture: move it to where the scratch space began
        small = img_alloc(w * h * 4);
        for (u32 i = 0; i < w * h; i++) small[i] = pix[i];                        // (small <= pix: copying forward is safe)
    } else {
        u32 *res = img_alloc(tw * th * 4);                                       // the result comes after the scratch; moved down afterwards
        if (!res) { img_bump = mark; img_why = "out of picture memory"; return 0; }
        for (u32 y = 0; y < th; y++) {
            u32 y0 = (u32)((u64)y * h / th), y1 = (u32)((u64)(y + 1) * h / th); if (y1 <= y0) y1 = y0 + 1;
            for (u32 x = 0; x < tw; x++) {
                u32 x0 = (u32)((u64)x * w / tw), x1 = (u32)((u64)(x + 1) * w / tw); if (x1 <= x0) x1 = x0 + 1;
                u32 sr = 0, sg = 0, sb = 0, cnt = 0;
                for (u32 yy = y0; yy < y1; yy++) for (u32 xx = x0; xx < x1; xx++) { u32 v = pix[yy * w + xx]; sr += v >> 16 & 255; sg += v >> 8 & 255; sb += v & 255; cnt++; }
                res[y * tw + x] = (sr / cnt) << 16 | (sg / cnt) << 8 | (sb / cnt);
            }
            if ((y & 31) == 0) wdt_kick();
        }
        img_bump = mark;
        small = img_alloc(tw * th * 4);
        for (u32 i = 0; i < tw * th; i++) small[i] = res[i];                      // small starts at mark, before res: forward copy is safe
    }
    *ow = tw; *oh = th;
    return small;
}
