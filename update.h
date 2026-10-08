// Self-update over the network (stage 1 "netboot"): fetch /manifest and /Image from the PC, check the size, SHA-256
// and a keyed signature (HMAC-SHA256 with the key in update_key.h), and only then jump into the downloaded image.
// The image runs from RAM; nothing is written to any storage, so a bad update is fixed by rebooting.
#include "update_key.h"
#include "pc_addr.h"                                  // PC_ADDR_DEFAULT: this PC's address when the image was built

#define UPDATE_ADDR 0x70000000UL                      // free RAM (the framebuffer, ramoops, DTB and our own image are elsewhere)
#define UPDATE_MAX  (6u * 1024 * 1024)
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
    u8 *img = (u8 *)UPDATE_ADDR;
    u64 t0 = ticks();
    n = http_get(update_srv, 8000, host, "/Image", img, UPDATE_MAX, 60000);
    if (n < 0) { puts("  image download failed ("); put_dec((u64)-n); puts(")\n"); err("NET", 23, "update: could not download the image"); return 0; }
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
    delay_us(2000000);
    // Hand over: invalidate the instruction cache (we wrote the new code through the data side), then jump with the
    // device tree pointer in x0, exactly as the firmware did for us.
    __asm__ volatile("mov x0, %0\n\tic iallu\n\tdsb sy\n\tisb\n\tbr %1" :: "r"(boot_dtb), "r"(UPDATE_ADDR) : "x0", "memory");
    return 1;
}