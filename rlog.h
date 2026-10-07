// Device-tree scan + "ramoops" logger.
// ChromeOS reserves RAM (ramoops) that survives a reboot; the firmware puts its location in the DT.
// We write our log into the ramoops *console* zone in the format Linux expects, so after rebooting
// into ChromeOS it shows up in /sys/fs/pstore/console-ramoops-0. Every character is written straight
// to RAM, so the log survives even if we freeze partway.

struct dtinfo {
    const char *model;
    const u8 *ramoops_reg; u32 ramoops_reg_len;
    u32 rec_size, con_size, ftrace_size, pmsg_size, ecc_size;
    const u8 *cb_reg; u32 cb_reg_len;
    const char *bootargs;
    u32 nodes;
};
static struct dtinfo dti;

static int str_has(const u8 *d, u32 len, const char *s) {   // s in a NUL-separated string list
    u32 i = 0;
    while (i < len) { if (streq((const char *)d + i, s)) return 1; while (i < len && d[i]) i++; i++; }
    return 0;
}

// One pass over the flattened DT, collecting what we need. Matching is done at END_NODE because
// "compatible" may come after the other properties.
static void dt_scan(const u8 *dt) {
    if (be32(dt) != 0xd00dfeed) return;
    const u8 *p = dt + be32(dt + 8);
    const char *strs = (const char *)dt + be32(dt + 12);
    struct { int ramoops, cb, chosen; const u8 *reg; u32 reglen; u32 rs, cs, fs, ps, es; const char *bootargs; } st[16];
    int depth = -1;
    for (u32 guard = 0; guard < 200000; guard++) {
        u32 tok = be32(p); p += 4;
        if (tok == 1) {                                   // BEGIN_NODE
            const char *name = (const char *)p;
            while (*p) p++;
            p = (const u8 *)(((u64)p + 4) & ~3UL);
            if (++depth >= 16) return;
            st[depth].ramoops = st[depth].cb = 0; st[depth].reg = 0; st[depth].reglen = 0;
            st[depth].rs = st[depth].cs = st[depth].fs = st[depth].ps = st[depth].es = 0;
            st[depth].bootargs = 0;
            st[depth].chosen = streq(name, "chosen");
            dti.nodes++;
        } else if (tok == 2) {                            // END_NODE
            if (depth < 0) return;
            if (st[depth].ramoops && !dti.ramoops_reg) {
                dti.ramoops_reg = st[depth].reg; dti.ramoops_reg_len = st[depth].reglen;
                dti.rec_size = st[depth].rs; dti.con_size = st[depth].cs;
                dti.ftrace_size = st[depth].fs; dti.pmsg_size = st[depth].ps; dti.ecc_size = st[depth].es;
            }
            if (st[depth].cb && !dti.cb_reg) { dti.cb_reg = st[depth].reg; dti.cb_reg_len = st[depth].reglen; }
            if (st[depth].chosen && st[depth].bootargs) dti.bootargs = st[depth].bootargs;
            depth--;
        } else if (tok == 3) {                            // PROP
            u32 len = be32(p), off = be32(p + 4); p += 8;
            const char *n = strs + off;
            if (depth >= 0) {
                if (streq(n, "compatible")) {
                    if (str_has(p, len, "ramoops")) st[depth].ramoops = 1;
                    if (str_has(p, len, "coreboot")) st[depth].cb = 1;
                } else if (streq(n, "reg")) { st[depth].reg = p; st[depth].reglen = len; }
                else if (streq(n, "record-size")) st[depth].rs = be32(p);
                else if (streq(n, "console-size")) st[depth].cs = be32(p);
                else if (streq(n, "ftrace-size")) st[depth].fs = be32(p);
                else if (streq(n, "pmsg-size")) st[depth].ps = be32(p);
                else if (streq(n, "ecc-size")) st[depth].es = be32(p);
                else if (streq(n, "bootargs")) st[depth].bootargs = (const char *)p;
                else if (depth == 0 && streq(n, "model")) dti.model = (const char *)p;
            }
            p = (const u8 *)(((u64)p + len + 3) & ~3UL);
        } else if (tok == 4) {                            // NOP
        } else return;                                    // END
    }
}

// Decode the first (address, size) from a reg property, guessing the cell sizes from its length.
// nregs = how many entries the node is expected to have.
static void reg_first(const u8 *reg, u32 len, u32 nregs, u64 *addr, u64 *size) {
    u32 per = nregs ? len / nregs : len;
    if (per >= 16)      { *addr = be64(reg); *size = be64(reg + 8); }   // 2 address + 2 size cells
    else if (per == 12) { *addr = be64(reg); *size = be32(reg + 8); }   // 2 + 1
    else if (per >= 8)  { *addr = be32(reg); *size = be32(reg + 4); }   // 1 + 1
    else { *addr = 0; *size = 0; }
}

#define RAM_LO 0x40000000UL
#define RAM_HI 0x140000000UL
static int in_ram(u64 a, u64 n) { return a >= RAM_LO && a + n <= RAM_HI && a + n >= a; }

// ---- the ramoops console-zone logger ----
static volatile u32 *rlog_hdr;   // persistent_ram_buffer: u32 sig, u32 start, u32 size, then data
static volatile u8 *rlog_data;
static u32 rlog_cap, rlog_len;
static u64 rlog_zone, rlog_zone_size;

static void logc(char c) {
    if (!rlog_data || rlog_len >= rlog_cap) return;
    rlog_data[rlog_len++] = c;
    rlog_hdr[1] = rlog_len;      // start (next write position; no wrap)
    rlog_hdr[2] = rlog_len;      // size
}

// Same zone layout as Linux fs/pstore/ram.c: dmesg records first, then console, ftrace, pmsg.
static const char *rlog_src = "device tree";
static int rlog_init(void) {
    u64 base, size;
    if (dti.ramoops_reg && dti.con_size) {
        reg_first(dti.ramoops_reg, dti.ramoops_reg_len, 1, &base, &size);
    } else {
        // Not in the device tree on hana: use the values ChromeOS itself uses there
        // (/sys/module/ramoops/parameters: 1 MB at 0xb1f00000, 128K record/console/pmsg, no ftrace, no ECC)
        base = 0xb1f00000UL; size = 0x100000;
        dti.rec_size = 0x20000; dti.con_size = 0x20000; dti.pmsg_size = 0x20000;
        dti.ftrace_size = 0; dti.ecc_size = 0;
        rlog_src = "hana defaults";
    }
    if (!in_ram(base, size)) return 0;
    u64 dump = size - dti.con_size - dti.ftrace_size - dti.pmsg_size;
    u64 zone = base;
    if (dti.rec_size && dump >= dti.rec_size) {
        u64 cnt = dump / dti.rec_size;
        zone = base + cnt * (dump / cnt);
    }
    if (zone + dti.con_size > base + size) return 0;
    rlog_zone = zone; rlog_zone_size = dti.con_size;
    rlog_hdr = (volatile u32 *)zone;
    rlog_data = (volatile u8 *)zone + 12;
    rlog_cap = dti.con_size - 12;
    if (dti.ecc_size) rlog_cap = rlog_cap / 2;   // leave room for ECC blocks if enabled
    rlog_len = 0;
    rlog_hdr[1] = 0; rlog_hdr[2] = 0;
    rlog_hdr[0] = 0x43474244;                    // PERSISTENT_RAM_SIG "DBGC"
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}

// ---- coreboot table: firmware's own record of the framebuffer ----
struct cbfb { u64 addr; u32 w, h, pitch; u8 bpp, rpos, rsz, gpos, gsz, bpos, bsz; };

static int cb_find_fb(struct cbfb *out, u64 *table_out) {
    if (!dti.cb_reg) return 0;
    u64 t, tsz;
    reg_first(dti.cb_reg, dti.cb_reg_len, 2, &t, &tsz);
    for (int hop = 0; hop < 3; hop++) {
        *table_out = t;
        if (!in_ram(t, 24)) return 0;
        const u8 *h = (const u8 *)t;
        if (h[0] != 'L' || h[1] != 'B' || h[2] != 'I' || h[3] != 'O') return 0;
        u32 hb = *(const u32 *)(h + 4), tb = *(const u32 *)(h + 12), n = *(const u32 *)(h + 20);
        if (!in_ram(t + hb, tb) || n > 1000) return 0;
        const u8 *r = h + hb, *end = r + tb;
        int forwarded = 0;
        for (u32 i = 0; i < n && r + 8 <= end; i++) {
            u32 tag = *(const u32 *)r, sz = *(const u32 *)(r + 4);
            if (sz < 8 || r + sz > end) break;
            if (tag == 0x11 && sz >= 16) { t = *(const u64 *)(r + 8); forwarded = 1; break; }   // LB_TAG_FORWARD
            if (tag == 0x12 && sz >= 28) {                                                      // LB_TAG_FRAMEBUFFER
                out->addr = *(const u64 *)(r + 8);
                out->w = *(const u32 *)(r + 16); out->h = *(const u32 *)(r + 20); out->pitch = *(const u32 *)(r + 24);
                const u8 *b = r + 28;
                out->bpp = b[0]; out->rpos = b[1]; out->rsz = b[2]; out->gpos = b[3]; out->gsz = b[4];
                out->bpos = b[5]; out->bsz = b[6];
                return 1;
            }
            r += sz;
        }
        if (!forwarded) return 0;
    }
    return 0;
}
