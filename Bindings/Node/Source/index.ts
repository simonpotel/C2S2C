// index.ts - Node.js / TypeScript API of Duplex, on top of the native addon.
// SPDX-License-Identifier: MIT
//
// In plain words: Context owns the engine; poll() returns a Batch whose packets point straight
// into the library's memory and become unreadable (their buffers are detached) when you call
// batch.release(); dispatch() runs your handlers and hooks and releases for you. Errors are
// thrown as DuplexError carrying the library's code.
// Run it with Node 22.6+ (`node --experimental-strip-types`) or Node 24, which run .ts directly.
import { createRequire } from "node:module";
import { PACKETS } from "../../../Generated/TypeScript/registry.ts";

const require = createRequire(import.meta.url);
const native = require(process.env.DUPLEX_ADDON ?? new URL("./duplex.node", import.meta.url).pathname);

// Interface version this binding was written for: major 1, minor 4.
const abi: number = native.abiVersion();
if (abi >> 16 !== 1 || ((abi >> 8) & 0xff) < 4) throw new Error("incompatible libduplex version");

export const SYS_CONNECTED = 0x7f01;
export const SYS_DISCONNECTED = 0x7f02;
const WOULD_BLOCK = -14;
const REJECTED = -22;

/** An error from the library: `code` is the DX_ERR_* value (negative). */
export class DuplexError extends Error {
  code: number;
  constructor(code: number) {
    super(native.strerror(code));
    this.name = "DuplexError";
    this.code = code;
  }
}

function check(rc: number): number {
  if (rc < 0) throw new DuplexError(rc);
  return rc;
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

// ---- packets, batches, context ----

export class Packet {
  readonly conn: number;
  readonly id: number;
  readonly sequence: number;
  #raw: { data: Uint8Array };
  #batch: { released: boolean };

  constructor(raw: { conn: number; id: number; sequence: number; data: Uint8Array }, batch: { released: boolean }) {
    this.conn = raw.conn;
    this.id = raw.id;
    this.sequence = raw.sequence;
    this.#raw = raw;
    this.#batch = batch;
  }

  /** Zero-copy view of the payload. Throws after the batch was released. */
  get data(): Uint8Array {
    if (this.#batch.released) throw new Error("the packet was released: copy data.slice() to keep it");
    return this.#raw.data;
  }
  values(): Value[] {
    return decode(this.id, this.data);
  }
  /** For SYS_DISCONNECTED: 0 for a normal close, a negative DX_ERR_* otherwise. */
  get reason(): number {
    return new DataView(this.data.buffer, this.data.byteOffset, 4).getInt32(0, true);
  }
}

export class Batch {
  readonly packets: Packet[];
  released = false;
  #ctx: any;
  #raw: unknown[];

  constructor(ctx: any, raw: any[]) {
    this.#ctx = ctx;
    this.#raw = raw;
    this.packets = raw.map((r) => new Packet(r, this));
  }
  [Symbol.iterator]() {
    return this.packets[Symbol.iterator]();
  }
  get length() {
    return this.packets.length;
  }
  /** Gives the memory back to the library; every packet of this batch becomes unreadable. */
  release() {
    if (this.released) return;
    this.released = true;
    native.release(this.#ctx, this.#raw);
  }
  [Symbol.dispose]() {
    this.release();
  }
}

export interface Hooks {
  onConnect?(conn: number): void;
  onDisconnect?(conn: number, reason: number): void;
  onError?(conn: number, error: number): void;
  /** Return true to reject the packet (send then throws DuplexError(-22)). */
  beforeSend?(conn: number, id: number, data: Uint8Array): boolean;
  /** Return true to drop the packet before any handler sees it (dispatch only). */
  afterRecv?(packet: Packet): boolean;
}

export type Handler = (ctx: Context, packet: Packet) => void;

export class Context {
  #ctx: any;
  #handlers = new Map<number, Handler>();
  #hooks: Hooks = {};

  constructor(settings: { maxConns?: number; maxPayload?: number; rxQueue?: number; rxRingBytes?: number } = {}) {
    this.#ctx = native.create(settings);
  }

  destroy() {
    if (this.#ctx) native.destroy(this.#ctx);
    this.#ctx = null;
  }
  [Symbol.dispose]() {
    this.destroy();
  }

  listen(address = "127.0.0.1", port = 0) {
    check(native.listen(this.#ctx, address, port));
  }
  get port(): number {
    return check(native.port(this.#ctx));
  }
  connect(address: string, port: number): number {
    return check(native.connect(this.#ctx, address, port));
  }
  close(conn: number) {
    check(native.close(this.#ctx, conn));
  }

  /** Sends a payload. Throws DuplexError(-14) if the send buffer is full (see trySend). */
  send(conn: number, id: number, data: Uint8Array = new Uint8Array()) {
    const rc = this.#rawSend(conn, id, data);
    if (rc < 0) throw new DuplexError(rc);
  }
  /** Like send, but returns false instead of throwing when the send buffer is full. */
  trySend(conn: number, id: number, data: Uint8Array = new Uint8Array()): boolean {
    const rc = this.#rawSend(conn, id, data);
    if (rc === WOULD_BLOCK) return false;
    if (rc < 0) throw new DuplexError(rc);
    return true;
  }
  #rawSend(conn: number, id: number, data: Uint8Array): number {
    if (this.#hooks.beforeSend?.(conn, id, data)) return REJECTED;
    return native.send(this.#ctx, conn, id, data);
  }

  /** Waits up to timeoutMs and returns a batch; call batch.release() when done. */
  poll(timeoutMs = 0, max = 64): Batch {
    const raw = native.poll(this.#ctx, timeoutMs, max);
    if (typeof raw === "number") throw new DuplexError(raw);
    return new Batch(this.#ctx, raw);
  }

  /** Calls fn for packets with this id (0 = default handler); undefined removes it. */
  onPacket(id: number, fn?: Handler) {
    if (fn) this.#handlers.set(id, fn);
    else this.#handlers.delete(id);
  }
  setHooks(hooks: Hooks) {
    this.#hooks = hooks;
  }

  /** Like poll, but runs hooks and handlers, then releases. Returns the entries handled. */
  dispatch(timeoutMs = 0): number {
    const batch = this.poll(timeoutMs);
    try {
      for (const p of batch) {
        if (p.id === SYS_CONNECTED) {
          this.#hooks.onConnect?.(p.conn);
        } else if (p.id === SYS_DISCONNECTED) {
          if (p.reason < 0) this.#hooks.onError?.(p.conn, p.reason);
          this.#hooks.onDisconnect?.(p.conn, p.reason);
        } else if (!this.#hooks.afterRecv?.(p)) {
          (this.#handlers.get(p.id) ?? this.#handlers.get(0))?.(this, p);
        }
      }
      return batch.length;
    } finally {
      batch.release();
    }
  }
}
