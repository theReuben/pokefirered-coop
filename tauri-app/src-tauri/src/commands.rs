use crate::serial_bridge;
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

/// Create a brand-new guest session (first-time join, no existing .coop).
/// The guest connects without a session_id so the server doesn't reject them;
/// see server.ts which only validates session_id for the host slot.
#[tauri::command]
pub async fn join_new_session(
    sav_path: String,
    room_code: String,
) -> Result<SessionInfo, String> {
    let sav = Path::new(&sav_path);
    let coop = session::coop_path(sav);
    let session_id = Uuid::new_v4().to_string();

    let sidecar = CoopSidecar {
        session_id: session_id.clone(),
        created_at: chrono_now(),
        randomize_encounters: true, // overwritten by session_settings from server
    };
    session::write_sidecar(&coop, &sidecar).map_err(|e| e.to_string())?;

    Ok(SessionInfo {
        session_id: String::new(), // omitted from WS URL so server skips validation
        sav_path,
        coop_path: coop.to_string_lossy().into(),
        room_code,
        is_host: false,
        encounter_seed: 0,
        randomize_encounters: true,
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
    app: tauri::AppHandle,
    session: SessionInfo,
    state: State<'_, AppState>,
) -> Result<(), String> {
    let rom_path = resolve_rom_path(&app)?;

    let mut emu = state.emulator.lock().unwrap();
    emu.start(&session, &rom_path).map_err(|e| e.to_string())?;

    // Prime the encounter seed so it's written to gCoopSettings.encounterSeed
    // from the very first tick, before the ROM's Multiplayer_Init runs.
    serial_bridge::set_encounter_seed(session.encounter_seed);

    let mut net = state.net.lock().unwrap();
    net.connect(&session, app.clone()).map_err(|e| e.to_string())?;

    Ok(())
}

#[tauri::command]
pub async fn stop_emulator(state: State<'_, AppState>) -> Result<(), String> {
    let mut emu = state.emulator.lock().unwrap();
    // Flush save first so the file is written before the emulator core is dropped.
    emu.flush_save().map_err(|e| e.to_string())?;
    emu.stop().map_err(|e| e.to_string())?;

    let mut net = state.net.lock().unwrap();
    net.disconnect().map_err(|e| e.to_string())?;

    Ok(())
}

#[tauri::command]
pub async fn get_frame(state: State<'_, AppState>) -> Result<Vec<u8>, String> {
    let mut emu = state.emulator.lock().unwrap();
    let net = state.net.lock().unwrap();
    emu.step_frame();
    serial_bridge::tick(&mut emu, &net);
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

/// Return current multiplayer bridge state for the debug overlay.
/// Reads ring-buffer head/tail/magic and counters without affecting emulator state.
#[tauri::command]
pub async fn get_mp_debug(state: State<'_, AppState>) -> Result<serial_bridge::DebugState, String> {
    let emu = state.emulator.lock().unwrap();
    Ok(serial_bridge::get_debug_state(&emu))
}

/// Flush the battery save to disk without stopping the emulator.
/// Call this on window close events or explicit save requests.
#[tauri::command]
pub async fn save_game(state: State<'_, AppState>) -> Result<(), String> {
    let emu = state.emulator.lock().unwrap();
    emu.flush_save().map_err(|e| e.to_string())?;
    Ok(())
}

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Resolve the bundled ROM path.
///
/// In dev mode `resource_dir()` points to `src-tauri/`, so the ROM lives at
/// `src-tauri/rom/pokefirered.gba`. In a bundled app it lives at
/// `{resources}/rom/pokefirered.gba` (matching the tauri.conf.json mapping).
fn resolve_rom_path(app: &tauri::AppHandle) -> Result<String, String> {
    use tauri::Manager;
    let rom_path = app
        .path()
        .resource_dir()
        .map_err(|e| format!("resource_dir: {e}"))?
        .join("rom")
        .join("pokefirered.gba");

    if !rom_path.exists() {
        return Err(format!(
            "ROM not found at {}. Run 'make bundle-rom' first.",
            rom_path.display()
        ));
    }

    Ok(rom_path.to_string_lossy().into_owned())
}

fn chrono_now() -> String {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| format!("{}", d.as_secs()))
        .unwrap_or_else(|_| "0".into())
}
