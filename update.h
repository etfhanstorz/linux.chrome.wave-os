// Self-update over the network (stage 1 "netboot"): fetch /manifest and /Image from the PC, check the size, SHA-256
// and a keyed signature (HMAC-SHA256 with the key in update_key.h), and only then jump into the downloaded image.
// The image runs from RAM; nothing is written to any storage, so a bad update is fixed by rebooting.
#include "update_key.h"
#include "pc_addr.h"                                  // PC_ADDR_DEFAULT: this PC's address when the image was built

// The download goes into a buffer inside our own image (BSS), which is guaranteed to be real RAM that the firmware gave us. (v1.53.7 used a fixed
// address, 0x70000000, and the real Chromebook froze right after "new image": that address is probably not usable.) After the checks, a small
// routine copies the new image over this one and starts it from the same address, like a fresh boot.
#define UPDATE_MAX  (2u * 1024 * 1024)
#define UPDATE_TRAMP_OFF (2u * 1024 * 1024)           // the copy routine is parked here, above the image, while it overwrites us
static u8 update_buf[UPDATE_TRAMP_OFF + 4096] __attribute__((aligned(4096)));

// Wi-Fi key hand-over (v1.53.9): the derived key (never the password) rides over an update in the new image's header bytes 24..55 (unused fields),
// with the marker 'PMK1' at byte 60. The new image reads it at boot, wipes it, and reconnects by itself. (The old build writes it into the downloaded
// copy in RAM only; the file on the PC is never touched.)
static u8 pmk[32]; static u32 pmk_valid, pmk_any, pmk_handoff;       // pmk_any: the key came over an update, the network name is not known yet
extern const u8 _start[] __attribute__((visibility("hidden")));
#define PMK_MARK 0x314b4d50u                                         // 'PMK1'
static void pmk_restore(void) {
    volatile u8 *h = (volatile u8 *)_start;
    u32 mark = h[60] | h[61] << 8 | h[62] << 16 | (u32)h[63] << 24;
    if (mark != PMK_MARK) return;
    for (u32 i = 0; i < 32; i++) { pmk[i] = h[24 + i]; h[24 + i] = 0; }
    h[60] = h[61] = h[62] = h[63] = 0;
    pmk_valid = 1; pmk_any = 1; pmk_handoff = 1;
    puts("Wi-Fi key kept across the update: reconnecting without asking for the password\n");
}
extern const u8 update_tramp[] __attribute__((visibility("hidden")));
extern const u8 update_tramp_end[] __attribute__((visibility("hidden")));
static int log_ship(void);                            // log.h: send what has been printed to the PC
static void update_progress(void) {                   // called every 2 s while the image downloads: show it, and send the log so a stall is visible on the PC
    puts("  ... "); put_dec(tcp.got); puts(" bytes, "); put_dec(tcp.segs); puts(" segments, damaged dropped "); put_dec(net_bad_ip + net_bad_l4); putc('\n');
    log_ship();
}
static const u8 *boot_dtb;                            // the device tree the firmware gave us: handed on to the new image
static u32 update_srv = PC_ADDR_DEFAULT;              // where `up` fetches the new build from: the PC's address at build time; `upset ADDRESS` changes it (until the next reboot)

static int parse_ip(const char *s, u32 *out) {
    u32 v[4] = {0, 0, 0, 0}, k = 0;
    for (; *s && k < 4; s++) { if (*s == '.') k++; else if (*s >= '0' && *s <= '9') v[k] = v[k] * 10 + (*s - '0'); else return 0; }
    if (k != 3) return 0;
    *out = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
    return 1;
}
static u32 hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
static void hex_to_bytes(const char *h, u8 *out, u32 n) { for (u32 i = 0; i < n; i++) out[i] = hexv(h[2 * i]) << 4 | hexv(h[2 * i + 1]); }
static void put_hex_bytes(const u8 *b, u32 n) { for (u32 i = 0; i < n; i++) { putc("0123456789abcdef"[b[i] >> 4]); putc("0123456789abcdef"[b[i] & 15]); } }

// upset [ADDRESS]: show or change the PC address `up` uses
static int wave_upset(const char *arg) {
    if (arg && arg[0]) { u32 a = 0; if (!parse_ip(arg, &a) || !a) { sum_s("bad address"); return 0; } update_srv = a; }
    puts("up fetches updates from "); if (update_srv) put_ip(update_srv); else puts("(unknown: set it with  upset ADDRESS)"); putc('\n');
    if (update_srv) { sum_s("pc "); sum_u(update_srv >> 24); sum_c('.'); sum_u(update_srv >> 16 & 255); sum_c('.'); sum_u(update_srv >> 8 & 255); sum_c('.'); sum_u(update_srv & 255); }
    return 1;
}

static int wave_update(const char *arg) {
    if (arg && arg[0] && !parse_ip(arg, &update_srv)) { puts("usage: up   (or up ADDRESS, or upset ADDRESS to change the saved address)\n"); return 0; }
    if (!net_ip) { puts("the network is not up yet: run k first\n"); return 0; }
    if (!update_srv) { puts("no PC address saved: type  upset ADDRESS  (the PC's address, like 192.168.1.50)\n"); return 0; }
    static char host[20]; u32 hn = 0;
    for (int sh = 24; sh >= 0; sh -= 8) { u32 b = update_srv >> sh & 255; if (b >= 100) host[hn++] = '0' + b / 100; if (b >= 10) host[hn++] = '0' + b / 10 % 10; host[hn++] = '0' + b % 10; if (sh) host[hn++] = '.'; }
    host[hn] = 0;
    static u8 man[300];
    puts("update: asking "); put_ip(update_srv); puts(" for the manifest\n");
    int n = http_get(update_srv, 8000, host, "/manifest", man, sizeof man - 1, 6000);
    if (n < 100) { puts("  manifest download failed ("); put_dec((u64)(n < 0 ? -n : n)); puts(")\n"); err("NET", 20 + (n < 0 ? 0 : 0), "update: could not download the manifest"); return 0; }
    man[n] = 0;
    u32 size = 0, i = 0;
    while (man[i] >= '0' && man[i] <= '9') size = size * 10 + (man[i++] - '0');
    if (man[i++] != ' ' || n < (int)(i + 64 + 1 + 64)) { err("NET", 21, "update: the manifest is malformed"); return 0; }
    u8 want_sha[32], want_mac[32];
    hex_to_bytes((const char *)man + i, want_sha, 32);
    hex_to_bytes((const char *)man + i + 65, want_mac, 32);
    puts("  new image: "); put_dec(size); puts(" bytes\n");
    if (size < 4096 || size > UPDATE_MAX) { err("NET", 22, "update: the image size is not plausible"); return 0; }
    u8 *img = update_buf;
    puts("  download buffer at "); put_hex((u64)img); putc('\n');
    log_ship();
    u64 t0 = ticks();
    net_idle_hook = update_progress;
    n = http_get(update_srv, 8000, host, "/Image", img, UPDATE_MAX, 60000);
    net_idle_hook = 0;
    if (n < 0) {
        puts("  image download failed ("); put_dec((u64)-n); puts(")\n");
        puts("  got "); put_dec(tcp.got); puts(" of "); put_dec(size); puts(" bytes in "); put_dec(tcp.segs); puts(" segments; damaged packets dropped: "); put_dec(net_bad_ip + net_bad_l4); puts("\n");
        err("NET", 23, "update: could not download the image"); return 0;
    }
    puts("  downloaded "); put_dec((u64)n); puts(" bytes in "); put_dec((ticks() - t0) * 1000 / tick_hz()); puts(" ms, damaged packets dropped: "); put_dec(net_bad_ip + net_bad_l4); putc('\n');
    if ((u32)n != size) { err("NET", 24, "update: the image size does not match the manifest"); return 0; }
    u8 sha[32], mac[32];
    sha256(img, size, sha);
    hmac_sha256(update_key, sizeof update_key, img, size, mac);
    puts("  sha-256 "); put_hex_bytes(sha, 8); puts("...  signature ");
    if (!meq(sha, want_sha, 32)) { puts("\n"); err("NET", 25, "update: the checksum does not match: download is damaged"); return 0; }
    if (!meq(mac, want_mac, 32)) { puts("BAD\n"); err("NET", 26, "update: the signature is wrong: not signed with our key, refusing"); return 0; }
    puts("ok\n");
    if (img[56] != 'A' || img[57] != 'R' || img[58] != 'M' || img[59] != 0x64) { err("NET", 27, "update: not a valid arm64 image"); return 0; }
    puts("  verified. starting the new wave-os from RAM...\n");
    if (pmk_valid) {                                                   // hand the Wi-Fi key to the new image (only in the RAM copy)
        for (u32 i = 0; i < 32; i++) img[24 + i] = pmk[i];
        img[60] = PMK_MARK & 255; img[61] = PMK_MARK >> 8 & 255; img[62] = PMK_MARK >> 16 & 255; img[63] = PMK_MARK >> 24 & 255;
        puts("  Wi-Fi key handed to the new image\n");
    }
    log_ship();
    delay_us(2000000);
    // Hand over: park the copy routine above the downloaded image, then run it: it copies the image over ours and starts it from our own load
    // address with the device tree pointer in x0, exactly as the firmware did for us.
    u8 *tr = update_buf + UPDATE_TRAMP_OFF;
    for (u32 i = 0; i < (u32)(update_tramp_end - update_tramp); i++) tr[i] = update_tramp[i];
    u64 base; __asm__ volatile("adrp %0, _start\n\tadd %0, %0, :lo12:_start" : "=r"(base));
    u64 size8 = ((u64)size + 7) & ~7ull;
    __asm__ volatile("mov x0, %0\n\tmov x1, %1\n\tmov x2, %2\n\tmov x3, %3\n\tic iallu\n\tdsb sy\n\tisb\n\tbr %4"
                     :: "r"((u64)boot_dtb), "r"((u64)img), "r"(base), "r"(size8), "r"((u64)tr) : "x0", "x1", "x2", "x3", "x5", "memory");
    return 1;
}