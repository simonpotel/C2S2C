// Benchmark service written from scratch in Node (no Duplex): what you write when you recode the
// protocol yourself. It checks the same things the Duplex library checks for this packet and drops
// idle connections after one second. SPDX-License-Identifier: MIT
import net from "node:net";

const MAGIC = 0xc0de, PING = 3, PONG = 0x8003, MAX_PAYLOAD = 1024;

const server = net.createServer((socket) => {
  socket.setNoDelay(true);
  socket.setTimeout(1000, () => socket.destroy()); // idle connections are dropped
  let buf = Buffer.alloc(0);
  let rx = 0, tx = 0;
  socket.on("error", () => socket.destroy());
  socket.on("data", (chunk) => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
    const replies: Buffer[] = [];
    while (buf.length >= 14) {
      const flags = buf[3], id = buf.readUInt16LE(4), length = buf.readUInt32LE(6), seq = buf.readUInt32LE(10);
      if (buf.readUInt16LE(0) !== MAGIC || buf[2] !== 1 || (flags & 0xfc) || (flags & 1) || (id & 0x8000)) return void socket.destroy();
      if (length > MAX_PAYLOAD || seq !== rx) return void socket.destroy();
      if (buf.length < 14 + length) break;
      if (id !== PING || length !== 4) return void socket.destroy();
      const out = Buffer.alloc(18);
      out.writeUInt16LE(MAGIC, 0); out[2] = 1; out[3] = 1; out.writeUInt16LE(PONG, 4);
      out.writeUInt32LE(4, 6); out.writeUInt32LE(tx++, 10); buf.copy(out, 14, 14, 18);
      replies.push(out);
      rx++;
      buf = buf.subarray(18);
    }
    if (replies.length) socket.write(replies.length === 1 ? replies[0] : Buffer.concat(replies));
  });
});
server.listen(0, "127.0.0.1", () => console.log((server.address() as net.AddressInfo).port));
