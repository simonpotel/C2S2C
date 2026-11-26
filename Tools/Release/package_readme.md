# C2S2C release archive

This archive is the C2S2C library for one platform, with everything needed to use it from any
language that can call a C library (C, C++, C#, Go, Java, Python, Rust, Swift...). It contains
no code specific to one language.

| Path | Content |
|---|---|
| `include/C2S2C/c2s2c.h` | the whole C interface (every function, documented in the header) |
| `include/C2S2C/c2s2c_types.h`, `c2s2c_errors.h`, `c2s2c_version.h` | structures, error codes, versions |
| `include/c2s2c_packets.h` | the packet ids of the default registry (`C2S2C_PKT_PING`...) |
| `lib/` (and `bin/` on Windows) | the shared library (`libc2s2c.so` / `libc2s2c.dylib` / `c2s2c.dll`) and a static one |
| `lib/pkgconfig/c2s2c.pc` | compiler and linker flags for C/C++ build systems |
| `share/c2s2c/protocol.toml` | the packet definitions (ids and field layouts) |
| `share/c2s2c/registry.txt` | the same, one line per packet (`0x8002 chat_message u32,string,u64`), easy to read from any language |

## From C or C++

```bash
cc app.c -I<archive>/include -L<archive>/lib -lc2s2c      # or: pkg-config --cflags --libs c2s2c
```

## From any other language

1. Load the shared library with the language's foreign-function interface: C# `DllImport`,
   Java JNA or Panama, Go cgo, Python `ctypes`, Rust `extern "C"`, Swift, etc.
2. Declare the functions you use exactly as in `include/C2S2C/c2s2c.h`. All of them use plain C
   types; structures (`c2s2c_config`, `c2s2c_packet`, `c2s2c_outgoing`) are ordinary C structs
   with natural alignment.
3. Read the packet ids and layouts from `share/c2s2c/registry.txt` (or generate constants from
   it at build time).
4. Check `c2s2c_abi_version()` at start-up: the major number must match the one in
   `c2s2c_version.h`.

Ready-made bindings to copy or depend on are in the repository: `Bindings/Rust`,
`Bindings/Python`, `Bindings/Node`, `Bindings/Cpp`, `Bindings/Wasm`. They are also complete
examples of steps 1 to 4. Documentation: `DOCUMENTATION.md` in the repository.

To use your own packets, edit `protocol.toml` and rebuild the library from source
(`Tools/Codegen/gen_registry.py`, then CMake).

License: MIT (`share/c2s2c/LICENSE`).
