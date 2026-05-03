import { useEffect, useRef, useState, useCallback } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import ConnectionStatus from "./ConnectionStatus";
import type { ConnectionStatus as ConnStatus, SessionInfo } from "./types";

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
  a:          1 << 8, // L
  s:          1 << 9, // R
};

export default function GameScreen({ session, onDisconnect }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const divRef = useRef<HTMLDivElement>(null);
  const [connStatus, setConnStatus] = useState<ConnStatus>("connecting");
  const keysHeld = useRef<Set<string>>(new Set());
  const animFrame = useRef<number>(0);

  // Render loop: poll the backend for the latest frame buffer
  const renderLoop = useCallback(async () => {
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
  }, []);

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

    return () => {
      cancelAnimationFrame(animFrame.current);
      void invoke("stop_emulator");
      void unlistenConn.then((f) => f());
    };
  }, [session, renderLoop]);

  function handleKeyDown(e: React.KeyboardEvent) {
    if (keysHeld.current.has(e.key)) return;
    keysHeld.current.add(e.key);
    const mask = GBA_BUTTONS[e.key];
    if (mask !== undefined) {
      void invoke("set_key_pressed", { keyMask: mask });
    }
  }

  function handleKeyUp(e: React.KeyboardEvent) {
    keysHeld.current.delete(e.key);
    const mask = GBA_BUTTONS[e.key];
    if (mask !== undefined) {
      void invoke("set_key_released", { keyMask: mask });
    }
  }

  function handleBlur() {
    // Release every held button so keys don't get stuck when focus leaves the window
    for (const key of keysHeld.current) {
      const mask = GBA_BUTTONS[key];
      if (mask !== undefined) {
        void invoke("set_key_released", { keyMask: mask });
      }
    }
    keysHeld.current.clear();
  }

  return (
    <div ref={divRef} className="screen game-screen" tabIndex={0} onKeyDown={handleKeyDown} onKeyUp={handleKeyUp} onBlur={handleBlur}>
      <div className="top-bar">
        <ConnectionStatus status={connStatus} />
        <span className="room-code-small">Room: {session.roomCode}</span>
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
      </div>
    </div>
  );
}
