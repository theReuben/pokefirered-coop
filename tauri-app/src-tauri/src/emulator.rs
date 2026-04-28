use crate::session::SessionInfo;

// GBA display dimensions
pub const GBA_WIDTH: usize = 240;
pub const GBA_HEIGHT: usize = 160;
pub const FRAME_BYTES: usize = GBA_WIDTH * GBA_HEIGHT * 4; // RGBA

/// Handle to the mGBA emulator core.
///
/// The actual libmgba FFI lives in emulator_ffi (below). This wrapper
/// manages lifecycle and presents a safe Rust interface to the rest of
/// the app. When libmgba is not available at build time, a stub that
/// renders a black frame is used instead (feature "stub-emu").
pub struct EmulatorHandle {
    inner: EmuInner,
    key_state: u16, // bitmask; matches GBA KEYINPUT register (active-low)
    frame_buf: Vec<u8>,
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
        }
    }

    pub fn start(&mut self, session: &SessionInfo) -> anyhow::Result<()> {
        if matches!(self.inner, EmuInner::Running(_)) {
            return Ok(()); // already running
        }

        let backend: Box<dyn EmuBackend> = {
            #[cfg(feature = "mgba")]
            {
                Box::new(MgbaBackend::new(session)?)
            }
            #[cfg(not(feature = "mgba"))]
            {
                log::warn!("mGBA not compiled in — using stub emulator");
                Box::new(StubBackend::new(session)?)
            }
        };

        self.inner = EmuInner::Running(backend);
        Ok(())
    }

    pub fn stop(&mut self) -> anyhow::Result<()> {
        self.inner = EmuInner::Stopped;
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
        self.key_state &= !mask; // clear bit = pressed
    }

    /// Release a GBA button (set its active-low bit to 1).
    pub fn key_released(&mut self, mask: u16) {
        self.key_state |= mask; // set bit = released
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
}

// ── Stub backend (no-op, renders black frames) ────────────────────────────────

struct StubBackend;

impl StubBackend {
    fn new(_session: &SessionInfo) -> anyhow::Result<Self> {
        Ok(Self)
    }
}

impl EmuBackend for StubBackend {
    fn step(&mut self, _key_state: u16, frame_out: &mut Vec<u8>) {
        // Fill with a dark grey so the canvas isn't just black
        for (i, byte) in frame_out.iter_mut().enumerate() {
            *byte = match i % 4 {
                0 | 1 | 2 => 0x20,
                _ => 0xFF, // alpha
            };
        }
    }

    fn read_u32(&self, _addr: u32) -> u32 { 0 }
    fn write_u32(&mut self, _addr: u32, _value: u32) {}
    fn read_bytes(&self, _addr: u32, len: usize) -> Vec<u8> { vec![0u8; len] }
    fn write_bytes(&mut self, _addr: u32, _data: &[u8]) {}
}

// ── mGBA backend (real FFI — compiled only with --features mgba) ──────────────
//
// To enable: add `mgba` to the feature list in Cargo.toml and provide the
// libmgba.a static library. The FFI surface mirrors mGBA's public C API.
//
// Build steps:
//   1. git submodule add https://github.com/mgba-emu/mgba.git mgba
//   2. cd mgba && cmake -DBUILD_SHARED=OFF -DBUILD_STATIC=ON .
//   3. Build the `mgba-core` target; copy libmgba.a to src-tauri/lib/
//   4. Add bindgen + cc to build-dependencies; run bindgen on mgba/include/mgba/core/core.h
//   5. Enable the `mgba` feature in Cargo.toml

#[cfg(feature = "mgba")]
mod mgba_ffi {
    // Generated by bindgen from mgba/include/mgba/core/core.h
    // Run: bindgen mgba/include/mgba/core/core.h -o src/mgba_bindings.rs
    include!(concat!(env!("OUT_DIR"), "/mgba_bindings.rs"));
}

#[cfg(feature = "mgba")]
struct MgbaBackend {
    // Opaque pointer to the mGBACore C struct
    core: *mut mgba_ffi::mCore,
    video_buf: Vec<u32>,
}

#[cfg(feature = "mgba")]
unsafe impl Send for MgbaBackend {}

#[cfg(feature = "mgba")]
impl MgbaBackend {
    fn new(session: &SessionInfo) -> anyhow::Result<Self> {
        use std::ffi::CString;

        unsafe {
            let core = mgba_ffi::GBAVideoSoftwareRendererCreate();
            if core.is_null() {
                anyhow::bail!("Failed to create mGBA core");
            }

            // Load ROM from bundled resource path
            let rom_path = CString::new(rom_resource_path())?;
            if mgba_ffi::mCoreLoadFile(core, rom_path.as_ptr()) == 0 {
                anyhow::bail!("Failed to load ROM");
            }

            // Load save file if it exists
            if std::path::Path::new(&session.sav_path).exists() {
                let sav = CString::new(session.sav_path.clone())?;
                mgba_ffi::mCoreLoadSave(core, sav.as_ptr());
            }

            mgba_ffi::mCoreInitConfig(core, std::ptr::null());
            mgba_ffi::mCoreReset(core);

            Ok(Self {
                core,
                video_buf: vec![0u32; crate::emulator::GBA_WIDTH * crate::emulator::GBA_HEIGHT],
            })
        }
    }
}

#[cfg(feature = "mgba")]
impl EmuBackend for MgbaBackend {
    fn step(&mut self, key_state: u16, frame_out: &mut Vec<u8>) {
        unsafe {
            (*(*self.core).inputInfo.ops.setKeys)(self.core, key_state as u32);
            mgba_ffi::mCoreRunFrame(self.core);

            // Copy the soft renderer framebuffer into our RGBA vec
            let pixels = (*self.core).videoRenderer.outputBuffer as *const u32;
            let count = GBA_WIDTH * GBA_HEIGHT;
            let src = std::slice::from_raw_parts(pixels, count);

            // mGBA outputs BGR5 or XRGB8888 depending on renderer — normalize to RGBA
            for (i, &pixel) in src.iter().enumerate() {
                let r = ((pixel >> 0) & 0xFF) as u8;
                let g = ((pixel >> 8) & 0xFF) as u8;
                let b = ((pixel >> 16) & 0xFF) as u8;
                let base = i * 4;
                frame_out[base]     = r;
                frame_out[base + 1] = g;
                frame_out[base + 2] = b;
                frame_out[base + 3] = 0xFF;
            }
        }
    }

    fn read_u32(&self, addr: u32) -> u32 {
        unsafe { mgba_ffi::mCoreRawRead32(self.core, addr, -1) as u32 }
    }

    fn write_u32(&mut self, addr: u32, value: u32) {
        unsafe { mgba_ffi::mCoreRawWrite32(self.core, addr, -1, value as i32); }
    }

    fn read_bytes(&self, addr: u32, len: usize) -> Vec<u8> {
        let mut buf = vec![0u8; len];
        for (i, byte) in buf.iter_mut().enumerate() {
            *byte = unsafe {
                mgba_ffi::mCoreRawRead8(self.core, addr + i as u32, -1) as u8
            };
        }
        buf
    }

    fn write_bytes(&mut self, addr: u32, data: &[u8]) {
        for (i, &byte) in data.iter().enumerate() {
            unsafe {
                mgba_ffi::mCoreRawWrite8(self.core, addr + i as u32, -1, byte as i8);
            }
        }
    }
}

#[cfg(feature = "mgba")]
impl Drop for MgbaBackend {
    fn drop(&mut self) {
        unsafe {
            mgba_ffi::mCoreDestroy(self.core);
        }
    }
}

#[cfg(feature = "mgba")]
fn rom_resource_path() -> String {
    // In a bundled Tauri app, resources are in a known location relative to the executable.
    // tauri::api::path::resource_dir() or resolve_resource() is the canonical way.
    // This placeholder is replaced by the real Tauri resource resolver in commands.rs.
    "resources/rom/pokefirered.gba".to_string()
}
