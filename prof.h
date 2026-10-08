// Speed framework (v1.50.2): measure where a command spends its time, and switch optimizations on and off to compare.
//
// Timing: prof_start()/prof_stop("name", t) add the time between the two calls to the span called "name" for the current command
// (spans: pwr = Wi-Fi power cycle, fw = firmware upload, cmd = firmware commands, scan = scan commands). prof_idle is the time spent
// waiting inside delay_us(). The shell resets all of it before each command and shows the total on the summary line; `prof` prints
// the breakdown of the last command on one line, e.g.  P pwr0.3 fw5.1 cmd1.2 scan6.0 idle9.1 of 14.2s
//
// Switches: `opt` lists them, `opt N` flips switch N. They all start OFF, so a normal run behaves exactly as before; turn one on,
// run the same command again and compare the times.  To add an optimization: add an OPT_ bit + a name below and guard the new code
// path with opt_on(OPT_...).

static u64 prof_idle, prof_last_total;
struct prof_span { const char *name; u64 t; u32 calls; };
static struct prof_span prof_spans[12];
static u32 prof_n;

static u64 prof_start(void) { return ticks(); }
static void prof_stop(const char *name, u64 t0) {
    u64 d = ticks() - t0;
    for (u32 i = 0; i < prof_n; i++) if (streq(prof_spans[i].name, name)) { prof_spans[i].t += d; prof_spans[i].calls++; return; }
    if (prof_n < sizeof prof_spans / sizeof prof_spans[0]) { prof_spans[prof_n].name = name; prof_spans[prof_n].t = d; prof_spans[prof_n].calls = 1; prof_n++; }
}
static void prof_reset(void) { prof_idle = 0; prof_n = 0; }

static u32 ms_of(u64 t) { u64 hz = tick_hz(); return hz ? (u32)(t / (hz / 1000)) : 0; }
static void put_secs(u32 ms) {                                        // 12345 -> "12.3"
    put_dec(ms / 1000); putc('.'); putc('0' + ms / 100 % 10);
}

// ---- optimization switches ----
#define OPT_WARM    0          // wifiinit: keep a chip that is already up instead of power-cycling and re-uploading the firmware (saves ~the whole fw upload)
#define OPT_FASTGAP 1          // 1 ms instead of 5 ms pause between Wi-Fi commands
#define OPT_FASTCLK 2          // twice the SDIO bus clock for the Wi-Fi chip (firmware upload and all traffic)
#define OPT_COUNT   3
static const char *opt_names[OPT_COUNT] = {"warm", "fastgap", "fastclk"};
static u32 opt_flags;
static int opt_on(int bit) { return (opt_flags >> bit) & 1; }
static void opt_list(void) {
    for (u32 i = 0; i < OPT_COUNT; i++) { putc('0' + i); putc(' '); puts(opt_names[i]); puts(opt_on(i) ? " ON  " : " off "); }
    putc('\n');
}
static void opt_toggle(u32 bit) { if (bit < OPT_COUNT) opt_flags ^= 1u << bit; }
