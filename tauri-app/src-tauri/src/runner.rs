//! Emulator runner thread.
//!
//! The emulator used to advance only when the webview asked for a frame, which
//! tied game speed to the display refresh rate and to IPC latency. It now runs
//! on this thread, clocked by the audio device: a frame is emulated whenever
//! the sample queue is below its high-water mark, which is exactly the rate the
//! sound card consumes samples. `get_frame` just hands the webview the most
//! recent picture.
//!
//! With no usable audio device the loop falls back to a wall-clock pacer at the
//! GBA's real frame rate.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use tauri::Manager;

use crate::audio::{AudioOut, AudioSink, HIGH_WATER_FRAMES};
use crate::{serial_bridge, AppState};

/// GBA frame rate: 16.78 MHz / 280896 cycles ≈ 59.7275 Hz.
const FRAME_PERIOD: Duration = Duration::from_nanos(16_743_035);

/// How long the queue may sit full before we decide the stream is dead and
/// switch to wall-clock pacing. Without this, a device that stops draining
/// (headphones unplugged, backend error) would freeze the game outright.
const AUDIO_STALL_TIMEOUT: Duration = Duration::from_secs(1);

pub struct RunnerHandle {
    stop: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

impl RunnerHandle {
    pub fn new() -> Self {
        Self {
            stop: Arc::new(AtomicBool::new(false)),
            join: None,
        }
    }

    pub fn is_running(&self) -> bool {
        self.join.is_some()
    }

    /// Spawn the emulator thread. No-op if one is already running.
    pub fn start(&mut self, app: tauri::AppHandle, sink: AudioSink) {
        if self.is_running() {
            return;
        }
        self.stop = Arc::new(AtomicBool::new(false));
        let stop = self.stop.clone();
        self.join = Some(
            std::thread::Builder::new()
                .name("emulator".into())
                .spawn(move || run(app, sink, stop))
                .expect("failed to spawn emulator thread"),
        );
    }

    /// Signal the thread and wait for it to exit.
    ///
    /// The caller must NOT hold the emulator or net lock: the loop takes both
    /// every frame, so joining while holding either deadlocks.
    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

impl Default for RunnerHandle {
    fn default() -> Self {
        Self::new()
    }
}

fn run(app: tauri::AppHandle, sink: AudioSink, stop: Arc<AtomicBool>) {
    // The cpal stream is owned by this thread for its whole life — it is not
    // Send on every backend, so it must never reach the shared AppState.
    let audio = AudioOut::open(sink.clone());

    if let Some(out) = &audio {
        let state = app.state::<AppState>();
        let mut emu = state.emulator.lock().unwrap();
        emu.set_audio_rate(out.sample_rate());
    }

    let mut audio_alive = audio.is_some();
    let mut waiting_since: Option<Instant> = None;
    let mut next_frame = Instant::now();

    while !stop.load(Ordering::Relaxed) {
        if audio_alive {
            if sink.queued_frames() >= HIGH_WATER_FRAMES {
                let since = *waiting_since.get_or_insert_with(Instant::now);
                if since.elapsed() >= AUDIO_STALL_TIMEOUT {
                    log::warn!("audio stream stalled — falling back to wall-clock pacing");
                    audio_alive = false;
                    next_frame = Instant::now();
                } else {
                    std::thread::sleep(Duration::from_millis(1));
                }
                continue;
            }
            waiting_since = None;
        } else {
            let now = Instant::now();
            if now < next_frame {
                std::thread::sleep(Duration::from_millis(1));
                continue;
            }
            next_frame += FRAME_PERIOD;
            // Don't try to make up an unbounded backlog after a long stall.
            if next_frame < now {
                next_frame = now + FRAME_PERIOD;
            }
        }

        // Same lock order as the Tauri commands (emulator, then net).
        let state = app.state::<AppState>();
        let mut emu = state.emulator.lock().unwrap();
        let net = state.net.lock().unwrap();
        emu.step_frame();
        serial_bridge::tick(&mut emu, &net);
    }

    drop(audio);
    sink.clear();
}
