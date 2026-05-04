use crate::session::SessionInfo;
use futures_util::{SinkExt, StreamExt};
use serde_json::Value;
use std::sync::{Arc, Mutex};
use tauri::Emitter;
use tokio::sync::mpsc;
use tokio_tungstenite::{connect_async, tungstenite::Message};
use url::Url;

const RELAY_URL_DEFAULT: &str = "wss://pokefirered-coop.thereuben.partykit.dev/parties/main";
const RECONNECT_DELAY_MS: u64 = 2000;
const MAX_RECONNECT_ATTEMPTS: u32 = 10;

/// Return the relay URL. Set the COOP_RELAY_URL environment variable to override.
/// Example: COOP_RELAY_URL=ws://localhost:1999/parties/main for local PartyKit dev server.
fn relay_url() -> String {
    std::env::var("COOP_RELAY_URL").unwrap_or_else(|_| RELAY_URL_DEFAULT.to_string())
}

/// Messages sent from the emulator to the relay server.
pub type OutboundMsg = serde_json::Value;

/// Messages received from the relay server (forwarded to the emulator).
pub type InboundMsg = serde_json::Value;

/// Thread-safe handle to the network connection.
pub struct NetHandle {
    tx: Option<mpsc::UnboundedSender<OutboundMsg>>,
    // Inbound messages queued for the emulator to consume each frame
    inbound: Arc<Mutex<Vec<InboundMsg>>>,
}

impl NetHandle {
    pub fn new() -> Self {
        Self {
            tx: None,
            inbound: Arc::new(Mutex::new(Vec::new())),
        }
    }

    /// Establish a WebSocket connection to the relay server.
    pub fn connect(&mut self, session: &SessionInfo, app: tauri::AppHandle) -> anyhow::Result<()> {
        if self.tx.is_some() {
            return Ok(()); // already connected
        }

        // Omit session_id for new guest joins (empty string) so the server
        // skips session validation for the guest slot.
        let url_str = if session.session_id.is_empty() {
            format!("{}/{}", relay_url(), session.room_code)
        } else {
            format!("{}/{}?session_id={}", relay_url(), session.room_code, session.session_id)
        };
        let url = Url::parse(&url_str)?;

        let (tx, rx) = mpsc::unbounded_channel::<OutboundMsg>();
        self.tx = Some(tx);

        let inbound = Arc::clone(&self.inbound);
        let is_host = session.is_host;
        let encounter_seed = session.encounter_seed;
        let randomize_encounters = session.randomize_encounters;

        // Spawn the WebSocket task on the Tokio runtime
        tokio::spawn(async move {
            run_ws_loop(url, rx, inbound, app, is_host, encounter_seed, randomize_encounters).await;
        });

        Ok(())
    }

    /// Disconnect from the relay server.
    pub fn disconnect(&mut self) -> anyhow::Result<()> {
        self.tx = None; // dropping the sender causes the recv loop to end
        Ok(())
    }

    /// Enqueue a JSON message to be sent to the relay server.
    pub fn send(&self, msg: OutboundMsg) {
        if let Some(tx) = &self.tx {
            let _ = tx.send(msg);
        }
    }

    /// Drain all messages received from the relay server since the last call.
    pub fn drain_inbound(&self) -> Vec<InboundMsg> {
        self.inbound.lock().unwrap().drain(..).collect()
    }
}

fn emit_status(app: &tauri::AppHandle, status: &str) {
    let _ = app.emit("connection_status", status);
}

/// Long-running async task: maintain the WebSocket connection with reconnect.
async fn run_ws_loop(
    url: Url,
    mut rx: mpsc::UnboundedReceiver<OutboundMsg>,
    inbound: Arc<Mutex<Vec<InboundMsg>>>,
    app: tauri::AppHandle,
    is_host: bool,
    encounter_seed: u32,
    randomize_encounters: bool,
) {
    let mut attempts = 0u32;

    loop {
        emit_status(&app, "connecting");

        match connect_async(url.as_str()).await {
            Ok((ws_stream, _)) => {
                attempts = 0;
                log::info!("WebSocket connected to {}", url);
                emit_status(&app, "connected");

                let (mut write, mut read) = ws_stream.split();

                // Host sends session settings immediately so the relay can
                // store and forward them to the guest on connect.
                if is_host {
                    let settings = serde_json::json!({
                        "type": "session_settings",
                        "randomizeEncounters": randomize_encounters,
                        "encounterSeed": encounter_seed,
                    });
                    let _ = write.send(Message::Text(settings.to_string())).await;
                    log::info!("net: sent session_settings (seed={}, randomize={})", encounter_seed, randomize_encounters);
                }

                loop {
                    tokio::select! {
                        // Forward outbound messages to the server
                        msg = rx.recv() => {
                            match msg {
                                Some(json) => {
                                    let text = json.to_string();
                                    if let Err(e) = write.send(Message::Text(text)).await {
                                        log::warn!("WS send error: {e}");
                                        break;
                                    }
                                }
                                None => {
                                    // Channel closed — clean disconnect
                                    let _ = write.close().await;
                                    emit_status(&app, "disconnected");
                                    return;
                                }
                            }
                        }

                        // Receive messages from the server
                        frame = read.next() => {
                            match frame {
                                Some(Ok(Message::Text(text))) => {
                                    match serde_json::from_str::<Value>(&text) {
                                        Ok(json) => {
                                            // session_mismatch = stale room; abort, don't retry
                                            if json.get("type").and_then(|v| v.as_str()) == Some("session_mismatch") {
                                                log::error!("Server rejected connection: session_mismatch (stale room). Reconnect to a new room.");
                                                emit_status(&app, "error");
                                                return;
                                            }
                                            inbound.lock().unwrap().push(json);
                                        }
                                        Err(e) => log::warn!("Bad JSON from server: {e}"),
                                    }
                                }
                                Some(Ok(Message::Close(_))) | None => {
                                    log::info!("WebSocket connection closed by server");
                                    break;
                                }
                                Some(Err(e)) => {
                                    log::warn!("WebSocket error: {e}");
                                    break;
                                }
                                _ => {} // ping/pong/binary — ignore
                            }
                        }
                    }
                }
            }
            Err(e) => {
                log::warn!("WebSocket connect failed: {e}");
            }
        }

        attempts += 1;
        if attempts >= MAX_RECONNECT_ATTEMPTS {
            log::error!("Giving up after {MAX_RECONNECT_ATTEMPTS} reconnect attempts");
            emit_status(&app, "error");
            return;
        }

        let delay = RECONNECT_DELAY_MS * (1 << attempts.min(5));
        log::info!("Reconnecting in {delay}ms (attempt {attempts})");
        emit_status(&app, "reconnecting");
        tokio::time::sleep(tokio::time::Duration::from_millis(delay)).await;
    }
}
