// Keyboard: MT8173 SPI controller (polled, FIFO mode) -> Chrome EC host-command protocol v3 -> key matrix.
// Register behaviour follows Linux drivers/spi/spi-mt65xx.c; protocol follows
// drivers/platform/chrome/cros_ec_spi.c. The firmware already uses this bus (it reads Ctrl+U through
// the EC), so clocks and pins are set up when we start.
#include "keymap.h"

#define SPI_BASE      0x1100a000UL
#define SPI_CFG0      0x00
#define SPI_CFG1      0x04
#define SPI_TX_DATA   0x10
#define SPI_RX_DATA   0x14
#define SPI_CMD       0x18
#define SPI_STATUS0   0x1c
#define SPI_PAD_SEL   0x24
#define CMD_ACT       (1u << 0)
#define CMD_RESUME    (1u << 1)
#define CMD_RST       (1u << 2)
#define CMD_PAUSE_EN  (1u << 4)
#define CMD_CPHA      (1u << 8)
#define CMD_CPOL      (1u << 9)
#define CMD_RX_DMA    (1u << 10)
#define CMD_TX_DMA    (1u << 11)
#define CMD_TXMSBF    (1u << 12)
#define CMD_RXMSBF    (1u << 13)
#define CMD_RX_ENDIAN (1u << 14)
#define CMD_TX_ENDIAN (1u << 15)
#define CMD_FINISH_IE (1u << 16)
#define CMD_PAUSE_IE  (1u << 17)

static u32 spi_rd(u32 off) { return *(volatile u32 *)(SPI_BASE + off); }
static void spi_wr(u32 off, u32 v) { *(volatile u32 *)(SPI_BASE + off) = v; }
static int spi_paused;          // a chunk ended with chip-select still held
static u32 spi_timeouts, spi_last_cmd, spi_last_len;   // last timeout: CMD register, chunk length

static void spi_init(void) {
    u32 cmd = spi_rd(SPI_CMD);
    cmd &= ~(CMD_CPHA | CMD_CPOL | CMD_RX_DMA | CMD_TX_DMA | CMD_RX_ENDIAN | CMD_TX_ENDIAN | CMD_PAUSE_EN);
    cmd |= CMD_TXMSBF | CMD_RXMSBF | CMD_FINISH_IE | CMD_PAUSE_IE;   // SPI mode 0, MSB first, FIFO mode
    spi_wr(SPI_CMD, cmd);
    spi_wr(SPI_PAD_SEL, 1);                                          // mediatek,pad-select = <1> (hana)
}

static void spi_cs(int active) {
    u32 cmd = spi_rd(SPI_CMD);
    if (active) { spi_wr(SPI_CMD, cmd | CMD_PAUSE_EN); return; }
    cmd &= ~CMD_PAUSE_EN;                                            // release chip-select...
    spi_wr(SPI_CMD, cmd);
    spi_wr(SPI_CMD, cmd | CMD_RST);                                  // ...and reset the controller
    spi_wr(SPI_CMD, cmd & ~CMD_RST);
    spi_paused = 0;
}

// One FIFO chunk (<= 32 bytes). tx may be 0 (send zeros). Returns 0 on success.
static int spi_chunk(const u8 *tx, u8 *rx, u32 n) {
    u32 cfg1 = spi_rd(SPI_CFG1);
    cfg1 &= ~(0x3ff0000u | 0xff00u);
    cfg1 |= (n - 1) << 16;                                           // packet length; loop count 1
    spi_wr(SPI_CFG1, cfg1);
    for (u32 i = 0; i < n; i += 4) {
        u32 w = 0;
        for (u32 j = 0; j < 4 && i + j < n; j++) w |= (u32)(tx ? tx[i + j] : 0) << (8 * j);
        spi_wr(SPI_TX_DATA, w);
    }
    spi_wr(SPI_CMD, spi_rd(SPI_CMD) | (spi_paused ? CMD_RESUME : CMD_ACT));
    u32 st = 0;
    u64 hz = tick_hz(), t0 = ticks();
    for (u32 i = 0; i < 2000000; i++) {
        st = spi_rd(SPI_STATUS0);
        if (st) break;
        if (hz && ticks() - t0 > hz / 20) break;                     // 50 ms
    }
    if (!st) { spi_timeouts++; spi_last_cmd = spi_rd(SPI_CMD); spi_last_len = n; return -1; }
    spi_paused = (st & 2) != 0;
    for (u32 i = 0; i < n; i += 4) {
        u32 w = spi_rd(SPI_RX_DATA);
        for (u32 j = 0; j < 4 && i + j < n; j++) rx[i + j] = w >> (8 * j);
    }
    return 0;
}

static int spi_xfer(const u8 *tx, u8 *rx, u32 n) {
    for (u32 off = 0; off < n; off += 32) {
        u32 len = n - off < 32 ? n - off : 32;
        if (spi_chunk(tx ? tx + off : 0, rx + off, len)) return -1;
    }
    return 0;
}

// ---- Chrome EC host command protocol v3 over SPI ----
#define EC_SPI_FRAME_START 0xec
#define EC_SPI_PAST_END    0xed
#define EC_SPI_RX_BAD_DATA 0xfb
#define EC_SPI_NOT_READY   0xfc
#define EC_CMD_HELLO       0x0001
#define EC_CMD_GET_VERSION 0x0002
#define EC_CMD_MKBP_STATE  0x0060
#define EC_CMD_MKBP_INFO   0x0061

static u8 ec_out[256], ec_in[256];
static u64 ec_last;              // tick of the last chip-select release
static int ec_last_err;

// Returns the EC result code (0 = success), or a negative wave-os error. Response data -> resp.
static int ec_cmd(u16 cmd, u8 ver, const u8 *p, u16 plen, u8 *resp, u16 rmax, u16 *rlen) {
    if (plen > 200) return -10;
    u64 hz = tick_hz();
    while (hz && ticks() - ec_last < hz / 2000) ;                    // 500 us between messages (DT msg-delay)
    u8 *q = ec_out;
    q[0] = 3; q[1] = 0; q[2] = cmd; q[3] = cmd >> 8; q[4] = ver; q[5] = 0; q[6] = plen; q[7] = plen >> 8;
    for (u16 i = 0; i < plen; i++) q[8 + i] = p[i];
    u8 sum = 0;
    for (u32 i = 0; i < 8u + plen; i++) sum += q[i];
    q[1] = -sum;

    int ret = -1;
    u32 sendlen = (8u + plen + 3) & ~3u;                              // a multiple of 4 bytes: the SPI controller jams on anything else (v1.6-002: a 10-byte battery request did)
    for (u32 i = 8u + plen; i < sendlen; i++) q[i] = 0;
    spi_cs(1);
    if (spi_xfer(ec_out, ec_in, sendlen)) { ret = -2; goto out; }
    for (u32 i = 0; i < 8u + plen; i++)
        if (ec_in[i] == EC_SPI_PAST_END || ec_in[i] == EC_SPI_RX_BAD_DATA || ec_in[i] == EC_SPI_NOT_READY) { ret = -3; goto out; }

    // Clock in 32 bytes at a time until the frame start byte, for up to 200 ms
    u32 have = 0;
    u64 t0 = ticks();
    for (;;) {
        if (spi_xfer(0, ec_in, 32)) { ret = -4; goto out; }
        u32 k = 0;
        while (k < 32 && ec_in[k] != EC_SPI_FRAME_START) k++;
        if (k < 32) {
            have = 31 - k;
            for (u32 i = 0; i < have; i++) ec_in[i] = ec_in[k + 1 + i];
            break;
        }
        if (hz && ticks() - t0 > hz / 5) { ret = -5; goto out; }
    }
    // Read the rest in whole 32-byte chunks only: an odd-sized chunk mid-message left the real
    // controller stuck (v0.8 on hana). Bytes past the end of the reply are harmless (EC sends 0xed).
    while (have < 8) {
        if (spi_xfer(0, ec_in + have, 32)) { ret = -6; goto out; }
        have += 32;
    }
    u16 dlen = ec_in[4] | (ec_in[5] << 8);
    if (ec_in[0] != 3 || 8u + dlen + 32 > sizeof ec_in) { ret = -7; goto out; }
    while (have < 8u + dlen) {
        if (spi_xfer(0, ec_in + have, 32)) { ret = -8; goto out; }
        have += 32;
    }
    sum = 0;
    for (u32 i = 0; i < 8u + dlen; i++) sum += ec_in[i];
    if (sum) { ret = -9; goto out; }
    ret = ec_in[2] | (ec_in[3] << 8);                                // EC result code
    if (resp) for (u16 i = 0; i < dlen && i < rmax; i++) resp[i] = ec_in[8 + i];
    if (rlen) *rlen = dlen;
out:
    spi_cs(0);
    ec_last = ticks();
    ec_last_err = ret;
    return ret;
}

// ---- keyboard: poll the matrix, turn changes into characters ----
static u8 kb_state[KB_COLS];
static int kb_shift_down, kb_ctrl_down, kb_ok;
static char kb_queue[32];
static u32 kb_head, kb_tail;
static u64 kb_next_poll;

static void kb_push(char c) { if (kb_head - kb_tail < sizeof kb_queue) kb_queue[kb_head++ % sizeof kb_queue] = c; }

static void kb_poll(void) {
    u64 hz = tick_hz(), now = ticks();
    if (hz && now < kb_next_poll) return;
    kb_next_poll = now + hz / 100;                                   // every 10 ms
    u8 m[KB_COLS]; u16 n = 0;
    int r = ec_cmd(EC_CMD_MKBP_STATE, 0, 0, 0, m, KB_COLS, &n);
    if (r != 0) { err_ec(r); return; }                              // shown once; typing keeps working
    if (n < KB_COLS) return;
    for (u32 c = 0; c < KB_COLS; c++) {
        u8 diff = m[c] ^ kb_state[c];
        for (u32 r = 0; r < KB_ROWS; r++) {
            if (!(diff & (1u << r))) continue;
            int down = (m[c] >> r) & 1;
            char k = kb_norm[r][c];
            if (k == KB_SHIFT) kb_shift_down = down;
            else if (k == KB_CTRL) kb_ctrl_down = down;
            else if (down && k > 4) kb_push(kb_shift_down ? kb_shift[r][c] : k);
        }
        kb_state[c] = m[c];
    }
}

static int kb_getc(void) {
    if (kb_ok) kb_poll();
    if (kb_head == kb_tail) return -1;
    return kb_queue[kb_tail++ % sizeof kb_queue];
}

// Bring up SPI + EC, log everything. Returns 1 if the keyboard answers.
static int kb_init(void) {
    puts("spi regs: cfg0 "); put_hex(spi_rd(SPI_CFG0)); puts(" cfg1 "); put_hex(spi_rd(SPI_CFG1));
    puts(" cmd "); put_hex(spi_rd(SPI_CMD)); puts(" pad "); put_hex(spi_rd(SPI_PAD_SEL)); putc('\n');
    spi_init();
    u8 buf[96]; u16 n = 0;
    u8 hello[4] = {0x00, 0x00, 0xa0, 0xa0};                          // 0xa0a00000; EC answers +0x01020304
    int r = -1;
    for (int attempt = 1; attempt <= 5; attempt++) {                 // the first transfer after boot sometimes stalls (v1.4 on hana: KB-02)
        r = ec_cmd(EC_CMD_HELLO, 0, hello, 4, buf, 4, &n);
        if (r == 0) break;
        puts("ec hello try "); put_dec(attempt); puts(" failed: "); put_dec((u64)(r < 0 ? -r : r)); puts(" (spi timeouts "); put_dec(spi_timeouts);
        puts(", status "); put_hex(spi_rd(SPI_STATUS0)); puts(", cmd "); put_hex(spi_rd(SPI_CMD)); puts(")\n");
        spi_init();                                                  // reset and reprogram the controller, then wait
        delay_us(50000);
    }
    puts("ec hello: "); put_dec((u64)(r < 0 ? -r : r)); puts(r < 0 ? " (wave-os error)" : " (ec result)");
    if (r == 0) { puts(" answer "); put_hex(buf[0] | buf[1] << 8 | (u32)buf[2] << 16 | (u32)buf[3] << 24); }
    puts(" spi timeouts "); put_dec(spi_timeouts); putc('\n');
    if (r != 0) { err_ec(r); errs("KB", 1, 1, "keyboard unavailable: EC did not answer hello"); return 0; }
    r = ec_cmd(EC_CMD_GET_VERSION, 0, 0, 0, buf, 96, &n);
    if (r == 0) { buf[31] = 0; puts("ec version: "); puts((const char *)buf); putc('\n'); } else err_ec(r);
    r = ec_cmd(EC_CMD_MKBP_INFO, 0, 0, 0, buf, 9, &n);
    if (r == 0) { puts("keyboard matrix: "); put_dec(buf[0]); puts(" rows x "); put_dec(buf[4]); puts(" cols\n"); } else err_ec(r);
    u8 m[KB_COLS];
    for (int attempt = 1; attempt <= 3; attempt++) {
        n = 0;
        r = ec_cmd(EC_CMD_MKBP_STATE, 0, 0, 0, m, KB_COLS, &n);
        puts("mkbp state try "); put_dec(attempt); puts(": result "); put_dec((u64)(r < 0 ? -r : r));
        puts(r < 0 ? " (wave-os error)" : ""); puts(", "); put_dec(n); puts(" bytes");
        if (r != 0) {
            puts(" | spi timeouts "); put_dec(spi_timeouts); puts(" last cmd "); put_hex(spi_last_cmd);
            puts(" len "); put_dec(spi_last_len); puts(" | rx");
            for (u32 i = 0; i < 24; i++) { putc(' '); put_hex(ec_in[i]); }
        }
        putc('\n');
        if (r == 0) break;
    }
    if (r != 0) { err_ec(r); errs("KB", 1, 2, "keyboard unavailable: matrix read failed 3 times"); return 0; }
    for (u32 c = 0; c < KB_COLS; c++) kb_state[c] = m[c];
    kb_ok = 1;
    return 1;
}
