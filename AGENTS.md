# AGENTS.md — rules (MUST / MUST NEVER)

## Purpose
- The C library is the data path only: framing, validation, batched `c2s2c_poll`, grouped writes, the
  native relay (`c2s2c_tunnel`). It MUST NEVER call back into application code; handlers and hooks
  belong in the bindings, over batched polls.
- Any claim that C2S2C is faster or safer than rewriting the protocol MUST be backed by
  `Tools/Scripts/bench_service.sh` and the hostile-traffic checker.

## Naming and structure
- Directories MUST be PascalCase; acronyms keep their case (`IO/`, `Abi/`, `Simd/`, `Cpp/`, `Wasm/`).
  Exceptions: `.github/`, tool dot-dirs (`.cargo/`), `node_modules/`.
- Files MUST be `snake_case`; only conventional root files are upper-case (`README.md`,
  `AGENTS.md`, `DOCUMENTATION.md`, `LICENSE`, `CMakeLists.txt`). `check_naming.sh` MUST pass.
- Public symbols MUST be prefixed `c2s2c_`, macros/constants `C2S2C_`. MUST NEVER use a `_t` suffix.
- Documentation is `README.md` (under two minutes to read) and `DOCUMENTATION.md`; nothing else.
- Examples exist only in Rust, Python and Node (plus the web chat).

## Language and build
- C11 strict, warning-free with `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
  -Wcast-align -Wstrict-prototypes`. ASan, UBSan and TSan MUST pass.
- `Source/` MUST NOT have external dependencies, allocate outside `Ctx/context.c`, or print
  (`check_source_rules.sh`).

## ABI and API
- MUST NEVER reorder, remove or retype a public field: append, bump `C2S2C_ABI_VERSION`, update
  `Tests/Abi/symbols.txt` (`check_abi.sh <lib> --print`).
- Functions return `int`: 0 = success, < 0 = `C2S2C_ERR_*`. MUST NEVER `abort()`/`exit()`/active `assert`.
- Handles are opaque; no internal struct in `Include/`.
- An API change MUST update the API section of DOCUMENTATION.md in the same commit.

## Binary format
- Wire integers MUST be read/written through `Source/Codec/endian.h` only. MUST NEVER cast a buffer
  to a struct, use `#pragma pack`, or assume endianness.
- Every read MUST be bounds-checked; network data is never trusted; invalid input gets a precise error.
- A format change MUST update DOCUMENTATION.md and `Tests/Vectors/`.

## Registry
- `Registry/protocol.toml` is the ONLY source of packet IDs. `Generated/` MUST NEVER be hand-edited.
  A removed ID is marked `deprecated = true`, never reused.

## Performance
- Hot path: zero allocation. Every optimisation needs a before/after benchmark.
- SIMD/asm only in `Source/Simd/`, always with a tested portable fallback.

## Style
- `clang-format` (100 columns, 4 spaces). Functions do one thing; > 60 lines MUST be split or justified.
- File header: name, one-line role, `SPDX-License-Identifier: MIT`.
- `/** */` doc on every public function: role, params, return, errors, memory ownership, threading.
- No mutable globals; internal things are `static`.
- Comments are in English and explain the why. MUST NEVER write banner comments (`/* ---- x ---- */`)
  or start a comment with "In plain words"; split code into files instead.
- Write as little code as possible.

## Tests
- No function without a test; no bug fix without a regression test. Library tests are in C.
- MUST NEVER delete, disable or weaken a test, warning or sanitizer to get green.
- MUST NEVER claim a test passed without running it, nor commit while a test fails (gate on the
  exit code of `ctest`, not on its printed output).

## Process
- Read AGENTS.md then DOCUMENTATION.md before changing anything. Small atomic changes.
- Conventional Commits in English, without AI attribution lines. Branch `master`.
- Blocking ambiguity: ask; otherwise decide prudently and record it in DOCUMENTATION.md
  ("Why it is built this way").
