// duplex_web.ts - Duplex in the browser: packets over a WebSocket, checked by the WebAssembly codec.
// SPDX-License-Identifier: MIT
//
// A browser cannot open a raw TCP connection, so it talks to a gateway over a
// WebSocket. Each WebSocket message carries exactly one Duplex frame (header + payload). This
// client builds outgoing frames and checks incoming ones with the same C code as the server.
import { decode, encode } from "../../Node/Source/codec.ts";
import type { Value } from "../../Node/Source/codec.ts";
import type { Codec } from "./duplex_wasm.ts";

export interface WebPacket {
  id: number;
  sequence: number;
  data: Uint8Array;
  /** The decoded fields of a registry packet. */
  values(): Value[];
}

export type WebHandler = (packet: WebPacket) => void;

export class WebClient {
  #socket: WebSocket;
  #codec: Codec;
  #sequence = 0;
  #handlers = new Map<number, WebHandler>();
  #onClose: (reason: string) => void = () => {};

  private constructor(socket: WebSocket, codec: Codec) {
    this.#socket = socket;
    this.#codec = codec;
    socket.binaryType = "arraybuffer";
    socket.onmessage = (event) => this.#receive(new Uint8Array(event.data as ArrayBuffer));
    socket.onclose = () => this.#onClose("closed");
  }

  /** Opens the WebSocket; `WebSocketImpl` defaults to the browser's (Node 22+ has one too). */
  static connect(url: string, codec: Codec, WebSocketImpl: typeof WebSocket = globalThis.WebSocket): Promise<WebClient> {
    return new Promise((resolve, reject) => {
      const socket = new WebSocketImpl(url);
      socket.onopen = () => resolve(new WebClient(socket, codec));
      socket.onerror = () => reject(new Error(`cannot connect to ${url}`));
    });
  }

  /** Calls `fn` for packets with this id (0 = any other packet). */
  onPacket(id: number, fn: WebHandler) {
    this.#handlers.set(id, fn);
  }

  onClose(fn: (reason: string) => void) {
    this.#onClose = fn;
  }

  /** Sends a registry packet from JavaScript values (one per field). */
  send(id: number, ...values: Value[]) {
    this.sendRaw(id, encode(id, ...values));
  }

  /** Sends an already encoded payload; the frame is validated before it leaves. */
  sendRaw(id: number, payload: Uint8Array) {
    this.#socket.send(this.#codec.encodeFrame(id, payload, this.#sequence++));
  }

  close() {
    this.#socket.close();
  }

  #expected = 0;

  #receive(bytes: Uint8Array) {
    let frame;
    try {
      frame = this.#codec.decodeFrame(bytes);
    } catch (e) {
      this.#socket.close(); // a frame the codec rejects ends the session, like on the server
      return;
    }
    if (!(frame.flags & 1)) {
      this.#socket.close(); // a client only accepts server-to-client packets
      return;
    }
    if (frame.sequence !== this.#expected++) {
      this.#socket.close(); // lost or reordered frame
      return;
    }
    const packet: WebPacket = {
      id: frame.id,
      sequence: frame.sequence,
      data: frame.payload,
      values: () => decode(frame.id, frame.payload),
    };
    (this.#handlers.get(frame.id) ?? this.#handlers.get(0))?.(packet);
  }
}
