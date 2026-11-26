// index.ts - Node.js / TypeScript API of C2S2C, on top of the native addon.
// SPDX-License-Identifier: MIT
//
// Context owns the engine; poll() returns a Batch (one crossing into the native
// library per batch) whose packets become unreadable after batch.release(); dispatch() runs your handlers and hooks and releases for you. Errors are
// thrown as C2S2CError carrying the library's code.
// Run it with Node 22.6+ (`node --experimental-strip-types`) or Node 24, which run .ts directly.
import { createRequire } from "node:module";
import { C2S2CError, decode, encode } from "./codec.ts";
import type { Value } from "./codec.ts";

export { C2S2CError, decode, encode };
export type { Value };

const require = createRequire(import.meta.url);
const native = require(process.env.C2S2C_ADDON ?? new URL("./c2s2c.node", import.meta.url).pathname);

// Interface version this binding was written for: major 2.
const abi: number = native.abiVersion();
if (abi >> 16 !== 2) throw new Error("incompatible libc2s2c version");

export const SYS_CONNECTED = 0x7f01;
export const SYS_DISCONNECTED = 0x7f02;
const WOULD_BLOCK = -14;
const REJECTED = -22;

function check(rc: number): number {
  if (rc < 0) throw new C2S2CError(rc);
  return rc;
}

// ---- packets, batches, context ----

export class Packet {
  readonly conn: number;
  readonly id: number;
  readonly sequence: number;
  #batch: Batch;
  #offset: number;
  #length: number;

  constructor(batch: Batch, conn: number, id: number, sequence: number, offset: number, length: number) {
    this.#batch = batch;
    this.conn = conn;
    this.id = id;
    this.sequence = sequence;
    this.#offset = offset;
    this.#length = length;
  }

  /** The payload. Throws after the batch was released (copy it with data.slice() to keep it). */
  get data(): Uint8Array {
    if (this.#batch.released) throw new Error("the packet was released: copy data.slice() to keep it");
    return this.#batch.payloads.subarray(this.#offset, this.#offset + this.#length);
  }
  values(): Value[] {
    return decode(this.id, this.data);
  }
  /** For SYS_DISCONNECTED: 0 for a normal close, a negative C2S2C_ERR_* otherwise. */
  get reason(): number {
    const d = this.data;
    return new DataView(d.buffer, d.byteOffset, 4).getInt32(0, true);
  }
}

export class Batch {
  readonly packets: Packet[] = [];
  released = false;
  /** @internal all payloads of the batch, in order */
  payloads: Uint8Array;
  #ctx: any;

  constructor(ctx: any, meta: Uint32Array, payloads: Uint8Array) {
    this.#ctx = ctx;
    this.payloads = payloads;
    for (let i = 0, offset = 0; i < meta.length; i += 4) {
      this.packets.push(new Packet(this, meta[i], meta[i + 1], meta[i + 2], offset, meta[i + 3]));
      offset += meta[i + 3];
    }
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
    native.release(this.#ctx, this.packets.length);
  }
  [Symbol.dispose]() {
    this.release();
  }
}

export interface Hooks {
  onConnect?(conn: number): void;
  onDisconnect?(conn: number, reason: number): void;
  onError?(conn: number, error: number): void;
  /** Return true to reject the packet (send then throws C2S2CError(-22)). */
  beforeSend?(conn: number, id: number, data: Uint8Array): boolean;
  /** Return true to drop the packet before any handler sees it (dispatch only). */
  afterRecv?(packet: Packet): boolean;
}

export type Handler = (ctx: Context, packet: Packet) => void;

export class Context {
  #ctx: any;
  #handlers = new Map<number, Handler>();
  #hooks: Hooks = {};

  constructor(settings: { maxConns?: number; maxPayload?: number; rxQueue?: number; rxRingBytes?: number; idleTimeoutMs?: number } = {}) {
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

  /** Sends a payload. Throws C2S2CError(-14) if the send buffer is full (see trySend). */
  send(conn: number, id: number, data: Uint8Array = new Uint8Array()) {
    const rc = this.#rawSend(conn, id, data);
    if (rc < 0) throw new C2S2CError(rc);
  }
  /** Like send, but returns false instead of throwing when the send buffer is full. */
  trySend(conn: number, id: number, data: Uint8Array = new Uint8Array()): boolean {
    const rc = this.#rawSend(conn, id, data);
    if (rc === WOULD_BLOCK) return false;
    if (rc < 0) throw new C2S2CError(rc);
    return true;
  }
  /** Sends [[conn, id, data], ...] with ONE call into the library; returns how many were queued. */
  sendMany(packets: [number, number, Uint8Array][]): number {
    const allowed = this.#hooks.beforeSend ? packets.filter(([c, id, d]) => !this.#hooks.beforeSend!(c, id, d)) : packets;
    const meta = new Uint32Array(allowed.length * 3);
    allowed.forEach(([conn, id, data], i) => {
      meta[3 * i] = conn;
      meta[3 * i + 1] = id;
      meta[3 * i + 2] = data.length;
    });
    return check(native.sendBatch(this.#ctx, meta, Buffer.concat(allowed.map((p) => p[2]))));
  }
  /** Writes queued packets now (only needed when no poll/dispatch follows). */
  flush() {
    check(native.flush(this.#ctx));
  }
  #rawSend(conn: number, id: number, data: Uint8Array): number {
    if (this.#hooks.beforeSend?.(conn, id, data)) return REJECTED;
    return native.send(this.#ctx, conn, id, data);
  }

  /** Waits up to timeoutMs and returns a batch; call batch.release() when done. */
  poll(timeoutMs = 0, max = 64): Batch {
    const raw = native.poll(this.#ctx, timeoutMs, max);
    if (typeof raw === "number") throw new C2S2CError(raw);
    return new Batch(this.#ctx, raw[0], raw[1]);
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
