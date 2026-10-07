// QEMU-only: configure the ramfb display via fw_cfg so a window shows our framebuffer.
#define FWCFG 0x09020000UL
static inline u32 bs32(u32 v) { return __builtin_bswap32(v); }
static void fw_dma(u32 ctl, void *buf, u32 len) {
    volatile u32 cmd[4] __attribute__((aligned(16)));
    cmd[0] = bs32(ctl); cmd[1] = bs32(len);
    u64 a = (u64)buf; cmd[2] = bs32(a >> 32); cmd[3] = bs32(a);
    u64 c = (u64)cmd;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile u32 *)(FWCFG + 0x10) = bs32(c >> 32);
    *(volatile u32 *)(FWCFG + 0x14) = bs32(c);
    while (bs32(cmd[0]) & ~1u) __asm__ volatile("" ::: "memory");
}
static void ramfb_init(u64 addr, u32 w, u32 h) {
    u8 dir[4 + 64 * 64] __attribute__((aligned(16)));
    fw_dma((0x19 << 16) | 8 | 2, dir, sizeof dir);
    u32 n = be32(dir);
    for (u32 i = 0; i < n && i < 64; i++) {
        const u8 *e = dir + 4 + i * 64;
        if (streq((const char *)e + 8, "etc/ramfb")) {
            u32 sel = (e[4] << 8) | e[5];
            u8 cfg[28] __attribute__((aligned(16)));
            u64 a = addr; u32 x[5] = {0x34325258, 0, w, h, w * 4};
            for (int k = 0; k < 8; k++) cfg[k] = a >> (56 - 8 * k);
            for (int j = 0; j < 5; j++) for (int k = 0; k < 4; k++) cfg[8 + j * 4 + k] = x[j] >> (24 - 8 * k);
            fw_dma((sel << 16) | 8 | 16, cfg, 28);
            return;
        }
    }
}
