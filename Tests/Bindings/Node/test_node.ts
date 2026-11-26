// test_node.ts - tests of the Node binding: errors, codec, release (detached buffers), handlers, hooks.
// SPDX-License-Identifier: MIT
// Run: node --experimental-strip-types --test test_node.ts   (C2S2C_ADDON points at c2s2c.node)
import assert from "node:assert/strict";
import { test } from "node:test";
import { Context, C2S2CError, decode, encode } from "../../../Bindings/Node/Source/index.ts";
import { ERROR_MESSAGES } from "../../../Bindings/Node/Source/codec.ts";
import { createRequire } from "node:module";
import * as reg from "../../../Generated/TypeScript/registry.ts";

/** Runs both contexts until done() or 10 s (no fixed sleeps). */
function spin(a: Context, b: Context, done: () => boolean): boolean {
  const t0 = Date.now();
  while (!done() && Date.now() - t0 < 10000) {
    a.dispatch(1);
    b.dispatch(1);
  }
  return done();
}

function pair(settings = {}) {
  const server = new Context(settings);
  const client = new Context(settings);
  server.listen("127.0.0.1", 0);
  return { server, client, conn: client.connect("127.0.0.1", server.port) };
}

test("errors carry the library code and message", () => {
  const ctx = new Context();
  assert.throws(() => ctx.send(12345, reg.PING, encode(reg.PING, 1)), (e: any) => {
    return e instanceof C2S2CError && e.code === -20 && e.message === "invalid connection";
  });
  assert.throws(() => new Context({ rxRingBytes: 100 }), /c2s2c_create failed/); // ring smaller than two packets
  ctx.destroy();
});

test("codec round trips and matches the wire format", () => {
  const data = encode(reg.CHAT_MESSAGE, 7, "héllo €", 2n ** 40n);
  assert.deepEqual(decode(reg.CHAT_MESSAGE, data), [7, "héllo €", 2n ** 40n]);
  assert.deepEqual(decode(reg.USER_LIST, encode(reg.USER_LIST, ["ann", "bob"])), [["ann", "bob"]]);
  assert.deepEqual([...encode(reg.PING, 0x01020304)], [4, 3, 2, 1]); // little-endian
  assert.deepEqual([...encode(reg.LOGIN, "hi")], [2, 0, 0x68, 0x69]);
  assert.throws(() => encode(reg.PING), TypeError); // wrong field count
  assert.throws(() => encode(0x1234), C2S2CError); // unknown packet
  assert.throws(() => decode(reg.LOGIN, new Uint8Array([2, 0, 0xc0, 0x80]))); // invalid UTF-8
});

test("the library validates what JavaScript sends", () => {
  const { client, conn } = pair();
  assert.throws(() => client.send(conn, reg.LOGIN, new Uint8Array([2, 0, 0xc0, 0x80])), (e: any) => e.code === -8);
});

test("a packet is unreadable once its batch is released", () => {
  const { server, client, conn } = pair();
  client.send(conn, reg.PING, encode(reg.PING, 42));
  let kept: any = null;
  const t0 = Date.now();
  while (!kept && Date.now() - t0 < 10000) {
    client.dispatch(0);
    const batch = server.poll(1);
    for (const p of batch) if (p.id === reg.PING) {
      assert.deepEqual(p.values(), [42]);
      kept = { packet: p, view: p.data };
    }
    batch.release();
  }
  assert.ok(kept);
  assert.throws(() => kept.packet.data, /released/); // no way to read the released packet
});

test("a small queue keeps flowing because batches are released", () => {
  const { server, client, conn } = pair({ rxQueue: 8 });
  let sent = 0, got = 0;
  const t0 = Date.now();
  while (got < 300 && Date.now() - t0 < 15000) {
    if (sent < 300 && client.trySend(conn, reg.PING, encode(reg.PING, sent))) sent++;
    client.dispatch(0);
    const batch = server.poll(1);
    got += [...batch].filter((p) => p.id === reg.PING).length;
    batch.release();
  }
  assert.equal(got, 300);
});

test("handlers, hooks and flow control", () => {
  const { server, client, conn } = pair();
  const seen = { pings: 0, pongs: 0, connects: 0, dropped: 0, rejected: 0, chat: 0 };
  server.onPacket(reg.PING, (ctx, p) => { seen.pings++; ctx.send(p.conn, reg.PONG, p.data.slice()); });
  client.onPacket(reg.PONG, () => { seen.pongs++; });
  server.onPacket(reg.CHAT_SEND, () => { seen.chat++; });
  server.setHooks({
    onConnect: () => { seen.connects++; },
    afterRecv: (p) => {
      const spam = p.id === reg.CHAT_SEND && (p.values()[0] as string).startsWith("spam");
      if (spam) seen.dropped++;
      return spam;
    },
  });
  client.setHooks({ beforeSend: (_c, id) => { if (id === reg.LOGIN) seen.rejected++; return id === reg.LOGIN; } });

  assert.ok(spin(server, client, () => seen.connects === 1));
  client.send(conn, reg.PING, encode(reg.PING, 1));
  client.send(conn, reg.CHAT_SEND, encode(reg.CHAT_SEND, "spam spam"));
  client.send(conn, reg.CHAT_SEND, encode(reg.CHAT_SEND, "hello"));
  assert.throws(() => client.send(conn, reg.LOGIN, encode(reg.LOGIN, "me")), (e: any) => e.code === -22);
  assert.ok(spin(server, client, () => seen.pongs === 1 && seen.chat === 1));
  assert.deepEqual(seen, { pings: 1, pongs: 1, connects: 1, dropped: 1, rejected: 1, chat: 1 });

  const payload = encode(reg.CHAT_SEND, "x".repeat(1000));
  let full = false;
  for (let i = 0; i < 200000 && !full; i++) full = !client.trySend(conn, reg.CHAT_SEND, payload);
  assert.ok(full); // a full send buffer is `false` from trySend (nobody serves the other side)
});

test("disconnect reason reaches the hook", () => {
  const { server, client, conn } = pair();
  const reasons: number[] = [];
  let connected = 0;
  server.setHooks({ onConnect: () => { connected++; }, onDisconnect: (_c, r) => { reasons.push(r); } });
  assert.ok(spin(server, client, () => connected === 1));
  client.close(conn);
  assert.ok(spin(server, client, () => reasons.length === 1));
  assert.deepEqual(reasons, [0]);
});

test("the error messages of the pure codec match the C library", () => {
  // codec.ts repeats the messages so browsers can use it; this test catches any drift.
  const native = createRequire(import.meta.url)(process.env.C2S2C_ADDON as string);
  for (const code of Object.keys(ERROR_MESSAGES).map(Number)) {
    assert.equal(ERROR_MESSAGES[code], native.strerror(code), `code ${code}`);
  }
});
