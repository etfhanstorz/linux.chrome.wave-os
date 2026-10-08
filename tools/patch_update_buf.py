root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# start.S: the copy-and-start routine
s = open(root + 'start.S', newline='').read().replace('\r\n', '\n')
if 'update_tramp:' not in s:
    s += """
// ---- update hand-over (v1.53.8): copy a downloaded image over this one and start it, exactly like a fresh boot ----
// x0 = DTB, x1 = source, x2 = destination (our own load address), x3 = size in bytes (multiple of 8).
// Position independent: update.h copies these bytes into the RAM buffer first, because this code itself gets overwritten by the copy.
    .section .text, "ax"
    .balign 16
    .global update_tramp
    .global update_tramp_end
update_tramp:
    mov     x5, x2
1:  ldr     x4, [x1], #8
    str     x4, [x2], #8
    subs    x3, x3, #8
    b.hi    1b
    ic      iallu
    dsb     sy
    isb
    br      x5
update_tramp_end:
    nop
"""
    open(root + 'start.S', 'w', newline='').write(s)

edit('update.h', [
("""#define UPDATE_ADDR 0x70000000UL                      // free RAM (the framebuffer, ramoops, DTB and our own image are elsewhere)
#define UPDATE_MAX  (6u * 1024 * 1024)""",
"""// The download goes into a buffer inside our own image (BSS), which is guaranteed to be real RAM that the firmware gave us. (v1.53.7 used a fixed
// address, 0x70000000, and the real Chromebook froze right after "new image": that address is probably not usable.) After the checks, a small
// routine copies the new image over this one and starts it from the same address, like a fresh boot.
#define UPDATE_MAX  (2u * 1024 * 1024)
#define UPDATE_TRAMP_OFF (2u * 1024 * 1024)           // the copy routine is parked here, above the image, while it overwrites us
static u8 update_buf[UPDATE_TRAMP_OFF + 4096] __attribute__((aligned(4096)));
extern const u8 update_tramp[] __attribute__((visibility("hidden")));
extern const u8 update_tramp_end[] __attribute__((visibility("hidden")));
static int log_ship(void);                            // log.h: send what has been printed to the PC
static void update_progress(void) {                   // called every 2 s while the image downloads: show it, and send the log so a stall is visible on the PC
    puts("  ... "); put_dec(tcp.got); puts(" bytes, "); put_dec(tcp.segs); puts(" segments, damaged dropped "); put_dec(net_bad_ip + net_bad_l4); putc('\\n');
    log_ship();
}"""),
("""    u8 *img = (u8 *)UPDATE_ADDR;
    u64 t0 = ticks();
    n = http_get(update_srv, 8000, host, "/Image", img, UPDATE_MAX, 60000);""",
"""    u8 *img = update_buf;
    puts("  download buffer at "); put_hex((u64)img); putc('\\n');
    log_ship();
    u64 t0 = ticks();
    net_idle_hook = update_progress;
    n = http_get(update_srv, 8000, host, "/Image", img, UPDATE_MAX, 60000);
    net_idle_hook = 0;"""),
("""    delay_us(2000000);
    // Hand over: invalidate the instruction cache (we wrote the new code through the data side), then jump with the
    // device tree pointer in x0, exactly as the firmware did for us.
    __asm__ volatile("mov x0, %0\\n\\tic iallu\\n\\tdsb sy\\n\\tisb\\n\\tbr %1" :: "r"(boot_dtb), "r"(UPDATE_ADDR) : "x0", "memory");""",
"""    log_ship();
    delay_us(2000000);
    // Hand over: park the copy routine above the downloaded image, then run it: it copies the image over ours and starts it from our own load
    // address with the device tree pointer in x0, exactly as the firmware did for us.
    u8 *tr = update_buf + UPDATE_TRAMP_OFF;
    for (u32 i = 0; i < (u32)(update_tramp_end - update_tramp); i++) tr[i] = update_tramp[i];
    u64 base; __asm__ volatile("adrp %0, _start\\n\\tadd %0, %0, :lo12:_start" : "=r"(base));
    u64 size8 = ((u64)size + 7) & ~7ull;
    __asm__ volatile("mov x0, %0\\n\\tmov x1, %1\\n\\tmov x2, %2\\n\\tmov x3, %3\\n\\tic iallu\\n\\tdsb sy\\n\\tisb\\n\\tbr %4"
                     :: "r"((u64)boot_dtb), "r"((u64)img), "r"(base), "r"(size8), "r"((u64)tr) : "x0", "x1", "x2", "x3", "x5", "memory");"""),
])

edit('net.h', [
("""static u32 tcp_mss = 1460;""",
"""static void (*net_idle_hook)(void);                       // called about every 2 s while a download waits (update.h shows progress and ships the log)
static u32 tcp_mss = 1460;"""),
("""    u64 hz = tick_hz(), t0 = ticks(), last = t0; u32 last_got = 0;
    while (tcp.state == TCP_ESTAB && hz && ticks() - t0 < hz / 1000 * timeout_ms) {
        net_poll(); wdt_kick();""",
"""    u64 hz = tick_hz(), t0 = ticks(), last = t0, hook_at = t0; u32 last_got = 0;
    while (tcp.state == TCP_ESTAB && hz && ticks() - t0 < hz / 1000 * timeout_ms) {
        net_poll(); wdt_kick();
        if (net_idle_hook && ticks() - hook_at > hz * 2) { hook_at = ticks(); net_idle_hook(); }"""),
])
edit('main.c', [("wave-os v1.53.7", "wave-os v1.53.8")])
print('ok')
