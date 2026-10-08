"""A fake WPA2-PSK router for the fake Chromebook: runs the router's half of the 4-way handshake with real cryptography
(PBKDF2 / PRF / HMAC-SHA1 / RFC 3394 key wrap), so wave-os's password handling is checked end to end.

Network = the name in wifi_local.h, password PASSWORD below (a test value, only used inside the fake). A wrong password makes the router
ignore message 2 (bad signature), exactly like a real one, so the host times out waiting for message 3.
"""
import hashlib, hmac, struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

import localcfg
AP_MAC = localcfg.BSSID
SSID = localcfg.SSID
PASSWORD = b'wavetest1234'
RSN_IE = bytes([48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0])


def prf(key, label, data, n):
    out = b''
    i = 0
    while len(out) < n:
        out += hmac.new(key, label + b'\0' + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:n]


def aes_wrap(kek, plain):
    """RFC 3394."""
    enc = Cipher(algorithms.AES(kek), modes.ECB()).encryptor()
    n = len(plain) // 8
    a = b'\xa6' * 8
    r = [plain[i * 8:(i + 1) * 8] for i in range(n)]
    for j in range(6):
        for i in range(n):
            b = enc.update(a + r[i])
            t = n * j + i + 1
            a = bytes(x ^ y for x, y in zip(b[:8], struct.pack('>Q', t)))
            r[i] = b[8:]
    return a + b''.join(r)


class FakeAP:
    def __init__(self, loader, password=PASSWORD):
        self.l = loader
        self.pmk = hashlib.pbkdf2_hmac('sha1', password, SSID, 4096, 32)
        self.anonce = bytes(range(1, 33))
        self.gtk = bytes(range(0x50, 0x60))
        self.state = 'idle'
        self.sta = None
        self.log = []
        self.done = False
        self.tk = None

    def _frame(self, info, keylen, replay, nonce, data, kck=None):
        body = bytes([2]) + struct.pack('>HH', info, keylen) + struct.pack('>Q', replay) + nonce + bytes(16) + bytes(8) + bytes(8) + bytes(16) + struct.pack('>H', len(data)) + data
        eapol = bytes([2, 3]) + struct.pack('>H', len(body)) + body
        if kck is not None:
            mic = hmac.new(kck, eapol, hashlib.sha1).digest()[:16]
            eapol = eapol[:4 + 77] + mic + eapol[4 + 93:]
        return eapol

    def start(self, sta):
        """The host associated: send handshake message 1."""
        self.sta = sta
        self.replay = 1
        self.state = 'm1'
        self.done = False
        self.log.append('assoc')
        self.l.push_data(sta, AP_MAC, 0x888e, self._frame(0x008a, 16, self.replay, self.anonce, b''))

    def on_eapol(self, src, eapol):
        if len(eapol) < 99 or eapol[1] != 3:
            return
        k = eapol[4:]
        info, replay = struct.unpack('>H', k[1:3])[0], struct.unpack('>Q', k[5:13])[0]
        nonce, mic, dlen = k[13:45], k[77:93], struct.unpack('>H', k[93:95])[0]
        data = k[95:95 + dlen]

        def mic_ok(kck):
            z = eapol[:4 + 77] + bytes(16) + eapol[4 + 93:]
            return hmac.new(kck, z, hashlib.sha1).digest()[:16] == mic

        if self.state == 'm1' and info == 0x010a and replay == self.replay:
            sta, ap = self.sta, AP_MAC
            lo, hi = (sta, ap) if sta < ap else (ap, sta)
            nlo, nhi = (self.anonce, nonce) if self.anonce < nonce else (nonce, self.anonce)
            ptk = prf(self.pmk, b'Pairwise key expansion', lo + hi + nlo + nhi, 48)
            if not mic_ok(ptk[:16]):
                self.log.append('msg2 bad mic (wrong password?)')
                return
            if data != RSN_IE:
                self.log.append('msg2 rsn mismatch')
                return
            self.ptk = ptk
            self.log.append('msg2 ok')
            self.replay += 1
            kde = bytes([0xdd, 22, 0x00, 0x0f, 0xac, 1, 1, 0]) + self.gtk
            plain = RSN_IE + kde
            plain += b'\xdd'                                   # padding: one 0xdd then zeros up to a multiple of 8
            while len(plain) % 8:
                plain += b'\0'
            wrapped = aes_wrap(ptk[16:32], plain)
            self.state = 'm3'
            self.l.push_data(sta, AP_MAC, 0x888e, self._frame(0x13ca, 16, self.replay, self.anonce, wrapped, kck=ptk[:16]))
        elif self.state == 'm3' and info == 0x030a and replay == self.replay:
            if not mic_ok(self.ptk[:16]):
                self.log.append('msg4 bad mic')
                return
            self.log.append('msg4 ok')
            self.state = 'done'
            self.done = True
            self.tk = self.ptk[32:48]
