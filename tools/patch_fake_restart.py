p = '/mnt/c/!ab1/os/fakehana/fakesdio.py'
s = open(p, newline='').read().replace('\r\n', '\n')

def sub(old, new):
    global s
    assert old in s, old[:80]
    s = s.replace(old, new, 1)

# MODEL (real hana, measured with v1.53.3 rxtest): every CMD53 read of a data port starts at the beginning of the packet again.
sub("""            pt = addr - 0x10000
            if pt not in self.partial and pt in self.data_q:
                self.partial[pt] = self.data_q.pop(pt)
            buf = self.partial.get(pt, b'')
            out, rest = buf[:nbytes], buf[nbytes:]
            if rest: self.partial[pt] = rest
            else: self.partial.pop(pt, None)
            return out.ljust(nbytes, b'\\0')""",
"""            pt = addr - 0x10000
            if pt not in self.partial and pt in self.data_q:
                self.partial[pt] = self.data_q.pop(pt)
            buf = self.partial.get(pt, b'')
            self.reads = getattr(self, 'reads', 0) + 1
            if self.restart_model:
                return buf[:nbytes].ljust(nbytes, b'\\0')       # a second read of the same packet gets the packet's beginning again; the packet is released at the next bitmap check
            out, rest = buf[:nbytes], buf[nbytes:]
            if rest: self.partial[pt] = rest
            else: self.partial.pop(pt, None)
            return out.ljust(nbytes, b'\\0')""")
sub("""        if 0x04 <= r <= 0x07:                            # upload (receive) bitmap: bit p = data port p has a packet
            self.fill_ports()""",
"""        if 0x04 <= r <= 0x07:                            # upload (receive) bitmap: bit p = data port p has a packet
            if self.restart_model and r == 0x04: self.partial.clear()     # the packet the host just read is gone
            self.fill_ports()""")
sub("""        self.pending = []                # received packets waiting for a free data port (the real chip's flow control)""",
"""        self.pending = []                # received packets waiting for a free data port (the real chip's flow control)
        self.restart_model = True        # MODEL (real hana): see port_read""")
open(p, 'w', newline='').write(s)
print('ok')
