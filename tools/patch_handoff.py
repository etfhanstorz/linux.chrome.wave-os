root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('update.h', [
("""static u8 update_buf[UPDATE_TRAMP_OFF + 4096] __attribute__((aligned(4096)));""",
"""static u8 update_buf[UPDATE_TRAMP_OFF + 4096] __attribute__((aligned(4096)));

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
    puts("Wi-Fi key kept across the update: reconnecting without asking for the password\\n");
}"""),
("""    log_ship();
    delay_us(2000000);""",
"""    if (pmk_valid) {                                                   // hand the Wi-Fi key to the new image (only in the RAM copy)
        for (u32 i = 0; i < 32; i++) img[24 + i] = pmk[i];
        img[60] = PMK_MARK & 255; img[61] = PMK_MARK >> 8 & 255; img[62] = PMK_MARK >> 16 & 255; img[63] = PMK_MARK >> 24 & 255;
        puts("  Wi-Fi key handed to the new image\\n");
    }
    log_ship();
    delay_us(2000000);"""),
])

edit('wpa.h', [
("""static u8 pmk[32], ptk[48], anonce[32], snonce[32], gtk[16], gtk_id, ap_ver;""",
"""static u8 ptk[48], anonce[32], snonce[32], gtk[16], gtk_id, ap_ver;                // (pmk, pmk_valid: update.h, so they can ride over an update)"""),
("""static u32 pmk_valid; static char pmk_ssid[33];""", """static char pmk_ssid[33];"""),
("""    if (!(pmk_valid && streq(pmk_ssid, target.ssid))) {""",
"""    if (pmk_valid && pmk_any) { for (u32 i = 0; i <= sl && i < sizeof pmk_ssid; i++) pmk_ssid[i] = target.ssid[i]; pmk_any = 0; }   // a key that came over an update: it belongs to the network we just found
    if (!(pmk_valid && streq(pmk_ssid, target.ssid))) {"""),
])

edit('shell.h', [
("""    outs("  type help   c = wifichan   f = wififind   r = repeat\\n\\n"); con_fg = C_TEXT;""",
"""    outs("  type help   c = wifichan   f = wififind   r = repeat\\n\\n"); con_fg = C_TEXT;
    if (pmk_handoff) { pmk_handoff = 0; static char first[8] = "k"; con_fg = 0x40E0FF; outs("wave"); con_fg = 0x60FF80; outs("> "); con_fg = C_TEXT; outs("k\\n"); run_cmd(first); }   // after an update: reconnect on our own"""),
])
print('ok')
