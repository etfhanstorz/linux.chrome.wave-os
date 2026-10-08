// Minimal line-editing shell. Input comes from input_poll(): returns a char or -1.
#ifdef QEMU
#define UART0 0x09000000UL   // PL011 on QEMU virt
static int input_poll(void) {
    if (*(volatile u32 *)(UART0 + 0x18) & (1 << 4)) return -1;   // RX FIFO empty
    return *(volatile u32 *)UART0 & 0xff;
}
static void uart_put(char c) { *(volatile u32 *)UART0 = c; }
#else
static int input_poll(void) { wdt_kick(); return kb_getc(); }   // Chrome EC keyboard (ec.h)
static void uart_put(char c) { (void)c; }
#endif

static void out(char c) { putc(c); if (c == '\n') uart_put('\r'); uart_put(c); }
static void outs(const char *s) { while (*s) out(*s++); }

static void cmd_info(void) {
    u64 el; __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    outs("EL"); out('0' + (el >> 2)); outs(", fb "); put_dec(con_fb.w); out('x'); put_dec(con_fb.h);
    outs(" @ "); put_hex(con_fb.addr); outs(", console "); put_dec(con_cols); out('x'); put_dec(con_rows); out('\n');
}
static void run_cmd(char *line) {
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    if (!*line) return;
    if (streq(line, "help")) outs("commands: help version info echo color clear errors wifi wifion wififw wifiinit wifiv wifigap wifiscan wifiscanm wifiscanblk wifiscan5 cryptotest reboot\n");
    else if (streq(line, "version")) outs(VERSION "\n");
    else if (streq(line, "info")) cmd_info();
    else if (streq(line, "echo")) { outs(arg); out('\n'); }
    else if (streq(line, "clear")) con_clear();
    else if (streq(line, "errors")) list_errors();
    else if (streq(line, "wifi")) wifi_probe();
    else if (streq(line, "wifion")) wifi_on();
    else if (streq(line, "wififw")) wifi_fw();
    else if (streq(line, "wifiinit")) wifi_init();
    else if (streq(line, "wifiscan")) { wifi_read_bytes = 1; wifi_scan(0); }
    else if (streq(line, "wifigap")) { rd_gap_us = rd_gap_us ? (rd_gap_us >= 4000 ? 0 : rd_gap_us * 2) : 250; outs("pause between read pieces (us): "); put_dec(rd_gap_us); outs("\n"); }
    else if (streq(line, "wifiv")) { wifi_verbose = !wifi_verbose; outs(wifi_verbose ? "verbose scan output on\n" : "verbose scan output off\n"); }
    else if (streq(line, "wifiscanm")) { wifi_read_bytes = 2; wifi_scan(0); }
    else if (streq(line, "wifiscanblk")) { wifi_read_bytes = 0; wifi_scan(0); }
    else if (streq(line, "wifiscan5")) { wifi_read_bytes = 1; wifi_scan(1); }
    else if (streq(line, "cryptotest")) crypto_test();
    else if (streq(line, "reboot")) { outs("rebooting\n"); reboot(); }
    else if (streq(line, "color")) {
        if (streq(arg, "white")) con_fg = 0xFFFFFF;
        else if (streq(arg, "green")) con_fg = 0x40FF40;
        else if (streq(arg, "yellow")) con_fg = 0xFFE040;
        else if (streq(arg, "cyan")) con_fg = 0x40E0FF;
        else outs("colors: white green yellow cyan\n");
    } else { outs("unknown command: "); outs(line); out('\n'); }
}

static void shell(void) {
    static char buf[128];
    u32 n = 0;
    for (;;) {
        outs("wave> "); n = 0;
        for (;;) {
            glyph(con_col, con_row, '_');
            int c;
            while ((c = input_poll()) < 0) __asm__ volatile("nop");
            glyph(con_col, con_row, ' ');
            if (c == '\r' || c == '\n') { out('\n'); break; }
            if (c == 0x7f || c == '\b') { if (n) { n--; putc('\b'); uart_put('\b'); uart_put(' '); uart_put('\b'); } }
            else if (c >= 32 && c < 127 && n < sizeof buf - 1) { buf[n++] = c; out(c); }
        }
        buf[n] = 0;
        run_cmd(buf);
    }
}
