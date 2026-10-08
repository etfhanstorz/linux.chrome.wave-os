root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('bar.h', [
("""#define TZ_OFFSET (-7 * 3600)                                  // MST = UTC-7 (no daylight saving)
#define TZ_NAME "MST\"""",
"""// Mountain time: MST = UTC-7, and MDT = UTC-6 from the second Sunday in March (2:00) to the first Sunday in November (2:00), US rules.
// (v1.6-002 used MST all year and was an hour off in summer.)"""),
("""static void civil_from_days(long long z, int *y, int *m, int *d) {""",
"""static void civil_from_days(long long z, int *y, int *m, int *d);
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
static void civil_from_days(long long z, int *y, int *m, int *d) {"""),
("""        long long lt = t + TZ_OFFSET, days""", """        int dst = tz_dst(t);
        long long lt = t + (dst ? -6 : -7) * 3600, days"""),
("""bar_append(b, &n, hh < 12 ? " AM " : " PM "); bar_append(b, &n, TZ_NAME);""",
"""bar_append(b, &n, hh < 12 ? " AM " : " PM "); bar_append(b, &n, dst ? "MDT" : "MST");"""),
("""    } else bar_append(b, &n, "--:-- " TZ_NAME);""", """    } else bar_append(b, &n, "--:--");"""),
])
s = open(root + 'version.h').read().replace('#define WAVE_PATCH   "-002"', '#define WAVE_PATCH   "-003"')
open(root + 'version.h', 'w', newline='').write(s)
print('ok')
