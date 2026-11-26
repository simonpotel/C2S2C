"""Tests of the Python binding: errors, codec, release semantics, handlers and hooks.

Run with PYTHONPATH set to the installed layout and C2S2C_LIBRARY to the shared library
(CMake does both). SPDX-License-Identifier: MIT
"""
import time
import unittest

import c2s2c
import c2s2c_registry as reg


def spin(contexts, done, seconds=10):
    """Runs every context until done() is true (no fixed sleeps)."""
    deadline = time.monotonic() + seconds
    while not done() and time.monotonic() < deadline:
        for c in contexts:
            c.dispatch(1)
    return done()


def pair(**settings):
    server, client = c2s2c.Context(**settings), c2s2c.Context(**settings)
    server.listen("127.0.0.1", 0)
    return server, client, client.connect("127.0.0.1", server.port)


class Errors(unittest.TestCase):
    def test_error_carries_code_and_message(self):
        with c2s2c.Context() as ctx:
            with self.assertRaises(c2s2c.Error) as cm:
                ctx.send(12345, reg.PING, c2s2c.encode(reg.PING, 1))
            self.assertEqual(cm.exception.code, -20)
            self.assertEqual(str(cm.exception), "invalid connection")

    def test_invalid_settings(self):
        with self.assertRaises(c2s2c.Error):
            c2s2c.Context(rx_ring_bytes=100)  # smaller than two maximal packets
        with self.assertRaises(TypeError):
            c2s2c.Context(no_such_setting=1)

    def test_context_is_freed_by_with(self):
        with c2s2c.Context() as ctx:
            pass
        ctx.close()  # closing twice is harmless


class Codec(unittest.TestCase):
    def test_roundtrip_every_shape(self):
        data = c2s2c.encode(reg.CHAT_MESSAGE, 7, "héllo €", 2**40)
        self.assertEqual(c2s2c.decode(reg.CHAT_MESSAGE, data), (7, "héllo €", 2**40))
        data = c2s2c.encode(reg.USER_LIST, ["ann", "bob"])
        self.assertEqual(c2s2c.decode(reg.USER_LIST, data), (["ann", "bob"],))
        self.assertEqual(c2s2c.decode(reg.UPLOAD, c2s2c.encode(reg.UPLOAD, b"\x00\x01")), (b"\x00\x01",))

    def test_bytes_match_the_wire_format(self):
        self.assertEqual(c2s2c.encode(reg.PING, 0x01020304), bytes([4, 3, 2, 1]))
        self.assertEqual(c2s2c.encode(reg.LOGIN, "hi"), bytes([2, 0, 0x68, 0x69]))

    def test_wrong_field_count_and_unknown_packet(self):
        with self.assertRaises(ValueError):
            c2s2c.encode(reg.PING)
        with self.assertRaises(c2s2c.Error):
            c2s2c.encode(0x1234)

    def test_library_validates_what_python_sends(self):
        server, client, conn = pair()
        # Raw bytes that are not valid UTF-8: the C library refuses to send them.
        with self.assertRaises(c2s2c.Error) as cm:
            client.send(conn, reg.LOGIN, bytes([2, 0, 0xC0, 0x80]))
        self.assertEqual(cm.exception.code, -8)


class Release(unittest.TestCase):
    def test_packet_data_dies_with_the_batch(self):
        server, client, conn = pair()
        spin([server, client], lambda: False, 0.2)  # let the connection come up
        client.send(conn, reg.PING, c2s2c.encode(reg.PING, 42))
        kept = None
        deadline = time.monotonic() + 10
        while kept is None and time.monotonic() < deadline:
            client.dispatch(0)
            with server.poll(1) as batch:
                for p in batch:
                    if p.id == reg.PING:
                        self.assertEqual(p.values(), (42,))
                        kept = p
        self.assertIsNotNone(kept)
        with self.assertRaises(ValueError):  # released: no dangling pointer is reachable
            kept.data

    def test_small_queue_keeps_flowing_because_batches_release(self):
        server, client, conn = pair(rx_queue=8)
        got = sent = 0
        deadline = time.monotonic() + 15
        while got < 300 and time.monotonic() < deadline:
            if sent < 300 and client.try_send(conn, reg.PING, c2s2c.encode(reg.PING, sent)):
                sent += 1
            client.dispatch(0)
            with server.poll(1) as batch:
                got += sum(1 for p in batch if p.id == reg.PING)
        self.assertEqual(got, 300)


class HandlersAndHooks(unittest.TestCase):
    def test_handlers_hooks_and_flow_control(self):
        server, client, conn = pair()
        seen = {"pings": 0, "pongs": 0, "connects": 0, "dropped": 0, "rejected": 0, "chat": 0}

        def on_ping(ctx, p):
            seen["pings"] += 1
            ctx.send(p.conn, reg.PONG, bytes(p.data))  # reply from inside a handler

        server.on_packet(reg.PING, on_ping)
        client.on_packet(reg.PONG, lambda ctx, p: seen.__setitem__("pongs", seen["pongs"] + 1))
        server.on_packet(reg.CHAT_SEND, lambda ctx, p: seen.__setitem__("chat", seen["chat"] + 1))

        def drop_spam(p):
            if p.id == reg.CHAT_SEND and p.values()[0].startswith("spam"):
                seen["dropped"] += 1
                return True
            return False

        server.set_hooks(on_connect=lambda c: seen.__setitem__("connects", seen["connects"] + 1),
                         after_recv=drop_spam)
        client.set_hooks(before_send=lambda c, pid, data: seen.__setitem__("rejected", seen["rejected"] + 1) or pid == reg.LOGIN)

        self.assertTrue(spin([server, client], lambda: seen["connects"] == 1))
        client.send(conn, reg.PING, c2s2c.encode(reg.PING, 1))
        client.send(conn, reg.CHAT_SEND, c2s2c.encode(reg.CHAT_SEND, "spam spam"))
        client.send(conn, reg.CHAT_SEND, c2s2c.encode(reg.CHAT_SEND, "hello"))
        with self.assertRaises(c2s2c.Error) as cm:
            client.send(conn, reg.LOGIN, c2s2c.encode(reg.LOGIN, "me"))
        self.assertEqual(cm.exception.code, -22)  # vetoed by before_send
        self.assertTrue(spin([server, client], lambda: seen["pongs"] == 1 and seen["chat"] == 1))
        self.assertEqual((seen["pings"], seen["dropped"], seen["chat"]), (1, 1, 1))

        # A full send buffer is reported as False by try_send (nobody serves the other side).
        payload = c2s2c.encode(reg.CHAT_SEND, "x" * 1000)
        full = False
        for _ in range(200000):
            if not client.try_send(conn, reg.CHAT_SEND, payload):
                full = True
                break
        self.assertTrue(full)

    def test_disconnect_reason(self):
        server, client, conn = pair()
        events = []
        server.set_hooks(on_disconnect=lambda c, r: events.append(r))
        spin([server, client], lambda: False, 0.2)
        client.close_connection(conn)
        self.assertTrue(spin([server, client], lambda: events == [0]))


if __name__ == "__main__":
    unittest.main(verbosity=0)
