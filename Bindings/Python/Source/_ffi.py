"""Low-level ctypes description of the Duplex C API (SPDX-License-Identifier: MIT).

In plain words: this file tells Python what the functions and structures of libduplex look like,
and loads the shared library. Everything user-facing is in __init__.py.
"""
import ctypes
import os
import sys
from pathlib import Path

# Version of the C interface this binding was written for: (major, minor).
EXPECTED_ABI = (1, 4)


def _candidates():
    """Where to look for the library: DUPLEX_LIBRARY first, then next to the package and Build/."""
    env = os.environ.get("DUPLEX_LIBRARY")
    if env:
        yield Path(env)
    name = {"darwin": "libduplex.dylib", "win32": "duplex.dll"}.get(sys.platform, "libduplex.so")
    here = Path(__file__).resolve().parent
    yield here / name
    root = here.parents[2]  # repository root when run from a checkout
    for preset in ("release", "debug"):
        yield root / "Build" / preset / name


def load():
    """Loads libduplex and checks that its interface version is compatible."""
    for path in _candidates():
        if path.exists():
            lib = ctypes.CDLL(str(path))
            break
    else:
        raise OSError("libduplex not found: set DUPLEX_LIBRARY to its path")
    version = lib.dx_abi_version
    version.restype = ctypes.c_uint32
    v = version()
    major, minor = v >> 16, (v >> 8) & 0xFF
    if major != EXPECTED_ABI[0] or minor < EXPECTED_ABI[1]:
        raise OSError(f"libduplex ABI {major}.{minor} is not compatible with this binding "
                      f"(needs {EXPECTED_ABI[0]}.{EXPECTED_ABI[1]}+)")
    return lib


class Alloc(ctypes.Structure):
    _fields_ = [("alloc", ctypes.c_void_p), ("release", ctypes.c_void_p), ("user", ctypes.c_void_p)]


class Config(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in (
        "size", "max_conns", "max_payload", "rx_ring_bytes", "rx_queue", "tx_bytes", "flags",
        "idle_timeout_ms", "backend")] + [("alloc", Alloc), ("max_tunnels", ctypes.c_uint32),
                                           ("reserved", ctypes.c_uint32)]


class Packet(ctypes.Structure):
    _fields_ = [("conn", ctypes.c_uint32), ("sequence", ctypes.c_uint32),
                ("data", ctypes.c_void_p), ("length", ctypes.c_uint32),
                ("packet_id", ctypes.c_uint16), ("flags", ctypes.c_uint8),
                ("reserved", ctypes.c_uint8)]


Ctx = ctypes.c_void_p
PacketPtr = ctypes.POINTER(Packet)

HANDLER = ctypes.CFUNCTYPE(None, Ctx, PacketPtr, ctypes.c_void_p)
ON_CONNECT = ctypes.CFUNCTYPE(None, Ctx, ctypes.c_uint32, ctypes.c_void_p)
ON_DISCONNECT = ctypes.CFUNCTYPE(None, Ctx, ctypes.c_uint32, ctypes.c_int, ctypes.c_void_p)
BEFORE_SEND = ctypes.CFUNCTYPE(ctypes.c_int, Ctx, ctypes.c_uint32, ctypes.c_uint16,
                               ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)
AFTER_RECV = ctypes.CFUNCTYPE(ctypes.c_int, Ctx, PacketPtr, ctypes.c_void_p)
ON_ERROR = ctypes.CFUNCTYPE(None, Ctx, ctypes.c_uint32, ctypes.c_int, ctypes.c_void_p)


class Hooks(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("reserved", ctypes.c_uint32),
                ("on_connect", ON_CONNECT), ("on_disconnect", ON_DISCONNECT),
                ("before_send", BEFORE_SEND), ("after_recv", AFTER_RECV),
                ("on_error", ON_ERROR), ("user", ctypes.c_void_p)]


def declare(lib):
    """Sets argument and return types of every function we call."""
    def sig(name, restype, *argtypes):
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = restype, list(argtypes)

    c_int, u16, u32 = ctypes.c_int, ctypes.c_uint16, ctypes.c_uint32
    sig("dx_strerror", ctypes.c_char_p, c_int)
    sig("dx_config_default", None, ctypes.POINTER(Config))
    sig("dx_create", Ctx, ctypes.POINTER(Config))
    sig("dx_destroy", None, Ctx)
    sig("dx_listen", c_int, Ctx, ctypes.c_char_p, u16)
    sig("dx_listen_port", c_int, Ctx)
    sig("dx_connect", c_int, Ctx, ctypes.c_char_p, u16, ctypes.POINTER(u32))
    sig("dx_poll", c_int, Ctx, PacketPtr, c_int, c_int)
    sig("dx_release", None, Ctx, c_int)
    sig("dx_send", c_int, Ctx, u32, u16, ctypes.c_char_p, u32)
    sig("dx_close", c_int, Ctx, u32)
    sig("dx_on_packet", c_int, Ctx, u16, HANDLER, ctypes.c_void_p)
    sig("dx_set_hooks", c_int, Ctx, ctypes.POINTER(Hooks))
    sig("dx_dispatch", c_int, Ctx, c_int)
    sig("dx_payload_validate", c_int, u16, ctypes.c_char_p, u32)
