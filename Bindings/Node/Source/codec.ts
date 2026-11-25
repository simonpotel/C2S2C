// codec.ts - payload encoding/decoding by registry layout; no Node-specific code, so browsers use it too.
// SPDX-License-Identifier: MIT
//
// In plain words: a packet's payload is a plain sequence of fields (see DOCUMENTATION.md). The
// layout of each packet comes from the registry; this file turns JavaScript values into those
// bytes and back. (The C library re-validates everything it sends and receives.)
import { PACKETS } from "../../../Generated/TypeScript/registry.ts";

export const ERROR_MESSAGES: Record<number, string> = {
  0: "success", [-1]: "invalid argument", [-2]: "truncated input", [-3]: "bad magic",
  [-4]: "unsupported protocol version", [-5]: "reserved flag bits set",
  [-6]: "packet id does not match direction", [-7]: "payload too large", [-8]: "invalid UTF-8",
  [-9]: "invalid value for type", [-10]: "unexpected trailing bytes", [-11]: "invalid schema",
  [-12]: "output buffer too small", [-13]: "unknown packet id", [-14]: "send buffer full",
  [-15]: "socket error", [-16]: "unexpected sequence number",
  [-17]: "packet travels the wrong direction", [-18]: "out of memory", [-19]: "limit reached",
  [-20]: "invalid connection", [-21]: "connection timed out", [-22]: "rejected by a hook",
};

/** An error from the library: `code` is the DX_ERR_* value (negative). */
export class DuplexError extends Error {
  code: number;
  constructor(code: number) {
    super(ERROR_MESSAGES[code] ?? `error ${code}`);
    this.name = "DuplexError";
    this.code = code;
  }
}

// ---- payload codec driven by the registry layouts ("u32,string,array<u16>") ----

type Value = number | bigint | boolean | string | Uint8Array | Value[];
const FIXED: Record<string, [number, string]> = {
  u8: [1, "Uint8"], u16: [2, "Uint16"], u32: [4, "Uint32"], u64: [8, "BigUint64"],
  i8: [1, "Int8"], i16: [2, "Int16"], i32: [4, "Int32"], i64: [8, "BigInt64"],
  f32: [4, "Float32"], f64: [8, "Float64"], bool: [1, "Uint8"],
};

function schemaOf(id: number): string[] {
  const entry = PACKETS.get(id);
  if (!entry) throw new DuplexError(-13);
  return entry.schema ? entry.schema.split(",") : [];
}

function writeValue(kind: string, value: Value): Uint8Array {
  const fixed = FIXED[kind];
  if (fixed) {
    const out = new Uint8Array(fixed[0]);
    const dv = new DataView(out.buffer);
    const v = kind === "bool" ? (value ? 1 : 0) : value;
    (dv as any)[`set${fixed[1]}`](0, v, true);
    return out;
  }
  const raw = kind === "string" ? new TextEncoder().encode(value as string) : (value as Uint8Array);
  const out = new Uint8Array((kind === "string" ? 2 : 4) + raw.length);
  const dv = new DataView(out.buffer);
  if (kind === "string") dv.setUint16(0, raw.length, true);
  else dv.setUint32(0, raw.length, true);
  out.set(raw, kind === "string" ? 2 : 4);
  return out;
}

/** Builds the payload of a registry packet from JavaScript values, one per field. */
export function encode(id: number, ...values: Value[]): Uint8Array {
  const schema = schemaOf(id);
  if (values.length !== schema.length) throw new TypeError(`packet 0x${id.toString(16)} has ${schema.length} fields`);
  const parts: Uint8Array[] = [];
  schema.forEach((kind, i) => {
    if (kind.startsWith("array<")) {
      const items = values[i] as Value[];
      parts.push(writeValue("u16", items.length));
      for (const item of items) parts.push(writeValue(kind.slice(6, -1), item));
    } else {
      parts.push(writeValue(kind, values[i]));
    }
  });
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let pos = 0;
  for (const p of parts) { out.set(p, pos); pos += p.length; }
  return out;
}

const strict = new TextDecoder("utf-8", { fatal: true });

/** Reads the fields of a registry packet payload (u64/i64 come back as bigint). */
export function decode(id: number, data: Uint8Array): Value[] {
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let pos = 0;
  const read = (kind: string): Value => {
    const fixed = FIXED[kind];
    if (fixed) {
      const v = (dv as any)[`get${fixed[1]}`](pos, true);
      pos += fixed[0];
      return kind === "bool" ? v !== 0 : v;
    }
    const long = kind === "string" ? dv.getUint16(pos, true) : dv.getUint32(pos, true);
    pos += kind === "string" ? 2 : 4;
    if (pos + long > data.length) throw new RangeError("truncated payload");
    const bytes = data.subarray(pos, pos + long);
    pos += long;
    return kind === "string" ? strict.decode(bytes) : bytes.slice();
  };
  return schemaOf(id).map((kind) => {
    if (!kind.startsWith("array<")) return read(kind);
    const count = dv.getUint16(pos, true);
    pos += 2;
    return Array.from({ length: count }, () => read(kind.slice(6, -1)));
  });
}

