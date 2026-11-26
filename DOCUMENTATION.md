# C2S2C

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

A packet server that answers `ping` with `pong` (the full chat exists in Rust, Python and Node: `Examples/<Lang>/Server`):

```c
#include <C2S2C/c2s2c.h>
#include "c2s2c_packets.h"                                   /* C2S2C_PKT_PING, C2S2C_PKT_PONG... (generated) */

c2s2c_ctx* ctx = c2s2c_create(NULL);                            /* all memory reserved here */
c2s2c_listen(ctx, "127.0.0.1", 0);                           /* port 0 = any; c2s2c_listen_port(ctx) says which */
c2s2c_packet pk[64];
for (;;) {
    int n = c2s2c_poll(ctx, pk, 64, 100);                    /* wait up to 100 ms, up to 64 packets at once */
    for (int i = 0; i < n; i++)
        if (pk[i].packet_id == C2S2C_PKT_PING)               /* pk[i].data points into a library ring */
            c2s2c_send(ctx, pk[i].conn, C2S2C_PKT_PONG, pk[i].data, pk[i].length);
    c2s2c_release(ctx, n);                                   /* give the memory back */
}
```

Where to go next:

| I want to… | Read |
|---|---|
| know what is on the wire | [The header](#the-header-14-bytes-little-endian), [The payload](#the-payload) |
| define my own packets | [The packet registry](#the-packet-registry) (`Registry/protocol.toml`) |
| run a server or client in C | [Running a server and a client](#running-a-server-and-a-client) |
| know what the library does and what the bindings do | [Library or binding?](#library-or-binding) |
| prove it beats recoding the protocol | [Service benchmark](#service-benchmark) |
| use another language | [Using it from other languages](#using-it-from-other-languages) |
| see speed and tests | [Checksum and performance](#checksum-and-performance), [Tests](#tests) |

**State of the project:** everything described here exists and is tested; what is missing is
listed at the end ([Planned](#planned)).

## The header (14 bytes, little-endian)

| offset | size | field | meaning |
|---|---|---|---|
| 0 | 2 | `magic` | always `0xC0DE`: "this is a C2S2C packet" |
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
| magic is `0xC0DE` | `C2S2C_ERR_BAD_MAGIC` |
| version is 1 | `C2S2C_ERR_BAD_VERSION` |
| reserved flag bits are 0 | `C2S2C_ERR_BAD_FLAGS` |
| direction flag matches `packet_id` bit 15 | `C2S2C_ERR_BAD_ID` |
| length ≤ 16 MiB (stops a hostile size) | `C2S2C_ERR_TOO_LARGE` |

Also: `C2S2C_ERR_INVALID_ARG` (NULL pointer), `C2S2C_ERR_TRUNCATED` (fewer than 14 bytes: wait for more).

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

A **schema** (`c2s2c_schema`) is the ordered list of field types a packet must contain.
`c2s2c_payload_validate_schema` checks a payload against it: nothing missing, nothing left over.
Extra errors: `C2S2C_ERR_BAD_UTF8`, `C2S2C_ERR_BAD_VALUE`, `C2S2C_ERR_TRAILING`, `C2S2C_ERR_BAD_SCHEMA`,
`C2S2C_ERR_OVERFLOW` (writer buffer full).

Writing and reading use a cursor with a **sticky error**: call `c2s2c_w_*` / `c2s2c_r_*` for each field,
check `err` once at the end.

```c
uint8_t buf[64];
c2s2c_writer w;
c2s2c_writer_init(&w, buf, sizeof buf);
c2s2c_w_u32(&w, 42);
c2s2c_w_string(&w, "hello", 5);
if (w.err < 0) puts(c2s2c_strerror(w.err));      /* else send buf[0..w.len) */

c2s2c_reader r;
c2s2c_reader_init(&r, buf, w.len);
uint32_t id = c2s2c_r_u32(&r);
uint16_t n; const char* s = c2s2c_r_string(&r, &n);   /* points into buf, not NUL-terminated */
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
`deprecated = true` instead). In C, `C2S2C_PKT_CHAT_MESSAGE` is the id and
`c2s2c_payload_validate(id, bytes, len)` checks a payload against its layout.

## Running a server and a client

A **context** (`c2s2c_ctx`) is one network engine. It reserves all its memory when created, then runs
without allocating. Use one context from one thread; several contexts can run in parallel threads.

```c
c2s2c_ctx* ctx = c2s2c_create(NULL);                      /* NULL = default settings */
c2s2c_listen(ctx, "127.0.0.1", 0);                     /* server: port 0 = any, read it back: */
int port = c2s2c_listen_port(ctx);
uint32_t conn; c2s2c_connect(ctx, "127.0.0.1", port, &conn);   /* client (does not block) */

c2s2c_packet pk[64];
for (;;) {
    int n = c2s2c_poll(ctx, pk, 64, 100);              /* wait up to 100 ms, get up to 64 at once */
    for (int i = 0; i < n; i++) {
        /* pk[i].packet_id: a registry id, or C2S2C_SYS_CONNECTED / C2S2C_SYS_DISCONNECTED */
        /* pk[i].data, pk[i].length: the payload, zero-copy, inside a library-owned ring */
    }
    c2s2c_release(ctx, n);                             /* give the memory back (in order) */
}
```

- **Zero-copy:** `data` points into the library's ring buffer and stays valid until `c2s2c_release`.
  If you never release, the ring fills and reading pauses (back-pressure, nothing is lost).
- **Roles:** the side that called `c2s2c_listen` is the server (sends ids with bit 15 set, receives
  ids without it); the side that called `c2s2c_connect` is the client. `c2s2c_send` enforces it.
- **Sending:** `c2s2c_send` / `c2s2c_send_batch` copy into a per-connection buffer; the bytes leave at
  the next `c2s2c_poll` (one write per connection for a whole batch of replies), when a buffer is
  half full, or on `c2s2c_flush`. `C2S2C_ERR_WOULD_BLOCK` means the buffer is full: poll, then retry.
  Sequence numbers are added by the library.
- **Bad traffic:** invalid headers, payloads, wrong direction or sequence, oversized lengths, or a
  peer silent for `idle_timeout_ms` close that connection only; `C2S2C_SYS_DISCONNECTED` carries the
  reason (`C2S2C_ERR_*`, 0 for a normal close). Unknown/mid-packet hang-ups are reported too.
- **Settings** (`c2s2c_config`, zero = default): `max_conns` (256), `max_payload` (64 KiB),
  `rx_ring_bytes` (4 MiB), `rx_queue`, `tx_bytes`, `idle_timeout_ms` (60 s), `backend`, `flags`,
  `max_tunnels`, optional allocator. Memory is about `max_conns × (max_payload + tx_bytes)` +
  `rx_ring_bytes`, reserved once at `c2s2c_create`.

## Library or binding?

The library is the **data path**, written once and shared by every language: framing, strict
validation, sequence and direction checks, buffers reserved up front, batched `c2s2c_poll`, writes
grouped per connection, and one feature that runs entirely inside it, the relay:

| In the library (C) | What it does |
|---|---|
| `c2s2c_poll` / `c2s2c_release` | hand out a whole batch of packets per call (one crossing from another language per batch) |
| `c2s2c_send`, `c2s2c_send_batch`, `c2s2c_flush` | queue packets; they leave grouped at the next poll |
| `c2s2c_tunnel(ctx, a, b, drop_ids, n)` | relay two connections both ways without the application (a proxy), dropping the listed packet ids; a slow side pauses the other instead of buffering (`Tests/Integration/test_tunnel.c`) |

**Handlers and hooks live in the bindings** (`on_packet`, `set_hooks` with `on_connect`,
`on_disconnect`, `on_error`, `before_send`, `after_recv`, and `dispatch`), written in each
language over one batched poll. Why: a callback from C into Python, Rust or JavaScript crosses the
language boundary for every packet, which is exactly the cost batching removes. The C library
never calls back into the application.

## I/O backends

| Backend (`c2s2c_config.backend`) | Where | Notes |
|---|---|---|
| `C2S2C_BACKEND_AUTO` (default) | all | epoll on Linux, kqueue on macOS/BSD, poll elsewhere |
| `C2S2C_BACKEND_POLL` | Unix | portable, scans every socket: fine for hundreds of connections |
| `C2S2C_BACKEND_EPOLL` | Linux | cost depends on ready sockets, not on idle ones |
| `C2S2C_BACKEND_KQUEUE` | macOS/BSD | same idea as epoll |
| (Windows) | Windows | `C2S2C_BACKEND_AUTO` means `WSAPoll` through the poll backend; **there is no IOCP backend yet** |
| `C2S2C_BACKEND_URING` | Linux ≥ 5.11 | io_uring: readiness requests and answers travel through shared queues (fewer system calls); `c2s2c_create` returns NULL if the kernel or sandbox refuses it |

## Using it from other languages

All bindings wrap the same C library and share the packet ids from `Generated/<Lang>/`. Each has
the same chat example (`Examples/<Lang>/Server|Client`, same command line, same output), and the
**cross-language** tests run every client against every server (Rust, Python, Node: 3×3).
In every binding, handlers and hooks run in the language over batched polls (see above).

| Language | Where | How |
|---|---|---|
| C | `Include/C2S2C/c2s2c.h` + `Generated/C/c2s2c_packets.h` | link `libc2s2c` |
| C++17 | `Bindings/Cpp/c2s2c.hpp` (header only) | `c2s2c::Context` owns the engine, `Batch` releases packets when it leaves scope, errors are `c2s2c::Error`, handlers/hooks are lambdas, `flush()`; add `Bindings/Cpp`, `Generated/Cpp` and `Include` to the include path |
| Python ≥ 3.9 | `Bindings/Python` (`pip install ./Bindings/Python`) | stdlib only (`ctypes`): `c2s2c.Context`, `with ctx.poll() as batch:` (`p.data` is a zero-copy view that raises after release, `p.payload()` a copy), `ctx.send_many([...])` answers a whole batch in one call (a received packet can be echoed without copying), `c2s2c.encode/decode` from registry layouts; find the library with `C2S2C_LIBRARY=/path/to/libc2s2c.*` |
| Rust | `Bindings/Rust` (crates `c2s2c-sys` = raw FFI, `c2s2c` = safe) | `Context` frees itself on `Drop`; a `Batch` releases its packets on `Drop` and `Packet<'a>` borrows from it, so the compiler rejects any use after release; errors are `Result<_, c2s2c::Error>`; handlers/hooks are closures; `flush()`. Build with `C2S2C_LIB_DIR=<folder of libc2s2c> cargo build` from `Bindings/Rust` |
| Node ≥ 22.6 / TypeScript | `Bindings/Node` (N-API addon `Source/binding.c`, API in `Source/index.ts`, types in `index.d.ts`) | `Context`; `poll()` returns a `Batch` (one native call per batch: packet table + one payload buffer) whose packets throw after `batch.release()`; `sendMany([...])` answers a batch in one call; `dispatch()` runs handlers/hooks and releases for you; errors are `C2S2CError`; `encode/decode` follow the registry (u64 as `bigint`). Node runs the `.ts` files directly (type stripping). `c2s2c_tunnel` is C-only |
| Browser (WebAssembly) | `Bindings/Wasm` (`build.sh`, `Source/c2s2c_wasm.ts`, `Source/c2s2c_web.ts`) | the codec (headers, payload and UTF-8 validation, registry, CRC-32C) compiled to a ~6 KB `.wasm` with plain clang `--target=wasm32` + `wasm-ld` (no Emscripten): `loadCodec(bytes)` then `WebClient.connect(wsUrl, codec)`; each WebSocket message is exactly one C2S2C frame; no sockets, no event loop in the browser build |

**Web chat:** `Examples/Web/Gateway/gateway.ts <server port>` serves `Examples/Web/Chat/index.html`
and relays WebSockets to a C2S2C TCP server (a browser cannot open raw TCP); it turns the page's
TypeScript into JavaScript on the fly and serves only `Examples/Web/Chat`, `Bindings` and
`Generated/TypeScript`. Start any chat server, then the gateway, then open the printed URL:
`chat_server` prints its port; `gateway.ts <port>` prints the http port; set `C2S2C_WASM` to the
`.wasm` if it is not under `Build/Wasm`.

```python
import c2s2c, c2s2c_registry as reg
ctx = c2s2c.Context(); ctx.listen("127.0.0.1", 0)
ctx.on_packet(reg.PING, lambda c, p: c.send(p.conn, reg.PONG, p.payload()))
while True: ctx.dispatch(200)
```

## Checksum and performance

`c2s2c_crc32c(crc, data, len)` is a CRC-32C checksum for applications that want to verify data, for
example the chunks of a file sent with `upload`. It starts from 0 and can continue over more data
(`c2s2c_crc32c(c2s2c_crc32c(0, a, n), b, m)` equals the checksum of `a` then `b`). The CPU's CRC
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

### Service benchmark

Question: is it worth using the library rather than rewriting the protocol in each language?
`Tools/Scripts/bench_service.sh Build/release` runs the same ping/pong service written three
ways: on `libc2s2c` directly (C), **from scratch** in Rust, Python and Node (same checks as the
library for that packet: header fields, direction, length limit, sequence, id, payload size, idle
timeout), and **on each binding**. One C load generator (3 processes × 64 connections × 4 pings in
flight) drives every server, then one C checker sends 17 kinds of hostile traffic (bad headers,
4 GiB length, garbage, slowloris, floods...). Apple silicon, release, loopback:

| Server | Round trips/s | p99 | Memory | Lines | Hostile traffic |
|---|---|---|---|---|---|
| C on libc2s2c | 1 297 000 | 0.8 ms | 54 MB | 20 | 17/17 |
| Rust from scratch (thread per connection) | 419 000 | 3.7 ms | 2 MB | 48 | 17/17 |
| Rust on the binding (`poll` / `dispatch`) | 1 341 000 / 1 319 000 | 0.7 ms | 7 MB | 23 | 17/17 |
| Python from scratch (selectors) | 440 000 | 1.9 ms | 14 MB | 82 | 17/17 |
| Python on the binding, `poll` + `send_many` | 633 000 | 2.1 ms | 17 MB | 27 | 17/17 |
| Python on the binding, `dispatch` (a Python handler per packet) | 374 000 | 2.8 ms | 20 MB | 27 | 17/17 |
| Node from scratch (`net`) | 688 000 | 1.3 ms | 108 MB | 28 | 17/17 |
| Node on the binding (`poll` / `dispatch`) | 1 148 000 / 1 183 000 | 0.9 / 0.8 ms | 109 / 78 MB | 26 | 17/17 |

What it shows:
- **Faster:** on the library each language serves 1.4× (Python) to 3.2× (Rust) more round trips
  than the same service written by hand, with lower tail latency. The C library is the ceiling
  and Rust and Node reach it. A Python handler called per packet (`dispatch`) is slower than a
  hand-written loop: for hot paths in Python, loop over `poll` and answer with `send_many`.
- **Stable:** the hand-written servers pass the hostile checks too, but only because each one
  re-implements the checks (48–82 lines for one packet type, without UTF-8, arrays or the rest of
  the registry). On the library the same service is 20–27 lines in every language and gets every
  check, for every packet of the registry, from one tested implementation.
- **Memory:** the library reserves its buffers for `max_conns` connections up front (54 MB for
  4096 here): predictable, but more than a server that allocates as it goes.

The benchmark itself led to two library changes: writes are now grouped per connection (the C
server went from 561 000 to 1 297 000 round trips/s), and a readable socket is read again while
it fills the buffer (big packets no longer wait for the next poll).

Run them: `ctest --preset release -L Performance` (needs `cmake --preset release` first). The
`performance` test runs `Tools/Scripts/bench_compare.sh`, which takes the best of 5 runs and fails
if a throughput is more than 5 % below `Bench/Baselines/<os>-<arch>.txt` (latencies only fail at
2×: they are noisy). Record a baseline for a new machine with `bench_compare.sh --record <bench
programs>`.

## API

All functions are pure (no I/O, no allocation), so thread-safe; the caller owns all buffers.

| Function | Role |
|---|---|
| `c2s2c_header_encode(h, out)` | validate `h`, write 14 bytes |
| `c2s2c_header_decode(in, len, out)` | read ≤ `len` bytes, validate; `out` untouched on error |
| `c2s2c_writer_init`, `c2s2c_w_u8/u16/u32/u64/f32/f64/string/bytes` | append fields to your buffer |
| `c2s2c_reader_init`, `c2s2c_r_u8/u16/u32/u64/f32/f64/string/bytes` | read fields, bounds-checked; strings/bytes point into the input |
| `c2s2c_payload_validate_schema(schema, in, len)` | is `in` exactly one valid payload for the schema? |
| `c2s2c_payload_validate(id, in, len)` | same, using the registry layout of packet `id`; `C2S2C_ERR_UNKNOWN_ID` if not registered |
| `c2s2c_packet_name(id)` | registered name (for logs), or NULL |
| `c2s2c_create` / `c2s2c_destroy` / `c2s2c_config_default` | make and free a context; settings |
| `c2s2c_listen`, `c2s2c_listen_port`, `c2s2c_connect` | start a server, read its port, start a client |
| `c2s2c_poll` / `c2s2c_release` | receive a batch of packets (zero-copy) / give their memory back |
| `c2s2c_send`, `c2s2c_send_batch`, `c2s2c_close` | send (validated, copied), send many, close gracefully |
| `c2s2c_flush`, `c2s2c_tunnel` | write queued packets now; relay two connections inside the library |
| `c2s2c_crc32c(crc, data, len)` | CRC-32C checksum, hardware accelerated when possible |
| `c2s2c_strerror(err)` | static text for an error code, never NULL |
| `c2s2c_abi_version()` | version of the loaded library |

## Platforms and CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push:

| Platform | What runs |
|---|---|
| Linux x86-64 and arm64, macOS arm64 and x86-64 | everything: debug suite (C tests, bindings in C++/Python/Rust/Node, WebAssembly, 3×3 matrix), then AddressSanitizer + UBSan, and ThreadSanitizer on Linux |
| Windows x86-64 and arm64 | the library (Winsock + `WSAPoll`) and the C unit, property, vector, integration and memory tests (clang from the runner, MSVC runtime). The bindings, shell-based checks, raw-socket fault tests and threads test are not ported |
| Linux (fuzz job) | both libFuzzer targets for 20 s each |

Locally on a Mac: `Tools/Scripts/linux_check.sh` runs debug/asan/tsan on Linux in Docker (and the
io_uring and poll backends), `Tools/Scripts/coverage.sh` measures coverage (see Tests).

## Why it is built this way

- **Magic + fixed header + length**: same idea as file signatures and length-prefixed protocols.
- **Little-endian**: native on x86/ARM, no byte swapping; `Source/Codec/endian.h` still builds
  numbers byte by byte, so it is safe on any CPU and on unaligned data.
- **Reserved bits must be 0**: lets a future version give them a meaning safely.
- **Prefix `c2s2c_`/`C2S2C_`, hidden symbols**: only declared functions are public, which keeps the
  stable binary interface (ABI) small. Versions: `C2S2C_ABI_VERSION` (C interface) and
  `C2S2C_PROTOCOL_VERSION` (bytes on the wire) are independent.

## Layout

| Folder | What is in it |
|---|---|
| `Include/C2S2C/` | the public headers (the contract) |
| `Source/Core/` | header logic, registry lookup; `Ctx/` context, event loop, delivery queue, listener, relay; `Conn/` one connection (lifecycle, sending, receiving, events, maintenance) |
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

## Tests

`ctest --preset debug` prints a pass/fail table. Library tests are C; the two repository checks
are short shell scripts.

| Check | What it proves |
|---|---|
| Naming | folders PascalCase, files snake_case, no case-only clashes (macOS/Windows are case-insensitive) |
| Unit | exact bytes, round trips, limits (0/max/max+1), every error, NULL args, struct layout, UTF-8 edge cases |
| Property | 100 000 random round trips per type (write, validate, read back); garbage never crashes. Seed is printed; replay with `C2S2C_SEED=<n>` |
| Vectors | reference `.bin`/`.json` pairs (headers and payloads) give the expected result (`Tests/Vectors/test_vectors.c`); reusable by every future binding |
| Generated | one test per language (`Tests/Generated/<Lang>/`) checks that its generated code lists the same packets, names and layouts as `Generated/registry.txt`; C always runs, C++/Python/Rust/TypeScript run if `c++`, Python 3.11+, `rustc`, `node` (22.6+) are installed |
| Registry | `Generated/` is up to date with `protocol.toml` (needs Python 3.11+, else not registered) |
| Integration | real server and client over loopback on every backend: handshake, ping/pong, 1 MiB both ways, 10 000 ordered packets, clean close from either side; the native relay (a filtering proxy, back-pressure through it, teardown); every misuse of the API and every allocation failure |
| Faults | a raw socket sends hostile traffic: 1 byte at a time, bad magic/version/flags/direction/sequence/id, huge lengths, bad UTF-8, hang-up mid-packet, slowloris, flood with back-pressure |
| Memory | after warm-up the allocator hook counts **zero** calls during traffic and connection churn, and nothing leaks; `check_source_rules.sh` forbids allocation and printing outside `context.c` |
| Concurrency | several contexts in parallel threads (meant for ThreadSanitizer) |
| Stress | thousands of connections (up to the OS limit), sustained traffic, memory drift check; `C2S2C_STRESS_SECONDS=600 ctest -L Stress` for the long run |
| Bindings | per binding: idiomatic API, errors, release semantics (no packet used after release), handlers and hooks (written in the language) |
| Wasm | the WebAssembly codec passes the same reference vectors as C; the web chat runs end to end (gateway, WebSocket client code, C server) and the page and every module it imports are fetched like a browser would |
| Examples | the chat server and client in Rust, Python and Node run as separate processes |
| CrossLanguage | one test per (client, server) pair among Rust, Python and Node (3×3), same scenario and checks |
| Performance | `ctest -L Performance` (release): codec, CRC and loopback round-trip benchmarks compared with the machine's baseline; per-binding cost reports. The service benchmark (`Tools/Scripts/bench_service.sh`) is run by hand |
| Fuzz | libFuzzer feeds random bytes to the decoders and to a live connection's framing under ASan/UBSan |
| Abi | exported symbols equal `Tests/Abi/symbols.txt` |

Coverage of `Source/` measured with `Tools/Scripts/coverage.sh` on macOS (C tests only, so
the Linux-only epoll/io_uring files are not in it): **96 % of lines, 85 % of branches**.

Extra runs (macOS needs Homebrew LLVM: `export CC=/opt/homebrew/opt/llvm/bin/clang`):

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # memory errors, UB
cmake --preset fuzz && cmake --build --preset fuzz && ctest --preset fuzz   # fuzz, 10 s per target
```

Apple's own clang hangs with ASan on recent macOS, and ThreadSanitizer (`tsan`) crashes there,
so run `Tools/Scripts/linux_check.sh` (Docker, Ubuntu) to run debug, asan and tsan on Linux, which
also exercises the epoll backend, and replays the runtime tests on poll and io_uring. Any runtime
test can be pointed at a backend with `C2S2C_TEST_BACKEND=<n>` (1 poll, 2 epoll, 3 kqueue, 4 io_uring). Coverage of `Source/`: 100 % lines, about 98 % branches.

Add a vector: `header_<name>` or `payload_<name>` `.bin` + `.json` in `Tests/Vectors/`
(payload json: `{"schema": "u8,string,array<u16>", "error": "C2S2C_OK"}`; `packet_<name>` uses `{"packet_id": 32770, ...}` and the registry layout).
Add a unit test: `Tests/Unit/test_x.c` + `c2s2c_unit_test(test_x)` in `Tests/CMakeLists.txt`.

## Sources

Where each idea comes from, with a short summary in our own words. Every link was opened and
checked when this section was written (November 2026).

### TCP is a stream: framing and partial reads

| Source | What it says | Used for |
|---|---|---|
| [Message Framing](https://blog.stephencleary.com/2009/04/message-framing.html), Stephen Cleary | TCP keeps no message boundaries; the application must add them. Compares length prefixes with delimiters and prefers length prefixes. | the 14-byte header with a `length` field |
| [Beej's Guide, Handling partial send()s](https://beej.us/guide/bgnet/html/#sendall), Brian Hall | `send` may write only part of a buffer; loop until everything is sent. | the per-connection send buffer and `c2s2c_conn_flush` |
| [Beej's Guide, Serialization](https://beej.us/guide/bgnet/html/#serialization) | Pack numbers into a portable binary form and wrap them in a header with type and length. | the payload format and `packet_id` |
| [Stack Overflow 41948032](https://stackoverflow.com/questions/41948032/how-does-a-tcp-packet-arrive-when-using-the-socket-api-in-c), accepted answer | A read may return part of a message, part of its prefix, or several messages: buffer until a frame is complete. In the answer's words: "TCP is a stream protocol - it transmits a stream of bytes." | `Conn/receiving.c`, the Faults tests (one byte at a time, split frames) |
| [Stack Overflow 1386142](https://stackoverflow.com/questions/1386142/handling-partial-return-from-recv-tcp-in-c) and [10250666](https://stackoverflow.com/questions/10250666/socket-programming-issue-with-recv-receiving-partial-messages) | Real cases of "truncated" messages solved by a read loop and length framing. | the problem statement in the README |

### Encoding

| Source | What it says | Used for |
|---|---|---|
| [The byte order fallacy](https://commandcenter.blogspot.com/2012/04/byte-order-fallacy.html), Rob Pike | Never depend on the CPU's byte order: build values from single bytes with shifts and the code is correct everywhere. | `Source/Codec/endian.h` |
| [Reading and Writing Packets](https://gafferongames.com/post/reading_and_writing_packets/), Glenn Fiedler | A packet reader/writer for games; check every field, because packets can come from malicious senders. | strict header and payload validation |
| [Serialization Strategies](https://gafferongames.com/post/serialization_strategies/), Glenn Fiedler | Describe a message once so reading and writing cannot drift apart; range-check what you read. | one registry (`protocol.toml`) generating every language's tables |
| [RFC 3629](https://www.rfc-editor.org/rfc/rfc3629), UTF-8 | The definition of UTF-8: no surrogates, nothing above U+10FFFF, and decoders must reject overlong forms. | `Source/Codec/utf8.c` |
| [Unicode TR #36, rev. 15, §3.1](https://www.unicode.org/reports/tr36/tr36-15.html) | Accepting ill-formed or non-shortest UTF-8 lets attackers slip characters past filters. | why invalid text closes the connection |
| [Overlong UTF-8 Escapes Bite](https://learn.microsoft.com/en-us/archive/blogs/michael_howard/overlong-utf-8-escapes-bite), Michael Howard | A real web-server flaw where overlong characters bypassed a path check (the class of bug used by the Nimda worm). | the UTF-8 test cases |

### Event loops and I/O

| Source | What it says | Used for |
|---|---|---|
| [The C10K problem](http://www.kegel.com/c10k.html), Dan Kegel | How to serve ten thousand clients at once; readiness notification (epoll, kqueue) beats one thread per client. | one event loop per context, the 10 000-connection stress test |
| [epoll(7)](https://man7.org/linux/man-pages/man7/epoll.7.html), [kqueue(2)](https://man.freebsd.org/cgi/man.cgi?query=kqueue&sektion=2) | The Linux and BSD/macOS readiness APIs: register sockets once, get many ready ones per call. | `Source/IO/io_epoll.c`, `io_kqueue.c` |
| [io_uring(7)](https://man7.org/linux/man-pages/man7/io_uring.7.html) and [Efficient IO with io_uring](https://kernel.dk/io_uring.pdf), Jens Axboe | Requests and completions go through two queues shared with the kernel, so many operations cost few system calls. | `Source/IO/io_uring.c` |
| [RFC 896](https://www.rfc-editor.org/rfc/rfc896), John Nagle | The small-packet problem and the algorithm that delays small segments. | why sockets set `TCP_NODELAY` |
| [Nagle and Delayed ACK](https://www.stuartcheshire.org/papers/NagleDelayedAck/), Stuart Cheshire; [It's always TCP_NODELAY](https://brooker.co.za/blog/2024/05/09/nagle.html), Marc Brooker ([HN discussion](https://news.ycombinator.com/item?id=40310896)) | Nagle plus delayed acknowledgements stalls request/response traffic; latency-sensitive programs should disable Nagle. | `TCP_NODELAY`, and grouping our own writes per connection instead |
| [tcp(7)](https://man7.org/linux/man-pages/man7/tcp.7.html) | `TCP_NODELAY` turns Nagle off; `TCP_CORK` gathers partial writes. | the "dirty list" that writes once per connection per batch (`c2s2c_flush`) |

### Memory, flow control, attacks

| Source | What it says | Used for |
|---|---|---|
| [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/disruptor.html), Thompson, Farley et al. | A pre-sized ring buffer with reused slots avoids allocation on the hot path and cuts latency. | `Source/Memory/ring.c`, all memory reserved at `c2s2c_create` |
| [Backpressuring in Streams](https://nodejs.org/en/learn/modules/backpressuring-in-streams), Node.js | A fast producer overwhelms a slow consumer unless buffers are bounded and the producer is paused. | reading pauses when the application or a relay peer is behind |
| [Slowloris](https://en.wikipedia.org/wiki/Slowloris_%28cyber_attack%29), Wikipedia | One client keeps many connections open by sending very slowly; mitigations are time limits and minimum rates. | `idle_timeout_ms`, the slowloris tests |

### Checksum

| Source | What it says | Used for |
|---|---|---|
| [RFC 7143](https://www.rfc-editor.org/rfc/rfc7143) (iSCSI) | Requires the CRC-32C (Castagnoli) digest, polynomial 0x1EDC6F41, for integrity. | the choice of CRC-32C for `c2s2c_crc32c` |
| [x86 CRC32 instruction](https://www.felixcloutier.com/x86/crc32) (copy of Intel's manual), [Arm C Language Extensions](https://arm-software.github.io/acle/main/acle.html) | The SSE4.2 and Armv8 instructions that compute CRC-32C in hardware. | `Source/Simd/crc32_sse42.c`, `crc32_armv8.c` |

### Testing

| Source | What it says | Used for |
|---|---|---|
| [libFuzzer](https://llvm.org/docs/LibFuzzer.html) | Coverage-guided, in-process fuzzing through a `LLVMFuzzerTestOneInput` function. | `Tests/Fuzz/` |
| [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html), [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html) | Compiler modes that catch out-of-bounds accesses, use-after-free and data races at run time. | the `asan` and `tsan` presets, CI |

### Bindings

| Source | What it says | Used for |
|---|---|---|
| [ctypes](https://docs.python.org/3/library/ctypes.html) | Load a shared library and describe its functions and structs from plain Python. | `Bindings/Python` (no compiled extension) |
| [Node-API](https://nodejs.org/api/n-api.html) | A stable C interface for native Node add-ons, including buffer exchange. | `Bindings/Node/Source/binding.c` |
| [The Rustonomicon, FFI](https://doc.rust-lang.org/nomicon/ffi.html) | Call C through `extern` blocks and wrap the unsafe calls in safe types. | `Bindings/Rust` (`c2s2c-sys` raw, `c2s2c` safe) |

### Existing libraries in the same space

| Library | What it is | Difference with C2S2C |
|---|---|---|
| [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets), Valve | Message transport over UDP with reliability, fragmentation and encryption. | UDP and encryption; C2S2C is TCP with a typed packet registry shared by several languages. |
| [ENet](http://enet.bespin.org/), Lee Salzman | A small C library for reliable, ordered delivery over UDP, common in games. | no schema or payload validation; C2S2C validates every field against the registry. |

## Planned

- An **IOCP backend** for Windows (today Windows uses `WSAPoll`), and porting the bindings and
  the shell-based checks to Windows.
- Long runs: fuzzing for hours and the 10-minute stress run (`C2S2C_STRESS_SECONDS=600`) were not
  executed for this version; the short versions run in CI.

License: MIT.
