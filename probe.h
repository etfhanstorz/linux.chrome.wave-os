// Find the firmware's framebuffer by reading the MT8173 display overlay (OVL) registers.
// Reads are fault-safe: a bad read sets fault_flag (see fault_handler in start.S).
volatile u32 fault_flag;

static u32 rd32(u64 a) {
    fault_flag = 0;
    u32 v = *(volatile u32 *)a;
    __asm__ volatile("dsb sy" ::: "memory");
    return fault_flag ? 0xDEADDEAD : v;
}

struct layer { u32 ovl, n, en, con, size, pitch; u64 addr; };
static struct layer layers[8];     // OVL0 L0-3, OVL1 L0-3
static int nlayers;

#define RAM_LO 0x40000000UL
#define RAM_HI 0x140000000UL

static int probe_fb(struct fb *f) {
    static const u64 bases[2] = {0x1400c000UL, 0x1400d000UL};
    int best = -1;
    nlayers = 0;
    for (int o = 0; o < 2; o++) {
        u32 src_con = rd32(bases[o] + 0x2c);
        for (u32 n = 0; n < 4; n++) {
            struct layer *l = &layers[nlayers];
            l->ovl = o; l->n = n;
            l->en = (src_con == 0xDEADDEAD || src_con == 0xFFFFFFFF) ? 0 : (src_con >> n) & 1;
            l->con = rd32(bases[o] + 0x30 + 0x20 * n);
            l->size = rd32(bases[o] + 0x38 + 0x20 * n);
            l->pitch = rd32(bases[o] + 0x44 + 0x20 * n) & 0xffff;
            l->addr = rd32(bases[o] + 0xf40 + 0x20 * n);
            u32 w = l->size & 0x1fff, h = (l->size >> 16) & 0x1fff;
            if (best < 0 && l->en && l->addr >= RAM_LO && l->addr < RAM_HI && w && h && l->pitch >= w * 2)
                best = nlayers;
            nlayers++;
        }
    }
    if (best < 0) return 0;
    struct layer *l = &layers[best];
    f->addr = l->addr; f->w = l->size & 0x1fff; f->h = (l->size >> 16) & 0x1fff;
    f->stride = l->pitch; f->bpp = l->pitch >= f->w * 4 ? 32 : 16;
    return 1;
}
