# USE.md — guide for an agent integrating C2S2C

Read this before adding C2S2C to a project. Then [README.md](README.md) (what it is and why, two
minutes) and [DOCUMENTATION.md](DOCUMENTATION.md) (wire format, full API, every binding). If you
change C2S2C itself, follow [AGENTS.md](AGENTS.md).

## What it is for

A project where programs (possibly in different languages) exchange typed messages over TCP:
game client and server, chat, services talking to each other, a proxy. C2S2C gives them one
tested engine for framing, validation, sockets and batching. Do not re-implement packet framing,
header checks or a socket loop in the project: call C2S2C.

## 1. Get the library

- **Release archive** (no build needed): download `c2s2c-<version>-<platform>.tar.gz` from the
  repository's GitHub releases (Linux, macOS, Windows; x64 and arm64). It contains `include/`,
  `lib/` (`bin/` on Windows), `lib/pkgconfig/c2s2c.pc` and `share/c2s2c/` (`protocol.toml`,
  `registry.txt`, its own README).
- **From source** (needed to use your own packets): CMake ≥ 3.25, Ninja, a C23 compiler.
  ```bash
  cmake --preset release && cmake --build --preset release && ctest --preset release
  cmake --install Build/release --prefix <where>
  ```

## 2. Define the packets

Edit `Registry/protocol.toml` (one `[[packet]]` per message: `name`, `id`, `fields`), then run
`python3.11+ Tools/Codegen/gen_registry.py` and rebuild. Rules: bit 15 of the id set = server to
client; ids are never reused (mark `deprecated = true`). The library carries the table:
Python, Node and browsers read it at run time; C, C++ and Rust get generated constants.

## 3. Call it from the project's language

| Language | How |
|---|---|
| C | `#include <C2S2C/c2s2c.h>` and `<c2s2c_packets.h>`, link `-lc2s2c` (or `pkg-config c2s2c`) |
| C++ | add `Bindings/Cpp` to the include path, `#include <c2s2c.hpp>` |
| Python ≥ 3.9 | `pip install ./Bindings/Python`, set `C2S2C_LIBRARY=/path/to/libc2s2c.*`, then `import c2s2c` and `from c2s2c import packets` |
| Rust | path dependency on `Bindings/Rust/C2S2C`, build with `C2S2C_LIB_DIR=<folder of libc2s2c>` |
| Node ≥ 22.6 | build the addon (`c2s2c.node`, CMake target `c2s2c_node`), set `C2S2C_ADDON` to it, import `Bindings/Node/Source/index.ts` |
| Browser | `Bindings/Wasm/build.sh`, then `loadCodec` + `WebClient` (a gateway such as `Examples/Web/Gateway` bridges WebSocket to TCP) |
| Anything else (C#, Go, Java, Swift…) | load the shared library with the language's FFI, declare the functions of `c2s2c.h`, read packets from `registry.txt`, check `c2s2c_abi_version()` |

Working examples: `Examples/Rust`, `Examples/Python`, `Examples/Node` (chat server and client),
`Examples/Web` (browser chat).

## 4. The rules that matter

- One context per thread. Loop: `poll` a batch → handle it → `release` it. Packet data is valid
  only until the release; copy what you keep.
- `send` queues; bytes leave at the next `poll` (or `flush`). `WOULD_BLOCK` = buffer full: poll,
  then retry.
- The side that listens sends ids with bit 15 set; the side that connects sends ids without it.
- A `DISCONNECTED` event carries the reason (0 = normal close, negative = the error).
- For speed in Python and Node, answer a batch with `send_many` / `sendMany`.

## 5. Verify

`ctest --preset debug` in the C2S2C checkout runs everything available on the machine. In your
project, test against a real C2S2C peer (the examples can serve as one). Performance claims:
`Tools/Scripts/bench_service.sh Build/release`.
