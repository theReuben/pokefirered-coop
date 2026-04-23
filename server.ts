import type * as Party from "partykit/server";

// ─── Types ───────────────────────────────────────────────────────────────

type PlayerID = "host" | "guest";

interface PlayerState {
  mapId: number;
  x: number;
  y: number;
  facing: number; // 0-3: down, up, left, right
  spriteState: number;
}

interface RoomState {
  players: Partial<Record<PlayerID, PlayerState>>;
  flags: number[]; // shared trainer/story flags (bitfield indices that are SET)
  vars: Record<number, number>; // shared script variables
  bossReady: { host: boolean; guest: boolean };
}

// ─── Message types (ROM <-> Server) ─────────────────────────────────────

// Inbound from ROM client
type ClientMessage =
  | { type: "position"; data: PlayerState }
  | { type: "flag_set"; flagId: number }
  | { type: "var_set"; varId: number; value: number }
  | { type: "boss_ready"; bossId: number }
  | { type: "boss_cancel" }
  | { type: "battle_turn"; turnData: string }; // base64 encoded turn selection

// Outbound to ROM client
type ServerMessage =
  | { type: "role"; role: PlayerID } // tells client if they're host or guest
  | { type: "partner_position"; data: PlayerState }
  | { type: "flag_set"; flagId: number } // broadcast flag change
  | { type: "var_set"; varId: number; value: number }
  | { type: "full_sync"; flags: number[]; vars: Record<number, number> }
  | { type: "boss_start"; bossId: number } // both ready, begin battle
  | { type: "boss_waiting" } // you're ready, partner isn't
  | { type: "battle_turn"; turnData: string } // relay partner's turn
  | { type: "partner_connected" }
  | { type: "partner_disconnected" }
  | { type: "room_full" };

// ─── Server ──────────────────────────────────────────────────────────────

export default class PokemonCoopServer implements Party.Server {
  state: RoomState;
  roles: Map<string, PlayerID>; // connectionId -> role

  constructor(readonly room: Party.Room) {
    this.state = {
      players: {},
      flags: [],
      vars: {},
      bossReady: { host: false, guest: false },
    };
    this.roles = new Map();
  }

  onConnect(conn: Party.Connection) {
    // First player is host, second is guest, reject further connections
    const currentCount = [...this.room.getConnections()].length;

    if (currentCount > 2) {
      this.send(conn, { type: "room_full" });
      conn.close();
      return;
    }

    const role: PlayerID = currentCount <= 1 ? "host" : "guest";
    this.roles.set(conn.id, role);

    // Tell the client their role
    this.send(conn, { type: "role", role });

    // Send full flag/var state so late-joiner syncs up
    this.send(conn, {
      type: "full_sync",
      flags: this.state.flags,
      vars: this.state.vars,
    });

    // Notify the other player
    this.broadcast(conn, { type: "partner_connected" });

    // If partner already has a position, send it
    const partnerRole = role === "host" ? "guest" : "host";
    const partnerPos = this.state.players[partnerRole];
    if (partnerPos) {
      this.send(conn, { type: "partner_position", data: partnerPos });
    }
  }

  onClose(conn: Party.Connection) {
    const role = this.roles.get(conn.id);
    if (role) {
      delete this.state.players[role];
      this.roles.delete(conn.id);
    }
    this.broadcast(conn, { type: "partner_disconnected" });
  }

  onMessage(message: string, sender: Party.Connection) {
    const role = this.roles.get(sender.id);
    if (!role) return;

    const msg: ClientMessage = JSON.parse(message);

    switch (msg.type) {
      case "position":
        // Store locally, relay to partner
        this.state.players[role] = msg.data;
        this.broadcast(sender, { type: "partner_position", data: msg.data });
        break;

      case "flag_set":
        // Idempotent — add flag if not already set, broadcast to both
        if (!this.state.flags.includes(msg.flagId)) {
          this.state.flags.push(msg.flagId);
          // Broadcast to ALL including sender (confirms the set)
          this.broadcastAll({ type: "flag_set", flagId: msg.flagId });
        }
        break;

      case "var_set":
        this.state.vars[msg.varId] = msg.value;
        this.broadcastAll({ type: "var_set", varId: msg.varId, value: msg.value });
        break;

      case "boss_ready":
        this.state.bossReady[role] = true;
        if (this.state.bossReady.host && this.state.bossReady.guest) {
          // Both ready — tell both to start the battle
          this.broadcastAll({ type: "boss_start", bossId: msg.bossId });
          this.state.bossReady = { host: false, guest: false };
        } else {
          this.send(sender, { type: "boss_waiting" });
        }
        break;

      case "boss_cancel":
        this.state.bossReady[role] = false;
        break;

      case "battle_turn":
        // Relay turn data to the other player
        this.broadcast(sender, { type: "battle_turn", turnData: msg.turnData });
        break;
    }
  }

  // ─── Helpers ─────────────────────────────────────────────────────────

  /** Send to one connection */
  send(conn: Party.Connection, msg: ServerMessage) {
    conn.send(JSON.stringify(msg));
  }

  /** Send to everyone EXCEPT sender */
  broadcast(sender: Party.Connection, msg: ServerMessage) {
    for (const conn of this.room.getConnections()) {
      if (conn.id !== sender.id) {
        conn.send(JSON.stringify(msg));
      }
    }
  }

  /** Send to ALL connections including sender */
  broadcastAll(msg: ServerMessage) {
    for (const conn of this.room.getConnections()) {
      conn.send(JSON.stringify(msg));
    }
  }
}
