p = '/mnt/c/!ab1/os/'
def sub(f, old, new):
    s = open(p + f).read()
    assert old in s, (f, old)
    open(p + f, 'w').write(s.replace(old, new, 1))

sub('err.h', 'static void errs(const char *area, u32 num, u32 sub, const char *what) {\n', '''// One-line summary of the last command: sum_res = what the command says, cmd_codes = every error code it raised.
static char sum_res[40]; static u32 sum_n;
static char cmd_codes[48]; static u32 cmd_codes_n;
static void sum_reset(void) { sum_n = 0; sum_res[0] = 0; cmd_codes_n = 0; cmd_codes[0] = 0; }
static void sum_c(char c) { if (sum_n < sizeof sum_res - 1) { sum_res[sum_n++] = c; sum_res[sum_n] = 0; } }
static void sum_s(const char *s) { while (*s) sum_c(*s++); }
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
''')
sub('wifi.h', '    puts("R:"); puts(res); puts(" D:"); putc(dom_res); putc(\'\\n\');          // the whole result in one short line\n',
    '    sum_s("R:"); sum_s(res); sum_s(" D:"); sum_c(dom_res);                // the shell prints this on the one-line summary\n')
sub('wifi.h', '    if (!have_target) { puts("N "); puts(grp); putc(\'\\n\'); return 0; }',
    '    if (!have_target) { sum_s("N "); sum_s(grp); return 0; }')
sub('wifi.h', '    puts("F ch"); put_dec(target.chan); putc(\' \'); puts(target.sec == 2 ? "WPA2" : target.sec == 1 ? "WPA" : target.sec == 3 ? "WEP" : "open");\n    puts(" -"); put_dec((u64)(target.rssi < 0 ? -target.rssi : target.rssi)); putc(\'\\n\');\n',
    '''    sum_s("F ch"); { char t[4]; u32 c = target.chan, k = 0; if (c >= 100) t[k++] = '0' + c / 100 % 10; t[k++] = '0' + c / 10 % 10; t[k++] = '0' + c % 10; for (u32 q = 0; q < k; q++) sum_c(t[q]); }
    sum_c(' '); sum_s(target.sec == 2 ? "WPA2" : target.sec == 1 ? "WPA" : target.sec == 3 ? "WEP" : "open");
    { int d = target.rssi < 0 ? -target.rssi : target.rssi; sum_s(" -"); sum_c('0' + d / 10 % 10); sum_c('0' + d % 10); }
''')
sub('shell.h', '    if (!*line) return;\n', '''    if (!*line) return;
    char name[16]; { u32 q = 0; while (line[q] && q < 15) { name[q] = line[q]; q++; } name[q] = 0; }
    sum_reset();
    run_cmd2(line, arg);
    if ((name[0] == 'w' && name[1] == 'i') || streq(name, "update") || streq(name, "cryptotest") || streq(name, "netdemo")) {   // one-line summary: command, what it says, error codes
        outs("> "); outs(name); out(' ');
        outs(sum_res[0] ? sum_res : cmd_codes_n ? "FAIL" : "ok");
        if (cmd_codes_n) { outs(" e"); outs(cmd_codes); }
        out('\\n');
    }
}
static void run_cmd2(char *line, char *arg) {
''')
sub('shell.h', 'static void run_cmd(char *line) {', 'static void run_cmd2(char *line, char *arg);\nstatic void run_cmd(char *line) {')
print('patched')
