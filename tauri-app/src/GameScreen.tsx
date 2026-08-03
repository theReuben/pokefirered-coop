import { useEffect, useRef, useState, useCallback } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import ConnectionStatus from "./ConnectionStatus";
import { copyText } from "./clipboard";
import { pollGamepad } from "./gamepad";
import type { ConnectionStatus as ConnStatus, SessionInfo } from "./types";

interface MpDebug {
  partner_connected: boolean;
  role: number; // 0=none, 1=host, 2=guest
  conn_state: number;
  send_magic: number;
  send_head: number;
  send_tail: number;
  recv_magic: number;
  recv_head: number;
  recv_tail: number;
  packets_sent: number;
  packets_recv: number;
  ticks: number;
}

interface Props {
  session: SessionInfo;
  onDisconnect: () => void;
}

// GBA display: 240×160 pixels
const GBA_W = 240;
const GBA_H = 160;
const SCALE = 3;

// GBA button bitmask values (matches GBA KEYINPUT register, active-low)
const GBA_BUTTONS: Record<string, number> = {
  ArrowRight: 1 << 4,
  ArrowLeft:  1 << 5,
  ArrowUp:    1 << 6,
  ArrowDown:  1 << 7,
  z:          1 << 0, // A
  x:          1 << 1, // B
  Enter:      1 << 3, // Start
  Backspace:  1 << 2, // Select
  a:          1 << 9, // L  (KEYINPUT bit 9 — R is bit 8, not the other way round)
  s:          1 << 8, // R
};

export default function GameScreen({ session, onDisconnect }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const divRef = useRef<HTMLDivElement>(null);
  const [connStatus, setConnStatus] = useState<ConnStatus>("connecting");
  const [mpDebug, setMpDebug] = useState<MpDebug | null>(null);
  const [showDebug, setShowDebug] = useState(false);
  const [codeCopied, setCodeCopied] = useState(false);
  const keysHeld = useRef<Set<string>>(new Set());
  const animFrame = useRef<number>(0);

  // Input is tracked PER SOURCE and only the union is sent to the backend.
  // `key_state` in emulator.rs is a single u16 with no notion of who pressed
  // what, so releasing a button on the gamepad would otherwise clear a key the
  // keyboard is still holding (and vice versa).
  const keyboardMask = useRef(0);
  const gamepadMask = useRef(0);
  const appliedMask = useRef(0);

  // Push the current union to the backend, emitting only the edges.
  const syncInput = useCallback(() => {
    const want = keyboardMask.current | gamepadMask.current;
    const have = appliedMask.current;
    if (want === have) return;
    appliedMask.current = want;

    const pressed = want & ~have;
    const released = have & ~want;
    if (pressed) void invoke("set_key_pressed", { keyMask: pressed });
    if (released) void invoke("set_key_released", { keyMask: released });
  }, []);

  // Copy the room code, then hand keyboard focus straight back to the game so
  // the player isn't left typing into the button.
  async function handleCopyCode() {
    if (await copyText(session.roomCode)) {
      setCodeCopied(true);
      setTimeout(() => setCodeCopied(false), 1500);
    }
    divRef.current?.focus();
  }

  // Render loop: poll the backend for the latest frame buffer
  const renderLoop = useCallback(async () => {
    // Controller polling rides the existing frame loop — no second timer.
    // navigator.getGamepads() is a snapshot poll, so there is no event to
    // listen for; this is the only way to read a pad.
    gamepadMask.current = pollGamepad();
    syncInput();

    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    try {
      // Backend returns raw RGBA bytes for the 240×160 frame
      const frameBytes: number[] = await invoke("get_frame");
      const imageData = new ImageData(
        new Uint8ClampedArray(frameBytes),
        GBA_W,
        GBA_H,
      );
      ctx.putImageData(imageData, 0, 0);
    } catch {
      // Frame not ready yet — skip
    }

    animFrame.current = requestAnimationFrame(() => void renderLoop());
  }, [syncInput]);

  useEffect(() => {
    // Auto-focus so keyboard events are captured immediately
    divRef.current?.focus();

    // Start the emulator
    void invoke("start_emulator", { session });

    // Begin render loop
    animFrame.current = requestAnimationFrame(() => void renderLoop());

    // Listen for connection status events from the backend
    const unlistenConn = listen<ConnStatus>("connection_status", (ev) => {
      setConnStatus(ev.payload);
    });

    // Poll multiplayer debug state every 2 s
    const debugInterval = setInterval(() => {
      void invoke<MpDebug>("get_mp_debug").then(setMpDebug).catch(() => {});
    }, 2000);

    // A pad unplugged mid-hold never reports a release, so drop its bits.
    const onPadLost = () => { gamepadMask.current = 0; syncInput(); };
    window.addEventListener("gamepaddisconnected", onPadLost);

    return () => {
      cancelAnimationFrame(animFrame.current);
      clearInterval(debugInterval);
      window.removeEventListener("gamepaddisconnected", onPadLost);
      // Release everything; the emulator is about to be torn down but the
      // masks must not survive into a following session.
      keyboardMask.current = 0;
      gamepadMask.current = 0;
      syncInput();
      void invoke("stop_emulator");
      void unlistenConn.then((f) => f());
    };
  }, [session, renderLoop, syncInput]);

  function handleKeyDown(e: React.KeyboardEvent) {
    if (e.key === "F3") { setShowDebug(v => !v); return; }
    if (keysHeld.current.has(e.key)) return;
    keysHeld.current.add(e.key);
    const mask = GBA_BUTTONS[e.key];
    if (mask !== undefined) {
      keyboardMask.current |= mask;
      syncInput();
    }
  }

  function handleKeyUp(e: React.KeyboardEvent) {
    keysHeld.current.delete(e.key);
    const mask = GBA_BUTTONS[e.key];
    if (mask !== undefined) {
      keyboardMask.current &= ~mask;
      syncInput();
    }
  }

  function handleBlur() {
    // Release every held KEY so keys don't get stuck when focus leaves the
    // window.  The gamepad is deliberately left alone — it doesn't need focus
    // and stays live (e.g. right after clicking the room-code button).
    keysHeld.current.clear();
    keyboardMask.current = 0;
    syncInput();
  }

  return (
    <div ref={divRef} className="screen game-screen" tabIndex={0} onKeyDown={handleKeyDown} onKeyUp={handleKeyUp} onBlur={handleBlur}>
      <div className="top-bar">
        <ConnectionStatus status={connStatus} />
        <button
          className="room-code-small"
          onClick={handleCopyCode}
          title="Click to copy the room code"
        >
          {codeCopied ? "Copied!" : `Room: ${session.roomCode}`}
        </button>
        <button className="btn btn-ghost btn-small" onClick={onDisconnect}>
          Quit
        </button>
      </div>

      <canvas
        ref={canvasRef}
        width={GBA_W}
        height={GBA_H}
        style={{ width: GBA_W * SCALE, height: GBA_H * SCALE, imageRendering: "pixelated" }}
        className="gba-canvas"
      />

      <div className="controls-hint">
        Z=A &nbsp; X=B &nbsp; Enter=Start &nbsp; Backspace=Select &nbsp; Arrows=D-pad &nbsp; A/S=L/R
        &nbsp;|&nbsp; Controller supported
        &nbsp;|&nbsp; <span style={{cursor:"pointer",textDecoration:"underline"}} onClick={() => setShowDebug(v=>!v)}>F3=debug</span>
      </div>

      {showDebug && mpDebug && (
        <pre style={{
          position:"absolute", top:40, left:0, right:0,
          background:"rgba(0,0,0,0.75)", color:"#0f0",
          fontSize:11, padding:"4px 8px", margin:0,
          fontFamily:"monospace", whiteSpace:"pre", userSelect:"text",
          zIndex:100,
        }}>
{`role=${["none","host","guest"][mpDebug.role]??mpDebug.role} partner=${mpDebug.partner_connected} connState=${mpDebug.conn_state}
snd magic=0x${mpDebug.send_magic.toString(16).padStart(2,"0")} h=${mpDebug.send_head} t=${mpDebug.send_tail}
rcv magic=0x${mpDebug.recv_magic.toString(16).padStart(2,"0")} h=${mpDebug.recv_head} t=${mpDebug.recv_tail}
sent=${mpDebug.packets_sent} recv=${mpDebug.packets_recv} ticks=${mpDebug.ticks}`}
        </pre>
      )}
    </div>
  );
}
