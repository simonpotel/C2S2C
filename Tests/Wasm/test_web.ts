// test_web.ts - the web chat end to end, without a browser window: the C chat server, the
// gateway, and the browser-side client code (WebAssembly codec + WebSocket) run for real.
// SPDX-License-Identifier: MIT
// "Headless" here means Node's built-in WebSocket stands in for the browser's: the page's own
// modules are loaded exactly as written. Env: DUPLEX_WASM, DX_CHAT_SERVER (the C server binary).
import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import { test } from "node:test";
import { WebClient } from "../../Bindings/Wasm/Source/duplex_web.ts";
import { loadCodec } from "../../Bindings/Wasm/Source/duplex_wasm.ts";
import * as reg from "../../Generated/TypeScript/registry.ts";

/** Starts a program and resolves with the first line it prints (its port). */
function start(cmd: string, args: string[], children: ChildProcess[]): Promise<number> {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { stdio: ["ignore", "pipe", "inherit"] });
    children.push(child);
    child.on("error", reject);
    child.stdout.once("data", (d) => resolve(Number(String(d).split("\n")[0])));
  });
}

test("web client chats through the gateway, and the page is served", async (t) => {
  const children: ChildProcess[] = [];
  t.after(() => children.forEach((c) => c.kill()));
  const serverPort = await start(process.env.DX_CHAT_SERVER as string, [], children);
  const gatewayPort = await start(process.execPath, ["--experimental-strip-types", "--no-warnings",
    new URL("../../Examples/Web/Gateway/gateway.ts", import.meta.url).pathname, String(serverPort)], children);
  const base = `http://127.0.0.1:${gatewayPort}`;

  // The gateway serves the page and turns its TypeScript into JavaScript (no type syntax left).
  const page = await fetch(`${base}/`);
  assert.equal(page.status, 200);
  assert.match(await page.text(), /Duplex chat/);
  // Follow what a browser would: the page's script, then every module it imports, recursively.
  // Each must exist, be served as JavaScript, and contain no TypeScript syntax.
  const html = await (await fetch(`${base}/`)).text();
  const entry = html.match(/<script type="module" src="([^"]+)"/)?.[1];
  assert.ok(entry, "the page has a module script");
  const queue = [new URL(entry, `${base}/`).href];
  const seen = new Set<string>();
  while (queue.length) {
    const url = queue.pop() as string;
    if (seen.has(url)) continue;
    seen.add(url);
    const res = await fetch(url);
    assert.equal(res.status, 200, url);
    assert.match(res.headers.get("content-type") ?? "", /javascript/, url);
    const code = await res.text();
    assert.doesNotMatch(code, /\bas HTMLElement\b|\(\w+: (string|number)\b|\binterface \w+ \{/, `TypeScript syntax left in ${url}`);
    for (const m of code.matchAll(/(?:from|import\()\s*"(\.[^"]+)"/g)) queue.push(new URL(m[1], url).href);
  }
  assert.ok(seen.size >= 5, `followed ${seen.size} modules`);
  assert.equal((await fetch(`${base}/etc/passwd`)).status, 404);
  // Paths that try to leave the allowed folders. fetch() would tidy "/../" away, so use a raw request.
  for (const evil of ["/Bindings/../AGENTS.md", "/Bindings/%2e%2e/AGENTS.md", "/Source/Core/header.c"]) {
    const status = await new Promise<number>((resolve) =>
      http.get({ host: "127.0.0.1", port: gatewayPort, path: evil }, (res) => { res.resume(); resolve(res.statusCode ?? 0); }));
    assert.equal(status, 404, evil);
  }
  const wasm = await fetch(`${base}/duplex_codec.wasm`);
  assert.equal(wasm.headers.get("content-type"), "application/wasm");

  // A chat session driven by the same client class the page uses.
  const codec = await loadCodec(fs.readFileSync(process.env.DUPLEX_WASM as string));
  const client = await WebClient.connect(`ws://127.0.0.1:${gatewayPort}/ws`, codec);
  const received: string[] = [];
  let loggedIn = false;
  client.onPacket(reg.LOGIN_OK, () => { loggedIn = true; });
  client.onPacket(reg.CHAT_MESSAGE, (p) => received.push(p.values()[1] as string));
  const until = async (done: () => boolean) => {
    for (let i = 0; i < 500 && !done(); i++) await new Promise((r) => setTimeout(r, 10));
    assert.ok(done());
  };

  client.send(reg.LOGIN, "web user");
  await until(() => loggedIn);
  client.send(reg.CHAT_SEND, "hello from the web");
  client.send(reg.CHAT_SEND, "héllo €");
  await until(() => received.length === 2);
  assert.deepEqual(received, ["hello from the web", "héllo €"]);
  client.close();
});
