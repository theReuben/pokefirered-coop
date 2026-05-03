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
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { .. } = event {
                // Flush the battery save before the process exits so the
                // user doesn't lose progress if they close via the OS button
                // instead of the in-app Quit button.
                if let Some(state) = window.try_state::<AppState>() {
                    let emu = state.emulator.lock().unwrap();
                    let _ = emu.flush_save();
                }
            }
        })
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
            commands::get_mp_debug,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
