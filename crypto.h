// Crypto for WPA2-PSK: SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1 (passphrase -> PMK), the 802.11i PRF (PMK -> PTK),
// AES-128 (encrypt + decrypt) and RFC 3394 AES key unwrap (the group key in handshake message 3).
// Plain C, no tables of pointers, no libc: runs with the MMU off. Tested against published vectors by `cryptotest`.

typedef unsigned int u32c;

// ---- SHA-1 ----
struct sha1 { u32c h[5]; u8 buf[64]; u32c len; u64 total; };
static u32c rol(u32c x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1_block(struct sha1 *s, const u8 *p) {
    u32c w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (u32c)p[4 * i] << 24 | (u32c)p[4 * i + 1] << 16 | (u32c)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        u32c f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32c t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(struct sha1 *s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->len = 0; s->total = 0;
}
static void sha1_update(struct sha1 *s, const u8 *d, u32c n) {
    s->total += n;
    while (n) {
        u32c take = 64 - s->len < n ? 64 - s->len : n;
        for (u32c i = 0; i < take; i++) s->buf[s->len + i] = d[i];
        s->len += take; d += take; n -= take;
        if (s->len == 64) { sha1_block(s, s->buf); s->len = 0; }
    }
}
static void sha1_final(struct sha1 *s, u8 *out) {
    u64 bits = s->total * 8;
    u8 pad = 0x80;
    sha1_update(s, &pad, 1);
    u8 z = 0;
    while (s->len != 56) sha1_update(s, &z, 1);
    u8 l[8];
    for (int i = 0; i < 8; i++) l[i] = bits >> (56 - 8 * i);
    sha1_update(s, l, 8);
    for (int i = 0; i < 5; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; }
}
static void sha1(const u8 *d, u32c n, u8 *out) { struct sha1 s; sha1_init(&s); sha1_update(&s, d, n); sha1_final(&s, out); }

// ---- HMAC-SHA1 (a context holding the keyed inner/outer states, so PBKDF2 does not re-hash the key 8192 times) ----
struct hmac { struct sha1 inner, outer; };
static void hmac_init(struct hmac *h, const u8 *key, u32c klen) {
    u8 k[64], t[20];
    for (int i = 0; i < 64; i++) k[i] = 0;
    if (klen > 64) { sha1(key, klen, t); for (int i = 0; i < 20; i++) k[i] = t[i]; }
    else for (u32c i = 0; i < klen; i++) k[i] = key[i];
    u8 ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    sha1_init(&h->inner); sha1_update(&h->inner, ip, 64);
    sha1_init(&h->outer); sha1_update(&h->outer, op, 64);
}
static void hmac_run(const struct hmac *h, const u8 *d1, u32c n1, const u8 *d2, u32c n2, u8 *out) {
    struct sha1 in = h->inner, o = h->outer;
    u8 t[20];
    sha1_update(&in, d1, n1);
    if (n2) sha1_update(&in, d2, n2);
    sha1_final(&in, t);
    sha1_update(&o, t, 20);
    sha1_final(&o, out);
}
static void hmac_sha1(const u8 *key, u32c klen, const u8 *d, u32c n, u8 *out) {
    struct hmac h; hmac_init(&h, key, klen); hmac_run(&h, d, n, 0, 0, out);
}

// ---- PBKDF2-HMAC-SHA1: WPA2 passphrase -> 256-bit PMK (ssid as salt, 4096 iterations) ----
static void pbkdf2_wpa(const char *pass, u32c plen, const u8 *ssid, u32c slen, u8 *pmk32) {
    struct hmac h; hmac_init(&h, (const u8 *)pass, plen);
    for (u32c blk = 1; blk <= 2; blk++) {
        u8 cnt[4] = {0, 0, 0, (u8)blk}, u[20], t[20];
        hmac_run(&h, ssid, slen, cnt, 4, u);
        for (int i = 0; i < 20; i++) t[i] = u[i];
        for (u32c it = 1; it < 4096; it++) {
            hmac_run(&h, u, 20, 0, 0, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        u32c n = blk == 1 ? 20 : 12;
        for (u32c i = 0; i < n; i++) pmk32[(blk - 1) * 20 + i] = t[i];
    }
}

// ---- 802.11i PRF: PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(...)) ----
static int mem_cmp(const u8 *a, const u8 *b, u32c n) { for (u32c i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static void wpa_prf(const u8 *key, u32c klen, const char *label, const u8 *data, u32c dlen, u8 *out, u32c outlen) {
    u8 buf[100]; u32c ll = 0;
    while (label[ll]) { buf[ll] = label[ll]; ll++; }
    buf[ll] = 0;
    for (u32c i = 0; i < dlen; i++) buf[ll + 1 + i] = data[i];
    u32c total = ll + 1 + dlen;
    struct hmac h; hmac_init(&h, key, klen);
    for (u32c done = 0, i = 0; done < outlen; i++) {
        buf[total] = (u8)i;
        u8 t[20];
        hmac_run(&h, buf, total + 1, 0, 0, t);
        for (u32c k = 0; k < 20 && done < outlen; k++) out[done++] = t[k];
    }
}
static void wpa_ptk(const u8 *pmk, const u8 *aa, const u8 *spa, const u8 *anonce, const u8 *snonce, u8 *ptk48) {
    u8 d[76];
    const u8 *lo = mem_cmp(aa, spa, 6) < 0 ? aa : spa, *hi = lo == aa ? spa : aa;
    for (int i = 0; i < 6; i++) { d[i] = lo[i]; d[6 + i] = hi[i]; }
    const u8 *nlo = mem_cmp(anonce, snonce, 32) < 0 ? anonce : snonce, *nhi = nlo == anonce ? snonce : anonce;
    for (int i = 0; i < 32; i++) { d[12 + i] = nlo[i]; d[44 + i] = nhi[i]; }
    wpa_prf(pmk, 32, "Pairwise key expansion", d, 76, ptk48, 48);
}

// ---- AES-128 ----
static u8 aes_sbox[256], aes_inv[256];
static int aes_ready;
static u8 xt(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static u8 gmul(u8 a, u8 b) { u8 r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
static void aes_tables(void) {                       // build the S-box instead of storing 512 bytes of tables
    if (aes_ready) return;
    u8 p = 1, q = 1;
    do {
        p = p ^ (u8)(p << 1) ^ ((p & 0x80) ? 0x1b : 0);       // p *= 3
        q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;   // q /= 3
        u8 x = q ^ (u8)((q << 1) | (q >> 7)) ^ (u8)((q << 2) | (q >> 6)) ^ (u8)((q << 3) | (q >> 5)) ^ (u8)((q << 4) | (q >> 4));
        aes_sbox[p] = x ^ 0x63;
    } while (p != 1);
    aes_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) aes_inv[aes_sbox[i]] = (u8)i;
    aes_ready = 1;
}
struct aes { u8 rk[176]; };
static void aes_key(struct aes *a, const u8 *key) {
    aes_tables();
    for (int i = 0; i < 16; i++) a->rk[i] = key[i];
    u8 rc = 1;
    for (int i = 16; i < 176; i += 4) {
        u8 t[4] = {a->rk[i - 4], a->rk[i - 3], a->rk[i - 2], a->rk[i - 1]};
        if (i % 16 == 0) {
            u8 s = t[0]; t[0] = aes_sbox[t[1]] ^ rc; t[1] = aes_sbox[t[2]]; t[2] = aes_sbox[t[3]]; t[3] = aes_sbox[s];
            rc = xt(rc);
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = a->rk[i - 16 + j] ^ t[j];
    }
}
static void aes_enc(const struct aes *a, const u8 *in, u8 *out) {
    u8 s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= 10; r++) {
        u8 t[16];
        for (int i = 0; i < 16; i++) t[i] = aes_sbox[s[i]];
        for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) s[4 * c + rr] = t[4 * ((c + rr) % 4) + rr];        // ShiftRows
        if (r != 10)
            for (int c = 0; c < 4; c++) {
                u8 *x = s + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3; x[1] = a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3;
                x[2] = a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3); x[3] = (xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * r + i];
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}
static void aes_dec(const struct aes *a, const u8 *in, u8 *out) {
    u8 s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        u8 t[16];
        for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) t[4 * ((c + rr) % 4) + rr] = s[4 * c + rr];       // InvShiftRows
        for (int i = 0; i < 16; i++) s[i] = aes_inv[t[i]] ^ a->rk[16 * r + i];
        if (r != 0)
            for (int c = 0; c < 4; c++) {
                u8 *x = s + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9); x[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                x[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11); x[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

// RFC 3394 key unwrap: `in` = (n+1) 8-byte blocks, `out` = n*8 bytes. Returns 0 if the integrity check (A = A6A6A6A6A6A6A6A6) passes.
static int aes_unwrap(const u8 *kek16, const u8 *in, u32c n, u8 *out) {
    struct aes a; aes_key(&a, kek16);
    u8 A[8], r[64 * 8], b[16];
    if (n > 64) return -1;
    for (int i = 0; i < 8; i++) A[i] = in[i];
    for (u32c i = 0; i < n * 8; i++) r[i] = in[8 + i];
    for (int j = 5; j >= 0; j--)
        for (int i = (int)n; i >= 1; i--) {
            u32c t = n * j + i;
            for (int k = 0; k < 8; k++) b[k] = A[k];
            b[7] ^= (u8)t; b[6] ^= (u8)(t >> 8);
            for (int k = 0; k < 8; k++) b[8 + k] = r[(i - 1) * 8 + k];
            aes_dec(&a, b, b);
            for (int k = 0; k < 8; k++) A[k] = b[k];
            for (int k = 0; k < 8; k++) r[(i - 1) * 8 + k] = b[8 + k];
        }
    for (int i = 0; i < 8; i++) if (A[i] != 0xA6) return -1;
    for (u32c i = 0; i < n * 8; i++) out[i] = r[i];
    return 0;
}


// ---- SHA-256 and HMAC-SHA256 (update verification: image checksum + shared-secret authentication) ----
struct sha256 { u32c h[8]; u8 buf[64]; u32c len; u64 total; };
static u32c ror(u32c x, int n) { return (x >> n) | (x << (32 - n)); }
static const u32c sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static void sha256_block(struct sha256 *s, const u8 *p) {
    u32c w[64], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 16; i++) w[i] = (u32c)p[4 * i] << 24 | (u32c)p[4 * i + 1] << 16 | (u32c)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32c s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        u32c S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + sha256_k[i] + w[i];
        u32c S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha256_init(struct sha256 *s) {
    static const u32c iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0; s->total = 0;
}
static void sha256_update(struct sha256 *s, const u8 *d, u64 n) {
    s->total += n;
    while (n) {
        if (s->len == 0 && n >= 64) { sha256_block(s, d); d += 64; n -= 64; continue; }       // whole blocks straight from the source
        u32c take = 64 - s->len < n ? 64 - s->len : (u32c)n;
        for (u32c i = 0; i < take; i++) s->buf[s->len + i] = d[i];
        s->len += take; d += take; n -= take;
        if (s->len == 64) { sha256_block(s, s->buf); s->len = 0; }
    }
}
static void sha256_final(struct sha256 *s, u8 *out) {
    u64 bits = s->total * 8; u8 pad = 0x80, z = 0, l[8];
    sha256_update(s, &pad, 1);
    while (s->len != 56) sha256_update(s, &z, 1);
    for (int i = 0; i < 8; i++) l[i] = bits >> (56 - 8 * i);
    sha256_update(s, l, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; }
}
static void sha256(const u8 *d, u64 n, u8 *out) { struct sha256 s; sha256_init(&s); sha256_update(&s, d, n); sha256_final(&s, out); }
static void hmac_sha256(const u8 *key, u32c klen, const u8 *d, u64 n, u8 *out) {
    u8 k[64], ip[64], op[64], t[32];
    for (int i = 0; i < 64; i++) k[i] = 0;
    if (klen > 64) { sha256(key, klen, t); for (int i = 0; i < 32; i++) k[i] = t[i]; } else for (u32c i = 0; i < klen; i++) k[i] = key[i];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    struct sha256 in; sha256_init(&in); sha256_update(&in, ip, 64); sha256_update(&in, d, n); sha256_final(&in, t);
    struct sha256 o; sha256_init(&o); sha256_update(&o, op, 64); sha256_update(&o, t, 32); sha256_final(&o, out);
}
// ---- self test against published vectors ----
static int hex_eq(const u8 *got, const char *hex, u32c n) {
    for (u32c i = 0; i < n; i++) {
        u8 hi = hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10, lo = hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10;
        if (got[i] != (u8)(hi << 4 | lo)) return 0;
    }
    return 1;
}
static void crypto_test(void) {
    u8 o[64], k[32];
    int ok;
    sha1((const u8 *)"abc", 3, o);
    ok = hex_eq(o, "a9993e364706816aba3e25717850c26c9cd0d89d", 20); puts("SHA-1 (FIPS 180):                 "); puts(ok ? "ok\n" : "FAIL\n");
    for (int i = 0; i < 20; i++) k[i] = 0x0b;
    hmac_sha1(k, 20, (const u8 *)"Hi There", 8, o);
    ok = hex_eq(o, "b617318655057264e28bc0b6fb378c8ef146be00", 20); puts("HMAC-SHA1 (RFC 2202 #1):          "); puts(ok ? "ok\n" : "FAIL\n");
    pbkdf2_wpa("password", 8, (const u8 *)"IEEE", 4, o);
    ok = hex_eq(o, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32); puts("PBKDF2 passphrase->PMK (802.11i): "); puts(ok ? "ok\n" : "FAIL\n");
    for (int i = 0; i < 16; i++) k[i] = (u8)i;
    u8 pt[16]; for (int i = 0; i < 16; i++) pt[i] = (u8)(i * 0x11);
    struct aes a; aes_key(&a, k);
    aes_enc(&a, pt, o);
    ok = hex_eq(o, "69c4e0d86a7b0430d8cdb78070b4c55a", 16); puts("AES-128 encrypt (FIPS 197):       "); puts(ok ? "ok\n" : "FAIL\n");
    u8 d[16]; aes_dec(&a, o, d);
    ok = 1; for (int i = 0; i < 16; i++) if (d[i] != pt[i]) ok = 0; puts("AES-128 decrypt (round trip):     "); puts(ok ? "ok\n" : "FAIL\n");
    static const u8 wrapped[24] = {0x1F, 0xA6, 0x8B, 0x0A, 0x81, 0x12, 0xB4, 0x47, 0xAE, 0xF3, 0x4B, 0xD8, 0xFB, 0x5A, 0x7B, 0x82, 0x9D, 0x3E, 0x86, 0x23, 0x71, 0xD2, 0xCF, 0xE5};
    sha256((const u8 *)"abc", 3, o);
    ok = hex_eq(o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32); puts("SHA-256 (FIPS 180):               "); puts(ok ? "ok\n" : "FAIL\n");
    for (int i = 0; i < 20; i++) k[i] = 0x0b;
    hmac_sha256(k, 20, (const u8 *)"Hi There", 8, o);
    ok = hex_eq(o, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32); puts("HMAC-SHA256 (RFC 4231 #1):        "); puts(ok ? "ok\n" : "FAIL\n");
    for (int i = 0; i < 16; i++) k[i] = (u8)i;
    int r = aes_unwrap(k, wrapped, 2, o);
    ok = r == 0 && hex_eq(o, "00112233445566778899aabbccddeeff", 16); puts("AES key unwrap (RFC 3394):        "); puts(ok ? "ok\n" : "FAIL\n");
}
