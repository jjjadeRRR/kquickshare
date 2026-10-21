#!/usr/bin/env python3
"""Independent encrypted Quick Share sender for local integration tests.
Uses Python cryptography, not the receiver's frame/crypto implementation.
Run: python tests/fake_sender.py --binary build/kquickshare-cli
"""
import argparse
import hashlib
import hmac
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives import hashes, padding, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF


def varint(value):
    value &= (1 << 64) - 1
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def v(number, value):
    return varint(number << 3) + varint(value)


def b(number, value):
    if isinstance(value, str):
        value = value.encode()
    return varint(number << 3 | 2) + varint(len(value)) + value


def fields(data):
    pos = 0
    def read():
        nonlocal pos
        value = shift = 0
        while True:
            byte = data[pos]; pos += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
            if shift > 63:
                raise ValueError('invalid varint')
    out = {}
    while pos < len(data):
        tag = read(); wire = tag & 7
        if wire == 0:
            value = read()
        elif wire == 2:
            n = read(); value = data[pos:pos+n]; pos += n
        else:
            raise ValueError(f'unsupported wire {wire}')
        out.setdefault(tag >> 3, []).append(value)
    return out


def one(data, number):
    return fields(data)[number][0]


def offline(kind, field, content):
    return v(1, 1) + b(2, v(1, kind) + b(field, content))


def sharing(kind, field, content):
    return v(1, 1) + b(2, v(1, kind) + b(field, content))


def hkdf(key, salt, info):
    return HKDF(hashes.SHA256(), 32, salt, info).derive(key)


class Sender:
    def __init__(self, host, port, name='Test Android'):
        self.sock = socket.create_connection((host, port), timeout=8)
        self.sock.settimeout(8)
        self.sent = self.received = 0
        self.payload_id = 1000
        endpoint = bytes([2]) + os.urandom(16) + bytes([len(name.encode())]) + name.encode()
        self.send(offline(1, 2, b(1, 'ABCD') + b(2, name) + b(6, endpoint)))
        key = ec.generate_private_key(ec.SECP256R1())
        point = key.public_key().public_numbers()
        def coord(n):
            data = n.to_bytes((n.bit_length()+7)//8, 'big')
            return b'\0' + data if data[0] & 128 else data
        public = v(1, 1) + b(2, b(1, coord(point.x)) + b(2, coord(point.y)))
        finish = v(1, 4) + b(2, b(1, public))
        init = v(1, 2) + b(2, v(1, 1) + b(2, os.urandom(32)) + b(3, v(1, 100) + b(2, hashlib.sha512(finish).digest())) + b(4, 'AES_256_CBC-HMAC_SHA256'))
        self.send(init)
        server_init = self.recv()
        assert one(server_init, 1) == 3
        peer = one(one(one(server_init, 2), 4), 2)
        x, y = int.from_bytes(one(peer, 1), 'big'), int.from_bytes(one(peer, 2), 'big')
        peer_key = ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()
        dhs = hashlib.sha256(key.exchange(ec.ECDH(), peer_key)).digest()
        secret = hkdf(dhs, b'UKEY2 v1 next', init + server_init)
        salt = hashlib.sha256(b'D2D').digest()
        client, server = hkdf(secret, salt, b'client'), hkdf(secret, salt, b'server')
        salt = hashlib.sha256(b'SecureMessage').digest()
        self.enc, self.mac = hkdf(client, salt, b'ENC:2'), hkdf(client, salt, b'SIG:1')
        self.dec, self.verify = hkdf(server, salt, b'ENC:2'), hkdf(server, salt, b'SIG:1')
        self.send(finish)
        self.send(offline(2, 3, v(1, 0) + v(3, 1)))
        assert one(one(self.recv(), 2), 1) == 2
        assert self.recv_sharing()[0] == 3
        self.send_sharing(sharing(3, 4, b(1, os.urandom(72)) + b(2, os.urandom(6))))
        assert self.recv_sharing()[0] == 4
        self.send_sharing(sharing(4, 5, v(1, 3)))

    def close(self):
        self.sock.close()

    def send(self, data):
        self.sock.sendall(struct.pack('!I', len(data)) + data)

    def recv(self):
        def exact(n):
            out = b''
            while len(out) < n:
                chunk = self.sock.recv(n-len(out))
                if not chunk:
                    raise EOFError()
                out += chunk
            return out
        return exact(struct.unpack('!I', exact(4))[0])

    def secure(self, data):
        self.sent += 1
        plain = b(1, data) + v(2, self.sent)
        pad = padding.PKCS7(128).padder(); plain = pad.update(plain) + pad.finalize()
        iv = os.urandom(16)
        enc = Cipher(algorithms.AES(self.enc), modes.CBC(iv)).encryptor()
        header = v(1, 1) + v(2, 2) + b(5, iv) + b(6, v(1, 13) + v(2, 1))
        hab = b(1, header) + b(2, enc.update(plain) + enc.finalize())
        return b(1, hab) + b(2, hmac.digest(self.mac, hab, 'sha256'))

    def recv_secure(self):
        msg = self.recv(); hab = one(msg, 1)
        assert hmac.compare_digest(one(msg, 2), hmac.digest(self.verify, hab, 'sha256'))
        iv = one(one(hab, 1), 5)
        dec = Cipher(algorithms.AES(self.dec), modes.CBC(iv)).decryptor()
        padded = dec.update(one(hab, 2)) + dec.finalize()
        unpad = padding.PKCS7(128).unpadder(); plain = unpad.update(padded) + unpad.finalize()
        self.received += 1
        assert one(plain, 2) == self.received
        return one(plain, 1)

    def payload(self, ident, kind, total, offset, body, last=False):
        header = v(1, ident) + v(2, kind) + v(3, total)
        chunk = v(1, int(last)) + v(2, offset) + b(3, body)
        self.send(self.secure(offline(3, 4, v(1, 1) + b(2, header) + b(3, chunk))))

    def send_sharing(self, data):
        self.payload_id += 1
        self.payload(self.payload_id, 1, len(data), 0, data)
        self.payload(self.payload_id, 1, len(data), len(data), b'', True)

    def recv_sharing(self):
        buffer = b''
        while True:
            raw = one(self.recv_secure(), 2)
            kind = one(raw, 1)
            if kind == 5:
                continue
            pt = one(raw, 4); chunk = one(pt, 3)
            buffer += fields(chunk).get(3, [b''])[0]
            if one(chunk, 1) & 1:
                frame = one(buffer, 2)
                return one(frame, 1), frame

    def introduce(self, files):
        intro = b''
        for ident, name, size in files:
            intro += b(1, b(1, name) + v(2, 1) + v(3, ident) + v(4, size) + b(5, 'application/octet-stream'))
        self.send_sharing(sharing(1, 2, intro))
        return self.recv_sharing()


BINARY = None
class Transfers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dest = Path(self.temp.name)
        self.process = subprocess.Popen([BINARY, '--dest', str(self.dest), '--once'], stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, env={**os.environ, 'QT_FORCE_STDERR_LOGGING': '1'})
        self.addCleanup(self.cleanup_process)
        ready, _, _ = select.select([self.process.stderr], [], [], 8)
        self.assertTrue(ready, 'receiver did not start')
        line = self.process.stderr.readline().decode()
        match = re.search(r'порту (\d+)', line)
        self.assertIsNotNone(match, line)
        self.sender = Sender('127.0.0.1', int(match[1]))

    def cleanup_process(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.communicate(timeout=8)
        self.temp.cleanup()

    def tearDown(self):
        if hasattr(self, "sender"): self.sender.close()

    def wait(self):
        self.process.wait(timeout=8)
        self.assertFalse(list(self.dest.glob('*.part')))
        self.assertFalse(list(self.dest.glob('.kquickshare-*')))

    def test_files_and_collision(self):
        (self.dest / 'hello.txt').write_bytes(b'original')
        data = os.urandom(140000)
        kind, response = self.sender.introduce([(10, '../hello.txt', len(data)), (11, 'empty', 0)])
        self.assertEqual(one(one(response, 3), 1), 1)
        for offset in range(0, len(data), 32768):
            self.sender.payload(10, 2, len(data), offset, data[offset:offset+32768])
        self.sender.payload(10, 2, len(data), len(data), b'', True)
        self.sender.payload(11, 2, 0, 0, b'', True)
        self.wait()
        self.assertEqual((self.dest / 'hello (1).txt').read_bytes(), data)
        self.assertEqual((self.dest / 'hello.txt').read_bytes(), b'original')
        self.assertEqual((self.dest / 'empty').read_bytes(), b'')

    def test_last_chunk_with_body(self):
        self.sender.introduce([(10, 'last.txt', 5)])
        self.sender.payload(10, 2, 5, 0, b'hello', True)
        self.wait()
        self.assertEqual((self.dest / 'last.txt').read_bytes(), b'hello')

    def test_truncated_file(self):
        self.sender.introduce([(10, 'truncated', 100)])
        self.sender.payload(10, 2, 100, 0, b'bad', True)
        self.wait(); self.assertEqual(list(self.dest.iterdir()), [])

    def test_disconnect_removes_partial(self):
        self.sender.introduce([(10, 'partial', 100)])
        self.sender.payload(10, 2, 100, 0, b'bad')
        self.sender.close(); self.wait(); self.assertEqual(list(self.dest.iterdir()), [])

    def test_duplicate_payload_id(self):
        self.sender.send_sharing(sharing(1, 2, b(1, b(1, 'a') + v(3, 10) + v(4, 1)) + b(1, b(1, 'b') + v(3, 10) + v(4, 1))))
        self.wait(); self.assertEqual(list(self.dest.iterdir()), [])

    def test_bad_offset(self):
        self.sender.introduce([(10, 'offset', 5)])
        self.sender.payload(10, 2, 5, 2, b'hello', True)
        self.wait(); self.assertEqual(list(self.dest.iterdir()), [])

    def test_tampered_hmac(self):
        self.sender.introduce([(10, 'tampered', 5)])
        packet = bytearray(self.sender.secure(offline(5, 6, v(1, 0))))
        packet[-1] ^= 1
        self.sender.send(packet); self.wait(); self.assertEqual(list(self.dest.iterdir()), [])

    def test_negative_file_size(self):
        self.sender.send_sharing(sharing(1, 2, b(1, b(1, 'negative') + v(3, 10) + v(4, -1))))
        self.wait(); self.assertEqual(list(self.dest.iterdir()), [])

    def test_dot_filename(self):
        self.sender.introduce([(10, '..', 1)])
        self.sender.payload(10, 2, 1, 0, b'x', True)
        self.wait(); self.assertEqual((self.dest / 'unnamed').read_bytes(), b'x')

    def test_text(self):
        text = 'Привет из Quick Share'.encode()
        self.sender.send_sharing(sharing(1, 2, b(2, b(2, 'Shared text') + v(3, 1) + v(4, 20) + v(5, len(text)))))
        self.sender.recv_sharing()
        self.sender.payload(20, 1, len(text), 0, text, True)
        self.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='build/kquickshare-cli')
    args, remaining = parser.parse_known_args()
    BINARY = str(Path(args.binary).resolve())
    unittest.main(argv=['fake_sender'] + remaining)
