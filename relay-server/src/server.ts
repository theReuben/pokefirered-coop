import type * as Party from "partykit/server";

// ─── Types ───────────────────────────────────────────────────────────────────

type PlayerID = "host" | "guest";

export interface PlayerState {
  mapId: number;
  x: number;
  y: number;
  facing: number;  // 0-3: down, up, left, right
  spriteState: number;
}

interface RoomState {
  sessionId: string | null;
  randomizeEncounters: boolean;
  positions: Partial<Record<PlayerID, PlayerState>>;
  flags: Set<number>;
  vars: Map<number, number>;
  bossReady: { host: number | null; guest: number | null };
  starters: { host: number | null; guest: number | null };
  partyData: Partial<Record<PlayerID, string>>;
}

// ─── Message types (ROM/Tauri ↔ Server) ──────────────────────────────────────

// Client → Server (sent by Tauri app on behalf of ROM)
export type ClientMessage =
  | { type: "position"; data: PlayerState }
  | { type: "flag_set"; flagId: number }
  | { type: "var_set"; varId: number; value: number }
  | { type: "boss_ready"; bossId: number }
  | { type: "boss_cancel" }
  | { type: "battle_turn"; turnData: string }       // base64-encoded turn selection
  | { type: "party_sync"; partyData: string }       // base64-encoded party snapshot
  | { type: "starter_pick"; speciesId: number }
  | { type: "session_settings"; randomizeEncounters: boolean };

// Server → Client (received by Tauri app, forwarded to ROM serial buffer)
export type ServerMessage =
  | { type: "role"; role: PlayerID }
  | { type: "partner_position"; data: PlayerState }
  | { type: "flag_set"; flagId: number }
  | { type: "var_set"; varId: number; value: number }
  | { type: "full_sync"; flags: number[]; vars: Record<number, number> }
  | { type: "boss_start"; bossId: number }
  | { type: "boss_waiting" }
  | { type: "battle_turn"; turnData: string }
  | { type: "party_sync"; partyData: string }
  | { type: "partner_connected" }
  | { type: "partner_disconnected" }
  | { type: "room_full" }
  | { type: "session_mismatch" }
  | { type: "starter_taken"; speciesId: number }
  | { type: "starter_denied"; speciesId: number }
  | { type: "session_settings"; randomizeEncounters: boolean };

// ─── Server ───────────────────────────────────────────────────────────────────

export default class PokemonCoopServer implements Party.Server {
  private state: RoomState = {
    sessionId: null,
    randomizeEncounters: true,
    positions: {},
    flags: new Set(),
    vars: new Map(),
    bossReady: { host: null, guest: null },
    starters: { host: null, guest: null },
    partyData: {},
  };

  // connectionId → role; in-memory, resets on server restart
  private roles = new Map<string, PlayerID>();

  constructor(readonly room: Party.Room) {}

  onConnect(conn: Party.Connection, ctx: Party.ConnectionContext): void {
    // ── Session ID validation ──────────────────────────────────────────────
    const url = new URL(ctx.request.url);
    const incomingSessionId = url.searchParams.get("session_id");

    if (incomingSessionId) {
      if (this.state.sessionId === null) {
        // First player establishes the session for this room
        this.state.sessionId = incomingSessionId;
      } else if (this.state.sessionId !== incomingSessionId) {
        // Wrong save file — this player's .coop sidecar doesn't match
        this.send(conn, { type: "session_mismatch" });
        conn.close();
        return;
      }
    }

    // ── Capacity check ────────────────────────────────────────────────────
    // Use roles.size (not getConnections().length) to avoid counting this
    // connection before we decide whether to accept it.
    if (this.roles.size >= 2) {
      this.send(conn, { type: "room_full" });
      conn.close();
      return;
    }

    // ── Role assignment ───────────────────────────────────────────────────
    const role: PlayerID = this.roles.size === 0 ? "host" : "guest";
    this.roles.set(conn.id, role);

    // ── Initial state dump ────────────────────────────────────────────────
    this.send(conn, { type: "role", role });

    this.send(conn, {
      type: "full_sync",
      flags: [...this.state.flags],
      vars: Object.fromEntries(this.state.vars),
    });

    // Guests always mirror host's session settings
    if (role === "guest") {
      this.send(conn, {
        type: "session_settings",
        randomizeEncounters: this.state.randomizeEncounters,
      });
    }

    // Relay partner's last known position
    const partnerRole: PlayerID = role === "host" ? "guest" : "host";
    const partnerPos = this.state.positions[partnerRole];
    if (partnerPos !== undefined) {
      this.send(conn, { type: "partner_position", data: partnerPos });
    }

    // Relay partner's party data (needed for double battle display)
    const partnerParty = this.state.partyData[partnerRole];
    if (partnerParty !== undefined) {
      this.send(conn, { type: "party_sync", partyData: partnerParty });
    }

    // Inform about any starter already claimed by the partner
    const partnerStarter = this.state.starters[partnerRole];
    if (partnerStarter !== null) {
      this.send(conn, { type: "starter_taken", speciesId: partnerStarter });
    }

    // Notify the other player
    this.broadcast(conn, { type: "partner_connected" });
  }

  onClose(conn: Party.Connection): void {
    const role = this.roles.get(conn.id);
    if (role !== undefined) {
      delete this.state.positions[role];
      delete this.state.partyData[role];
      this.state.bossReady[role] = null;
      this.roles.delete(conn.id);
    }
    this.broadcast(conn, { type: "partner_disconnected" });
  }

  onMessage(message: string, sender: Party.Connection): void {
    const role = this.roles.get(sender.id);
    if (role === undefined) return;

    let msg: ClientMessage;
    try {
      msg = JSON.parse(message) as ClientMessage;
    } catch {
      return; // malformed JSON — silently drop
    }

    switch (msg.type) {
      case "position":
        this.state.positions[role] = msg.data;
        this.broadcast(sender, { type: "partner_position", data: msg.data });
        break;

      case "flag_set":
        // Idempotent: broadcast only on first set
        if (!this.state.flags.has(msg.flagId)) {
          this.state.flags.add(msg.flagId);
          this.broadcastAll({ type: "flag_set", flagId: msg.flagId });
        }
        break;

      case "var_set":
        this.state.vars.set(msg.varId, msg.value);
        this.broadcastAll({ type: "var_set", varId: msg.varId, value: msg.value });
        break;

      case "boss_ready": {
        this.state.bossReady[role] = msg.bossId;
        const { host: hostBoss, guest: guestBoss } = this.state.bossReady;
        if (hostBoss !== null && guestBoss !== null && hostBoss === guestBoss) {
          // Both players at the same boss — start the battle
          this.broadcastAll({ type: "boss_start", bossId: hostBoss });
          this.state.bossReady = { host: null, guest: null };
        } else {
          // Still waiting for the partner (or boss IDs don't match yet)
          this.send(sender, { type: "boss_waiting" });
        }
        break;
      }

      case "boss_cancel":
        this.state.bossReady[role] = null;
        break;

      case "battle_turn":
        this.broadcast(sender, { type: "battle_turn", turnData: msg.turnData });
        break;

      case "party_sync":
        this.state.partyData[role] = msg.partyData;
        this.broadcast(sender, { type: "party_sync", partyData: msg.partyData });
        break;

      case "starter_pick": {
        // Already claimed — idempotent, ignore
        if (this.state.starters[role] !== null) break;

        const partnerRole: PlayerID = role === "host" ? "guest" : "host";
        if (this.state.starters[partnerRole] === msg.speciesId) {
          // Conflict: partner took this one first
          this.send(sender, { type: "starter_denied", speciesId: msg.speciesId });
          break;
        }

        this.state.starters[role] = msg.speciesId;
        this.broadcast(sender, { type: "starter_taken", speciesId: msg.speciesId });
        break;
      }

      case "session_settings":
        // Only the host controls session settings
        if (role === "host") {
          this.state.randomizeEncounters = msg.randomizeEncounters;
          this.broadcast(sender, {
            type: "session_settings",
            randomizeEncounters: msg.randomizeEncounters,
          });
        }
        break;
    }
  }

  // ─── Helpers ──────────────────────────────────────────────────────────────

  private send(conn: Party.Connection, msg: ServerMessage): void {
    conn.send(JSON.stringify(msg));
  }

  /** Relay to everyone except the sender. */
  private broadcast(sender: Party.Connection, msg: ServerMessage): void {
    for (const conn of this.room.getConnections()) {
      if (conn.id !== sender.id) {
        conn.send(JSON.stringify(msg));
      }
    }
  }

  /** Send to all connections including the sender. */
  private broadcastAll(msg: ServerMessage): void {
    for (const conn of this.room.getConnections()) {
      conn.send(JSON.stringify(msg));
    }
  }
}
