# Duplex

A small C library that defines a **binary packet protocol** between a client and a server, so
programs in different languages (C, Python, Rust, Node…) can talk to each other with the same
bytes. **CTOS** = client → server, **STOC** = server → client.

Typical use: a real-time game or chat backend made of services in several languages.

**Status:** only the packet *header* exists (encode, decode, validation). Payloads, sockets,
bindings and callbacks are planned (bottom of this page).

## Quick start

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug
```

```c
#include <Duplex/duplex.h>

dx_header h = {DX_MAGIC, DX_PROTOCOL_VERSION, 0, /*packet_id*/ 1, /*length*/ 5, /*seq*/ 7};
uint8_t wire[DX_HEADER_SIZE];
int rc = dx_header_encode(&h, wire);     /* 0 = ok, negative = error */
if (rc < 0) puts(dx_strerror(rc));
```

Requirements: CMake ≥ 3.20, Ninja, a C11 compiler, a POSIX shell with `git` and `nm`.

## The header (14 bytes, little-endian)

| offset | size | field | meaning |
|---|---|---|---|
| 0 | 2 | `magic` | always `0xC0DE`: "this is a Duplex packet" |
| 2 | 1 | `version` | protocol version (1) |
| 3 | 1 | `flags` | bit0 = direction (1 = STOC), bit1 = compressed, bits 2–7 must be 0 |
| 4 | 2 | `packet_id` | kind of packet; bit 15 = 1 for STOC |
| 6 | 4 | `length` | payload size, max 16 MiB |
| 10 | 4 | `sequence` | per-connection counter |

Example (CTOS, id 1, 5-byte payload, sequence 7):
`DE C0 | 01 | 00 | 01 00 | 05 00 00 00 | 07 00 00 00`

Network bytes are never trusted, so the **first** broken rule is reported, in this order:

| Rule | Error |
|---|---|
| magic is `0xC0DE` | `DX_ERR_BAD_MAGIC` |
| version is 1 | `DX_ERR_BAD_VERSION` |
| reserved flag bits are 0 | `DX_ERR_BAD_FLAGS` |
| direction flag matches `packet_id` bit 15 | `DX_ERR_BAD_ID` |
| length ≤ 16 MiB (stops a hostile size) | `DX_ERR_TOO_LARGE` |

Also: `DX_ERR_INVALID_ARG` (NULL pointer), `DX_ERR_TRUNCATED` (fewer than 14 bytes: wait for more).

## API

All functions are pure (no I/O, no allocation), so thread-safe; the caller owns all buffers.

| Function | Role |
|---|---|
| `dx_header_encode(h, out)` | validate `h`, write 14 bytes |
| `dx_header_decode(in, len, out)` | read ≤ `len` bytes, validate; `out` untouched on error |
| `dx_strerror(err)` | static text for an error code, never NULL |
| `dx_abi_version()` | version of the loaded library |

## Why it is built this way

- **Magic + fixed header + length**: same idea as file signatures and length-prefixed protocols.
- **Little-endian**: native on x86/ARM, no byte swapping; `Source/Codec/endian.h` still builds
  numbers byte by byte, so it is safe on any CPU and on unaligned data.
- **Reserved bits must be 0**: lets a future version give them a meaning safely.
- **Prefix `dx_`/`DX_`, hidden symbols**: only declared functions are public, which keeps the
  stable binary interface (ABI) small. Versions: `DX_ABI_VERSION` (C interface) and
  `DX_PROTOCOL_VERSION` (bytes on the wire) are independent.

## Layout

`Include/Duplex/` public headers · `Source/Core/` header logic · `Source/Codec/` byte order ·
`CMake/` build modules · `Tests/` tests · `Tools/Scripts/` shell checks.

## Tests

`ctest` runs four groups of checks and prints a pass/fail table (library tests are C; the two repository checks are short shell scripts):

| Check | What it proves |
|---|---|
| Naming | folders PascalCase, files snake_case, no case-only clashes (macOS/Windows are case-insensitive) |
| Unit | exact bytes, round trip, limits (0/max/max+1), every error, NULL args, struct layout |
| Vectors | reference `.bin`/`.json` pairs decode as expected (C test `Tests/Vectors/test_vectors.c`); reusable by every future binding |
| Abi | exported symbols equal `Tests/Abi/symbols.txt` |

Memory-error check: `CC=/opt/homebrew/opt/llvm/bin/clang cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
(`CC=…` only on macOS; Apple's clang hangs with ASan there, and ThreadSanitizer (`tsan`) crashes
there too, so use Linux/CI for it). Coverage of `Source/`: 100 % lines and branches.
Add a vector: drop `header_<name>.bin` + `.json` in `Tests/Vectors/`. Add a unit test:
`Tests/Unit/test_x.c` + `dx_unit_test(test_x)` in `Tests/CMakeLists.txt`.

## Planned

Payload fields (ints, floats, strings, bytes, arrays) · one packet registry generating code for
every language · sockets and event loop (epoll, io_uring, kqueue, IOCP) · **handlers and hooks**
(per-packet callbacks, connect/disconnect/before-send/after-receive/error hooks, a tunnel that
relays and filters packets between connections; callbacks run on the owner thread and received
pointers are valid only during the call) · bindings for C++, Python, Rust, Node, WASM ·
measured optimisations and fuzzing.

License: MIT.
