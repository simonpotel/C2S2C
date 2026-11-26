// chat_client.ts - the chat client (see Examples/C/Client) written with the Node binding.
// SPDX-License-Identifier: MIT
//
// Usage: chat_client.ts <port> <nickname> <message>...
// Logs in, sends each message, prints the broadcasts it receives until its own messages came back.
import { Context, encode } from "../../../Bindings/Node/Source/index.ts";
import { packets as reg } from "../../../Bindings/Node/Source/index.ts";

const [port, nickname, ...messages] = process.argv.slice(2);
if (!port || !nickname || messages.length === 0) {
  console.error("usage: chat_client.ts <port> <nickname> <message>...");
  process.exit(2);
}

const ctx = new Context();
let loggedIn = false;
let received = 0;
ctx.onPacket(reg.LOGIN_OK, () => { loggedIn = true; });
ctx.onPacket(reg.CHAT_MESSAGE, (_c, p) => {
  const [id, text] = p.values();
  console.log(`[${id}] ${text}`);
  received++;
});

const conn = ctx.connect("127.0.0.1", Number(port));
const waitUntil = (done: () => boolean): boolean => {
  const t0 = Date.now();
  while (!done() && Date.now() - t0 < 10000) ctx.dispatch(50);
  return done();
};

ctx.send(conn, reg.LOGIN, encode(reg.LOGIN, nickname));
if (!waitUntil(() => loggedIn)) process.exit(1);
for (const text of messages) ctx.send(conn, reg.CHAT_SEND, encode(reg.CHAT_SEND, text));
const ok = waitUntil(() => received >= messages.length);
ctx.close(conn);
for (let i = 0; i < 20; i++) ctx.dispatch(10); // let the close go out
ctx.destroy();
process.exit(ok ? 0 : 1);
