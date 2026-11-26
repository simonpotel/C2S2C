// bench_ffi.ts - cost of the Node binding: pipelined ping/pong round trips per second.
// SPDX-License-Identifier: MIT
import { Context, encode } from "../../Bindings/Node/Source/index.ts";
import { packets as reg } from "../../Bindings/Node/Source/index.ts";

const server = new Context();
const client = new Context();
server.listen("127.0.0.1", 0);
const conn = client.connect("127.0.0.1", server.port);
server.onPacket(reg.PING, (c, p) => c.send(p.conn, reg.PONG, p.data.slice()));
let pongs = 0;
client.onPacket(reg.PONG, () => { pongs++; });
const payload = encode(reg.PING, 1);
let inflight = 0;
const t0 = performance.now();
while (performance.now() - t0 < 1000) {
  while (inflight < 32 && client.trySend(conn, reg.PING, payload)) inflight++;
  server.dispatch(0);
  const before = pongs;
  client.dispatch(0);
  inflight -= pongs - before;
}
console.log(`node_roundtrips ${Math.round((pongs / (performance.now() - t0)) * 1000)} packets/s`);
