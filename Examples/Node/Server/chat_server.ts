// chat_server.ts - the chat room server (see Examples/C/Server) written with the Node binding.
// SPDX-License-Identifier: MIT
//
// Same protocol and command line as the C version. Usage: chat_server.ts [port]
// The chosen port is printed on the first line.
import { Context, encode } from "../../../Bindings/Node/Source/index.ts";
import { packets as reg } from "../../../Bindings/Node/Source/index.ts";

const ctx = new Context();
ctx.listen("127.0.0.1", Number(process.argv[2] ?? 0));
const users = new Map<number, string>(); // connection handle -> nickname

ctx.onPacket(reg.LOGIN, (c, p) => {
  users.set(p.conn, p.values()[0] as string);
  c.send(p.conn, reg.LOGIN_OK, encode(reg.LOGIN_OK, p.conn)); // the handle is the user id
});
ctx.onPacket(reg.CHAT_SEND, (c, p) => {
  if (!users.has(p.conn)) return; // must log in first
  const message = encode(reg.CHAT_MESSAGE, p.conn, p.values()[0] as string, BigInt(Math.floor(Date.now() / 1000)));
  for (const conn of users.keys()) c.send(conn, reg.CHAT_MESSAGE, message); // broadcast
});
ctx.onPacket(reg.PING, (c, p) => c.send(p.conn, reg.PONG, p.data.slice())); // same nonce back
ctx.setHooks({ onDisconnect: (conn) => { users.delete(conn); } });

// No signal handler on purpose: this loop never yields to Node's event loop, and the default
// action of SIGTERM/SIGINT (exit) is exactly what we want.
console.log(ctx.port);
for (;;) ctx.dispatch(200);
