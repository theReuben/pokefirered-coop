import { useState } from "react";
import HostJoin from "./HostJoin";
import GameScreen from "./GameScreen";
import type { SessionInfo } from "./types";

type AppScreen = "host-join" | "game";

export default function App() {
  const [screen, setScreen] = useState<AppScreen>("host-join");
  const [session, setSession] = useState<SessionInfo | null>(null);

  function handleSessionReady(info: SessionInfo) {
    setSession(info);
    setScreen("game");
  }

  function handleDisconnect() {
    setSession(null);
    setScreen("host-join");
  }

  return (
    <div className="app">
      {screen === "host-join" && (
        <HostJoin onSessionReady={handleSessionReady} />
      )}
      {screen === "game" && session !== null && (
        <GameScreen session={session} onDisconnect={handleDisconnect} />
      )}
    </div>
  );
}
