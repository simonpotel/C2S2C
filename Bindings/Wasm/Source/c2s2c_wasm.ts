// c2s2c_wasm.ts - the C2S2C codec compiled to WebAssembly, with a small JavaScript wrapper.
// SPDX-License-Identifier: MIT
//
// The browser has no sockets we can use directly, but it can run the very same C
// code that checks headers, payloads (UTF-8 included) and checksums on the server. This wrapper
// loads that module and exposes it as functions on Uint8Array. Nothing here touches the network.
import { C2S2CError } from "../../Node/Source/codec.ts";

export const HEADER_SIZE = 14;
const STOC_FLAG = 1;

export interface Header {
  magic: number;
  version: number;
  flags: number;
  id: number;
  length: number;
  sequence: number;
}

export interface Frame {
  id: number;
  sequence: number;
  flags: number;
  payload: Uint8Array;
}

export interface Codec {
  abiVersion(): number;
  strerror(code: number): string;
  /** Parses and validates a 14-byte header; throws C2S2CError with the precise reason. */
  decodeHeader(bytes: Uint8Array): Header;
  encodeHeader(h: { flags: number; id: number; length: number; sequence: number }): Uint8Array;
  /** Result code (0 = valid) for a payload checked against the registry layout of packet `id`. */
  validate(id: number, payload: Uint8Array): number;
  /** Result code for a payload checked against a schema text such as "u32,string,array<u16>". */
  validateSchema(schema: string, payload: Uint8Array): number;
  crc32c(data: Uint8Array, crc?: number): number;
  /** Checks a whole frame (header, length, payload layout) and returns its parts. */
  decodeFrame(frame: Uint8Array): Frame;
  /** Builds a whole frame; `stoc` marks a server-to-client packet. */
  encodeFrame(id: number, payload: Uint8Array, sequence: number, stoc?: boolean): Uint8Array;
}

/** Instantiates the module from its bytes (read a file in Node, `fetch()` it in a browser). */
export async function loadCodec(wasm: BufferSource): Promise<Codec> {
  const { instance } = await WebAssembly.instantiate(wasm, {});
  const e = instance.exports as Record<string, any>;
  const memory = e.memory as WebAssembly.Memory;
  const scratch: number = e.c2s2c_wasm_scratch();
  const capacity: number = e.c2s2c_wasm_scratch_size();
  const bytes = () => new Uint8Array(memory.buffer, scratch, capacity);
  const text = new TextEncoder();

  const put = (data: Uint8Array, at = 0) => {
    if (at + data.length > capacity) throw new RangeError("payload too large for the WebAssembly scratch area");
    bytes().set(data, at);
  };
  const check = (rc: number) => {
    if (rc < 0) throw new C2S2CError(rc);
  };

  const codec: Codec = {
    abiVersion: () => e.c2s2c_abi_version() >>> 0,
    strerror: (code) => new TextDecoder().decode(bytes().slice(0, e.c2s2c_wasm_strerror(code))),
    decodeHeader(header) {
      put(header.subarray(0, HEADER_SIZE));
      check(e.c2s2c_wasm_decode_header(Math.min(header.length, HEADER_SIZE)));
      const f = new Uint32Array(memory.buffer, scratch + 16, 6);
      return { magic: f[0], version: f[1], flags: f[2], id: f[3], length: f[4], sequence: f[5] };
    },
    encodeHeader(h) {
      check(e.c2s2c_wasm_encode_header(h.flags, h.id, h.length, h.sequence));
      return bytes().slice(0, HEADER_SIZE);
    },
    validate(id, payload) {
      put(payload);
      return e.c2s2c_wasm_validate(id, payload.length);
    },
    validateSchema(schema, payload) {
      const s = text.encode(schema);
      put(payload);
      put(s, payload.length);
      return e.c2s2c_wasm_validate_schema(payload.length, s.length);
    },
    crc32c(data, crc = 0) {
      put(data);
      return e.c2s2c_wasm_crc32c(crc, data.length) >>> 0;
    },
    decodeFrame(frame) {
      const h = codec.decodeHeader(frame);
      if (frame.length !== HEADER_SIZE + h.length) throw new C2S2CError(h.length > frame.length - HEADER_SIZE ? -2 : -10);
      const payload = frame.slice(HEADER_SIZE);
      check(codec.validate(h.id, payload));
      return { id: h.id, sequence: h.sequence, flags: h.flags, payload };
    },
    encodeFrame(id, payload, sequence, stoc = false) {
      const header = codec.encodeHeader({ flags: stoc ? STOC_FLAG : 0, id, length: payload.length, sequence });
      check(codec.validate(id, payload));
      const out = new Uint8Array(HEADER_SIZE + payload.length);
      out.set(header);
      out.set(payload, HEADER_SIZE);
      return out;
    },
  };
  return codec;
}
