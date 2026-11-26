// gateway.ts - serves the web chat page and bridges WebSockets to a Duplex TCP server.
// SPDX-License-Identifier: MIT
//
// Usage: gateway.ts <duplex server port> [http port]     (the http port is printed first)
// Browsers cannot speak raw TCP, so this small relay accepts a WebSocket from the
// page, opens a TCP connection to the chat server, and passes whole Duplex frames both ways (one
// frame per WebSocket message). It does not look inside the frames: the server validates them.
// It also serves the page and its TypeScript modules, turning the TypeScript into JavaScript on
// the fly (a browser cannot run .ts), but only from a short list of folders.
import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import { stripTypeScriptTypes } from "node:module";
import net from "node:net";
import path from "node:path";
import type { Duplex } from "node:stream";

const root = path.resolve(import.meta.dirname, "../../..");
const upstreamPort = Number(process.argv[2]);
const wasmFile = process.env.DUPLEX_WASM ?? path.join(root, "Build/Wasm/duplex_codec.wasm");
const PUBLIC_FOLDERS = ["Examples/Web/Chat/", "Bindings/", "Generated/TypeScript/"];
const TYPES: Record<string, string> = { ".html": "text/html", ".ts": "text/javascript", ".js": "text/javascript", ".wasm": "application/wasm" };

const server = http.createServer((req, res) => {
  const url = decodeURIComponent((req.url ?? "/").split("?")[0]);
  let file: string | undefined;
  if (url === "/" || url === "/index.html") file = path.join(root, "Examples/Web/Chat/index.html");
  else if (url === "/duplex_codec.wasm") file = wasmFile;
  else if (!url.includes("..") && PUBLIC_FOLDERS.some((f) => url.startsWith("/" + f))) file = path.join(root, url);
  if (!file || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404).end("not found");
    return;
  }
  const ext = path.extname(file);
  let body: Buffer | string = fs.readFileSync(file);
  if (ext === ".ts") body = stripTypeScriptTypes(body.toString("utf8")); // browsers get plain JavaScript
  res.writeHead(200, { "content-type": TYPES[ext] ?? "application/octet-stream" }).end(body);
});

// ---- WebSocket (RFC 6455), just what a binary relay needs ----
const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function sendFrame(socket: Duplex, payload: Buffer) {
  const n = payload.length;
  const head = n < 126 ? Buffer.from([0x82, n]) : n < 65536 ? Buffer.from([0x82, 126, n >> 8, n & 255]) : Buffer.concat([Buffer.from([0x82, 127]), Buffer.alloc(8)]);
  if (n >= 65536) head.writeBigUInt64BE(BigInt(n), 2);
  socket.write(Buffer.concat([head, payload]));
}

/** Reads one WebSocket frame from the start of `buf`, or returns null if it is incomplete. */
function readFrame(buf: Buffer) {
  if (buf.length < 2) return null;
  const opcode = buf[0] & 0x0f;
  const masked = (buf[1] & 0x80) !== 0;
  let length = buf[1] & 0x7f;
  let pos = 2;
  if (length === 126) { if (buf.length < 4) return null; length = buf.readUInt16BE(2); pos = 4; }
  else if (length === 127) { if (buf.length < 10) return null; length = Number(buf.readBigUInt64BE(2)); pos = 10; }
  const maskKey = masked ? buf.subarray(pos, pos + 4) : null;
  if (masked) pos += 4;
  if (buf.length < pos + length) return null;
  const payload = Buffer.from(buf.subarray(pos, pos + length));
  if (maskKey) for (let i = 0; i < payload.length; i++) payload[i] ^= maskKey[i % 4];
  return { opcode, payload, size: pos + length };
}

server.on("upgrade", (req, socket) => {
  const key = req.headers["sec-websocket-key"];
  if (req.url !== "/ws" || typeof key !== "string") return void socket.destroy();
  const accept = crypto.createHash("sha1").update(key + GUID).digest("base64");
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);

  const upstream = net.connect(upstreamPort, "127.0.0.1");
  upstream.setNoDelay(true);
  const closeBoth = () => { socket.destroy(); upstream.destroy(); };
  socket.on("error", closeBoth);
  upstream.on("error", closeBoth);
  socket.on("close", closeBoth);
  upstream.on("close", closeBoth);

  // WebSocket messages -> TCP (each binary message is one Duplex frame)
  let fromBrowser = Buffer.alloc(0);
  socket.on("data", (chunk) => {
    fromBrowser = Buffer.concat([fromBrowser, chunk]);
    for (let f = readFrame(fromBrowser); f; f = readFrame(fromBrowser)) {
      fromBrowser = fromBrowser.subarray(f.size);
      if (f.opcode === 2) upstream.write(f.payload);
      else if (f.opcode === 8) return closeBoth();
      else if (f.opcode === 9) socket.write(Buffer.concat([Buffer.from([0x8a, f.payload.length]), f.payload])); // pong
    }
  });

  // TCP -> WebSocket: cut the byte stream into whole frames using the length in each header
  let fromServer = Buffer.alloc(0);
  upstream.on("data", (chunk) => {
    fromServer = Buffer.concat([fromServer, chunk]);
    while (fromServer.length >= 14) {
      const size = 14 + fromServer.readUInt32LE(6);
      if (fromServer.length < size) break;
      sendFrame(socket, fromServer.subarray(0, size));
      fromServer = fromServer.subarray(size);
    }
  });
});

server.listen(Number(process.argv[3] ?? 0), "127.0.0.1", () => {
  console.log((server.address() as net.AddressInfo).port);
});
