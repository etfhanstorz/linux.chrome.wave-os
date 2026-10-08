root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# TCP: advertise a small maximum segment size so every incoming packet fits one 512-byte bus read
edit('net.h', [
("""static int tcp_send(u32 flags, const u8 *data, u32 len) {""",
"""static u32 tcp_mss = 320;                                 // largest TCP payload we ask the sender for (v1.53.5): the chip can only hand over ~512 bytes per bus read (see rdmode); `mss N` changes it
static int tcp_send(u32 flags, const u8 *data, u32 len) {"""),
("""be16w(s + 22, 1460); }""", """be16w(s + 22, tcp_mss); }"""),
])

# wifi.h: mode 2 (one multi-block transfer) was unreachable; add a data timeout setting for it
edit('wifi.h', [
("""    if (wifi_read_bytes) {
        u32 len = blocks * 256;
        int fail = 0;""",
"""    if (wifi_read_bytes == 1) {
        u32 len = blocks * 256;
        int fail = 0;"""),
("""static int sdio_read_port1(u32 addr, u8 *out, u32 blocks) {
    u32 len = blocks * 256, got = 0;""",
"""static u32 rd_dtoc;                       // data timeout counter for multi-block reads (0 = leave the controller's value; 1..255 = units of ~1M bus clocks)
static int sdio_read_port1(u32 addr, u8 *out, u32 blocks) {
    u32 len = blocks * 256, got = 0;
    if (rd_dtoc) msdc_wr(SDC_CFG, (msdc_rd(SDC_CFG) & 0x00ffffffu) | ((rd_dtoc & 0xff) << 24));"""),
])

edit('shell.h', [
("""    else if (streq(line, "rdcfg")) wifi_rdcfg(arg);""",
"""    else if (streq(line, "rdcfg")) wifi_rdcfg(arg);
    else if (streq(line, "rdmode")) wifi_rdmode(arg);
    else if (streq(line, "mss")) wifi_mss(arg);"""),
("""streq(name, "rdcfg") ||""", """streq(name, "rdcfg") || streq(name, "rdmode") || streq(name, "mss") ||"""),
("""commands: c f j k r up upset log ping rxtest rdcfg""", """commands: c f j k r up upset log ping rxtest rdcfg rdmode mss"""),
])

open(root + 'wpa.h', 'a', newline='').write("""
// rdmode [MODE [DTOC]]: how a packet is read from the chip. 1 = pieces of up to 512 bytes (only works for packets up to ~512 bytes: a second read
// restarts the packet), 2 = the whole packet in ONE multi-block transfer (what Linux does; DTOC = data timeout counter 1..255 for it), 0 = 256-byte blocks.
static int wifi_rdmode(const char *arg) {
    const char *s = arg;
    if (*s) { u32 m = parse_num(&s); wifi_read_bytes = m > 2 ? 1 : (int)m; while (*s == ' ') s++; if (*s) rd_dtoc = parse_num(&s) & 255; }
    puts("read mode "); put_dec((u32)wifi_read_bytes); puts(", data timeout "); put_dec(rd_dtoc); putc('\\n');
    sum_s("mode "); sum_u((u32)wifi_read_bytes); sum_s(" dtoc "); sum_u(rd_dtoc);
    return 1;
}
// mss [N]: the largest TCP payload we ask senders for (applies to the next connection)
static int wifi_mss(const char *arg) {
    const char *s = arg;
    if (*s) { u32 m = parse_num(&s); if (m >= 100 && m <= 1460) tcp_mss = m; }
    puts("tcp mss "); put_dec(tcp_mss); putc('\\n');
    sum_s("mss "); sum_u(tcp_mss);
    return 1;
}
""")
edit('main.c', [("wave-os v1.53.4", "wave-os v1.53.5")])
print('ok')
