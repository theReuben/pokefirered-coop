# Relay Server — Local Testing Guide

## Unit Tests

Run the vitest suite (no server required):

```bash
make check-relay
# or directly:
cd relay-server && npm test
```

Expected output: 39 tests pass.

## Local Dev Server

Start the PartyKit dev server on `localhost:1999`:

```bash
cd relay-server
npm run dev
```

PartyKit opens a local relay at `ws://localhost:1999/party/{room}`.

## WebSocket Smoke Test

With the dev server running, open two terminal windows and use `wscat`
(or any WebSocket client) to simulate two players:

**Terminal 1 — Host:**
```bash
wscat -c "ws://localhost:1999/party/testroom?session_id=abc123"
# Receives: {"type":"role","role":"host"}
# Receives: {"type":"full_sync","flags":[],"vars":{}}
```

**Terminal 2 — Guest:**
```bash
wscat -c "ws://localhost:1999/party/testroom?session_id=abc123"
# Receives: {"type":"role","role":"guest"}
# Receives: {"type":"full_sync","flags":[],"vars":{}}
# Receives: {"type":"session_settings","randomizeEncounters":true}
```

**Send a position from Terminal 1:**
```
{"type":"position","data":{"mapId":1,"x":5,"y":3,"facing":0,"spriteState":0}}
```
Terminal 2 should receive: `{"type":"partner_position","data":{...}}`

**Set a flag from Terminal 1:**
```
{"type":"flag_set","flagId":1280}
```
Both terminals receive `{"type":"flag_set","flagId":1280}`.

**Test boss readiness (both terminals):**
```
{"type":"boss_ready","bossId":1}
```
After both send this message, both receive `{"type":"boss_start","bossId":1}`.

## Session ID Mismatch Test

Connect a client with a different session_id — it should be rejected immediately:
```bash
wscat -c "ws://localhost:1999/party/testroom?session_id=wrongsession"
# Receives: {"type":"session_mismatch"}
# Connection closes
```

## Room Full Test

Connect a third client to the same room (already has two players):
```bash
wscat -c "ws://localhost:1999/party/testroom"
# Receives: {"type":"room_full"}
# Connection closes
```

## Production Deployment

```bash
cd relay-server
npx partykit deploy
```

The deployed URL will be `wss://pokefirered-coop.{account}.partykit.dev/party/{room}`.
Update the Tauri app's `RELAY_URL` constant to this value for Phase 7.
