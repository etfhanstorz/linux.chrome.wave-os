// Status bar (v1.6-002): the top row shows the date and time (MST), a Wi-Fi symbol (green = connected), the battery and the version.
//   Time: the Chromebook's clock chip (MT6397 PMIC RTC, kept in UTC by ChromeOS) at boot, corrected from the internet (NTP) once Wi-Fi is up.
//   Battery: the Chrome EC's battery figures (EC_CMD_READ_MEMMAP).
// The shell keeps the row for itself (console.h con_top = 1); the browser draws the right half of it into its own title bar.

// Mountain time: MST = UTC-7, and MDT = UTC-6 from the second Sunday in March (2:00) to the first Sunday in November (2:00), US rules.
// (v1.6-002 used MST all year and was an hour off in summer.)

static u32 time_src;                                           // 0 = unknown, 1 = clock chip, 2 = internet
static long long time_base;                                    // unix seconds at time_tick
static u64 time_tick;
static long long now_unix(void) { u64 hz = tick_hz(); return time_src && hz ? time_base + (long long)((ticks() - time_tick) / hz) : 0; }
static void time_set(long long unix_s, u32 src) { time_base = unix_s; time_tick = ticks(); time_src = src; }

static long long days_from_civil(int y, int m, int d) {          // Howard Hinnant's algorithm
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(long long z, int *y, int *m, int *d);
static long long nth_sunday(int y, int m, int nth) {                 // day number of the nth Sunday of a month
    long long d1 = days_from_civil(y, m, 1);
    int wd = (int)(((d1 + 4) % 7 + 7) % 7);                           // 0 = Sunday (1970-01-01 was a Thursday)
    return d1 + (7 - wd) % 7 + 7 * (nth - 1);
}
static int tz_dst(long long utc) {
    int y, m, d; civil_from_days(utc / 86400, &y, &m, &d);
    long long start = nth_sunday(y, 3, 2) * 86400 + 9 * 3600;         // 2:00 MST = 09:00 UTC
    long long end = nth_sunday(y, 11, 1) * 86400 + 8 * 3600;          // 2:00 MDT = 08:00 UTC
    return utc >= start && utc < end;
}
static void civil_from_days(long long z, int *y, int *m, int *d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

// The PMIC clock (registers from Linux drivers/rtc/rtc-mt6397.c: base 0xe000, year counted from 1968).
static void rtc_read(void) {
    u32 v[7];
    static const u16 regs[7] = {0xe00a, 0xe00c, 0xe00e, 0xe010, 0xe012, 0xe014, 0xe016};   // sec min hour day weekday month year
    for (u32 i = 0; i < 7; i++) if (pmic_read(regs[i], &v[i])) return;
    int sec = v[0] & 0x3f, min = v[1] & 0x3f, hour = v[2] & 0x1f, day = v[3] & 0x1f, mon = v[5] & 0x0f, year = (int)(v[6] & 0x7f) + 1968;
    if (year < 2024 || year > 2099 || mon < 1 || mon > 12 || day < 1 || hour > 23 || min > 59 || sec > 59) return;
    time_set(days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec, 1);
}

// SNTP: one question to pool.ntp.org, answer = seconds since 1900 at offset 40.
static u32 ntp_tries; static u64 ntp_next;
static int ntp_sync(void) {
    u32 ip; if (dns_lookup("pool.ntp.org", &ip)) return 0;
    static u8 q[48]; mset(q, 0, 48); q[0] = 0x23;                 // version 4, client
    udp_listen_port = 49123;
    for (int attempt = 0; attempt < 2; attempt++) {
        udp_in_got = 0;
        if (net_udp(ip, 49123, 123, q, 48)) continue;
        net_wait_ms(1500, (int *)&udp_in_got);
        if (udp_in_got && udp_in_len >= 48 && (udp_in[0] & 7) == 4) {
            u32 secs = be32r(udp_in + 40);
            udp_listen_port = 0;
            if (secs > 2208988800u) { time_set((long long)secs - 2208988800LL, 2); return 1; }
            return 0;
        }
    }
    udp_listen_port = 0;
    return 0;
}

// Battery from the EC's memory map: capacity left (0x48) / last full capacity (0x58), flags at 0x4c (1 AC, 2 battery present, 8 charging).
static int bat_pct = -1, bat_charging, bat_unsupported;
static void bat_read(void) {
    if (bat_unsupported) return;
    u8 p[2] = {0x40, 32}, r[32]; u16 n = 0;
    int rc = ec_cmd(0x0007, 0, p, 2, r, sizeof r, &n);
    if (rc > 0) { bat_unsupported = 1; bat_pct = -1; return; }
    if (rc || n < 32) { bat_pct = -1; return; }
    u32 cap = r[8] | r[9] << 8 | (u32)r[10] << 16 | (u32)r[11] << 24, flag = r[12], full = r[24] | r[25] << 8 | (u32)r[26] << 16 | (u32)r[27] << 24;
    if (!(flag & 2) || !full) { bat_pct = -1; return; }
    u32 pct = cap * 100 / full; if (pct > 100) pct = 100;
    bat_pct = (int)pct; bat_charging = (flag & 8) || (flag & 1);
}

// ---- drawing ----
static int bar_dirty = 1;                                      // redraw at the next tick even if nothing changed (after a screen clear)
static char bar_last[96];
static void bar_cell(u32 x, char c, u32 fg, u32 bg) { u32 sf = con_fg, sb = con_bg; con_fg = fg; con_bg = bg; glyph(x, 0, c); con_fg = sf; con_bg = sb; }
static void bar_wifi_icon(u32 x, u32 on, u32 bg) {             // three arcs and a dot, drawn into one character cell
    static const char *pic[12] = {
        "...######...", ".##......##.", "#..........#", "...######...", "..#......#..", ".#........#.",
        "....####....", "...#....#...", "............", ".....##.....", ".....##.....", "............" };
    u32 fg = on ? 0x60FF80 : 0x5A6272;
    for (u32 yy = 0; yy < CH; yy++) for (u32 xx = 0; xx < CW; xx++) {
        u32 r = yy >= 6 && yy < 18 ? yy - 6 : 99;
        px(x * CW + xx, yy, (r < 12 && pic[r][xx] == '#') ? fg : bg);
    }
}
static void bar_append(char *b, u32 *n, const char *s) { while (*s && *n < 94) b[(*n)++] = *s++; }
static void bar_num(char *b, u32 *n, u32 v, u32 width) { char t[10]; u32 k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k < width) t[k++] = '0'; while (k && *n < 94) b[(*n)++] = t[--k]; }
static void bar_text(char *b) {
    static const char *dn[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    static const char *mn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    u32 n = 0;
    long long t = now_unix();
    if (t) {
        int dst = tz_dst(t);
        long long lt = t + (dst ? -6 : -7) * 3600, days = lt >= 0 ? lt / 86400 : (lt - 86399) / 86400;
        int y, m, d; civil_from_days(days, &y, &m, &d);
        u32 sod = (u32)(lt - days * 86400), hh = sod / 3600, mm = sod / 60 % 60;
        bar_append(b, &n, dn[(u32)((days % 7 + 7) % 7)]); bar_append(b, &n, " "); bar_append(b, &n, mn[m - 1]); bar_append(b, &n, " "); bar_num(b, &n, (u32)d, 1);
        bar_append(b, &n, " "); bar_num(b, &n, (u32)y, 4); bar_append(b, &n, "  ");
        bar_num(b, &n, hh % 12 ? hh % 12 : 12, 1); bar_append(b, &n, ":"); bar_num(b, &n, mm, 2); bar_append(b, &n, hh < 12 ? " AM " : " PM "); bar_append(b, &n, dst ? "MDT" : "MST");
    } else bar_append(b, &n, "--:--");
    bar_append(b, &n, "  \x01 ");                               // \x01 = where the Wi-Fi symbol goes
    if (bat_pct >= 0) { bar_num(b, &n, (u32)bat_pct, 1); bar_append(b, &n, bat_charging ? "%+" : "%"); } else bar_append(b, &n, "--%");
    bar_append(b, &n, "  v" WAVE_VERSION WAVE_PATCH " ");
    b[n] = 0;
}
// Draw the right part of row 0 (and, for the shell, fill the left part too).
static void bar_draw(int browser) {
    char b[96]; bar_text(b);
    u32 n = 0; while (b[n]) n++;
    u32 bg = browser ? 0x1F3A5F : 0x1A2433, x0 = con_cols > n ? con_cols - n : 0;
    if (!browser) { const char *l = " wave-os"; u32 x = 0; for (; l[x] && x < x0; x++) bar_cell(x, l[x], 0x40E0FF, bg); for (; x < x0; x++) bar_cell(x, ' ', C_TEXT, bg); }
    for (u32 i = 0; i < n && x0 + i < con_cols; i++) {
        if (b[i] == '\x01') bar_wifi_icon(x0 + i, wifi_up, bg);
        else bar_cell(x0 + i, b[i], i < 30 && time_src != 2 && b[0] == '-' ? 0x808090 : 0xE0E6F0, bg);
    }
    for (u32 i = 0; i < sizeof bar_last; i++) { bar_last[i] = b[i]; if (!b[i]) break; }
    bar_last[sizeof bar_last - 1] = (char)(wifi_up ? 'W' : 'w');
}
// Called whenever the shell or the browser waits for a key: keeps the clock, battery and Wi-Fi symbol current.
static u64 bar_next, bat_next;
static u32 bar_started;
static void bar_tick(int browser) {
    u64 hz = tick_hz(), now = ticks();
    if (!bar_started) { bar_started = 1; rtc_read(); }
    if (hz && now < bar_next && !bar_dirty) return;
    bar_next = now + hz / 2;
    if (hz && now >= bat_next) { bat_next = now + hz * 30; bat_read(); }
    if (wifi_up && time_src != 2 && ntp_tries < 3 && hz && now >= ntp_next) { ntp_tries++; ntp_next = now + hz * 20; u32 k = con_on; con_on = 0; ntp_sync(); con_on = k; }
    char b[96]; bar_text(b);
    int same = 1; for (u32 i = 0; i < 95; i++) { if (b[i] != bar_last[i]) { same = 0; break; } if (!b[i]) break; }
    if (bar_last[sizeof bar_last - 1] != (char)(wifi_up ? 'W' : 'w')) same = 0;
    if (same && !bar_dirty) return;
    bar_dirty = 0;
    bar_draw(browser);
}

// battery: show what the EC reports (and why there is no number, if there is none)
static int bat_cmd(void) {
    u8 p[2] = {0x40, 32}, r[32]; u16 n = 0;
    int rc = ec_cmd(0x0007, 0, p, 2, r, sizeof r, &n);
    puts("EC battery read: result "); put_dec((u64)(rc < 0 ? -rc : rc)); puts(rc < 0 ? " (wave-os error)" : ""); puts(", "); put_dec(n); puts(" bytes\n");
    bat_unsupported = 0; bat_read();
    if (bat_pct < 0) { sum_s("no battery reading"); return 0; }
    sum_u((u32)bat_pct); sum_s(bat_charging ? "% charging" : "%");
    return 1;
}