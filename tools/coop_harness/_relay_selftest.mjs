// Two-client smoke test for live_relay_host.mjs: verifies role assignment,
// raw passthrough, and typed flag_set forwarding over real WebSockets.
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
const __dirname = dirname(fileURLToPath(import.meta.url));
const require = createRequire(join(__dirname, "..", "..", "relay-server", "package.json"));
const WebSocket = require("ws");

// argv[2] may be a bare port (local host) or a full ws(s):// base URL (live edge).
const ARG = process.argv[2] || "1999";
const ROOM = "selftest" + Date.now();
const base = /^wss?:\/\//.test(ARG)
  ? ARG.replace(/\/+$/, "")                 // strip trailing slash
  : `ws://localhost:${ARG}`;
const url = `${base}/parties/main/${ROOM}`;

function client(name) {
  const ws = new WebSocket(url);
  ws.received = [];
  ws.on("message", (d) => ws.received.push(JSON.parse(d.toString())));
  return new Promise((res) => ws.on("open", () => res(ws)));
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const host = await client("host");
await sleep(200);
const guest = await client("guest");
await sleep(300);

// host sends a raw packet + a typed flag_set
host.send(JSON.stringify({ type: "raw", bytes: "1b01" }));
host.send(JSON.stringify({ type: "flag_set", flagId: 0x820 }));
await sleep(300);

const guestTypes = guest.received.map((m) => m.type);
const hostTypes = host.received.map((m) => m.type);

const checks = {
  "host got role":   host.received.some((m) => m.type === "role" && m.role === "host"),
  "guest got role":  guest.received.some((m) => m.type === "role" && m.role === "guest"),
  "guest got raw":   guest.received.some((m) => m.type === "raw" && m.bytes === "1b01"),
  "guest got flag":  guest.received.some((m) => m.type === "flag_set" && m.flagId === 0x820),
  "host no echo raw": !host.received.some((m) => m.type === "raw"),
};

let ok = true;
for (const [k, v] of Object.entries(checks)) {
  console.log(`${v ? "PASS" : "FAIL"}  ${k}`);
  if (!v) ok = false;
}
console.log("host received:", hostTypes.join(","));
console.log("guest received:", guestTypes.join(","));
host.close(); guest.close();
process.exit(ok ? 0 : 1);
