// test_wasm.ts - the WebAssembly codec: the shared reference vectors, checksum, frames.
// SPDX-License-Identifier: MIT
// Run: node --experimental-strip-types --test test_wasm.ts   (DUPLEX_WASM = path of the .wasm)
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { DuplexError, ERROR_MESSAGES } from "../../Bindings/Node/Source/codec.ts";
import { loadCodec } from "../../Bindings/Wasm/Source/duplex_wasm.ts";
import * as reg from "../../Generated/TypeScript/registry.ts";

const root = path.resolve(import.meta.dirname, "../..");
const codec = await loadCodec(fs.readFileSync(process.env.DUPLEX_WASM as string));

// Error names of the vector files (DX_ERR_BAD_MAGIC...) -> codes, read from the public header.
const codes: Record<string, number> = {};
for (const m of fs.readFileSync(path.join(root, "Include/Duplex/dx_errors.h"), "utf8").matchAll(/#define (DX_\w+)\s+\(?(-?\d+)\)?/g)) codes[m[1]] = Number(m[2]);

test("the module loads and reports the library version", () => {
  assert.equal(codec.abiVersion() >> 16, 1);
  for (const code of Object.keys(ERROR_MESSAGES).map(Number)) assert.equal(codec.strerror(code), ERROR_MESSAGES[code]);
});

// The same .bin/.json pairs that the C, Python, Rust and Node tests use.
const vectorsDir = path.join(root, "Tests/Vectors");
const vectors = fs.readdirSync(vectorsDir).filter((f) => f.endsWith(".bin")).sort();

test(`all ${vectors.length} reference vectors give the expected result`, () => {
  assert.ok(vectors.length >= 20);
  for (const file of vectors) {
    const name = file.replace(/\.bin$/, "");
    const bytes = new Uint8Array(fs.readFileSync(path.join(vectorsDir, file)));
    const expected = JSON.parse(fs.readFileSync(path.join(vectorsDir, name + ".json"), "utf8"));
    if (name.startsWith("header_")) {
      if (expected.error) {
        assert.throws(() => codec.decodeHeader(bytes), (e: any) => e instanceof DuplexError && e.code === codes[expected.error], name);
      } else {
        const h = codec.decodeHeader(bytes);
        const want = expected.header;
        assert.deepEqual([h.magic, h.version, h.flags, h.id, h.length, h.sequence],
          [want.magic, want.version, want.flags, want.packet_id, want.length, want.sequence], name);
      }
    } else if (expected.schema !== undefined) {
      assert.equal(codec.validateSchema(expected.schema, bytes), codes[expected.error], name);
    } else {
      assert.equal(codec.validate(expected.packet_id, bytes), codes[expected.error], name);
    }
  }
});

test("CRC-32C gives the standard value and chains", () => {
  const data = new TextEncoder().encode("123456789");
  assert.equal(codec.crc32c(data), 0xe3069283);
  assert.equal(codec.crc32c(data.subarray(4), codec.crc32c(data.subarray(0, 4))), 0xe3069283);
});

test("frames round trip and bad frames are rejected", () => {
  const payload = new Uint8Array([7, 0, 0, 0]);
  const frame = codec.encodeFrame(reg.PING, payload, 5);
  assert.equal(frame.length, 18);
  assert.deepEqual([...frame.subarray(0, 4)], [0xde, 0xc0, 1, 0]); // magic, version, flags
  const parsed = codec.decodeFrame(frame);
  assert.deepEqual([parsed.id, parsed.sequence, [...parsed.payload]], [reg.PING, 5, [7, 0, 0, 0]]);
  assert.throws(() => codec.decodeFrame(frame.subarray(0, 17)), (e: any) => e.code === -2); // truncated
  assert.throws(() => codec.decodeFrame(new Uint8Array([...frame, 0])), (e: any) => e.code === -10); // trailing
  assert.throws(() => codec.encodeFrame(reg.LOGIN, new Uint8Array([2, 0, 0xc0, 0x80]), 0), (e: any) => e.code === -8); // invalid UTF-8
  assert.throws(() => codec.encodeFrame(0x1234, payload, 0), (e: any) => e.code === -13); // unknown id
});
