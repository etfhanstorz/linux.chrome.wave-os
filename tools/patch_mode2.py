root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('join.h', [
("""static int fn1_rd_u32(""",
"""// How data packets are read from the chip. Measured on the real hana (v1.53.6 rdsweep): mode 2 = the whole packet in ONE multi-block transfer
// is the only mode that returns big packets intact (mode 1 = pieces of 512 bytes: every extra read restarts the packet, bytes after ~512 are wrong;
// mode 0 = 256-byte blocks: wrong from byte ~120). Command-port reads keep using wifi_read_bytes (mode 1 works for those small packets).
static int data_read_mode = 2;

static int fn1_rd_u32("""),
("""    if (sdio_read_port(DATA_PORT + p, dbuf, blocks)) { rx_errs++; return -1; }""",
"""    int saved_mode = wifi_read_bytes; wifi_read_bytes = data_read_mode;
    int rerr = sdio_read_port(DATA_PORT + p, dbuf, blocks);
    wifi_read_bytes = saved_mode;
    if (rerr) { rx_errs++; return -1; }"""),
])

edit('wpa.h', [
("""    if (*s) { u32 m = parse_num(&s); wifi_read_bytes = m > 2 ? 1 : (int)m; while (*s == ' ') s++; if (*s) rd_dtoc = parse_num(&s) & 255; }
    puts("read mode "); put_dec((u32)wifi_read_bytes);""",
"""    if (*s) { u32 m = parse_num(&s); data_read_mode = m > 2 ? 2 : (int)m; while (*s == ' ') s++; if (*s) rd_dtoc = parse_num(&s) & 255; }
    puts("data read mode "); put_dec((u32)data_read_mode);"""),
("""    sum_s("mode "); sum_u((u32)wifi_read_bytes);""", """    sum_s("mode "); sum_u((u32)data_read_mode);"""),
("""    int old_mode = wifi_read_bytes; u32 old_dtoc = rd_dtoc, old_div = rd_div;""",
"""    int old_mode = data_read_mode; u32 old_dtoc = rd_dtoc, old_div = rd_div;"""),
("""        wifi_read_bytes = cfg[c].mode; rd_dtoc = cfg[c].dtoc; rd_div = cfg[c].div;""",
"""        data_read_mode = cfg[c].mode; rd_dtoc = cfg[c].dtoc; rd_div = cfg[c].div;"""),
("""    wifi_read_bytes = old_mode; rd_dtoc = old_dtoc; rd_div = old_div;""",
"""    data_read_mode = old_mode; rd_dtoc = old_dtoc; rd_div = old_div;"""),
])

edit('net.h', [
("""static u32 tcp_mss = 320;                                 // largest TCP payload we ask the sender for (v1.53.5): the chip can only hand over ~512 bytes per bus read (see rdmode); `mss N` changes it""",
"""static u32 tcp_mss = 1460;                                // largest TCP payload we ask the sender for; `mss N` changes it (v1.53.5 tried 320, but Windows never sends less than 536, and v1.53.7 reads whole packets anyway)"""),
])
edit('main.c', [("wave-os v1.53.6", "wave-os v1.53.7")])
print('ok')
