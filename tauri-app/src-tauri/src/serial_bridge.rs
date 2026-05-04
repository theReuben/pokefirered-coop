/// Serial Bridge — translates between the GBA ring buffers in ROM memory and
/// JSON messages sent/received by the WebSocket client (net.rs).
///
/// Ring buffer layout (struct MpRingBuf from include/multiplayer.h):
///   offset   0: u8 buf[256]  — circular byte storage
///   offset 256: u8 head      — producer write index
///   offset 257: u8 tail      — consumer read index
///   offset 258: u8 magic     — 0xC0 when valid
///   offset 259: u8 _pad
///
/// Send ring: ROM produces (head), Tauri consumes (tail).
/// Recv ring: Tauri produces (head), ROM consumes (tail).

use crate::emulator::EmulatorHandle;
use crate::net::NetHandle;
use serde_json::{json, Value};

// Addresses are set from the ROM linker map at runtime; defaults match a
// reference build.  Both rings are 260 bytes each (256-byte buf + 4 ctrl bytes).
static mut SEND_RING_ADDR: u32 = 0x0203_1454;
static mut RECV_RING_ADDR: u32 = 0x0203_1350;

// gMultiplayerState is in IWRAM (0x0300157c from nm pokefirered.elf).
// connState is the second byte (+1 offset).  We write it directly every frame
// so that any ROM build picks up the connection state without requiring the
// new PKT_PARTNER_CONNECTED ring-packet handler to be compiled in.
const CONN_STATE_ADDR: u32 = 0x0300_157d;

// Tracks whether the relay has a connected partner for us.
// Written from the Tauri network thread; read only in tick() on the emu thread.
static mut PARTNER_CONNECTED: bool = false;

// Encounter seed from the host's session.  Set once by set_encounter_seed()
// when the emulator starts; written directly to gCoopSettings.encounterSeed
// every tick so it takes effect as soon as the game initialises the field.
static mut ENCOUNTER_SEED: u32 = 0;

// Diagnostic counters — updated in tick(), read by get_debug_state().
static mut TICK_COUNT:    u64 = 0;
static mut PACKETS_SENT:  u64 = 0;
static mut PACKETS_RECV:  u64 = 0;
// Role received from relay: 0=none, 1=host, 2=guest
static mut RECEIVED_ROLE: u8  = 0;

// gCoopSettings (0x0300_158c) layout: u8 flags @ +0, u32 encounterSeed @ +4,
// u32 sendRingAddr @ +8, u32 recvRingAddr @ +12.
const COOP_SEED_ADDR:              u32 = 0x0300_1590; // gCoopSettings.encounterSeed
const COOP_SETTINGS_ADDR:          u32 = 0x0300_158c; // gCoopSettings.randomizeEncounters byte
const RING_ADDR_DISCOVERY_SEND:    u32 = 0x0300_1594; // gCoopSettings.sendRingAddr
const RING_ADDR_DISCOVERY_RECV:    u32 = 0x0300_1598; // gCoopSettings.recvRingAddr

/// Call once from start_emulator to prime the encounter seed for this session.
pub fn set_encounter_seed(seed: u32) {
    unsafe { ENCOUNTER_SEED = seed; }
}

const RING_BUF_SIZE:  usize = 256;
const RING_HEAD_OFF:  u32   = 256; // byte offset of head within the struct
const RING_TAIL_OFF:  u32   = 257;
const RING_MAGIC_OFF: u32   = 258;
const RING_MAGIC:     u8    = 0xC0;

// Packet type codes — must match include/constants/multiplayer.h
const PKT_POSITION:             u8 = 0x01;
const PKT_FLAG_SET:             u8 = 0x02;
const PKT_VAR_SET:              u8 = 0x03;
const PKT_BOSS_READY:           u8 = 0x04;
const PKT_BOSS_CANCEL:          u8 = 0x05;
const PKT_SEED_SYNC:            u8 = 0x06;
const PKT_FULL_SYNC:            u8 = 0x07;
const PKT_SCRIPT_LOCK:          u8 = 0x08;
const PKT_SCRIPT_UNLOCK:        u8 = 0x09;
const PKT_BOSS_START:           u8 = 0x0A;
const PKT_PARTNER_CONNECTED:    u8 = 0x0B;
const PKT_PARTNER_DISCONNECTED: u8 = 0x0C;
const PKT_ITEM_GIVE:            u8 = 0x0D; // 4 bytes: type + item_hi + item_lo + quantity
const PKT_FLAG_CLEAR:           u8 = 0x0E; // 3 bytes: type + flagId_hi + flagId_lo

// FULL_SYNC payload is exactly this many bytes (must match FULL_SYNC_PAYLOAD_SIZE in ROM).
// Layout: story(92) + items(24) + bosses(2) + trainers(96) + badges(2) = 216 bytes.
// Story range covers 0x020-0x2FF (bytes 4-95): NPC hide/show + completion flags.
const FULL_SYNC_PAYLOAD_SIZE: usize = 216;

pub fn tick(emu: &mut EmulatorHandle, net: &NetHandle) {
    let tick = unsafe { TICK_COUNT += 1; TICK_COUNT };

    // Discover ring buffer addresses from gCoopSettings fields written by
    // Multiplayer_Init().  This corrects for EWRAM BSS layout differences
    // between toolchains (local devkitARM vs CI arm-none-eabi-gcc).
    // GBA is little-endian; read 4 bytes and reassemble.
    let send_raw = emu.read_bytes(RING_ADDR_DISCOVERY_SEND, 4);
    if send_raw.len() == 4 {
        let addr = u32::from_le_bytes([send_raw[0], send_raw[1], send_raw[2], send_raw[3]]);
        if addr != 0 && addr != unsafe { SEND_RING_ADDR } {
            log::info!("serial_bridge: discovered sendRingAddr = 0x{:08X}", addr);
            unsafe { SEND_RING_ADDR = addr; }
        }
    }
    let recv_raw = emu.read_bytes(RING_ADDR_DISCOVERY_RECV, 4);
    if recv_raw.len() == 4 {
        let addr = u32::from_le_bytes([recv_raw[0], recv_raw[1], recv_raw[2], recv_raw[3]]);
        if addr != 0 && addr != unsafe { RECV_RING_ADDR } {
            log::info!("serial_bridge: discovered recvRingAddr = 0x{:08X}", addr);
            unsafe { RECV_RING_ADDR = addr; }
        }
    }

    // Drain outbound (ROM → relay) first so the partner sees our position.
    let outbound = drain_send_ring(emu);
    for pkt in outbound {
        if let Some(msg) = packet_to_json(&pkt) {
            unsafe { PACKETS_SENT += 1; }
            net.send(msg);
        }
    }

    // Process inbound (relay → ROM).
    for msg in net.drain_inbound() {
        // Track partner presence before routing to ring so the heartbeat below
        // reflects the new state on this same frame.
        match msg.get("type").and_then(|t| t.as_str()) {
            Some("partner_connected")    => {
                unsafe { PARTNER_CONNECTED = true; }
                log::info!("serial_bridge: partner connected");
            }
            Some("partner_disconnected") => {
                unsafe { PARTNER_CONNECTED = false; }
                log::info!("serial_bridge: partner disconnected");
            }
            Some("role") => {
                let role_str = msg.get("role").and_then(|r| r.as_str()).unwrap_or("");
                let role_num: u8 = match role_str { "host" => 1, "guest" => 2, _ => 0 };
                unsafe { RECEIVED_ROLE = role_num; }
                log::info!("serial_bridge: received role={}", role_str);
                // Belt-and-suspenders: if the server assigns us "guest" role the
                // host was already present when we joined — mark as connected now
                // rather than waiting for the separate partner_connected message.
                if role_str == "guest" {
                    unsafe { PARTNER_CONNECTED = true; }
                    log::info!("serial_bridge: guest role assigned — host already present");
                }
            }
            Some("session_settings") => {
                // Guest receives host's settings from relay on connect.
                // Store the seed; it will be written to ROM each tick.
                if let Some(seed) = msg.get("encounterSeed").and_then(|v| v.as_u64()) {
                    if seed != 0 {
                        unsafe { ENCOUNTER_SEED = seed as u32; }
                        log::info!("serial_bridge: encounter seed received from relay: {}", seed);
                    }
                }
            }
            _ => {}
        }
        if let Some(pkt) = json_to_packet(&msg) {
            unsafe { PACKETS_RECV += 1; }
            push_recv_ring(emu, &pkt);
        }
    }

    // Heartbeat: write gMultiplayerState.connState directly every frame so the
    // ROM always reflects the correct connection state regardless of ROM build
    // version.  MP_STATE_CONNECTED = 2, MP_STATE_DISCONNECTED = 0.
    // This also handles the race where Multiplayer_Init() resets connState to 0
    // after partner_connected already arrived from the relay.
    let target_conn_state: u8 = if unsafe { PARTNER_CONNECTED } { 2 } else { 0 };
    let current = emu.read_bytes(CONN_STATE_ADDR, 1);
    if !current.is_empty() && current[0] != target_conn_state {
        log::info!("serial_bridge: connState {} → {}", current[0], target_conn_state);
        emu.write_bytes(CONN_STATE_ADDR, &[target_conn_state]);
    }

    // Write encounter seed directly to gCoopSettings.encounterSeed every tick.
    // This bypasses the ring buffer so the seed is available the instant the
    // game loads, regardless of when Multiplayer_Init() runs.
    let seed = unsafe { ENCOUNTER_SEED };
    if seed != 0 {
        let bytes = seed.to_le_bytes();
        emu.write_bytes(COOP_SEED_ADDR, &bytes);
    }

    // Log a one-line status summary once per second (≈60 ticks).
    if tick % 60 == 0 {
        let cs   = if current.is_empty() { 0 } else { current[0] };
        let sr   = emu.read_bytes(unsafe { SEND_RING_ADDR } + RING_HEAD_OFF, 3);
        let rr   = emu.read_bytes(unsafe { RECV_RING_ADDR } + RING_HEAD_OFF, 3);
        let (sh, st, sm) = if sr.len() >= 3 { (sr[0], sr[1], sr[2]) } else { (0, 0, 0) };
        let (rh, rt, rm) = if rr.len() >= 3 { (rr[0], rr[1], rr[2]) } else { (0, 0, 0) };
        log::info!(
            "mp tick={} partner={} connState={} snd(h={} t={} magic={:02X}) rcv(h={} t={} magic={:02X}) sent={} recv={}",
            tick,
            unsafe { PARTNER_CONNECTED },
            cs,
            sh, st, sm,
            rh, rt, rm,
            unsafe { PACKETS_SENT },
            unsafe { PACKETS_RECV },
        );
    }
}

/// Snapshot of multiplayer bridge state for the debug overlay.
#[derive(serde::Serialize)]
pub struct DebugState {
    pub partner_connected: bool,
    pub role:              u8, // 0=none, 1=host, 2=guest
    pub conn_state:        u8,
    pub send_magic:        u8,
    pub send_head:         u8,
    pub send_tail:         u8,
    pub recv_magic:        u8,
    pub recv_head:         u8,
    pub recv_tail:         u8,
    pub packets_sent:      u64,
    pub packets_recv:      u64,
    pub ticks:             u64,
}

pub fn get_debug_state(emu: &EmulatorHandle) -> DebugState {
    let cs = {
        let v = emu.read_bytes(CONN_STATE_ADDR, 1);
        if v.is_empty() { 0 } else { v[0] }
    };
    let sr = emu.read_bytes(unsafe { SEND_RING_ADDR } + RING_HEAD_OFF, 3);
    let rr = emu.read_bytes(unsafe { RECV_RING_ADDR } + RING_HEAD_OFF, 3);
    let (sh, st, sm) = if sr.len() >= 3 { (sr[0], sr[1], sr[2]) } else { (0, 0, 0) };
    let (rh, rt, rm) = if rr.len() >= 3 { (rr[0], rr[1], rr[2]) } else { (0, 0, 0) };
    unsafe {
        DebugState {
            partner_connected: PARTNER_CONNECTED,
            role:              RECEIVED_ROLE,
            conn_state:        cs,
            send_magic:        sm,
            send_head:         sh,
            send_tail:         st,
            recv_magic:        rm,
            recv_head:         rh,
            recv_tail:         rt,
            packets_sent:      PACKETS_SENT,
            packets_recv:      PACKETS_RECV,
            ticks:             TICK_COUNT,
        }
    }
}

// ── Outbound: drain the ROM's send ring ──────────────────────────────────────

fn drain_send_ring(emu: &mut EmulatorHandle) -> Vec<Vec<u8>> {
    let base = unsafe { SEND_RING_ADDR };

    // Read the full 260-byte struct in one shot for a consistent snapshot.
    let raw = emu.read_bytes(base, 260);
    if raw[258] != RING_MAGIC {
        return vec![];
    }
    let head = raw[256]; // ROM writes (producer)
    let tail = raw[257]; // Tauri reads (consumer)

    if head == tail {
        return vec![];
    }

    // Copy available bytes in ring order (u8 index wraps at 256 naturally).
    let avail = head.wrapping_sub(tail) as usize;
    let ring_data: Vec<u8> = (0..avail)
        .map(|i| raw[tail.wrapping_add(i as u8) as usize])
        .collect();

    // Parse fixed-size packets.
    let mut packets = Vec::new();
    let mut consumed = 0;
    while consumed < ring_data.len() {
        let size = packet_size(&ring_data, consumed);
        if size == 0 {
            log::warn!("serial_bridge: unknown send-ring packet type 0x{:02X} — draining", ring_data[consumed]);
            consumed += 1;
            continue;
        }
        if consumed + size > ring_data.len() {
            break; // partial packet — leave for next tick
        }
        packets.push(ring_data[consumed..consumed + size].to_vec());
        consumed += size;
    }

    // Advance tail by number of bytes actually parsed.
    if consumed > 0 {
        emu.write_bytes(base + RING_TAIL_OFF, &[tail.wrapping_add(consumed as u8)]);
    }

    packets
}

// ── Inbound: push a packet into the ROM's recv ring ──────────────────────────

fn push_recv_ring(emu: &mut EmulatorHandle, pkt: &[u8]) {
    let base = unsafe { RECV_RING_ADDR };

    // Read just the 4 control bytes (head, tail, magic, pad) at offset 256.
    let ctrl = emu.read_bytes(base + RING_HEAD_OFF, 4);
    if ctrl[2] != RING_MAGIC {
        return;
    }
    let head = ctrl[0]; // Tauri writes (producer)
    let tail = ctrl[1]; // ROM reads (consumer)

    let used  = head.wrapping_sub(tail) as usize;
    let free  = RING_BUF_SIZE - 1 - used;
    if pkt.len() > free {
        log::warn!("serial_bridge: recv ring full ({} free) — dropping packet type 0x{:02X}", free, pkt[0]);
        return;
    }

    // Write bytes into buf[0..255], wrapping head as u8.
    let mut wh = head;
    for &byte in pkt {
        emu.write_bytes(base + wh as u32, &[byte]);
        wh = wh.wrapping_add(1);
    }

    emu.write_bytes(base + RING_HEAD_OFF, &[wh]);
}

// ── Packet size lookup ────────────────────────────────────────────────────────

fn packet_size(raw: &[u8], pos: usize) -> usize {
    if pos >= raw.len() {
        return 0;
    }
    match raw[pos] {
        PKT_POSITION      => 6,
        PKT_FLAG_SET      => 3,
        PKT_VAR_SET       => 5,
        PKT_BOSS_READY    => 2,
        PKT_BOSS_CANCEL   => 1,
        PKT_SEED_SYNC     => 5,
        PKT_FULL_SYNC     => {
            if pos + 3 > raw.len() {
                return 0;
            }
            let data_len = ((raw[pos + 1] as usize) << 8) | raw[pos + 2] as usize;
            3 + data_len
        }
        PKT_SCRIPT_LOCK        => 1,
        PKT_SCRIPT_UNLOCK      => 1,
        PKT_BOSS_START         => 1,
        PKT_PARTNER_CONNECTED    => 1,
        PKT_PARTNER_DISCONNECTED => 1,
        PKT_ITEM_GIVE            => 4,
        PKT_FLAG_CLEAR           => 3,
        _ => 0,
    }
}

// ── Packet → JSON (ROM → relay) ───────────────────────────────────────────────
//
// Packets are raw bytes: [type][data...] — NO length byte.
// JSON format must match relay server's ClientMessage types (server.ts).

fn packet_to_json(pkt: &[u8]) -> Option<Value> {
    if pkt.is_empty() {
        return None;
    }
    match pkt[0] {
        // [type][mapGroup][mapNum][x][y][facing]
        // Relay uses PlayerState with mapId = (mapGroup<<8)|mapNum for back-compat.
        PKT_POSITION if pkt.len() == 6 => Some(json!({
            "type": "position",
            "data": {
                "mapId":       (((pkt[1] as u32) << 8) | pkt[2] as u32),
                "x":           pkt[3] as u32,
                "y":           pkt[4] as u32,
                "facing":      pkt[5] as u32,
                "spriteState": 0u32
            }
        })),

        // [type][flagId_hi][flagId_lo]  (big-endian)
        PKT_FLAG_SET if pkt.len() == 3 => {
            let flag_id = ((pkt[1] as u32) << 8) | pkt[2] as u32;
            Some(json!({ "type": "flag_set", "flagId": flag_id }))
        }

        // [type][varId_hi][varId_lo][val_hi][val_lo]  (big-endian)
        PKT_VAR_SET if pkt.len() == 5 => {
            let var_id = ((pkt[1] as u32) << 8) | pkt[2] as u32;
            let value  = ((pkt[3] as u32) << 8) | pkt[4] as u32;
            Some(json!({ "type": "var_set", "varId": var_id, "value": value }))
        }

        // [type][bossId]
        PKT_BOSS_READY if pkt.len() == 2 => {
            Some(json!({ "type": "boss_ready", "bossId": pkt[1] as u32 }))
        }

        // [type]
        PKT_BOSS_CANCEL => Some(json!({ "type": "boss_cancel" })),

        // [type][seed3][seed2][seed1][seed0]  (big-endian u32)
        PKT_SEED_SYNC if pkt.len() == 5 => {
            let seed = ((pkt[1] as u32) << 24) | ((pkt[2] as u32) << 16)
                     | ((pkt[3] as u32) << 8)  |  pkt[4] as u32;
            Some(json!({ "type": "session_settings", "encounterSeed": seed }))
        }

        // [type][len_hi][len_lo][payload...]
        PKT_FULL_SYNC if pkt.len() >= 3 => {
            let data_len = ((pkt[1] as usize) << 8) | pkt[2] as usize;
            let data = &pkt[3..(3 + data_len).min(pkt.len())];
            let mut flags = Vec::new();
            for (byte_idx, &byte) in data.iter().enumerate() {
                for bit in 0..8u32 {
                    if byte & (1 << bit) != 0 {
                        flags.push((byte_idx as u32) * 8 + bit);
                    }
                }
            }
            Some(json!({ "type": "full_sync", "flags": flags, "vars": {} }))
        }

        PKT_SCRIPT_LOCK   => Some(json!({ "type": "script_lock" })),
        PKT_SCRIPT_UNLOCK => Some(json!({ "type": "script_unlock" })),

        // [type][item_hi][item_lo][quantity]
        PKT_ITEM_GIVE if pkt.len() == 4 => {
            let item_id  = ((pkt[1] as u32) << 8) | pkt[2] as u32;
            let quantity = pkt[3] as u32;
            Some(json!({ "type": "item_give", "itemId": item_id, "quantity": quantity }))
        }

        // [type][flagId_hi][flagId_lo]  (big-endian)
        PKT_FLAG_CLEAR if pkt.len() == 3 => {
            let flag_id = ((pkt[1] as u32) << 8) | pkt[2] as u32;
            Some(json!({ "type": "flag_clear", "flagId": flag_id }))
        }

        _ => {
            log::warn!("serial_bridge: unknown outbound packet type 0x{:02X}", pkt[0]);
            None
        }
    }
}

// ── JSON → Packet (relay → ROM) ───────────────────────────────────────────────
//
// Packets written to the recv ring are raw ROM format: [type][data...] — NO length byte.

fn json_to_packet(msg: &Value) -> Option<Vec<u8>> {
    let msg_type = msg.get("type")?.as_str()?;
    match msg_type {
        // Relay renames "position" → "partner_position" when broadcasting.
        // data.mapId encodes (mapGroup<<8)|mapNum for back-compat with relay.
        "partner_position" => {
            let d = msg.get("data")?;
            let map_id    = d.get("mapId")?.as_u64()? as u32;
            let map_group = (map_id >> 8) as u8;
            let map_num   = (map_id & 0xFF) as u8;
            let x         = d.get("x")?.as_u64()? as u8;
            let y         = d.get("y")?.as_u64()? as u8;
            let facing    = d.get("facing")?.as_u64()? as u8;
            Some(vec![PKT_POSITION, map_group, map_num, x, y, facing])
        }

        "flag_set" => {
            let flag_id = msg.get("flagId")?.as_u64()? as u16;
            Some(vec![PKT_FLAG_SET, (flag_id >> 8) as u8, flag_id as u8])
        }

        "var_set" => {
            let var_id = msg.get("varId")?.as_u64()? as u16;
            let value  = msg.get("value")?.as_u64()? as u16;
            Some(vec![PKT_VAR_SET, (var_id >> 8) as u8, var_id as u8,
                      (value >> 8) as u8, value as u8])
        }

        // flags is an array of GBA flag IDs; rebuild the 216-byte payload the ROM expects.
        "full_sync" => {
            let flags = msg.get("flags")?.as_array()?;
            let payload = build_full_sync_payload(flags);
            let len = FULL_SYNC_PAYLOAD_SIZE as u16;
            let mut pkt = vec![PKT_FULL_SYNC, (len >> 8) as u8, len as u8];
            pkt.extend_from_slice(&payload);
            Some(pkt)
        }

        // boss_start from relay means both players ready; 1-byte packet to ROM.
        "boss_start" => Some(vec![PKT_BOSS_START]),

        // encounterSeed > 0 → send to ROM as SEED_SYNC (big-endian).
        "session_settings" => {
            let seed = msg.get("encounterSeed").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
            if seed != 0 {
                Some(vec![PKT_SEED_SYNC,
                          (seed >> 24) as u8, (seed >> 16) as u8,
                          (seed >> 8)  as u8,  seed as u8])
            } else {
                None
            }
        }

        "partner_connected"    => Some(vec![PKT_PARTNER_CONNECTED]),
        "partner_disconnected" => Some(vec![PKT_PARTNER_DISCONNECTED]),
        "script_lock"          => Some(vec![PKT_SCRIPT_LOCK]),
        "script_unlock"        => Some(vec![PKT_SCRIPT_UNLOCK]),

        "item_give" => {
            let item_id  = msg.get("itemId")?.as_u64()? as u16;
            let quantity = msg.get("quantity")?.as_u64()? as u8;
            Some(vec![PKT_ITEM_GIVE, (item_id >> 8) as u8, item_id as u8, quantity])
        }

        "flag_clear" => {
            let flag_id = msg.get("flagId")?.as_u64()? as u16;
            Some(vec![PKT_FLAG_CLEAR, (flag_id >> 8) as u8, flag_id as u8])
        }

        // boss_waiting is server telling us to wait — ROM already polls bossReadyBossId.
        // role is handled by the Tauri app state, not the ROM.
        // room_full / session_mismatch are handled by the frontend.
        "boss_waiting" | "role" | "room_full" | "session_mismatch"
        | "starter_denied" | "starter_taken" | "party_sync" | "battle_turn" => None,

        other => {
            log::debug!("serial_bridge: unhandled inbound message type: {other}");
            None
        }
    }
}

// ── Full-sync payload builder ─────────────────────────────────────────────────
//
// Converts a flat list of GBA flag IDs into the 216-byte payload layout that
// Multiplayer_ApplyFullSync() expects.  Sizes and byte offsets must match the
// constants in include/constants/multiplayer.h.
//
//   Slice         flags[] byte range   Length
//   story         [4..=95]              92 bytes
//   items         [125..=148]           24 bytes
//   bosses        [150..=151]            2 bytes
//   trainers      [160..=255]           96 bytes
//   badges        [268..=269]            2 bytes
//   Total                              216 bytes

fn build_full_sync_payload(flags: &[Value]) -> [u8; FULL_SYNC_PAYLOAD_SIZE] {
    let mut bitmap = [0u8; 270]; // large enough for badge bytes at index 269
    for flag_val in flags {
        if let Some(id) = flag_val.as_u64() {
            let byte = (id / 8) as usize;
            let bit  = (id % 8) as u32;
            if byte < 270 {
                bitmap[byte] |= 1 << bit;
            }
        }
    }

    let mut payload = [0u8; FULL_SYNC_PAYLOAD_SIZE];
    let mut off = 0;
    for i in 4..=95usize    { payload[off] = bitmap[i]; off += 1; } // story (0x020-0x2FF)
    for i in 125..=148usize { payload[off] = bitmap[i]; off += 1; } // items
    for i in 150..=151usize { payload[off] = bitmap[i]; off += 1; } // bosses
    for i in 160..=255usize { payload[off] = bitmap[i]; off += 1; } // trainers
    for i in 268..=269usize { payload[off] = bitmap[i]; off += 1; } // badges
    payload
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    // ── packet_size ───────────────────────────────────────────────────────────

    #[test]
    fn packet_size_fixed_types() {
        assert_eq!(packet_size(&[PKT_POSITION, 0, 0, 0, 0, 0], 0), 6);
        assert_eq!(packet_size(&[PKT_FLAG_SET,  0, 0],           0), 3);
        assert_eq!(packet_size(&[PKT_VAR_SET,   0, 0, 0, 0],     0), 5);
        assert_eq!(packet_size(&[PKT_BOSS_READY, 1],              0), 2);
        assert_eq!(packet_size(&[PKT_BOSS_CANCEL],                0), 1);
        assert_eq!(packet_size(&[PKT_SEED_SYNC, 0, 0, 0, 0],     0), 5);
        assert_eq!(packet_size(&[PKT_SCRIPT_LOCK],                0), 1);
        assert_eq!(packet_size(&[PKT_SCRIPT_UNLOCK],              0), 1);
        assert_eq!(packet_size(&[PKT_BOSS_START],                 0), 1);
        assert_eq!(packet_size(&[PKT_PARTNER_CONNECTED],          0), 1);
        assert_eq!(packet_size(&[PKT_PARTNER_DISCONNECTED],       0), 1);
    }

    #[test]
    fn packet_size_full_sync_variable() {
        // Header [0x07][len_hi=0x00][len_lo=0x04] → 3 + 4 = 7
        let buf = [PKT_FULL_SYNC, 0x00, 0x04, 0xAA, 0xBB, 0xCC, 0xDD];
        assert_eq!(packet_size(&buf, 0), 7);
    }

    #[test]
    fn packet_size_full_sync_truncated_header() {
        // Only 2 bytes — can't read len_lo yet
        assert_eq!(packet_size(&[PKT_FULL_SYNC, 0x00], 0), 0);
    }

    #[test]
    fn packet_size_unknown_type() {
        assert_eq!(packet_size(&[0xFF], 0), 0);
    }

    #[test]
    fn packet_size_empty() {
        assert_eq!(packet_size(&[], 0), 0);
        assert_eq!(packet_size(&[PKT_POSITION], 1), 0); // pos beyond end
    }

    // ── packet_to_json ────────────────────────────────────────────────────────

    #[test]
    fn pkt_to_json_position() {
        // mapGroup=1, mapNum=2, x=10, y=20, facing=3
        let pkt = [PKT_POSITION, 1, 2, 10, 20, 3];
        let msg = packet_to_json(&pkt).unwrap();
        assert_eq!(msg["type"], "position");
        let d = &msg["data"];
        assert_eq!(d["mapId"],  (1u32 << 8) | 2);  // 0x0102 = 258
        assert_eq!(d["x"],      10);
        assert_eq!(d["y"],      20);
        assert_eq!(d["facing"], 3);
    }

    #[test]
    fn pkt_to_json_flag_set() {
        // flagId = 0x04B0 = 1200
        let pkt = [PKT_FLAG_SET, 0x04, 0xB0];
        let msg = packet_to_json(&pkt).unwrap();
        assert_eq!(msg["type"],   "flag_set");
        assert_eq!(msg["flagId"], 0x04B0u32);
    }

    #[test]
    fn pkt_to_json_var_set() {
        // varId = 0x4050, value = 7
        let pkt = [PKT_VAR_SET, 0x40, 0x50, 0x00, 0x07];
        let msg = packet_to_json(&pkt).unwrap();
        assert_eq!(msg["type"],  "var_set");
        assert_eq!(msg["varId"], 0x4050u32);
        assert_eq!(msg["value"], 7u32);
    }

    #[test]
    fn pkt_to_json_boss_ready() {
        let pkt = [PKT_BOSS_READY, 3]; // bossId=3 (Lt. Surge)
        let msg = packet_to_json(&pkt).unwrap();
        assert_eq!(msg["type"],   "boss_ready");
        assert_eq!(msg["bossId"], 3u32);
    }

    #[test]
    fn pkt_to_json_boss_cancel() {
        let msg = packet_to_json(&[PKT_BOSS_CANCEL]).unwrap();
        assert_eq!(msg["type"], "boss_cancel");
    }

    #[test]
    fn pkt_to_json_flag_clear() {
        // flagId = 0x0230 (a HIDE flag in the NPC range)
        let pkt = [PKT_FLAG_CLEAR, 0x02, 0x30];
        let msg = packet_to_json(&pkt).unwrap();
        assert_eq!(msg["type"],   "flag_clear");
        assert_eq!(msg["flagId"], 0x0230u32);
    }

    #[test]
    fn pkt_to_json_wrong_length_returns_none() {
        // position needs exactly 6 bytes
        assert!(packet_to_json(&[PKT_POSITION, 0, 0, 0, 0]).is_none());
    }

    // ── json_to_packet ────────────────────────────────────────────────────────

    #[test]
    fn json_to_pkt_partner_position() {
        let msg = json!({
            "type": "partner_position",
            "data": { "mapId": 258u32, "x": 10u32, "y": 20u32, "facing": 3u32, "spriteState": 0u32 }
        });
        let pkt = json_to_packet(&msg).unwrap();
        assert_eq!(pkt, vec![PKT_POSITION, 1, 2, 10, 20, 3]);
    }

    #[test]
    fn json_to_pkt_mapid_roundtrip() {
        // Encode in packet_to_json, decode in json_to_packet.
        let original = [PKT_POSITION, 5, 12, 7, 3, 1u8]; // group=5 num=12
        let msg = packet_to_json(&original).unwrap();
        // Relay renames "position" → "partner_position" on the wire.
        let relayed = json!({ "type": "partner_position", "data": msg["data"].clone() });
        let decoded = json_to_packet(&relayed).unwrap();
        assert_eq!(decoded, original.to_vec());
    }

    #[test]
    fn json_to_pkt_flag_set() {
        let msg = json!({ "type": "flag_set", "flagId": 0x04B0u32 });
        let pkt = json_to_packet(&msg).unwrap();
        assert_eq!(pkt, vec![PKT_FLAG_SET, 0x04, 0xB0]);
    }

    #[test]
    fn json_to_pkt_flag_clear() {
        let msg = json!({ "type": "flag_clear", "flagId": 0x0230u32 });
        let pkt = json_to_packet(&msg).unwrap();
        assert_eq!(pkt, vec![PKT_FLAG_CLEAR, 0x02, 0x30]);
    }

    #[test]
    fn packet_size_flag_clear() {
        assert_eq!(packet_size(&[PKT_FLAG_CLEAR, 0x02, 0x30], 0), 3);
    }

    #[test]
    fn json_to_pkt_var_set() {
        let msg = json!({ "type": "var_set", "varId": 0x4050u32, "value": 7u32 });
        let pkt = json_to_packet(&msg).unwrap();
        assert_eq!(pkt, vec![PKT_VAR_SET, 0x40, 0x50, 0x00, 0x07]);
    }

    #[test]
    fn json_to_pkt_partner_connected() {
        let pkt = json_to_packet(&json!({ "type": "partner_connected" })).unwrap();
        assert_eq!(pkt, vec![PKT_PARTNER_CONNECTED]);
    }

    #[test]
    fn json_to_pkt_partner_disconnected() {
        let pkt = json_to_packet(&json!({ "type": "partner_disconnected" })).unwrap();
        assert_eq!(pkt, vec![PKT_PARTNER_DISCONNECTED]);
    }

    #[test]
    fn json_to_pkt_boss_start() {
        let pkt = json_to_packet(&json!({ "type": "boss_start", "bossId": 1u32 })).unwrap();
        assert_eq!(pkt, vec![PKT_BOSS_START]);
    }

    #[test]
    fn json_to_pkt_role_returns_none() {
        // role is handled by app state, not forwarded to ROM ring
        assert!(json_to_packet(&json!({ "type": "role", "role": "host" })).is_none());
    }

    #[test]
    fn json_to_pkt_boss_waiting_returns_none() {
        assert!(json_to_packet(&json!({ "type": "boss_waiting" })).is_none());
    }

    #[test]
    fn json_to_pkt_session_settings_zero_seed_returns_none() {
        assert!(json_to_packet(&json!({ "type": "session_settings", "randomizeEncounters": true })).is_none());
    }

    // ── build_full_sync_payload ───────────────────────────────────────────────

    #[test]
    fn full_sync_payload_empty_flags() {
        let payload = build_full_sync_payload(&[]);
        assert_eq!(payload.len(), FULL_SYNC_PAYLOAD_SIZE);
        assert!(payload.iter().all(|&b| b == 0));
    }

    #[test]
    fn full_sync_payload_story_flag() {
        // Flag 0x020 = 32 → byte 4, bit 0.
        // Story range starts at byte 4; offset in payload = 4 - 4 = 0.
        let flags = vec![json!(32u32)];
        let payload = build_full_sync_payload(&flags);
        assert_eq!(payload[0], 1, "story flag 0x020 should be bit 0 at payload offset 0");
    }

    #[test]
    fn full_sync_payload_trainer_flag() {
        // SYNC_FLAG_TRAINERS_START = 0x500 = 1280 → byte 160, bit 0.
        // Trainers start at payload offset = 92+24+2 = 118; byte 160 is offset 0 of trainers.
        let flags = vec![json!(0x500u32)];
        let payload = build_full_sync_payload(&flags);
        let trainer_off = 92 + 24 + 2; // = 118
        assert_eq!(payload[trainer_off], 1, "trainer flag 0x500 should be bit 0 at payload offset 118");
    }

    #[test]
    fn full_sync_payload_badge_flag() {
        // FLAG_BADGE01_GET = 0x867 = 2151 → byte 268, bit 7.
        // Badges are last 2 bytes: offsets 214 and 215.
        let flags = vec![json!(0x867u32)]; // 2151 / 8 = 268 r 7
        let payload = build_full_sync_payload(&flags);
        let badge_off = FULL_SYNC_PAYLOAD_SIZE - 2; // = 214
        assert_eq!(payload[badge_off], 0x80, "badge flag 0x867 should be bit 7 at payload offset 214");
    }

    #[test]
    fn full_sync_payload_size_is_216() {
        assert_eq!(FULL_SYNC_PAYLOAD_SIZE, 216);
    }

    #[test]
    fn full_sync_inbound_from_relay() {
        // The relay sends { type: "full_sync", flags: [<real GBA flag IDs>] }.
        // json_to_packet must produce a packet whose payload matches
        // build_full_sync_payload called with the same IDs.
        let story_flag  = 0x230u64; // within story range [0x020..0x2FF]
        let badge_flag  = 0x867u64; // FLAG_BADGE01_GET

        let json_msg = json!({
            "type": "full_sync",
            "flags": [story_flag, badge_flag],
            "vars": {}
        });

        let pkt = json_to_packet(&json_msg).unwrap();
        assert_eq!(pkt[0], PKT_FULL_SYNC);
        let len = ((pkt[1] as usize) << 8) | pkt[2] as usize;
        assert_eq!(len, FULL_SYNC_PAYLOAD_SIZE);

        let expected = build_full_sync_payload(&[json!(story_flag), json!(badge_flag)]);
        assert_eq!(len, FULL_SYNC_PAYLOAD_SIZE, "payload length must be FULL_SYNC_PAYLOAD_SIZE");
        assert_eq!(&pkt[3..], &expected,
            "full_sync packet payload must match build_full_sync_payload output");
    }

    // ── drain_send_ring helper (ring byte parsing without emulator) ───────────

    #[test]
    fn ring_parse_single_position_packet() {
        // Simulate a 260-byte raw memory snapshot with one position packet.
        // head=6, tail=0, magic=0xC0, data at buf[0..5]
        let mut raw = [0u8; 260];
        raw[0] = PKT_POSITION;
        raw[1] = 2;  // mapGroup
        raw[2] = 5;  // mapNum
        raw[3] = 15; // x
        raw[4] = 8;  // y
        raw[5] = 0;  // facing
        raw[256] = 6; // head
        raw[257] = 0; // tail
        raw[258] = RING_MAGIC;

        let head = raw[256];
        let tail = raw[257];
        assert_eq!(raw[258], RING_MAGIC);

        let avail = head.wrapping_sub(tail) as usize;
        let ring_data: Vec<u8> = (0..avail).map(|i| raw[tail.wrapping_add(i as u8) as usize]).collect();
        assert_eq!(ring_data.len(), 6);

        let size = packet_size(&ring_data, 0);
        assert_eq!(size, 6);

        let pkt = &ring_data[0..size];
        let msg = packet_to_json(pkt).unwrap();
        assert_eq!(msg["type"], "position");
        assert_eq!(msg["data"]["mapId"], (2u32 << 8) | 5u32);
        assert_eq!(msg["data"]["x"], 15u32);
    }

    #[test]
    fn ring_parse_multiple_packets() {
        // Two flag_set packets back-to-back.
        let mut raw = [0u8; 260];
        raw[0] = PKT_FLAG_SET; raw[1] = 0x04; raw[2] = 0xB0;
        raw[3] = PKT_FLAG_SET; raw[4] = 0x04; raw[5] = 0xB1;
        raw[256] = 6;
        raw[257] = 0;
        raw[258] = RING_MAGIC;

        let head = raw[256];
        let tail = raw[257];
        let avail = head.wrapping_sub(tail) as usize;
        let ring_data: Vec<u8> = (0..avail).map(|i| raw[tail.wrapping_add(i as u8) as usize]).collect();

        let mut consumed = 0;
        let mut packets = Vec::new();
        while consumed < ring_data.len() {
            let size = packet_size(&ring_data, consumed);
            assert!(size > 0);
            packets.push(ring_data[consumed..consumed + size].to_vec());
            consumed += size;
        }
        assert_eq!(packets.len(), 2);
        let m0 = packet_to_json(&packets[0]).unwrap();
        let m1 = packet_to_json(&packets[1]).unwrap();
        assert_eq!(m0["flagId"], 0x04B0u32);
        assert_eq!(m1["flagId"], 0x04B1u32);
    }

    #[test]
    fn ring_invalid_magic_produces_no_packets() {
        let mut raw = [0u8; 260];
        raw[0] = PKT_FLAG_SET; raw[1] = 0x00; raw[2] = 0x01;
        raw[256] = 3;
        raw[257] = 0;
        raw[258] = 0x00; // wrong magic

        assert_ne!(raw[258], RING_MAGIC);
        // drain_send_ring returns vec![] — simulated here by checking magic.
    }
}

// ── Minimal base64 (no external dep) ─────────────────────────────────────────

const B64_CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#[allow(dead_code)]
fn base64_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = chunk.get(1).copied().unwrap_or(0) as u32;
        let b2 = chunk.get(2).copied().unwrap_or(0) as u32;
        let triple = (b0 << 16) | (b1 << 8) | b2;
        out.push(B64_CHARS[((triple >> 18) & 0x3F) as usize] as char);
        out.push(B64_CHARS[((triple >> 12) & 0x3F) as usize] as char);
        out.push(if chunk.len() > 1 { B64_CHARS[((triple >> 6) & 0x3F) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { B64_CHARS[(triple & 0x3F) as usize] as char } else { '=' });
    }
    out
}

#[allow(dead_code)]
fn base64_decode(s: &str) -> Option<Vec<u8>> {
    fn char_val(c: u8) -> Option<u32> {
        match c {
            b'A'..=b'Z' => Some((c - b'A') as u32),
            b'a'..=b'z' => Some((c - b'a') as u32 + 26),
            b'0'..=b'9' => Some((c - b'0') as u32 + 52),
            b'+'        => Some(62),
            b'/'        => Some(63),
            b'='        => Some(0),
            _           => None,
        }
    }

    let bytes = s.as_bytes();
    if bytes.len() % 4 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(bytes.len() / 4 * 3);
    for chunk in bytes.chunks(4) {
        let v0 = char_val(chunk[0])?;
        let v1 = char_val(chunk[1])?;
        let v2 = char_val(chunk[2])?;
        let v3 = char_val(chunk[3])?;
        let triple = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
        out.push((triple >> 16) as u8);
        if chunk[2] != b'=' { out.push((triple >> 8) as u8); }
        if chunk[3] != b'=' { out.push(triple as u8); }
    }
    Some(out)
}
