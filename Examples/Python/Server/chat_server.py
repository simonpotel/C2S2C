"""Chat room server (see Examples/C/Server) written with the Python binding.

Same protocol and command line as the C version. Usage: chat_server.py [port]
The chosen port is printed on the first line. SPDX-License-Identifier: MIT
"""
import signal
import sys
import time

import duplex
import duplex_registry as reg

users = {}  # connection handle -> nickname
stop = False


def on_login(ctx, p):
    (nickname,) = p.values()
    users[p.conn] = nickname
    ctx.send(p.conn, reg.LOGIN_OK, duplex.encode(reg.LOGIN_OK, p.conn))  # the handle is the user id


def on_chat(ctx, p):
    if p.conn not in users:
        return  # must log in first
    (text,) = p.values()
    message = duplex.encode(reg.CHAT_MESSAGE, p.conn, text, int(time.time()))
    for conn in list(users):  # broadcast to everybody who is logged in
        ctx.send(conn, reg.CHAT_MESSAGE, message)


def main():
    global stop
    ctx = duplex.Context()
    ctx.listen("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 0)
    ctx.on_packet(reg.LOGIN, on_login)
    ctx.on_packet(reg.CHAT_SEND, on_chat)
    ctx.on_packet(reg.PING, lambda c, p: c.send(p.conn, reg.PONG, bytes(p.data)))  # same nonce back
    ctx.set_hooks(on_disconnect=lambda conn, reason: users.pop(conn, None))

    def request_stop(*_):
        global stop
        stop = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    print(ctx.port, flush=True)
    while not stop:
        ctx.dispatch(200)
    ctx.close()


main()
