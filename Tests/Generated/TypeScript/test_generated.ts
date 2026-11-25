// test_generated.ts - the generated TypeScript registry must match Generated/registry.txt.
// SPDX-License-Identifier: MIT
//
// Why: see Tests/Generated/C/test_generated.c.
// Usage: node test_generated.ts <path to registry.txt>   (Node 22.6+ runs .ts directly)
import { readFileSync } from "node:fs";
import { PACKETS } from "../../../Generated/TypeScript/registry.ts";

let pass = 0;
let fail = 0;
const lines = readFileSync(process.argv[2], "utf8").split("\n").filter(Boolean);
for (const line of lines) {
  const [id, name, schema] = line.split(" ");
  const p = PACKETS.get(parseInt(id, 16));
  p && p.name === name && p.schema === (schema ?? "") ? pass++ : fail++;
}
// The table must have exactly as many entries as the listing.
PACKETS.size === lines.length ? pass++ : fail++;
console.log(`${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
