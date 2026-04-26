# Link Cable System — Research Notes (Phase 2)

## Summary

The existing `src/link.c` implements a GBA multi-player link cable protocol
designed for 2–4 players connected via a physical cable.  We do **not** use
this protocol for the co-op mod.  Instead we define our own ring-buffer
interface in EWRAM that the Tauri host app reads/writes through libmgba's
memory API.

## Existing Link Cable Protocol (background)

### Key globals

| Symbol | Type | Purpose |
|---|---|---|
| `gLink` | `struct Link` | Core link state: queues, player count, error flags |
| `gSendCmd[CMD_LENGTH]` | `u16[8]` | Staging buffer built by `BuildSendCmd()` then enqueued |
| `gRecvCmds[5][8]` | `u16[5][8]` | Most-recently-dequeued receive data (per player) |
| `gLink.sendQueue` | `struct SendQueue` | Circular queue of 50 × 8-word entries to transmit |
| `gLink.recvQueue` | `struct RecvQueue` | Circular queue of 50 × 8-word entries received |

### Timing / interrupt flow

1. Master GBA fires `REG_TM3` at a fixed rate (~60 Hz per exchange).
2. `REG_SIOIRQ` (serial interrupt) fires after each 16-bit SIO exchange.
3. `DoHandshake()` / `DoSend()` / `DoRecv()` / `SendRecvDone()` run inside
   the interrupt.
4. `DoSend()` drains `gLink.sendQueue.data`, one u16 per interrupt.
5. `DoRecv()` fills `gLink.recvQueue.data`, one u16 per interrupt.
6. After `CMD_LENGTH=8` u16s have been exchanged, `SendRecvDone()` advances
   the queue pointers.
7. `EnqueueSendCmd(u16 *sendCmd)` is how caller code adds 8-word messages.
8. `ProcessRecvCmds(playerId)` runs in the game loop to dispatch received
   messages to their handlers.

### Command format

Each "command" is exactly 8 × u16 = 16 bytes:

```
Word 0: command type (LINKCMD_*)
Word 1–7: payload (interpretation depends on command type)
```

`LINKCMD_SEND_PACKET (0x2FFF)` is used for block data transfers.
Most command codes are in the 0x1000–0xFFFF range.

## Co-op Mod Strategy: Ring Buffer in EWRAM

We bypass `src/link.c` entirely.  The ROM and the Tauri host communicate
through two ring buffers in EWRAM:

```
gMpSendRing  — ROM writes outgoing packets; Tauri reads and forwards to relay
gMpRecvRing  — Tauri writes incoming packets from relay; ROM reads and processes
```

### Why ring buffers?

- No hardware dependencies — works with any mGBA libmgba integration.
- Tauri polls them every frame (or on a timer) via `mgba_memory_read/write`.
- Natural FIFO ordering; ROM never blocks.
- 256-byte buffers with `u8` head/tail wrap naturally with no modulo.

### Memory layout (`struct MpRingBuf`)

```c
struct MpRingBuf {
    u8  buf[MP_RING_SIZE];   // circular byte storage
    u8  head;                // producer advances (ROM for send, Tauri for recv)
    u8  tail;                // consumer advances (Tauri for send, ROM for recv)
    u8  magic;               // 0xC0 when valid — Tauri uses this to locate buffer
    u8  _pad;
};
```

With `MP_RING_SIZE = 256`, `u8` arithmetic gives free modulo-256 wrapping.
Buffer is empty when `head == tail`, full when `(head+1) == tail`.

### Packet format (binary, little-endian on GBA)

All packets begin with a 1-byte type byte:

| Type | Name | Payload | Total |
|---|---|---|---|
| `0x01` | POSITION | mapGroup, mapNum, x, y, facing | 6 B |
| `0x02` | FLAG\_SET | flagId\_hi, flagId\_lo | 3 B |
| `0x03` | VAR\_SET | varId\_hi, varId\_lo, value\_hi, value\_lo | 5 B |
| `0x04` | BOSS\_READY | bossId | 2 B |
| `0x05` | BOSS\_CANCEL | (none) | 1 B |
| `0x06` | SEED\_SYNC | seed[0..3] (BE) | 5 B |
| `0x07` | FULL\_SYNC | len\_hi, len\_lo, data[len] | 3+len B |

Maximum single-packet payload: FULL\_SYNC with up to ~250 bytes of flag data
(flag bitmap is ~172 bytes for trainer flags).  This fits in one 256-byte ring.

### Hook points

- **Sending**: `Multiplayer_SendPosition()` etc. call `Mp_Push()` to write a
  packet into `gMpSendRing`.
- **Receiving**: `Multiplayer_Update()` calls `Mp_ProcessRecv()` which reads
  from `gMpRecvRing` one packet per frame and dispatches.
- **Tauri side**: on each frame tick, libmgba callback reads `gMpSendRing`
  (advancing `tail`), sends to WebSocket, then writes any pending incoming
  data into `gMpRecvRing` (advancing `head`).

### Locating the buffers from Tauri

The Tauri app hard-codes the EWRAM address of `gMpSendRing` and `gMpRecvRing`
after each build (from the `.map` file or ELF symbols).  The `magic` field
(0xC0) provides a sanity check.  In a future iteration we could expose a
fixed-address sentinel in EWRAM for dynamic discovery.
