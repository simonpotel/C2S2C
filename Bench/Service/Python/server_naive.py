"""Benchmark service written from scratch in Python (no C2S2C): what you write when you recode
the protocol yourself. It checks everything the C2S2C library checks for this packet: header
fields, direction, length limit, sequence numbers, packet id and payload size, and drops idle
connections after one second. SPDX-License-Identifier: MIT"""
import selectors
import socket
import struct
import time

MAGIC, PING, PONG, MAX_PAYLOAD, IDLE = 0xC0DE, 0x0003, 0x8003, 1024, 1.0
HEADER = struct.Struct("<HBBHII")

sel = selectors.DefaultSelector()
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(1024)
listener.setblocking(False)
sel.register(listener, selectors.EVENT_READ, None)
print(listener.getsockname()[1], flush=True)


class Conn:
    def __init__(self, sock):
        self.sock, self.buf, self.rx_seq, self.tx_seq, self.last = sock, bytearray(), 0, 0, time.monotonic()
        self.out = bytearray()


def close(c):
    sel.unregister(c.sock)
    c.sock.close()


def process(c):
    """Cuts frames out of c.buf; returns False if the connection must be closed."""
    while len(c.buf) >= 14:
        magic, version, flags, pid, length, seq = HEADER.unpack_from(c.buf)
        if magic != MAGIC or version != 1 or flags & 0xFC:
            return False
        if (flags & 1) or (pid & 0x8000):          # a client may only send CTOS
            return False
        if length > MAX_PAYLOAD or seq != c.rx_seq:
            return False
        if len(c.buf) < 14 + length:
            return True
        if pid != PING or length != 4:             # unknown id, or wrong payload size
            return False
        c.out += HEADER.pack(MAGIC, 1, 1, PONG, 4, c.tx_seq) + c.buf[14:18]
        c.tx_seq += 1
        c.rx_seq += 1
        del c.buf[:18]
    return True


last_scan = time.monotonic()
while True:
    for key, _ in sel.select(0.1):
        if key.data is None:
            try:
                while True:
                    s, _ = listener.accept()
                    s.setblocking(False)
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    sel.register(s, selectors.EVENT_READ, Conn(s))
            except BlockingIOError:
                pass
            continue
        c = key.data
        try:
            data = c.sock.recv(65536)
        except BlockingIOError:
            continue
        except ConnectionError:
            data = b""
        if not data:
            close(c)
            continue
        c.last = time.monotonic()
        c.buf += data
        if not process(c):
            close(c)
            continue
        if c.out:
            try:
                sent = c.sock.send(c.out)
                del c.out[:sent]
            except (BlockingIOError, ConnectionError):
                pass
    now = time.monotonic()
    if now - last_scan > 0.25:
        last_scan = now
        for key in list(sel.get_map().values()):
            if key.data is not None and now - key.data.last > IDLE:
                close(key.data)
