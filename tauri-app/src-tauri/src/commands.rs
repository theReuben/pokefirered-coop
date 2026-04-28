use crate::session::{self, CoopSidecar, SessionInfo};
use crate::AppState;
use rand::Rng;
use std::path::Path;
use tauri::State;
use uuid::Uuid;

// ── Session management ────────────────────────────────────────────────────────

#[tauri::command]
pub async fn create_new_session(
    sav_path: String,
    room_code: String,
    randomize_encounters: bool,
) -> Result<SessionInfo, String> {
    let sav = Path::new(&sav_path);
    let coop = session::coop_path(sav);
    let session_id = Uuid::new_v4().to_string();
    let encounter_seed: u32 = rand::thread_rng().gen_range(1..=u32::MAX);

    let sidecar = CoopSidecar {
        session_id: session_id.clone(),
        created_at: chrono_now(),
        randomize_encounters,
    };
    session::write_sidecar(&coop, &sidecar).map_err(|e| e.to_string())?;

    Ok(SessionInfo {
        session_id,
        sav_path,
        coop_path: coop.to_string_lossy().into(),
        room_code,
        is_host: true,
        encounter_seed,
        randomize_encounters,
    })
}

#[tauri::command]
pub async fn load_host_session(
    sav_path: String,
    room_code: String,
    randomize_encounters: bool,
) -> Result<SessionInfo, String> {
    let sav = Path::new(&sav_path);
    let coop = session::coop_path(sav);

    let (session_id, sidecar_rng) = if coop.exists() {
        let s = session::read_sidecar(&coop).map_err(|e| e.to_string())?;
        let rng = s.randomize_encounters;
        (s.session_id, rng)
    } else {
        // First time using this save as a host: create a sidecar
        let id = Uuid::new_v4().to_string();
        let sidecar = CoopSidecar {
            session_id: id.clone(),
            created_at: chrono_now(),
            randomize_encounters,
        };
        session::write_sidecar(&coop, &sidecar).map_err(|e| e.to_string())?;
        (id, randomize_encounters)
    };

    let encounter_seed: u32 = rand::thread_rng().gen_range(1..=u32::MAX);

    Ok(SessionInfo {
        session_id,
        sav_path,
        coop_path: coop.to_string_lossy().into(),
        room_code,
        is_host: true,
        encounter_seed,
        randomize_encounters: sidecar_rng,
    })
}

#[tauri::command]
pub async fn load_guest_session(
    sav_path: String,
    room_code: String,
) -> Result<SessionInfo, String> {
    let sav = Path::new(&sav_path);
    let coop = session::coop_path(sav);

    if !coop.exists() {
        return Err("No .coop sidecar found. Ask the host to share their save file.".into());
    }
    let sidecar = session::read_sidecar(&coop).map_err(|e| e.to_string())?;

    Ok(SessionInfo {
        session_id: sidecar.session_id,
        sav_path,
        coop_path: coop.to_string_lossy().into(),
        room_code,
        is_host: false,
        encounter_seed: 0, // filled in on server connect via session_settings
        randomize_encounters: sidecar.randomize_encounters,
    })
}

// ── Emulator lifecycle ────────────────────────────────────────────────────────

#[tauri::command]
pub async fn start_emulator(
    session: SessionInfo,
    state: State<'_, AppState>,
) -> Result<(), String> {
    let mut emu = state.emulator.lock().unwrap();
    emu.start(&session).map_err(|e| e.to_string())?;

    let mut net = state.net.lock().unwrap();
    net.connect(&session).map_err(|e| e.to_string())?;

    Ok(())
}

#[tauri::command]
pub async fn stop_emulator(state: State<'_, AppState>) -> Result<(), String> {
    let mut emu = state.emulator.lock().unwrap();
    emu.stop().map_err(|e| e.to_string())?;

    let mut net = state.net.lock().unwrap();
    net.disconnect().map_err(|e| e.to_string())?;

    Ok(())
}

#[tauri::command]
pub async fn get_frame(state: State<'_, AppState>) -> Result<Vec<u8>, String> {
    let emu = state.emulator.lock().unwrap();
    Ok(emu.get_frame_rgba())
}

#[tauri::command]
pub async fn set_key_pressed(key_mask: u16, state: State<'_, AppState>) -> Result<(), String> {
    let mut emu = state.emulator.lock().unwrap();
    emu.key_pressed(key_mask);
    Ok(())
}

#[tauri::command]
pub async fn set_key_released(key_mask: u16, state: State<'_, AppState>) -> Result<(), String> {
    let mut emu = state.emulator.lock().unwrap();
    emu.key_released(key_mask);
    Ok(())
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn chrono_now() -> String {
    // RFC 3339 timestamp without pulling in chrono for now
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| format!("{}", d.as_secs()))
        .unwrap_or_else(|_| "0".into())
}
