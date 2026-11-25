# Duplex

A small C library that defines a **binary packet protocol** between a client and a server, so
programs in different languages (C, Python, Rust, Node…) can talk to each other with the same
bytes. **CTOS** = client → server, **STOC** = server → client.

Typical use: a real-time game or chat backend made of services in several languages.

**Status:** packet header and payload codecs exist (encode, decode, strict validation). Packet registry
with code generation exists too. Sockets, bindings and callbacks are planned (bottom of this page).

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

## The payload

After the header comes `length` bytes of payload: a plain sequence of fields, no padding,
little-endian. Field types:

| Type | On the wire |
|---|---|
| `u8 u16 u32 u64`, `i8 i16 i32 i64`, `f32 f64` | fixed size (floats = IEEE-754 bits) |
| `bool` | 1 byte, must be 0 or 1 |
| `string` | u16 byte count + UTF-8 (strictly validated: no overlong forms, no surrogates) |
| `bytes` | u32 byte count + data |
| `array<T>` | u16 element count + elements (`T` is any type except array) |

A **schema** (`dx_schema`) is the ordered list of field types a packet must contain.
`dx_payload_validate_schema` checks a payload against it: nothing missing, nothing left over.
Extra errors: `DX_ERR_BAD_UTF8`, `DX_ERR_BAD_VALUE`, `DX_ERR_TRAILING`, `DX_ERR_BAD_SCHEMA`,
`DX_ERR_OVERFLOW` (writer buffer full).

Writing and reading use a cursor with a **sticky error**: call `dx_w_*` / `dx_r_*` for each field,
check `err` once at the end.

```c
uint8_t buf[64];
dx_writer w;
dx_writer_init(&w, buf, sizeof buf);
dx_w_u32(&w, 42);
dx_w_string(&w, "hello", 5);
if (w.err < 0) puts(dx_strerror(w.err));      /* else send buf[0..w.len) */

dx_reader r;
dx_reader_init(&r, buf, w.len);
uint32_t id = dx_r_u32(&r);
uint16_t n; const char* s = dx_r_string(&r, &n);   /* points into buf, not NUL-terminated */
```

## The packet registry

`Registry/protocol.toml` is the **only** place packets are defined (name, id, fields). The script
`Tools/Codegen/gen_registry.py` (Python 3.11+, no dependency) turns it into ready-made tables
for every language in `Generated/`: C (`dx_packets.h`, `dx_registry.c`), C++, Python, Rust,
TypeScript. Never edit `Generated/` by hand; edit the TOML, then run the script.

```toml
[[packet]]
name = "chat_message"            # server -> client because bit 15 of the id is set
id = 0x8002
fields = ["user_id:u32", "text:string", "timestamp:u64"]
```

Rules, enforced by the generator: ids are unique; bit 15 = 1 means server → client; ids
`0x7F00-0x7FFF` and `0xFF00-0xFFFF` are reserved for the library; an id is **never reused**
(`Registry/ids.lock` remembers every id ever used, deleting or renaming one is refused: mark it
`deprecated = true` instead). In C, `DX_PKT_CHAT_MESSAGE` is the id and
`dx_payload_validate(id, bytes, len)` checks a payload against its layout.

## API

All functions are pure (no I/O, no allocation), so thread-safe; the caller owns all buffers.

| Function | Role |
|---|---|
| `dx_header_encode(h, out)` | validate `h`, write 14 bytes |
| `dx_header_decode(in, len, out)` | read ≤ `len` bytes, validate; `out` untouched on error |
| `dx_writer_init`, `dx_w_u8/u16/u32/u64/f32/f64/string/bytes` | append fields to your buffer |
| `dx_reader_init`, `dx_r_u8/u16/u32/u64/f32/f64/string/bytes` | read fields, bounds-checked; strings/bytes point into the input |
| `dx_payload_validate_schema(schema, in, len)` | is `in` exactly one valid payload for the schema? |
| `dx_payload_validate(id, in, len)` | same, using the registry layout of packet `id`; `DX_ERR_UNKNOWN_ID` if not registered |
| `dx_packet_name(id)` | registered name (for logs), or NULL |
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

`Include/Duplex/` public headers · `Source/Core/` header logic · `Registry/` + `Generated/` packet definitions and generated tables · `Tools/Codegen/` generator · `Source/Codec/` byte order, payload read/write, UTF-8 ·
`CMake/` build modules · `Tests/` tests · `Tools/Scripts/` shell checks.

## Tests

`ctest --preset debug` prints a pass/fail table. Library tests are C; the two repository checks
are short shell scripts.

| Check | What it proves |
|---|---|
| Naming | folders PascalCase, files snake_case, no case-only clashes (macOS/Windows are case-insensitive) |
| Unit | exact bytes, round trips, limits (0/max/max+1), every error, NULL args, struct layout, UTF-8 edge cases |
| Property | 100 000 random round trips per type (write, validate, read back); garbage never crashes. Seed is printed; replay with `DX_SEED=<n>` |
| Vectors | reference `.bin`/`.json` pairs (headers and payloads) give the expected result (`Tests/Vectors/test_vectors.c`); reusable by every future binding |
| Registry | `Generated/` is up to date with `protocol.toml` (needs Python 3.11+, else not registered) |
| Fuzz | libFuzzer feeds random bytes to the decoders under ASan/UBSan |
| Abi | exported symbols equal `Tests/Abi/symbols.txt` |

Extra runs (macOS needs Homebrew LLVM: `export CC=/opt/homebrew/opt/llvm/bin/clang`):

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # memory errors, UB
cmake --preset fuzz && cmake --build --preset fuzz && ctest --preset fuzz   # fuzz, 10 s per target
```

Apple's own clang hangs with ASan on recent macOS, and ThreadSanitizer (`tsan`) crashes there,
so run `tsan` on Linux (docker or CI). Coverage of `Source/`: 100 % lines, about 98 % branches.

Add a vector: `header_<name>` or `payload_<name>` `.bin` + `.json` in `Tests/Vectors/`
(payload json: `{"schema": "u8,string,array<u16>", "error": "DX_OK"}`; `packet_<name>` uses `{"packet_id": 32770, ...}` and the registry layout).
Add a unit test: `Tests/Unit/test_x.c` + `dx_unit_test(test_x)` in `Tests/CMakeLists.txt`.

## Planned

Sockets and event loop (epoll, io_uring, kqueue, IOCP) · **handlers and hooks**
(per-packet callbacks, connect/disconnect/before-send/after-receive/error hooks, a tunnel that
relays and filters packets between connections; callbacks run on the owner thread and received
pointers are valid only during the call) · bindings for C++, Python, Rust, Node, WASM ·
measured optimisations and fuzzing.

License: MIT.
