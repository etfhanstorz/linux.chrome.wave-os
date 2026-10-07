// Error codes: short, stable names for everything that can go wrong (meanings in ERRORS.md).
// Each code is shown in yellow on screen, written to the log, and summarised in a status line
// kept at the very top of the log, so `head -3` of console-ramoops-0 tells the story.

struct errent { const char *area; u32 num; const char *what; };
static struct errent err_seen[24];
static u32 err_count;
static u32 status_pos;          // where the status line's value starts in the log
#define STATUS_WIDTH 72

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
        if (i) ADD(" ");
        ADD(err_seen[i].area); ADD("-");
        char d[3] = {(char)('0' + err_seen[i].num / 10 % 10), (char)('0' + err_seen[i].num % 10), 0};
        ADD(d);
    }
    #undef ADD
    while (n < STATUS_WIDTH) line[n++] = ' ';
    for (u32 i = 0; i < STATUS_WIDTH; i++) rlog_data[status_pos + i] = line[i];
}

static void put_code(const char *area, u32 num) {
    puts(area); putc('-'); putc('0' + num / 10 % 10); putc('0' + num % 10);
}

// Report an error once per boot (repeats of the same code are only counted).
static void err(const char *area, u32 num, const char *what) {
    for (u32 i = 0; i < err_count; i++)
        if (err_seen[i].num == num && streq(err_seen[i].area, area)) return;
    if (err_count < sizeof err_seen / sizeof err_seen[0]) {
        err_seen[err_count].area = area; err_seen[err_count].num = num; err_seen[err_count].what = what;
        err_count++;
    }
    u32 fg = con_fg;
    con_fg = 0xFFE040;
    puts("error "); put_code(area, num); puts(": "); puts(what); putc('\n');
    con_fg = fg;
    status_update();
}

static void list_errors(void) {
    if (!err_count) { puts("no errors this boot\n"); return; }
    for (u32 i = 0; i < err_count; i++) {
        put_code(err_seen[i].area, err_seen[i].num); puts("  "); puts(err_seen[i].what); putc('\n');
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
