# Duplex

**One packet-protocol engine in C, used from Rust, Python, Node and the browser, instead of
rewriting it in every project.**

## The problem

Every networked project ends up rewriting the same low-level code in its own language: cutting
a TCP stream into packets, checking every header field, refusing hostile input, numbering
packets, buffering, an event loop. It is tedious and slow to write, easy to get subtly wrong, and
it is written again for each language of the stack.

Duplex writes it **once**, in C11, and every language uses the same compiled library
(`libduplex`) through a thin binding. Packets are defined once in `Registry/protocol.toml`.

## What you gain

The same ping/pong service, written from scratch in each language versus on top of Duplex
(`Tools/Scripts/bench_service.sh`, Apple silicon, loopback, 192 connections):

| | Rewritten by hand | With Duplex |
|---|---|---|
| Code for the service | 48–82 lines **for one packet type** | 20–27 lines, every packet of the registry |
| Validation (headers, direction, sequence, lengths, UTF-8, idle peers) | yours to write and maintain, per language | built in, identical everywhere |
| Rust: round trips/s, p99 | 419 000, 3.7 ms | **1 341 000, 0.7 ms** |
| Node: round trips/s, p99 | 688 000, 1.3 ms | **1 148 000, 0.9 ms** |
| Python: round trips/s, p99 | 440 000, 1.9 ms | **633 000, 2.1 ms** |
| 17 hostile-traffic checks (bad headers, 4 GiB length, garbage, slowloris, floods) | pass only because each checker was rewritten | pass, from one tested implementation |
| Verified by | your own tests | fuzzing, AddressSanitizer, ThreadSanitizer, property tests, CI on Linux/macOS/Windows |
| Memory | allocates as it goes | reserved up front, no allocation while running |

Python is faster with Duplex when it loops over batches (`poll` + `send_many`). A Python
function called for every packet (`dispatch`) is slower than a hand-written loop (374 000/s).

## What is inside

| Folder | Content |
|---|---|
| `Include/`, `Source/` | the C library: framing, validation, batched `dx_poll`, sockets (epoll, kqueue, io_uring, poll/WSAPoll), a native relay |
| `Registry/`, `Generated/` | the packet definitions, and the ids generated from them for each language |
| `Bindings/` | Rust, Python, Node/TypeScript, C++, and a 6 KB WebAssembly build for browsers |
| `Examples/` | the same chat server and client in Rust, Python and Node, and a web chat |
| `Tests/`, `Bench/` | C tests, fuzzing, faults, stress, cross-language tests, benchmarks |

## Use it

```bash
cmake --preset release && cmake --build --preset release   # CMake ≥ 3.20, Ninja, a C11 compiler
ctest --preset release                                     # every test this machine can run
```

```python
import duplex, duplex_registry as reg              # Bindings/Python + Generated/Python
ctx = duplex.Context(); ctx.listen("127.0.0.1", 0)
while True:
    with ctx.poll(100) as batch:                    # many packets per call into the library
        ctx.send_many([(p.conn, reg.PONG, p) for p in batch if p.id == reg.PING])
```

Rust, Node and the browser look the same: see `Examples/` and [DOCUMENTATION.md](DOCUMENTATION.md)
(wire format, API, every binding, tests, performance).

## Also an experiment

This project is also a test of how far an AI coding agent (Claude Code) can go on **low-level
work**: design a binary protocol, write the C, the sockets and the bindings, then prove with
fuzzing, sanitizers and benchmarks that the shared engine is faster and safer than rewriting the
mechanism in each project. The rules the agent follows are in [AGENTS.md](AGENTS.md).

## Status

Everything above is implemented and tested in CI. Known gaps: no IOCP backend on Windows (it uses
`WSAPoll`), and the stress test times out on the io_uring backend (being fixed).

License: MIT.
