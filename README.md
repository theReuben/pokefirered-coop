# Pokémon FireRed Co-Op

A networked 2-player co-op mod for Pokémon FireRed, built on pokeemerald-expansion. Both players explore Kanto simultaneously in a shared world — beating trainers, fighting gym leaders together, and catching randomized Pokémon from across all generations.

## Building the ROM

Requires devkitARM. See [INSTALL.md](INSTALL.md) for toolchain setup.

```bash
make firered -j$(nproc)
```

Output: `pokefirered.gba`. One expected linker warning about RWX ELF segments is normal.

## Status

All ROM-side features are complete and tested. The Tauri desktop app and PartyKit relay server are built and need end-to-end integration testing before release.

| Feature | Status |
|---|---|
| Ghost NPC — see your partner on the map | ✅ |
| Position sync (~4 updates/sec) | ✅ |
| Follower Pokémon ghost | ✅ |
| Co-op starter selection (Oak's lab) | ✅ |
| Trainer / story flag sync | ✅ |
| Randomized wild encounters (shared seed) | ✅ |
| Boss readiness system (gym leaders wait for both) | ✅ |
| Co-op double battles (turn sync, RNG lockstep) | ✅ |
| Disconnect detection + ghost despawn (<0.5s) | ✅ |
| Battle grace timer / reconnect | ✅ |
| Auto-checkpoint save on map change | ✅ |
| Tauri desktop app | 🔧 built, needs E2E test |
| PartyKit relay deployment | 🔧 built, needs deployment |

## Project Structure

```
src/multiplayer.c              Core co-op logic (1700+ lines)
include/multiplayer.h          Packet format, ring buffer API, state struct
include/constants/multiplayer.h  Packet type constants
src/event_data.c               FlagSet/VarSet hooks for flag sync
data/maps/*/scripts.inc        Per-map event scripts (boss triggers, Oak's lab)
src/scrcmd.c                   Script commands: waitbossstart, waitcoopparty, etc.

tools/mcp_gamestate/           Headless mGBA MCP server for Claude Code testing
  server.py                    MCP server — boots instances, relays packets
  bridge.lua                   Lua script running inside mGBA (IPC bridge)
test/lua/states/               Pre-built save states for automated testing
  oaks_lab.ss1                 Outside Oak's lab, starters ready to pick
  tall_grass_route1.ss1        Route 1 with both players present
  pewter_gym.ss1               Pewter Gym ready for co-op boss battle

relay-server/src/server.ts     PartyKit relay server (302 lines)
tauri-app/src-tauri/           Rust backend: libmgba FFI, ring buffer bridge, WebSocket
tauri-app/src/                 React frontend: host/join UI, game screen
```

## Testing with Claude Code (MCP)

The project includes a headless mGBA MCP server that lets Claude Code drive two emulator instances and verify co-op behaviour automatically.

```bash
# Add the MCP server once:
claude mcp add --scope project gamestate -- python3 tools/mcp_gamestate/server.py

# Rebuild save states after any ROM change:
make build-states
```

Available tools: `start_emulator`, `stop_emulator`, `screenshot`, `press_button`, `wait`, `load_savestate`. Instances `p1` and `p2` relay packets automatically.

## Running the Relay Server Locally

```bash
cd relay-server/
npm install
npx partykit dev    # starts at ws://localhost:1999
```

Override the relay URL in the Tauri app:

```bash
COOP_RELAY_URL=ws://localhost:1999/parties/main cargo tauri dev
```

## Running the Tauri App

```bash
cd tauri-app/
npm install
cargo tauri dev     # development build with hot reload
cargo tauri build   # release build
```

The production relay URL (`wss://pokefirered-coop.thereuben.partykit.dev`) is baked into release builds. Override with the `COOP_RELAY_URL` environment variable for staging.

## Architecture

```
Player 1 (Tauri)                        Player 2 (Tauri)
┌────────────────────┐                  ┌────────────────────┐
│  pokefirered.gba   │                  │  pokefirered.gba   │
│  (libmgba)         │                  │  (libmgba)         │
│                    │                  │                    │
│  gMpSendRing ──────┼──► serial   ─────┼──► gMpRecvRing     │
│  gMpRecvRing ◄─────┼── bridge   ◄─────┼─── gMpSendRing     │
└────────────────────┘  (WebSocket)     └────────────────────┘
          │                                       │
          └──────────────┐       ┌────────────────┘
                         ▼       ▼
                   PartyKit relay server
          (pokefirered-coop.thereuben.partykit.dev)
```

The ROM uses two ring buffers in EWRAM (`gMpSendRing`, `gMpRecvRing`). The Tauri app's `serial_bridge.rs` polls these buffers at ~60fps via libmgba memory reads/writes and forwards raw bytes over WebSocket to the relay server, which fans them out to the partner.

## Packet Reference

| Byte | Name | Description |
|---|---|---|
| `0x01` | POSITION | Map ID, tile X/Y, facing direction (every 4 frames) |
| `0x02` | FLAG\_SET | Trainer/story flag set |
| `0x03` | FLAG\_CLEAR | Flag cleared |
| `0x04` | VAR\_SET | Script variable changed |
| `0x08` | BOSS\_READY | Player at gym trigger, ready to fight |
| `0x09` | BOSS\_CANCEL | Player walked away from trigger |
| `0x0B` | PARTNER\_CONNECTED | Partner joined session |
| `0x0C` | PARTNER\_DISCONNECTED | Partner left or timed out |
| `0x0D` | FULL\_SYNC | Full flag/var dump sent on connect |
| `0x10` | STARTER\_PICK | Player chose a starter species |
| `0x11` | NAME | Player name (sent on connect) |
| `0x12` | PARTY\_SYNC | Full party snapshot for double battle setup |
| `0x13` | BATTLE\_TURN | Move slot + target selection |
| `0x14` | FOLLOWER\_GFX | Lead follower Pokémon graphics ID |
| `0x16` | PING | Heartbeat (every 2s, used for disconnect detection) |
| `0x17` | HOST\_MIGRATE | Surviving player promoted to host on partner death |

For full architecture and implementation guidelines see [CLAUDE.md](CLAUDE.md).
