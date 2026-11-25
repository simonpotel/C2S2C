#!/bin/sh
# Builds the WebAssembly codec: Build/Wasm/duplex_codec.wasm (header, payload, UTF-8, registry, CRC).
# Needs clang with the wasm32 target and wasm-ld (LLVM). Usage: build.sh [output-file]
# Without Emscripten: the sources are freestanding C, so plain clang is enough.
cd "$(dirname "$0")/../.." || exit 1
out=${1:-Build/Wasm/duplex_codec.wasm}
clang=${DX_WASM_CLANG:-$(command -v /opt/homebrew/opt/llvm/bin/clang || command -v clang)}
ld=${DX_WASM_LD:-$(command -v wasm-ld)}
mkdir -p "$(dirname "$out")"

# -nostdinc keeps the host's C library headers out: only the compiler's own freestanding headers
# (stdint.h, stddef.h) and our tiny string.h are used.
resource=$("$clang" --print-resource-dir)
exports="dx_abi_version dx_strerror dx_header_encode dx_header_decode dx_payload_validate dx_packet_name \
dx_crc32c dx_wasm_scratch dx_wasm_scratch_size dx_wasm_validate_schema dx_wasm_validate \
dx_wasm_decode_header dx_wasm_encode_header dx_wasm_crc32c dx_wasm_strerror"
export_flags=""
for e in $exports; do export_flags="$export_flags -Wl,--export=$e"; done

# shellcheck disable=SC2086
"$clang" --target=wasm32 -O2 -std=c11 -Wall -Wextra -Werror -nostdlib -nostdinc -ffreestanding \
    -isystem "$resource/include" -IBindings/Wasm/Source/Include -IInclude -IGenerated/C -ISource -ISource/Internal \
    -fuse-ld="$ld" -Wl,--no-entry $export_flags -Wl,--initial-memory=2097152 \
    -DDX_BUILDING_SHARED -o "$out" \
    Bindings/Wasm/Source/wasm_glue.c Source/Core/header.c Source/Core/version.c Source/Core/registry.c \
    Source/Codec/encode.c Source/Codec/decode.c Source/Codec/utf8.c Source/Simd/crc32_generic.c \
    Generated/C/dx_registry.c
