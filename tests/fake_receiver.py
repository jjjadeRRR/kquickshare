#!/usr/bin/env python3
"""Independent Quick Share receiver to verify the C++ outbound implementation."""
import argparse
import hashlib
import os
from pathlib import Path
import socket
import select
import subprocess
import tempfile
import threading
import unittest
from cryptography.hazmat.primitives.asymmetric import ec
from fake_sender import Sender, b, v, one, fields, hkdf, sharing, offline

BINARY = None

class Receiver:
    def __init__(self, server, mode='accept'):
        sock, _ = server.accept(); sock.settimeout(8)
        self.peer = Sender.__new__(Sender); self.peer.sock = sock; self.peer.sent = self.peer.received = 0; self.peer.payload_id = 2000
        self.files = {}; self.text = None
        p = self.peer
        request = p.recv(); assert one(one(request, 2), 1) == 1
        init = p.recv(); assert one(init, 1) == 2
        commitment = one(one(one(init, 2), 3), 2)
        private = ec.generate_private_key(ec.SECP256R1()); point = private.public_key().public_numbers()
        def coord(n):
            data = n.to_bytes((n.bit_length()+7)//8, 'big')
            return b'\0' + data if data[0] & 128 else data
        public = v(1, 1) + b(2, b(1, coord(point.x)) + b(2, coord(point.y)))
        server_init = v(1, 3) + b(2, v(1, 1) + b(2, os.urandom(32)) + v(3, 100) + b(4, public))
        p.send(server_init)
        finish = p.recv(); assert hashlib.sha512(finish).digest() == commitment
        peer = one(one(one(finish, 2), 1), 2)
        x, y = int.from_bytes(one(peer, 1), 'big'), int.from_bytes(one(peer, 2), 'big')
        pub = ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()
        dhs = hashlib.sha256(private.exchange(ec.ECDH(), pub)).digest()
        secret = hkdf(dhs, b'UKEY2 v1 next', init + server_init)
        salt = hashlib.sha256(b'D2D').digest(); client, serverkey = hkdf(secret, salt, b'client'), hkdf(secret, salt, b'server')
        salt = hashlib.sha256(b'SecureMessage').digest()
        p.enc, p.mac = hkdf(serverkey, salt, b'ENC:2'), hkdf(serverkey, salt, b'SIG:1')
        p.dec, p.verify = hkdf(client, salt, b'ENC:2'), hkdf(client, salt, b'SIG:1')
        assert one(one(p.recv(), 2), 1) == 2
        p.send(offline(2, 3, v(1, 0) + v(3, 2 if mode == 'connection_reject' else 1)))
        if mode == 'connection_reject': sock.close(); return
        p.send_sharing(sharing(3, 4, b(1, os.urandom(72)) + b(2, os.urandom(6))))
        assert p.recv_sharing()[0] == 3
        p.send_sharing(sharing(4, 5, v(1, 3))); assert p.recv_sharing()[0] == 4
        kind, intro = p.recv_sharing(); assert kind == 1
        if mode == 'tamper':
            packet = bytearray(p.secure(offline(5, 6, v(1, 0)))); packet[-1] ^= 1
            p.send(packet); sock.close(); return
        p.send_sharing(sharing(2, 3, v(1, 2 if mode == 'reject' else 1)))
        if mode == 'reject': sock.close(); return
        metadata = fields(one(intro, 2))
        expected = {}
        for meta in metadata.get(1, []):
            expected[one(meta, 3)] = (one(meta, 1).decode(), one(meta, 4))
        textmeta = metadata.get(2, [])
        if textmeta:
            assert one(textmeta[0], 3) == 1
            expected[one(textmeta[0], 4)] = (None, one(textmeta[0], 5))
        received = {ident: bytearray() for ident in expected}
        complete = set()
        while len(complete) < len(expected):
            frame = one(p.recv_secure(), 2)
            if one(frame, 1) == 5: continue
            assert one(frame, 1) == 3
            payload = one(frame, 4); header, chunk = one(payload, 2), one(payload, 3)
            ident = one(header, 1); name, size = expected[ident]
            assert one(header, 3) == size
            assert one(chunk, 2) == len(received[ident])
            received[ident] += fields(chunk).get(3, [b''])[0]
            if one(chunk, 1) & 1:
                assert len(received[ident]) == size
                complete.add(ident)
        for ident, data in received.items():
            name, size = expected[ident]
            if name is None: self.text = bytes(data).decode()
            else: self.files[name] = bytes(data)
        if mode in ('slow_finish', 'late_cancel'):
            # Reaching 100% is not a peer decision. It may still be committing
            # the attachment or reporting a final error. No sender goodbye/EOF
            # is allowed to interrupt this phase.
            readable, _, _ = select.select([sock], [], [], 0.35)
            assert not readable, 'Sender closed before receiver finished'
            if mode == 'late_cancel':
                p.send_sharing(sharing(6, 7, b''))
        # Receiver confirms completion by disconnecting only after full data.
        p.send(p.secure(offline(6, 7, b'')))
        sock.close()


class OutboundTests(unittest.TestCase):
    def run_transfer(self, args, mode='accept'):
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0)); server.listen(); server.settimeout(8)
            result = []; errors = []
            def receive():
                try: result.append(Receiver(server, mode))
                except BaseException as error: errors.append(error)
            thread = threading.Thread(target=receive, daemon=True); thread.start()
            process = subprocess.run([BINARY, '--host', '127.0.0.1', '--port', str(server.getsockname()[1]), *args], capture_output=True, timeout=15, env={**os.environ, 'QT_FORCE_STDERR_LOGGING':'1'})
            thread.join(8); self.assertFalse(thread.is_alive())
            self.assertFalse(errors, repr(errors))
            self.assertEqual(process.returncode, 0 if mode in ('accept', 'slow_finish') else 1, process.stderr.decode())
            return result[0]

    def test_files(self):
        with tempfile.TemporaryDirectory() as directory:
            files = {'test space.txt': os.urandom(2_100_000), 'пустой.txt': b'', 'ещё.txt': b'hello'}
            paths = []
            for name, data in files.items():
                path = Path(directory)/name; path.write_bytes(data); paths.append(str(path))
            result = self.run_transfer(['--', *paths])
            self.assertEqual(result.files, files)

    def test_text(self):
        text = 'Привет!\n"$()`; & < > — полный текст\n' * 100
        self.assertEqual(self.run_transfer(['--text', text]).text, text)

    def test_slow_finalization(self):
        self.assertEqual(self.run_transfer(['--text', 'slow commit'], 'slow_finish').text, 'slow commit')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'slow.bin'
            data = os.urandom(1_100_000)
            path.write_bytes(data)
            self.assertEqual(self.run_transfer(['--', str(path)], 'slow_finish').files, {'slow.bin': data})

    def test_cancel_after_100_percent(self):
        self.run_transfer(['--text', 'late error'], 'late_cancel')

    def test_rejected(self): self.run_transfer(['--text', 'test'], 'reject')
    def test_connection_rejected(self): self.run_transfer(['--text', 'test'], 'connection_reject')
    def test_tampered(self): self.run_transfer(['--text', 'test'], 'tamper')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--binary', default='build/kquickshare-send')
    args, remaining = parser.parse_known_args(); BINARY = str(Path(args.binary).resolve())
    unittest.main(argv=['fake_receiver'] + remaining)
