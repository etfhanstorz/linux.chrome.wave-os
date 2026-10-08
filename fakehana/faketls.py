"""A real HTTPS server for the fake PC (port 443): Python's ssl module (OpenSSL) speaks TLS over the fake LAN, so wave-os's own TLS client
is tested against a real implementation. It serves the same pages as the fake web server on port 80, with a fresh self-signed certificate."""
import datetime, os, ssl, struct, tempfile


def make_context():
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'secure.test')])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key()).serial_number(1)
            .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName('secure.test'), x509.DNSName('example.com')]), False)
            .sign(key, hashes.SHA256()))
    d = tempfile.mkdtemp()
    cf, kf = os.path.join(d, 'c.pem'), os.path.join(d, 'k.pem')
    open(cf, 'wb').write(cert.public_bytes(serialization.Encoding.PEM))
    open(kf, 'wb').write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cf, kf)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx


class FakeTLS:
    def __init__(self, net):
        self.net = net
        self.conns = {}
        self.ctx = make_context()
        self.log = []

    def segment(self, srcmac, p):
        sport, dport, seq, ack, off, fl = struct.unpack('>HHIIBB', p[:14])
        hl = (off >> 4) * 4
        data = bytes(p[hl:])
        c = self.conns.get(sport)
        if fl & 2:                                                   # SYN
            mss, i = 1460, 20
            while i + 1 < hl:
                if p[i] == 0: break
                if p[i] == 1: i += 1; continue
                if p[i] == 2 and p[i + 1] == 4: mss = struct.unpack('>H', p[i + 2:i + 4])[0]
                i += max(2, p[i + 1])
            inc, out = ssl.MemoryBIO(), ssl.MemoryBIO()
            c = self.conns[sport] = dict(snd=9000, rcv=seq + 1, mss=mss, inc=inc, out=out, obj=self.ctx.wrap_bio(inc, out, server_side=True),
                                         hs=False, req=b'', done=False, fin=False)
            self.net.send(self.net.tcp_packet(443, sport, c['snd'], c['rcv'], 18, mss=True))
            c['snd'] += 1
            return
        if not c:
            return
        if data:
            if seq != c['rcv']:                                      # a copy of something we already have: just acknowledge again
                self.net.send(self.net.tcp_packet(443, sport, c['snd'], c['rcv'], 16))
                return
            c['rcv'] += len(data)
            c['inc'].write(data)
            self.pump(c, sport)
        if fl & 1:
            c['rcv'] = seq + len(data) + 1
            self.net.send(self.net.tcp_packet(443, sport, c['snd'], c['rcv'], 16))

    def pump(self, c, sport):
        obj = c['obj']
        try:
            if not c['hs']:
                obj.do_handshake()
                c['hs'] = True
                self.log.append('handshake ok: %s %s' % (obj.version(), obj.cipher()[0]))
        except ssl.SSLWantReadError:
            pass
        except ssl.SSLError as e:
            self.log.append('tls error: %s' % e)
        if c['hs'] and not c['done']:
            try:
                c['req'] += obj.read(65536)
            except ssl.SSLWantReadError:
                pass
            except ssl.SSLError as e:
                self.log.append('tls read error: %s' % e)
            if b'\r\n\r\n' in c['req']:
                path = c['req'].split(b' ')[1].decode(errors='replace')
                status, extra, body = self.net.web.get(path, (404, '', b'<html><body><h1>Not Found</h1></body></html>'))
                resp = ('HTTP/1.0 %d X\r\nContent-Type: text/html\r\n%sContent-Length: %d\r\n\r\n' % (status, extra, len(body))).encode() + body
                obj.write(resp)
                self.log.append('https GET %s -> %d bytes' % (path, len(resp)))
                c['done'] = True
                try:
                    obj.unwrap()                                     # close_notify
                except (ssl.SSLWantReadError, ssl.SSLError):
                    pass
        out = c['out'].read()
        m = c['mss']
        if not out:
            self.net.send(self.net.tcp_packet(443, sport, c['snd'], c['rcv'], 16))
        for i in range(0, len(out), m):
            chunk = out[i:i + m]
            self.net.send(self.net.tcp_packet(443, sport, c['snd'], c['rcv'], 24, chunk))
            c['snd'] += len(chunk)
        if c['done'] and not c['fin']:
            self.net.send(self.net.tcp_packet(443, sport, c['snd'], c['rcv'], 17))
            c['snd'] += 1
            c['fin'] = True
