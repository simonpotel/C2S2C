# Duplex

A small C library that defines a **binary packet protocol** between a client and a server, so
programs in different languages (C, C++, Python, Rust, Node, browsers) can talk to each other with
the same bytes, and that runs the sockets and event loop for you. **CTOS** = client → server,
**STOC** = server → client. Typical use: a real-time game or chat backend made of services in
several languages.

## In two minutes

```bash
cmake --preset debug && cmake --build --preset debug      # needs CMake ≥ 3.20, Ninja, a C11 compiler
ctest --preset debug                                      # all tests available on this machine
```

A packet server that answers `ping` with `pong` (full chat example: `Examples/C/Server`):

```c
#include <Duplex/duplex.h>
#include "dx_packets.h"                                   /* DX_PKT_PING, DX_PKT_PONG... (generated) */

dx_ctx* ctx = dx_create(NULL);                            /* all memory reserved here */
dx_listen(ctx, "127.0.0.1", 0);                           /* port 0 = any; dx_listen_port(ctx) says which */
dx_packet pk[64];
for (;;) {
    int n = dx_poll(ctx, pk, 64, 100);                    /* wait up to 100 ms, up to 64 packets at once */
    for (int i = 0; i < n; i++)
        if (pk[i].packet_id == DX_PKT_PING)               /* pk[i].data points into a library ring */
            dx_send(ctx, pk[i].conn, DX_PKT_PONG, pk[i].data, pk[i].length);
    dx_release(ctx, n);                                   /* give the memory back */
}
```

Where to go next:

| I want to… | Read |
|---|---|
| know what is on the wire | [The header](#the-header-14-bytes-little-endian), [The payload](#the-payload) |
| define my own packets | [The packet registry](#the-packet-registry) (`Registry/protocol.toml`) |
| run a server or client in C | [Running a server and a client](#running-a-server-and-a-client) |
| react with callbacks, filter, proxy | [Handlers, hooks and tunnels](#handlers-hooks-and-tunnels) |
| use another language | [Using it from other languages](#using-it-from-other-languages) |
| see speed and tests | [Checksum and performance](#checksum-and-performance), [Tests](#tests) |

**State of the project:** everything described here exists and is tested; what is missing is
listed at the end ([Planned](#planned)).

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
for every language, one folder each: `Generated/C`, `Generated/Cpp`, `Generated/Python`,
`Generated/Rust`, `Generated/TypeScript`, plus the language-neutral listing
`Generated/registry.txt` (`0x8002 chat_message u32,string,u64`, one packet per line).
Never edit `Generated/` by hand; edit the TOML, then run the script.

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

## Running a server and a client

A **context** (`dx_ctx`) is one network engine. It reserves all its memory when created, then runs
without allocating. Use one context from one thread; several contexts can run in parallel threads.

```c
dx_ctx* ctx = dx_create(NULL);                      /* NULL = default settings */
dx_listen(ctx, "127.0.0.1", 0);                     /* server: port 0 = any, read it back: */
int port = dx_listen_port(ctx);
uint32_t conn; dx_connect(ctx, "127.0.0.1", port, &conn);   /* client (does not block) */

dx_packet pk[64];
for (;;) {
    int n = dx_poll(ctx, pk, 64, 100);              /* wait up to 100 ms, get up to 64 at once */
    for (int i = 0; i < n; i++) {
        /* pk[i].packet_id: a registry id, or DX_SYS_CONNECTED / DX_SYS_DISCONNECTED */
        /* pk[i].data, pk[i].length: the payload, zero-copy, inside a library-owned ring */
    }
    dx_release(ctx, n);                             /* give the memory back (in order) */
}
```

- **Zero-copy:** `data` points into the library's ring buffer and stays valid until `dx_release`.
  If you never release, the ring fills and reading pauses (back-pressure, nothing is lost).
- **Roles:** the side that called `dx_listen` is the server (sends ids with bit 15 set, receives
  ids without it); the side that called `dx_connect` is the client. `dx_send` enforces it.
- **Sending:** `dx_send` / `dx_send_batch` copy into a per-connection buffer and write what the
  socket accepts; `DX_ERR_WOULD_BLOCK` means the buffer is full: poll, then retry. Sequence
  numbers are added by the library.
- **Bad traffic:** invalid headers, payloads, wrong direction or sequence, oversized lengths, or a
  peer silent for `idle_timeout_ms` close that connection only; `DX_SYS_DISCONNECTED` carries the
  reason (`DX_ERR_*`, 0 for a normal close). Unknown/mid-packet hang-ups are reported too.
- **Settings** (`dx_config`, zero = default): `max_conns` (256), `max_payload` (64 KiB),
  `rx_ring_bytes` (4 MiB), `rx_queue`, `tx_bytes`, `idle_timeout_ms` (60 s), `backend`, `flags`,
  `max_tunnels`, optional allocator. Memory is about `max_conns × (max_payload + tx_bytes)` +
  `rx_ring_bytes`, reserved once at `dx_create`.

## Handlers, hooks and tunnels

A push-style layer on top of `dx_poll`: call `dx_dispatch(ctx, timeout)` in your loop instead.

| Feature | What it does |
|---|---|
| `dx_on_packet(ctx, id, fn, user)` | `fn` runs for each received packet with that id (id 0 = default handler) |
| `dx_set_hooks(ctx, &hooks)` | `on_connect`, `on_disconnect`, `on_error` observe; `before_send` and `after_recv` can **reject** a packet (non-zero) |
| `dx_tunnel(ctx, a, b, filter, user)` | relays packets between two connections (a proxy); `filter` can drop some; a peer that cannot keep up is disconnected |

Callbacks run in the owner thread; the packet they get is valid only during the call (copy it to
keep it). They may call `dx_send`, `dx_close`, `dx_connect`; never `dx_destroy`. A `before_send`
hook must not call `dx_send`. See `Examples/C/Server/chat_server.c` (handlers + a hook) and
`Tests/Integration/test_hooks.c` (hooks, filtering proxy).

## I/O backends

| Backend (`dx_config.backend`) | Where | Notes |
|---|---|---|
| `DX_BACKEND_AUTO` (default) | all | epoll on Linux, kqueue on macOS/BSD, poll elsewhere |
| `DX_BACKEND_POLL` | Unix | portable, scans every socket: fine for hundreds of connections |
| `DX_BACKEND_EPOLL` | Linux | cost depends on ready sockets, not on idle ones |
| `DX_BACKEND_KQUEUE` | macOS/BSD | same idea as epoll |
| (Windows) | Windows | `DX_BACKEND_AUTO` means `WSAPoll` through the poll backend; **there is no IOCP backend yet** |
| `DX_BACKEND_URING` | Linux ≥ 5.11 | io_uring: readiness requests and answers travel through shared queues (fewer system calls); `dx_create` returns NULL if the kernel or sandbox refuses it |

## Using it from other languages

All bindings wrap the same C library and share the packet ids from `Generated/<Lang>/`. Each has
the same chat example (`Examples/<Lang>/Server|Client`, same command line, same output), and the
**cross-language** tests run every client against every server.

| Language | Where | How |
|---|---|---|
| C | `Include/Duplex/duplex.h` + `Generated/C/dx_packets.h` | link `libduplex` |
| C++17 | `Bindings/Cpp/duplex.hpp` (header only) | `duplex::Context` owns the engine, `Batch` releases packets when it leaves scope, errors are `duplex::Error`, handlers/hooks are lambdas; add `Bindings/Cpp`, `Generated/Cpp` and `Include` to the include path |
| Python ≥ 3.9 | `Bindings/Python` (`pip install ./Bindings/Python`) | stdlib only (`ctypes`): `duplex.Context`, `with ctx.poll() as batch:` (packet data is a zero-copy view that raises after release), `duplex.encode/decode` from registry layouts; find the library with `DUPLEX_LIBRARY=/path/to/libduplex.*` |
| Rust | `Bindings/Rust` (crates `duplex-sys` = raw FFI, `duplex` = safe) | `Context` frees itself on `Drop`; a `Batch` releases its packets on `Drop` and `Packet<'a>` borrows from it, so the compiler rejects any use after release; errors are `Result<_, duplex::Error>`; handlers/hooks are closures. Build with `DUPLEX_LIB_DIR=<folder of libduplex> cargo build` from `Bindings/Rust` |
| Node ≥ 22.6 / TypeScript | `Bindings/Node` (N-API addon `Source/binding.c`, API in `Source/index.ts`, types in `index.d.ts`) | `Context`; `poll()` returns a `Batch` whose payloads are zero-copy `Uint8Array`s that are **detached** (length 0) on `batch.release()`; `dispatch()` runs handlers/hooks and releases for you; errors are `DuplexError`; `encode/decode` follow the registry (u64 as `bigint`). Node runs the `.ts` files directly (type stripping). Handlers and hooks run in the JS layer; `dx_tunnel` is C-only |
| Browser (WebAssembly) | `Bindings/Wasm` (`build.sh`, `Source/duplex_wasm.ts`, `Source/duplex_web.ts`) | the codec (headers, payload and UTF-8 validation, registry, CRC-32C) compiled to a ~6 KB `.wasm` with plain clang `--target=wasm32` + `wasm-ld` (no Emscripten): `loadCodec(bytes)` then `WebClient.connect(wsUrl, codec)`; each WebSocket message is exactly one Duplex frame; no sockets, no event loop in the browser build |

**Web chat:** `Examples/Web/Gateway/gateway.ts <server port>` serves `Examples/Web/Chat/index.html`
and relays WebSockets to a Duplex TCP server (a browser cannot open raw TCP); it turns the page's
TypeScript into JavaScript on the fly and serves only `Examples/Web/Chat`, `Bindings` and
`Generated/TypeScript`. Start any chat server, then the gateway, then open the printed URL:
`chat_server` prints its port; `gateway.ts <port>` prints the http port; set `DUPLEX_WASM` to the
`.wasm` if it is not under `Build/Wasm`.

```python
import duplex, duplex_registry as reg
ctx = duplex.Context(); ctx.listen("127.0.0.1", 0)
ctx.on_packet(reg.PING, lambda c, p: c.send(p.conn, reg.PONG, bytes(p.data)))
while True: ctx.dispatch(200)
```

## Checksum and performance

`dx_crc32c(crc, data, len)` is a CRC-32C checksum for applications that want to verify data, for
example the chunks of a file sent with `upload`. It starts from 0 and can continue over more data
(`dx_crc32c(dx_crc32c(0, a, n), b, m)` equals the checksum of `a` then `b`). The CPU's CRC
instructions are used when present (SSE4.2 on x86-64, ARMv8 CRC on arm64, detected at run time,
code in `Source/Simd/`); otherwise portable C. The hardware code is tested against the portable
code for every length and alignment.

Measured on Apple silicon, release build, one thread, loopback (your numbers will differ; the
regression check only compares a machine with its own baseline):

| Measure | Result |
|---|---|
| header encode / decode | about 570 / 490 million per second |
| payload validation (3 fields) | about 62 million per second |
| CRC-32C, portable → hardware | 415 MB/s → 8300 MB/s (20×) |
| round trip over loopback TCP | p50 13–46 µs, p99 70 µs–1.3 ms (varies run to run) |
| pipelined round trips | about 384 000 per second |
| cost through each binding (round trips/s, same scenario) | C++ 349k · Rust 348k · Node 166k · Python 111k |

Run them: `ctest --preset release -L Performance` (needs `cmake --preset release` first). The
`performance` test runs `Tools/Scripts/bench_compare.sh`, which takes the best of 5 runs and fails
if a throughput is more than 5 % below `Bench/Baselines/<os>-<arch>.txt` (latencies only fail at
2×: they are noisy). Record a baseline for a new machine with `bench_compare.sh --record <bench
programs>`.

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
| `dx_create` / `dx_destroy` / `dx_config_default` | make and free a context; settings |
| `dx_listen`, `dx_listen_port`, `dx_connect` | start a server, read its port, start a client |
| `dx_poll` / `dx_release` | receive a batch of packets (zero-copy) / give their memory back |
| `dx_send`, `dx_send_batch`, `dx_close` | send (validated, copied), send many, close gracefully |
| `dx_on_packet`, `dx_set_hooks`, `dx_dispatch`, `dx_tunnel` | handlers, hooks, push-style loop, relays |
| `dx_crc32c(crc, data, len)` | CRC-32C checksum, hardware accelerated when possible |
| `dx_strerror(err)` | static text for an error code, never NULL |
| `dx_abi_version()` | version of the loaded library |

## Platforms and CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push:

| Platform | What runs |
|---|---|
| Linux x86-64 and arm64, macOS arm64 and x86-64 | everything: debug suite (C tests, bindings in C++/Python/Rust/Node, WebAssembly, 5×5 matrix), then AddressSanitizer + UBSan, and ThreadSanitizer on Linux |
| Windows x86-64 and arm64 | the library (Winsock + `WSAPoll`) and the C unit, property, vector, integration and memory tests (clang from the runner, MSVC runtime). The bindings, shell-based checks, raw-socket fault tests and threads test are not ported |
| Linux (fuzz job) | both libFuzzer targets for 20 s each |

Locally on a Mac: `Tools/Scripts/linux_check.sh` runs debug/asan/tsan on Linux in Docker (and the
io_uring and poll backends), `Tools/Scripts/coverage.sh` measures coverage (see Tests).

## Why it is built this way

- **Magic + fixed header + length**: same idea as file signatures and length-prefixed protocols.
- **Little-endian**: native on x86/ARM, no byte swapping; `Source/Codec/endian.h` still builds
  numbers byte by byte, so it is safe on any CPU and on unaligned data.
- **Reserved bits must be 0**: lets a future version give them a meaning safely.
- **Prefix `dx_`/`DX_`, hidden symbols**: only declared functions are public, which keeps the
  stable binary interface (ABI) small. Versions: `DX_ABI_VERSION` (C interface) and
  `DX_PROTOCOL_VERSION` (bytes on the wire) are independent.

## Layout

| Folder | What is in it |
|---|---|
| `Include/Duplex/` | the public headers (the contract) |
| `Source/Core/` | header logic, registry lookup; `Ctx/` context, event loop, delivery queue, listener; `Conn/` one connection (lifecycle, sending, receiving, events, maintenance); `Hooks/` handlers, hooks, tunnels |
| `Source/Codec/` | byte order, payload writer/reader, UTF-8 |
| `Source/Memory/` | ring buffer and slot pool (no allocation: the caller gives the memory) |
| `Source/IO/` | I/O backends (poll, epoll, kqueue, io_uring) and the socket layer (BSD sockets, Winsock) |
| `Source/Simd/` | CRC-32C: CPU detection, hardware and portable versions |
| `Registry/`, `Generated/<Lang>/`, `Tools/Codegen/` | packet definitions, generated tables per language, the generator |
| `Bindings/<Lang>/` | Cpp, Python, Rust, Node, Wasm |
| `Examples/<Lang>/Server`, `Client`; `Examples/Web/` | the chat in every language; the web chat and its gateway |
| `Tests/`, `Bench/` | all tests (`Unit`, `Property`, `Vectors`, `Integration`, `Faults`, `Fuzz`, `Memory`, `Concurrency`, `Stress`, `Generated`, `Bindings`, `Wasm`, `Examples`); benchmarks and baselines |
| `Tools/Scripts/` | shell checks (naming, ABI, source rules, benchmark comparison, Linux/Docker run, coverage) |
| `CMake/` | build modules (warnings, sanitizers, coverage) |

## Planned

- An **IOCP backend** for Windows (today Windows uses `WSAPoll`), and porting the bindings and
  the shell-based checks to Windows.
- Long runs: fuzzing for hours and the 10-minute stress run (`DX_STRESS_SECONDS=600`) were not
  executed for this version; the short versions run in CI.

License: MIT.
