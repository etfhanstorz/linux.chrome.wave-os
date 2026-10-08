// HTTPS (v1.7): a TLS 1.2 client. Key exchange X25519 (ECDHE), encryption AES-128-GCM, cipher suites
// TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xc02f) and TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 (0xc02b).
//
// NOT YET: checking the server's certificate and signature. The connection is encrypted, but a machine on the network could pretend
// to be the website. The browser marks such pages "encrypted, not verified" until certificate checking is added.

// ---------------- X25519 (RFC 7748), after TweetNaCl ----------------
typedef long long gf25[16];
static const gf25 gf25_121665 = {0xDB41, 1};
static void car25519(long long *o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        long long c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void sel25519(long long *p, long long *q, int b) {
    long long c = ~(long long)(b - 1);
    for (int i = 0; i < 16; i++) { long long t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(u8 *o, const long long *n) {
    long long m[16], t[16];
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (u8)(t[i] & 0xff); o[2 * i + 1] = (u8)(t[i] >> 8); }
}
static void unpack25519(long long *o, const u8 *n) { for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((long long)n[2 * i + 1] << 8); o[15] &= 0x7fff; }
static void A25(long long *o, const long long *a, const long long *b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z25(long long *o, const long long *a, const long long *b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void M25(long long *o, const long long *a, const long long *b) {
    long long t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void S25(long long *o, const long long *a) { M25(o, a, a); }
static void inv25519(long long *o, const long long *i) {
    long long c[16];
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) { S25(c, c); if (a != 2 && a != 4) M25(c, c, i); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}
// q = n * p (all 32 bytes, little endian u-coordinates)
static void x25519(u8 *q, const u8 *n, const u8 *p) {
    u8 z[32]; long long x[80], a[16], b[16], c[16], d[16], e[16], f[16];
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (u8)((n[31] & 127) | 64); z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        A25(e, a, c); Z25(a, a, c); A25(c, b, d); Z25(b, b, d); S25(d, e); S25(f, a); M25(a, c, a); M25(c, b, e);
        A25(e, a, c); Z25(a, a, c); S25(b, a); Z25(c, d, f); M25(a, c, gf25_121665); A25(a, a, d); M25(c, c, a); M25(a, d, f); M25(d, b, x); S25(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519(x + 32, x + 32);
    M25(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}

// ---------------- AES-128-GCM (NIST SP 800-38D), GHASH with 4-bit tables (as in mbed TLS) ----------------
struct gcm { struct aes a; u64 HL[16], HH[16]; };
static u64 be64r(const u8 *p) { u64 v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static void be64w(u8 *p, u64 v) { for (int i = 7; i >= 0; i--) { p[i] = (u8)v; v >>= 8; } }
static void gcm_init(struct gcm *g, const u8 *key16) {
    aes_key(&g->a, key16);
    u8 h[16] = {0}; aes_enc(&g->a, h, h);
    u64 vh = be64r(h), vl = be64r(h + 8);
    g->HL[8] = vl; g->HH[8] = vh; g->HH[0] = 0; g->HL[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        u32 T = (u32)(vl & 1) * 0xe1000000U;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((u64)T << 32);
        g->HL[i] = vl; g->HH[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2) {
        u64 *HiL = g->HL + i, *HiH = g->HH + i; vh = *HiH; vl = *HiL;
        for (int j = 1; j < i; j++) { HiH[j] = vh ^ g->HH[j]; HiL[j] = vl ^ g->HL[j]; }
    }
}
static const u16 gcm_last4[16] = {0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0, 0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0};
static void gcm_mult(const struct gcm *g, const u8 *x, u8 *out) {
    u32 lo = x[15] & 0xf;
    u64 zh = g->HH[lo], zl = g->HL[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 0xf; u32 hi = (x[i] >> 4) & 0xf, rem;
        if (i != 15) { rem = (u32)(zl & 0xf); zl = (zh << 60) | (zl >> 4); zh = zh >> 4; zh ^= (u64)gcm_last4[rem] << 48; zh ^= g->HH[lo]; zl ^= g->HL[lo]; }
        rem = (u32)(zl & 0xf); zl = (zh << 60) | (zl >> 4); zh = zh >> 4; zh ^= (u64)gcm_last4[rem] << 48; zh ^= g->HH[hi]; zl ^= g->HL[hi];
    }
    be64w(out, zh); be64w(out + 8, zl);
}
static void gcm_ghash(const struct gcm *g, u8 *y, const u8 *d, u32 n) {
    for (u32 off = 0; off < n; off += 16) {
        for (u32 i = 0; i < 16 && off + i < n; i++) y[i] ^= d[off + i];
        gcm_mult(g, y, y);
    }
}
// Encrypt (decrypt = 0) or decrypt n bytes in -> out (may be the same buffer); writes the 16-byte tag.
static void gcm_crypt(const struct gcm *g, const u8 *iv12, const u8 *aad, u32 alen, const u8 *in, u8 *out, u32 n, u8 *tag, int decrypt) {
    u8 j0[16], ctr[16], ks[16], y[16] = {0}, lens[16];
    for (int i = 0; i < 12; i++) j0[i] = iv12[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    gcm_ghash(g, y, aad, alen);
    if (decrypt) gcm_ghash(g, y, in, n);                                   // the tag covers the ciphertext: hash it before it is overwritten
    for (int i = 0; i < 16; i++) ctr[i] = j0[i];
    for (u32 off = 0; off < n; off += 16) {
        for (int i = 15; i >= 12; i--) if (++ctr[i]) break;
        aes_enc(&g->a, ctr, ks);
        for (u32 i = 0; i < 16 && off + i < n; i++) out[off + i] = in[off + i] ^ ks[i];
    }
    if (!decrypt) gcm_ghash(g, y, out, n);
    be64w(lens, (u64)alen * 8); be64w(lens + 8, (u64)n * 8);
    for (int i = 0; i < 16; i++) y[i] ^= lens[i];
    gcm_mult(g, y, y);
    aes_enc(&g->a, j0, ks);
    for (int i = 0; i < 16; i++) tag[i] = ks[i] ^ y[i];
}

// ---------------- TLS 1.2 PRF (RFC 5246 section 5, SHA-256) ----------------
static void tls_prf(const u8 *secret, u32 slen, const char *label, const u8 *seed, u32 seedlen, u8 *out, u32 outlen) {
    u8 ls[160]; u32 ll = 0;
    while (label[ll]) { ls[ll] = (u8)label[ll]; ll++; }
    for (u32 i = 0; i < seedlen && ll + i < sizeof ls; i++) ls[ll + i] = seed[i];
    u32 lsn = ll + seedlen;
    u8 a[32], buf[32 + 160], t[32];
    hmac_sha256(secret, slen, ls, lsn, a);
    for (u32 done = 0; done < outlen;) {
        for (int i = 0; i < 32; i++) buf[i] = a[i];
        for (u32 i = 0; i < lsn; i++) buf[32 + i] = ls[i];
        hmac_sha256(secret, slen, buf, 32 + lsn, t);
        for (u32 k = 0; k < 32 && done < outlen; k++) out[done++] = t[k];
        hmac_sha256(secret, slen, a, 32, a);
    }
}

// ---------------- the TLS connection ----------------
#define TLS_IN_MAX (64u * 1024)                                             // TCP stream buffer (encrypted records not yet processed)
#define TLS_HS_MAX (128u * 1024)                                            // handshake messages being put together (certificate chains can be large)
static u8 *tls_in, *tls_hs;                                                 // in update_buf, after the browser's buffers
static u32 tls_hs_len, tls_hs_pos;
static struct { u8 crand[32], srand[32], ms[48], spub[32], civ[4], siv[4]; struct gcm cg, sg; u64 cseq, sseq; int enc_out, enc_in; struct sha256 hs; u16 suite; int alert; } tls;
static u32 tls_err_why;                                                     // why the last https_get failed (for the error page)

static void tls_buffers(void) {
    tls_in = update_buf + 1664u * 1024;
    tls_hs = update_buf + 1664u * 1024 + TLS_IN_MAX;
}
// TCP stream: wait until n bytes are buffered. 0 = ok, -1 = timeout, -2 = connection reset, -3 = closed before that much arrived.
static int tls_wait(u32 n, u64 deadline) {
    while (tcp.got < n) {
        if (tcp.state == TCP_RESET) return -2;
        if (tcp.state == TCP_DONE) return -3;
        if (ticks() > deadline) return -1;
        net_poll(); wdt_kick(); tcp_retransmit_check();
    }
    return 0;
}
static void tls_consume(u32 n) {
    u32 rest = tcp.got - n;
    for (u32 i = 0; i < rest; i++) tls_in[i] = tls_in[n + i];
    tcp.got = rest;
}
static u32 tls_sent;
static int tls_send(u8 type, const u8 *d, u32 n) {
    static u8 rec[2048];
    if (n + 5 + 8 + 16 > sizeof rec) return -1;
    rec[0] = type; rec[1] = 3; rec[2] = tls_sent++ ? 3 : 1;                 // the very first record says TLS 1.0 like everyone does
    if (!tls.enc_out) { be16w(rec + 3, n); mcopy(rec + 5, d, n); return tcp_write(rec, 5 + n); }
    u8 nonce[12], aad[13];
    mcopy(nonce, tls.civ, 4); be64w(nonce + 4, tls.cseq);
    be64w(aad, tls.cseq); aad[8] = type; aad[9] = 3; aad[10] = 3; be16w(aad + 11, n);
    be16w(rec + 3, 8 + n + 16);
    mcopy(rec + 5, nonce + 4, 8);
    gcm_crypt(&tls.cg, nonce, aad, 13, d, rec + 13, n, rec + 13 + n, 0);
    tls.cseq++;
    return tcp_write(rec, 5 + 8 + n + 16);
}
// Next record: *type, *p (plaintext inside tls_in), *n. The caller calls tls_consume(*used) when done with it. 0 = ok, <0 = error.
static int tls_record(u8 *type, u8 **p, u32 *n, u32 *used, u64 deadline) {
    int r = tls_wait(5, deadline); if (r) return r;
    u32 len = be16r(tls_in + 3);
    if (len > 16384 + 2048) return -5;
    r = tls_wait(5 + len, deadline); if (r) return r;
    *type = tls_in[0]; *used = 5 + len;
    if (!tls.enc_in || *type == 20) { *p = tls_in + 5; *n = len; return 0; }
    if (len < 8 + 16) return -6;
    u32 ct = len - 8 - 16;
    u8 nonce[12], aad[13], tag[16];
    mcopy(nonce, tls.siv, 4); mcopy(nonce + 4, tls_in + 5, 8);
    be64w(aad, tls.sseq); aad[8] = *type; aad[9] = 3; aad[10] = 3; be16w(aad + 11, ct);
    gcm_crypt(&tls.sg, nonce, aad, 13, tls_in + 13, tls_in + 13, ct, tag, 1);
    if (!meq(tag, tls_in + 13 + ct, 16)) return -7;                        // damaged or forged record
    tls.sseq++;
    *p = tls_in + 13; *n = ct;
    return 0;
}
// Next whole handshake message (header included): returns its length, or <0.
static int tls_handshake_msg(u8 **msg, u64 deadline) {
    for (;;) {
        if (tls_hs_len - tls_hs_pos >= 4) {
            u32 ml = (u32)tls_hs[tls_hs_pos + 1] << 16 | (u32)tls_hs[tls_hs_pos + 2] << 8 | tls_hs[tls_hs_pos + 3];
            if (tls_hs_len - tls_hs_pos >= 4 + ml) { *msg = tls_hs + tls_hs_pos; tls_hs_pos += 4 + ml; return (int)(4 + ml); }
            if (4 + ml > TLS_HS_MAX) return -8;
        }
        u8 type; u8 *p; u32 n, used;
        int r = tls_record(&type, &p, &n, &used, deadline); if (r) return r;
        if (type == 21) { tls.alert = n >= 2 ? p[1] : 0; tls_consume(used); return -9; }
        if (type != 22) { tls_consume(used); return -10; }
        if (tls_hs_pos == tls_hs_len) tls_hs_pos = tls_hs_len = 0;
        if (tls_hs_len + n > TLS_HS_MAX) { tls_consume(used); return -8; }
        mcopy(tls_hs + tls_hs_len, p, n); tls_hs_len += n;
        tls_consume(used);
    }
}
static void tls_hash(const u8 *m, u32 n) { sha256_update(&tls.hs, m, n); }
static void tls_hash_now(u8 *out) {                                         // the hash so far, without ending it (byte copy: a struct copy would need memcpy)
    struct sha256 c; volatile u8 *d = (volatile u8 *)&c; const u8 *s = (const u8 *)&tls.hs;
    for (u32 i = 0; i < sizeof c; i++) d[i] = s[i];
    sha256_final(&c, out);
}

// The handshake on an open TCP connection. 0 = ok; <0 = failed (tls_err_why says which stage).
static int tls_handshake(const char *host, u64 deadline) {
    tls.enc_in = tls.enc_out = 0; tls.cseq = tls.sseq = 0; tls.alert = -1; tls_hs_len = tls_hs_pos = 0; tls_sent = 0;
    sha256_init(&tls.hs);
    wifi_random(tls.crand);
    // ---- ClientHello ----
    static u8 ch[512]; u32 n = 4;
    ch[n++] = 3; ch[n++] = 3;
    mcopy(ch + n, tls.crand, 32); n += 32;
    ch[n++] = 0;                                                            // no session id
    static const u16 suites[] = {0xc02f, 0xc02b, 0x00ff};                   // ECDHE-RSA/ECDSA AES-128-GCM SHA-256, + "renegotiation info" signal
    be16w(ch + n, sizeof suites); n += 2;
    for (u32 i = 0; i < sizeof suites / 2; i++) { be16w(ch + n, suites[i]); n += 2; }
    ch[n++] = 1; ch[n++] = 0;                                               // compression: none
    u32 ext = n; n += 2;
    u32 hl = 0; while (host[hl]) hl++;
    be16w(ch + n, 0); be16w(ch + n + 2, hl + 5); be16w(ch + n + 4, hl + 3); ch[n + 6] = 0; be16w(ch + n + 7, hl); n += 9;   // server_name
    mcopy(ch + n, (const u8 *)host, hl); n += hl;
    be16w(ch + n, 0x000a); be16w(ch + n + 2, 4); be16w(ch + n + 4, 2); be16w(ch + n + 6, 0x001d); n += 8;          // supported_groups: x25519
    be16w(ch + n, 0x000b); be16w(ch + n + 2, 2); ch[n + 4] = 1; ch[n + 5] = 0; n += 6;                              // ec_point_formats: uncompressed
    static const u16 sigs[] = {0x0804, 0x0403, 0x0401, 0x0805, 0x0503, 0x0501, 0x0806, 0x0601};
    be16w(ch + n, 0x000d); be16w(ch + n + 2, 2 + sizeof sigs); be16w(ch + n + 4, sizeof sigs); n += 6;              // signature_algorithms
    for (u32 i = 0; i < sizeof sigs / 2; i++) { be16w(ch + n, sigs[i]); n += 2; }
    be16w(ch + ext, n - ext - 2);
    ch[0] = 1; ch[1] = 0; be16w(ch + 2, n - 4);
    tls_hash(ch, n);
    if (tls_send(22, ch, n)) { tls_err_why = 1; return -1; }
    // ---- ServerHello .. ServerHelloDone ----
    int have_kx = 0;
    for (;;) {
        u8 *m; int ml = tls_handshake_msg(&m, deadline);
        if (ml < 0) { tls_err_why = tls.alert >= 0 ? 2 : 3; return ml; }
        tls_hash(m, (u32)ml);
        u8 t = m[0]; u8 *b = m + 4; u32 bl = (u32)ml - 4;
        if (t == 2) {                                                         // ServerHello
            if (bl < 38 || b[0] != 3 || b[1] != 3) { tls_err_why = 2; return -11; }
            mcopy(tls.srand, b + 2, 32);
            u32 sid = b[34]; if (35 + sid + 3 > bl) { tls_err_why = 2; return -11; }
            tls.suite = (u16)be16r(b + 35 + sid);
            if (tls.suite != 0xc02f && tls.suite != 0xc02b) { tls_err_why = 2; return -12; }
        } else if (t == 12) {                                                 // ServerKeyExchange: named curve x25519 + its public key (signature not checked yet)
            if (bl < 36 || b[0] != 3 || be16r(b + 1) != 0x001d || b[3] != 32) { tls_err_why = 2; return -13; }
            mcopy(tls.spub, b + 4, 32); have_kx = 1;
        } else if (t == 13) { tls_err_why = 2; return -14; }                  // the server wants a client certificate
        else if (t == 14) break;                                              // ServerHelloDone
        // 11 = Certificate: kept for later (certificate checking is the next step)
    }
    if (!have_kx) { tls_err_why = 2; return -13; }
    // ---- our key, the shared secret, the keys ----
    u8 priv[32], pub[32], shared[32], base[32] = {9};
    wifi_random(priv);
    x25519(pub, priv, base);
    x25519(shared, priv, tls.spub);
    u32 nz = 0; for (int i = 0; i < 32; i++) nz |= shared[i];
    if (!nz) { tls_err_why = 2; return -15; }
    u8 seed[64]; mcopy(seed, tls.crand, 32); mcopy(seed + 32, tls.srand, 32);
    tls_prf(shared, 32, "master secret", seed, 64, tls.ms, 48);
    mcopy(seed, tls.srand, 32); mcopy(seed + 32, tls.crand, 32);
    u8 kb[40]; tls_prf(tls.ms, 48, "key expansion", seed, 64, kb, 40);
    gcm_init(&tls.cg, kb); gcm_init(&tls.sg, kb + 16); mcopy(tls.civ, kb + 32, 4); mcopy(tls.siv, kb + 36, 4);
    for (int i = 0; i < 32; i++) { priv[i] = 0; shared[i] = 0; }
    // ---- ClientKeyExchange, ChangeCipherSpec, Finished ----
    u8 cke[37]; cke[0] = 16; cke[1] = 0; cke[2] = 0; cke[3] = 33; cke[4] = 32; mcopy(cke + 5, pub, 32);
    tls_hash(cke, 37);
    if (tls_send(22, cke, 37)) { tls_err_why = 1; return -1; }
    u8 one = 1;
    if (tls_send(20, &one, 1)) { tls_err_why = 1; return -1; }
    tls.enc_out = 1;
    u8 h[32], fin[16]; tls_hash_now(h);
    fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 12;
    tls_prf(tls.ms, 48, "client finished", h, 32, fin + 4, 12);
    tls_hash(fin, 16);
    if (tls_send(22, fin, 16)) { tls_err_why = 1; return -1; }
    // ---- the server's ChangeCipherSpec and Finished ----
    for (;;) {
        u8 type; u8 *p; u32 rn, used;
        int r = tls_record(&type, &p, &rn, &used, deadline);
        if (r) { tls_err_why = 3; return r; }
        if (type == 21) { tls.alert = rn >= 2 ? p[1] : 0; tls_consume(used); tls_err_why = 2; return -9; }
        tls_consume(used);
        if (type == 20) { tls.enc_in = 1; break; }
    }
    u8 *m; int ml = tls_handshake_msg(&m, deadline);
    if (ml != 16 || m[0] != 20) { tls_err_why = 4; return -16; }
    tls_hash_now(h);
    u8 want[12]; tls_prf(tls.ms, 48, "server finished", h, 32, want, 12);
    if (!meq(want, m + 4, 12)) { tls_err_why = 4; return -17; }
    return 0;
}

// HTTPS GET into dst (max bytes): the body length, or <0 (same meanings as http_get, plus -20.. for TLS problems).
static int tls_insecure_shown;
static int https_get(u32 ip, u32 port, const char *host, const char *path, u8 *dst, u32 max, u32 timeout_ms) {
    tls_buffers();
    u64 hz = tick_hz(), deadline = ticks() + hz / 1000 * timeout_ms;
    tls_err_why = 0;
    tcp.dst = tls_in; tcp.dstmax = TLS_IN_MAX;
    int r = tcp_connect(ip, port);
    if (r) { tcp.state = TCP_CLOSED; tls_err_why = 1; return r == -1 ? -1 : -2; }
    r = tls_handshake(host, deadline);
    if (r) { tcp_send(4, 0, 0); tcp.state = TCP_CLOSED; return -20 - (int)tls_err_why; }
    static u8 req[400]; u32 n = 0;
    const char *parts[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nUser-Agent: wave-os\r\nAccept: text/html, text/plain, */*\r\nConnection: close\r\n\r\n"};
    for (u32 k = 0; k < 5; k++) for (u32 i = 0; parts[k][i] && n < sizeof req; i++) req[n++] = (u8)parts[k][i];
    if (tls_send(23, req, n)) { tcp.state = TCP_CLOSED; tls_err_why = 1; return -21; }
    u32 total = 0, trunc = 0;
    for (;;) {
        u8 type; u8 *p; u32 rn, used;
        r = tls_record(&type, &p, &rn, &used, deadline);
        if (r == -3 || r == -2) break;                                       // the server closed the connection: that is the end of the page
        if (r) { tcp_send(4, 0, 0); tcp.state = TCP_CLOSED; tls_err_why = r == -7 ? 5 : 3; return total ? -4 : -3; }
        if (type == 23) { u32 c = rn; if (total + c > max) { c = max - total; trunc = 1; } mcopy(dst + total, p, c); total += c; }
        tls_consume(used);
        if (type == 21) break;                                                // close_notify (or another alert): done
    }
    (void)trunc;
    tcp_send(17, 0, 0); tcp.snd_nxt++; tcp.state = TCP_CLOSED;
    u32 hdr = 0;
    for (u32 i = 0; i + 3 < total; i++) if (dst[i] == '\r' && dst[i + 1] == '\n' && dst[i + 2] == '\r' && dst[i + 3] == '\n') { hdr = i + 4; break; }
    if (!hdr || total < 12) return -4;
    http_parse_headers(dst, hdr);
    if (!http_any && http_status != 200) return -6;
    for (u32 i = hdr; i < total; i++) dst[i - hdr] = dst[i];
    return (int)(total - hdr);
}

// ---- self test (part of cryptotest) ----
static void tls_selftest(void) {
    u8 o[64]; int ok;
    static const u8 k7748[32] = {0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd, 0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18, 0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4};
    static const u8 u7748[32] = {0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c, 0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b, 0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c};
    x25519(o, k7748, u7748);
    ok = hex_eq(o, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32); puts("X25519 (RFC 7748 5.2):            "); puts(ok ? "ok\n" : "FAIL\n");
    static struct gcm g; u8 z[16] = {0}, iv[12] = {0}, tag[16];
    gcm_init(&g, z);
    gcm_crypt(&g, iv, 0, 0, z, o, 16, tag, 0);
    ok = hex_eq(o, "0388dace60b6a392f328c2b971b2fe78", 16) && hex_eq(tag, "ab6e47d42cec13bdf53a67b21257bddf", 16); puts("AES-128-GCM (NIST test case 2):   "); puts(ok ? "ok\n" : "FAIL\n");
    gcm_crypt(&g, iv, 0, 0, o, o, 16, tag, 1);
    ok = 1; for (int i = 0; i < 16; i++) if (o[i]) ok = 0; ok = ok && hex_eq(tag, "ab6e47d42cec13bdf53a67b21257bddf", 16); puts("AES-128-GCM decrypt (round trip): "); puts(ok ? "ok\n" : "FAIL\n");
    static const u8 sec[16] = {0x9b, 0xbe, 0x43, 0x6b, 0xa9, 0x40, 0xf0, 0x17, 0xb1, 0x76, 0x52, 0x84, 0x9a, 0x71, 0xdb, 0x35};
    static const u8 sd[16] = {0xa0, 0xba, 0x9f, 0x93, 0x6c, 0xda, 0x31, 0x18, 0x27, 0xa6, 0xf7, 0x96, 0xff, 0xd5, 0x19, 0x8c};
    tls_prf(sec, 16, "test label", sd, 16, o, 32);
    ok = hex_eq(o, "e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a", 32); puts("TLS 1.2 PRF (SHA-256 vector):     "); puts(ok ? "ok\n" : "FAIL\n");
}
