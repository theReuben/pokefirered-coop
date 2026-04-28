# App End-to-End Testing Guide

This document covers how to build and manually test the Pokémon FireRed Co-Op Tauri app.
Automated tests cover relay server logic (`make check-relay`) and ROM-side C code
(`make check-native`); this guide covers the app layer that ties them together.

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Rust + Cargo | ≥1.75 | Install via `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \| sh` |
| Node.js | ≥18 | Required for Tauri CLI and Vite |
| GBA toolchain | devkitARM | Required only for ROM builds, not app builds |
| pokefirered.gba | any co-op build | Built with `make firered -j4` |

---

## Build Steps

### 1. Build the ROM

```bash
make firered -j4          # builds pokefirered.gba
make bundle-rom           # copies it to tauri-app/src-tauri/rom/
```

### 2. Install Node dependencies

```bash
cd tauri-app
npm install
```

### 3. Compile and type-check

```bash
# Check Rust code compiles:
make check-tauri

# Check TypeScript types:
cd tauri-app && npx tsc --noEmit
```

### 4. Run in dev mode (stub emulator)

```bash
cd tauri-app
npm run tauri dev
```

The stub emulator renders solid grey frames — no actual game logic runs. This is
sufficient for testing the UI flow (host/join screens, session management, WebSocket
handshake).

### 5. Build with mGBA (full emulation)

The mGBA backend is gated behind the `mgba` Cargo feature. See
`tauri-app/src-tauri/src/emulator.rs` for the build steps:

```bash
# From tauri-app/src-tauri/:
git submodule add https://github.com/mgba-emu/mgba.git mgba
cd mgba && cmake -DBUILD_SHARED=OFF -DBUILD_STATIC=ON . && make mgba-core
mkdir -p lib && cp libmgba.a lib/
```

Then enable the feature:

```toml
# tauri-app/src-tauri/Cargo.toml
[features]
mgba = []
```

Build:

```bash
cd tauri-app
npm run tauri build -- --features mgba
```

---

## End-to-End Test: Two-Player Session

Run both instances on the same machine with different save files.

### Instance A — Host

1. Launch app (dev or release build).
2. Click **Host Game → New Game**.
3. Choose a save path (e.g. `~/saves/player1.sav`).
4. Note the 6-character room code displayed (e.g. `K8MNQ2`).
5. A `player1.coop` sidecar is created alongside `player1.sav`.

### Instance B — Guest

1. Launch a second app instance.
2. Click **Join Game**.
3. Enter the room code from Instance A.
4. Select a save file that has a matching `.coop` sidecar (copy `player1.coop` to
   `player2.coop` to simulate a shared session; or ask the host to share).
5. Guest connects and loads the host's `session_id` from the sidecar.

### Verification Checklist

#### Session / Handshake

- [ ] Host app displays room code on the Host Game screen before any input.
- [ ] Guest successfully connects; both apps show the game screen.
- [ ] In the relay server log: host receives `{"type":"role","role":"host"}`;
      guest receives `{"type":"role","role":"guest"}`.
- [ ] Connection status indicator shows "Connected" on both instances.

#### session_id Validation

1. Start a host session, note the `session_id` in `player1.coop`.
2. Manually edit a copy of the sidecar to have a different `session_id`.
3. Attempt to join as guest using the altered sidecar.
4. **Expected:** guest receives a `session_mismatch` message and sees an error:
   "This save was started with a different partner."
5. Host session remains unaffected.

#### Room Full (3rd Player Rejection)

1. Start a host session and connect a guest.
2. Launch a third app instance and attempt to join with the same code.
3. **Expected:** third connection receives `{"type":"room_full"}` and is rejected.
   First two instances are unaffected.

---

## End-to-End Test: Ghost NPC (requires mGBA feature)

Both players must be in-game on the same mGBA map.

### Setup

```
Instance A: host, map = PALLET_TOWN (map 0x001)
Instance B: guest, map = PALLET_TOWN
```

### Verification Checklist

- [ ] Guest's position is broadcast every ~4 frames as a `position` JSON message.
- [ ] Host's ROM receives `partner_position` and the ghost NPC sprite (Leaf/Green) 
      appears at the correct tile.
- [ ] Moving the guest causes the ghost NPC to move on the host's screen.
- [ ] Guest travels to ROUTE_1 (map 0x00C); ghost NPC despawns on host's screen.
- [ ] Guest returns to PALLET_TOWN; ghost NPC respawns at guest's position.

---

## End-to-End Test: Flag Sync (requires mGBA feature)

### Trainer Flag Sync

1. Host defeats a trainer (e.g. Bug Catcher on Route 1).
2. In the relay server log, verify a `flag_set` message is forwarded.
3. Open the guest's memory map or save file — the trainer defeated flag should be set
   even though the guest never fought that trainer.
4. Reload the guest's save; flag persists.

### Full Sync on Connect

1. Host defeats several trainers then disconnects guest.
2. Reconnect guest.
3. **Expected:** on reconnect, host sends a `full_sync` message; guest applies all
   trainer/story flags set by host. Guest's ROM reflects host's progress.

---

## End-to-End Test: Save File Persistence

### Normal Exit

1. Start a session, play for a few minutes (get into a trainer battle, etc.).
2. Close the app window (or click Quit).
3. **Expected:** `stop_emulator` command calls `flush_save()` before dropping the
   emulator. The `.sav` file on disk is updated.
4. Relaunch the app, load the same `.sav` file — progress is preserved.

### Explicit Save

While in-game, trigger `save_game` via Tauri (currently no UI button; use
`window.__TAURI__.invoke("save_game")` in the browser console during dev mode).

### Save File Location

The save path is whatever the user chose on the Host/Join screen. The `.coop`
sidecar lives at the same path with `.coop` extension:

```
~/saves/player1.sav   — GBA battery save (128 KiB)
~/saves/player1.coop  — JSON session metadata:
                         { sessionId, createdAt, randomizeEncounters }
```

---

## End-to-End Test: Relay Server Live

Verify the deployed PartyKit relay handles real traffic.

### Dev relay (local)

```bash
cd relay-server
npm run dev    # starts partykit dev server on port 1999
```

Edit `tauri-app/src-tauri/src/net.rs` temporarily:
```rust
const RELAY_URL: &str = "ws://localhost:1999/party";
```

Rebuild and run two instances against the local relay.

### Production relay

The hardcoded URL in `net.rs`:
```rust
const RELAY_URL: &str = "wss://pokefirered-coop.reubenday.partykit.dev/party";
```

Verify deployment is live:
```bash
cd relay-server
npx partykit deploy
```

Confirm by connecting two app instances and exchanging a position message.

---

## Keyboard Controls

| Key | GBA Button |
|-----|-----------|
| Arrow keys | D-pad |
| Z | A |
| X | B |
| Enter | Start |
| Backspace | Select |
| A | L |
| S | R |

---

## Known Limitations (v1)

- **Stub emulator:** Dev builds without `--features mgba` run the StubBackend, which
  renders grey frames. Use it only to test the UI flow and WebSocket handshake.
- **Serial bridge address:** `SEND_RING_ADDR` and `RECV_RING_ADDR` in `serial_bridge.rs`
  default to reference-build addresses (`0x0203_F000` and `0x0203_F800`). If the ROM
  is recompiled with different code size, re-run `extract_symbols.py` to regenerate
  the symbol map and update these defaults.
- **No gamepad support:** Only keyboard input is mapped. USB gamepad support is a
  future enhancement.
- **No in-game save button:** Call `save_game` via `stop_emulator` (on quit) or
  Tauri invoke from the browser console.

---

## Automated Test Coverage

| Layer | Command | Coverage |
|-------|---------|---------|
| ROM C unit tests | `make check-native` | Ring buffer, flag sync, boss state, randomizer |
| Relay server | `make check-relay` | 39 Vitest tests: roles, capacity, boss state, flag dedup |
| Tauri Rust types | `make check-tauri` | Type-checks all Rust sources |
| TypeScript types | `cd tauri-app && npx tsc --noEmit` | Catches TS type errors in frontend |
