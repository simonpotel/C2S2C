// Benchmark service on the Node binding. Usage: server_c2s2c.ts poll|dispatch
// poll: replies to a whole batch with one sendMany call; dispatch: JS handlers over the same batches.
// SPDX-License-Identifier: MIT
import { Context } from "../../../Bindings/Node/Source/index.ts";
import { packets as reg } from "../../../Bindings/Node/Source/index.ts";

const ctx = new Context({ maxConns: 4096, maxPayload: 1024, idleTimeoutMs: 1000 });
ctx.listen("127.0.0.1", 0);
console.log(ctx.port);
if (process.argv[2] === "dispatch") {
  ctx.onPacket(reg.PING, (c, p) => {
    try {
      c.send(p.conn, reg.PONG, p.data);
    } catch {
      // the client already left
    }
  });
  for (;;) ctx.dispatch(100);
}
for (;;) {
  const batch = ctx.poll(100, 256);
  const replies: [number, number, Uint8Array][] = [];
  for (const p of batch) if (p.id === reg.PING) replies.push([p.conn, reg.PONG, p.data]);
  if (replies.length) {
    try {
      ctx.sendMany(replies);
    } catch {
      // the first client already left
    }
  }
  batch.release();
}
