use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Persistent sidecar stored alongside the .sav file.
/// File name: <savname>.coop
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CoopSidecar {
    pub session_id: String,
    pub created_at: String,
    pub randomize_encounters: bool,
}

/// Full session info passed to both frontend and Tauri commands.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionInfo {
    pub session_id: String,
    pub sav_path: String,
    pub coop_path: String,
    pub room_code: String,
    pub is_host: bool,
    pub encounter_seed: u32,
    pub randomize_encounters: bool,
}

/// Derive the .coop sidecar path from a .sav path.
pub fn coop_path(sav_path: &Path) -> PathBuf {
    sav_path.with_extension("coop")
}

/// Write a CoopSidecar JSON file to disk.
pub fn write_sidecar(path: &Path, sidecar: &CoopSidecar) -> anyhow::Result<()> {
    let json = serde_json::to_string_pretty(sidecar)?;
    std::fs::write(path, json)?;
    Ok(())
}

/// Read a CoopSidecar JSON file from disk.
pub fn read_sidecar(path: &Path) -> anyhow::Result<CoopSidecar> {
    let json = std::fs::read_to_string(path)?;
    let sidecar: CoopSidecar = serde_json::from_str(&json)?;
    Ok(sidecar)
}
