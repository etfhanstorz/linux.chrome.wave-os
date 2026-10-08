// Remote shell (v1.6-005): type on the PC, run on the Chromebook. The PC (tools\wsh.ps1, wsh.bat) sends a command line as one UDP packet to
// port 5150; the output comes back through the log shipping like everything else. Only packets from the PC's address (update_srv / upset)
// that carry a valid signature (HMAC-SHA256 with the update key, which only your PC has) are accepted, and each one only once.
//   packet = "WRSH" | sequence (8 bytes, big endian: the PC's clock in milliseconds) | signature (16 bytes, over sequence + command) | command

#define RSH_PORT 5150
static char rsh_cmd[124]; static u32 rsh_pending, rsh_rejected;
static u64 rsh_last_seq;
static int rsh_udp(u32 src, u32 dport, const u8 *d, u32 n) {
    if (dport != RSH_PORT) return 0;
    if (src != update_srv || n < 4 + 8 + 16 + 1 || n > 4 + 8 + 16 + 120 || d[0] != 'W' || d[1] != 'R' || d[2] != 'S' || d[3] != 'H') { rsh_rejected++; return 1; }
    u64 seq = 0; for (u32 i = 0; i < 8; i++) seq = seq << 8 | d[4 + i];
    u32 cl = n - 28;
    static u8 m[8 + 120]; u8 mac[32];
    mcopy(m, d + 4, 8); mcopy(m + 8, d + 28, cl);
    hmac_sha256(update_key, sizeof update_key, m, 8 + cl, mac);
    if (!meq(mac, d + 12, 16)) { rsh_rejected++; return 1; }                 // not signed with our key
    if (seq <= rsh_last_seq) { rsh_rejected++; return 1; }                    // a copy of an old command
    long long now = now_unix();
    if (time_src == 2 && ((long long)(seq / 1000) < now - 300 || (long long)(seq / 1000) > now + 300)) { rsh_rejected++; return 1; }   // too old (or from the future)
    rsh_last_seq = seq;
    u32 k = 0; for (u32 i = 0; i < cl && k + 1 < sizeof rsh_cmd; i++) if (d[28 + i] >= 32 && d[28 + i] < 127) rsh_cmd[k++] = (char)d[28 + i];
    rsh_cmd[k] = 0;
    if (k) rsh_pending = 1;
    return 1;
}
static void rsh_init(void) { udp_hook = rsh_udp; }
