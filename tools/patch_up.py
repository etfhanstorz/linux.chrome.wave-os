import re
root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# build.sh: remember this PC's address for the `up` command
edit('build.sh', [(
"""${X}gcc -c -O2 -ffreestanding""",
"""# This PC's address (the Chromebook's `up` command fetches updates from it). Detected on every build; 0.0.0.0 if it cannot be found.
PCIP=$(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w tools/pcaddr.ps1)" 2>/dev/null | tr -d '\\r' | head -1)
python3 - "$PCIP" <<'EOF'
import sys
p = sys.argv[1].split('.')
ok = len(p) == 4 and all(x.isdigit() and int(x) < 256 for x in p)
v = (int(p[0]) << 24 | int(p[1]) << 16 | int(p[2]) << 8 | int(p[3])) if ok else 0
open('pc_addr.h', 'w').write('#define PC_ADDR_DEFAULT 0x%08xu   /* %s */\\n' % (v, sys.argv[1] if ok else 'unknown'))
EOF
${X}gcc -c -O2 -ffreestanding""")])

# update.h: saved address from the build, `up` / `upset`
edit('update.h', [
("""#include "update_key.h"
""",
"""#include "update_key.h"
#include "pc_addr.h"                                  // PC_ADDR_DEFAULT: this PC's address when the image was built
"""),
("""static u32 update_srv = 0;                            // set by the `update ADDRESS` command""",
"""static u32 update_srv = PC_ADDR_DEFAULT;              // where `up` fetches the new build from: the PC's address at build time; `upset ADDRESS` changes it (until the next reboot)"""),
("""static int wave_update(const char *arg) {
    if (arg && arg[0] && !parse_ip(arg, &update_srv)) { puts("usage: update ADDRESS   (the PC's address, like 192.168.1.50)\\n"); return 0; }
    if (!net_ip) { puts("the network is not up yet\\n"); return 0; }
    if (!update_srv) { puts("usage: update ADDRESS   (the PC's address, like 192.168.1.50)\\n"); return 0; }""",
"""// upset [ADDRESS]: show or change the PC address `up` uses
static int wave_upset(const char *arg) {
    if (arg && arg[0]) { u32 a = 0; if (!parse_ip(arg, &a) || !a) { sum_s("bad address"); return 0; } update_srv = a; }
    puts("up fetches updates from "); if (update_srv) put_ip(update_srv); else puts("(unknown: set it with  upset ADDRESS)"); putc('\\n');
    if (update_srv) { sum_s("pc "); sum_u(update_srv >> 24); sum_c('.'); sum_u(update_srv >> 16 & 255); sum_c('.'); sum_u(update_srv >> 8 & 255); sum_c('.'); sum_u(update_srv & 255); }
    return 1;
}

static int wave_update(const char *arg) {
    if (arg && arg[0] && !parse_ip(arg, &update_srv)) { puts("usage: up   (or up ADDRESS, or upset ADDRESS to change the saved address)\\n"); return 0; }
    if (!net_ip) { puts("the network is not up yet: run k first\\n"); return 0; }
    if (!update_srv) { puts("no PC address saved: type  upset ADDRESS  (the PC's address, like 192.168.1.50)\\n"); return 0; }""")])

# shell: up / upset (update stays as an alias)
edit('shell.h', [
("""    else if (streq(line, "update")) wave_update(arg);""",
"""    else if (streq(line, "up") || streq(line, "update")) wave_update(arg);
    else if (streq(line, "upset")) wave_upset(arg);"""),
("""streq(name, "ping") || streq(name, "rxtest")) {""",
"""streq(name, "ping") || streq(name, "rxtest") || streq(name, "up") || streq(name, "upset")) {"""),
("""commands: c f j k r ping rxtest""", """commands: c f j k r up upset ping rxtest"""),
])

# fake: queue packets for the 32 data ports instead of overwriting them (a real chip holds the rest back)
edit('fakehana/fakesdio.py', [
("""        self.partial = {}                # data port -> bytes not yet read (byte-mode reads come in 512-byte pieces)""",
"""        self.partial = {}                # data port -> bytes not yet read (byte-mode reads come in 512-byte pieces)
        self.pending = []                # received packets waiting for a free data port (the real chip's flow control)"""),
("""        self.data_q = {}; self.data_ptr = 0; self.partial = {}; self.assoc = None; self.keys_set = []""",
"""        self.data_q = {}; self.data_ptr = 0; self.partial = {}; self.assoc = None; self.keys_set = []; self.pending = []"""),
("""        pkt = struct.pack('<HH', 4 + len(rxpd) + len(frame), 0) + rxpd + frame
        port = self.data_ptr
        self.data_ptr = (self.data_ptr + 1) % 32
        self.data_q[port] = pkt
""",
"""        pkt = struct.pack('<HH', 4 + len(rxpd) + len(frame), 0) + rxpd + frame
        self.pending.append(pkt)
        self.fill_ports()

    def fill_ports(self):
        while self.pending and self.data_ptr not in self.data_q and self.data_ptr not in self.partial:
            self.data_q[self.data_ptr] = self.pending.pop(0)
            self.data_ptr = (self.data_ptr + 1) % 32
"""),
("""        if 0x04 <= r <= 0x07:                            # upload (receive) bitmap: bit p = data port p has a packet
            bm = sum""",
"""        if 0x04 <= r <= 0x07:                            # upload (receive) bitmap: bit p = data port p has a packet
            self.fill_ports()
            bm = sum"""),
])
print('ok')
