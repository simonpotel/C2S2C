// chat.ts - the web chat page: Duplex in the browser (WebAssembly codec + WebSocket).
// SPDX-License-Identifier: MIT
//
// Same protocol as the C, C++, Python, Rust and Node chat clients: login, chat_send, and the
// server's login_ok / chat_message. The page is served by Examples/Web/Gateway/gateway.ts.
import { WebClient } from "../../../Bindings/Wasm/Source/duplex_web.ts";
import { loadCodec } from "../../../Bindings/Wasm/Source/duplex_wasm.ts";
import * as reg from "../../../Generated/TypeScript/registry.ts";

const $ = (id: string) => document.getElementById(id) as HTMLElement & HTMLInputElement;
const log = (text: string) => {
  const line = document.createElement("p");
  line.textContent = text; // textContent, never innerHTML: messages come from other users
  $("log").append(line);
  $("log").scrollTop = $("log").scrollHeight;
};

const codec = await loadCodec(await (await fetch("/duplex_codec.wasm")).arrayBuffer());
const client = await WebClient.connect(`ws://${location.host}/ws`, codec);
$("status").textContent = "connected";

client.onPacket(reg.LOGIN_OK, () => {
  $("login").hidden = true;
  $("send").hidden = false;
  $("text").focus();
  log("— you joined the room —");
});
client.onPacket(reg.CHAT_MESSAGE, (p) => {
  const [userId, text] = p.values();
  log(`[${userId}] ${text}`);
});
client.onClose(() => { $("status").textContent = "disconnected"; });

$("login").addEventListener("submit", (e) => {
  e.preventDefault();
  if ($("nick").value) client.send(reg.LOGIN, $("nick").value);
});
$("send").addEventListener("submit", (e) => {
  e.preventDefault();
  if ($("text").value) client.send(reg.CHAT_SEND, $("text").value);
  $("text").value = "";
});
