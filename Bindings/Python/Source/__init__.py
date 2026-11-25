"""Duplex: Python binding of the Duplex packet library (SPDX-License-Identifier: MIT).

In plain words: Context is one network engine. You listen or connect, then either loop on
``poll`` (packets arrive in batches and are given back when the ``with`` block ends) or register
handlers/hooks and call ``dispatch``. Payloads are built and read with ``encode`` / ``decode``
from the layouts in the packet registry. Errors are raised as ``duplex.Error``.
"""
import ctypes
import struct
import weakref

from . import _ffi

_lib = _ffi.load()
_ffi.declare(_lib)

__all__ = ["Context", "Error", "Packet", "encode", "decode", "SYS_CONNECTED", "SYS_DISCONNECTED"]

SYS_CONNECTED = 0x7F01
SYS_DISCONNECTED = 0x7F02
WOULD_BLOCK = -14


class Error(Exception):
    """A library error: ``code`` is the DX_ERR_* value (negative), ``str(e)`` its message."""

    def __init__(self, code):
        super().__init__(_lib.dx_strerror(code).decode())
        self.code = code


def _check(rc):
    if rc < 0:
        raise Error(rc)
    return rc


# ---- payload codec driven by the registry schemas ("u32,string,array<u16>") ----

_FIXED = {"u8": "B", "u16": "H", "u32": "I", "u64": "Q", "i8": "b", "i16": "h", "i32": "i",
          "i64": "q", "f32": "f", "f64": "d", "bool": "?"}


def _registry():
    import duplex_registry  # generated from Registry/protocol.toml
    return duplex_registry


def _schema(packet_id):
    try:
        return _registry().PACKETS[packet_id][1].split(",") if _registry().PACKETS[packet_id][1] else []
    except KeyError:
        raise Error(-13)  # DX_ERR_UNKNOWN_ID


def _write_value(out, kind, value):
    if kind in _FIXED:
        out += struct.pack("<" + _FIXED[kind], value)
    elif kind == "string":
        raw = value.encode("utf-8")
        out += struct.pack("<H", len(raw)) + raw
    elif kind == "bytes":
        out += struct.pack("<I", len(value)) + bytes(value)
    else:
        raise ValueError(f"unknown type {kind}")


def encode(packet_id, *values):
    """Builds the payload of a registry packet from Python values, one per field."""
    schema = _schema(packet_id)
    if len(values) != len(schema):
        raise ValueError(f"packet 0x{packet_id:04X} has {len(schema)} fields, got {len(values)}")
    out = bytearray()
    for kind, value in zip(schema, values):
        if kind.startswith("array<"):
            out += struct.pack("<H", len(value))
            for item in value:
                _write_value(out, kind[6:-1], item)
        else:
            _write_value(out, kind, value)
    return bytes(out)


def _read_value(data, pos, kind):
    if kind in _FIXED:
        fmt = "<" + _FIXED[kind]
        return struct.unpack_from(fmt, data, pos)[0], pos + struct.calcsize(fmt)
    if kind == "string":
        (n,) = struct.unpack_from("<H", data, pos)
        return bytes(data[pos + 2:pos + 2 + n]).decode("utf-8"), pos + 2 + n
    (n,) = struct.unpack_from("<I", data, pos)
    return bytes(data[pos + 4:pos + 4 + n]), pos + 4 + n


def decode(packet_id, data):
    """Reads the fields of a registry packet payload; returns a tuple of Python values."""
    values, pos = [], 0
    for kind in _schema(packet_id):
        if kind.startswith("array<"):
            (count,) = struct.unpack_from("<H", data, pos)
            pos += 2
            items = []
            for _ in range(count):
                item, pos = _read_value(data, pos, kind[6:-1])
                items.append(item)
            values.append(items)
        else:
            value, pos = _read_value(data, pos, kind)
            values.append(value)
    return tuple(values)


# ---- packets, batches, context ----

class Packet:
    """A received packet. ``data`` is a zero-copy view valid only until the batch is released."""

    def __init__(self, raw):
        self.conn, self.id, self.sequence = raw.conn, raw.packet_id, raw.sequence
        self._view = None
        if raw.length:
            buf = (ctypes.c_char * raw.length).from_address(raw.data)
            self._view = memoryview(buf).cast("B")
        else:
            self._view = memoryview(b"")

    @property
    def data(self):
        if self._view is None:
            raise ValueError("the packet was released: copy bytes(packet.data) to keep them")
        return self._view

    def values(self):
        """The decoded fields (registry packets only)."""
        return decode(self.id, self.data)

    @property
    def reason(self):
        """For DX_SYS_DISCONNECTED: 0 for a normal close, a negative DX_ERR_* otherwise."""
        return struct.unpack("<i", self.data)[0]

    def _invalidate(self):
        view, self._view = self._view, None
        if view is not None:
            try:
                view.release()
            except BufferError:  # the user still holds a slice: it will not be reachable via us
                pass


class _Batch:
    """Packets of one poll. Use it in a ``with`` block: they are released when it ends."""

    def __init__(self, ctx, packets):
        self._ctx, self.packets = ctx, packets

    def __iter__(self):
        return iter(self.packets)

    def __len__(self):
        return len(self.packets)

    def __getitem__(self, i):
        return self.packets[i]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()

    def release(self):
        if self.packets:
            for p in self.packets:
                p._invalidate()
            _lib.dx_release(self._ctx, len(self.packets))
            self.packets = []


class Context:
    """One network engine (use it from one thread). Keyword arguments set dx_config fields."""

    def __init__(self, **settings):
        cfg = _ffi.Config()
        _lib.dx_config_default(ctypes.byref(cfg))
        for key, value in settings.items():
            if not hasattr(cfg, key):
                raise TypeError(f"unknown setting {key!r}")
            setattr(cfg, key, value)
        if "tx_bytes" not in settings and "max_payload" in settings:
            cfg.tx_bytes = 4 * (cfg.max_payload + 14)
        self._ctx = _lib.dx_create(ctypes.byref(cfg))
        if not self._ctx:
            raise Error(-18)  # DX_ERR_NO_MEMORY: invalid settings or out of memory
        self._finalizer = weakref.finalize(self, _lib.dx_destroy, self._ctx)
        self._callbacks = {}  # keeps ctypes callbacks alive as long as the context

    def close(self):
        """Frees the context now (also done automatically when the object is collected)."""
        self._finalizer()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def listen(self, address="127.0.0.1", port=0):
        _check(_lib.dx_listen(self._ctx, address.encode(), port))

    @property
    def port(self):
        return _check(_lib.dx_listen_port(self._ctx))

    def connect(self, address, port):
        conn = ctypes.c_uint32()
        _check(_lib.dx_connect(self._ctx, address.encode(), port, ctypes.byref(conn)))
        return conn.value

    def send(self, conn, packet_id, data=b""):
        """Sends a payload (bytes). Raises Error(-14) if the send buffer is full."""
        _check(_lib.dx_send(self._ctx, conn, packet_id, bytes(data), len(data)))

    def try_send(self, conn, packet_id, data=b""):
        """Like send, but returns False instead of raising when the send buffer is full."""
        rc = _lib.dx_send(self._ctx, conn, packet_id, bytes(data), len(data))
        if rc == WOULD_BLOCK:
            return False
        _check(rc)
        return True

    def close_connection(self, conn):
        _check(_lib.dx_close(self._ctx, conn))

    def poll(self, timeout_ms=0, max_packets=64):
        """Returns a batch; use ``with ctx.poll(100) as batch:`` so it is released afterwards."""
        raw = (_ffi.Packet * max_packets)()
        n = _check(_lib.dx_poll(self._ctx, raw, max_packets, timeout_ms))
        return _Batch(self._ctx, [Packet(raw[i]) for i in range(n)])

    def on_packet(self, packet_id, fn):
        """Calls ``fn(ctx, packet)`` for packets with this id (0 = default); None removes it."""
        if fn is None:
            self._callbacks.pop(("h", packet_id), None)
            _check(_lib.dx_on_packet(self._ctx, packet_id, _ffi.HANDLER(), None))
            return

        def trampoline(_c, p, _u):
            fn(self, Packet(p.contents))

        cb = _ffi.HANDLER(trampoline)
        self._callbacks[("h", packet_id)] = cb
        _check(_lib.dx_on_packet(self._ctx, packet_id, cb, None))

    def set_hooks(self, on_connect=None, on_disconnect=None, on_error=None, before_send=None,
                  after_recv=None):
        """Installs hooks; before_send/after_recv return True to reject/drop the packet."""
        h = _ffi.Hooks()
        h.size = ctypes.sizeof(h)
        cbs = {}
        if on_connect:
            cbs["on_connect"] = _ffi.ON_CONNECT(lambda _c, conn, _u: on_connect(conn))
        if on_disconnect:
            cbs["on_disconnect"] = _ffi.ON_DISCONNECT(lambda _c, conn, r, _u: on_disconnect(conn, r))
        if on_error:
            cbs["on_error"] = _ffi.ON_ERROR(lambda _c, conn, e, _u: on_error(conn, e))
        if before_send:
            cbs["before_send"] = _ffi.BEFORE_SEND(
                lambda _c, conn, pid, d, n, _u: 1 if before_send(conn, pid, ctypes.string_at(d, n) if n else b"") else 0)
        if after_recv:
            cbs["after_recv"] = _ffi.AFTER_RECV(lambda _c, p, _u: 1 if after_recv(Packet(p.contents)) else 0)
        for name, cb in cbs.items():
            setattr(h, name, cb)
        self._callbacks["hooks"] = cbs
        _check(_lib.dx_set_hooks(self._ctx, ctypes.byref(h)))

    def dispatch(self, timeout_ms=0):
        """Like poll, but runs handlers and hooks, then releases. Returns the entries handled."""
        return _check(_lib.dx_dispatch(self._ctx, timeout_ms))
