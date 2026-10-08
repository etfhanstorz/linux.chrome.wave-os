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
static void run_cmd2(char *line, char *arg);
static void run_cmd(char *line) {
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    if (!*line) return;
    static char al_arg[24];                                            // short aliases for the long test commands: c = wifichan 157 HomeWifi, f = wififind HomeWifi
    if (streq(line, "c")) { line = "wifichan"; const char *a = "157 HomeWifi"; u32 q = 0; while ((al_arg[q] = a[q])) q++; arg = al_arg; }
    else if (streq(line, "k")) { line = "wificonnect"; const char *a = "HomeWifi"; u32 q = 0; while ((al_arg[q] = a[q])) q++; arg = al_arg; }
    else if (streq(line, "j")) { line = "wifijoin"; const char *a = "HomeWifi"; u32 q = 0; while ((al_arg[q] = a[q])) q++; arg = al_arg; }
    else if (streq(line, "f")) { line = "wififind"; const char *a = "HomeWifi"; u32 q = 0; while ((al_arg[q] = a[q])) q++; arg = al_arg; }
    char name[16]; { u32 q = 0; while (line[q] && q < 15) { name[q] = line[q]; q++; } name[q] = 0; }
    if (!streq(name, "prof")) prof_reset();
    sum_reset();
    u64 t_cmd = ticks();
    run_cmd2(line, arg);
    if (!streq(name, "prof")) prof_last_total = ticks() - t_cmd;
    if ((name[0] == 'w' && name[1] == 'i') || streq(name, "update") || streq(name, "cryptotest") || streq(name, "netdemo") || streq(name, "ping") || streq(name, "rxtest") || streq(name, "rdcfg") || streq(name, "rdmode") || streq(name, "rdsweep") || streq(name, "mss") || streq(name, "log") || streq(name, "up") || streq(name, "upset")) {   // one-line summary: command, what it says, error codes
        u32 keep = con_fg;                                            // coloured: green = fine, red = errors
        con_fg = cmd_codes_n ? 0xFF6060 : 0x60FF80; outs("> "); con_fg = 0x40E0FF; outs(name); out(' ');
        con_fg = cmd_codes_n ? 0xFFE040 : C_TEXT;
        outs(sum_res[0] ? sum_res : cmd_codes_n ? "FAIL" : "ok");
        if (cmd_codes_n) { con_fg = 0xFF6060; outs(" e"); outs(cmd_codes); }
        con_fg = 0x808090; out(' '); put_secs(ms_of(prof_last_total)); out('s');       // how long the command took
        out('\n'); con_fg = keep;
    }
    if (!streq(name, "log")) log_ship();                          // send what this command printed to the PC (log.h)
}
static void run_cmd2(char *line, char *arg) {
    if (streq(line, "help")) outs("commands: c f j k r up upset log ping rxtest rdcfg rdmode rdsweep mss opt prof wifi5 wifijoin wificonnect help version info echo color clear errors wifi wifion wififw wifiinit netdemo update wifichan wififind wifitry wifiv wifigap wifidiv wifiscan wifiscanm wifiscanblk wifiscan5 cryptotest reboot\n");
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
    else if (streq(line, "wifidiv")) { rd_div = rd_div == 0 ? 8 : rd_div >= 64 ? 0 : rd_div * 2; outs("big-read clock divider: "); put_dec(rd_div); outs(rd_div ? "\n" : " (unchanged)\n"); }
    else if (streq(line, "netdemo")) net_demo();
    else if (streq(line, "up") || streq(line, "update")) wave_update(arg);
    else if (streq(line, "upset")) wave_upset(arg);
    else if (streq(line, "wifichan")) wifi_chan(arg);
    else if (streq(line, "wifi5")) wifi_5g(arg);
    else if (streq(line, "wifijoin")) wifi_join(*arg ? arg : "HomeWifi");
    else if (streq(line, "wificonnect")) wifi_connect(*arg ? arg : "HomeWifi");
    else if (streq(line, "ping")) wifi_ping(arg);
    else if (streq(line, "rxtest")) wifi_rxtest();
    else if (streq(line, "rdcfg")) wifi_rdcfg(arg);
    else if (streq(line, "rdmode")) wifi_rdmode(arg);
    else if (streq(line, "rdsweep")) wifi_rdsweep(arg);
    else if (streq(line, "mss")) wifi_mss(arg);
    else if (streq(line, "log")) wave_log(arg);
    else if (streq(line, "opt")) { if (*arg >= '0' && *arg <= '9') opt_toggle(*arg - '0'); opt_list(); }
    else if (streq(line, "prof")) {
        outs("P "); u64 tot = 0;
        for (u32 q = 0; q < prof_n; q++) { outs(prof_spans[q].name); put_secs(ms_of(prof_spans[q].t)); out(' '); tot += prof_spans[q].t; }
        outs("idle "); put_secs(ms_of(prof_idle)); outs(" of "); put_secs(ms_of(prof_last_total)); outs("s"); out('\n');
    }
    else if (streq(line, "wififind")) wifi_find(arg);
    else if (streq(line, "wifitry")) wifi_try();
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
    con_fg = 0x40E0FF; outs("\n  ~~~~  "); con_fg = C_TEXT; outs(VERSION); con_fg = 0x40E0FF; outs("  ~~~~\n"); con_fg = 0x808090;
    outs("  type help   c = wifichan   f = wififind   r = repeat\n\n"); con_fg = C_TEXT;
    for (;;) {
        con_fg = 0x40E0FF; outs("wave"); con_fg = 0x60FF80; outs("> "); con_fg = C_TEXT; n = 0;
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
        static char last[128];                                          // r = run the previous command again
        if (buf[0] == 'r' && !buf[1] && last[0]) { u32 q = 0; while ((buf[q] = last[q])) q++; outs(buf); out('\n'); }
        else if (buf[0]) { u32 q = 0; while ((last[q] = buf[q])) q++; }
        run_cmd(buf);
    }
}
