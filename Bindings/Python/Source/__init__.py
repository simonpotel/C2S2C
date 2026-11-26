"""Duplex: Python binding of the Duplex packet library (SPDX-License-Identifier: MIT).

Context is one network engine. You listen or connect, then either loop on
``poll`` (packets arrive in batches and are given back when the ``with`` block ends) or register
handlers/hooks and call ``dispatch`` (they run in Python over one batched poll). Payloads are built and read with ``encode`` / ``decode``
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
    """A received packet. ``data`` is a zero-copy view valid only until the batch is released;
    ``payload()`` returns a copy as bytes."""

    __slots__ = ("conn", "id", "sequence", "_ptr", "_len", "_batch")

    def __init__(self, conn, packet_id, sequence, ptr, length, batch):
        self.conn, self.id, self.sequence = conn, packet_id, sequence
        self._ptr, self._len, self._batch = ptr, length, batch

    @property
    def data(self):
        if self._batch.released:
            raise ValueError("the packet was released: copy it with packet.payload() to keep it")
        if not self._len:
            return memoryview(b"")
        view = memoryview((ctypes.c_char * self._len).from_address(self._ptr)).cast("B")
        self._batch.views.append(view)  # invalidated on release
        return view

    def payload(self):
        """The payload as bytes (a copy, so it stays valid after the release)."""
        if self._batch.released:
            raise ValueError("the packet was released")
        return ctypes.string_at(self._ptr, self._len) if self._len else b""

    def values(self):
        """The decoded fields (registry packets only)."""
        return decode(self.id, self.payload())

    @property
    def reason(self):
        """For DX_SYS_DISCONNECTED: 0 for a normal close, a negative DX_ERR_* otherwise."""
        return struct.unpack("<i", self.payload())[0]


# The array of dx_packet that dx_poll fills is decoded in one go with struct (much faster than
# reading ctypes fields one by one): conn, sequence, data pointer, length, id, flags, reserved.
_PTR = "Q" if ctypes.sizeof(ctypes.c_void_p) == 8 else "I"
_RECORD = struct.Struct("<II" + _PTR + "IHBB" + ("4x" if ctypes.sizeof(_ffi.Packet) == 28 else ""))
assert _RECORD.size == ctypes.sizeof(_ffi.Packet), "dx_packet layout mismatch"
_OUT = struct.Struct("<IHH" + _PTR + "I" + ("4x" if _PTR == "Q" else ""))  # conn, id, reserved, data, length
assert _OUT.size == ctypes.sizeof(_ffi.Outgoing), "dx_outgoing layout mismatch"


class _Batch:
    """Packets of one poll. Use it in a ``with`` block: they are released when it ends."""

    def __init__(self, ctx, raw, n):
        self._ctx, self.released, self.views = ctx, False, []
        mv = memoryview(raw).cast("B")[: n * _RECORD.size]
        self.packets = [Packet(c, i, s, ptr, ln, self) for c, s, ptr, ln, i, _f, _r in _RECORD.iter_unpack(mv)]

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
        if self.released:
            return
        self.released = True
        for view in self.views:
            try:
                view.release()
            except BufferError:  # the user still holds a slice: it will not be reachable via us
                pass
        if self.packets:
            _lib.dx_release(self._ctx, len(self.packets))


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
        self._handlers = {}
        self._hooks = (None, None, None, None)
        self._before_send = None

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
        """Queues a payload (written at the next poll/dispatch, or by flush). Raises Error(-14)
        if the send buffer is full, Error(-22) if the before_send hook refuses it."""
        _check(self._raw_send(conn, packet_id, data))

    def try_send(self, conn, packet_id, data=b""):
        """Like send, but returns False instead of raising when the send buffer is full."""
        rc = self._raw_send(conn, packet_id, data)
        if rc == WOULD_BLOCK:
            return False
        _check(rc)
        return True

    def send_many(self, packets):
        """Sends [(conn, packet_id, data), ...] with ONE call into the library (the cheap way to
        answer a whole batch). `data` is bytes-like, or a received Packet to send its payload back
        without copying (before its batch is released). Returns how many were queued; stops at
        the first that fails."""
        if self._before_send:
            packets = [p for p in packets if not self._before_send(p[0], p[1], bytes(p[2]))]
        n = len(packets)
        if n == 0:
            return 0
        # All payloads go into one buffer; each record points at its slice of it.
        # A received Packet can be sent back as it is (no copy: its bytes are still in the
        # library's ring until the batch is released). Other payloads go into one buffer.
        blob = b"".join(p[2] for p in packets if not isinstance(p[2], Packet))
        base = ctypes.cast(ctypes.c_char_p(blob), ctypes.c_void_p).value or 0
        out = bytearray(n * _OUT.size)
        offset = 0
        for i, (conn, pid, data) in enumerate(packets):
            if isinstance(data, Packet):
                _OUT.pack_into(out, i * _OUT.size, conn, pid, 0, data._ptr, data._len)
                continue
            length = len(data)
            _OUT.pack_into(out, i * _OUT.size, conn, pid, 0, base + offset, length)
            offset += length
        records = (_ffi.Outgoing * n).from_buffer(out)
        return _check(_lib.dx_send_batch(self._ctx, records, n))

    def flush(self):
        """Writes queued packets now (only needed when no poll/dispatch follows)."""
        _check(_lib.dx_flush(self._ctx))

    def _raw_send(self, conn, packet_id, data):
        if self._before_send and self._before_send(conn, packet_id, bytes(data)):
            return -22  # DX_ERR_REJECTED
        data = bytes(data)
        return _lib.dx_send(self._ctx, conn, packet_id, data, len(data))

    def close_connection(self, conn):
        _check(_lib.dx_close(self._ctx, conn))

    def poll(self, timeout_ms=0, max_packets=64):
        """Returns a batch; use ``with ctx.poll(100) as batch:`` so it is released afterwards."""
        raw = (_ffi.Packet * max_packets)()
        n = _check(_lib.dx_poll(self._ctx, raw, max_packets, timeout_ms))
        return _Batch(self._ctx, raw, n)

    def on_packet(self, packet_id, fn):
        """Calls ``fn(ctx, packet)`` for packets with this id (0 = default) during dispatch;
        None removes it."""
        if fn is None:
            self._handlers.pop(packet_id, None)
        else:
            self._handlers[packet_id] = fn

    def set_hooks(self, on_connect=None, on_disconnect=None, on_error=None, before_send=None,
                  after_recv=None):
        """Installs hooks; before_send/after_recv return True to reject/drop the packet."""
        self._hooks = (on_connect, on_disconnect, on_error, after_recv)
        self._before_send = before_send

    def dispatch(self, timeout_ms=0, max_packets=64):
        """Polls once, runs hooks and handlers for the whole batch (in Python: one call into the
        library per batch, not per packet), then releases it. Returns the entries handled."""
        on_connect, on_disconnect, on_error, after_recv = self._hooks
        handlers = self._handlers
        with self.poll(timeout_ms, max_packets) as batch:
            for p in batch:
                if p.id == SYS_CONNECTED:
                    if on_connect:
                        on_connect(p.conn)
                elif p.id == SYS_DISCONNECTED:
                    reason = p.reason
                    if reason < 0 and on_error:
                        on_error(p.conn, reason)
                    if on_disconnect:
                        on_disconnect(p.conn, reason)
                elif not (after_recv and after_recv(p)):
                    fn = handlers.get(p.id) or handlers.get(0)
                    if fn:
                        fn(self, p)
            return len(batch)
