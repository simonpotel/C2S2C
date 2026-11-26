// Type declarations of the C2S2C Node binding (see Source/index.ts for the implementation).
// SPDX-License-Identifier: MIT

export const SYS_CONNECTED: number;
export const SYS_DISCONNECTED: number;

/** An error from the library: `code` is the C2S2C_ERR_* value (negative). */
export class C2S2CError extends Error {
  code: number;
  constructor(code: number);
}

export type Value = number | bigint | boolean | string | Uint8Array | Value[];

/** Builds the payload of a registry packet from JavaScript values, one per field. */
export function encode(id: number, ...values: Value[]): Uint8Array;
/** Reads the fields of a registry packet payload (u64/i64 come back as bigint). */
export function decode(id: number, data: Uint8Array): Value[];

export class Packet {
  readonly conn: number;
  readonly id: number;
  readonly sequence: number;
  /** The payload. Throws after the batch was released (copy with data.slice() to keep it). */
  readonly data: Uint8Array;
  values(): Value[];
  /** For SYS_DISCONNECTED: 0 for a normal close, a negative C2S2C_ERR_* otherwise. */
  readonly reason: number;
}

export class Batch implements Iterable<Packet> {
  readonly packets: Packet[];
  readonly length: number;
  released: boolean;
  [Symbol.iterator](): Iterator<Packet>;
  /** Gives the memory back to the library; every packet of this batch becomes unreadable. */
  release(): void;
  [Symbol.dispose](): void;
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
  constructor(settings?: { maxConns?: number; maxPayload?: number; rxQueue?: number; rxRingBytes?: number; idleTimeoutMs?: number });
  destroy(): void;
  [Symbol.dispose](): void;
  listen(address?: string, port?: number): void;
  readonly port: number;
  connect(address: string, port: number): number;
  close(conn: number): void;
  send(conn: number, id: number, data?: Uint8Array): void;
  trySend(conn: number, id: number, data?: Uint8Array): boolean;
  /** Sends [[conn, id, data], ...] with one call into the library; returns how many were queued. */
  sendMany(packets: [number, number, Uint8Array][]): number;
  /** Writes queued packets now (only needed when no poll/dispatch follows). */
  flush(): void;
  poll(timeoutMs?: number, max?: number): Batch;
  onPacket(id: number, fn?: Handler): void;
  setHooks(hooks: Hooks): void;
  dispatch(timeoutMs?: number): number;
}
