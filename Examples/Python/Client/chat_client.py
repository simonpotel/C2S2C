"""Chat client (see Examples/C/Client) written with the Python binding.

Usage: chat_client.py <port> <nickname> <message>...
Logs in, sends each message, prints the broadcasts it receives until its own messages came back.
SPDX-License-Identifier: MIT
"""
import sys
import time

import c2s2c
from c2s2c import packets as reg


def main():
    if len(sys.argv) < 4:
        print("usage: chat_client.py <port> <nickname> <message>...", file=sys.stderr)
        return 2
    port, nickname, messages = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
    state = {"logged_in": False, "received": 0}

    def on_message(ctx, p):
        user_id, text, _timestamp = p.values()
        print(f"[{user_id}] {text}", flush=True)
        state["received"] += 1

    ctx = c2s2c.Context()
    ctx.on_packet(reg.LOGIN_OK, lambda c, p: state.update(logged_in=True))
    ctx.on_packet(reg.CHAT_MESSAGE, on_message)
    conn = ctx.connect("127.0.0.1", port)

    def wait_until(done, seconds=10):
        deadline = time.monotonic() + seconds
        while not done() and time.monotonic() < deadline:
            ctx.dispatch(50)
        return done()

    ctx.send(conn, reg.LOGIN, c2s2c.encode(reg.LOGIN, nickname))
    if not wait_until(lambda: state["logged_in"]):
        return 1
    for text in messages:
        ctx.send(conn, reg.CHAT_SEND, c2s2c.encode(reg.CHAT_SEND, text))
    ok = wait_until(lambda: state["received"] >= len(messages))
    ctx.close_connection(conn)
    for _ in range(20):  # let the close go out
        ctx.dispatch(10)
    ctx.close()
    return 0 if ok else 1


sys.exit(main())
