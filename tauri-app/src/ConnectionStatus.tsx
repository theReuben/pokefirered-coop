import type { ConnectionStatus } from "./types";

interface Props {
  status: ConnectionStatus;
}

const LABEL: Record<ConnectionStatus, string> = {
  disconnected: "Disconnected",
  connecting: "Connecting…",
  connected: "Connected",
  reconnecting: "Reconnecting…",
  error: "Connection error",
};

const DOT_CLASS: Record<ConnectionStatus, string> = {
  disconnected: "dot-grey",
  connecting: "dot-yellow",
  connected: "dot-green",
  reconnecting: "dot-yellow",
  error: "dot-red",
};

export default function ConnectionStatus({ status }: Props) {
  return (
    <div className={`connection-status ${status}`} aria-label={LABEL[status]}>
      <span className={`dot ${DOT_CLASS[status]}`} />
      <span className="label">{LABEL[status]}</span>
    </div>
  );
}
