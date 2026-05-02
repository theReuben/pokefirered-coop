mod commands;
mod emulator;
mod net;
mod serial_bridge;
mod session;

use tauri::Manager;

pub use session::SessionInfo;

/// Global application state shared across Tauri commands.
pub struct AppState {
    pub emulator: std::sync::Mutex<emulator::EmulatorHandle>,
    pub net: std::sync::Mutex<net::NetHandle>,
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::init();

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .manage(AppState {
            emulator: std::sync::Mutex::new(emulator::EmulatorHandle::new()),
            net: std::sync::Mutex::new(net::NetHandle::new()),
        })
        .invoke_handler(tauri::generate_handler![
            commands::create_new_session,
            commands::load_host_session,
            commands::join_new_session,
            commands::load_guest_session,
            commands::start_emulator,
            commands::stop_emulator,
            commands::get_frame,
            commands::set_key_pressed,
            commands::set_key_released,
            commands::save_game,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
