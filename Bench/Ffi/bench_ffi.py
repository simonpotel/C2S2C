"""Cost of the Python binding: pipelined ping/pong round trips per second (server and client in
this one process, 32 pings in flight, one second). SPDX-License-Identifier: MIT"""
import time

import duplex
import duplex_registry as reg

server, client = duplex.Context(), duplex.Context()
server.listen("127.0.0.1", 0)
conn = client.connect("127.0.0.1", server.port)
server.on_packet(reg.PING, lambda c, p: c.send(p.conn, reg.PONG, bytes(p.data)))
pongs = 0


def on_pong(ctx, p):
    global pongs
    pongs += 1


client.on_packet(reg.PONG, on_pong)
payload = duplex.encode(reg.PING, 1)
inflight = total = 0
t0 = time.monotonic()
while time.monotonic() - t0 < 1.0:
    while inflight < 32 and client.try_send(conn, reg.PING, payload):
        inflight += 1
    server.dispatch(0)
    before = pongs
    client.dispatch(0)
    inflight -= pongs - before
elapsed = time.monotonic() - t0
print(f"python_roundtrips {pongs / elapsed:.0f} packets/s")
