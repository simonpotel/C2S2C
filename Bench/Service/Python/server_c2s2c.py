"""Benchmark service on the Python binding. Usage: server_c2s2c.py poll|dispatch
poll: replies to a whole batch with one send_many call; dispatch: Python handlers (on_packet)
run over the same batched poll. SPDX-License-Identifier: MIT"""
import sys

import c2s2c
from c2s2c import packets as reg

ctx = c2s2c.Context(max_conns=4096, max_payload=1024, idle_timeout_ms=1000)
ctx.listen("127.0.0.1", 0)
print(ctx.port, flush=True)
PING, PONG = reg.PING, reg.PONG

if sys.argv[1] == "dispatch":
    def on_ping(c, p):
        try:
            c.send(p.conn, PONG, p.payload())
        except c2s2c.Error:
            pass  # the client already left

    ctx.on_packet(PING, on_ping)
    while True:
        ctx.dispatch(100, 256)
else:
    while True:
        with ctx.poll(100, 256) as batch:
            replies = [(p.conn, PONG, p) for p in batch if p.id == PING]  # echo, no copy
            if replies:
                try:
                    ctx.send_many(replies)
                except c2s2c.Error:
                    pass  # the first client already left
