/// Serial Bridge — translates between the GBA serial ring buffers in ROM memory
/// and JSON messages sent/received by the WebSocket client (net.rs).
///
/// The multiplayer.c module in the ROM maintains two ring buffers in GBA EWRAM:
///
///   gMpSendRing  — ROM writes outbound packets here; Tauri reads and clears it
///   gMpRecvRing  — Tauri writes inbound packets here; ROM reads from it
///
/// The ring layout (from include/multiplayer.h):
///   offset 0:  magic u32 (0xC00PCAFE) — checked by ROM on first access
///   offset 4:  write_head u32 — next byte index to write (producer advances this)
///   offset 8:  read_head  u32 — next byte index to read  (consumer advances this)
///   offset 12: data[4096] u8  — raw packet bytes
///
/// Each packet in the ring is:
///   [u8 type][u8 len][u8 data[len]]
///
/// Packet type codes are defined in include/constants/multiplayer.h:
///   0x01 POSITION, 0x02 FLAG_SET, 0x03 VAR_SET, 0x04 FULL_SYNC,
///   0x05 BOSS_READY, 0x06 BOSS_CANCEL, 0x07 BOSS_START,
///   0x08 SCRIPT_LOCK, 0x09 SCRIPT_UNLOCK, 0x0A SEED_SYNC,
///   0x0B STARTER_PICK, 0x0C PARTY_SYNC, 0x0D BATTLE_TURN,
///   0x0E SESSION_SETTINGS

use crate::emulator::EmulatorHandle;
use crate::net::NetHandle;
use serde_json::{json, Value};

// GBA EWRAM base in the mGBA address space
const EWRAM_BASE: u32 = 0x02000000;

// These symbol addresses are set by the ROM linker map.
// They are constant for a given ROM build, but we read them from a known
// symbol table at runtime to avoid hard-coding offsets that change when
// ROM code is recompiled.
//
// The Tauri app reads the symbol table from pokefirered.map (built alongside
// pokefirered.gba) on first launch and caches the addresses.
//
// Defaults here correspond to a reference build — updated by init_addresses().
static mut SEND_RING_ADDR: u32 = 0x0203_F000;
static mut RECV_RING_ADDR: u32 = 0x0203_F800;

const RING_MAGIC:      u32 = 0xC00FCAFE;
const RING_HDR_BYTES:  u32 = 12;
const RING_DATA_SIZE:  u32 = 4096;

// ── Packet type constants (must match include/constants/multiplayer.h) ─────────

const PKT_POSITION:         u8 = 0x01;
const PKT_FLAG_SET:         u8 = 0x02;
const PKT_VAR_SET:          u8 = 0x03;
const PKT_FULL_SYNC:        u8 = 0x04;
const PKT_BOSS_READY:       u8 = 0x05;
const PKT_BOSS_CANCEL:      u8 = 0x06;
const PKT_BOSS_START:       u8 = 0x07;
const PKT_SCRIPT_LOCK:      u8 = 0x08;
const PKT_SCRIPT_UNLOCK:    u8 = 0x09;
const PKT_SEED_SYNC:        u8 = 0x0A;
const PKT_STARTER_PICK:     u8 = 0x0B;
const PKT_PARTY_SYNC:       u8 = 0x0C;
const PKT_BATTLE_TURN:      u8 = 0x0D;
const PKT_SESSION_SETTINGS: u8 = 0x0E;

/// Called once per emulated frame from the main loop.
/// Drains the ROM's send ring and forwards packets as JSON to the relay.
/// Drains the relay's inbound queue and writes packets into the ROM's recv ring.
pub fn tick(emu: &mut EmulatorHandle, net: &NetHandle) {
    // ── ROM → relay ────────────────────────────────────────────────────────
    let outbound = drain_send_ring(emu);
    for pkt in outbound {
        if let Some(json) = packet_to_json(&pkt) {
            net.send(json);
        }
    }

    // ── relay → ROM ────────────────────────────────────────────────────────
    for msg in net.drain_inbound() {
        if let Some(pkt) = json_to_packet(&msg) {
            push_recv_ring(emu, &pkt);
        }
    }
}

// ── Outbound: drain the ROM's send ring ───────────────────────────────────────

fn drain_send_ring(emu: &mut EmulatorHandle) -> Vec<Vec<u8>> {
    let base = unsafe { SEND_RING_ADDR };

    // Validate magic
    if emu.read_u32(base) != RING_MAGIC {
        return vec![];
    }

    let write_head = emu.read_u32(base + 4);
    let read_head  = emu.read_u32(base + 8);

    if write_head == read_head {
        return vec![]; // ring is empty
    }

    let data_base = base + RING_HDR_BYTES;
    let mut packets = Vec::new();
    let mut rh = read_head;

    while rh != write_head {
        let ptype = emu.read_bytes(data_base + (rh % RING_DATA_SIZE), 1)[0];
        rh = (rh + 1) % RING_DATA_SIZE;

        let plen = emu.read_bytes(data_base + (rh % RING_DATA_SIZE), 1)[0] as u32;
        rh = (rh + 1) % RING_DATA_SIZE;

        let mut data = vec![0u8; plen as usize];
        for byte in data.iter_mut() {
            *byte = emu.read_bytes(data_base + (rh % RING_DATA_SIZE), 1)[0];
            rh = (rh + 1) % RING_DATA_SIZE;
        }

        let mut pkt = vec![ptype, plen as u8];
        pkt.extend_from_slice(&data);
        packets.push(pkt);
    }

    // Advance the read head so the ROM knows we consumed the data
    // Safety: only the Tauri app writes read_head; the ROM writes write_head
    emu.write_u32(base + 8, rh);

    packets
}

// ── Inbound: push a packet into the ROM's recv ring ──────────────────────────

fn push_recv_ring(emu: &mut EmulatorHandle, pkt: &[u8]) {
    let base = unsafe { RECV_RING_ADDR };

    if emu.read_u32(base) != RING_MAGIC {
        return;
    }

    let write_head = emu.read_u32(base + 4);
    let read_head  = emu.read_u32(base + 8);
    let data_base  = base + RING_HDR_BYTES;

    // Check that there's room for the full packet
    let space = RING_DATA_SIZE - (write_head.wrapping_sub(read_head) % RING_DATA_SIZE);
    if (pkt.len() as u32) > space.saturating_sub(1) {
        log::warn!("Recv ring full — dropping inbound packet type 0x{:02X}", pkt[0]);
        return;
    }

    let mut wh = write_head;
    for &byte in pkt {
        emu.write_bytes(data_base + (wh % RING_DATA_SIZE), &[byte]);
        wh = (wh + 1) % RING_DATA_SIZE;
    }

    emu.write_u32(base + 4, wh);
}

// ── Packet ↔ JSON translation ─────────────────────────────────────────────────

fn packet_to_json(pkt: &[u8]) -> Option<Value> {
    if pkt.len() < 2 { return None; }
    let ptype = pkt[0];
    let data  = &pkt[2..];

    match ptype {
        PKT_POSITION if data.len() >= 8 => {
            let map_id     = u16::from_le_bytes([data[0], data[1]]) as u32;
            let x          = u16::from_le_bytes([data[2], data[3]]) as u32;
            let y          = u16::from_le_bytes([data[4], data[5]]) as u32;
            let facing     = data[6] as u32;
            let sprite_state = data[7] as u32;
            Some(json!({
                "type": "position",
                "data": { "mapId": map_id, "x": x, "y": y, "facing": facing, "spriteState": sprite_state }
            }))
        }

        PKT_FLAG_SET if data.len() >= 2 => {
            let flag_id = u16::from_le_bytes([data[0], data[1]]) as u32;
            Some(json!({ "type": "flag_set", "flagId": flag_id }))
        }

        PKT_VAR_SET if data.len() >= 4 => {
            let var_id = u16::from_le_bytes([data[0], data[1]]) as u32;
            let value  = u16::from_le_bytes([data[2], data[3]]) as u32;
            Some(json!({ "type": "var_set", "varId": var_id, "value": value }))
        }

        PKT_FULL_SYNC if !data.is_empty() => {
            // data is a raw byte bitmap of set flags; expand to a list of flag IDs
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

        PKT_BOSS_READY if data.len() >= 1 => {
            Some(json!({ "type": "boss_ready", "bossId": data[0] as u32 }))
        }

        PKT_BOSS_CANCEL => {
            Some(json!({ "type": "boss_cancel" }))
        }

        PKT_SEED_SYNC if data.len() >= 4 => {
            let seed = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
            Some(json!({ "type": "session_settings", "encounterSeed": seed }))
        }

        PKT_STARTER_PICK if data.len() >= 2 => {
            let species_id = u16::from_le_bytes([data[0], data[1]]) as u32;
            Some(json!({ "type": "starter_pick", "speciesId": species_id }))
        }

        PKT_PARTY_SYNC if !data.is_empty() => {
            let encoded = base64_encode(data);
            Some(json!({ "type": "party_sync", "partyData": encoded }))
        }

        PKT_BATTLE_TURN if !data.is_empty() => {
            let encoded = base64_encode(data);
            Some(json!({ "type": "battle_turn", "turnData": encoded }))
        }

        PKT_SESSION_SETTINGS if data.len() >= 1 => {
            Some(json!({ "type": "session_settings", "randomizeEncounters": data[0] != 0 }))
        }

        _ => {
            log::warn!("Unknown/malformed outbound packet type 0x{:02X}", ptype);
            None
        }
    }
}

fn json_to_packet(msg: &Value) -> Option<Vec<u8>> {
    let msg_type = msg.get("type")?.as_str()?;

    match msg_type {
        "role" => {
            let role: u8 = if msg.get("role")?.as_str()? == "host" { 0 } else { 1 };
            Some(make_pkt(PKT_SESSION_SETTINGS, &[role]))
        }

        "partner_position" => {
            let d = msg.get("data")?;
            let map_id = d.get("mapId")?.as_u64()? as u16;
            let x      = d.get("x")?.as_u64()? as u16;
            let y      = d.get("y")?.as_u64()? as u16;
            let facing = d.get("facing")?.as_u64()? as u8;
            let sprite  = d.get("spriteState")?.as_u64()? as u8;
            let mut data = Vec::with_capacity(8);
            data.extend_from_slice(&map_id.to_le_bytes());
            data.extend_from_slice(&x.to_le_bytes());
            data.extend_from_slice(&y.to_le_bytes());
            data.push(facing);
            data.push(sprite);
            Some(make_pkt(PKT_POSITION, &data))
        }

        "flag_set" => {
            let flag_id = msg.get("flagId")?.as_u64()? as u16;
            Some(make_pkt(PKT_FLAG_SET, &flag_id.to_le_bytes()))
        }

        "var_set" => {
            let var_id = msg.get("varId")?.as_u64()? as u16;
            let value  = msg.get("value")?.as_u64()? as u16;
            let mut data = [0u8; 4];
            data[0..2].copy_from_slice(&var_id.to_le_bytes());
            data[2..4].copy_from_slice(&value.to_le_bytes());
            Some(make_pkt(PKT_VAR_SET, &data))
        }

        "full_sync" => {
            // flags is a JSON array of flag IDs; pack into a byte bitmap
            let flags = msg.get("flags")?.as_array()?;
            let mut bitmap = vec![0u8; 256]; // covers up to flag 0x7FF (2048 flags / 8)
            for flag_val in flags {
                if let Some(id) = flag_val.as_u64() {
                    let byte = (id / 8) as usize;
                    let bit  = id % 8;
                    if byte < bitmap.len() {
                        bitmap[byte] |= 1 << bit;
                    }
                }
            }
            Some(make_pkt(PKT_FULL_SYNC, &bitmap))
        }

        "boss_start" => {
            let boss_id = msg.get("bossId")?.as_u64()? as u8;
            Some(make_pkt(PKT_BOSS_START, &[boss_id]))
        }

        "boss_waiting" => {
            // Reuse BOSS_START pkt with id=0xFF to signal "still waiting"
            Some(make_pkt(PKT_BOSS_START, &[0xFF]))
        }

        "session_settings" => {
            let randomize = msg.get("randomizeEncounters")?.as_bool().unwrap_or(true);
            let seed = msg.get("encounterSeed").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
            let mut data = vec![randomize as u8];
            data.extend_from_slice(&seed.to_le_bytes());
            Some(make_pkt(PKT_SESSION_SETTINGS, &data))
        }

        "starter_taken" => {
            let species = msg.get("speciesId")?.as_u64()? as u16;
            Some(make_pkt(PKT_STARTER_PICK, &species.to_le_bytes()))
        }

        "party_sync" => {
            let encoded = msg.get("partyData")?.as_str()?;
            let data = base64_decode(encoded)?;
            Some(make_pkt(PKT_PARTY_SYNC, &data))
        }

        "battle_turn" => {
            let encoded = msg.get("turnData")?.as_str()?;
            let data = base64_decode(encoded)?;
            Some(make_pkt(PKT_BATTLE_TURN, &data))
        }

        "partner_connected" | "partner_disconnected" | "room_full"
        | "session_mismatch" | "starter_denied" => {
            // These are informational only; the ROM doesn't need to see them as packets.
            // The Tauri frontend handles room_full/session_mismatch by navigating away.
            None
        }

        other => {
            log::debug!("Unhandled inbound message type: {other}");
            None
        }
    }
}

fn make_pkt(ptype: u8, data: &[u8]) -> Vec<u8> {
    let mut pkt = vec![ptype, data.len() as u8];
    pkt.extend_from_slice(data);
    pkt
}

// ── Minimal base64 (no external dep needed — data is small) ──────────────────

const B64_CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
    if bytes.len() % 4 != 0 { return None; }

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
