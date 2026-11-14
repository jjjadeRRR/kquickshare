#!/usr/bin/env python3
"""Exercise encrypted BLE-to-LAN migration with an independent protocol peer."""
import socket
import struct
import sys

from fake_sender import Sender, b, fields, offline, one, v


class UpgradeSender(Sender):
    def __init__(self, port, mode):
        self.mode = mode
        self.pending = []
        self.upgraded = False
        super().__init__('127.0.0.1', port)

    def recv_secure(self):
        return self.pending.pop(0) if self.pending else super().recv_secure()

    def read_until(self, expected):
        while True:
            frame = super().recv_secure()
            v1 = one(frame, 2)
            if one(v1, 1) == 4 and one(one(v1, 5), 1) == expected:
                return
            self.pending.append(frame)

    def handle_offer(self, v1):
        if self.mode == 'fallback':
            return
        if self.mode == 'decline':
            self.send(self.secure(offline(4, 5, v(1, 5))))
            return
        info = one(one(v1, 5), 2)
        address = one(info, 3)
        host_port = (socket.inet_ntoa(one(address, 1)), one(address, 2))
        if self.mode == 'wrong-id-first':
            wrong = socket.create_connection(host_port, timeout=8)
            wrong.settimeout(8)
            invalid = offline(4, 5, v(1, 4) + b(3, b(1, 'WXYZ')))
            wrong.sendall(struct.pack('!I', len(invalid)) + invalid)
            try:
                assert wrong.recv(1) == b''
            except ConnectionResetError:
                pass
            wrong.close()
        lan = socket.create_connection(host_port, timeout=8)
        lan.settimeout(8)
        intro = offline(4, 5, v(1, 4) + b(3, b(1, 'ABCD')))
        lan.sendall(struct.pack('!I', len(intro)) + intro)

        def exact(n):
            data = b''
            while len(data) < n:
                data += lan.recv(n - len(data))
            return data

        ack = exact(struct.unpack('!I', exact(4))[0])
        assert one(one(one(ack, 2), 5), 1) == 6
        self.read_until(2)
        self.send(self.secure(offline(4, 5, v(1, 2))))
        self.read_until(3)
        self.send(self.secure(offline(4, 5, v(1, 3))))
        assert one(one(self.recv(), 2), 1) == 6  # plaintext old-channel disconnection
        old = self.sock
        self.sock = lan
        old.close()
        self.upgraded = True

    def recv_sharing(self):
        content = b''
        while True:
            v1 = one(self.recv_secure(), 2)
            kind = one(v1, 1)
            if kind == 4:
                self.handle_offer(v1)
                continue
            if kind == 5:
                continue
            chunk = one(one(v1, 4), 3)
            content += fields(chunk).get(3, [b''])[0]
            if one(chunk, 1) & 1:
                frame = one(content, 2)
                return one(frame, 1), frame


sender = UpgradeSender(int(sys.argv[1]), sys.argv[2])
data = b'hello over the selected transport'
files = [(42, 'upgrade.txt', len(data))]
if sys.argv[2] == 'upgrade-multi':
    files.append((43, 'second.txt', len(b'second file')))
assert sender.introduce(files)[0] == 2
assert sender.upgraded == (sys.argv[2] in ('upgrade', 'wrong-id-first', 'upgrade-multi'))
sender.payload(42, 2, len(data), 0, data, True)
if sys.argv[2] == 'upgrade-multi':
    sender.payload(43, 2, len(b'second file'), 0, b'second file', True)
sender.close()
