// Error codes: short, stable names for everything that can go wrong (meanings in ERRORS.md).
// Each code is shown in yellow on screen, written to the log, and summarised in a status line
// kept at the very top of the log, so `head -3` of console-ramoops-0 tells the story.

static char area_digit(const char *a);
struct errent { const char *area; u32 num; u32 sub; const char *what; };   // sub 0 = none, else the .N cause
static struct errent err_seen[24];
static u32 err_count;
static u32 status_pos;          // where the status line's value starts in the log
#define STATUS_WIDTH 200

// Reserve the status line as the first line of the log; filled in by status_update().
static void status_reserve(void) {
    puts("status: ");
    status_pos = rlog_len;
    for (int i = 0; i < STATUS_WIDTH; i++) logc(' ');
    logc('\n');
}

static void status_update(void) {
    if (!rlog_data || !status_pos) return;
    char line[STATUS_WIDTH];
    u32 n = 0;
    #define ADD(s) for (const char *q_ = (s); *q_ && n < STATUS_WIDTH; q_++) line[n++] = *q_
    if (!err_count) { ADD("ok, no errors"); }
    for (u32 i = 0; i < err_count; i++) {
        if (i) ADD(" | ");
        { char a[3] = {area_digit(err_seen[i].area), '.', 0}; ADD(a); }   // where it happened (6 = Wi-Fi, 4 = keyboard...)
        char d[3] = {(char)('0' + err_seen[i].num / 10 % 10), (char)('0' + err_seen[i].num % 10), 0};
        ADD(d);                                            // which error
        if (err_seen[i].sub) { char s[3] = {'.', (char)('0' + err_seen[i].sub % 10), 0}; ADD(s); }   // which cause
        ADD(": "); ADD(err_seen[i].what);                  // what it says
    }
    #undef ADD
    while (n < STATUS_WIDTH) line[n++] = ' ';
    for (u32 i = 0; i < STATUS_WIDTH; i++) rlog_data[status_pos + i] = line[i];
}

// Area numbers: 1 BOOT, 2 LOG, 3 DISP, 4 KB (keyboard), 5 EC (the keyboard chip's own errors), 6 WIFI.
static char area_digit(const char *a) {
    if (streq(a, "BOOT")) return '1'; if (streq(a, "LOG")) return '2'; if (streq(a, "DISP")) return '3';
    if (streq(a, "KB")) return '4'; if (streq(a, "EC")) return '5'; if (streq(a, "WIFI")) return '6'; if (streq(a, "NET")) return '7';
    return '0';
}
static void put_code(const char *area, u32 num, u32 sub) {
    putc(area_digit(area)); putc('.'); putc('0' + num / 10 % 10); putc('0' + num % 10);
    if (sub) { putc('.'); putc('0' + sub % 10); }
}

// Report an error once per boot (repeats of the same code are only counted). sub (1..9) names which of
// several causes behind a code it was, shown as AREA.NN.sub (e.g. WIFI.09.2); 0 = the code has one cause.
// One-line summary of the last command: sum_res = what the command says, cmd_codes = every error code it raised.
static char sum_res[40]; static u32 sum_n;
static char cmd_codes[48]; static u32 cmd_codes_n;
static void sum_reset(void) { sum_n = 0; sum_res[0] = 0; cmd_codes_n = 0; cmd_codes[0] = 0; }
static void sum_c(char c) { if (sum_n < sizeof sum_res - 1) { sum_res[sum_n++] = c; sum_res[sum_n] = 0; } }
static void sum_s(const char *s) { while (*s) sum_c(*s++); }
static void sum_u(u32 v) { char t[12]; u32 k = 0; do { t[k++] = '0' + v % 10; v /= 10; } while (v); while (k) sum_c(t[--k]); }
// Run-length form of a result string: 111111111111 -> 1x12, aaaaFFaFFFFF -> ax4.Fx2.a.Fx5
static void sum_rle(const char *s) {
    u32 i = 0, first = 1;
    while (s[i]) {
        u32 j = i; while (s[j] == s[i]) j++;
        u32 k = j - i;
        if (!first) sum_c('.');
        first = 0; sum_c(s[i]);
        if (k > 1) { sum_c('x'); if (k >= 10) sum_c('0' + k / 10 % 10); sum_c('0' + k % 10); }
        i = j;
    }
}
static void code_note(const char *area, u32 num, u32 sub) {
    char t[8]; u32 k = 0;
    t[k++] = area_digit(area); t[k++] = '.'; t[k++] = '0' + num / 10 % 10; t[k++] = '0' + num % 10;
    if (sub) { t[k++] = '.'; t[k++] = '0' + sub % 10; }
    t[k] = 0;
    for (u32 i = 0; i + k <= cmd_codes_n; i++) {                       // already listed?
        u32 j = 0; while (j < k && cmd_codes[i + j] == t[j]) j++;
        if (j == k && (i + k == cmd_codes_n || cmd_codes[i + k] == ',')) return;
    }
    if (cmd_codes_n + k + 2 >= sizeof cmd_codes) return;
    if (cmd_codes_n) cmd_codes[cmd_codes_n++] = ',';
    for (u32 j = 0; j < k; j++) cmd_codes[cmd_codes_n++] = t[j];
    cmd_codes[cmd_codes_n] = 0;
}
static void errs(const char *area, u32 num, u32 sub, const char *what) {
    code_note(area, num, sub);
    for (u32 i = 0; i < err_count; i++)
        if (err_seen[i].num == num && err_seen[i].sub == sub && streq(err_seen[i].area, area)) return;
    if (err_count < sizeof err_seen / sizeof err_seen[0]) {
        err_seen[err_count].area = area; err_seen[err_count].num = num; err_seen[err_count].sub = sub; err_seen[err_count].what = what;
        err_count++;
    }
    u32 fg = con_fg;
    con_fg = 0xFFE040;
    puts("error "); put_code(area, num, sub); puts(": "); puts(what); putc('\n');
    con_fg = fg;
    status_update();
}

#define err(area, num, what) errs(area, num, 0, what)
static void list_errors(void) {
    if (!err_count) { puts("no errors this boot\n"); return; }
    for (u32 i = 0; i < err_count; i++) {
        put_code(err_seen[i].area, err_seen[i].num, err_seen[i].sub); puts("  "); puts(err_seen[i].what); putc('\n');
    }
}

// Chrome EC: wave-os side failures are KB-02..KB-10 (= -ec_cmd() result), EC replies are EC-nn.
static const char *kb_err_text(int code) {
    switch (code) {
    case 2: return "SPI send failed (controller never finished a chunk)";
    case 3: return "EC not ready while receiving the request";
    case 4: return "SPI read failed while waiting for the reply";
    case 5: return "EC never started a reply (200 ms)";
    case 6: return "SPI read failed in the reply header";
    case 7: return "reply header invalid (version or length)";
    case 8: return "SPI read failed in the reply data";
    case 9: return "reply checksum wrong";
    default: return "request too long";
    }
}
static const char *ec_res_text(int r) {
    switch (r) {
    case 1: return "EC: invalid command"; case 2: return "EC: error"; case 3: return "EC: invalid parameter";
    case 4: return "EC: access denied"; case 5: return "EC: invalid response"; case 6: return "EC: invalid version";
    case 7: return "EC: invalid checksum"; case 8: return "EC: in progress"; case 9: return "EC: unavailable";
    case 10: return "EC: timeout"; case 11: return "EC: overflow"; default: return "EC: other error";
    }
}
static void err_ec(int r) {
    if (r < 0) err("KB", (u32)-r, kb_err_text(-r));
    else if (r > 0) err("EC", (u32)r, ec_res_text(r));
}
