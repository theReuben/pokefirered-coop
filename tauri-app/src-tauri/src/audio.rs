//! Host audio output for the emulated GBA.
//!
//! Two halves, deliberately kept apart:
//!
//! * [`AudioSink`] — a plain `Arc<Mutex<VecDeque<i16>>>` of interleaved stereo
//!   samples. It contains no cpal types, so it is `Send + Sync` on every
//!   platform and can live in the shared `AppState` / `EmulatorHandle`.
//! * [`AudioOut`] — owns the cpal `Stream`. `cpal::Stream` is not `Send` on all
//!   backends, so it is created, kept, and dropped entirely inside the emulator
//!   runner thread and never stored in shared state.
//!
//! The emulator thread pushes samples; the cpal callback pops them. The queue
//! depth is also the frame clock: the runner only advances the core while the
//! queue is below [`HIGH_WATER_FRAMES`], which paces emulation to the sound
//! card rather than to the display refresh rate.

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

/// Stop emulating once this many stereo frames are queued (~80 ms at 48 kHz).
pub const HIGH_WATER_FRAMES: usize = 3840;

/// Hard cap on the queue (~340 ms at 48 kHz). Only reached when nothing is
/// draining it — no output device, or a stream that died — in which case the
/// oldest samples are dropped so the buffer cannot grow without bound.
const MAX_QUEUED_FRAMES: usize = 16384;

/// Shared sample queue between the emulator thread and the audio callback.
#[derive(Clone)]
pub struct AudioSink {
    ring: Arc<Mutex<VecDeque<i16>>>,
}

impl AudioSink {
    pub fn new() -> Self {
        Self {
            ring: Arc::new(Mutex::new(VecDeque::with_capacity(MAX_QUEUED_FRAMES * 2))),
        }
    }

    /// Append interleaved stereo samples produced by the core.
    pub fn push(&self, samples: &[i16]) {
        let mut ring = self.ring.lock().unwrap();
        ring.extend(samples.iter().copied());
        let max = MAX_QUEUED_FRAMES * 2;
        if ring.len() > max {
            let excess = ring.len() - max;
            ring.drain(..excess);
        }
    }

    /// Stereo frames currently queued.
    pub fn queued_frames(&self) -> usize {
        self.ring.lock().unwrap().len() / 2
    }

    pub fn clear(&self) {
        self.ring.lock().unwrap().clear();
    }

    /// Pop one stereo frame, or `None` on underrun. An odd leftover sample can
    /// only happen if `push` dropped half a frame at the cap; it is discarded
    /// rather than allowed to swap the channels for the rest of the session.
    fn pop_frame(ring: &mut VecDeque<i16>) -> Option<(i16, i16)> {
        match (ring.pop_front(), ring.pop_front()) {
            (Some(l), Some(r)) => Some((l, r)),
            _ => None,
        }
    }

    /// Fill `out` (`channels` interleaved samples per frame) from the queue,
    /// padding with silence on underrun. `convert` maps an i16 sample to the
    /// device's sample type.
    fn fill<T: Copy>(&self, out: &mut [T], channels: usize, convert: impl Fn(i16) -> T) {
        // chunks_mut(0) panics, and this runs in the audio callback.
        if channels == 0 {
            return;
        }
        let mut ring = self.ring.lock().unwrap();
        for frame in out.chunks_mut(channels) {
            let (l, r) = Self::pop_frame(&mut ring).unwrap_or((0, 0));
            if channels == 1 {
                frame[0] = convert(((l as i32 + r as i32) / 2) as i16);
                continue;
            }
            // A trailing partial chunk shouldn't happen, but indexing blind here
            // would panic inside the callback if it ever did.
            for (i, slot) in frame.iter_mut().enumerate() {
                *slot = convert(match i {
                    0 => l,
                    1 => r,
                    _ => 0,
                });
            }
        }
    }
}

impl Default for AudioSink {
    fn default() -> Self {
        Self::new()
    }
}

// ── cpal output stream ────────────────────────────────────────────────────────

/// A live output stream. Dropping it stops playback.
pub struct AudioOut {
    _stream: cpal::Stream,
    sample_rate: u32,
}

impl AudioOut {
    /// Open the default output device and start playing from `sink`.
    /// Returns `None` if there is no usable device — the caller then falls back
    /// to wall-clock pacing and runs silently.
    pub fn open(sink: AudioSink) -> Option<Self> {
        use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};

        let device = cpal::default_host().default_output_device()?;
        let supported = device
            .default_output_config()
            .map_err(|e| log::warn!("no default audio config: {e}"))
            .ok()?;

        let sample_rate = supported.sample_rate().0;
        let channels = supported.channels() as usize;
        let format = supported.sample_format();
        let config: cpal::StreamConfig = supported.into();

        let err_fn = |e| log::error!("audio stream error: {e}");
        let stream = match format {
            cpal::SampleFormat::F32 => device.build_output_stream(
                &config,
                move |out: &mut [f32], _: &cpal::OutputCallbackInfo| {
                    sink.fill(out, channels, |s| s as f32 / 32768.0);
                },
                err_fn,
                None,
            ),
            cpal::SampleFormat::I16 => device.build_output_stream(
                &config,
                move |out: &mut [i16], _: &cpal::OutputCallbackInfo| {
                    sink.fill(out, channels, |s| s);
                },
                err_fn,
                None,
            ),
            cpal::SampleFormat::U16 => device.build_output_stream(
                &config,
                move |out: &mut [u16], _: &cpal::OutputCallbackInfo| {
                    sink.fill(out, channels, |s| (s as i32 + 32768) as u16);
                },
                err_fn,
                None,
            ),
            other => {
                log::warn!("unsupported audio sample format {other:?} — running silent");
                return None;
            }
        }
        .map_err(|e| log::error!("failed to open audio stream: {e}"))
        .ok()?;

        stream
            .play()
            .map_err(|e| log::error!("failed to start audio stream: {e}"))
            .ok()?;

        log::info!("audio: {sample_rate} Hz, {channels} ch, {format:?}");
        Some(Self {
            _stream: stream,
            sample_rate,
        })
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fill_pads_with_silence_on_underrun() {
        let sink = AudioSink::new();
        sink.push(&[100, -100]);
        let mut out = [1i16; 6]; // 3 stereo frames, only 1 available
        sink.fill(&mut out, 2, |s| s);
        assert_eq!(out, [100, -100, 0, 0, 0, 0]);
        assert_eq!(sink.queued_frames(), 0);
    }

    #[test]
    fn fill_downmixes_to_mono() {
        let sink = AudioSink::new();
        sink.push(&[100, 200]);
        let mut out = [0i16; 1];
        sink.fill(&mut out, 1, |s| s);
        assert_eq!(out, [150]);
    }

    #[test]
    fn push_drops_oldest_beyond_cap() {
        let sink = AudioSink::new();
        let block = vec![0i16; MAX_QUEUED_FRAMES * 2];
        sink.push(&block);
        sink.push(&[7, 7]);
        assert_eq!(sink.queued_frames(), MAX_QUEUED_FRAMES);

        // The newest samples must survive; the oldest are the ones dropped.
        let mut out = vec![0i16; MAX_QUEUED_FRAMES * 2];
        sink.fill(&mut out, 2, |s| s);
        assert_eq!(&out[out.len() - 2..], &[7, 7]);
    }
}
