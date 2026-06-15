// Live relay host — runs the REAL relay-server/src/server.ts PokemonCoopServer
// class over real WebSockets, locally, with no PartyKit login required.
//
// Why this exists: the deployed edge (wss://pokefirered-coop.thereuben.partykit.dev)
// is live but stale — it predates the `raw` passthrough case and silently drops
// {type:"raw"} messages, which carry seed-sync, role-assign, the state beacon,
// battle-turn and party-sync in the peer-to-peer ROM protocol. Redeploying the
// edge needs the user's interactive `partykit login`. This host loads the exact
// same server class (transpiled on the fly with esbuild) and serves it over the
// `ws` library, so the wire path is genuine WebSocket framing/ordering with the
// identical server logic — the closest faithful relay we can stand up
// autonomously.
//
// URL form mirrors PartyKit: ws://localhost:<port>/parties/main/<room>?session_id=<id>
//
// Usage: node live_relay_host.mjs [port]   (default 1999)

import { pathToFileURL } from "node:url";
import { writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { randomUUID } from "node:crypto";
import { createRequire } from "node:module";

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(__dirname, "..", "..");

// ws and esbuild live in relay-server/node_modules; resolve them from there
// regardless of where this script sits.
const require = createRequire(join(REPO_ROOT, "relay-server", "package.json"));
const { WebSocketServer } = require("ws");
const { build } = require("esbuild");
const SERVER_TS = join(REPO_ROOT, "relay-server", "src", "server.ts");
const PORT = parseInt(process.argv[2] || "1999", 10);

// ── Transpile the real server.ts → a loadable ESM module ──────────────────────
async function loadServerClass() {
  const out = await build({
    entryPoints: [SERVER_TS],
    bundle: true,
    format: "esm",
    platform: "node",
    write: false,
    // partykit/server is a type-only import; mark external so esbuild doesn't
    // try to resolve it (it is erased at runtime).
    external: ["partykit/server"],
    logLevel: "silent",
  });
  const code = out.outputFiles[0].text;
  const dir = mkdtempSync(join(tmpdir(), "coop-relay-"));
  const file = join(dir, "server.mjs");
  writeFileSync(file, code);
  const mod = await import(pathToFileURL(file).href);
  return mod.default;
}

// ── Room registry: one PokemonCoopServer instance per room path ───────────────
const rooms = new Map(); // roomKey -> { server, connections:Set<conn> }

function getRoom(PokemonCoopServer, roomKey) {
  let entry = rooms.get(roomKey);
  if (!entry) {
    const connections = new Set();
    // Minimal Party.Room adapter: only getConnections() and id are used.
    const roomAdapter = {
      id: roomKey,
      getConnections() {
        return connections;
      },
    };
    const server = new PokemonCoopServer(roomAdapter);
    entry = { server, connections };
    rooms.set(roomKey, entry);
  }
  return entry;
}

const PokemonCoopServer = await loadServerClass();

const wss = new WebSocketServer({ port: PORT });
let connCount = 0;

wss.on("connection", (ws, req) => {
  const fullUrl = `ws://localhost:${PORT}${req.url}`;
  const url = new URL(fullUrl);
  // /parties/main/<room>  ->  roomKey = <room>
  const parts = url.pathname.split("/").filter(Boolean);
  const roomKey = parts[parts.length - 1] || "default";

  const { server, connections } = getRoom(PokemonCoopServer, roomKey);

  // Party.Connection adapter
  const conn = {
    id: `c${++connCount}-${randomUUID().slice(0, 8)}`,
    _ws: ws,
    send(s) {
      if (ws.readyState === ws.OPEN) ws.send(s);
    },
    close() {
      try { ws.close(); } catch {}
    },
  };
  connections.add(conn);

  // Party.ConnectionContext adapter — server only reads ctx.request.url
  const ctx = { request: { url: fullUrl } };

  const ts = () => new Date().toISOString().slice(11, 23);
  console.log(`[${ts()}] + ${conn.id} -> room "${roomKey}" (now ${connections.size})`);

  try {
    server.onConnect(conn, ctx);
  } catch (e) {
    console.error("onConnect error:", e);
  }

  ws.on("message", (data) => {
    const text = typeof data === "string" ? data : data.toString("utf8");
    try {
      server.onMessage(text, conn);
    } catch (e) {
      console.error("onMessage error:", e);
    }
  });

  ws.on("close", () => {
    connections.delete(conn);
    try {
      server.onClose(conn);
    } catch (e) {
      console.error("onClose error:", e);
    }
    console.log(`[${ts()}] - ${conn.id} (room "${roomKey}" now ${connections.size})`);
    if (connections.size === 0) rooms.delete(roomKey);
  });

  ws.on("error", (e) => console.error(`ws error ${conn.id}:`, e.message));
});

console.log(`live relay host running: ws://localhost:${PORT}/parties/main/<room>`);
console.log(`(real PokemonCoopServer from relay-server/src/server.ts over ws)`);
