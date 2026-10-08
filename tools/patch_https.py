root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# ---- TCP: remember what the server acknowledged, write a block of data and resend it if it is not acknowledged ----
edit('net.h', [
("""static struct { u32 state, lport, rport, rip, snd_nxt, rcv_nxt; u8 *dst; u32 dstmax, got, overflow, segs; u8 rmac[6]; } tcp;""",
"""static struct { u32 state, lport, rport, rip, snd_nxt, rcv_nxt, una; u8 *dst; u32 dstmax, got, overflow, segs; u8 rmac[6]; } tcp;"""),
("""    if (fl & 4) { tcp.state = TCP_RESET; return; }""",
"""    if (fl & 4) { tcp.state = TCP_RESET; return; }
    if ((fl & 16) && tcp.state != TCP_SYN_SENT && (int)(ack - tcp.una) > 0 && (int)(ack - tcp.snd_nxt) <= 0) tcp.una = ack;   // the peer has our data up to here"""),
("""        if ((fl & 18) == 18 && ack == tcp.snd_nxt + 1) { tcp.snd_nxt++; tcp.rcv_nxt = seq + 1; tcp.state = TCP_ESTAB; tcp_send(16, 0, 0); }""",
"""        if ((fl & 18) == 18 && ack == tcp.snd_nxt + 1) { tcp.snd_nxt++; tcp.una = tcp.snd_nxt; tcp.rcv_nxt = seq + 1; tcp.state = TCP_ESTAB; tcp_send(16, 0, 0); }"""),
("""// HTTP/1.0 GET into dst (max bytes). Returns the body length, or a negative error:""",
"""// Send a block of data (TLS records) and keep a copy until the peer acknowledges it; tcp_retransmit_check() sends it again after 1 s.
static u8 tcp_hold[4096]; static u32 tcp_hold_seq, tcp_hold_len, tcp_hold_tries; static u64 tcp_hold_t;
static int tcp_write(const u8 *d, u32 n) {
    if (tcp.state != TCP_ESTAB) return -1;
    if (tcp_hold_len && (int)(tcp.una - (tcp_hold_seq + tcp_hold_len)) < 0) {          // the previous block is still unacknowledged: add to it
        if (tcp_hold_len + n > sizeof tcp_hold) return -1;
        mcopy(tcp_hold + tcp_hold_len, d, n);
    } else {
        if (n > sizeof tcp_hold) return -1;
        tcp_hold_seq = tcp.snd_nxt; tcp_hold_len = 0; mcopy(tcp_hold, d, n);
    }
    u32 start = tcp_hold_len; tcp_hold_len += n;
    for (u32 off = 0; off < n; off += 1400) { u32 len = n - off < 1400 ? n - off : 1400; tcp_send(24, tcp_hold + start + off, len); tcp.snd_nxt += len; }
    tcp_hold_t = ticks(); tcp_hold_tries = 0;
    return 0;
}
static void tcp_retransmit_check(void) {
    if (!tcp_hold_len || tcp.state != TCP_ESTAB) return;
    if ((int)(tcp.una - (tcp_hold_seq + tcp_hold_len)) >= 0) { tcp_hold_len = 0; return; }   // all acknowledged
    u64 hz = tick_hz();
    if (!hz || ticks() - tcp_hold_t < hz || tcp_hold_tries >= 6) return;
    u32 from = (int)(tcp.una - tcp_hold_seq) > 0 ? tcp.una - tcp_hold_seq : 0, save = tcp.snd_nxt;
    tcp.snd_nxt = tcp_hold_seq + from;
    for (u32 off = from; off < tcp_hold_len; off += 1400) { u32 len = tcp_hold_len - off < 1400 ? tcp_hold_len - off : 1400; tcp_send(24, tcp_hold + off, len); tcp.snd_nxt += len; }
    tcp.snd_nxt = save; tcp_hold_t = ticks(); tcp_hold_tries++;
}

// HTTP/1.0 GET into dst (max bytes). Returns the body length, or a negative error:"""),
])
edit('tls.h', [
("""static int tls_send(u8 type, const u8 *d, u32 n) {
    static u8 rec[2048];
    if (n + 5 + 8 + 16 > sizeof rec) return -1;
    rec[0] = type; rec[1] = 3; rec[2] = tls.enc_out || type != 22 ? 3 : 1;     // the very first record says TLS 1.0 like everyone does""",
"""static u32 tls_sent;
static int tls_send(u8 type, const u8 *d, u32 n) {
    static u8 rec[2048];
    if (n + 5 + 8 + 16 > sizeof rec) return -1;
    rec[0] = type; rec[1] = 3; rec[2] = tls_sent++ ? 3 : 1;                 // the very first record says TLS 1.0 like everyone does"""),
("""    tls.enc_in = tls.enc_out = 0; tls.cseq = tls.sseq = 0; tls.alert = -1; tls_hs_len = tls_hs_pos = 0;""",
"""    tls.enc_in = tls.enc_out = 0; tls.cseq = tls.sseq = 0; tls.alert = -1; tls_hs_len = tls_hs_pos = 0; tls_sent = 0;"""),
("""static void tls_hash_now(u8 *out) { struct sha256 c = tls.hs; sha256_final(&c, out); }""",
"""static void tls_hash_now(u8 *out) {                                         // the hash so far, without ending it (byte copy: a struct copy would need memcpy)
    struct sha256 c; volatile u8 *d = (volatile u8 *)&c; const u8 *s = (const u8 *)&tls.hs;
    for (u32 i = 0; i < sizeof c; i++) d[i] = s[i];
    sha256_final(&c, out);
}"""),
])

edit('main.c', [("""#include "bar.h\"""", """#include "bar.h"
#include "tls.h\"""")])

# ---- the browser: https:// pages ----
edit('web.h', [
("""        if (u.https) {
            errs("NET", 31, 2, "this address needs HTTPS (encrypted web), which wave-os cannot do yet");
            web_error_page("HTTPS is not supported yet", "This page needs an encrypted connection (https://). Wave-os cannot do that yet.", "Plain http:// pages work. HTTPS is the next big step.");
            return 0;
        }
""", ""),
("""        web_status("loading...");
        http_any = 1; http_status = 0;
        int n = http_get(ip, u.port, u.host, u.path, web_raw, WEB_RAW_MAX, 25000);
        http_any = 0;
        if (n < 0) {""",
"""        web_status(u.https ? "connecting securely..." : "loading...");
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
        if (n < 0) {"""),
("""static char web_url[300];                                    // the page being shown""",
"""static char web_url[300];                                    // the page being shown
static u32 web_secure;                                       // the page came over https (encrypted; the certificate is not checked yet)"""),
("""    for (u32 i = 0; web_url[i] && n < sizeof b - 30; i++) b[n++] = web_url[i];""",
"""    for (u32 i = 0; web_url[i] && n < sizeof b - 60; i++) b[n++] = web_url[i];
    if (web_secure) { const char *s = "  (encrypted, not verified)"; for (u32 i = 0; s[i] && n < sizeof b - 30; i++) b[n++] = s[i]; }"""),
])
edit('shell.h', [("""    else if (streq(line, "cryptotest")) crypto_test();""", """    else if (streq(line, "cryptotest")) { crypto_test(); tls_selftest(); }""")])
s = open(root + 'version.h').read().replace('#define WAVE_VERSION "1.6"\n#define WAVE_PATCH   "-005"', '#define WAVE_VERSION "1.7"\n#define WAVE_PATCH   ""')
open(root + 'version.h', 'w', newline='').write(s)
print('ok')
