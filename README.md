# C2S2C

![C23](https://img.shields.io/badge/C23-00599C?style=for-the-badge&logo=c&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white)
![No dependencies](https://img.shields.io/badge/Dependencies-none-2EA44F?style=for-the-badge)
![License MIT](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)

**One packet-protocol engine in C, used from Rust, Python, Node and the browser, instead of
rewriting it in every project.**

*C2S2C* reads "client to server to client": packets travel both ways, client to server (CTOS) and
server to client (STOC).

**How it works:**

![Encoding: binary, little-endian](https://img.shields.io/badge/Encoding-binary%2C%20little--endian-455A64?style=for-the-badge)
![Framing: 14-byte header + length](https://img.shields.io/badge/Framing-14--byte%20header%20%2B%20length-455A64?style=for-the-badge)
![Fields: ints · floats · bool · string · bytes · array](https://img.shields.io/badge/Fields-ints%20%C2%B7%20floats%20%C2%B7%20bool%20%C2%B7%20string%20%C2%B7%20bytes%20%C2%B7%20array-455A64?style=for-the-badge)
![Text: strict UTF-8](https://img.shields.io/badge/Text-strict%20UTF--8-455A64?style=for-the-badge)
![Schema: one TOML registry](https://img.shields.io/badge/Schema-one%20TOML%20registry-455A64?style=for-the-badge)
![I/O: epoll · kqueue · io_uring · poll](https://img.shields.io/badge/I%2FO-epoll%20%C2%B7%20kqueue%20%C2%B7%20io__uring%20%C2%B7%20poll-1565C0?style=for-the-badge)
![Delivery: batched · zero-copy](https://img.shields.io/badge/Delivery-batched%20%C2%B7%20zero--copy-1565C0?style=for-the-badge)
![Writes: grouped per connection](https://img.shields.io/badge/Writes-grouped%20per%20connection-1565C0?style=for-the-badge)
![Memory: no allocation at runtime](https://img.shields.io/badge/Memory-no%20allocation%20at%20runtime-1565C0?style=for-the-badge)
![Flow control: back-pressure](https://img.shields.io/badge/Flow%20control-back--pressure-1565C0?style=for-the-badge)
![Checksum: CRC-32C, SSE4.2 / ARMv8](https://img.shields.io/badge/Checksum-CRC--32C%2C%20SSE4.2%20%2F%20ARMv8-6A1B9A?style=for-the-badge)
![Tested with: libFuzzer · ASan · UBSan · TSan](https://img.shields.io/badge/Tested%20with-libFuzzer%20%C2%B7%20ASan%20%C2%B7%20UBSan%20%C2%B7%20TSan-2E7D32?style=for-the-badge)

## The problem

Every networked project ends up rewriting the same low-level code in its own language: cutting
a TCP stream into packets, checking every header field, refusing hostile input, numbering
packets, buffering, an event loop. It is tedious and slow to write, easy to get subtly wrong, and
it is written again for each language of the stack.

C2S2C writes it **once**, in C23, and every language uses the same compiled library
(`libc2s2c`) through a thin binding. Packets are defined once in `Registry/protocol.toml`.

## The problem, as people meet it

The same questions come back on forums whenever someone writes this layer by hand:

- [Handling partial return from recv() TCP in C](https://stackoverflow.com/questions/1386142/handling-partial-return-from-recv-tcp-in-c) (Stack Overflow): one message can need several reads, and several messages can arrive in one.
- [recv() receiving partial messages](https://stackoverflow.com/questions/10250666/socket-programming-issue-with-recv-receiving-partial-messages) (Stack Overflow): a market-data feed that looks truncated, until framing is added.
- [How does a TCP packet arrive when using the Socket API](https://stackoverflow.com/questions/41948032/how-does-a-tcp-packet-arrive-when-using-the-socket-api-in-c) (Stack Overflow): a length-prefixed message can be split or merged with the next one.
- [It's always TCP_NODELAY](https://news.ycombinator.com/item?id=40310896) (Hacker News): latency surprises from TCP's small-packet handling, told by game and network engineers.
- [Message Framing](https://blog.stephencleary.com/2009/04/message-framing.html) (Stephen Cleary) and [Reading and Writing Packets](https://gafferongames.com/post/reading_and_writing_packets/) (Glenn Fiedler, games): the classic explanations of what you must build yourself.

Every concept C2S2C uses is sourced in [DOCUMENTATION.md, "Sources"](DOCUMENTATION.md#sources).

## What you gain

The same ping/pong service, written from scratch in each language versus on top of C2S2C
(`Tools/Scripts/bench_service.sh`, Apple silicon, loopback, 192 connections):

| | Rewritten by hand | With C2S2C |
|---|---|---|
| Code for the service | 48–82 lines **for one packet type** | 20–27 lines, every packet of the registry |
| Validation (headers, direction, sequence, lengths, UTF-8, idle peers) | yours to write and maintain, per language | built in, identical everywhere |
| Rust: round trips/s, p99 | 419 000, 3.7 ms | **1 341 000, 0.7 ms** |
| Node: round trips/s, p99 | 688 000, 1.3 ms | **1 148 000, 0.9 ms** |
| Python: round trips/s, p99 | 440 000, 1.9 ms | **633 000, 2.1 ms** |
| 17 hostile-traffic checks (bad headers, 4 GiB length, garbage, slowloris, floods) | pass only because each checker was rewritten | pass, from one tested implementation |
| Verified by | your own tests | fuzzing, AddressSanitizer, ThreadSanitizer, property tests, CI on Linux/macOS/Windows |
| Memory | allocates as it goes | reserved up front, no allocation while running |

Python is faster with C2S2C when it loops over batches (`poll` + `send_many`). A Python
function called for every packet (`dispatch`) is slower than a hand-written loop (374 000/s).

## Use cases

Anywhere several programs exchange many small, typed messages over TCP and must not trust each
other's bytes:

| Use case | What travels | Why C2S2C fits |
|---|---|---|
| **Multiplayer games** | player inputs to the server (CTOS), world state and events to players (STOC) | thousands of connections per process, batched receive, grouped sends, a C++/Rust game server and a browser client sharing one packet list |
| **Chat and real-time apps** | messages, presence, notifications | strict UTF-8 checks on every string, idle peers dropped, the same protocol in the web page through WebAssembly |
| **Microservices in several languages** | internal requests and events between Rust, Python and Node services | one registry instead of one hand-written encoder per language; a field added once reaches every service |
| **Proxies and gateways** | the traffic of a client, relayed to the real server | `c2s2c_tunnel` relays inside the library, drops chosen packet types, and slows the sender instead of buffering without limit |
| **Telemetry and IoT ingestion** | small readings from many devices | fixed memory, no allocation while running, malformed device data rejected with a precise reason |
| **Low-latency feeds** (market data, simulations, robotics) | frequent small updates | sub-millisecond round trips on loopback, one write per connection per batch |

## What is inside

| Folder | Content |
|---|---|
| `Include/`, `Source/` | the C library: framing, validation, batched `c2s2c_poll`, sockets (epoll, kqueue, io_uring, poll/WSAPoll), a native relay |
| `Registry/`, `Generated/` | the packet definitions; constants generated for C, C++ and Rust (Python, Node and browsers read the table from the library) |
| `Bindings/` | Rust, Python, Node/TypeScript, C++, and a 6 KB WebAssembly build for browsers |
| `Examples/` | the same chat server and client in Rust, Python and Node, and a web chat |
| `Tests/`, `Bench/` | C tests, fuzzing, faults, stress, cross-language tests, benchmarks |

Platforms tested in CI (the library on all three; the bindings and examples on Linux and macOS):

![Linux](https://img.shields.io/badge/Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black)
![macOS](https://img.shields.io/badge/macOS-000000?style=for-the-badge&logo=apple&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white)

Bindings and examples:

![Rust](https://img.shields.io/badge/Rust-000000?style=for-the-badge&logo=rust&logoColor=white)
![Python](https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white)
![Node.js](https://img.shields.io/badge/Node.js-339933?style=for-the-badge&logo=nodedotjs&logoColor=white)
![TypeScript](https://img.shields.io/badge/TypeScript-3178C6?style=for-the-badge&logo=typescript&logoColor=white)
![WebAssembly](https://img.shields.io/badge/WebAssembly-654FF0?style=for-the-badge&logo=webassembly&logoColor=white)

## Use it

```bash
cmake --preset release && cmake --build --preset release   # CMake ≥ 3.25, Ninja, a C23 compiler
ctest --preset release                                     # every test this machine can run
```

```python
import c2s2c                                      # Bindings/Python
from c2s2c import packets as reg                  # packet ids, read from the library
ctx = c2s2c.Context(); ctx.listen("127.0.0.1", 0)
while True:
    with ctx.poll(100) as batch:                    # many packets per call into the library
        ctx.send_many([(p.conn, reg.PONG, p) for p in batch if p.id == reg.PING])
```

Rust, Node and the browser look the same: see `Examples/` and [DOCUMENTATION.md](DOCUMENTATION.md)
(wire format, API, every binding, tests, performance). Integrating it with an agent: [USE.md](USE.md).

**As a dependency, from any language:** each [release](../../releases) has one archive per
platform (`c2s2c-<version>-<platform>.tar.gz`: headers, shared and static libraries, packet
registry, pkg-config file). Nothing in it is tied to one language: C#, Go, Java... load the
library with their FFI and declare the functions of `include/C2S2C/c2s2c.h`.

## Also an experiment

This project is also a test of how far an AI coding agent (Claude Code) can go on **low-level
work**: design a binary protocol, write the C, the sockets and the bindings, then prove with
fuzzing, sanitizers and benchmarks that the shared engine is faster and safer than rewriting the
mechanism in each project. The rules the agent follows when changing C2S2C are in [AGENTS.md](AGENTS.md); how an agent should
use it in another project is in [USE.md](USE.md).

## Status

Everything above is implemented and tested in CI. Known gaps: no IOCP backend on Windows (it uses
`WSAPoll`), and the stress test times out on the io_uring backend (being fixed).

License: MIT.
