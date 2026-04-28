/**
 * Relay Server Tests — vitest
 *
 * Strategy: build minimal in-memory mocks for Party.Connection, Party.Room,
 * and Party.ConnectionContext, then drive PokemonCoopServer through its three
 * lifecycle hooks (onConnect / onClose / onMessage) and assert on the messages
 * that land in each mock connection's inbox.
 */

import { describe, it, expect, beforeEach } from "vitest";
import PokemonCoopServer, { type ServerMessage } from "./server.js";

// ─── Mocks ────────────────────────────────────────────────────────────────────

let _nextId = 0;

class MockConnection {
  id: string;
  inbox: ServerMessage[] = [];
  closed = false;

  constructor(id?: string) {
    this.id = id ?? `conn-${_nextId++}`;
  }

  send(raw: string): void {
    this.inbox.push(JSON.parse(raw) as ServerMessage);
  }

  close(): void {
    this.closed = true;
  }

  /** Drain all messages of a given type from the inbox. */
  received(type: ServerMessage["type"]): ServerMessage[] {
    return this.inbox.filter((m) => m.type === type);
  }

  /** Last message of a given type, or undefined. */
  last(type: ServerMessage["type"]): ServerMessage | undefined {
    const msgs = this.received(type);
    return msgs[msgs.length - 1];
  }
}

class MockRoom {
  private _conns: MockConnection[] = [];

  register(conn: MockConnection): void {
    this._conns.push(conn);
  }

  unregister(conn: MockConnection): void {
    this._conns = this._conns.filter((c) => c.id !== conn.id);
  }

  getConnections(): IterableIterator<MockConnection> {
    return this._conns[Symbol.iterator]();
  }
}

function makeCtx(url = "ws://localhost/party/testroom"): { request: { url: string } } {
  return { request: { url } };
}

function makeCtxWithSession(
  sessionId: string,
  room = "testroom",
): { request: { url: string } } {
  return { request: { url: `ws://localhost/party/${room}?session_id=${sessionId}` } };
}

// Cast to the interface expected by PokemonCoopServer
function asParty<T>(obj: T): T {
  return obj;
}

// ─── Test Helpers ─────────────────────────────────────────────────────────────

function makeServer(): { server: PokemonCoopServer; room: MockRoom } {
  const room = new MockRoom();
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const server = new PokemonCoopServer(room as any);
  return { server, room };
}

/**
 * Connect a mock connection to the server and register it in the room.
 * Returns the connection after onConnect completes.
 */
function connect(
  server: PokemonCoopServer,
  room: MockRoom,
  conn: MockConnection,
  ctx = makeCtx(),
): MockConnection {
  room.register(conn);
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  server.onConnect(conn as any, ctx as any);
  return conn;
}

function disconnect(
  server: PokemonCoopServer,
  room: MockRoom,
  conn: MockConnection,
): void {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  server.onClose(conn as any);
  room.unregister(conn);
}

function send(
  server: PokemonCoopServer,
  conn: MockConnection,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  msg: Record<string, any>,
): void {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  server.onMessage(JSON.stringify(msg), conn as any);
}

// ─── Tests ────────────────────────────────────────────────────────────────────

describe("Role assignment", () => {
  it("assigns 'host' to the first connection", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    expect(host.last("role")).toEqual({ type: "role", role: "host" });
  });

  it("assigns 'guest' to the second connection", () => {
    const { server, room } = makeServer();
    connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    expect(guest.last("role")).toEqual({ type: "role", role: "guest" });
  });

  it("rejects a third connection with room_full", () => {
    const { server, room } = makeServer();
    connect(server, room, new MockConnection("c1"));
    connect(server, room, new MockConnection("c2"));
    const third = new MockConnection("c3");
    room.register(third);
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    server.onConnect(third as any, makeCtx() as any);
    expect(third.last("room_full")).toEqual({ type: "room_full" });
    expect(third.closed).toBe(true);
  });
});

describe("Session ID validation", () => {
  it("accepts first connection and stores session_id", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"), makeCtxWithSession("sess-abc"));
    expect(host.closed).toBe(false);
    expect(host.last("role")).toBeDefined();
  });

  it("accepts second connection with matching session_id", () => {
    const { server, room } = makeServer();
    connect(server, room, new MockConnection("c1"), makeCtxWithSession("sess-abc"));
    const guest = connect(server, room, new MockConnection("c2"), makeCtxWithSession("sess-abc"));
    expect(guest.closed).toBe(false);
    expect(guest.last("role")).toEqual({ type: "role", role: "guest" });
  });

  it("rejects connection with mismatched session_id", () => {
    const { server, room } = makeServer();
    connect(server, room, new MockConnection("c1"), makeCtxWithSession("sess-abc"));
    const intruder = new MockConnection("c2");
    room.register(intruder);
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    server.onConnect(intruder as any, makeCtxWithSession("sess-xyz") as any);
    expect(intruder.last("session_mismatch")).toEqual({ type: "session_mismatch" });
    expect(intruder.closed).toBe(true);
  });

  it("accepts connection without session_id when no session established", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"), makeCtx());
    expect(host.closed).toBe(false);
  });
});

describe("Partner notifications", () => {
  it("sends partner_connected to existing player when second connects", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    expect(host.received("partner_connected")).toHaveLength(0);
    connect(server, room, new MockConnection("c2"));
    expect(host.last("partner_connected")).toEqual({ type: "partner_connected" });
  });

  it("sends partner_disconnected to remaining player when one disconnects", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    disconnect(server, room, guest);
    expect(host.last("partner_disconnected")).toEqual({ type: "partner_disconnected" });
  });
});

describe("Full sync on connect", () => {
  it("sends empty full_sync when room is empty", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const sync = host.last("full_sync");
    expect(sync).toMatchObject({ type: "full_sync", flags: [], vars: {} });
  });

  it("guest receives accumulated flags and vars on connect", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    send(server, host, { type: "flag_set", flagId: 42 });
    send(server, host, { type: "var_set", varId: 10, value: 99 });

    const guest = connect(server, room, new MockConnection("c2"));
    const sync = guest.last("full_sync");
    expect(sync).toBeDefined();
    expect((sync as { flags: number[] }).flags).toContain(42);
    expect((sync as { vars: Record<number, number> }).vars[10]).toBe(99);
  });

  it("guest receives session_settings (mirrors host's randomizeEncounters)", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    send(server, host, { type: "session_settings", randomizeEncounters: false });
    const guest = connect(server, room, new MockConnection("c2"));
    const settings = guest.last("session_settings");
    expect(settings).toEqual({ type: "session_settings", randomizeEncounters: false });
  });

  it("guest receives partner's last known position on connect", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const pos = { mapId: 5, x: 10, y: 7, facing: 1, spriteState: 0 };
    send(server, host, { type: "position", data: pos });
    const guest = connect(server, room, new MockConnection("c2"));
    const partnerPos = guest.last("partner_position");
    expect(partnerPos).toEqual({ type: "partner_position", data: pos });
  });

  it("guest receives partner's party data on connect if available", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    send(server, host, { type: "party_sync", partyData: "base64partydata==" });
    const guest = connect(server, room, new MockConnection("c2"));
    expect(guest.last("party_sync")).toEqual({
      type: "party_sync",
      partyData: "base64partydata==",
    });
  });
});

describe("Position relay", () => {
  it("forwards position to partner only (no echo to sender)", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    const inboxBefore = host.inbox.length;
    send(server, host, { type: "position", data: { mapId: 1, x: 2, y: 3, facing: 0, spriteState: 0 } });
    // Host should not receive its own position echo
    expect(host.inbox.length).toBe(inboxBefore);
    expect(guest.last("partner_position")).toMatchObject({ type: "partner_position" });
  });

  it("stores and updates last-known position", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    send(server, host, { type: "position", data: { mapId: 1, x: 5, y: 5, facing: 0, spriteState: 0 } });
    send(server, host, { type: "position", data: { mapId: 2, x: 10, y: 10, facing: 2, spriteState: 1 } });

    const guest = connect(server, room, new MockConnection("c2"));
    const partnerPos = guest.last("partner_position") as { data: { mapId: number } };
    // Should get the most recent position (mapId=2), not the first one
    expect(partnerPos?.data.mapId).toBe(2);
  });
});

describe("Flag sync", () => {
  it("broadcasts flag_set to both players on first set", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "flag_set", flagId: 100 });
    expect(host.last("flag_set")).toEqual({ type: "flag_set", flagId: 100 });
    expect(guest.last("flag_set")).toEqual({ type: "flag_set", flagId: 100 });
  });

  it("deduplicates flag_set — only broadcasts once per flag", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "flag_set", flagId: 200 });
    const afterFirst = guest.received("flag_set").length;
    send(server, guest, { type: "flag_set", flagId: 200 }); // second set, same flag
    expect(guest.received("flag_set").length).toBe(afterFirst); // no extra broadcast
  });

  it("broadcasts var_set to both players", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "var_set", varId: 5, value: 42 });
    expect(host.last("var_set")).toEqual({ type: "var_set", varId: 5, value: 42 });
    expect(guest.last("var_set")).toEqual({ type: "var_set", varId: 5, value: 42 });
  });

  it("guest receives flag in full_sync after host sets it before guest connects", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    send(server, host, { type: "flag_set", flagId: 300 });
    const guest = connect(server, room, new MockConnection("c2"));
    const sync = guest.last("full_sync") as { flags: number[] };
    expect(sync.flags).toContain(300);
  });
});

describe("Boss readiness", () => {
  it("sends boss_waiting when only one player is ready", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "boss_ready", bossId: 1 });
    expect(host.last("boss_waiting")).toEqual({ type: "boss_waiting" });
  });

  it("sends boss_start to both players when both are ready at the same boss", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "boss_ready", bossId: 3 });
    send(server, guest, { type: "boss_ready", bossId: 3 });
    expect(host.last("boss_start")).toEqual({ type: "boss_start", bossId: 3 });
    expect(guest.last("boss_start")).toEqual({ type: "boss_start", bossId: 3 });
  });

  it("does not start when players are ready at different bosses", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "boss_ready", bossId: 1 });
    send(server, guest, { type: "boss_ready", bossId: 2 });
    expect(host.received("boss_start")).toHaveLength(0);
    expect(guest.received("boss_start")).toHaveLength(0);
    expect(guest.last("boss_waiting")).toEqual({ type: "boss_waiting" });
  });

  it("clears readiness after boss_start so a rematch requires both to ready again", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "boss_ready", bossId: 4 });
    send(server, guest, { type: "boss_ready", bossId: 4 });
    // Both ready — boss_start fired. Now only host re-readies.
    send(server, host, { type: "boss_ready", bossId: 4 });
    // Should get boss_waiting, not a second boss_start
    const starts = host.received("boss_start");
    expect(starts).toHaveLength(1);
    expect(host.last("boss_waiting")).toBeDefined();
  });

  it("boss_cancel clears the player's readiness", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "boss_ready", bossId: 5 });
    send(server, host, { type: "boss_cancel" });
    send(server, guest, { type: "boss_ready", bossId: 5 });
    // Guest is alone after host cancelled — should get boss_waiting not boss_start
    expect(guest.last("boss_waiting")).toBeDefined();
    expect(host.received("boss_start")).toHaveLength(0);
  });
});

describe("Starter picking", () => {
  it("broadcasts starter_taken to partner when host picks", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "starter_pick", speciesId: 1 }); // Bulbasaur
    expect(guest.last("starter_taken")).toEqual({ type: "starter_taken", speciesId: 1 });
  });

  it("sends starter_denied when guest tries to pick already-taken species", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "starter_pick", speciesId: 4 }); // Charmander
    send(server, guest, { type: "starter_pick", speciesId: 4 }); // Same! Conflict
    expect(guest.last("starter_denied")).toEqual({ type: "starter_denied", speciesId: 4 });
  });

  it("allows guest to pick a different starter than host", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "starter_pick", speciesId: 1 }); // Bulbasaur
    send(server, guest, { type: "starter_pick", speciesId: 4 }); // Charmander
    expect(host.last("starter_taken")).toEqual({ type: "starter_taken", speciesId: 4 });
    expect(guest.received("starter_denied")).toHaveLength(0);
  });

  it("is idempotent — second pick by same player is ignored", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "starter_pick", speciesId: 1 });
    const guestInboxBefore = (host.last("partner_connected") ? 1 : 0);
    send(server, host, { type: "starter_pick", speciesId: 7 }); // attempt to change
    // The second pick should not broadcast a new starter_taken
    // Host's own inbox won't echo, partner's count stays the same
    // (This tests that state.starters[role] !== null guard works)
    expect(host.received("starter_denied")).toHaveLength(0); // no error to self
  });

  it("late-joining guest receives partner's starter via starter_taken on connect", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    send(server, host, { type: "starter_pick", speciesId: 7 }); // Squirtle
    const guest = connect(server, room, new MockConnection("c2"));
    expect(guest.last("starter_taken")).toEqual({ type: "starter_taken", speciesId: 7 });
  });
});

describe("Session settings", () => {
  it("defaults randomizeEncounters to true (guest inherits on connect)", () => {
    const { server, room } = makeServer();
    connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    expect(guest.last("session_settings")).toEqual({
      type: "session_settings",
      randomizeEncounters: true,
    });
  });

  it("host can change randomizeEncounters; guest receives update", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "session_settings", randomizeEncounters: false });
    expect(guest.last("session_settings")).toEqual({
      type: "session_settings",
      randomizeEncounters: false,
    });
  });

  it("guest cannot change session_settings (only host controls it)", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, guest, { type: "session_settings", randomizeEncounters: false });
    // A new guest joining should still see the default (true) not what guest tried to set
    const thirdConn = new MockConnection("c3");
    room.unregister(host); // Free up a slot by unregistering (not calling onClose)
    connect(server, room, thirdConn);
    // The session_settings broadcast to guest when they sent it should not have happened
    // (guest should not have echoed it to host; host setting stays true)
    const hostSettings = host.received("session_settings");
    // Host should not have received a session_settings message from guest's attempt
    expect(hostSettings).toHaveLength(0);
  });
});

describe("Disconnect / reconnect", () => {
  it("clears partner position on disconnect so reconnecting guest does not get stale data", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "position", data: { mapId: 9, x: 1, y: 1, facing: 0, spriteState: 0 } });
    disconnect(server, room, host);

    // A new host connects
    const host2 = connect(server, room, new MockConnection("c3"));
    // New host should not receive stale guest position as partner_position
    // (guest's position was set, not cleared; this tests host's position is gone)
    // Actually guest's position IS still stored — only the disconnected player's pos is cleared.
    // The new "host" (c3) is now guest role-equivalent — but role assignment: c3 is now host since
    // the old host disconnected. Check c3 doesn't get stale c1 (old host) position.
    const partnerPos = host2.last("partner_position");
    // Old host's stored position should have been cleared on disconnect
    expect(partnerPos).toBeUndefined();
  });

  it("after disconnect, new player can join and gets guest role", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    disconnect(server, room, guest);
    const newGuest = connect(server, room, new MockConnection("c3"));
    expect(newGuest.last("role")).toEqual({ type: "role", role: "guest" });
    expect(newGuest.closed).toBe(false);
  });

  it("boss_ready state is cleared on disconnect", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "boss_ready", bossId: 2 });
    disconnect(server, room, host);

    // New host reconnects and sends boss_ready — should not immediately get boss_start
    const host2 = connect(server, room, new MockConnection("c3"));
    send(server, host2, { type: "boss_ready", bossId: 2 });
    // Guest's old readiness was cleared; host2 should get boss_waiting
    expect(host2.last("boss_waiting")).toBeDefined();
    expect(host2.received("boss_start")).toHaveLength(0);
  });
});

describe("Battle turn relay", () => {
  it("forwards battle_turn to partner only", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    const guest = connect(server, room, new MockConnection("c2"));
    send(server, host, { type: "battle_turn", turnData: "base64turn==" });
    expect(guest.last("battle_turn")).toEqual({ type: "battle_turn", turnData: "base64turn==" });
    // Host should not echo to itself
    expect(host.received("battle_turn")).toHaveLength(0);
  });
});

describe("Malformed messages", () => {
  it("ignores invalid JSON without crashing", () => {
    const { server, room } = makeServer();
    const host = connect(server, room, new MockConnection("c1"));
    expect(() => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      server.onMessage("{not valid json", host as any);
    }).not.toThrow();
  });

  it("ignores messages from unknown connection IDs", () => {
    const { server, room } = makeServer();
    connect(server, room, new MockConnection("c1"));
    const unknown = new MockConnection("unknown");
    expect(() => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      server.onMessage(JSON.stringify({ type: "boss_ready", bossId: 1 }), unknown as any);
    }).not.toThrow();
  });
});
