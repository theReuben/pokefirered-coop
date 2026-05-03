use crate::session::SessionInfo;

// GBA display dimensions
pub const GBA_WIDTH: usize = 240;
pub const GBA_HEIGHT: usize = 160;
pub const FRAME_BYTES: usize = GBA_WIDTH * GBA_HEIGHT * 4; // RGBA

/// Handle to the mGBA emulator core.
pub struct EmulatorHandle {
    inner: EmuInner,
    key_state: u16, // bitmask; matches GBA KEYINPUT register (active-low)
    frame_buf: Vec<u8>,
    sav_path: String,
}

enum EmuInner {
    Running(Box<dyn EmuBackend>),
    Stopped,
}

impl EmulatorHandle {
    pub fn new() -> Self {
        Self {
            inner: EmuInner::Stopped,
            key_state: 0x03FF, // all buttons released (active-low)
            frame_buf: vec![0u8; FRAME_BYTES],
            sav_path: String::new(),
        }
    }

    /// Start the emulator with the given session and ROM path.
    ///
    /// `rom_path` must be an absolute path to the bundled ROM file.
    /// Obtain it via `app.path().resource_dir()?.join("rom/pokefirered.gba")`.
    pub fn start(&mut self, session: &SessionInfo, rom_path: &str) -> anyhow::Result<()> {
        if matches!(self.inner, EmuInner::Running(_)) {
            return Ok(()); // already running
        }

        self.sav_path = session.sav_path.clone();

        let backend: Box<dyn EmuBackend> = {
            #[cfg(feature = "mgba")]
            {
                Box::new(MgbaBackend::new(session, rom_path)?)
            }
            #[cfg(not(feature = "mgba"))]
            {
                let _ = rom_path; // suppress unused warning in stub builds
                log::warn!("mGBA not compiled in — using stub emulator");
                Box::new(StubBackend::new(session)?)
            }
        };

        self.inner = EmuInner::Running(backend);
        Ok(())
    }

    pub fn stop(&mut self) -> anyhow::Result<()> {
        // Drop the backend; for mGBA this triggers mCoreDestroy which flushes the save file.
        self.inner = EmuInner::Stopped;
        Ok(())
    }

    /// Flush the current battery save to disk explicitly.
    /// Call this before stop() for a clean exit.
    pub fn flush_save(&self) -> anyhow::Result<()> {
        if let EmuInner::Running(backend) = &self.inner {
            backend.flush_save(&self.sav_path)?;
        }
        Ok(())
    }

    /// Step the emulator one frame and copy the resulting pixel data into
    /// the internal frame buffer.
    pub fn step_frame(&mut self) {
        if let EmuInner::Running(backend) = &mut self.inner {
            backend.step(self.key_state, &mut self.frame_buf);
        }
    }

    /// Return the most recent frame as a flat RGBA byte vec (240×160×4).
    pub fn get_frame_rgba(&self) -> Vec<u8> {
        self.frame_buf.clone()
    }

    /// Press a GBA button (set its active-low bit to 0).
    pub fn key_pressed(&mut self, mask: u16) {
        self.key_state &= !mask;
    }

    /// Release a GBA button (set its active-low bit to 1).
    pub fn key_released(&mut self, mask: u16) {
        self.key_state |= mask;
    }

    /// Read a 32-bit value from GBA memory (used by the serial bridge).
    pub fn read_u32(&self, addr: u32) -> u32 {
        if let EmuInner::Running(backend) = &self.inner {
            backend.read_u32(addr)
        } else {
            0
        }
    }

    /// Write a 32-bit value to GBA memory (used by the serial bridge).
    pub fn write_u32(&mut self, addr: u32, value: u32) {
        if let EmuInner::Running(backend) = &mut self.inner {
            backend.write_u32(addr, value);
        }
    }

    /// Read a block of GBA memory (used by the serial bridge to drain send ring).
    pub fn read_bytes(&self, addr: u32, len: usize) -> Vec<u8> {
        if let EmuInner::Running(backend) = &self.inner {
            backend.read_bytes(addr, len)
        } else {
            vec![0u8; len]
        }
    }

    /// Write a block of GBA memory (used by the serial bridge to fill recv ring).
    pub fn write_bytes(&mut self, addr: u32, data: &[u8]) {
        if let EmuInner::Running(backend) = &mut self.inner {
            backend.write_bytes(addr, data);
        }
    }
}

// ── Backend trait ─────────────────────────────────────────────────────────────

trait EmuBackend: Send {
    fn step(&mut self, key_state: u16, frame_out: &mut Vec<u8>);
    fn read_u32(&self, addr: u32) -> u32;
    fn write_u32(&mut self, addr: u32, value: u32);
    fn read_bytes(&self, addr: u32, len: usize) -> Vec<u8>;
    fn write_bytes(&mut self, addr: u32, data: &[u8]);
    /// Flush the battery save to `sav_path`.
    fn flush_save(&self, sav_path: &str) -> anyhow::Result<()>;
}

// ── Stub backend (no-op, renders grey frames) ─────────────────────────────────

struct StubBackend;

impl StubBackend {
    fn new(_session: &SessionInfo) -> anyhow::Result<Self> {
        Ok(Self)
    }
}

impl EmuBackend for StubBackend {
    fn step(&mut self, _key_state: u16, frame_out: &mut Vec<u8>) {
        for (i, byte) in frame_out.iter_mut().enumerate() {
            *byte = match i % 4 {
                0 | 1 | 2 => 0x20,
                _ => 0xFF,
            };
        }
    }

    fn read_u32(&self, _addr: u32) -> u32 { 0 }
    fn write_u32(&mut self, _addr: u32, _value: u32) {}
    fn read_bytes(&self, _addr: u32, len: usize) -> Vec<u8> { vec![0u8; len] }
    fn write_bytes(&mut self, _addr: u32, _data: &[u8]) {}

    fn flush_save(&self, _sav_path: &str) -> anyhow::Result<()> {
        // Stub has no real emulation state to save.
        Ok(())
    }
}

// ── mGBA backend (real FFI via C wrapper — compiled only with --features mgba) ─
//
// Build: `cargo tauri dev --features mgba`
// The build.rs compiles mgba_wrapper.c against the mGBA submodule and links
// libmgba.a; bindgen generates the FFI bindings from mgba_wrapper.h.

#[cfg(feature = "mgba")]
mod mgba_ffi {
    #![allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code)]
    include!(concat!(env!("OUT_DIR"), "/mgba_bindings.rs"));
}

#[cfg(feature = "mgba")]
struct MgbaBackend {
    ctx: *mut mgba_ffi::MgbaCtx,
}

#[cfg(feature = "mgba")]
unsafe impl Send for MgbaBackend {}

#[cfg(feature = "mgba")]
impl MgbaBackend {
    fn new(session: &SessionInfo, rom_path: &str) -> anyhow::Result<Self> {
        use std::ffi::CString;
        unsafe {
            let ctx = mgba_ffi::mgba_create();
            if ctx.is_null() {
                anyhow::bail!("Failed to create mGBA core");
            }

            let rom = CString::new(rom_path)?;
            if !mgba_ffi::mgba_load_rom(ctx, rom.as_ptr()) {
                mgba_ffi::mgba_destroy(ctx);
                anyhow::bail!("Failed to load ROM from {}", rom_path);
            }

            // Load (or create) the battery save file.
            let sav = CString::new(session.sav_path.clone())?;
            mgba_ffi::mgba_load_save(ctx, sav.as_ptr());

            mgba_ffi::mgba_reset(ctx);

            Ok(Self { ctx })
        }
    }
}

#[cfg(feature = "mgba")]
impl EmuBackend for MgbaBackend {
    fn step(&mut self, key_state: u16, frame_out: &mut Vec<u8>) {
        unsafe {
            // mGBA setKeys expects active-high (1=pressed); key_state is active-low (0=pressed).
            mgba_ffi::mgba_set_keys(self.ctx, (!key_state & 0x03FF) as u32);
            mgba_ffi::mgba_run_frame(self.ctx);

            // mColor is XBGR8: R = pixel & 0xFF, G = pixel >> 8, B = pixel >> 16
            let pixels = mgba_ffi::mgba_get_pixels(self.ctx);
            let src = std::slice::from_raw_parts(pixels, GBA_WIDTH * GBA_HEIGHT);
            for (i, &pixel) in src.iter().enumerate() {
                let base = i * 4;
                frame_out[base]     = (pixel & 0xFF) as u8;         // R
                frame_out[base + 1] = ((pixel >> 8) & 0xFF) as u8;  // G
                frame_out[base + 2] = ((pixel >> 16) & 0xFF) as u8; // B
                frame_out[base + 3] = 0xFF;                          // A
            }
        }
    }

    fn read_u32(&self, addr: u32) -> u32 {
        unsafe { mgba_ffi::mgba_read32(self.ctx, addr) }
    }

    fn write_u32(&mut self, addr: u32, value: u32) {
        unsafe { mgba_ffi::mgba_write32(self.ctx, addr, value); }
    }

    fn read_bytes(&self, addr: u32, len: usize) -> Vec<u8> {
        let mut buf = vec![0u8; len];
        for (i, byte) in buf.iter_mut().enumerate() {
            *byte = unsafe { mgba_ffi::mgba_read8(self.ctx, addr + i as u32) };
        }
        buf
    }

    fn write_bytes(&mut self, addr: u32, data: &[u8]) {
        for (i, &byte) in data.iter().enumerate() {
            unsafe { mgba_ffi::mgba_write8(self.ctx, addr + i as u32, byte); }
        }
    }

    fn flush_save(&self, _sav_path: &str) -> anyhow::Result<()> {
        // mGBA writes dirty save data to the VFile automatically each frame.
        // mgba_destroy (called from Drop) closes the VFile cleanly.
        Ok(())
    }
}

#[cfg(feature = "mgba")]
impl Drop for MgbaBackend {
    fn drop(&mut self) {
        // deinit closes VFiles (flushes save) and frees the core.
        unsafe { mgba_ffi::mgba_destroy(self.ctx); }
    }
}
