root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('net.h', [
("""static u32 udp_listen_port;""",
"""static u32 udp_listen_port;
static int (*udp_hook)(u32 src, u32 dport, const u8 *data, u32 len);   // a program that wants other UDP packets (rsh.h: the remote shell); returns 1 if it took the packet"""),
("""        if ((p[6] | p[7]) && !l4_ok(17, src, dst, p, ulen)) { net_bad_l4++; return; }             // checksum 0 = none (legal over IPv4)""",
"""        if ((p[6] | p[7]) && !l4_ok(17, src, dst, p, ulen)) { net_bad_l4++; return; }             // checksum 0 = none (legal over IPv4)
        if (udp_hook && udp_hook(src, dport, p + 8, ulen - 8)) return;"""),
])
edit('main.c', [("""#include "bar.h\"""", """#include "bar.h"
#include "rsh.h\"""")])
edit('shell.h', [
("""    con_top = 1; if (con_row < 1) con_row = 1; bar_dirty = 1;                // row 0 is the status bar from now on""",
"""    con_top = 1; if (con_row < 1) con_row = 1; bar_dirty = 1;                // row 0 is the status bar from now on
    rsh_init();                                                              // commands typed on the PC (wsh.bat) arrive here too"""),
("""                if (wifi_service()) {                                   // the network printed something: show the prompt and what was typed again""",
"""                if (rsh_pending) {                                      // a command typed on the PC: run it like a typed one
                    rsh_pending = 0;
                    static char rb[128]; u32 q = 0; while (rsh_cmd[q] && q < 127) { rb[q] = rsh_cmd[q]; q++; } rb[q] = 0;
                    glyph(con_col, con_row, ' ');
                    con_fg = 0x808090; outs(n ? "\\n(from the PC) " : "(from the PC) "); con_fg = C_TEXT; outs(rb); out('\\n');
                    run_cmd(rb);
                    con_fg = 0x40E0FF; outs("wave"); con_fg = 0x60FF80; outs("> "); con_fg = C_TEXT;
                    for (u32 q2 = 0; q2 < n; q2++) out(buf[q2]);
                    glyph(con_col, con_row, '_');
                }
                if (wifi_service()) {                                   // the network printed something: show the prompt and what was typed again"""),
])
open(root + 'wsh.bat', 'w', newline='\r\n').write('@echo off\ncd /d "%~dp0"\npowershell -NoProfile -ExecutionPolicy Bypass -File tools\\wsh.ps1 %*\n')
s = open(root + 'version.h').read().replace('#define WAVE_PATCH   "-004"', '#define WAVE_PATCH   "-005"')
open(root + 'version.h', 'w', newline='').write(s)

# the fake PC can send signed commands too (for testing): fakenet.send_rsh(command)
edit('fakehana/fakenet.py', [
("""    def dns(self, srcmac, sport, srcip, dstip, q):""",
"""    def send_rsh(self, command, key, seq):
        \"\"\"The fake PC sends a signed remote-shell command to the guest (UDP port 5150).\"\"\"
        sb = struct.pack('>Q', seq); cb = command.encode()
        mac = hmac.new(key, sb + cb, hashlib.sha256).digest()[:16]
        payload = b'WRSH' + sb + mac + cb
        u = struct.pack('>HHHH', 5141, 5150, 8 + len(payload), 0) + payload
        self.send(self.ip_packet(PC_IP, GUEST_IP, 17, u, getattr(self, 'guest_mac', GUEST_MAC), PC_MAC))

    def dns(self, srcmac, sport, srcip, dstip, q):"""),
])
print('ok')
