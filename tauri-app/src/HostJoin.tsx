import { useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import { invoke } from "@tauri-apps/api/core";
import type { SessionInfo } from "./types";

interface Props {
  onSessionReady: (session: SessionInfo) => void;
}

type Mode = "pick" | "host-new" | "host-load" | "join" | "join-new" | "join-load";

function generateRoomCode(): string {
  const chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  let code = "";
  for (let i = 0; i < 6; i++) {
    code += chars[Math.floor(Math.random() * chars.length)];
  }
  return code;
}

export default function HostJoin({ onSessionReady }: Props) {
  const [mode, setMode] = useState<Mode>("pick");
  const [roomCode, setRoomCode] = useState("");
  const [joinCode, setJoinCode] = useState("");
  const [randomize, setRandomize] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  // ── Host: New Game ─────────────────────────────────────────────────────────

  async function handleHostNew() {
    setError(null);
    setLoading(true);
    try {
      // Ask where to save the .sav file
      const path = await save({
        filters: [{ name: "GBA Save", extensions: ["sav"] }],
        defaultPath: "pokefirered.sav",
        title: "Choose save file location",
      });
      if (!path) { setLoading(false); return; }

      const code = generateRoomCode();
      const session: SessionInfo = await invoke("create_new_session", {
        savPath: path,
        roomCode: code,
        randomizeEncounters: randomize,
      });

      setRoomCode(code);
      onSessionReady(session);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }

  // ── Host: Load Save ────────────────────────────────────────────────────────

  async function handleHostLoad() {
    setError(null);
    setLoading(true);
    try {
      const path = await open({
        filters: [{ name: "GBA Save", extensions: ["sav"] }],
        title: "Load save file",
      });
      if (!path || Array.isArray(path)) { setLoading(false); return; }

      const code = generateRoomCode();
      const session: SessionInfo = await invoke("load_host_session", {
        savPath: path,
        roomCode: code,
        randomizeEncounters: randomize,
      });

      setRoomCode(code);
      onSessionReady(session);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }

  // ── Join: New Game ─────────────────────────────────────────────────────────

  async function handleJoinNew() {
    setError(null);
    const code = joinCode.trim().toUpperCase();
    setLoading(true);
    try {
      const path = await save({
        filters: [{ name: "GBA Save", extensions: ["sav"] }],
        defaultPath: "pokefirered.sav",
        title: "Choose save file location",
      });
      if (!path) { setLoading(false); return; }

      const session: SessionInfo = await invoke("join_new_session", {
        savPath: path,
        roomCode: code,
      });

      onSessionReady(session);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }

  // ── Join: Load Save ────────────────────────────────────────────────────────

  async function handleJoinLoad() {
    setError(null);
    const code = joinCode.trim().toUpperCase();
    setLoading(true);
    try {
      const path = await open({
        filters: [{ name: "GBA Save", extensions: ["sav"] }],
        title: "Load save file",
      });
      if (!path || Array.isArray(path)) { setLoading(false); return; }

      const session: SessionInfo = await invoke("load_guest_session", {
        savPath: path,
        roomCode: code,
      });

      onSessionReady(session);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }

  // ── Render ─────────────────────────────────────────────────────────────────

  if (mode === "pick") {
    return (
      <div className="screen host-join">
        <div className="logo">
          <h1>Pokémon FireRed</h1>
          <h2>Co-Op</h2>
        </div>
        <div className="buttons">
          <button className="btn btn-primary" onClick={() => setMode("host-new")}>
            Host Game
          </button>
          <button className="btn btn-secondary" onClick={() => setMode("join")}>
            Join Game
          </button>
        </div>
      </div>
    );
  }

  if (mode === "join") {
    return (
      <div className="screen host-join">
        <h2>Join a Game</h2>

        <input
          className="code-input"
          type="text"
          placeholder="Enter room code"
          maxLength={6}
          value={joinCode}
          onChange={(e) => setJoinCode(e.target.value.toUpperCase())}
        />

        <div className="buttons">
          <button
            className="btn btn-primary"
            onClick={() => setMode("join-new")}
            disabled={joinCode.trim().length !== 6}
          >
            New Game
          </button>
          <button
            className="btn btn-secondary"
            onClick={() => setMode("join-load")}
            disabled={joinCode.trim().length !== 6}
          >
            Load Save
          </button>
          <button className="btn btn-ghost" onClick={() => setMode("pick")}>
            Back
          </button>
        </div>

        {error && <p className="error">{error}</p>}
      </div>
    );
  }

  if (mode === "join-new" || mode === "join-load") {
    const isNew = mode === "join-new";
    return (
      <div className="screen host-join">
        <h2>Join — {isNew ? "New Game" : "Load Save"}</h2>
        <p className="room-code-label">Room code: <strong>{joinCode}</strong></p>

        <div className="buttons">
          <button
            className="btn btn-primary"
            onClick={isNew ? handleJoinNew : handleJoinLoad}
            disabled={loading}
          >
            {isNew ? "Choose save location…" : "Open save file…"}
          </button>
          <button className="btn btn-ghost" onClick={() => setMode("join")} disabled={loading}>
            Back
          </button>
        </div>

        {error && <p className="error">{error}</p>}
      </div>
    );
  }

  if (mode === "host-new" || mode === "host-load") {
    return (
      <div className="screen host-join">
        <h2>Host a Game</h2>

        <label className="toggle-row">
          <span>Randomize wild Pokémon</span>
          <input
            type="checkbox"
            checked={randomize}
            onChange={(e) => setRandomize(e.target.checked)}
          />
        </label>

        <div className="buttons">
          <button className="btn btn-primary" onClick={handleHostNew} disabled={loading}>
            New Game
          </button>
          <button className="btn btn-secondary" onClick={handleHostLoad} disabled={loading}>
            Load Save
          </button>
          <button className="btn btn-ghost" onClick={() => setMode("pick")} disabled={loading}>
            Back
          </button>
        </div>

        {roomCode && (
          <div className="room-code">
            <span>Room code:</span>
            <strong>{roomCode}</strong>
            <small>Share this with your partner</small>
          </div>
        )}

        {error && <p className="error">{error}</p>}
      </div>
    );
  }

  return null; // all modes handled above
}
